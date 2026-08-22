#include <gtest/gtest.h>

#include "ipc/SharedRing.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

class TempBackingFile {
public:
    TempBackingFile() {
        char pattern[] = "/tmp/vst_parameter_tests_XXXXXX";
        const int fd = ::mkstemp(pattern);
        EXPECT_NE(fd, -1);
        if (fd < 0) return;
        ::close(fd);
        path_ = pattern;
    }

    ~TempBackingFile() {
        if (!path_.empty()) ::unlink(path_.c_str());
    }

    const std::string& path() const { return path_; }

private:
    std::string path_;
};

class SharedRingFixture : public ::testing::Test {
protected:
    TempBackingFile backing;
    SharedRing ring{backing.path()};
};

uint64_t LoadAcquire(const uint64_t* value) {
    return __atomic_load_n(value, __ATOMIC_ACQUIRE);
}

void StoreRelease(uint64_t* value, uint64_t next) {
    __atomic_store_n(value, next, __ATOMIC_RELEASE);
}

struct ParameterSnapshot {
    std::vector<float> values;
    std::vector<std::string> displays;
};

// This is the reader-side rule encoded by the v7 ABI: an even, non-zero
// sequence brackets one coherent values/display snapshot.
bool ReadStableSnapshot(const VstpocShared& shared, ParameterSnapshot* out) {
    if (!out) return false;
    const uint64_t before = LoadAcquire(&shared.param_values_seq);
    if (before == 0 || (before & 1u) != 0) return false;

    const int32_t count = shared.param_count;
    if (count <= 0 || count > VSTPOC_MAX_PARAMS) return false;

    ParameterSnapshot snapshot;
    snapshot.values.reserve(static_cast<size_t>(count));
    snapshot.displays.reserve(static_cast<size_t>(count));
    for (int32_t i = 0; i < count; ++i) {
        snapshot.values.push_back(shared.param_values[i]);
        const char* text = shared.param_display_values[i];
        snapshot.displays.emplace_back(text, strnlen(text, VSTPOC_PARAM_DISPLAY_LEN));
    }

    const uint64_t after = LoadAcquire(&shared.param_values_seq);
    if (before != after || (after & 1u) != 0) return false;
    *out = std::move(snapshot);
    return true;
}

TEST(VstParameterContractTest, SupportsMetadataBeyondLegacy128ParameterLimit) {
    TempBackingFile backing;
    SharedRing ring(backing.path());
    ASSERT_TRUE(ring.valid());

    ASSERT_GT(VSTPOC_MAX_PARAMS, 128u);
    constexpr int32_t kParameterCount = static_cast<int32_t>(VSTPOC_MAX_PARAMS);
    constexpr int32_t kHighIndex = kParameterCount - 1;
    VstpocShared* shared = ring.raw();
    shared->param_count = kParameterCount;
    std::strncpy(shared->param_names[kHighIndex], "High-index instrument control",
                 VSTPOC_PARAM_NAME_LEN - 1);
    shared->param_names[kHighIndex][VSTPOC_PARAM_NAME_LEN - 1] = '\0';
    shared->param_metadata[kHighIndex] = {
        0.375f, 127, VSTPOC_PARAM_FLAG_HIDDEN | VSTPOC_PARAM_FLAG_READ_ONLY, {}};
    std::strncpy(shared->param_metadata[kHighIndex].unit, "semitones",
                 VSTPOC_PARAM_UNIT_LEN - 1);
    shared->param_metadata[kHighIndex].unit[VSTPOC_PARAM_UNIT_LEN - 1] = '\0';
    StoreRelease(&shared->param_metadata_seq, 2);

    EXPECT_EQ(shared->param_count, kParameterCount);
    EXPECT_STREQ(shared->param_names[kHighIndex], "High-index instrument control");
    EXPECT_FLOAT_EQ(shared->param_metadata[kHighIndex].default_normalized, 0.375f);
    EXPECT_EQ(shared->param_metadata[kHighIndex].step_count, 127);
    EXPECT_EQ(shared->param_metadata[kHighIndex].flags,
              VSTPOC_PARAM_FLAG_HIDDEN | VSTPOC_PARAM_FLAG_READ_ONLY);
    EXPECT_STREQ(shared->param_metadata[kHighIndex].unit, "semitones");
    EXPECT_EQ(LoadAcquire(&shared->param_metadata_seq), 2u);
}

