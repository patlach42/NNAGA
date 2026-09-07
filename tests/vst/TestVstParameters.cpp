#include <gtest/gtest.h>

#include "ipc/SharedRing.h"

#include <cstdint>
#include <cstddef>
#include <string>
#include <unistd.h>

namespace {

static_assert(VSTPOC_SHARED_LAYOUT_VERSION == 10u);
static_assert(VSTPOC_SHARED_LAYOUT_MAGIC == UINT64_C(0x565354504f433130));
static_assert(sizeof(VstpocMidiEvent) == 16u);
static_assert(offsetof(VstpocTransportBlock, midi_payload_begin) <
              offsetof(VstpocTransportBlock, midi_events));
static_assert(offsetof(VstpocOutputBlock, midi_payload_begin) <
              offsetof(VstpocOutputBlock, midi_events));

class TempFile {
public:
    TempFile() {
        char pattern[] = "/tmp/vst_parameters_v10_XXXXXX";
        const int fd = ::mkstemp(pattern);
        EXPECT_NE(fd, -1);
        if (fd >= 0) { ::close(fd); path_ = pattern; }
    }
    ~TempFile() { if (!path_.empty()) ::unlink(path_.c_str()); }
    std::string path_;
};

TEST(VstV10AbiContractTest, SharedRingPublishesExactMagicVersionAndFeatureBit) {
    TempFile file;
    SharedRing ring(file.path_);
    ASSERT_TRUE(ring.valid());
    const auto* shared = ring.raw();
    EXPECT_EQ(shared->shared_layout_magic, VSTPOC_SHARED_LAYOUT_MAGIC);
    EXPECT_EQ(shared->shared_layout_version, VSTPOC_SHARED_LAYOUT_VERSION);
    EXPECT_EQ(shared->shared_layout_size, VSTPOC_SHARED_LAYOUT_V10_SIZE);
    EXPECT_NE(shared->shared_feature_bits & VSTPOC_FEATURE_MIDI_PAYLOAD_RING, 0u);
}

TEST(VstV10AbiContractTest, InputAndOutputPayloadCountersAreIndependent) {
    TempFile file;
    SharedRing ring(file.path_);
    ASSERT_TRUE(ring.valid());
    auto* shared = ring.raw();
    EXPECT_EQ(shared->midi_input_payload_head, 0u);
    EXPECT_EQ(shared->midi_input_payload_tail, 0u);
    EXPECT_EQ(shared->midi_output_payload_head, 0u);
    EXPECT_EQ(shared->midi_output_payload_tail, 0u);
    shared->midi_input_drop_count = 3;
    shared->midi_output_drop_count = 5;
    EXPECT_EQ(shared->midi_input_drop_count, 3u);
    EXPECT_EQ(shared->midi_output_drop_count, 5u);
}

}  // namespace
