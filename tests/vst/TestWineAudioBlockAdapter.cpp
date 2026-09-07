#include <gtest/gtest.h>

#include "vst/WineAudioBlockAdapter.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

using vsthost::WineAudioBlockAdapter;
using guitarrackcraft::AudioProcessContext;
using guitarrackcraft::MidiBuffer;

static_assert(std::is_nothrow_default_constructible_v<WineAudioBlockAdapter>);
static_assert(std::is_nothrow_destructible_v<WineAudioBlockAdapter>);
static_assert(noexcept(std::declval<WineAudioBlockAdapter&>().reset()));
static_assert(noexcept(std::declval<WineAudioBlockAdapter&>().configure(16)));
static_assert(noexcept(WineAudioBlockAdapter::acceptsCallbackFrames(1536, 192)));
static_assert(noexcept(std::declval<const WineAudioBlockAdapter&>().guestFrames()));
static_assert(noexcept(std::declval<WineAudioBlockAdapter&>().appendInput(
    nullptr, std::declval<const AudioProcessContext&>(),
    std::declval<const MidiBuffer&>())));

AudioProcessContext context(uint64_t samplePosition) {
    AudioProcessContext result;
    result.samplePosition = samplePosition;
    result.transportFrame = samplePosition + 17;
    result.sampleRate = 48000.0;
    result.playing = true;
    return result;
}

std::vector<float> marker(uint32_t frames, float base) {
    std::vector<float> result(frames);
    for (uint32_t i = 0; i < frames; ++i)
        result[i] = base + static_cast<float>(i);
    return result;
}

MidiBuffer event(uint32_t frame, uint8_t status, uint8_t data1, uint8_t data2) {
    MidiBuffer result;
    const uint8_t payload[] = {status, data1, data2};
    EXPECT_TRUE(result.append(frame, payload, sizeof(payload)));
    return result;
}
TEST(WineFailClosedAudioStateTest, StartupFailureIsSilentInsteadOfCurrentInput) {
    vsthost::WineFailClosedAudioState state;
    state.reset();
    constexpr uint32_t frames = 8;
    std::array<float, frames> left{};
    std::array<float, frames> right{};
    left.fill(91.0f);
    right.fill(-73.0f);
    float* outputs[] = {left.data(), right.data()};

    state.renderFailure(outputs, frames);

    for (uint32_t i = 0; i < frames; ++i) {
        EXPECT_FLOAT_EQ(left[i], 0.0f);
        EXPECT_FLOAT_EQ(right[i], 0.0f);
    }
}

TEST(WineFailClosedAudioStateTest, MissDecaysProcessedTailThenReachesSilence) {
    vsthost::WineFailClosedAudioState state;
    state.reset();
    constexpr uint32_t frames = 64;
    std::array<float, frames> processedLeft{};
    std::array<float, frames> processedRight{};
    processedLeft.fill(12.0f);
    processedRight.fill(-18.0f);
    state.rememberProcessed(processedLeft.data(), processedRight.data(), frames);

    std::array<float, frames> left{};
    std::array<float, frames> right{};
    left.fill(91.0f);
    right.fill(-73.0f);
    float* outputs[] = {left.data(), right.data()};
    state.renderFailure(outputs, frames);

    EXPECT_FLOAT_EQ(left.front(), 12.0f * 63.0f / 64.0f);
    EXPECT_FLOAT_EQ(right.front(), -18.0f * 63.0f / 64.0f);
    EXPECT_LT(std::abs(left.front()), 91.0f);
    EXPECT_LT(std::abs(right.front()), 73.0f);
    EXPECT_FLOAT_EQ(left.back(), 0.0f);
    EXPECT_FLOAT_EQ(right.back(), 0.0f);
}

TEST(WineFailClosedAudioStateTest, RecoveryFadesProcessedOutputWithoutCurrentInput) {
    vsthost::WineFailClosedAudioState state;
    state.reset();
    constexpr uint32_t frames = 64;
    std::array<float, frames> left{};
    std::array<float, frames> right{};
    for (uint32_t i = 0; i < frames; ++i) {
        left[i] = 300.0f + static_cast<float>(i);
        right[i] = -400.0f - static_cast<float>(i);
    }
    std::array<float, frames> inputLeft{};
    std::array<float, frames> inputRight{};
    inputLeft.fill(900.0f);
    inputRight.fill(-900.0f);
    float* outputs[] = {left.data(), right.data()};

    state.applyRecovery(outputs, frames);

    EXPECT_FLOAT_EQ(left.front(), 300.0f / 64.0f);
    EXPECT_FLOAT_EQ(right.front(), -400.0f / 64.0f);
    EXPECT_FLOAT_EQ(left.back(), 363.0f);
    EXPECT_FLOAT_EQ(right.back(), -463.0f);
    EXPECT_NE(left.front(), inputLeft.front());
    EXPECT_NE(right.front(), inputRight.front());
}


