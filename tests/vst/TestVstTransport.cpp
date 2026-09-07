#include <gtest/gtest.h>

#include "ipc/SharedRing.h"

#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

static_assert(VSTPOC_SHARED_LAYOUT_VERSION == 9u);
static_assert(VSTPOC_SHARED_LAYOUT_MAGIC == UINT64_C(0x565354504f435339));
static_assert(sizeof(VstpocOutputBlock) == 16u);
static_assert(sizeof(VstpocOutputMidiBlock) == 1088u);
static_assert(sizeof(VstpocTransportBlock) == 1080u);
static_assert(VSTPOC_TRANSPORT_QUEUE_CAPACITY == 1024u);
static_assert(VSTPOC_OUTPUT_BLOCK_CAPACITY == 128u);
static_assert(offsetof(VstpocShared, shared_layout_magic) == 335000u);
static_assert(offsetof(VstpocShared, shared_layout_version) == 335008u);
static_assert(offsetof(VstpocShared, shared_layout_size) == 335012u);
static_assert(offsetof(VstpocShared, shared_feature_bits) == 335016u);
static_assert(offsetof(VstpocShared, transport_seq) == 335040u);
static_assert(offsetof(VstpocShared, transport_queue_head) == 335424u);
static_assert(offsetof(VstpocShared, transport_queue_tail) == 335488u);
static_assert(offsetof(VstpocShared, transport_queue) == 335552u);
static_assert(offsetof(VstpocShared, transport_queue_dropped) == 1442560u);
static_assert(offsetof(VstpocShared, latency_seq) == 1557376u);
static_assert(offsetof(VstpocShared, guest_state) == 1557440u);
static_assert(offsetof(VstpocShared, block_deadline_ns) == 1557696u);
static_assert(offsetof(VstpocShared, deadline_miss_count) == 1557760u);
static_assert(offsetof(VstpocShared, starvation_count) == 1557824u);
static_assert(offsetof(VstpocShared, output_drop_count) == 1557888u);
static_assert(offsetof(VstpocShared, wake_requested) == 1558080u);
static_assert(offsetof(VstpocShared, output_block_head) == 1558144u);
static_assert(offsetof(VstpocShared, output_block_tail) == 1558208u);
static_assert(offsetof(VstpocShared, output_blocks) == 1558272u);
static_assert(offsetof(VstpocShared, output_block_sequence) == 1560320u);
static_assert(offsetof(VstpocShared, output_block_frames) == 1560384u);
static_assert(offsetof(VstpocShared, output_midi_blocks) == 1560704u);
static_assert(offsetof(VstpocShared, output_midi_blocks) ==
              VSTPOC_SHARED_LAYOUT_V8_SIZE);
static_assert(VSTPOC_SHARED_LAYOUT_V8_SIZE == 1560704u);
static_assert(VSTPOC_SHARED_LAYOUT_V9_SIZE == 1700032u);
static_assert(sizeof(VstpocShared) == 1700032u);

