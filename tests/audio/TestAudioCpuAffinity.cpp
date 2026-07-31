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