TEST(VstParameterContractTest, PublishesDescriptorFlagsDefaultStepsAndUnitAsOneMetadataSnapshot) {
    TempBackingFile backing;
    SharedRing ring(backing.path());
    ASSERT_TRUE(ring.valid());

    VstpocShared* shared = ring.raw();
    shared->param_count = 3;
    shared->param_metadata[0] = {0.5f, 0, 0, {}};
    shared->param_metadata[1] = {1.0f, 1, VSTPOC_PARAM_FLAG_READ_ONLY, {}};
    shared->param_metadata[2] = {0.25f, 8, VSTPOC_PARAM_FLAG_HIDDEN, {}};
    std::strncpy(shared->param_metadata[0].unit, "dB", VSTPOC_PARAM_UNIT_LEN - 1);
    std::strncpy(shared->param_metadata[1].unit, "Hz", VSTPOC_PARAM_UNIT_LEN - 1);
    std::strncpy(shared->param_metadata[2].unit, "ms", VSTPOC_PARAM_UNIT_LEN - 1);
    StoreRelease(&shared->param_metadata_seq, 4);

    ASSERT_EQ(LoadAcquire(&shared->param_metadata_seq), 4u);
    EXPECT_FLOAT_EQ(shared->param_metadata[0].default_normalized, 0.5f);
    EXPECT_EQ(shared->param_metadata[0].step_count, 0);
    EXPECT_EQ(shared->param_metadata[0].flags, 0u);
    EXPECT_STREQ(shared->param_metadata[0].unit, "dB");
    EXPECT_FLOAT_EQ(shared->param_metadata[1].default_normalized, 1.0f);
    EXPECT_EQ(shared->param_metadata[1].step_count, 1);
    EXPECT_EQ(shared->param_metadata[1].flags, VSTPOC_PARAM_FLAG_READ_ONLY);
    EXPECT_STREQ(shared->param_metadata[1].unit, "Hz");
    EXPECT_FLOAT_EQ(shared->param_metadata[2].default_normalized, 0.25f);
    EXPECT_EQ(shared->param_metadata[2].step_count, 8);
    EXPECT_EQ(shared->param_metadata[2].flags, VSTPOC_PARAM_FLAG_HIDDEN);
    EXPECT_STREQ(shared->param_metadata[2].unit, "ms");
}

TEST_F(SharedRingFixture, ParameterWritesCoalescePerIndexAndLatestValueWins) {
    ASSERT_TRUE(ring.valid());
    VstpocShared* shared = ring.raw();
    constexpr int32_t kIndex = 700;

    ASSERT_EQ(LoadAcquire(&shared->param_desired_seq[kIndex]), 0u);
    ASSERT_EQ(LoadAcquire(&shared->param_head), 0u);
    ring.pushParam(kIndex, 0.10f);
    ring.pushParam(kIndex, 0.20f);
    ring.pushParam(kIndex, 0.90f);

    EXPECT_FLOAT_EQ(shared->param_desired_values[kIndex], 0.90f);
    EXPECT_EQ(LoadAcquire(&shared->param_desired_seq[kIndex]), 3u);
    EXPECT_EQ(LoadAcquire(&shared->param_head), 0u);
}

TEST_F(SharedRingFixture, ParameterMailboxesRemainIndependentAcrossIndices) {
    ASSERT_TRUE(ring.valid());
    VstpocShared* shared = ring.raw();
    constexpr int32_t kFirst = 511;
    constexpr int32_t kSecond = 900;

    ring.pushParam(kFirst, 0.125f);
    ring.pushParam(kSecond, 0.875f);
    ring.pushParam(kFirst, 0.625f);

    EXPECT_FLOAT_EQ(shared->param_desired_values[kFirst], 0.625f);
    EXPECT_EQ(LoadAcquire(&shared->param_desired_seq[kFirst]), 2u);
    EXPECT_FLOAT_EQ(shared->param_desired_values[kSecond], 0.875f);
    EXPECT_EQ(LoadAcquire(&shared->param_desired_seq[kSecond]), 1u);
    EXPECT_EQ(LoadAcquire(&shared->param_head), 0u);
}

TEST(VstParameterContractTest, ValuesAndDisplaysAreAcceptedOnlyFromStableEvenSnapshot) {
    TempBackingFile backing;
    SharedRing ring(backing.path());
    ASSERT_TRUE(ring.valid());
    VstpocShared* shared = ring.raw();
    shared->param_count = 2;
    shared->param_values[0] = 0.25f;
    shared->param_values[1] = 0.75f;
    std::strncpy(shared->param_display_values[0], "-12.0 dB", VSTPOC_PARAM_DISPLAY_LEN - 1);
    std::strncpy(shared->param_display_values[1], "440 Hz", VSTPOC_PARAM_DISPLAY_LEN - 1);

    ParameterSnapshot snapshot;
    StoreRelease(&shared->param_values_seq, 1);
    EXPECT_FALSE(ReadStableSnapshot(*shared, &snapshot));

    StoreRelease(&shared->param_values_seq, 2);
    ASSERT_TRUE(ReadStableSnapshot(*shared, &snapshot));
    ASSERT_EQ(snapshot.values.size(), 2u);
    ASSERT_EQ(snapshot.displays.size(), 2u);
    EXPECT_FLOAT_EQ(snapshot.values[0], 0.25f);
    EXPECT_FLOAT_EQ(snapshot.values[1], 0.75f);
    EXPECT_EQ(snapshot.displays[0], "-12.0 dB");
    EXPECT_EQ(snapshot.displays[1], "440 Hz");
}

}  // namespace