TEST(WineAudioBlockAdapterContractTest, GuestQuantumRoundsUpForSupportedGraphQuantums) {
    struct Case { uint32_t graph; uint32_t guest; };
    const Case cases[] = {{16, 128}, {32, 128}, {64, 128}, {96, 192}, {128, 128}};
    for (const Case test : cases) {
        WineAudioBlockAdapter adapter;
        ASSERT_TRUE(adapter.configure(test.graph));
        EXPECT_TRUE(adapter.valid());
        EXPECT_EQ(adapter.graphFrames(), test.graph);
        EXPECT_EQ(adapter.guestFrames(), test.guest);
        EXPECT_EQ(adapter.accumulationLatencyFrames(), test.guest - test.graph);
    }
}

TEST(WineAudioBlockAdapterContractTest, RejectsInvalidConfiguration) {
    WineAudioBlockAdapter adapter;
    EXPECT_FALSE(adapter.configure(0));
    EXPECT_FALSE(adapter.valid());
    EXPECT_FALSE(adapter.configure(VSTPOC_MAX_BLOCK_FRAMES + 1u));
    EXPECT_FALSE(adapter.valid());
    EXPECT_EQ(adapter.graphFrames(), 0u);
    EXPECT_EQ(adapter.guestFrames(), 0u);
}

TEST(WineAudioBlockAdapterContractTest, AcceptsActualCallbackWithinActivationCapacity) {
    struct Case {
        uint32_t capacity;
        uint32_t callback;
        bool accepted;
    };
    const Case cases[] = {
        {1536, 192, true},
        {1536, 64, true},
        {192, 193, false},
        {192, 0, false},
        {VSTPOC_MAX_BLOCK_FRAMES, VSTPOC_MAX_BLOCK_FRAMES, true},
        {VSTPOC_MAX_BLOCK_FRAMES, VSTPOC_MAX_BLOCK_FRAMES + 1u, false},
    };
    for (const Case test : cases) {
        EXPECT_EQ(WineAudioBlockAdapter::acceptsCallbackFrames(
                      test.capacity, test.callback),
                  test.accepted)
            << "capacity=" << test.capacity << " callback=" << test.callback;
    }
}

TEST(WineAudioBlockAdapterContractTest, AppendsPlanarInputAndRebasesMidiFrames) {
    constexpr uint32_t graph = 16;
    WineAudioBlockAdapter adapter(graph);
    std::array<const float*, 2> noInput{nullptr, nullptr};
    MidiBuffer first = event(3, 0x90, 60, 100);
    MidiBuffer second = event(99, 0x80, 60, 0);
    ASSERT_TRUE(adapter.appendInput(noInput.data(), context(1), first));
    ASSERT_TRUE(adapter.appendInput(noInput.data(), context(2), second));
    MidiBuffer empty;
    for (uint32_t block = 2; block < adapter.guestFrames() / graph; ++block)
        ASSERT_TRUE(adapter.appendInput(noInput.data(), context(10 + block), empty));
    ASSERT_TRUE(adapter.inputReady());
    ASSERT_EQ(adapter.inputMidi().eventCount(), 2u);
    EXPECT_EQ(adapter.inputMidi().eventAt(0).frameOffset, 3u);
    EXPECT_EQ(adapter.inputMidi().eventAt(1).frameOffset, 31u);
    EXPECT_EQ(adapter.inputContext().samplePosition, 1u);
    EXPECT_EQ(adapter.inputContext().transportFrame, 18u);
}

TEST(WineAudioBlockAdapterContractTest, QueuesAudioAndMidiBlocksInFifoOrder) {
    constexpr uint32_t graph = 16;
    WineAudioBlockAdapter adapter(graph);
    const uint32_t capacity = adapter.outputSlotCapacity();
    const uint32_t slices = adapter.guestFrames() / graph;
    ASSERT_EQ(capacity, 3u);
    for (uint32_t block = 0; block < capacity; ++block) {
        const float base = 1000.0f + 1000.0f * block;
        for (uint32_t i = 0; i < adapter.guestFrames(); ++i) {
            adapter.outputLeft()[i] = base + i;
            adapter.outputRight()[i] = -base - i;
        }
        MidiBuffer midi;
        const uint8_t payload[] = {0x90, static_cast<uint8_t>(10 + block), 1};
        ASSERT_TRUE(midi.append(0, payload, sizeof(payload)));
        ASSERT_TRUE(adapter.commitOutput(midi));
    }
    EXPECT_FALSE(adapter.outputWriteAvailable());
    for (uint32_t block = 0; block < capacity; ++block) {
        std::array<float, graph> left{};
        std::array<float, graph> right{};
        for (uint32_t slice = 0; slice < slices; ++slice) {
            MidiBuffer midi;
            ASSERT_EQ(adapter.copyOutput(left.data(), right.data(), midi),
                      slice == 0 ? 1u : 0u);
            const float base = 1000.0f + 1000.0f * block;
            EXPECT_FLOAT_EQ(left[0], base + slice * graph);
            EXPECT_FLOAT_EQ(right[graph - 1], -base - slice * graph - graph + 1);
            if (slice == 0) {
                ASSERT_EQ(midi.eventCount(), 1u);
                EXPECT_EQ(midi.eventAt(0).frameOffset, 0u);
                EXPECT_EQ(midi.payloadFor(midi.eventAt(0))[1],
                          static_cast<uint8_t>(10 + block));
            }
        }
    }
    EXPECT_FALSE(adapter.outputReady());
    EXPECT_TRUE(adapter.outputWriteAvailable());
}

