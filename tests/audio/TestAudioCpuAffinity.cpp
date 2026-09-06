#include <gtest/gtest.h>

#include <liblowlatencyaudio/ThreadUtils.h>

#if defined(__linux__)
#include <sched.h>
#endif

#include <initializer_list>
#include <string>
#include <vector>

#if defined(__linux__)
namespace {

using guitarrackcraft::deriveAudioCpuMask;
using guitarrackcraft::deriveUiCpuMask;

cpu_set_t cpuMask(std::initializer_list<int> cpus) {
    cpu_set_t mask;
    CPU_ZERO(&mask);
    for (const int cpu : cpus) {
        CPU_SET(cpu, &mask);
    }
    return mask;
}

bool isSubset(const cpu_set_t& subset, const cpu_set_t& superset) {
    for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
        if (CPU_ISSET(cpu, &subset) && !CPU_ISSET(cpu, &superset)) {
            return false;
        }
    }
    return true;
}

struct UiMaskCase {
    const char* name;
    cpu_set_t allowed;
    cpu_set_t expected;
};

std::vector<UiMaskCase> exactUiMaskCases() {
    return {
        {"empty", cpuMask({}), cpuMask({})},
        {"singleton", cpuMask({13}), cpuMask({13})},
        // For two CPUs the lowest-ranked allowed CPU remains available to UI.
        {"two_cpus", cpuMask({2, 17}), cpuMask({2})},
        {"three_cpus", cpuMask({0, 4, 9}), cpuMask({0})},
        {"sparse_four_cpus", cpuMask({1, 7, 20, 63}), cpuMask({1, 7})},
        {"sparse_six_cpus", cpuMask({0, 3, 11, 24, 40, 61}),
         cpuMask({0, 3, 11, 24})},
        {"sparse_eight_cpus", cpuMask({2, 5, 14, 21, 33, 47, 60, 95}),
         cpuMask({2, 5, 14, 21, 33, 47})},
    };
}

class DeriveUiCpuMaskTest : public ::testing::TestWithParam<UiMaskCase> {};

TEST_P(DeriveUiCpuMaskTest, MatchesExactMaskContract) {
    const UiMaskCase& testCase = GetParam();
    const cpu_set_t actual = deriveUiCpuMask(testCase.allowed);

    EXPECT_TRUE(CPU_EQUAL(&actual, &testCase.expected)) << testCase.name;
}

INSTANTIATE_TEST_SUITE_P(
    CpuSets,
    DeriveUiCpuMaskTest,
    ::testing::ValuesIn(exactUiMaskCases()),
    [](const ::testing::TestParamInfo<UiMaskCase>& info) {
        return std::string(info.param.name);
    });

TEST(DeriveUiCpuMask, TwoCpuSetNeverLeavesUiEmpty) {
    const cpu_set_t allowed = cpuMask({6, 42});
    const cpu_set_t ui = deriveUiCpuMask(allowed);

    EXPECT_GT(CPU_COUNT(&ui), 0);
    EXPECT_TRUE(isSubset(ui, allowed));
}

TEST(DeriveUiCpuMask, ThreeOrMoreCpuSetsExcludeExactlyAudioCpus) {
    const std::vector<cpu_set_t> allowedSets = {
        cpuMask({0, 4, 9}),
        cpuMask({1, 7, 20, 63}),
        cpuMask({0, 3, 11, 24, 40, 61}),
        cpuMask({2, 5, 14, 21, 33, 47, 60, 95}),
    };

    for (const cpu_set_t& allowed : allowedSets) {
        const cpu_set_t audio = deriveAudioCpuMask(allowed);
        cpu_set_t expected = allowed;
        CPU_XOR(&expected, &allowed, &audio);

        const cpu_set_t actual = deriveUiCpuMask(allowed);
        EXPECT_TRUE(CPU_EQUAL(&actual, &expected));
    }
}