class TempBackingFile {
public:
    TempBackingFile() {
        char pattern[] = "/tmp/vst_transport_tests_XXXXXX";
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
    void SetUp() override { ring.setExpectedWakePeer(getpid()); }

    TempBackingFile backing;
    SharedRing ring{backing.path()};
};

constexpr int kWakePollTimeoutMs = 500;
constexpr int kNoWakePollTimeoutMs = 100;

int ConnectWake(const std::string& path) {
    if (path.empty()) return -1;

    sockaddr_un address{};
    if (path.size() + 1u > sizeof(address.sun_path)) return -1;

    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1u);
    const socklen_t addressLength = static_cast<socklen_t>(
        offsetof(sockaddr_un, sun_path) + path.size() + 1u);
    if (::connect(fd, reinterpret_cast<const sockaddr*>(&address),
                  addressLength) < 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

bool ReceiveWake(int fd, int timeoutMs = kWakePollTimeoutMs) {
    pollfd descriptor{fd, POLLIN | POLLHUP | POLLERR, 0};
    int result;
    do {
        result = ::poll(&descriptor, 1, timeoutMs);
    } while (result < 0 && errno == EINTR);
    if (result <= 0 || (descriptor.revents & (POLLIN | POLLHUP | POLLERR)) == 0) {
        return false;
    }

    char message[64];
    ssize_t received;
    do {
        received = ::recv(fd, message, sizeof(message),
                          MSG_DONTWAIT | MSG_NOSIGNAL);
    } while (received < 0 && errno == EINTR);
    return received > 0;
}

ssize_t ReceiveWakeBytes(int fd, int timeoutMs = kWakePollTimeoutMs) {
    pollfd descriptor{fd, POLLIN | POLLHUP | POLLERR, 0};
    int result;
    do {
        result = ::poll(&descriptor, 1, timeoutMs);
    } while (result < 0 && errno == EINTR);
    if (result <= 0 ||
        (descriptor.revents & (POLLIN | POLLHUP | POLLERR)) == 0) {
        return 0;
    }

    char message[64];
    ssize_t received;
    do {
        received = ::recv(fd, message, sizeof(message),
                          MSG_DONTWAIT | MSG_NOSIGNAL);
    } while (received < 0 && errno == EINTR);
    return received > 0 ? received : 0;
}

// A connect succeeds once the listener has queued the connection, but the
// host accept loop may not have installed it in its broadcast set yet.
bool PrimeWakeClient(SharedRing& ring, int fd) {
    for (int attempt = 0; attempt < 8; ++attempt) {
        ring.notifyGuest();
        if (ReceiveWake(fd)) return true;
        __atomic_store_n(&ring.raw()->wake_requested, 0u, __ATOMIC_RELEASE);
    }
    return false;
}

void StoreRelaxed(uint64_t* value, uint64_t next) {
    __atomic_store_n(value, next, __ATOMIC_RELAXED);
}

void StoreRelease(uint64_t* value, uint64_t next) {
    __atomic_store_n(value, next, __ATOMIC_RELEASE);
}

uint64_t LoadAcquire(const uint64_t* value) {
    return __atomic_load_n(value, __ATOMIC_ACQUIRE);
}

constexpr uint64_t AudioSlot(uint64_t frame) {
    return frame & (VSTPOC_AUDIO_RING_FRAMES - 1u);
}

uint64_t ConsumeWakeRequest(VstpocShared* shared) {
    return __atomic_exchange_n(&shared->wake_requested, 0u, __ATOMIC_ACQ_REL);
}

void StageOutputBlock(VstpocShared* shared, uint64_t blockIndex,
                      uint32_t frameCount, uint32_t ringOffset,
                      float leftBase, float rightBase) {
    for (uint32_t i = 0; i < frameCount; ++i) {
        const uint32_t slot =
            (ringOffset + i) & (VSTPOC_AUDIO_RING_FRAMES - 1u);
        shared->audio[0][slot] = leftBase + static_cast<float>(i);
        shared->audio[1][slot] = rightBase - static_cast<float>(i);
    }
    VstpocOutputBlock& block =
        shared->output_blocks[blockIndex & (VSTPOC_OUTPUT_BLOCK_CAPACITY - 1u)];
    block.frame_count = frameCount;
    block.ring_offset = ringOffset;
    StoreRelease(&block.sequence, blockIndex + 1u);
}
void StageOutputBlockWithMidi(
    VstpocShared* shared, uint64_t blockIndex, uint32_t frameCount,
    uint32_t ringOffset, float leftBase, float rightBase,
    const guitarrackcraft::MidiEvent* midiEvents, uint32_t midiEventCount) {
    for (uint32_t i = 0; i < frameCount; ++i) {
        const uint32_t slot =
            (ringOffset + i) & (VSTPOC_AUDIO_RING_FRAMES - 1u);
        shared->audio[0][slot] = leftBase + static_cast<float>(i);
        shared->audio[1][slot] = rightBase - static_cast<float>(i);
    }

    VstpocOutputMidiBlock& midiBlock =
        shared->output_midi_blocks[
            blockIndex & (VSTPOC_OUTPUT_BLOCK_CAPACITY - 1u)];
    const uint32_t count =
        midiEvents ? (midiEventCount < VSTPOC_MAX_MIDI_EVENTS_PER_BLOCK
                          ? midiEventCount
                          : VSTPOC_MAX_MIDI_EVENTS_PER_BLOCK)
                   : 0u;
    midiBlock.event_count = count;
    midiBlock.reserved = 0;
    for (uint32_t i = 0; i < count; ++i) {
        midiBlock.events[i].frame_offset = midiEvents[i].frameOffset;
        midiBlock.events[i].status = midiEvents[i].status;
        midiBlock.events[i].data1 = midiEvents[i].data1;
        midiBlock.events[i].data2 = midiEvents[i].data2;
        midiBlock.events[i].reserved = 0;
    }
    StoreRelease(&midiBlock.sequence, blockIndex + 1u);

    VstpocOutputBlock& block =
        shared->output_blocks[blockIndex & (VSTPOC_OUTPUT_BLOCK_CAPACITY - 1u)];
    block.frame_count = frameCount;
    block.ring_offset = ringOffset;
    StoreRelease(&block.sequence, blockIndex + 1u);
}

bool TryStageOutputBlockWithMidi(
    VstpocShared* shared, uint32_t frameCount, float marker,
    const guitarrackcraft::MidiEvent* midiEvents, uint32_t midiEventCount) {
    const uint64_t audioHead = LoadAcquire(&shared->audio_head);
    const uint64_t audioTail = LoadAcquire(&shared->audio_tail);
    const uint64_t blockHead = LoadAcquire(&shared->output_block_head);
    const uint64_t blockTail = LoadAcquire(&shared->output_block_tail);
    if (blockHead - blockTail >= VSTPOC_OUTPUT_BLOCK_CAPACITY ||
        audioHead - audioTail + frameCount > VSTPOC_AUDIO_RING_FRAMES) {
        __atomic_add_fetch(&shared->output_drop_count, 1u, __ATOMIC_RELAXED);
        return false;
    }
    const uint32_t offset = static_cast<uint32_t>(
        audioHead & (VSTPOC_AUDIO_RING_FRAMES - 1u));
    StageOutputBlockWithMidi(shared, blockHead, frameCount, offset, marker,
                             -marker, midiEvents, midiEventCount);
    StoreRelease(&shared->audio_head, audioHead + frameCount);
    StoreRelease(&shared->output_block_head, blockHead + 1u);
    return true;
}


// This is the guest-side shared-memory protocol seam. It deliberately only
// models descriptor publication so the host-side consumer can be tested
// without starting a Wine process.
bool TryStageOutputBlock(VstpocShared* shared, uint32_t frameCount,
                         float marker) {
    const uint64_t audioHead = LoadAcquire(&shared->audio_head);
    const uint64_t audioTail = LoadAcquire(&shared->audio_tail);
    const uint64_t blockHead = LoadAcquire(&shared->output_block_head);
    const uint64_t blockTail = LoadAcquire(&shared->output_block_tail);
    if (blockHead - blockTail >= VSTPOC_OUTPUT_BLOCK_CAPACITY ||
        audioHead - audioTail + frameCount > VSTPOC_AUDIO_RING_FRAMES) {
        __atomic_add_fetch(&shared->output_drop_count, 1u, __ATOMIC_RELAXED);
        return false;
    }
    const uint32_t offset = static_cast<uint32_t>(
        audioHead & (VSTPOC_AUDIO_RING_FRAMES - 1u));
    StageOutputBlock(shared, blockHead, frameCount, offset, marker, -marker);
    StoreRelease(&shared->audio_head, audioHead + frameCount);
    StoreRelease(&shared->output_block_head, blockHead + 1u);
    return true;
}

TEST(VstSharedLayoutTest, MetadataAndV9FeatureEnvelopeAreStable) {
    TempBackingFile backing;
    SharedRing ring(backing.path());

    ASSERT_TRUE(ring.valid());
    ASSERT_TRUE(ring.wakeReady());
    const VstpocShared* shared = ring.raw();
    EXPECT_EQ(shared->shared_layout_magic, VSTPOC_SHARED_LAYOUT_MAGIC);
    EXPECT_EQ(shared->shared_layout_version, VSTPOC_SHARED_LAYOUT_VERSION);
    EXPECT_EQ(shared->shared_layout_size, sizeof(VstpocShared));
    EXPECT_EQ(shared->shared_feature_bits,
              static_cast<uint64_t>(VSTPOC_FEATURE_PLANAR_AUDIO |
                                    VSTPOC_FEATURE_WAKE_SOCKET |
                                    VSTPOC_FEATURE_MIDI_EVENTS |
                                    VSTPOC_FEATURE_MIDI_OUTPUT |
                                    VSTPOC_FEATURE_OUTPUT_BLOCK_MIDI));
    EXPECT_EQ(LoadAcquire(&shared->transport_queue_head), 0u);
    EXPECT_EQ(LoadAcquire(&shared->transport_queue_tail), 0u);
    EXPECT_EQ(LoadAcquire(&shared->transport_queue_dropped), 0u);
    EXPECT_EQ(LoadAcquire(&shared->output_block_head), 0u);
    EXPECT_EQ(LoadAcquire(&shared->output_block_tail), 0u);
}

TEST_F(SharedRingFixture, TransportRecordPreservesFieldsAndFIFOOrder) {
    constexpr uint32_t kRecords = 6;
    for (uint32_t i = 0; i < kRecords; ++i) {
        ASSERT_TRUE(ring.publishTransport(
            1000 + i, 2000 + i, 3000 + i, 48000.0 + i, 120.0 + i,
            (i & 1u) != 0, (i & 2u) != 0, 64 + i, nullptr, 0));
    }

    VstpocShared* shared = ring.raw();
    EXPECT_EQ(LoadAcquire(&shared->transport_queue_head), kRecords);
    EXPECT_EQ(LoadAcquire(&shared->transport_seq), kRecords + 1u);
    EXPECT_EQ(LoadAcquire(&shared->transport_queue_tail), 0u);

    for (uint32_t i = 0; i < kRecords; ++i) {
        const VstpocTransportBlock& record = shared->transport_queue[i];
        EXPECT_EQ(record.sample_position, 1000u + i);
        EXPECT_EQ(record.transport_frame, 2000u + i);
        EXPECT_EQ(record.loop_end_frame, 3000u + i);
        EXPECT_DOUBLE_EQ(record.sample_rate, 48000.0 + i);
        EXPECT_DOUBLE_EQ(record.beats_per_minute, 120.0 + i);
        EXPECT_EQ(record.flags, (i & 1u ? 1u : 0u) | (i & 2u ? 2u : 0u));
        EXPECT_EQ(record.block_frames, 64u + i);
    }
}

TEST_F(SharedRingFixture, TransportRecordPreservesMidiPayloadAndClampsFrameOffsets) {
    const guitarrackcraft::MidiEvent midiEvents[] = {
        {7u, 0x90u, 60u, 100u},
        {99u, 0x80u, 60u, 0u},
    };

    ASSERT_TRUE(ring.publishTransport(
        1000, 2000, 3000, 48000.0, 120.0, true, false, 64,
        midiEvents, 2));

    const VstpocTransportBlock& record = ring.raw()->transport_queue[0];
    EXPECT_EQ(record.midi_event_count, 2u);
    EXPECT_EQ(record.midi_events[0].frame_offset, 7u);
    EXPECT_EQ(record.midi_events[0].status, 0x90u);
    EXPECT_EQ(record.midi_events[0].data1, 60u);
    EXPECT_EQ(record.midi_events[0].data2, 100u);
    EXPECT_EQ(record.midi_events[0].reserved, 0u);
    EXPECT_EQ(record.midi_events[1].frame_offset, 63u);
    EXPECT_EQ(record.midi_events[1].status, 0x80u);
    EXPECT_EQ(record.midi_events[1].data1, 60u);
    EXPECT_EQ(record.midi_events[1].data2, 0u);
    EXPECT_EQ(record.midi_events[1].reserved, 0u);
}

TEST_F(SharedRingFixture, TransportQueueWrapsWithoutReordering) {
    VstpocShared* shared = ring.raw();
    const uint64_t start = VSTPOC_TRANSPORT_QUEUE_CAPACITY - 2u;
    StoreRelaxed(&shared->transport_queue_tail, start);
    StoreRelaxed(&shared->transport_queue_head, start);

    ASSERT_TRUE(ring.publishTransport(11, 21, 31, 44100.0, 90.0, true, false, 3,
                                      nullptr, 0));
    ASSERT_TRUE(ring.publishTransport(12, 22, 32, 44100.0, 91.0, false, true, 4,
                                      nullptr, 0));
    ASSERT_TRUE(ring.publishTransport(13, 23, 33, 44100.0, 92.0, true, true, 5,
                                      nullptr, 0));
    ASSERT_TRUE(ring.publishTransport(14, 24, 34, 44100.0, 93.0, false, false, 6,
                                      nullptr, 0));

    EXPECT_EQ(LoadAcquire(&shared->transport_queue_head), start + 4u);
    const VstpocTransportBlock& wrapped0 = shared->transport_queue[(start + 0u) & (VSTPOC_TRANSPORT_QUEUE_CAPACITY - 1u)];
    const VstpocTransportBlock& wrapped1 = shared->transport_queue[(start + 1u) & (VSTPOC_TRANSPORT_QUEUE_CAPACITY - 1u)];
    const VstpocTransportBlock& wrapped2 = shared->transport_queue[(start + 2u) & (VSTPOC_TRANSPORT_QUEUE_CAPACITY - 1u)];
    const VstpocTransportBlock& wrapped3 = shared->transport_queue[(start + 3u) & (VSTPOC_TRANSPORT_QUEUE_CAPACITY - 1u)];
    EXPECT_EQ(wrapped0.sample_position, 11u);
    EXPECT_EQ(wrapped1.sample_position, 12u);
    EXPECT_EQ(wrapped2.sample_position, 13u);
    EXPECT_EQ(wrapped3.sample_position, 14u);
    EXPECT_EQ(wrapped0.loop_end_frame, 31u);
    EXPECT_EQ(wrapped1.loop_end_frame, 32u);
    EXPECT_EQ(wrapped2.loop_end_frame, 33u);
    EXPECT_EQ(wrapped3.loop_end_frame, 34u);
    EXPECT_EQ(wrapped0.flags, 1u);
    EXPECT_EQ(wrapped1.flags, 2u);
    EXPECT_EQ(wrapped2.flags, 3u);
    EXPECT_EQ(wrapped3.flags, 0u);
}

TEST_F(SharedRingFixture, FullTransportQueueRejectsWithoutOverwritingOrAdvancing) {
    VstpocShared* shared = ring.raw();
    const uint64_t tail = 37;
    const uint64_t head = tail + VSTPOC_TRANSPORT_QUEUE_CAPACITY;
    StoreRelaxed(&shared->transport_queue_tail, tail);
    StoreRelaxed(&shared->transport_queue_head, head);
    StoreRelaxed(&shared->transport_queue_dropped, 0);
    VstpocTransportBlock& occupied = shared->transport_queue[head & (VSTPOC_TRANSPORT_QUEUE_CAPACITY - 1u)];
    occupied.sample_position = 777;
    occupied.loop_end_frame = 999;
    occupied.transport_frame = 888;
    occupied.sample_rate = 96000.0;

    EXPECT_FALSE(ring.publishTransport(1, 2, 3, 4.0, 5.0, true, true, 6,
                                       nullptr, 0));
    EXPECT_EQ(LoadAcquire(&shared->transport_queue_head), head);
    EXPECT_EQ(LoadAcquire(&shared->transport_queue_tail), tail);
    EXPECT_EQ(LoadAcquire(&shared->transport_queue_dropped), 1u);
    EXPECT_EQ(occupied.sample_position, 777u);
    EXPECT_EQ(occupied.transport_frame, 888u);
    EXPECT_EQ(occupied.loop_end_frame, 999u);
    EXPECT_DOUBLE_EQ(occupied.sample_rate, 96000.0);
}

TEST_F(SharedRingFixture, CompleteOutputDescriptorBecomesVisibleOnlyAfterSequenceCommit) {
    VstpocShared* shared = ring.raw();
    constexpr uint32_t kFrames = 4;
    constexpr uint32_t kOffset = 100;
    StoreRelease(&shared->audio_tail, kOffset);
    StageOutputBlock(shared, 0, kFrames, kOffset, 10.0f, -10.0f);
    StoreRelaxed(&shared->output_blocks[0].sequence, 0u);
    StoreRelease(&shared->audio_head, kOffset + kFrames);
    StoreRelease(&shared->output_block_head, 1u);

    float left[kFrames] = {91.0f, 92.0f, 93.0f, 94.0f};
    float right[kFrames] = {-91.0f, -92.0f, -93.0f, -94.0f};
    EXPECT_EQ(ring.pullAudio(left, right, kFrames), 0);
    EXPECT_FLOAT_EQ(left[0], 91.0f);
    EXPECT_FLOAT_EQ(right[0], -91.0f);
    EXPECT_EQ(LoadAcquire(&shared->output_block_tail), 0u);
    EXPECT_EQ(LoadAcquire(&shared->audio_tail), kOffset);

    StoreRelease(&shared->output_blocks[0].sequence, 1u);
    ASSERT_EQ(ring.pullAudio(left, right, kFrames), kFrames);
    for (uint32_t i = 0; i < kFrames; ++i) {
        EXPECT_FLOAT_EQ(left[i], 10.0f + static_cast<float>(i));
        EXPECT_FLOAT_EQ(right[i], -10.0f - static_cast<float>(i));
    }
    EXPECT_EQ(LoadAcquire(&shared->output_block_tail), 1u);
    EXPECT_EQ(LoadAcquire(&shared->audio_tail), kOffset + kFrames);
}

TEST_F(SharedRingFixture, StaleOutputDescriptorSequenceIsNotConsumedOrExposed) {
    VstpocShared* shared = ring.raw();
    StoreRelease(&shared->audio_tail, 12u);
    StageOutputBlock(shared, 0, 2, 12, 3.0f, -3.0f);
    StoreRelaxed(&shared->output_blocks[0].sequence, 99u);
    StoreRelease(&shared->audio_head, 14u);
    StoreRelease(&shared->output_block_head, 1u);

    float left[2] = {7.0f, 8.0f};
    float right[2] = {-7.0f, -8.0f};
    EXPECT_EQ(ring.pullAudio(left, right, 2), 0);
    EXPECT_FLOAT_EQ(left[0], 7.0f);
    EXPECT_FLOAT_EQ(right[0], -7.0f);
    EXPECT_EQ(LoadAcquire(&shared->output_block_tail), 0u);
    EXPECT_EQ(LoadAcquire(&shared->audio_tail), 12u);
}

TEST_F(SharedRingFixture, PartialOutputRequestDropsWholeBlockWithoutTouchingProducerHeads) {
    VstpocShared* shared = ring.raw();
    constexpr uint32_t kFrames = 8;
    constexpr uint32_t kOffset = 32;
    StoreRelease(&shared->audio_tail, kOffset);
    StageOutputBlock(shared, 0, kFrames, kOffset, 20.0f, -20.0f);
    StoreRelease(&shared->audio_head, kOffset + kFrames);
    StoreRelease(&shared->output_block_head, 1u);

    float left[4] = {81.0f, 82.0f, 83.0f, 84.0f};
    float right[4] = {-81.0f, -82.0f, -83.0f, -84.0f};
    EXPECT_EQ(ring.pullAudio(left, right, 4), 0);
    for (uint32_t i = 0; i < 4; ++i) {
        EXPECT_FLOAT_EQ(left[i], 81.0f + static_cast<float>(i));
        EXPECT_FLOAT_EQ(right[i], -81.0f - static_cast<float>(i));
    }
    EXPECT_EQ(LoadAcquire(&shared->output_block_head), 1u);
    EXPECT_EQ(LoadAcquire(&shared->audio_head), kOffset + kFrames);
    EXPECT_EQ(LoadAcquire(&shared->output_block_tail), 1u);
    EXPECT_EQ(LoadAcquire(&shared->audio_tail), kOffset + kFrames);
}

TEST_F(SharedRingFixture, FullOutputQueueDropsNewestAndPreservesCommittedDescriptors) {
    VstpocShared* shared = ring.raw();
    StoreRelaxed(&shared->output_drop_count, 0u);
    for (uint32_t i = 0; i < VSTPOC_OUTPUT_BLOCK_CAPACITY; ++i) {
        ASSERT_TRUE(TryStageOutputBlock(shared, 1, static_cast<float>(i)));
    }
    ASSERT_EQ(LoadAcquire(&shared->output_block_head),
              static_cast<uint64_t>(VSTPOC_OUTPUT_BLOCK_CAPACITY));
    const VstpocOutputBlock first = shared->output_blocks[0];
    const float firstSample = shared->audio[0][0];

    EXPECT_FALSE(TryStageOutputBlock(shared, 1, 999.0f));
    EXPECT_EQ(LoadAcquire(&shared->output_drop_count), 1u);
    EXPECT_EQ(LoadAcquire(&shared->output_block_head),
              static_cast<uint64_t>(VSTPOC_OUTPUT_BLOCK_CAPACITY));
    EXPECT_EQ(shared->output_blocks[0].sequence, first.sequence);
    EXPECT_EQ(shared->output_blocks[0].frame_count, first.frame_count);
    EXPECT_EQ(shared->output_blocks[0].ring_offset, first.ring_offset);
    EXPECT_FLOAT_EQ(shared->audio[0][0], firstSample);
}

TEST_F(SharedRingFixture, PullAudioAdvancesConsumerTailsButNeverProducerHeads) {
    VstpocShared* shared = ring.raw();
    StoreRelease(&shared->audio_tail, 20u);
    StageOutputBlock(shared, 0, 3, 20, 31.0f, -31.0f);
    StoreRelease(&shared->audio_head, 23u);
    StoreRelease(&shared->output_block_head, 1u);

    float left[3] = {};
    float right[3] = {};
    ASSERT_EQ(ring.pullAudio(left, right, 3), 3);
    EXPECT_EQ(LoadAcquire(&shared->output_block_tail), 1u);
    EXPECT_EQ(LoadAcquire(&shared->audio_tail), 23u);
    EXPECT_EQ(LoadAcquire(&shared->output_block_head), 1u);
    EXPECT_EQ(LoadAcquire(&shared->audio_head), 23u);
}

TEST_F(SharedRingFixture,
       PullAudioTrimsMismatchedBlocksAndConsumesWrappedNonDivisorQuantum) {
    VstpocShared* shared = ring.raw();
    constexpr uint32_t kQuantum = 257;  // Deliberately does not divide the ring.
    constexpr uint32_t kStaleFrames = 256;
    const uint64_t audioStart =
        VSTPOC_AUDIO_RING_FRAMES - (kStaleFrames + 2u);
    const uint64_t blockStart = VSTPOC_OUTPUT_BLOCK_CAPACITY - 1u;
    const uint64_t secondStart = audioStart + kStaleFrames;
    const uint64_t thirdStart = secondStart + kQuantum;

    // The first descriptor is complete but mismatched for this host quantum.
    // The following descriptors are exact blocks; both descriptor and audio
    // indices cross their physical ring boundaries.
    StoreRelease(&shared->audio_tail, audioStart);
    StoreRelease(&shared->audio_head, audioStart);
    StoreRelease(&shared->output_block_tail, blockStart);
    StoreRelease(&shared->output_block_head, blockStart);
    StoreRelease(&shared->output_drop_count, 0u);
    ASSERT_TRUE(TryStageOutputBlock(shared, kStaleFrames, 10.0f));
    ASSERT_TRUE(TryStageOutputBlock(shared, kQuantum, 100.0f));
    ASSERT_TRUE(TryStageOutputBlock(shared, kQuantum, 200.0f));
    EXPECT_EQ(LoadAcquire(&shared->output_drop_count), 0u);
    EXPECT_EQ(LoadAcquire(&shared->audio_head), thirdStart + kQuantum);
    EXPECT_EQ(LoadAcquire(&shared->output_block_head), blockStart + 3u);

    std::vector<float> left(kQuantum, -1.0f);
    std::vector<float> right(kQuantum, 1.0f);
    ASSERT_EQ(ring.pullAudio(left.data(), right.data(), kQuantum), kQuantum);
    for (uint32_t i = 0; i < kQuantum; ++i) {
        EXPECT_FLOAT_EQ(left[i], 200.0f + static_cast<float>(i));
        EXPECT_FLOAT_EQ(right[i], -200.0f - static_cast<float>(i));
    }
    EXPECT_EQ(LoadAcquire(&shared->output_block_tail), blockStart + 3u);
    EXPECT_EQ(LoadAcquire(&shared->audio_tail), thirdStart + kQuantum);
    EXPECT_EQ(LoadAcquire(&shared->audio_head), thirdStart + kQuantum);

    EXPECT_EQ(ring.pullAudio(left.data(), right.data(), kQuantum), 0);
    EXPECT_EQ(LoadAcquire(&shared->output_block_tail), blockStart + 3u);
    EXPECT_EQ(LoadAcquire(&shared->audio_tail), thirdStart + kQuantum);
    EXPECT_EQ(LoadAcquire(&shared->audio_head), thirdStart + kQuantum);
}


TEST_F(SharedRingFixture,
       DefaultPullReturnsLatestMatchingBlockAndTrimsEarlierCommittedBlocks) {
    VstpocShared* shared = ring.raw();
    StoreRelease(&shared->audio_tail, 32u);
    StoreRelease(&shared->audio_head, 32u);
    StoreRelease(&shared->output_block_tail, 0u);
    StoreRelease(&shared->output_block_head, 0u);
    ASSERT_TRUE(TryStageOutputBlock(shared, 4, 10.0f));
    ASSERT_TRUE(TryStageOutputBlock(shared, 4, 20.0f));
    ASSERT_TRUE(TryStageOutputBlock(shared, 4, 30.0f));

    float left[4] = {};
    float right[4] = {};
    ASSERT_EQ(ring.pullAudio(left, right, 4), 4);
    for (uint32_t i = 0; i < 4; ++i) {
        EXPECT_FLOAT_EQ(left[i], 30.0f + static_cast<float>(i));
        EXPECT_FLOAT_EQ(right[i], -30.0f - static_cast<float>(i));
    }
    EXPECT_EQ(LoadAcquire(&shared->output_block_tail), 3u);
    EXPECT_EQ(LoadAcquire(&shared->audio_tail), 44u);
    EXPECT_EQ(LoadAcquire(&shared->output_block_head), 3u);
    EXPECT_EQ(LoadAcquire(&shared->audio_head), 44u);
}


TEST_F(SharedRingFixture, PullAudioBlockFifoPairsEachDescriptorWithItsMidi) {
    VstpocShared* shared = ring.raw();
    StoreRelease(&shared->audio_tail, 32u);
    StoreRelease(&shared->audio_head, 32u);
    StoreRelease(&shared->output_block_tail, 0u);
    StoreRelease(&shared->output_block_head, 0u);

    const guitarrackcraft::MidiEvent firstMidi[] = {
        {1u, 0x90u, 60u, 101u},
    };
    const guitarrackcraft::MidiEvent secondMidi[] = {
        {3u, 0x80u, 60u, 0u},
        {7u, 0x90u, 64u, 117u},
    };
    ASSERT_TRUE(TryStageOutputBlockWithMidi(shared, 4, 10.0f, firstMidi, 1));
    ASSERT_TRUE(
        TryStageOutputBlockWithMidi(shared, 4, 20.0f, secondMidi, 2));

    float left[4] = {};
    float right[4] = {};
    guitarrackcraft::MidiEvent midi[2] = {};
    uint32_t midiCount = 0;
    ASSERT_EQ(ring.pullAudioBlock(left, right, 4, midi, 2, &midiCount, true),
              4);
    EXPECT_FLOAT_EQ(left[0], 10.0f);
    EXPECT_FLOAT_EQ(right[0], -10.0f);
    ASSERT_EQ(midiCount, 1u);
    EXPECT_EQ(midi[0].frameOffset, 1u);
    EXPECT_EQ(midi[0].status, 0x90u);
    EXPECT_EQ(midi[0].data1, 60u);
    EXPECT_EQ(midi[0].data2, 101u);

    midiCount = 0;
    ASSERT_EQ(ring.pullAudioBlock(left, right, 4, midi, 2, &midiCount, true),
              4);
    EXPECT_FLOAT_EQ(left[0], 20.0f);
    EXPECT_FLOAT_EQ(right[0], -20.0f);
    ASSERT_EQ(midiCount, 2u);
    EXPECT_EQ(midi[0].frameOffset, 3u);
    EXPECT_EQ(midi[0].status, 0x80u);
    EXPECT_EQ(midi[0].data1, 60u);
    EXPECT_EQ(midi[0].data2, 0u);
    EXPECT_EQ(midi[1].frameOffset, 7u);
    EXPECT_EQ(midi[1].status, 0x90u);
    EXPECT_EQ(midi[1].data1, 64u);
    EXPECT_EQ(midi[1].data2, 117u);
}

TEST_F(SharedRingFixture, PullAudioBlockLatestOnlyPairsNewestDescriptorWithMidi) {
    VstpocShared* shared = ring.raw();
    StoreRelease(&shared->audio_tail, 32u);
    StoreRelease(&shared->audio_head, 32u);
    StoreRelease(&shared->output_block_tail, 0u);
    StoreRelease(&shared->output_block_head, 0u);

    const guitarrackcraft::MidiEvent oldMidi[] = {
        {2u, 0x90u, 36u, 90u},
    };
    const guitarrackcraft::MidiEvent newestMidi[] = {
        {5u, 0x90u, 72u, 110u},
    };
    ASSERT_TRUE(TryStageOutputBlockWithMidi(shared, 4, 40.0f, oldMidi, 1));
    ASSERT_TRUE(
        TryStageOutputBlockWithMidi(shared, 4, 50.0f, newestMidi, 1));

    float left[4] = {};
    float right[4] = {};
    guitarrackcraft::MidiEvent midi[1] = {};
    uint32_t midiCount = 0;
    ASSERT_EQ(ring.pullAudioBlock(left, right, 4, midi, 1, &midiCount, false),
              4);
    EXPECT_FLOAT_EQ(left[0], 50.0f);
    EXPECT_FLOAT_EQ(right[0], -50.0f);
    ASSERT_EQ(midiCount, 1u);
    EXPECT_EQ(midi[0].frameOffset, 5u);
    EXPECT_EQ(midi[0].status, 0x90u);
    EXPECT_EQ(midi[0].data1, 72u);
    EXPECT_EQ(midi[0].data2, 110u);
}

TEST_F(SharedRingFixture, PullAudioBlockFifoPairsMidiAcrossDescriptorAndAudioWrap) {
    VstpocShared* shared = ring.raw();
    const uint64_t blockStart = VSTPOC_OUTPUT_BLOCK_CAPACITY - 1u;
    const uint64_t audioStart = VSTPOC_AUDIO_RING_FRAMES - 2u;
    StoreRelease(&shared->audio_tail, audioStart);
    StoreRelease(&shared->audio_head, audioStart);
    StoreRelease(&shared->output_block_tail, blockStart);
    StoreRelease(&shared->output_block_head, blockStart);

    const guitarrackcraft::MidiEvent firstMidi[] = {
        {0u, 0x90u, 48u, 80u},
    };
    const guitarrackcraft::MidiEvent secondMidi[] = {
        {3u, 0x90u, 49u, 81u},
    };
    ASSERT_TRUE(TryStageOutputBlockWithMidi(shared, 4, 60.0f, firstMidi, 1));
    ASSERT_TRUE(
        TryStageOutputBlockWithMidi(shared, 4, 70.0f, secondMidi, 1));

    float left[4] = {};
    float right[4] = {};
    guitarrackcraft::MidiEvent midi[1] = {};
    uint32_t midiCount = 0;
    ASSERT_EQ(ring.pullAudioBlock(left, right, 4, midi, 1, &midiCount, true),
              4);
    EXPECT_FLOAT_EQ(left[0], 60.0f);
    EXPECT_EQ(midiCount, 1u);
    EXPECT_EQ(midi[0].data1, 48u);

    midiCount = 0;
    ASSERT_EQ(ring.pullAudioBlock(left, right, 4, midi, 1, &midiCount, true),
              4);
    EXPECT_FLOAT_EQ(left[0], 70.0f);
    EXPECT_EQ(midiCount, 1u);
    EXPECT_EQ(midi[0].data1, 49u);
}

TEST_F(SharedRingFixture, PullAudioBlockRejectsUncommittedOrWrongMidiSequence) {
    VstpocShared* shared = ring.raw();
    StoreRelease(&shared->audio_tail, 64u);
    StoreRelease(&shared->audio_head, 64u);
    StoreRelease(&shared->output_block_tail, 0u);
    StoreRelease(&shared->output_block_head, 0u);

    const guitarrackcraft::MidiEvent firstMidi[] = {
        {1u, 0x90u, 55u, 100u},
    };
    const guitarrackcraft::MidiEvent secondMidi[] = {
        {2u, 0x90u, 56u, 101u},
    };
    ASSERT_TRUE(TryStageOutputBlockWithMidi(shared, 4, 80.0f, firstMidi, 1));
    ASSERT_TRUE(
        TryStageOutputBlockWithMidi(shared, 4, 90.0f, secondMidi, 1));
    StoreRelaxed(&shared->output_midi_blocks[0].sequence, 0u);
    StoreRelaxed(&shared->output_midi_blocks[1].sequence, 999u);

    float left[4] = {};
    float right[4] = {};
    guitarrackcraft::MidiEvent midi[1] = {{77u, 0xf0u, 1u, 2u}};
    uint32_t midiCount = 99u;
    ASSERT_EQ(ring.pullAudioBlock(left, right, 4, midi, 1, &midiCount, true),
              4);
    EXPECT_FLOAT_EQ(left[0], 80.0f);
    EXPECT_EQ(midiCount, 0u);
    EXPECT_EQ(midi[0].status, 0xf0u);

    midiCount = 99u;
    ASSERT_EQ(ring.pullAudioBlock(left, right, 4, midi, 1, &midiCount, true),
              4);
    EXPECT_FLOAT_EQ(left[0], 90.0f);
    EXPECT_EQ(midiCount, 0u);
    EXPECT_EQ(midi[0].status, 0xf0u);
}

TEST_F(SharedRingFixture, TransportPublicationDerivesDeadlineFromFramesAndRate) {
    constexpr uint32_t kFrames = 257;
    constexpr double kRate = 44100.0;
    ASSERT_TRUE(ring.publishTransport(0, 0, 0, kRate, 120.0, true, false,
                                      kFrames, nullptr, 0));
    const uint64_t expected = static_cast<uint64_t>(
        static_cast<double>(kFrames) * 1'000'000'000.0 / kRate);
    EXPECT_EQ(ring.guestDeadlineNs(), expected);

    ASSERT_TRUE(ring.publishTransport(0, kFrames, 0, 48000.0, 120.0, true,
                                      false, 128, nullptr, 0));
    EXPECT_EQ(ring.guestDeadlineNs(), static_cast<uint64_t>(
        128.0 * 1'000'000'000.0 / 48000.0));
}

TEST_F(SharedRingFixture, HealthGettersKeepDeadlineStarvationAndDropDistinct) {
    VstpocShared* shared = ring.raw();
    StoreRelease(&shared->block_deadline_ns, 1234567u);
    StoreRelease(&shared->deadline_miss_count, 11u);
    StoreRelease(&shared->starvation_count, 23u);
    StoreRelease(&shared->output_drop_count, 37u);

    EXPECT_EQ(ring.guestDeadlineNs(), 1234567u);
    EXPECT_EQ(ring.deadlineMissCount(), 11u);
    EXPECT_EQ(ring.starvationCount(), 23u);
    EXPECT_EQ(ring.outputDropCount(), 37u);
}

TEST_F(SharedRingFixture,
       TransportWakeRequestIsOnePerEnqueueAfterGuestClearsIt) {
    VstpocShared* shared = ring.raw();
    StoreRelease(&shared->transport_queue_head, 0u);
    StoreRelease(&shared->transport_queue_tail, 0u);
    StoreRelease(&shared->wake_requested, 0u);

    ASSERT_TRUE(ring.publishTransport(0, 0, 0, 48000.0, 120.0, true, false,
                                      257, nullptr, 0));
    EXPECT_EQ(ConsumeWakeRequest(shared), 1u);
    ASSERT_TRUE(ring.publishTransport(257, 257, 0, 48000.0, 120.0, true,
                                      false, 257, nullptr, 0));
    EXPECT_EQ(ConsumeWakeRequest(shared), 1u);

    // The consumer advances its tail between producer commits. The next
    // enqueue must still publish a wake even though an old empty/non-empty
    // observation could be stale.
    StoreRelease(&shared->transport_queue_tail, 2u);
    ASSERT_TRUE(ring.publishTransport(514, 514, 0, 48000.0, 120.0, true,
                                      false, 257, nullptr, 0));
    EXPECT_EQ(ConsumeWakeRequest(shared), 1u);
    EXPECT_EQ(LoadAcquire(&shared->transport_queue_head), 3u);
}

TEST_F(SharedRingFixture, WakeListenerAdvertisesBoundedPath) {
    ASSERT_TRUE(ring.valid());
    ASSERT_TRUE(ring.wakeReady());

    const std::string wakePath = ring.wakePath();
    ASSERT_FALSE(wakePath.empty());
    sockaddr_un address{};
    EXPECT_LE(wakePath.size() + 1u, sizeof(address.sun_path));
    EXPECT_EQ(::access(wakePath.c_str(), F_OK), 0);
    EXPECT_NE(ring.raw()->shared_feature_bits & VSTPOC_FEATURE_WAKE_SOCKET, 0u);
}

TEST_F(SharedRingFixture,
       TransportWakeArrivesOnEmptyToNonemptyAndCoalescesUntilConsumed) {
    ASSERT_TRUE(ring.wakeReady());
    const int client = ConnectWake(ring.wakePath());
    ASSERT_NE(client, -1);
    ASSERT_TRUE(PrimeWakeClient(ring, client));
    StoreRelease(&ring.raw()->wake_requested, 0u);

    ASSERT_TRUE(ring.publishTransport(0, 0, 0, 48000.0, 120.0, true,
                                      false, 64, nullptr, 0));
    EXPECT_TRUE(ReceiveWake(client));

    ASSERT_TRUE(ring.publishTransport(64, 64, 0, 48000.0, 120.0, true,
                                      false, 64, nullptr, 0));
    EXPECT_FALSE(ReceiveWake(client, kNoWakePollTimeoutMs));
    StoreRelease(&ring.raw()->wake_requested, 0u);

    VstpocShared* shared = ring.raw();
    StoreRelease(&shared->transport_queue_tail,
                 LoadAcquire(&shared->transport_queue_head));

    ASSERT_TRUE(ring.publishTransport(128, 128, 0, 48000.0, 120.0, true,
                                      false, 64, nullptr, 0));
    EXPECT_TRUE(ReceiveWake(client));
    ::close(client);
}
TEST(SharedRingWakeTest, UnsetExpectedPeerRejectsWakeClient) {
    TempBackingFile backing;
    SharedRing ring(backing.path());
    ASSERT_TRUE(ring.valid());
    ASSERT_TRUE(ring.wakeReady());

    const int client = ConnectWake(ring.wakePath());
    ASSERT_NE(client, -1);
    ring.notifyGuest();
    EXPECT_FALSE(ReceiveWake(client, kNoWakePollTimeoutMs));
    ::close(client);
}
TEST(SharedRingWakeTest, WakeClientFromDifferentProcessIsRejected) {
    TempBackingFile backing;
    SharedRing ring(backing.path());
    ASSERT_TRUE(ring.valid());
    ASSERT_TRUE(ring.wakeReady());
    ring.setExpectedWakePeer(getpid());

    int connected[2] = {-1, -1};
    ASSERT_EQ(::pipe(connected), 0);
    const pid_t child = ::fork();
    ASSERT_NE(child, -1);
    if (child == 0) {
        ::close(connected[0]);
        const int client = ConnectWake(ring.wakePath());
        const char status = client >= 0 ? 'C' : 'F';
        (void)::write(connected[1], &status, 1);
        if (client < 0) _exit(2);
        const bool received = ReceiveWake(client, kWakePollTimeoutMs);
        ::close(client);
        _exit(received ? 1 : 0);
    }

    ::close(connected[1]);
    char status = '\0';
    ASSERT_EQ(::read(connected[0], &status, 1), 1);
    ASSERT_EQ(status, 'C');
    ::close(connected[0]);

    ring.notifyGuest();
    int childStatus = 0;
    ASSERT_EQ(::waitpid(child, &childStatus, 0), child);
    ASSERT_TRUE(WIFEXITED(childStatus));
    EXPECT_EQ(WEXITSTATUS(childStatus), 0);
}

TEST_F(SharedRingFixture,
       InputWakeArrivesOnEmptyToNonemptyAndCoalescesUntilConsumed) {
    ASSERT_TRUE(ring.wakeReady());
    const int client = ConnectWake(ring.wakePath());
    ASSERT_NE(client, -1);
    ASSERT_TRUE(PrimeWakeClient(ring, client));
    StoreRelease(&ring.raw()->wake_requested, 0u);

    const float left[] = {1.0f};
    const float right[] = {-1.0f};
    ASSERT_EQ(ring.pushInput(left, right, 1), 1);
    EXPECT_TRUE(ReceiveWake(client));

    ASSERT_EQ(ring.pushInput(left, right, 1), 1);
    EXPECT_FALSE(ReceiveWake(client, kNoWakePollTimeoutMs));
    StoreRelease(&ring.raw()->wake_requested, 0u);

    VstpocShared* shared = ring.raw();
    StoreRelease(&shared->audio_in_tail,
                 LoadAcquire(&shared->audio_in_head));
    ASSERT_EQ(ring.pushInput(left, right, 1), 1);
    EXPECT_TRUE(ReceiveWake(client));
    ::close(client);
}


TEST_F(SharedRingFixture,
       TransportAndInputWakeCoalesceUntilGuestClearsWakeRequest) {
    ASSERT_TRUE(ring.wakeReady());
    const int client = ConnectWake(ring.wakePath());
    ASSERT_NE(client, -1);
    ASSERT_TRUE(PrimeWakeClient(ring, client));
    StoreRelease(&ring.raw()->wake_requested, 0u);

    const float left[] = {2.0f};
    const float right[] = {-2.0f};
    ASSERT_TRUE(ring.publishTransport(0, 0, 0, 48000.0, 120.0, true,
                                      false, 1, nullptr, 0));
    EXPECT_EQ(ReceiveWakeBytes(client), 1);

    ASSERT_EQ(ring.pushInput(left, right, 1), 1);
    EXPECT_FALSE(ReceiveWake(client, kNoWakePollTimeoutMs));

    VstpocShared* shared = ring.raw();
    StoreRelease(&shared->transport_queue_tail,
                 LoadAcquire(&shared->transport_queue_head));
    StoreRelease(&shared->audio_in_tail,
                 LoadAcquire(&shared->audio_in_head));
    StoreRelease(&shared->wake_requested, 0u);

    ASSERT_EQ(ring.pushInput(left, right, 1), 1);
    EXPECT_EQ(ReceiveWakeBytes(client), 1);
    StoreRelease(&shared->wake_requested, 0u);

    StoreRelease(&shared->audio_in_tail,
                 LoadAcquire(&shared->audio_in_head));
    StoreRelease(&shared->transport_queue_tail,
                 LoadAcquire(&shared->transport_queue_head));
    ASSERT_TRUE(ring.publishTransport(1, 1, 0, 48000.0, 120.0, true,
                                      false, 1, nullptr, 0));
    EXPECT_EQ(ReceiveWakeBytes(client), 1);
    ::close(client);
}
TEST_F(SharedRingFixture, ParamNotifyAndStopAlwaysWakeConnectedClient) {
    ASSERT_TRUE(ring.wakeReady());
    const int client = ConnectWake(ring.wakePath());
    ASSERT_NE(client, -1);
    ASSERT_TRUE(PrimeWakeClient(ring, client));
    StoreRelease(&ring.raw()->wake_requested, 0u);

    ring.pushParam(4, 0.5f);
    EXPECT_TRUE(ReceiveWake(client));
    StoreRelease(&ring.raw()->wake_requested, 0u);

    ring.notifyGuest();
    EXPECT_TRUE(ReceiveWake(client));
    StoreRelease(&ring.raw()->wake_requested, 0u);

    ring.signalStop();
    EXPECT_TRUE(ReceiveWake(client));
    StoreRelease(&ring.raw()->wake_requested, 0u);
    EXPECT_EQ(LoadAcquire(&ring.raw()->stop_flag), 1u);
    ::close(client);
}

TEST_F(SharedRingFixture,
       DisconnectedClientDoesNotBlockWakeAndReconnectReceivesLaterWake) {
    ASSERT_TRUE(ring.wakeReady());
    const int disconnected = ConnectWake(ring.wakePath());
    ASSERT_NE(disconnected, -1);
    ASSERT_TRUE(PrimeWakeClient(ring, disconnected));
    StoreRelease(&ring.raw()->wake_requested, 0u);
    ::close(disconnected);

    // Exercise sends against the closed peer. These calls must remain
    // non-blocking and must not terminate the test process with SIGPIPE.
    ring.notifyGuest();
    ring.notifyGuest();

    const int reconnected = ConnectWake(ring.wakePath());
    ASSERT_NE(reconnected, -1);
    EXPECT_TRUE(PrimeWakeClient(ring, reconnected));
    ::close(reconnected);
}

TEST(SharedRingWakeTest, DestructorRemovesAdvertisedSocketPath) {
    TempBackingFile backing;
    std::string wakePath;
    {
        SharedRing ring(backing.path());
        ASSERT_TRUE(ring.valid());
        ASSERT_TRUE(ring.wakeReady());
        wakePath = ring.wakePath();
        ASSERT_FALSE(wakePath.empty());
        ASSERT_EQ(::access(wakePath.c_str(), F_OK), 0);
    }
    EXPECT_EQ(::access(wakePath.c_str(), F_OK), -1);
    EXPECT_EQ(errno, ENOENT);
}

TEST_F(SharedRingFixture, GuestHealthAndStarvationCountersRemainObservable) {
    VstpocShared* shared = ring.raw();
    StoreRelease(&shared->guest_ready, 1u);
    StoreRelaxed(&shared->guest_state, VSTPOC_GUEST_STATE_RUNNING);
    StoreRelaxed(&shared->guest_frames_produced, 256u);
    StoreRelaxed(&shared->guest_heartbeat, 11u);
    StoreRelaxed(&shared->block_deadline_ns, 123456789u);
    StoreRelaxed(&shared->deadline_miss_count, 0u);
    StoreRelaxed(&shared->starvation_count, 0u);
    StoreRelaxed(&shared->output_drop_count, 0u);

    ASSERT_TRUE(ring.guestReady());
    EXPECT_EQ(ring.guestFramesProduced(), 256u);
    EXPECT_EQ(LoadAcquire(&shared->guest_state), VSTPOC_GUEST_STATE_RUNNING);
    EXPECT_EQ(LoadAcquire(&shared->guest_heartbeat), 11u);
    EXPECT_EQ(LoadAcquire(&shared->block_deadline_ns), 123456789u);

    __atomic_add_fetch(&shared->deadline_miss_count, 1u, __ATOMIC_RELAXED);
    __atomic_add_fetch(&shared->deadline_miss_count, 1u, __ATOMIC_RELAXED);
    __atomic_add_fetch(&shared->starvation_count, 1u, __ATOMIC_RELAXED);
    __atomic_add_fetch(&shared->starvation_count, 1u, __ATOMIC_RELAXED);
    __atomic_add_fetch(&shared->output_drop_count, 1u, __ATOMIC_RELAXED);
    StoreRelease(&shared->guest_state, VSTPOC_GUEST_STATE_STARVED);
    EXPECT_EQ(LoadAcquire(&shared->guest_state), VSTPOC_GUEST_STATE_STARVED);
    EXPECT_EQ(LoadAcquire(&shared->deadline_miss_count), 2u);
    EXPECT_EQ(LoadAcquire(&shared->starvation_count), 2u);
    EXPECT_EQ(LoadAcquire(&shared->output_drop_count), 1u);

    ring.notifyGuest();
    EXPECT_EQ(LoadAcquire(&shared->deadline_miss_count), 2u);
    EXPECT_EQ(LoadAcquire(&shared->starvation_count), 2u);
    EXPECT_EQ(LoadAcquire(&shared->output_drop_count), 1u);
}

TEST_F(SharedRingFixture, InputPreflightAndPushAreAllOrNothingNearWrap) {
    VstpocShared* shared = ring.raw();
    const uint64_t start = VSTPOC_AUDIO_RING_FRAMES - 2u;
    StoreRelaxed(&shared->audio_in_tail, start);
    StoreRelaxed(&shared->audio_in_head, start);
    EXPECT_TRUE(ring.inputWritable(VSTPOC_AUDIO_RING_FRAMES));
    EXPECT_FALSE(ring.inputWritable(VSTPOC_AUDIO_RING_FRAMES + 1u));

    const float wrappedLeft[] = {1.0f, 2.0f, 3.0f, 4.0f};
    const float wrappedRight[] = {-1.0f, -2.0f, -3.0f, -4.0f};
    ASSERT_TRUE(ring.inputWritable(4));
    ASSERT_EQ(ring.pushInput(wrappedLeft, wrappedRight, 4), 4);
    EXPECT_EQ(LoadAcquire(&shared->audio_in_head), start + 4u);
    for (uint32_t i = 0; i < 4; ++i) {
        const uint64_t slot = AudioSlot(start + i);
        EXPECT_FLOAT_EQ(shared->audio_in[0][slot], wrappedLeft[i]);
        EXPECT_FLOAT_EQ(shared->audio_in[1][slot], wrappedRight[i]);
    }

    const uint64_t nearlyFullHead = start + 4u + VSTPOC_AUDIO_RING_FRAMES - 2u;
    StoreRelaxed(&shared->audio_in_head, nearlyFullHead);
    StoreRelease(&shared->audio_in_tail, start + 4u);
    const uint64_t occupiedSlot = AudioSlot(nearlyFullHead);
    for (uint32_t slot = 0; slot < VSTPOC_AUDIO_RING_FRAMES; ++slot) {
        shared->audio_in[0][slot] = 77.0f;
        shared->audio_in[1][slot] = -77.0f;
    }
    shared->audio_in[0][occupiedSlot] = 55.0f;
    shared->audio_in[1][occupiedSlot] = -55.0f;
    const float rejectedLeft[] = {9.0f, 10.0f, 11.0f};
    const float rejectedRight[] = {-9.0f, -10.0f, -11.0f};
    EXPECT_FALSE(ring.inputWritable(3));
    EXPECT_EQ(ring.pushInput(rejectedLeft, rejectedRight, 3), 0);
    EXPECT_EQ(LoadAcquire(&shared->audio_in_head), nearlyFullHead);
    for (uint32_t slot = 0; slot < VSTPOC_AUDIO_RING_FRAMES; ++slot) {
        const float expectedL = slot == occupiedSlot ? 55.0f : 77.0f;
        const float expectedR = slot == occupiedSlot ? -55.0f : -77.0f;
        EXPECT_FLOAT_EQ(shared->audio_in[0][slot], expectedL);
        EXPECT_FLOAT_EQ(shared->audio_in[1][slot], expectedR);
    }
}

TEST_F(SharedRingFixture, LegacyRawAudioHeadDoesNotExposeOutputWithoutDescriptor) {
    VstpocShared* shared = ring.raw();
    const uint64_t start = VSTPOC_AUDIO_RING_FRAMES - 3u;
    StoreRelaxed(&shared->audio_tail, start);
    StoreRelease(&shared->audio_head, start + 7u);
    StoreRelease(&shared->output_block_tail, 0u);
    StoreRelease(&shared->output_block_head, 0u);

    for (uint32_t i = 0; i < 7; ++i) {
        const uint64_t slot = AudioSlot(start + i);
        shared->audio[0][slot] = 100.0f + static_cast<float>(i);
        shared->audio[1][slot] = -200.0f - static_cast<float>(i);
    }

    float left[7] = {};
    float right[7] = {};
    EXPECT_EQ(ring.pullAudio(left, right, 7), 0);
}

TEST_F(SharedRingFixture, InputSpscStressPreservesEveryFrameAfterSuccessfulPreflight) {
    constexpr uint32_t kFrames = 20000;
    VstpocShared* shared = ring.raw();
    StoreRelaxed(&shared->audio_in_head, 0);
    StoreRelaxed(&shared->audio_in_tail, 0);

    std::vector<uint32_t> consumed;
    consumed.reserve(kFrames);
    std::atomic<bool> producerFailed{false};
    std::atomic<bool> producerDone{false};

    std::thread consumer([&] {
        uint64_t tail = 0;
        uint32_t spins = 0;
        while (consumed.size() < kFrames && spins++ < 5000000u) {
            const uint64_t head = LoadAcquire(&shared->audio_in_head);
            if (head == tail) {
                std::this_thread::yield();
                continue;
            }
            const uint64_t slot = tail & (VSTPOC_AUDIO_RING_FRAMES - 1u);
            const float left = shared->audio_in[0][slot];
            const float right = shared->audio_in[1][slot];
            if (left != -right || left < 0.0f || left >= static_cast<float>(kFrames)) {
                producerFailed.store(true, std::memory_order_relaxed);
                break;
            }
            consumed.push_back(static_cast<uint32_t>(left));
            ++tail;
            StoreRelease(&shared->audio_in_tail, tail);
        }
        if (consumed.size() != kFrames) producerFailed.store(true, std::memory_order_relaxed);
    });

    std::thread producer([&] {
        uint32_t next = 0;
        uint32_t spins = 0;
        while (next < kFrames && spins++ < 5000000u) {
            if (!ring.inputWritable(1)) {
                std::this_thread::yield();
                continue;
            }
            const float left = static_cast<float>(next);
            const float right = -static_cast<float>(next);
            if (ring.pushInput(&left, &right, 1) != 1) {
                producerFailed.store(true, std::memory_order_relaxed);
                break;
            }
            ++next;
        }
        if (next != kFrames) producerFailed.store(true, std::memory_order_relaxed);
        producerDone.store(true, std::memory_order_release);
    });

    producer.join();
    consumer.join();
    EXPECT_TRUE(producerDone.load(std::memory_order_acquire));
    ASSERT_FALSE(producerFailed.load(std::memory_order_relaxed));
    ASSERT_EQ(consumed.size(), kFrames);
    for (uint32_t i = 0; i < kFrames; ++i) EXPECT_EQ(consumed[i], i);
    EXPECT_EQ(LoadAcquire(&shared->audio_in_head), kFrames);
    EXPECT_EQ(LoadAcquire(&shared->audio_in_tail), kFrames);
}

}  // namespace