TEST(WineAudioBlockAdapterContractTest, ResetDropsPartialStateAndAllowsReuse) {
    WineAudioBlockAdapter adapter(32);
    std::array<const float*, 2> noInput{nullptr, nullptr};
    MidiBuffer empty;
    ASSERT_TRUE(adapter.appendInput(noInput.data(), context(7), empty));
    ASSERT_FALSE(adapter.inputReady());
    for (uint32_t block = 0; block < adapter.outputSlotCapacity(); ++block)
        ASSERT_TRUE(adapter.commitOutput(empty));
    ASSERT_TRUE(adapter.outputReady());
    adapter.reset();
    EXPECT_FALSE(adapter.valid());
    EXPECT_FALSE(adapter.inputReady());
    EXPECT_FALSE(adapter.outputReady());
    EXPECT_EQ(adapter.outputBlockCount(), 0u);
    EXPECT_EQ(adapter.midiDropCount(), 0u);

    ASSERT_TRUE(adapter.configure(32));
    std::vector<float> input(32, 5.0f);
    std::array<const float*, 2> inputs{input.data(), input.data()};
    for (uint32_t block = 0; block < adapter.guestFrames() / 32; ++block)
        ASSERT_TRUE(adapter.appendInput(inputs.data(), context(44 + block), empty));
    EXPECT_TRUE(adapter.inputReady());
    EXPECT_EQ(adapter.inputContext().samplePosition, 44u);
    EXPECT_FLOAT_EQ(adapter.inputLeft()[0], 5.0f);
}

TEST(WineAudioBlockAdapterContractTest, PreservesExactMaximumSysExAcrossInputAndOutput) {
    WineAudioBlockAdapter adapter(32);
    std::array<const float*, 2> noInput{nullptr, nullptr};
    MidiBuffer input;
    std::vector<uint8_t> bytes(guitarrackcraft::kMaxMidiPayloadBytes);
    bytes.front() = 0xf0;
    for (uint32_t i = 1; i + 1 < bytes.size(); ++i)
        bytes[i] = static_cast<uint8_t>((i * 11u) % 127u);
    bytes.back() = 0xf7;
    ASSERT_TRUE(input.append(31, bytes.data(), bytes.size()));
    ASSERT_TRUE(adapter.appendInput(noInput.data(), context(1), input));
    for (uint32_t block = 1; block < adapter.guestFrames() / 32; ++block)
        ASSERT_TRUE(adapter.appendInput(noInput.data(), context(2 + block), MidiBuffer{}));
    ASSERT_TRUE(adapter.inputReady());
    ASSERT_EQ(adapter.inputMidi().eventCount(), 1u);
    const auto& received = adapter.inputMidi().eventAt(0);
    EXPECT_EQ(received.frameOffset, 31u);
    ASSERT_EQ(received.payloadSize, bytes.size());
    EXPECT_TRUE(std::equal(bytes.begin(), bytes.end(),
                           adapter.inputMidi().payloadFor(received)));
    ASSERT_TRUE(adapter.commitOutput(adapter.inputMidi(), true));
    std::array<float, 32> left{};
    std::array<float, 32> right{};
    MidiBuffer output;
    bool saw = false;
    for (uint32_t slice = 0; slice < adapter.guestFrames() / 32; ++slice) {
        output.clear();
        EXPECT_TRUE(adapter.outputAuthoritative());
        adapter.copyOutput(left.data(), right.data(), output);
        if (slice == 0) {
            saw = true;
            ASSERT_EQ(output.eventCount(), 1u);
            const auto& event = output.eventAt(0);
            EXPECT_EQ(event.frameOffset, 31u);
            ASSERT_EQ(event.payloadSize, bytes.size());
            EXPECT_TRUE(std::equal(bytes.begin(), bytes.end(),
                                   output.payloadFor(event)));
        } else {
            EXPECT_EQ(output.eventCount(), 0u);
        }
    }
    EXPECT_TRUE(saw);
}

}  // namespace
