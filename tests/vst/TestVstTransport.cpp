#include <gtest/gtest.h>

#include "ipc/SharedRing.h"
#include "vst/WineAudioBlockAdapter.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <unistd.h>

using guitarrackcraft::MidiBuffer;

namespace {

static_assert(VSTPOC_SHARED_LAYOUT_VERSION == 10u);
static_assert(VSTPOC_SHARED_LAYOUT_MAGIC == UINT64_C(0x565354504f433130));
static_assert(sizeof(VstpocMidiEvent) == 16u);
static_assert(offsetof(VstpocOutputBlock, midi_events) > offsetof(VstpocOutputBlock, midi_payload_end));
static_assert(offsetof(VstpocTransportBlock, midi_events) > offsetof(VstpocTransportBlock, midi_payload_end));
static_assert(VSTPOC_MIDI_PAYLOAD_RING_BYTES == 1024u * 1024u);
static_assert(VSTPOC_MAX_MIDI_EVENTS_PER_BLOCK == 128u);

class TempBackingFile {
public:
    TempBackingFile() {
        char pattern[] = "/tmp/vst_transport_v10_XXXXXX";
        const int fd = ::mkstemp(pattern);
        EXPECT_NE(fd, -1);
        if (fd >= 0) { ::close(fd); path_ = pattern; }
    }
    ~TempBackingFile() { if (!path_.empty()) ::unlink(path_.c_str()); }
    const std::string& path() const { return path_; }
private:
    std::string path_;
};

class SharedRingFixture : public ::testing::Test {
protected:
    TempBackingFile backing;
    SharedRing ring{backing.path()};
};

void store(uint64_t* p, uint64_t value, int order = __ATOMIC_RELEASE) {
    __atomic_store_n(p, value, order);
}
uint64_t load(const uint64_t* p, int order = __ATOMIC_ACQUIRE) {
    return __atomic_load_n(p, order);
}

MidiBuffer shortMidi(uint32_t frame, uint8_t note) {
    MidiBuffer midi;
    const uint8_t payload[] = {0x90, note, 100};
    EXPECT_TRUE(midi.append(frame, payload, sizeof(payload)));
    return midi;
}

MidiBuffer exactSysex(uint32_t frame, uint8_t seed = 1) {
    MidiBuffer midi;
    std::vector<uint8_t> bytes(guitarrackcraft::kMaxMidiPayloadBytes);
    bytes.front() = 0xf0;
    for (uint32_t i = 1; i + 1 < bytes.size(); ++i)
        bytes[i] = static_cast<uint8_t>((seed + i * 29u) % 127u);
    bytes.back() = 0xf7;
    EXPECT_TRUE(midi.append(frame, bytes.data(), bytes.size()));
    return midi;
}

void writePayload(VstpocShared* shared, uint64_t absolute,
                  const uint8_t* bytes, uint32_t size) {
    const uint32_t slot = static_cast<uint32_t>(absolute & (VSTPOC_MIDI_PAYLOAD_RING_BYTES - 1u));
    const uint32_t first = std::min<uint32_t>(size, VSTPOC_MIDI_PAYLOAD_RING_BYTES - slot);
    std::memcpy(shared->midi_output_payload + slot, bytes, first);
    if (size > first) std::memcpy(shared->midi_output_payload, bytes + first, size - first);
}

bool stageOutput(VstpocShared* shared, uint32_t frames, float marker,
                 const MidiBuffer& midi, uint32_t flags = 0) {
    const uint64_t blockHead = load(&shared->output_block_head, __ATOMIC_RELAXED);
    const uint64_t blockTail = load(&shared->output_block_tail, __ATOMIC_ACQUIRE);
    if (blockHead - blockTail >= VSTPOC_OUTPUT_BLOCK_CAPACITY) {
        __atomic_add_fetch(&shared->output_drop_count, 1u, __ATOMIC_RELAXED);
        return false;
    }
    const uint64_t audioHead = load(&shared->audio_head, __ATOMIC_RELAXED);
    const uint64_t audioTail = load(&shared->audio_tail, __ATOMIC_ACQUIRE);
    if (audioHead - audioTail + frames > VSTPOC_AUDIO_RING_FRAMES) return false;
    const uint64_t payloadBegin = load(&shared->midi_output_payload_head, __ATOMIC_RELAXED);
    const uint64_t payloadTail = load(&shared->midi_output_payload_tail, __ATOMIC_ACQUIRE);
    if (payloadBegin - payloadTail + midi.payloadBytes() > VSTPOC_MIDI_PAYLOAD_RING_BYTES) return false;
    const uint64_t blockIndex = blockHead & (VSTPOC_OUTPUT_BLOCK_CAPACITY - 1u);
    VstpocOutputBlock& block = shared->output_blocks[blockIndex];
    const uint32_t offset = static_cast<uint32_t>(audioHead & (VSTPOC_AUDIO_RING_FRAMES - 1u));
    for (uint32_t i = 0; i < frames; ++i) {
        const uint32_t slot = (offset + i) & (VSTPOC_AUDIO_RING_FRAMES - 1u);
        shared->audio[0][slot] = marker + static_cast<float>(i);
        shared->audio[1][slot] = -marker - static_cast<float>(i);
    }
    block.frame_count = frames;
    block.ring_offset = offset;
    block.midi_event_count = 0;
    block.midi_flags = flags;
    block.midi_payload_begin = payloadBegin;
    uint64_t payloadEnd = payloadBegin;
    for (uint32_t i = 0; i < midi.eventCount() && i < VSTPOC_MAX_MIDI_EVENTS_PER_BLOCK; ++i) {
        const auto& event = midi.eventAt(i);
        writePayload(shared, payloadEnd, midi.payloadFor(event), event.payloadSize);
        auto& desc = block.midi_events[block.midi_event_count++];
        desc.frame_offset = event.frameOffset;
        desc.payload_size = event.payloadSize;
        desc.payload_offset = payloadEnd;
        payloadEnd += event.payloadSize;
    }
    block.midi_payload_end = payloadEnd;
    store(&shared->midi_output_payload_head, payloadEnd);
    store(&block.sequence, blockHead + 1u);
    store(&shared->audio_head, audioHead + frames);
    store(&shared->output_block_head, blockHead + 1u);
    return true;
}

std::vector<uint8_t> readRing(const uint8_t* bytes, uint64_t absolute,
                              uint32_t size) {
    std::vector<uint8_t> result(size);
    const uint32_t slot = static_cast<uint32_t>(
        absolute & (VSTPOC_MIDI_PAYLOAD_RING_BYTES - 1u));
    const uint32_t first = std::min<uint32_t>(
        size, VSTPOC_MIDI_PAYLOAD_RING_BYTES - slot);
    std::memcpy(result.data(), bytes + slot, first);
    if (size > first) std::memcpy(result.data() + first, bytes, size - first);
    return result;
}

std::vector<uint8_t> readPayload(const VstpocShared* shared, uint64_t absolute,
                                 uint32_t size) {
    return readRing(shared->midi_output_payload, absolute, size);
}

std::vector<uint8_t> readInputPayload(const VstpocShared* shared,
                                      uint64_t absolute, uint32_t size) {
    return readRing(shared->midi_input_payload, absolute, size);
}
TEST_F(SharedRingFixture, PublishesV10MagicFeaturesAndPayloadRings) {
    ASSERT_TRUE(ring.valid());
    const VstpocShared* shared = ring.raw();
    EXPECT_EQ(load(&shared->shared_layout_magic), VSTPOC_SHARED_LAYOUT_MAGIC);
    EXPECT_EQ(shared->shared_layout_version, 10u);
    EXPECT_NE(shared->shared_feature_bits & VSTPOC_FEATURE_MIDI_PAYLOAD_RING, 0u);
    EXPECT_EQ(shared->shared_layout_size, VSTPOC_SHARED_LAYOUT_V10_SIZE);
    EXPECT_EQ(VSTPOC_SHARED_LAYOUT_V10_SIZE, sizeof(VstpocShared));
}

TEST_F(SharedRingFixture, TransportPayloadRingWrapsAndPublishesBeforeQueueHead) {
    VstpocShared* shared = ring.raw();
    const uint64_t start = VSTPOC_MIDI_PAYLOAD_RING_BYTES - 4u;
    store(&shared->midi_input_payload_head, start, __ATOMIC_RELAXED);
    store(&shared->midi_input_payload_tail, start, __ATOMIC_RELEASE);
    MidiBuffer midi = shortMidi(7, 60);
    const uint8_t sysex[] = {0xf0, 1, 2, 3, 4, 0xf7};
    ASSERT_TRUE(midi.append(19, sysex, sizeof(sysex)));
    ASSERT_TRUE(ring.publishTransport(1, 2, 0, 48000.0, 120.0, true, false, 64, midi));
    const auto& block = shared->transport_queue[0];
    EXPECT_EQ(block.midi_payload_begin, start);
    EXPECT_EQ(block.midi_payload_end, start + midi.payloadBytes());
    EXPECT_EQ(block.midi_event_count, 2u);
    EXPECT_EQ(readInputPayload(shared, start, 3), (std::vector<uint8_t>{0x90, 60, 100}));
    EXPECT_EQ(readInputPayload(shared, start + 3, 6), (std::vector<uint8_t>{0xf0, 1, 2, 3, 4, 0xf7}));
}

TEST_F(SharedRingFixture,
       Vst3TransportQueuePopReleasesEverySlotAcrossTwoWraps) {
    VstpocShared* shared = ring.raw();
    constexpr uint64_t blocks =
        2u * VSTPOC_TRANSPORT_QUEUE_CAPACITY + 17u;
    MidiBuffer empty;

    for (uint64_t i = 0; i < blocks; ++i) {
        ASSERT_TRUE(ring.publishTransport(
            i * 64u, i * 64u + 11u, 0, 48000.0, 120.0, true, false,
            64, empty)) << "producer rejected block " << i;

        VstpocTransportBlock consumed{};
        ASSERT_TRUE(vstpoc_transport_queue_pop(shared, &consumed))
            << "consumer missed block " << i;
        EXPECT_EQ(consumed.sample_position, i * 64u);
        EXPECT_EQ(consumed.transport_frame, i * 64u + 11u);
        EXPECT_EQ(consumed.block_frames, 64u);
        EXPECT_EQ(load(&shared->transport_queue_tail), i + 1u);
        EXPECT_EQ(load(&shared->transport_queue_head), i + 1u);
        EXPECT_FALSE(vstpoc_transport_queue_pop(shared, &consumed));
    }
    EXPECT_EQ(load(&shared->transport_queue_dropped), 0u);
}

TEST_F(SharedRingFixture, OutputPublicationOrdersPayloadAndDescriptorBeforeHead) {
    VstpocShared* shared = ring.raw();
    MidiBuffer midi = exactSysex(11, 9);
    ASSERT_TRUE(stageOutput(shared, 32, 4.0f, midi, VSTPOC_MIDI_FLAG_AUTHORITATIVE));
    const auto& block = shared->output_blocks[0];
    EXPECT_EQ(block.midi_event_count, 1u);
    EXPECT_EQ(block.midi_events[0].payload_size, guitarrackcraft::kMaxMidiPayloadBytes);
    EXPECT_EQ(load(&shared->midi_output_payload_head), guitarrackcraft::kMaxMidiPayloadBytes);
    EXPECT_EQ(load(&shared->output_block_head), 1u);
    EXPECT_EQ(block.sequence, 1u);
    EXPECT_EQ(readPayload(shared, block.midi_events[0].payload_offset,
                          block.midi_events[0].payload_size).front(), 0xf0);
}

TEST_F(SharedRingFixture, OutputPayloadRingWrapsAcrossAdjacentDescriptorBytes) {
    VstpocShared* shared = ring.raw();
    const uint64_t start = VSTPOC_MIDI_PAYLOAD_RING_BYTES - 2u;
    store(&shared->midi_output_payload_head, start, __ATOMIC_RELAXED);
    store(&shared->midi_output_payload_tail, start, __ATOMIC_RELEASE);
    MidiBuffer midi = shortMidi(3, 66);
    ASSERT_TRUE(stageOutput(shared, 4, 14.0f, midi));
    const auto& block = shared->output_blocks[0];
    EXPECT_EQ(block.midi_payload_begin, start);
    EXPECT_EQ(block.midi_events[0].payload_offset, start);
    std::array<float, 4> left{};
    std::array<float, 4> right{};
    MidiBuffer output;
    bool authoritative = false;
    ASSERT_EQ(ring.pullOutput(left.data(), right.data(), 4, output, true,
                               authoritative), 4);
    ASSERT_EQ(output.eventCount(), 1u);
    const auto& event = output.eventAt(0);
    EXPECT_EQ(event.frameOffset, 3u);
    EXPECT_EQ(output.payloadFor(event)[0], 0x90u);
    EXPECT_EQ(output.payloadFor(event)[1], 66u);
}

TEST_F(SharedRingFixture, PullOutputReturnsAdjacentBlocksAndEachMidiExactlyOnce) {
    VstpocShared* shared = ring.raw();
    ASSERT_TRUE(stageOutput(shared, 4, 10.0f, shortMidi(1, 60)));
    ASSERT_TRUE(stageOutput(shared, 4, 20.0f, shortMidi(2, 61)));
    std::array<float, 4> left{};
    std::array<float, 4> right{};
    MidiBuffer midi;
    bool authoritative = false;
    ASSERT_EQ(ring.pullOutput(left.data(), right.data(), 4, midi, true, authoritative), 4);
    ASSERT_EQ(midi.eventCount(), 1u);
    EXPECT_EQ(midi.payloadFor(midi.eventAt(0))[1], 60u);
    EXPECT_FALSE(authoritative);
    midi.clear();
    ASSERT_EQ(ring.pullOutput(left.data(), right.data(), 4, midi, true, authoritative), 4);
    ASSERT_EQ(midi.eventCount(), 1u);
    EXPECT_EQ(midi.payloadFor(midi.eventAt(0))[1], 61u);
    EXPECT_EQ(load(&shared->output_block_tail), 2u);
    midi.clear();
    EXPECT_EQ(ring.pullOutput(left.data(), right.data(), 4, midi, true, authoritative), 0);
    EXPECT_EQ(midi.eventCount(), 0u);
}
TEST_F(SharedRingFixture, PullOutputAdvancesSeededAbsoluteAudioCursor) {
    VstpocShared* shared = ring.raw();
    constexpr uint64_t start = 32;
    constexpr uint32_t frames = 12;
    store(&shared->audio_head, start, __ATOMIC_RELAXED);
    store(&shared->audio_tail, start, __ATOMIC_RELEASE);
    MidiBuffer empty;
    ASSERT_TRUE(stageOutput(shared, frames, 100.0f, empty));
    ASSERT_TRUE(stageOutput(shared, frames, 200.0f, empty));
    ASSERT_TRUE(stageOutput(shared, frames, 300.0f, empty));

    std::array<float, frames> left{};
    std::array<float, frames> right{};
    MidiBuffer midi;
    bool authoritative = false;
    for (const float marker : {100.0f, 200.0f, 300.0f}) {
        ASSERT_EQ(ring.pullOutput(left.data(), right.data(), frames, midi, true,
                                  authoritative), frames);
        EXPECT_FLOAT_EQ(left.front(), marker);
        EXPECT_FLOAT_EQ(left.back(), marker + 11.0f);
        EXPECT_FLOAT_EQ(right.front(), -marker);
        EXPECT_EQ(load(&shared->audio_tail),
                  start + static_cast<uint64_t>(
                              (marker - 100.0f) / 100.0f + 1.0f) * frames);
    }
    EXPECT_EQ(load(&shared->output_block_tail), 3u);
    EXPECT_EQ(load(&shared->audio_tail), start + 3u * frames);
}


TEST_F(SharedRingFixture, PullOutputReportsAuthoritativeFlagWithExactSysEx) {
    VstpocShared* shared = ring.raw();
    MidiBuffer midiIn = exactSysex(7, 17);
    ASSERT_TRUE(stageOutput(shared, 8, 30.0f, midiIn, VSTPOC_MIDI_FLAG_AUTHORITATIVE));
    std::array<float, 8> left{};
    std::array<float, 8> right{};
    MidiBuffer midiOut;
    bool authoritative = false;
    ASSERT_EQ(ring.pullOutput(left.data(), right.data(), 8, midiOut, true, authoritative), 8);
    ASSERT_TRUE(authoritative);
    ASSERT_EQ(midiOut.eventCount(), 1u);
    const auto& event = midiOut.eventAt(0);
    ASSERT_EQ(event.payloadSize, guitarrackcraft::kMaxMidiPayloadBytes);
    EXPECT_EQ(midiOut.payloadFor(event)[0], 0xf0);
    EXPECT_EQ(midiOut.payloadFor(event)[event.payloadSize - 1], 0xf7);
}

TEST_F(SharedRingFixture, PullOutputDropsMismatchedAudioAndMidiTogether) {
    VstpocShared* shared = ring.raw();
    ASSERT_TRUE(stageOutput(shared, 7, 40.0f, shortMidi(2, 70)));
    ASSERT_TRUE(stageOutput(shared, 4, 50.0f, shortMidi(3, 71)));
    std::array<float, 4> left{};
    std::array<float, 4> right{};
    MidiBuffer midi;
    bool authoritative = false;
    EXPECT_EQ(ring.pullOutput(left.data(), right.data(), 4, midi, true, authoritative), 4);
    EXPECT_FALSE(authoritative);
    ASSERT_EQ(midi.eventCount(), 1u);
    EXPECT_EQ(midi.payloadFor(midi.eventAt(0))[1], 71u);
    EXPECT_EQ(load(&shared->output_block_tail), 2u);
    EXPECT_EQ(load(&shared->audio_tail), 11u);
    EXPECT_EQ(load(&shared->midi_output_payload_tail), load(&shared->midi_output_payload_head));
}

TEST_F(SharedRingFixture, PullOutputRejectsUnpublishedDescriptorWithoutExposure) {
    VstpocShared* shared = ring.raw();
    const uint8_t note[] = {0x90, 60, 100};
    store(&shared->midi_output_payload_head, sizeof(note), __ATOMIC_RELAXED);
    writePayload(shared, 0, note, sizeof(note));
    auto& block = shared->output_blocks[0];
    block.frame_count = 4;
    block.ring_offset = 0;
    block.midi_event_count = 1;
    block.midi_payload_begin = 0;
    block.midi_payload_end = sizeof(note);
    block.midi_events[0] = {0, sizeof(note), 0};
    store(&shared->output_block_head, 1u);
    store(&block.sequence, 0u, __ATOMIC_RELAXED);
    std::array<float, 4> left{};
    std::array<float, 4> right{};
    MidiBuffer midi;
    bool authoritative = true;
    EXPECT_EQ(ring.pullOutput(left.data(), right.data(), 4, midi, true, authoritative), 0);
    EXPECT_EQ(midi.eventCount(), 0u);
    EXPECT_EQ(load(&shared->output_block_tail), 0u);
    EXPECT_EQ(load(&shared->audio_tail), 0u);
    EXPECT_FALSE(authoritative);
}
TEST_F(SharedRingFixture, RejectedDescriptorDoesNotDiscardFollowingValidAudio) {
    VstpocShared* shared = ring.raw();
    MidiBuffer empty;
    ASSERT_TRUE(stageOutput(shared, 4, 10.0f, empty));
    auto& rejected = shared->output_blocks[0];
    store(&rejected.sequence, 0u, __ATOMIC_RELAXED);
    ASSERT_TRUE(stageOutput(shared, 4, 20.0f, empty));

    std::array<float, 4> left{};
    std::array<float, 4> right{};
    MidiBuffer midi;
    bool authoritative = true;
    EXPECT_EQ(ring.pullOutput(left.data(), right.data(), 4, midi, true,
                              authoritative), 0);
    EXPECT_EQ(load(&shared->output_block_tail), 0u);
    EXPECT_EQ(load(&shared->audio_tail), 0u);

    store(&rejected.sequence, 1u, __ATOMIC_RELEASE);
    ASSERT_EQ(ring.pullOutput(left.data(), right.data(), 4, midi, true,
                              authoritative), 4);
    EXPECT_EQ(left[0], 10.0f);
    ASSERT_EQ(ring.pullOutput(left.data(), right.data(), 4, midi, true,
                              authoritative), 4);
    EXPECT_EQ(left[0], 20.0f);
    EXPECT_EQ(load(&shared->output_block_tail), 2u);
    EXPECT_EQ(load(&shared->audio_tail), 8u);
    EXPECT_EQ(midi.eventCount(), 0u);
    EXPECT_FALSE(authoritative);
}


TEST_F(SharedRingFixture, OutputBlockRingRejectsFullState) {
    VstpocShared* shared = ring.raw();
    MidiBuffer empty;
    for (uint32_t i = 0; i < VSTPOC_OUTPUT_BLOCK_CAPACITY; ++i)
        ASSERT_TRUE(stageOutput(shared, 1, static_cast<float>(i), empty));
    EXPECT_FALSE(stageOutput(shared, 1, 99.0f, empty));
    EXPECT_EQ(load(&shared->output_drop_count), 1u);
}

TEST_F(SharedRingFixture, InputPayloadRingAcceptsExactMaximumAndRejectsOversizeAtBufferBoundary) {
    VstpocShared* shared = ring.raw();
    MidiBuffer exact = exactSysex(0);
    ASSERT_TRUE(ring.publishTransport(0, 0, 0, 48000.0, 120.0, true, false, 64, exact));
    EXPECT_EQ(shared->transport_queue[0].midi_events[0].payload_size,
              guitarrackcraft::kMaxMidiPayloadBytes);
    EXPECT_EQ(load(&shared->midi_input_drop_count), 0u);
    MidiBuffer invalid;
    std::vector<uint8_t> tooLarge(guitarrackcraft::kMaxMidiPayloadBytes + 1u);
    EXPECT_FALSE(invalid.append(0, tooLarge.data(), tooLarge.size()));
    EXPECT_TRUE(ring.publishTransport(0, 0, 0, 48000.0, 120.0, true,
                                      false, 64, invalid));
}

TEST_F(SharedRingFixture, PublishAndPullFailuresRenderSilenceThroughFailClosedSeam) {
    VstpocShared* shared = ring.raw();
    MidiBuffer empty;
    for (uint32_t i = 0; i < VSTPOC_TRANSPORT_QUEUE_CAPACITY; ++i) {
        ASSERT_TRUE(ring.publishTransport(i, i, 0, 48000.0, 120.0, true,
                                          false, 64, empty));
    }
    ASSERT_FALSE(ring.publishTransport(0, 0, 0, 48000.0, 120.0, true,
                                       false, 64, empty));

    vsthost::WineFailClosedAudioState failClosed;
    failClosed.reset();
    std::array<float, 4> left{};
    std::array<float, 4> right{};
    left.fill(-10.0f);
    right.fill(-20.0f);
    float* outputs[] = {left.data(), right.data()};
    failClosed.renderFailure(outputs, 4);
    for (uint32_t i = 0; i < 4; ++i) {
        EXPECT_FLOAT_EQ(left[i], 0.0f);
        EXPECT_FLOAT_EQ(right[i], 0.0f);
    }

    ASSERT_TRUE(stageOutput(shared, 4, 100.0f, empty));
    store(&shared->output_blocks[0].sequence, 0u, __ATOMIC_RELAXED);
    left.fill(-10.0f);
    right.fill(-20.0f);
    bool authoritative = true;
    EXPECT_EQ(ring.pullOutput(left.data(), right.data(), 4, empty, true,
                              authoritative), 0);
    failClosed.reset();
    failClosed.renderFailure(outputs, 4);
    for (uint32_t i = 0; i < 4; ++i) {
        EXPECT_FLOAT_EQ(left[i], 0.0f);
        EXPECT_FLOAT_EQ(right[i], 0.0f);
    }
}


TEST_F(SharedRingFixture,
       WineAudioProcessFailsClosedBeforePrimingAndRecoversProcessedOutput) {
    constexpr uint32_t graphFrames = 64;
    constexpr uint32_t activationFrames = graphFrames * 8;
    constexpr uint32_t guestFrames = 128;
    using Adapter = vsthost::WineAudioBlockAdapter;
    using FailClosed = vsthost::WineFailClosedAudioState;
    using Disposition = guitarrackcraft::MidiOutputDisposition;

    Adapter adapter(activationFrames);
    ASSERT_TRUE(adapter.valid());
    ASSERT_EQ(adapter.guestFrames(), activationFrames);
    ASSERT_EQ(adapter.outputSlotCapacity(), 3u);

    VstpocShared* shared = ring.raw();
    MidiBuffer emptyMidi;
    ASSERT_TRUE(stageOutput(shared, guestFrames, 100.0f, emptyMidi));
    ASSERT_TRUE(stageOutput(shared, guestFrames, 200.0f, emptyMidi));
    ASSERT_TRUE(stageOutput(shared, guestFrames, 300.0f, emptyMidi));

    struct CallbackResult {
        std::array<float, graphFrames> left{};
        std::array<float, graphFrames> right{};
        Disposition disposition = Disposition::Passthrough;
    };

    bool geometryEstablished = false;
    bool outputPrimed = false;
    FailClosed failClosed;
    failClosed.reset();

    const auto publishInput = [&]() {
        if (!adapter.inputReady()) return true;
        const auto& inputContext = adapter.inputContext();
        const bool published =
            ring.inputWritable(adapter.guestFrames()) &&
            ring.publishTransport(
                inputContext.samplePosition, inputContext.transportFrame,
                inputContext.loopEndFrame, inputContext.sampleRate,
                inputContext.beatsPerMinute, inputContext.playing,
                inputContext.looping, adapter.guestFrames(), adapter.inputMidi()) &&
            ring.pushInput(adapter.inputLeft(), adapter.inputRight(),
                           static_cast<int32_t>(adapter.guestFrames())) ==
                static_cast<int32_t>(adapter.guestFrames());
        if (published) adapter.consumeInput();
        return published;
    };

    const auto callback = [&](uint32_t callbackFrames, uint64_t samplePosition) {
        CallbackResult result;
        std::array<float, graphFrames> inputLeft{};
        std::array<float, graphFrames> inputRight{};
        for (uint32_t i = 0; i < graphFrames; ++i) {
            inputLeft[i] = -10.0f;
            inputRight[i] = -20.0f;
            result.left[i] = inputLeft[i];
            result.right[i] = inputRight[i];
        }
        float* outputs[] = {result.left.data(), result.right.data()};

        const auto fail = [&]() {
            failClosed.renderFailure(outputs, callbackFrames);
            return result;
        };
        if (!geometryEstablished) {
            if (!Adapter::acceptsCallbackFrames(activationFrames, callbackFrames))
                return fail();
            geometryEstablished = adapter.configure(callbackFrames);
            if (!geometryEstablished) return fail();
        }
        if (callbackFrames != graphFrames) return fail();

        guitarrackcraft::AudioProcessContext context;
        context.samplePosition = samplePosition;
        context.transportFrame = samplePosition + 17;
        context.sampleRate = 48000.0;
        context.playing = true;
        const std::array<const float*, 2> inputs{
            inputLeft.data(), inputRight.data()};
        if (adapter.inputReady()) EXPECT_TRUE(publishInput());
        if (!adapter.appendInput(inputs.data(), context, emptyMidi))
            return fail();
        if (adapter.inputReady()) EXPECT_TRUE(publishInput());

        bool authoritative = false;
        if (adapter.outputWriteAvailable()) {
            const int32_t pulled = ring.pullOutput(
                adapter.outputLeft(), adapter.outputRight(),
                static_cast<int32_t>(adapter.guestFrames()),
                adapter.outputMidi(), true, authoritative);
            if (pulled == static_cast<int32_t>(adapter.guestFrames()))
                EXPECT_TRUE(adapter.commitOutput(adapter.outputMidi(), authoritative));
        }

        if (!outputPrimed) {
            if (adapter.outputBlockCount() < adapter.outputSlotCapacity())
                return fail();
            outputPrimed = true;
        } else if (!adapter.outputReady()) {
            outputPrimed = false;
            return fail();
        }

        result.disposition = authoritative ? Disposition::Replace
                                           : Disposition::Passthrough;
        adapter.copyOutput(result.left.data(), result.right.data(), emptyMidi);
        if (adapter.outputBlockCount() == 0) outputPrimed = false;
        failClosed.applyRecovery(outputs, callbackFrames);
        failClosed.rememberProcessed(
            result.left.data(), result.right.data(), callbackFrames);
        return result;
    };

    const CallbackResult first = callback(graphFrames, 0);
    const CallbackResult second = callback(graphFrames, graphFrames);
    EXPECT_FLOAT_EQ(first.left[0], 0.0f);
    EXPECT_FLOAT_EQ(second.left[0], 0.0f);
    const CallbackResult primed = callback(graphFrames, 2 * graphFrames);
    EXPECT_FLOAT_EQ(primed.left[0], 100.0f / 64.0f);
    EXPECT_FLOAT_EQ(primed.left[63], 163.0f);
    ASSERT_TRUE(geometryEstablished);
    ASSERT_EQ(adapter.graphFrames(), graphFrames);
    ASSERT_EQ(adapter.guestFrames(), guestFrames);
    EXPECT_FALSE(adapter.outputWriteAvailable());
    EXPECT_EQ(primed.disposition, Disposition::Passthrough);
    const CallbackResult blockOneTail = callback(graphFrames, 3 * graphFrames);
    const CallbackResult blockTwoHead = callback(graphFrames, 4 * graphFrames);
    const CallbackResult blockTwoTail = callback(graphFrames, 5 * graphFrames);
    const CallbackResult blockThreeHead = callback(graphFrames, 6 * graphFrames);
    const CallbackResult blockThreeTail = callback(graphFrames, 7 * graphFrames);
    EXPECT_FLOAT_EQ(blockOneTail.left[63], 227.0f);
    EXPECT_FLOAT_EQ(blockTwoHead.left[0], 200.0f);
    EXPECT_FLOAT_EQ(blockTwoTail.left[63], 327.0f);
    EXPECT_FLOAT_EQ(blockThreeHead.left[0], 300.0f);
    EXPECT_FLOAT_EQ(blockThreeTail.left[63], 427.0f);
    EXPECT_FALSE(adapter.outputReady());

    const CallbackResult miss = callback(graphFrames, 8 * graphFrames);
    EXPECT_FLOAT_EQ(miss.left[0], 427.0f * 63.0f / 64.0f);
    EXPECT_NE(miss.left[0], -10.0f);

    ASSERT_TRUE(stageOutput(shared, guestFrames, 400.0f, emptyMidi));
    ASSERT_TRUE(stageOutput(shared, guestFrames, 500.0f, emptyMidi));
    ASSERT_TRUE(stageOutput(shared, guestFrames, 600.0f, emptyMidi));
    const CallbackResult recoveryMissOne = callback(graphFrames, 9 * graphFrames);
    const CallbackResult recoveryMissTwo = callback(graphFrames, 10 * graphFrames);
    EXPECT_FLOAT_EQ(recoveryMissOne.left[0], 0.0f);
    EXPECT_FLOAT_EQ(recoveryMissTwo.left[0], 0.0f);
    const CallbackResult recovered = callback(graphFrames, 11 * graphFrames);
    EXPECT_FLOAT_EQ(recovered.left[0], 400.0f / 64.0f);
    EXPECT_FLOAT_EQ(recovered.left[63], 463.0f);
    EXPECT_EQ(recovered.disposition, Disposition::Passthrough);

    EXPECT_EQ(load(&shared->audio_in_head), 6u * guestFrames);
}

}  // namespace
