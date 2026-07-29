#include <gtest/gtest.h>

#include "ipc/SharedRing.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

static_assert(sizeof(VstpocTransportBlock) == 48);
static_assert(VSTPOC_TRANSPORT_QUEUE_CAPACITY == 1024u);
static_assert(offsetof(VstpocShared, state_response_seq) < offsetof(VstpocShared, shared_layout_magic));
static_assert(offsetof(VstpocShared, state_command) < offsetof(VstpocShared, shared_layout_magic));
static_assert(offsetof(VstpocShared, state_path) < offsetof(VstpocShared, shared_layout_magic));
static_assert(offsetof(VstpocShared, state_message) < offsetof(VstpocShared, shared_layout_magic));
static_assert(offsetof(VstpocShared, shared_layout_magic) == 269976);
static_assert(offsetof(VstpocShared, shared_layout_version) == 269984);
static_assert(offsetof(VstpocShared, shared_layout_size) == 269988);
static_assert(offsetof(VstpocShared, transport_queue_head) == 270400);
static_assert(offsetof(VstpocShared, transport_queue_tail) == 270464);
static_assert(offsetof(VstpocShared, transport_queue) == 270528);
static_assert(offsetof(VstpocShared, transport_queue_dropped) == 319680);
static_assert(sizeof(VstpocShared) == 319744);

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
    TempBackingFile backing;
    SharedRing ring{backing.path()};
};

void StoreRelaxed(uint64_t* value, uint64_t next) {
    __atomic_store_n(value, next, __ATOMIC_RELAXED);
}

void StoreRelease(uint64_t* value, uint64_t next) {
    __atomic_store_n(value, next, __ATOMIC_RELEASE);
}

uint64_t LoadAcquire(const uint64_t* value) {
    return __atomic_load_n(value, __ATOMIC_ACQUIRE);
}

TEST(VstSharedLayoutTest, MetadataAndV3AppendOrderingAreStable) {
    TempBackingFile backing;
    SharedRing ring(backing.path());

    ASSERT_TRUE(ring.valid());
    const VstpocShared* shared = ring.raw();
    EXPECT_EQ(shared->shared_layout_magic, VSTPOC_SHARED_LAYOUT_MAGIC);
    EXPECT_EQ(shared->shared_layout_version, VSTPOC_SHARED_LAYOUT_VERSION);
    EXPECT_EQ(shared->shared_layout_size, sizeof(VstpocShared));

    EXPECT_LT(offsetof(VstpocShared, state_message),
              offsetof(VstpocShared, shared_layout_magic));
    EXPECT_LT(offsetof(VstpocShared, shared_layout_magic),
              offsetof(VstpocShared, transport_seq));
    EXPECT_LT(offsetof(VstpocShared, transport_seq),
              offsetof(VstpocShared, transport_queue_head));
    EXPECT_LT(offsetof(VstpocShared, transport_queue_head),
              offsetof(VstpocShared, transport_queue_tail));
    EXPECT_LT(offsetof(VstpocShared, transport_queue_tail),
              offsetof(VstpocShared, transport_queue));
    EXPECT_LT(offsetof(VstpocShared, transport_queue),
              offsetof(VstpocShared, transport_queue_dropped));

    EXPECT_EQ(LoadAcquire(&shared->transport_queue_head), 0u);
    EXPECT_EQ(LoadAcquire(&shared->transport_queue_tail), 0u);
    EXPECT_EQ(LoadAcquire(&shared->transport_queue_dropped), 0u);
}

