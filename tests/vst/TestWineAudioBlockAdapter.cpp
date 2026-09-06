#include <gtest/gtest.h>

#include "vst/WineAudioBlockAdapter.h"

#include <array>
#include <cstdint>
#include <type_traits>
#include <vector>

namespace {

using vsthost::WineAudioBlockAdapter;
using guitarrackcraft::AudioProcessContext;
using guitarrackcraft::MidiEvent;

static_assert(std::is_nothrow_default_constructible_v<WineAudioBlockAdapter>);
static_assert(std::is_nothrow_destructible_v<WineAudioBlockAdapter>);
static_assert(noexcept(std::declval<WineAudioBlockAdapter&>().reset()));
static_assert(noexcept(std::declval<WineAudioBlockAdapter&>().configure(16)));
static_assert(noexcept(std::declval<const WineAudioBlockAdapter&>().guestFrames()));
static_assert(noexcept(std::declval<WineAudioBlockAdapter&>().appendInput(
    nullptr, std::declval<const AudioProcessContext&>(), nullptr, 0)));
static_assert(noexcept(std::declval<const WineAudioBlockAdapter&>().accumulationLatencyFrames()));

AudioProcessContext Context(uint64_t samplePosition) {
    AudioProcessContext context;
    context.samplePosition = samplePosition;
    context.transportFrame = samplePosition + 17;
    context.sampleRate = 48000.0;
    context.playing = true;
    return context;
}

std::vector<float> Marker(uint32_t frames, float base) {
    std::vector<float> result(frames);
    for (uint32_t i = 0; i < frames; ++i) result[i] = base + static_cast<float>(i);
    return result;
}

MidiEvent Event(uint32_t frameOffset, uint8_t status = 0x90, uint8_t data1 = 60,
                uint8_t data2 = 100) {
    return MidiEvent{frameOffset, status, data1, data2};
}

TEST(WineAudioBlockAdapterContractTest, GuestQuantumRoundsUpForSupportedGraphQuantums) {
    struct Case {
        uint32_t graph;
        uint32_t guest;
    };
    const Case cases[] = {{16, 128}, {32, 128}, {64, 128}, {96, 192}, {128, 128}};

    for (const Case& test : cases) {
        WineAudioBlockAdapter adapter;
        ASSERT_TRUE(adapter.configure(test.graph)) << test.graph;
        EXPECT_TRUE(adapter.valid());
        EXPECT_EQ(adapter.graphFrames(), test.graph);
        EXPECT_EQ(adapter.guestFrames(), test.guest);
        EXPECT_EQ(adapter.guestFrames() - adapter.graphFrames(),
                  test.guest - test.graph);
        EXPECT_EQ(adapter.accumulationLatencyFrames(), test.guest - test.graph);
    }
}

TEST(WineAudioBlockAdapterContractTest, RejectsInvalidConfigurationWithoutBecomingUsable) {
    WineAudioBlockAdapter adapter;
    EXPECT_FALSE(adapter.configure(0));
    EXPECT_FALSE(adapter.valid());
    EXPECT_EQ(adapter.graphFrames(), 0u);
    EXPECT_EQ(adapter.guestFrames(), 0u);

    EXPECT_FALSE(adapter.configure(VSTPOC_MAX_BLOCK_FRAMES + 1u));
    EXPECT_FALSE(adapter.valid());
    EXPECT_EQ(adapter.graphFrames(), 0u);
    EXPECT_EQ(adapter.guestFrames(), 0u);
    EXPECT_EQ(adapter.accumulationLatencyFrames(), 0u);
}

TEST(WineAudioBlockAdapterContractTest, AppendsContinuousPlanarInputAndKeepsFirstContext) {
    const uint32_t graphs[] = {16, 32, 64, 96, 128};
    for (const uint32_t graph : graphs) {
        SCOPED_TRACE(graph);
        WineAudioBlockAdapter adapter(graph);
        ASSERT_TRUE(adapter.valid());

        std::array<const float*, 2> inputs{};
        std::vector<std::vector<float>> left;
        std::vector<std::vector<float>> right;
        const uint32_t blocks = adapter.guestFrames() / graph;
        for (uint32_t block = 0; block < blocks; ++block) {
            left.push_back(Marker(graph, 1000.0f + 100.0f * block));
            right.push_back(Marker(graph, -1000.0f - 100.0f * block));
            inputs = {left.back().data(), right.back().data()};
            ASSERT_TRUE(adapter.appendInput(inputs.data(), Context(100 + block), nullptr, 0));
            EXPECT_EQ(adapter.inputReady(), block + 1 == blocks);
        }

        ASSERT_TRUE(adapter.inputReady());
        EXPECT_FALSE(adapter.appendInput(inputs.data(), Context(999), nullptr, 0));
        ASSERT_NE(adapter.inputLeft(), nullptr);
        ASSERT_NE(adapter.inputRight(), nullptr);
        for (uint32_t block = 0; block < blocks; ++block) {
            for (uint32_t frame = 0; frame < graph; ++frame) {
                const uint32_t offset = block * graph + frame;
                EXPECT_FLOAT_EQ(adapter.inputLeft()[offset],
                                1000.0f + 100.0f * block + frame);
                EXPECT_FLOAT_EQ(adapter.inputRight()[offset],
                                -1000.0f - 100.0f * block + frame);
            }
        }
        EXPECT_EQ(adapter.inputContext().samplePosition, 100u);
        EXPECT_EQ(adapter.inputContext().transportFrame, 117u);
    }
}

TEST(WineAudioBlockAdapterContractTest, RebasesInputMidiAndCountsEventsDroppedAtCapacity) {
    std::array<const float*, 2> noInput{nullptr, nullptr};
    WineAudioBlockAdapter adapter(16);
    ASSERT_TRUE(adapter.valid());

    const MidiEvent first = Event(3, 0x90, 60, 100);
    const MidiEvent second = Event(99, 0x80, 60, 0);
    ASSERT_TRUE(adapter.appendInput(noInput.data(), Context(1), &first, 1));
    ASSERT_TRUE(adapter.appendInput(noInput.data(), Context(2), &second, 1));
    for (uint32_t block = 2; block < adapter.guestFrames() / adapter.graphFrames(); ++block) {
        ASSERT_TRUE(adapter.appendInput(noInput.data(), Context(10 + block), nullptr, 0));
    }
    ASSERT_TRUE(adapter.inputReady());
    ASSERT_EQ(adapter.inputMidiCount(), 2u);
    EXPECT_EQ(adapter.inputMidi()[0].frameOffset, 3u);
    EXPECT_EQ(adapter.inputMidi()[1].frameOffset, 31u);

    WineAudioBlockAdapter capped(16);
    std::vector<MidiEvent> events(VSTPOC_MAX_MIDI_EVENTS_PER_BLOCK + 3u);
    for (uint32_t i = 0; i < events.size(); ++i) {
        events[i] = Event(i % 16, 0x90, i & 0x7f, 1);
    }
    ASSERT_TRUE(capped.appendInput(noInput.data(), Context(1), events.data(), events.size()));
    for (uint32_t block = 1; block < capped.guestFrames() / capped.graphFrames(); ++block) {
        ASSERT_TRUE(capped.appendInput(noInput.data(), Context(10 + block), nullptr, 0));
    }
    ASSERT_TRUE(capped.inputReady());
    EXPECT_EQ(capped.inputMidiCount(), VSTPOC_MAX_MIDI_EVENTS_PER_BLOCK);
    EXPECT_EQ(capped.midiDropCount(), 3u);
    for (uint32_t i = 0; i < capped.inputMidiCount(); ++i) {
        EXPECT_EQ(capped.inputMidi()[i].frameOffset, i % 16u);
    }
}

TEST(WineAudioBlockAdapterContractTest,
     QueuesOutputBlocksWithPairedAudioAndMidiInFifoOrder) {
    constexpr uint32_t graph = 16;
    WineAudioBlockAdapter adapter(graph);
    ASSERT_TRUE(adapter.valid());
    const uint32_t slices = adapter.guestFrames() / graph;
    const uint32_t capacity = adapter.outputSlotCapacity();
    ASSERT_EQ(capacity, 3u);

    const auto fill = [&](float base) {
        for (uint32_t i = 0; i < adapter.guestFrames(); ++i) {
            adapter.outputLeft()[i] = base + static_cast<float>(i);
            adapter.outputRight()[i] = -base - static_cast<float>(i);
        }
    };

    for (uint32_t block = 0; block < capacity; ++block) {
        const float base = 1000.0f + 1000.0f * static_cast<float>(block);
        fill(base);
        const uint8_t midiBase = static_cast<uint8_t>(10u * (block + 1u));
        const MidiEvent midi[] = {
            Event(0, 0x90, midiBase, 1),
            Event(adapter.guestFrames() - 1, 0x90,
                  static_cast<uint8_t>(midiBase + 1u), 2),
        };
        ASSERT_TRUE(adapter.commitOutput(midi, 2));
        ASSERT_EQ(adapter.outputBlockCount(), block + 1u);
        EXPECT_EQ(adapter.outputWriteAvailable(), block + 1u < capacity);
        EXPECT_TRUE(adapter.outputReady());
    }
    EXPECT_FALSE(adapter.commitOutput(nullptr, 0));

    std::array<float, 16> left{};
    std::array<float, 16> right{};
    std::array<MidiEvent, 4> midi{};
    for (uint32_t block = 0; block < capacity; ++block) {
        const float base = 1000.0f + 1000.0f * static_cast<float>(block);
        const uint8_t midiBase = static_cast<uint8_t>(10u * (block + 1u));
        for (uint32_t slice = 0; slice < slices; ++slice) {
            const uint32_t count =
                adapter.copyOutput(left.data(), right.data(), midi.data(), midi.size());
            for (uint32_t i = 0; i < graph; ++i) {
                EXPECT_FLOAT_EQ(left[i], base + static_cast<float>(slice * graph + i));
                EXPECT_FLOAT_EQ(right[i], -base - static_cast<float>(slice * graph + i));
            }
            if (slice == 0 || slice + 1 == slices) {
                ASSERT_EQ(count, 1u);
                EXPECT_EQ(midi[0].frameOffset, slice == 0 ? 0u : graph - 1u);
                EXPECT_EQ(midi[0].data1,
                          static_cast<uint8_t>(midiBase + (slice == 0 ? 0 : 1)));
            } else {
                EXPECT_EQ(count, 0u);
            }
            const uint32_t remainingSlices = slices - slice - 1u;
            const uint32_t remainingBlocks = capacity - block - 1u;
            const uint32_t expectedFrames =
                remainingSlices != 0 ? remainingSlices * graph
                                     : (remainingBlocks != 0 ? adapter.guestFrames() : 0u);
            EXPECT_EQ(adapter.outputFramesAvailable(), expectedFrames);
        }
        EXPECT_EQ(adapter.outputBlockCount(), capacity - block - 1u);
    }
    EXPECT_FALSE(adapter.outputReady());
    EXPECT_TRUE(adapter.outputWriteAvailable());
    EXPECT_EQ(adapter.outputFramesAvailable(), 0u);
    EXPECT_EQ(adapter.outputMidiCount(), 0u);
}

TEST(WineAudioBlockAdapterContractTest,
     ReusesReleasedOutputSlotAndWrapsAcrossRepeatedQueueCycles) {
    constexpr uint32_t graph = 32;
    WineAudioBlockAdapter adapter(graph);
    ASSERT_TRUE(adapter.valid());
    const uint32_t slices = adapter.guestFrames() / graph;
    const uint32_t capacity = adapter.outputSlotCapacity();
    ASSERT_EQ(capacity, 3u);
    std::array<float, 32> left{};
    std::array<float, 32> right{};

    const auto fill = [&](float base) {
        for (uint32_t i = 0; i < adapter.guestFrames(); ++i) {
            adapter.outputLeft()[i] = base + static_cast<float>(i);
            adapter.outputRight()[i] = -base - static_cast<float>(i);
        }
        ASSERT_TRUE(adapter.commitOutput(nullptr, 0));
    };
    const auto drainBlock = [&](float base) {
        for (uint32_t slice = 0; slice < slices; ++slice) {
            ASSERT_EQ(adapter.copyOutput(left.data(), right.data(), nullptr, 0), 0u);
            EXPECT_FLOAT_EQ(left[0], base + static_cast<float>(slice * graph));
            EXPECT_FLOAT_EQ(right[graph - 1],
                            -base - static_cast<float>(slice * graph + graph - 1));
        }
    };

    for (uint32_t cycle = 0; cycle < 5; ++cycle) {
        const float first = 1000.0f + 100.0f * static_cast<float>(cycle);
        for (uint32_t block = 0; block < capacity; ++block) {
            fill(first + 10.0f * static_cast<float>(block));
            EXPECT_EQ(adapter.outputBlockCount(), block + 1u);
        }
        EXPECT_FALSE(adapter.outputWriteAvailable());
        drainBlock(first);
        EXPECT_EQ(adapter.outputBlockCount(), capacity - 1u);
        EXPECT_TRUE(adapter.outputWriteAvailable());

        const float reused = first + 10.0f * static_cast<float>(capacity);
        fill(reused);
        EXPECT_EQ(adapter.outputBlockCount(), capacity);
        for (uint32_t block = 1; block < capacity; ++block) {
            drainBlock(first + 10.0f * static_cast<float>(block));
        }
        drainBlock(reused);
        EXPECT_EQ(adapter.outputBlockCount(), 0u);
        EXPECT_FALSE(adapter.outputReady());
    }
}

TEST(WineAudioBlockAdapterContractTest, ReusesAdapterForIndependentFullCycles) {
    WineAudioBlockAdapter adapter(96);
    ASSERT_TRUE(adapter.valid());
    std::array<const float*, 2> noInput{nullptr, nullptr};
    std::array<float, 96> left{};
    std::array<float, 96> right{};

    for (uint32_t cycle = 0; cycle < 3; ++cycle) {
        ASSERT_TRUE(adapter.appendInput(noInput.data(), Context(100 + cycle), nullptr, 0));
        ASSERT_TRUE(adapter.appendInput(noInput.data(), Context(200 + cycle), nullptr, 0));
        ASSERT_TRUE(adapter.inputReady());
        EXPECT_EQ(adapter.inputContext().samplePosition, 100 + cycle);
        adapter.consumeInput();

        for (uint32_t i = 0; i < adapter.guestFrames(); ++i) {
            adapter.outputLeft()[i] = static_cast<float>(cycle * 1000 + i);
            adapter.outputRight()[i] = static_cast<float>(cycle * 1000 + i + 1);
        }
        ASSERT_TRUE(adapter.commitOutput(nullptr, 0));
        ASSERT_TRUE(adapter.outputReady());
        for (uint32_t slice = 0; slice < 2; ++slice) {
            ASSERT_EQ(adapter.copyOutput(left.data(), right.data(), nullptr, 0), 0u);
            EXPECT_FLOAT_EQ(left[0], static_cast<float>(cycle * 1000 + slice * 96));
            EXPECT_FLOAT_EQ(right[95], static_cast<float>(cycle * 1000 + slice * 96 + 96));
        }
        EXPECT_FALSE(adapter.outputReady());
    }
}

TEST(WineAudioBlockAdapterContractTest, OutputMidiCapacityDropsExcessAndDoesNotRepeatAcrossSlices) {
    WineAudioBlockAdapter adapter(128);
    ASSERT_TRUE(adapter.valid());
    for (uint32_t i = 0; i < adapter.guestFrames(); ++i) {
        adapter.outputLeft()[i] = static_cast<float>(i);
        adapter.outputRight()[i] = static_cast<float>(i + 1);
    }
    std::vector<MidiEvent> midi(VSTPOC_MAX_MIDI_EVENTS_PER_BLOCK + 2u);
    for (uint32_t i = 0; i < midi.size(); ++i) midi[i] = Event(i == 0 ? 0 : 127, 0x90, i, 1);
    ASSERT_TRUE(adapter.commitOutput(midi.data(), midi.size()));
    std::array<float, 128> left{};
    std::array<float, 128> right{};
    std::array<MidiEvent, VSTPOC_MAX_MIDI_EVENTS_PER_BLOCK> output{};
    EXPECT_EQ(adapter.copyOutput(left.data(), right.data(), output.data(), output.size()),
              VSTPOC_MAX_MIDI_EVENTS_PER_BLOCK);
    EXPECT_EQ(adapter.midiDropCount(), 2u);
    EXPECT_FALSE(adapter.outputReady());
}

TEST(WineAudioBlockAdapterContractTest, ResetClearsPartialInputOutputAndAllowsIndependentCycles) {
    WineAudioBlockAdapter adapter(32);
    ASSERT_TRUE(adapter.valid());
    const uint32_t capacity = adapter.outputSlotCapacity();
    ASSERT_EQ(capacity, 3u);
    std::array<const float*, 2> noInput{nullptr, nullptr};
    ASSERT_TRUE(adapter.appendInput(noInput.data(), Context(7), nullptr, 0));
    ASSERT_FALSE(adapter.inputReady());
    for (uint32_t block = 0; block < capacity; ++block) {
        ASSERT_TRUE(adapter.commitOutput(nullptr, 0));
    }
    ASSERT_TRUE(adapter.outputReady());
    ASSERT_EQ(adapter.outputBlockCount(), capacity);
    EXPECT_FALSE(adapter.outputWriteAvailable());

    adapter.reset();
    EXPECT_FALSE(adapter.valid());
    EXPECT_FALSE(adapter.inputReady());
    EXPECT_FALSE(adapter.outputReady());
    EXPECT_FALSE(adapter.outputWriteAvailable());
    EXPECT_EQ(adapter.outputBlockCount(), 0u);
    EXPECT_EQ(adapter.inputMidiCount(), 0u);
    EXPECT_EQ(adapter.outputMidiCount(), 0u);
    EXPECT_EQ(adapter.midiDropCount(), 0u);

    ASSERT_TRUE(adapter.configure(32));
    std::vector<float> input(32, 5.0f);
    std::array<const float*, 2> inputs{input.data(), input.data()};
    ASSERT_TRUE(adapter.appendInput(inputs.data(), Context(44), nullptr, 0));
    ASSERT_TRUE(adapter.appendInput(inputs.data(), Context(55), nullptr, 0));
    ASSERT_TRUE(adapter.appendInput(inputs.data(), Context(66), nullptr, 0));
    ASSERT_TRUE(adapter.appendInput(inputs.data(), Context(77), nullptr, 0));
    ASSERT_TRUE(adapter.inputReady());
    EXPECT_EQ(adapter.inputContext().samplePosition, 44u);
    EXPECT_FLOAT_EQ(adapter.inputLeft()[0], 5.0f);
}

}  // namespace