TEST(DeriveUiCpuMask, AlwaysReturnsSubsetOfAllowedSet) {
    for (const UiMaskCase& testCase : exactUiMaskCases()) {
        const cpu_set_t actual = deriveUiCpuMask(testCase.allowed);
        EXPECT_TRUE(isSubset(actual, testCase.allowed)) << testCase.name;
    }
}

} // namespace
#endif

#if defined(__linux__)
namespace {

// The mask helpers were always covered; the code that applies them was not,
// and it spent its life returning early. The raw sched_getaffinity system call
// answers with the number of bytes it wrote, so a check for zero treats every
// success as a failure - which is invisible to a test that only ever asks what
// mask the pure function would have computed.
// Two threads handed the same pair of cores are free to be stacked on one of
// them. The roles exist so each names a core of its own.
TEST(AudioCpuRoleTest, BothRolesTakeTheWholePool) {
    cpu_set_t allowed;
    CPU_ZERO(&allowed);
    for (const int cpu : {0, 1, 2, 3, 4, 5, 6, 7}) CPU_SET(cpu, &allowed);
    const cpu_set_t render = guitarrackcraft::deriveAudioRoleCpuMask(
        allowed, guitarrackcraft::AudioCpuRole::Render);
    const cpu_set_t service = guitarrackcraft::deriveAudioRoleCpuMask(
        allowed, guitarrackcraft::AudioCpuRole::Service);
    EXPECT_EQ(2, CPU_COUNT(&render)) << "the graph keeps the fast pair";
    EXPECT_TRUE(CPU_ISSET(6, &render));
    EXPECT_TRUE(CPU_ISSET(7, &render));
    // The whole pool, not one core of it: core control parks a prime core when
    // the cluster is quiet, and a thread pinned to the parked one waits.
    EXPECT_EQ(2, CPU_COUNT(&service));
    EXPECT_TRUE(CPU_ISSET(6, &service));
    EXPECT_TRUE(CPU_ISSET(7, &service));
}

// A cpuset too small to spare a core has to keep working rather than pin
// servicing onto no CPU at all.
TEST(AudioCpuRoleTest, CollapsesWhenThereIsNoPool) {
    cpu_set_t allowed;
    CPU_ZERO(&allowed);
    CPU_SET(3, &allowed);
    const cpu_set_t service = guitarrackcraft::deriveAudioRoleCpuMask(
        allowed, guitarrackcraft::AudioCpuRole::Service);
    EXPECT_TRUE(CPU_ISSET(3, &service));
}

TEST(ApplyAudioAffinityTest, NarrowsTheRunningThreadToTheAudioCpus) {
    cpu_set_t original;
    CPU_ZERO(&original);
    ASSERT_EQ(0, sched_getaffinity(0, sizeof(original), &original));
    if (CPU_COUNT(&original) < 3) {
        GTEST_SKIP() << "needs at least three runnable CPUs to narrow onto";
    }
    const cpu_set_t expected = guitarrackcraft::deriveAudioRoleCpuMask(
        original, guitarrackcraft::AudioCpuRole::Service);
    ASSERT_LT(CPU_COUNT(&expected), CPU_COUNT(&original))
        << "the audio mask has to be a proper subset for this to prove anything";

    guitarrackcraft::applyCurrentThreadAudioAffinity(
        guitarrackcraft::AudioCpuRole::Service);
    cpu_set_t applied;
    CPU_ZERO(&applied);
    ASSERT_EQ(0, sched_getaffinity(0, sizeof(applied), &applied));
    const bool restored =
        sched_setaffinity(0, sizeof(original), &original) == 0;

    EXPECT_TRUE(CPU_EQUAL(&expected, &applied))
        << "affinity was requested and not applied: " << CPU_COUNT(&applied)
        << " CPUs allowed, expected " << CPU_COUNT(&expected);
    EXPECT_TRUE(restored) << "failed to restore the original affinity";
}

}  // namespace
#endif