TEST_F(SharedRingFixture, TransportRecordPreservesFieldsAndFIFOOrder) {
    constexpr uint32_t kRecords = 6;
    for (uint32_t i = 0; i < kRecords; ++i) {
        ASSERT_TRUE(ring.publishTransport(
            1000 + i, 2000 + i, 3000 + i, 48000.0 + i, 120.0 + i,
            (i & 1u) != 0, (i & 2u) != 0, 64 + i));
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

TEST_F(SharedRingFixture, TransportQueueWrapsWithoutReordering) {
    VstpocShared* shared = ring.raw();
    const uint64_t start = VSTPOC_TRANSPORT_QUEUE_CAPACITY - 2u;
    StoreRelaxed(&shared->transport_queue_tail, start);
    StoreRelaxed(&shared->transport_queue_head, start);

    ASSERT_TRUE(ring.publishTransport(11, 21, 31, 44100.0, 90.0, true, false, 3));
    ASSERT_TRUE(ring.publishTransport(12, 22, 32, 44100.0, 91.0, false, true, 4));
    ASSERT_TRUE(ring.publishTransport(13, 23, 33, 44100.0, 92.0, true, true, 5));
    ASSERT_TRUE(ring.publishTransport(14, 24, 34, 44100.0, 93.0, false, false, 6));

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

    EXPECT_FALSE(ring.publishTransport(1, 2, 3, 4.0, 5.0, true, true, 6));
    EXPECT_EQ(LoadAcquire(&shared->transport_queue_head), head);
    EXPECT_EQ(LoadAcquire(&shared->transport_queue_tail), tail);
    EXPECT_EQ(LoadAcquire(&shared->transport_queue_dropped), 1u);
    EXPECT_EQ(occupied.sample_position, 777u);
    EXPECT_EQ(occupied.transport_frame, 888u);
    EXPECT_EQ(occupied.loop_end_frame, 999u);
    EXPECT_DOUBLE_EQ(occupied.sample_rate, 96000.0);
}

TEST_F(SharedRingFixture, InputPreflightAndPushAreAllOrNothingNearWrap) {
    VstpocShared* shared = ring.raw();
    const uint64_t start = VSTPOC_AUDIO_RING_FRAMES - 2u;
    StoreRelaxed(&shared->audio_in_tail, start);
    StoreRelaxed(&shared->audio_in_head, start);
    EXPECT_TRUE(ring.inputWritable(VSTPOC_AUDIO_RING_FRAMES));
    EXPECT_FALSE(ring.inputWritable(VSTPOC_AUDIO_RING_FRAMES + 1u));

    const float wrappedFrames[] = {
        1.0f, -1.0f,
        2.0f, -2.0f,
        3.0f, -3.0f,
        4.0f, -4.0f,
    };
    ASSERT_TRUE(ring.inputWritable(4));
    ASSERT_EQ(ring.pushInput(wrappedFrames, 4), 4);
    EXPECT_EQ(LoadAcquire(&shared->audio_in_head), start + 4u);
    for (uint32_t i = 0; i < 4; ++i) {
        const uint64_t slot = (start + i) & (VSTPOC_AUDIO_RING_FRAMES - 1u);
        EXPECT_FLOAT_EQ(shared->audio_in[slot * VSTPOC_CHANNELS], wrappedFrames[i * 2]);
        EXPECT_FLOAT_EQ(shared->audio_in[slot * VSTPOC_CHANNELS + 1], wrappedFrames[i * 2 + 1]);
    }

    const uint64_t nearlyFullHead = start + 4u + VSTPOC_AUDIO_RING_FRAMES - 2u;
    StoreRelaxed(&shared->audio_in_head, nearlyFullHead);
    StoreRelease(&shared->audio_in_tail, start + 4u);
    const uint64_t occupiedSlot = nearlyFullHead & (VSTPOC_AUDIO_RING_FRAMES - 1u);
    shared->audio_in[occupiedSlot * VSTPOC_CHANNELS] = 55.0f;
    shared->audio_in[occupiedSlot * VSTPOC_CHANNELS + 1] = -55.0f;
    const float rejectedFrames[] = {9.0f, -9.0f, 10.0f, -10.0f, 11.0f, -11.0f};

    EXPECT_FALSE(ring.inputWritable(3));
    EXPECT_EQ(ring.pushInput(rejectedFrames, 3), 0);
    EXPECT_EQ(LoadAcquire(&shared->audio_in_head), nearlyFullHead);
    EXPECT_FLOAT_EQ(shared->audio_in[occupiedSlot * VSTPOC_CHANNELS], 55.0f);
    EXPECT_FLOAT_EQ(shared->audio_in[occupiedSlot * VSTPOC_CHANNELS + 1], -55.0f);
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
            const float left = shared->audio_in[slot * VSTPOC_CHANNELS];
            const float right = shared->audio_in[slot * VSTPOC_CHANNELS + 1];
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
            const float frame[] = {static_cast<float>(next), -static_cast<float>(next)};
            if (ring.pushInput(frame, 1) != 1) {
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
