#include <gtest/gtest.h>

#include "plugin/RackGraph.h"
#include <liblowlatencyaudio/UsbScheduling.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <string>
#include <vector>
#include <utility>

namespace allocation_probe {
extern thread_local bool enabled;
extern thread_local std::size_t allocations;
}

namespace {
using guitarrackcraft::RackGraph;
using guitarrackcraft::RackPathId;
using guitarrackcraft::WavClip;
using guitarrackcraft::TrackClipSlotInfo;

using guitarrackcraft::IPlugin;

class StereoOffsetPlugin final : public IPlugin {
public:
    void activate(float, uint32_t) override {}
    void deactivate() override {}

    uint32_t process(const float* const* inputs, float* const* outputs,
                     uint32_t numFrames,
                     const guitarrackcraft::AudioProcessContext&,
                     const guitarrackcraft::MidiEvent*, uint32_t,
                     guitarrackcraft::MidiEvent*, uint32_t) override {
        for (uint32_t frame = 0; frame < numFrames; ++frame) {
            outputs[0][frame] = inputs[0][frame] + 10.0f;
            outputs[1][frame] = inputs[1][frame] + 20.0f;
        }
        return 0;
    }

    guitarrackcraft::PluginInfo getInfo() const override { return {}; }
    void setParameter(uint32_t, float) override {}
    float getParameter(uint32_t) const override { return 0.0f; }
    uint32_t getNumInputPorts() const override { return 2; }
    uint32_t getNumOutputPorts() const override { return 2; }
};
constexpr float kTestSampleRate = 60.0f;
constexpr double kTestBpm = 60.0;

std::shared_ptr<const WavClip> makeClip(std::initializer_list<float> left,
                                        uint32_t sampleRate,
                                        std::initializer_list<float> right = {},
                                        std::string name = "test.wav") {
    auto clip = std::make_shared<WavClip>();
    clip->left.assign(left);
    clip->right.assign(right);
    clip->sampleRate = sampleRate;
    clip->displayName = std::move(name);
    return clip;
}

struct StereoBuffers {
    std::array<float, 512> left{};
    std::array<float, 512> right{};
    std::array<float, 512> outputLeft{};
    std::array<float, 512> outputRight{};

    const float* inputs[2] = {left.data(), right.data()};
    float* outputs[2] = {outputLeft.data(), outputRight.data()};
};

void configure(RackGraph& graph, uint32_t capacity = 512) {
    graph.setSampleRate(kTestSampleRate, capacity);
    graph.setBeatsPerMinute(kTestBpm);
    graph.setAvailableInputChannelCount(2);
}

void clearBuffers(StereoBuffers& buffers) {
    buffers.left.fill(0.0f);
    buffers.right.fill(0.0f);
    buffers.outputLeft.fill(-99.0f);
    buffers.outputRight.fill(-99.0f);
}

void expectSilence(const StereoBuffers& buffers, uint32_t frames) {
    for (uint32_t frame = 0; frame < frames; ++frame) {
        EXPECT_FLOAT_EQ(buffers.outputLeft[frame], 0.0f) << "frame " << frame;
        EXPECT_FLOAT_EQ(buffers.outputRight[frame], 0.0f) << "frame " << frame;
    }
}

const guitarrackcraft::TrackSnapshot& trackSnapshot(const RackGraph& graph,
                                                     RackPathId id,
                                                     std::vector<guitarrackcraft::TrackSnapshot>& storage) {
    storage = graph.getTracks();
    for (const auto& track : storage) {
        if (track.id == id) return track;
    }
    ADD_FAILURE() << "missing track " << id;
    return storage.front();
}
const TrackClipSlotInfo* findClipSlot(const std::vector<TrackClipSlotInfo>& slots,
                                      uint32_t slot) {
    for (const auto& info : slots) {
        if (info.slot == slot) return &info;
    }
    return nullptr;
}

} // namespace

TEST(RackGraphTransportTest, GlobalClockIsUnboundedAndHasNoClipEnd) {
    RackGraph graph;
    configure(graph);
    const RackPathId track = graph.getTracks().front().id;
    ASSERT_TRUE(graph.attachTrackWav(track, makeClip({1.0f, 2.0f}, 60)));

    StereoBuffers buffers;
    clearBuffers(buffers);
    graph.setTransportPlaying(true);
    graph.setTrackTransportPlaying(track, true, guitarrackcraft::LaunchQuantization::Sixteenth);
    graph.process(buffers.inputs, 2, buffers.outputs, 30);

    // The two-frame clip ends at global frame 17, but the global clock remains running.
    EXPECT_FLOAT_EQ(buffers.outputLeft[15], 1.0f);
    EXPECT_FLOAT_EQ(buffers.outputLeft[16], 2.0f);
    EXPECT_FLOAT_EQ(buffers.outputLeft[17], 0.0f);
    const auto transport = graph.getTransportSnapshot();
    EXPECT_TRUE(transport.playing);
    EXPECT_EQ(transport.samplePosition, 30u);
    EXPECT_EQ(transport.transportFrame, 30u);
    EXPECT_DOUBLE_EQ(transport.positionSec, 0.5);

    std::vector<guitarrackcraft::TrackSnapshot> tracks;
    const auto& ended = trackSnapshot(graph, track, tracks);
    EXPECT_FALSE(ended.playing);
    EXPECT_EQ(ended.transportFrame, 2u);
    EXPECT_DOUBLE_EQ(ended.positionSec, 2.0 / 60.0);
    EXPECT_FALSE(ended.recordPending);
    EXPECT_FALSE(ended.recording);
}

TEST(RackGraphTransportTest, GlobalStopStopsTracksWithoutImplicitRelaunch) {
    RackGraph graph;
    configure(graph);
    const RackPathId track = graph.getTracks().front().id;
    ASSERT_TRUE(graph.attachTrackWav(track, makeClip({1.0f, 2.0f, 3.0f, 4.0f}, 60)));

    StereoBuffers buffers;
    clearBuffers(buffers);
    graph.setTransportPlaying(true);
    graph.setTrackTransportLooping(track, true);
    graph.setTrackTransportPlaying(track, true, guitarrackcraft::LaunchQuantization::Sixteenth);
    graph.process(buffers.inputs, 2, buffers.outputs, 16);
    EXPECT_FLOAT_EQ(buffers.outputLeft[15], 1.0f);

    graph.setTransportPlaying(false);
    clearBuffers(buffers);
    graph.process(buffers.inputs, 2, buffers.outputs, 4);
    expectSilence(buffers, 4);
    std::vector<guitarrackcraft::TrackSnapshot> tracks;
    EXPECT_FALSE(trackSnapshot(graph, track, tracks).playing);
    EXPECT_FALSE(trackSnapshot(graph, track, tracks).recordPending);
    EXPECT_FALSE(trackSnapshot(graph, track, tracks).recording);
    EXPECT_FALSE(graph.getTransportSnapshot().playing);
    EXPECT_EQ(graph.getTransportSnapshot().transportFrame, 16u);

    // Resuming the global clock does not relaunch a stopped track.
    graph.setTransportPlaying(true);
    clearBuffers(buffers);
    graph.process(buffers.inputs, 2, buffers.outputs, 4);
    expectSilence(buffers, 4);
    EXPECT_FALSE(trackSnapshot(graph, track, tracks).playing);

    // A new explicit launch is required, and is quantized from the resumed global frame.
    graph.setTrackTransportPlaying(track, true, guitarrackcraft::LaunchQuantization::Sixteenth);
    clearBuffers(buffers);
    graph.process(buffers.inputs, 2, buffers.outputs, 11);
    EXPECT_FLOAT_EQ(buffers.outputLeft[9], 0.0f);
    EXPECT_FLOAT_EQ(buffers.outputLeft[10], 1.0f);
    EXPECT_TRUE(trackSnapshot(graph, track, tracks).playing);
}

TEST(RackGraphTransportTest, TracksKeepIndependentPlayheadsAndLoopLocally) {
    RackGraph graph;
    configure(graph);
    const RackPathId first = graph.getTracks().front().id;
    const RackPathId second = graph.addTrack();
    ASSERT_NE(second, 0u);
    ASSERT_TRUE(graph.attachTrackWav(first, makeClip({1.0f, 2.0f}, 60, {}, "loop.wav")));
    ASSERT_TRUE(graph.attachTrackWav(second, makeClip({10.0f, 20.0f, 30.0f}, 60, {}, "one-shot.wav")));

    StereoBuffers buffers;
    clearBuffers(buffers);
    graph.setTransportPlaying(true);
    graph.setTrackTransportLooping(first, true);
    graph.setTrackTransportPlaying(first, true, guitarrackcraft::LaunchQuantization::Sixteenth);
    graph.setTrackTransportPlaying(second, true, guitarrackcraft::LaunchQuantization::Eighth);
    graph.process(buffers.inputs, 2, buffers.outputs, 32);

    // First launches at frame 15 and loops every two local frames; second launches at 30.
    EXPECT_FLOAT_EQ(buffers.outputLeft[14], 0.0f);
    EXPECT_FLOAT_EQ(buffers.outputLeft[15], 1.0f);
    EXPECT_FLOAT_EQ(buffers.outputLeft[16], 2.0f);
    EXPECT_FLOAT_EQ(buffers.outputLeft[17], 1.0f);
    EXPECT_FLOAT_EQ(buffers.outputLeft[29], 1.0f);
    EXPECT_FLOAT_EQ(buffers.outputLeft[30], 12.0f);
    EXPECT_FLOAT_EQ(buffers.outputLeft[31], 21.0f);

    std::vector<guitarrackcraft::TrackSnapshot> tracks;
    const auto& firstState = trackSnapshot(graph, first, tracks);
    const uint64_t firstFrame = firstState.transportFrame;
    const double firstPosition = firstState.positionSec;
    EXPECT_TRUE(firstState.playing);
    EXPECT_TRUE(firstState.looping);
    EXPECT_EQ(firstFrame, 1u);
    EXPECT_DOUBLE_EQ(firstPosition, 1.0 / 60.0);

    const auto& secondState = trackSnapshot(graph, second, tracks);
    EXPECT_TRUE(secondState.playing);
    EXPECT_FALSE(secondState.looping);
    EXPECT_EQ(secondState.transportFrame, 2u);
    EXPECT_DOUBLE_EQ(secondState.positionSec, 2.0 / 60.0);
    EXPECT_NE(firstFrame, secondState.transportFrame);
    EXPECT_FALSE(firstState.recordPending);
    EXPECT_FALSE(firstState.recording);
    EXPECT_FALSE(secondState.recordPending);
    EXPECT_FALSE(secondState.recording);
    EXPECT_NE(firstPosition, secondState.positionSec);

    clearBuffers(buffers);
    graph.process(buffers.inputs, 2, buffers.outputs, 3);
    EXPECT_FLOAT_EQ(buffers.outputLeft[0], 32.0f);
    EXPECT_FLOAT_EQ(buffers.outputLeft[1], 1.0f);
    EXPECT_FLOAT_EQ(buffers.outputLeft[2], 2.0f);
    EXPECT_TRUE(trackSnapshot(graph, first, tracks).playing);
    EXPECT_FALSE(trackSnapshot(graph, second, tracks).playing);
}

TEST(RackGraphTransportTest, LaunchRequestWhileGlobalStoppedDoesNotQueueAnImplicitLaunch) {
    RackGraph graph;
    configure(graph);
    const RackPathId track = graph.getTracks().front().id;
    ASSERT_TRUE(graph.attachTrackWav(track, makeClip({5.0f, 6.0f}, 60)));

    StereoBuffers buffers;
    clearBuffers(buffers);
    graph.setTrackTransportPlaying(track, true, guitarrackcraft::LaunchQuantization::Quarter);
    graph.process(buffers.inputs, 2, buffers.outputs, 10);
    expectSilence(buffers, 10);
    std::vector<guitarrackcraft::TrackSnapshot> tracks;
    EXPECT_FALSE(trackSnapshot(graph, track, tracks).playing);
    EXPECT_FALSE(trackSnapshot(graph, track, tracks).recordPending);
    EXPECT_FALSE(trackSnapshot(graph, track, tracks).recording);

    graph.setTransportPlaying(true);
    clearBuffers(buffers);
    graph.process(buffers.inputs, 2, buffers.outputs, 61);
    expectSilence(buffers, 61);
    EXPECT_FALSE(trackSnapshot(graph, track, tracks).playing);
}

TEST(RackGraphTransportTest, LaunchesAtStrictNextQuantizedGlobalBoundary) {
    struct QuantizationCase {
        guitarrackcraft::LaunchQuantization quantization;
        uint32_t boundary;
    };
    const std::array<QuantizationCase, 4> cases = {{
        {guitarrackcraft::LaunchQuantization::Bar, 240},
        {guitarrackcraft::LaunchQuantization::Quarter, 60},
        {guitarrackcraft::LaunchQuantization::Eighth, 30},
        {guitarrackcraft::LaunchQuantization::Sixteenth, 15},
    }};

    for (const auto& testCase : cases) {
        SCOPED_TRACE(testCase.boundary);
        RackGraph graph;
        configure(graph);
        const RackPathId track = graph.getTracks().front().id;
        ASSERT_TRUE(graph.attachTrackWav(track, makeClip({7.0f, 8.0f}, 60)));

        StereoBuffers buffers;
        clearBuffers(buffers);
        graph.setTransportPlaying(true);
        graph.process(buffers.inputs, 2, buffers.outputs, 3);
        graph.setTrackTransportPlaying(track, true, testCase.quantization);
        clearBuffers(buffers);
        const uint32_t framesThroughBoundary = testCase.boundary - 2;
        graph.process(buffers.inputs, 2, buffers.outputs, framesThroughBoundary);

        // Request occurs at global frame 3: frame boundary-1 is silent, boundary is clip frame 0.
        EXPECT_FLOAT_EQ(buffers.outputLeft[framesThroughBoundary - 2], 0.0f);
        EXPECT_FLOAT_EQ(buffers.outputLeft[framesThroughBoundary - 1], 7.0f);
        const auto transport = graph.getTransportSnapshot();
        EXPECT_EQ(transport.transportFrame, testCase.boundary + 1u);
        std::vector<guitarrackcraft::TrackSnapshot> tracks;
        const auto& state = trackSnapshot(graph, track, tracks);
        EXPECT_TRUE(state.playing);
        EXPECT_EQ(state.transportFrame, 1u);
        EXPECT_DOUBLE_EQ(state.positionSec, 1.0 / 60.0);
        EXPECT_FALSE(state.recordPending);
        EXPECT_FALSE(state.recording);
    }
}
TEST(RackGraphTransportTest, NoneLaunchesTrackAtCurrentTransportFrame) {
    RackGraph graph;
    configure(graph);
    const RackPathId track = graph.getTracks().front().id;
    ASSERT_TRUE(graph.attachTrackWav(track, makeClip({7.0f, 8.0f, 9.0f}, 60)));

    StereoBuffers buffers;
    clearBuffers(buffers);
    graph.setTransportPlaying(true);
    graph.process(buffers.inputs, 2, buffers.outputs, 3);
    ASSERT_EQ(graph.getTransportSnapshot().transportFrame, 3u);

    ASSERT_TRUE(graph.setTrackTransportPlaying(
        track, true, guitarrackcraft::LaunchQuantization::None));
    clearBuffers(buffers);
    graph.process(buffers.inputs, 2, buffers.outputs, 2);

    EXPECT_FLOAT_EQ(buffers.outputLeft[0], 7.0f);
    EXPECT_FLOAT_EQ(buffers.outputLeft[1], 8.0f);
    std::vector<guitarrackcraft::TrackSnapshot> tracks;
    const auto& state = trackSnapshot(graph, track, tracks);
    EXPECT_TRUE(state.playing);
    EXPECT_EQ(state.transportFrame, 2u);
    EXPECT_FALSE(state.recordPending);
    EXPECT_FALSE(state.recording);
}


TEST(RackGraphTransportTest, AudioCallbackDoesNotAllocateForSupportedQuantum) {
    RackGraph graph;
    configure(graph);
    const RackPathId track = graph.getTracks().front().id;
    ASSERT_TRUE(graph.setTrackInputArmed(track, true));

    StereoBuffers buffers;
    buffers.left.fill(0.25f);
    buffers.right.fill(-0.5f);
    graph.setTransportPlaying(true);
    allocation_probe::allocations = 0;
    allocation_probe::enabled = true;
    graph.process(buffers.inputs, 2, buffers.outputs, 64);
    allocation_probe::enabled = false;

    EXPECT_EQ(allocation_probe::allocations, 0u);
    for (uint32_t frame = 0; frame < 64; ++frame) {
        EXPECT_FLOAT_EQ(buffers.outputLeft[frame], 0.25f) << "frame " << frame;
        EXPECT_FLOAT_EQ(buffers.outputRight[frame], 0.25f) << "frame " << frame;
    }
}

TEST(RackGraphTransportTest, RejectsInvalidLoopRecordingRequests) {
    struct InvalidRequest {
        const char* name;
        bool armed;
        bool looping;
        uint32_t bars;
        bool attachClip;
    };
    const std::array<InvalidRequest, 6> cases = {{
        {"not armed", false, true, 1, false},
        {"looping disabled", true, false, 1, false},
        {"zero bars", true, true, 0, false},
        {"three bars", true, true, 3, false},
        {"too many bars", true, true, 32, false},
        {"existing clip", true, true, 1, true},
    }};

    for (const auto& testCase : cases) {
        SCOPED_TRACE(testCase.name);
        RackGraph graph;
        configure(graph);
        const RackPathId track = graph.getTracks().front().id;
        ASSERT_TRUE(graph.setTrackInputArmed(track, testCase.armed));
        ASSERT_TRUE(graph.setTrackTransportLooping(track, testCase.looping));
        if (testCase.attachClip) {
            ASSERT_TRUE(graph.attachTrackWav(track, makeClip({9.0f, 8.0f}, 60)));
        }

        EXPECT_FALSE(graph.startTrackLoopRecording(
            track, testCase.bars, guitarrackcraft::LaunchQuantization::Quarter, false));
        std::vector<guitarrackcraft::TrackSnapshot> tracks;
        const auto& state = trackSnapshot(graph, track, tracks);
        EXPECT_EQ(state.wavLoaded, testCase.attachClip);
        EXPECT_FALSE(state.recordPending);
        EXPECT_FALSE(state.recording);
    }
}

TEST(RackGraphTransportTest, LoopRecordingStartsAtStrictNextQuarterBoundary) {
    RackGraph graph;
    configure(graph);
    const RackPathId track = graph.getTracks().front().id;
    ASSERT_TRUE(graph.setTrackInputArmed(track, true));
    ASSERT_TRUE(graph.setTrackTransportLooping(track, true));
    ASSERT_TRUE(graph.setTransportPlaying(true));

    StereoBuffers buffers;
    clearBuffers(buffers);
    graph.process(buffers.inputs, 2, buffers.outputs, 3);
    ASSERT_TRUE(graph.startTrackLoopRecording(
        track, 1, guitarrackcraft::LaunchQuantization::Quarter, false));

    clearBuffers(buffers);
    graph.process(buffers.inputs, 2, buffers.outputs, 56);
    expectSilence(buffers, 56);
    std::vector<guitarrackcraft::TrackSnapshot> tracks;
    const auto& pending = trackSnapshot(graph, track, tracks);
    EXPECT_FALSE(pending.wavLoaded);
    EXPECT_TRUE(pending.recordPending);
    EXPECT_FALSE(pending.recording);

    // The request at global frame 3 must not start at frame 59.
    clearBuffers(buffers);
    graph.process(buffers.inputs, 2, buffers.outputs, 1);
    expectSilence(buffers, 1);
    const auto& stillPending = trackSnapshot(graph, track, tracks);
    EXPECT_TRUE(stillPending.recordPending);
    EXPECT_FALSE(stillPending.recording);

    // Frame 60 is the strict next quarter boundary.
    clearBuffers(buffers);
    buffers.left[0] = 0.25f;
    buffers.right[0] = 0.25f;
    graph.process(buffers.inputs, 2, buffers.outputs, 1);
    EXPECT_FLOAT_EQ(buffers.outputLeft[0], 0.25f);
    EXPECT_FLOAT_EQ(buffers.outputRight[0], 0.25f);
    const auto& recording = trackSnapshot(graph, track, tracks);
    EXPECT_FALSE(recording.wavLoaded);
    EXPECT_FALSE(recording.recordPending);
    EXPECT_TRUE(recording.recording);
}
TEST(RackGraphTransportTest, NoneStartsLoopRecordingAtCurrentTransportFrame) {
    RackGraph graph;
    configure(graph);
    const RackPathId track = graph.getTracks().front().id;
    ASSERT_TRUE(graph.setTrackInputArmed(track, true));
    ASSERT_TRUE(graph.setTrackTransportLooping(track, true));
    ASSERT_TRUE(graph.setTransportPlaying(true));

    StereoBuffers buffers;
    clearBuffers(buffers);
    graph.process(buffers.inputs, 2, buffers.outputs, 3);
    ASSERT_EQ(graph.getTransportSnapshot().transportFrame, 3u);
    ASSERT_TRUE(graph.startTrackLoopRecording(
        track, 1, guitarrackcraft::LaunchQuantization::None, false));

    clearBuffers(buffers);
    buffers.left[0] = 0.25f;
    buffers.left[1] = 0.5f;
    buffers.right[0] = 0.25f;
    buffers.right[1] = 0.5f;
    graph.process(buffers.inputs, 2, buffers.outputs, 2);

    EXPECT_FLOAT_EQ(buffers.outputLeft[0], 0.25f);
    EXPECT_FLOAT_EQ(buffers.outputLeft[1], 0.5f);
    EXPECT_FLOAT_EQ(buffers.outputRight[0], 0.25f);
    EXPECT_FLOAT_EQ(buffers.outputRight[1], 0.5f);
    std::vector<guitarrackcraft::TrackSnapshot> tracks;
    const auto& recording = trackSnapshot(graph, track, tracks);
    EXPECT_FALSE(recording.wavLoaded);
    EXPECT_FALSE(recording.recordPending);
    EXPECT_TRUE(recording.recording);
}


TEST(RackGraphTransportTest, ClipRecordingKeepsRequestedSlotAcrossSelectionAndCompletion) {
    RackGraph graph;
    configure(graph);
    const RackPathId track = graph.getTracks().front().id;
    ASSERT_TRUE(graph.setTrackInputArmed(track, true));
    ASSERT_TRUE(graph.setTransportPlaying(true));

    StereoBuffers buffers;
    clearBuffers(buffers);
    graph.process(buffers.inputs, 2, buffers.outputs, 1);
    ASSERT_TRUE(graph.startTrackClipRecording(
        track, 2, 0.25, guitarrackcraft::LaunchQuantization::None, false));

    std::vector<TrackClipSlotInfo> slots = graph.getTrackClipSlots(track);
    ASSERT_GE(slots.size(), 3u);
    const auto* reserved = findClipSlot(slots, 2);
    ASSERT_NE(reserved, nullptr);
    EXPECT_TRUE(reserved->wavLoaded);
    EXPECT_FALSE(reserved->midiLoaded);
    EXPECT_TRUE(reserved->active);
    EXPECT_DOUBLE_EQ(reserved->durationSec, 1.0);

    std::vector<guitarrackcraft::TrackSnapshot> tracks;
    const auto& pending = trackSnapshot(graph, track, tracks);
    EXPECT_TRUE(pending.wavLoaded);
    EXPECT_TRUE(pending.recordPending);
    EXPECT_FALSE(pending.recording);

    // The first frame starts the immediate recording and is written to slot 2.
    clearBuffers(buffers);
    buffers.left[0] = 1.0f;
    buffers.right[0] = 1.0f;
    graph.process(buffers.inputs, 2, buffers.outputs, 1);
    EXPECT_TRUE(trackSnapshot(graph, track, tracks).recording);

    // Changing the selected slot must not redirect the in-flight recording.
    ASSERT_TRUE(graph.selectTrackClipSlot(track, 0));
    slots = graph.getTrackClipSlots(track);
    reserved = findClipSlot(slots, 2);
    const auto* selected = findClipSlot(slots, 0);
    ASSERT_NE(reserved, nullptr);
    ASSERT_NE(selected, nullptr);
    EXPECT_TRUE(reserved->wavLoaded);
    EXPECT_FALSE(reserved->active);
    EXPECT_TRUE(selected->active);
    EXPECT_FALSE(selected->wavLoaded);

    clearBuffers(buffers);
    for (uint32_t frame = 0; frame < 59; ++frame) {
        const float sample = 2.0f + static_cast<float>(frame);
        buffers.left[frame] = sample;
        buffers.right[frame] = sample;
    }
    graph.process(buffers.inputs, 2, buffers.outputs, 59);

    slots = graph.getTrackClipSlots(track);
    reserved = findClipSlot(slots, 2);
    ASSERT_NE(reserved, nullptr);
    EXPECT_TRUE(reserved->wavLoaded);
    EXPECT_FALSE(reserved->active);
    EXPECT_DOUBLE_EQ(reserved->durationSec, 1.0);
    const auto& completedOnOtherSlot = trackSnapshot(graph, track, tracks);
    EXPECT_FALSE(completedOnOtherSlot.recordPending);
    EXPECT_FALSE(completedOnOtherSlot.recording);

    ASSERT_TRUE(graph.selectTrackClipSlot(track, 2));
    slots = graph.getTrackClipSlots(track);
    reserved = findClipSlot(slots, 2);
    ASSERT_NE(reserved, nullptr);
    EXPECT_TRUE(reserved->wavLoaded);
    EXPECT_TRUE(reserved->active);
    const auto& completed = trackSnapshot(graph, track, tracks);
    EXPECT_TRUE(completed.wavLoaded);
    EXPECT_TRUE(completed.looping);

    // The captured samples are still observable through the requested slot.
    const auto peaks = graph.getTrackWaveformPeaks(track, 4);
    ASSERT_EQ(peaks.size(), 4u);
    EXPECT_FLOAT_EQ(peaks[0], 15.0f);
    EXPECT_FLOAT_EQ(peaks[1], 30.0f);
    EXPECT_FLOAT_EQ(peaks[2], 45.0f);
    EXPECT_FLOAT_EQ(peaks[3], 60.0f);
}

TEST(RackGraphTransportTest, CancelClipRecordingRemovesReservedSlot) {
    RackGraph graph;
    configure(graph);
    const RackPathId track = graph.getTracks().front().id;
    ASSERT_TRUE(graph.setTrackInputArmed(track, true));
    ASSERT_TRUE(graph.startTrackClipRecording(
        track, 3, 0.25, guitarrackcraft::LaunchQuantization::None, false));

    std::vector<TrackClipSlotInfo> slots = graph.getTrackClipSlots(track);
    ASSERT_GE(slots.size(), 4u);
    const auto* reserved = findClipSlot(slots, 3);
    ASSERT_NE(reserved, nullptr);
    EXPECT_TRUE(reserved->wavLoaded);
    EXPECT_DOUBLE_EQ(reserved->durationSec, 1.0);

    std::vector<guitarrackcraft::TrackSnapshot> tracks;
    const auto& armed = trackSnapshot(graph, track, tracks);
    EXPECT_TRUE(armed.wavLoaded);
    EXPECT_TRUE(armed.recordPending);
    EXPECT_FALSE(armed.recording);

    ASSERT_TRUE(graph.cancelTrackLoopRecording(track));

    slots = graph.getTrackClipSlots(track);
    reserved = findClipSlot(slots, 3);
    ASSERT_NE(reserved, nullptr);
    EXPECT_FALSE(reserved->wavLoaded);
    EXPECT_FALSE(reserved->midiLoaded);
    EXPECT_TRUE(reserved->displayName.empty());
    EXPECT_DOUBLE_EQ(reserved->durationSec, 0.0);

    const auto& cancelled = trackSnapshot(graph, track, tracks);
    EXPECT_FALSE(cancelled.wavLoaded);
    EXPECT_FALSE(cancelled.recordPending);
    EXPECT_FALSE(cancelled.recording);
    EXPECT_FALSE(cancelled.punchArmed);
    EXPECT_FALSE(graph.cancelTrackLoopRecording(track));
}

TEST(RackGraphTransportTest, LoopRecordingCapturesOneBarMonitorsAndLoopsImmediately) {
    RackGraph graph;
    configure(graph);
    const RackPathId track = graph.getTracks().front().id;
    ASSERT_TRUE(graph.setTrackInputArmed(track, true));
    ASSERT_TRUE(graph.setTrackTransportLooping(track, true));
    ASSERT_TRUE(graph.setTransportPlaying(true));

    StereoBuffers buffers;
    clearBuffers(buffers);
    graph.process(buffers.inputs, 2, buffers.outputs, 3);
    ASSERT_TRUE(graph.startTrackLoopRecording(
        track, 1, guitarrackcraft::LaunchQuantization::Bar, false));

    clearBuffers(buffers);
    graph.process(buffers.inputs, 2, buffers.outputs, 237);
    expectSilence(buffers, 237);

    for (uint32_t frame = 0; frame < 240; ++frame) {
        buffers.left[frame] = 1000.0f + static_cast<float>(frame);
        buffers.right[frame] = 1000.0f + static_cast<float>(frame);
    }
    graph.process(buffers.inputs, 2, buffers.outputs, 240);
    for (uint32_t frame = 0; frame < 240; ++frame) {
        EXPECT_FLOAT_EQ(buffers.outputLeft[frame], 1000.0f + static_cast<float>(frame))
            << "frame " << frame;
        EXPECT_FLOAT_EQ(buffers.outputRight[frame], 1000.0f + static_cast<float>(frame))
            << "frame " << frame;
    }

    std::vector<guitarrackcraft::TrackSnapshot> tracks;
    const auto& completed = trackSnapshot(graph, track, tracks);
    EXPECT_TRUE(completed.wavLoaded);
    EXPECT_FALSE(completed.recordPending);
    EXPECT_FALSE(completed.recording);
    EXPECT_TRUE(completed.playing);
    EXPECT_TRUE(completed.looping);
    EXPECT_DOUBLE_EQ(completed.wavDurationSec, 4.0);

    clearBuffers(buffers);
    buffers.left[0] = 77.0f;
    buffers.right[0] = 77.0f;
    graph.process(buffers.inputs, 2, buffers.outputs, 1);
    EXPECT_FLOAT_EQ(buffers.outputLeft[0], 1000.0f);
    EXPECT_FLOAT_EQ(buffers.outputRight[0], 1000.0f);
    const auto& looping = trackSnapshot(graph, track, tracks);
    EXPECT_EQ(looping.transportFrame, 1u);
}

TEST(RackGraphTransportTest, GlobalStopCancelsPendingAndInProgressLoopRecording) {
    {
        RackGraph graph;
        configure(graph);
        const RackPathId track = graph.getTracks().front().id;
        ASSERT_TRUE(graph.setTrackInputArmed(track, true));
        ASSERT_TRUE(graph.setTrackTransportLooping(track, true));
        ASSERT_TRUE(graph.setTransportPlaying(true));
        ASSERT_TRUE(graph.startTrackLoopRecording(
            track, 1, guitarrackcraft::LaunchQuantization::Quarter, false));

        StereoBuffers buffers;
        clearBuffers(buffers);
        graph.process(buffers.inputs, 2, buffers.outputs, 10);
        std::vector<guitarrackcraft::TrackSnapshot> tracks;
        EXPECT_TRUE(trackSnapshot(graph, track, tracks).recordPending);

        graph.setTransportPlaying(false);
        clearBuffers(buffers);
        graph.process(buffers.inputs, 2, buffers.outputs, 1);
        const auto& stopped = trackSnapshot(graph, track, tracks);
        EXPECT_FALSE(stopped.wavLoaded);
        EXPECT_FALSE(stopped.recordPending);
        EXPECT_FALSE(stopped.recording);
    }

    {
        RackGraph graph;
        configure(graph);
        const RackPathId track = graph.getTracks().front().id;
        ASSERT_TRUE(graph.setTrackInputArmed(track, true));
        ASSERT_TRUE(graph.setTrackTransportLooping(track, true));
        ASSERT_TRUE(graph.setTransportPlaying(true));
        ASSERT_TRUE(graph.startTrackLoopRecording(
            track, 1, guitarrackcraft::LaunchQuantization::Quarter, false));

        StereoBuffers buffers;
        clearBuffers(buffers);
        graph.process(buffers.inputs, 2, buffers.outputs, 61);
        std::vector<guitarrackcraft::TrackSnapshot> tracks;
        EXPECT_TRUE(trackSnapshot(graph, track, tracks).recording);

        graph.setTransportPlaying(false);
        clearBuffers(buffers);
        graph.process(buffers.inputs, 2, buffers.outputs, 1);
        const auto& stopped = trackSnapshot(graph, track, tracks);
        EXPECT_FALSE(stopped.wavLoaded);
        EXPECT_FALSE(stopped.recordPending);
        EXPECT_FALSE(stopped.recording);
    }
}

TEST(RackGraphTransportTest, LoopRecordingAudioCallbackDoesNotAllocate) {
    RackGraph graph;
    configure(graph);
    const RackPathId track = graph.getTracks().front().id;
    ASSERT_TRUE(graph.setTrackInputArmed(track, true));
    ASSERT_TRUE(graph.setTrackTransportLooping(track, true));
    ASSERT_TRUE(graph.setTransportPlaying(true));
    ASSERT_TRUE(graph.startTrackLoopRecording(
        track, 1, guitarrackcraft::LaunchQuantization::Sixteenth, false));

    StereoBuffers buffers;
    clearBuffers(buffers);
    graph.process(buffers.inputs, 2, buffers.outputs, 15);
    buffers.left.fill(0.25f);
    buffers.right.fill(-0.5f);
    allocation_probe::allocations = 0;
    allocation_probe::enabled = true;
    graph.process(buffers.inputs, 2, buffers.outputs, 64);
    allocation_probe::enabled = false;

    EXPECT_EQ(allocation_probe::allocations, 0u);
    for (uint32_t frame = 0; frame < 64; ++frame) {
        EXPECT_FLOAT_EQ(buffers.outputLeft[frame], 0.25f) << "frame " << frame;
        EXPECT_FLOAT_EQ(buffers.outputRight[frame], 0.25f) << "frame " << frame;
    }
}

TEST(RackGraphTransportTest, CompletedLoopSurvivesGlobalStopAndExplicitRelaunch) {
    RackGraph graph;
    configure(graph);
    const RackPathId track = graph.getTracks().front().id;
    ASSERT_TRUE(graph.setTrackInputArmed(track, true));
    ASSERT_TRUE(graph.setTrackTransportLooping(track, true));
    ASSERT_TRUE(graph.setTransportPlaying(true));
    ASSERT_TRUE(graph.startTrackLoopRecording(
        track, 1, guitarrackcraft::LaunchQuantization::Bar, false));

    StereoBuffers buffers;
    clearBuffers(buffers);
    graph.process(buffers.inputs, 2, buffers.outputs, 240);
    for (uint32_t frame = 0; frame < 240; ++frame) {
        buffers.left[frame] = 300.0f + static_cast<float>(frame);
        buffers.right[frame] = 300.0f + static_cast<float>(frame);
    }
    graph.process(buffers.inputs, 2, buffers.outputs, 240);

    std::vector<guitarrackcraft::TrackSnapshot> tracks;
    const auto& completed = trackSnapshot(graph, track, tracks);
    ASSERT_TRUE(completed.wavLoaded);
    EXPECT_FALSE(completed.recordPending);
    EXPECT_FALSE(completed.recording);

    graph.setTransportPlaying(false);
    clearBuffers(buffers);
    graph.process(buffers.inputs, 2, buffers.outputs, 1);
    expectSilence(buffers, 1);
    const auto& stopped = trackSnapshot(graph, track, tracks);
    EXPECT_TRUE(stopped.wavLoaded);
    EXPECT_FALSE(stopped.playing);

    graph.setTransportPlaying(true);
    clearBuffers(buffers);
    graph.process(buffers.inputs, 2, buffers.outputs, 1);
    expectSilence(buffers, 1);
    const auto& resumed = trackSnapshot(graph, track, tracks);
    EXPECT_TRUE(resumed.wavLoaded);
    EXPECT_FALSE(resumed.playing);

    ASSERT_TRUE(graph.setTrackTransportPlaying(
        track, true, guitarrackcraft::LaunchQuantization::Sixteenth));
    clearBuffers(buffers);
    graph.process(buffers.inputs, 2, buffers.outputs, 15);
    EXPECT_FLOAT_EQ(buffers.outputLeft[14], 300.0f);
    EXPECT_FLOAT_EQ(buffers.outputRight[14], 300.0f);
    EXPECT_TRUE(trackSnapshot(graph, track, tracks).playing);
}

TEST(RackGraphTransportTest, LoopRecordingCapturesExactQuarterBarAtSixtyBpm) {
    RackGraph graph;
    configure(graph);
    const RackPathId track = graph.getTracks().front().id;
    ASSERT_TRUE(graph.setTrackInputArmed(track, true));
    ASSERT_TRUE(graph.setTrackTransportLooping(track, true));
    ASSERT_TRUE(graph.setTransportPlaying(true));

    StereoBuffers buffers;
    clearBuffers(buffers);
    ASSERT_TRUE(graph.startTrackLoopRecording(
        track, 0.25, guitarrackcraft::LaunchQuantization::Sixteenth, false));
    graph.process(buffers.inputs, 2, buffers.outputs, 15);
    std::vector<guitarrackcraft::TrackSnapshot> tracks;
    EXPECT_TRUE(trackSnapshot(graph, track, tracks).recordPending);

    for (uint32_t frame = 0; frame < 60; ++frame) {
        buffers.left[frame] = 100.0f + static_cast<float>(frame);
        buffers.right[frame] = 100.0f + static_cast<float>(frame);
    }
    graph.process(buffers.inputs, 2, buffers.outputs, 60);

    const auto& completed = trackSnapshot(graph, track, tracks);
    ASSERT_TRUE(completed.wavLoaded);
    EXPECT_FALSE(completed.recordPending);
    EXPECT_FALSE(completed.recording);
    EXPECT_DOUBLE_EQ(completed.wavDurationSec, 1.0);
    EXPECT_FLOAT_EQ(buffers.outputLeft[59], 159.0f);
    EXPECT_FLOAT_EQ(buffers.outputRight[59], 159.0f);

    clearBuffers(buffers);
    graph.process(buffers.inputs, 2, buffers.outputs, 1);
    EXPECT_FLOAT_EQ(buffers.outputLeft[0], 100.0f);
    EXPECT_FLOAT_EQ(buffers.outputRight[0], 100.0f);
}

TEST(RackGraphTransportTest, PunchRecordingRejectsPlayingAndInvalidPrerequisites) {
    struct PunchCase {
        const char* name;
        bool inputArmed;
        bool looping;
        bool globalPlaying;
        bool attachClip;
    };
    const std::array<PunchCase, 4> cases = {{
        {"global playing", true, true, true, false},
        {"input not armed", false, true, false, false},
        {"looping disabled", true, false, false, false},
        {"existing clip", true, true, false, true},
    }};

    for (const auto& testCase : cases) {
        SCOPED_TRACE(testCase.name);
        RackGraph graph;
        configure(graph);
        const RackPathId track = graph.getTracks().front().id;
        ASSERT_TRUE(graph.setTrackInputArmed(track, testCase.inputArmed));
        ASSERT_TRUE(graph.setTrackTransportLooping(track, testCase.looping));
        if (testCase.attachClip) {
            ASSERT_TRUE(graph.attachTrackWav(track, makeClip({9.0f, 8.0f}, 60)));
        }
        StereoBuffers buffers;
        clearBuffers(buffers);
        if (testCase.globalPlaying) {
            ASSERT_TRUE(graph.setTransportPlaying(true));
            graph.process(buffers.inputs, 2, buffers.outputs, 1);
        }

        EXPECT_FALSE(graph.startTrackLoopRecording(
            track, 0.25, guitarrackcraft::LaunchQuantization::Quarter, true));
        std::vector<guitarrackcraft::TrackSnapshot> tracks;
        const auto& state = trackSnapshot(graph, track, tracks);
        EXPECT_EQ(state.wavLoaded, testCase.attachClip);
        EXPECT_FALSE(state.recordPending);
        EXPECT_FALSE(state.recording);
        EXPECT_FALSE(state.punchArmed);
    }
}

TEST(RackGraphTransportTest, PunchImmediateLaunchCanArmWhilePlaying) {
    struct QuantizationCase {
        const char* name;
        guitarrackcraft::LaunchQuantization quantization;
        bool accepted;
    };
    const std::array<QuantizationCase, 5> cases = {{
        {"none", guitarrackcraft::LaunchQuantization::None, true},
        {"bar", guitarrackcraft::LaunchQuantization::Bar, false},
        {"quarter", guitarrackcraft::LaunchQuantization::Quarter, false},
        {"eighth", guitarrackcraft::LaunchQuantization::Eighth, false},
        {"sixteenth", guitarrackcraft::LaunchQuantization::Sixteenth, false},
    }};

    for (const auto& testCase : cases) {
        SCOPED_TRACE(testCase.name);
        RackGraph graph;
        configure(graph);
        const RackPathId track = graph.getTracks().front().id;
        ASSERT_TRUE(graph.setTrackInputArmed(track, true));
        ASSERT_TRUE(graph.setTrackTransportLooping(track, true));

        StereoBuffers buffers;
        clearBuffers(buffers);
        ASSERT_TRUE(graph.setTransportPlaying(true));
        graph.process(buffers.inputs, 2, buffers.outputs, 1);
        ASSERT_TRUE(graph.getTransportSnapshot().playing);

        EXPECT_EQ(graph.startTrackLoopRecording(
                      track, 0.25, testCase.quantization, true),
                  testCase.accepted);
        std::vector<guitarrackcraft::TrackSnapshot> tracks;
        const auto& armed = trackSnapshot(graph, track, tracks);
        EXPECT_EQ(armed.punchArmed, testCase.accepted);
        EXPECT_FALSE(armed.recordPending);
        EXPECT_FALSE(armed.recording);
        EXPECT_TRUE(graph.getTransportSnapshot().playing);

        if (testCase.quantization == guitarrackcraft::LaunchQuantization::None) {
            // Immediate punch keeps the already-running global transport live.
            clearBuffers(buffers);
            graph.process(buffers.inputs, 2, buffers.outputs, 1);
            const auto whileArmed = graph.getTransportSnapshot();
            EXPECT_TRUE(whileArmed.playing);
            EXPECT_GT(whileArmed.transportFrame, 1u);
            buffers.left[0] = 0.08f;
            buffers.right[0] = -0.08f;
            graph.process(buffers.inputs, 2, buffers.outputs, 1);

            const auto& recording = trackSnapshot(graph, track, tracks);
            EXPECT_FALSE(recording.punchArmed);
            EXPECT_TRUE(recording.recording);
            EXPECT_TRUE(graph.getTransportSnapshot().playing);
        }
    }
}

TEST(RackGraphTransportTest, PunchIgnoresSubThresholdInputWhilePaused) {
    RackGraph graph;
    configure(graph);
    const RackPathId track = graph.getTracks().front().id;
    ASSERT_TRUE(graph.setTrackInputArmed(track, true));
    ASSERT_TRUE(graph.setTrackTransportLooping(track, true));
    ASSERT_TRUE(graph.startTrackLoopRecording(
        track, 0.25, guitarrackcraft::LaunchQuantization::Quarter, true));

    std::vector<guitarrackcraft::TrackSnapshot> tracks;
    ASSERT_TRUE(trackSnapshot(graph, track, tracks).punchArmed);
    const auto before = graph.getTransportSnapshot();
    StereoBuffers buffers;
    clearBuffers(buffers);
    buffers.left[0] = 0.0199f;
    buffers.right[0] = -0.0199f;
    for (uint32_t frame = 0; frame < 3; ++frame) {
        graph.process(buffers.inputs, 2, buffers.outputs, 1);
        const auto& state = trackSnapshot(graph, track, tracks);
        EXPECT_TRUE(state.punchArmed);
        EXPECT_FALSE(state.recording);
        EXPECT_FALSE(state.wavLoaded);
    }
    const auto after = graph.getTransportSnapshot();
    EXPECT_FALSE(after.playing);
    EXPECT_EQ(after.transportFrame, before.transportFrame);
}

TEST(RackGraphTransportTest, PunchCalibratesSteadyNoiseBeforeTransient) {
    RackGraph graph;
    graph.setSampleRate(1000.0f, 512);
    graph.setBeatsPerMinute(60.0);
    graph.setAvailableInputChannelCount(2);
    const RackPathId track = graph.getTracks().front().id;
    ASSERT_TRUE(graph.setTrackInputArmed(track, true));
    ASSERT_TRUE(graph.setTrackTransportLooping(track, true));
    ASSERT_TRUE(graph.startTrackLoopRecording(
        track, 0.25, guitarrackcraft::LaunchQuantization::Quarter, true));

    std::vector<guitarrackcraft::TrackSnapshot> tracks;
    ASSERT_TRUE(trackSnapshot(graph, track, tracks).punchArmed);
    const auto paused = graph.getTransportSnapshot();
    EXPECT_FALSE(paused.playing);

    StereoBuffers buffers;
    clearBuffers(buffers);
    for (uint32_t frame = 0; frame < 20; ++frame) {
        buffers.left[frame] = 0.025f;
        buffers.right[frame] = 0.025f;
    }
    graph.process(buffers.inputs, 2, buffers.outputs, 20);

    const auto& stillArmed = trackSnapshot(graph, track, tracks);
    EXPECT_TRUE(stillArmed.punchArmed);
    EXPECT_FALSE(stillArmed.recording);
    EXPECT_FALSE(stillArmed.wavLoaded);
    const auto stillPaused = graph.getTransportSnapshot();
    EXPECT_FALSE(stillPaused.playing);
    EXPECT_EQ(stillPaused.transportFrame, paused.transportFrame);

    clearBuffers(buffers);
    buffers.left[0] = 0.08f;
    buffers.right[0] = 0.08f;
    graph.process(buffers.inputs, 2, buffers.outputs, 1);

    const auto& recording = trackSnapshot(graph, track, tracks);
    EXPECT_FALSE(recording.punchArmed);
    EXPECT_TRUE(recording.recording);
    const auto resumed = graph.getTransportSnapshot();
    EXPECT_TRUE(resumed.playing);

    uint32_t remaining = 999;
    while (remaining != 0) {
        const uint32_t chunk = remaining < 512u ? remaining : 512u;
        clearBuffers(buffers);
        graph.process(buffers.inputs, 2, buffers.outputs, chunk);
        remaining -= chunk;
    }

    const auto& completed = trackSnapshot(graph, track, tracks);
    ASSERT_TRUE(completed.wavLoaded);
    EXPECT_FALSE(completed.recording);
    EXPECT_FALSE(completed.punchArmed);
    EXPECT_DOUBLE_EQ(completed.wavDurationSec, 1.0);

    clearBuffers(buffers);
    graph.process(buffers.inputs, 2, buffers.outputs, 1);
    EXPECT_FLOAT_EQ(buffers.outputLeft[0], 0.08f);
    EXPECT_FLOAT_EQ(buffers.outputRight[0], 0.08f);
}

TEST(RackGraphTransportTest, PunchCapturesThresholdSampleAsFrameZeroAndResumesClock) {
    RackGraph graph;
    configure(graph);
    const RackPathId track = graph.getTracks().front().id;
    ASSERT_TRUE(graph.setTrackInputArmed(track, true));
    ASSERT_TRUE(graph.setTrackTransportLooping(track, true));
    ASSERT_TRUE(graph.startTrackLoopRecording(
        track, 0.25, guitarrackcraft::LaunchQuantization::Quarter, true));

    StereoBuffers buffers;
    clearBuffers(buffers);
    // At 60 Hz the detector calibrates one frame before evaluating a punch.
    buffers.left[0] = 0.0f;
    buffers.right[0] = 0.0f;
    graph.process(buffers.inputs, 2, buffers.outputs, 1);
    std::vector<guitarrackcraft::TrackSnapshot> tracks;
    ASSERT_TRUE(trackSnapshot(graph, track, tracks).punchArmed);
    const auto paused = graph.getTransportSnapshot();
    EXPECT_FALSE(paused.playing);

    constexpr float triggerLeft = 0.08f;
    constexpr float triggerRight = -0.08f;
    buffers.left[0] = triggerLeft;
    buffers.right[0] = triggerRight;
    graph.process(buffers.inputs, 2, buffers.outputs, 1);
    EXPECT_FLOAT_EQ(buffers.outputLeft[0], triggerLeft);
    EXPECT_FLOAT_EQ(buffers.outputRight[0], triggerLeft);
    const auto afterHit = graph.getTransportSnapshot();
    EXPECT_TRUE(afterHit.playing);
    EXPECT_GT(afterHit.transportFrame, paused.transportFrame);
    const auto& recording = trackSnapshot(graph, track, tracks);
    EXPECT_FALSE(recording.punchArmed);
    EXPECT_TRUE(recording.recording);

    for (uint32_t frame = 1; frame < 60; ++frame) {
        buffers.left[frame - 1] = 10.0f + static_cast<float>(frame);
        buffers.right[frame - 1] = 10.0f + static_cast<float>(frame);
    }
    graph.process(buffers.inputs, 2, buffers.outputs, 59);
    const auto& completed = trackSnapshot(graph, track, tracks);
    ASSERT_TRUE(completed.wavLoaded);
    EXPECT_FALSE(completed.recording);
    EXPECT_FALSE(completed.punchArmed);
    EXPECT_DOUBLE_EQ(completed.wavDurationSec, 1.0);

    clearBuffers(buffers);
    graph.process(buffers.inputs, 2, buffers.outputs, 1);
    EXPECT_FLOAT_EQ(buffers.outputLeft[0], triggerLeft);
    EXPECT_FLOAT_EQ(buffers.outputRight[0], triggerLeft);
}

TEST(RackGraphTransportTest, ManualGlobalPlayCancelsArmedPunch) {
    RackGraph graph;
    configure(graph);
    const RackPathId track = graph.getTracks().front().id;
    ASSERT_TRUE(graph.setTrackInputArmed(track, true));
    ASSERT_TRUE(graph.setTrackTransportLooping(track, true));
    ASSERT_TRUE(graph.startTrackLoopRecording(
        track, 0.25, guitarrackcraft::LaunchQuantization::Quarter, true));

    std::vector<guitarrackcraft::TrackSnapshot> tracks;
    ASSERT_TRUE(trackSnapshot(graph, track, tracks).punchArmed);
    ASSERT_TRUE(graph.setTransportPlaying(true));
    StereoBuffers buffers;
    clearBuffers(buffers);
    graph.process(buffers.inputs, 2, buffers.outputs, 1);

    const auto& cancelled = trackSnapshot(graph, track, tracks);
    EXPECT_FALSE(cancelled.punchArmed);
    EXPECT_FALSE(cancelled.recordPending);
    EXPECT_FALSE(cancelled.recording);
    EXPECT_FALSE(cancelled.wavLoaded);
}

TEST(RackGraphTransportTest, CancelTrackLoopRecordingDisarmsPunch) {
    RackGraph graph;
    configure(graph);
    const RackPathId track = graph.getTracks().front().id;
    ASSERT_TRUE(graph.setTrackInputArmed(track, true));
    ASSERT_TRUE(graph.setTrackTransportLooping(track, true));
    ASSERT_TRUE(graph.startTrackLoopRecording(
        track, 0.25, guitarrackcraft::LaunchQuantization::Quarter, true));
    ASSERT_TRUE(graph.cancelTrackLoopRecording(track));

    std::vector<guitarrackcraft::TrackSnapshot> tracks;
    const auto& cancelled = trackSnapshot(graph, track, tracks);
    EXPECT_FALSE(cancelled.punchArmed);
    EXPECT_FALSE(cancelled.recordPending);
    EXPECT_FALSE(cancelled.recording);
    EXPECT_FALSE(cancelled.wavLoaded);
    EXPECT_FALSE(graph.cancelTrackLoopRecording(track));
    EXPECT_FALSE(graph.cancelTrackLoopRecording(9999));
}

TEST(RackGraphTransportTest, PunchAudioCallbackDoesNotAllocate) {
    RackGraph graph;
    configure(graph);
    const RackPathId track = graph.getTracks().front().id;
    ASSERT_TRUE(graph.setTrackInputArmed(track, true));
    ASSERT_TRUE(graph.setTrackTransportLooping(track, true));
    ASSERT_TRUE(graph.startTrackLoopRecording(
        track, 0.25, guitarrackcraft::LaunchQuantization::Quarter, true));

    StereoBuffers buffers;
    clearBuffers(buffers);
    // At 60 Hz the detector calibrates one frame before evaluating a punch.
    allocation_probe::allocations = 0;
    allocation_probe::enabled = true;
    graph.process(buffers.inputs, 2, buffers.outputs, 1);

    clearBuffers(buffers);
    buffers.left[0] = 0.08f;
    buffers.right[0] = -0.08f;
    for (uint32_t frame = 1; frame < 60; ++frame) {
        buffers.left[frame] = 0.1f;
        buffers.right[frame] = -0.1f;
    }
    graph.process(buffers.inputs, 2, buffers.outputs, 64);
    allocation_probe::enabled = false;

    EXPECT_EQ(allocation_probe::allocations, 0u);
    std::vector<guitarrackcraft::TrackSnapshot> tracks;
    const auto& completed = trackSnapshot(graph, track, tracks);
    EXPECT_TRUE(completed.wavLoaded);
    EXPECT_FALSE(completed.punchArmed);
    EXPECT_FLOAT_EQ(buffers.outputLeft[0], 0.08f);
    EXPECT_FLOAT_EQ(buffers.outputRight[0], 0.08f);
    EXPECT_FLOAT_EQ(buffers.outputLeft[60], 0.08f);
    EXPECT_FLOAT_EQ(buffers.outputRight[60], 0.08f);
}
TEST(RackGraphInputChannelTest, AllowsPreSessionSelectionAndClampsOnNegotiation) {
    RackGraph graph;
    const RackPathId track = graph.getTracks().front().id;
    std::vector<guitarrackcraft::TrackSnapshot> tracks;

    EXPECT_EQ(trackSnapshot(graph, track, tracks).inputChannel, 0);
    EXPECT_TRUE(graph.setTrackInputChannel(track, 1));
    EXPECT_EQ(trackSnapshot(graph, track, tracks).inputChannel, 1);
    EXPECT_FALSE(graph.setTrackInputChannel(track, -1));
    EXPECT_FALSE(graph.setTrackInputChannel(
        track, monotrypt::usb::kMaxTransportChannels));
    EXPECT_EQ(trackSnapshot(graph, track, tracks).inputChannel, 1);

    graph.setAvailableInputChannelCount(1);
    EXPECT_EQ(trackSnapshot(graph, track, tracks).inputChannel, 0);
}
TEST(RackGraphInputChannelTest, SetterRejectsInvalidAndClampsWhenAvailableCountShrinks) {
    RackGraph graph;
    configure(graph);
    const RackPathId track = graph.getTracks().front().id;
    std::vector<guitarrackcraft::TrackSnapshot> tracks;
    EXPECT_EQ(trackSnapshot(graph, track, tracks).inputChannel, 0);
    EXPECT_TRUE(graph.setTrackInputChannel(track, 1));
    EXPECT_EQ(trackSnapshot(graph, track, tracks).inputChannel, 1);
    EXPECT_FALSE(graph.setTrackInputChannel(track, -1));
    EXPECT_FALSE(graph.setTrackInputChannel(track, 2));
    EXPECT_EQ(trackSnapshot(graph, track, tracks).inputChannel, 1);

    graph.setAvailableInputChannelCount(1);
    EXPECT_EQ(trackSnapshot(graph, track, tracks).inputChannel, 0);
    EXPECT_FALSE(graph.setTrackInputChannel(track, 1));
    EXPECT_TRUE(graph.setTrackInputChannel(track, 0));
}

TEST(RackGraphInputChannelTest, ArmedTracksSelectDifferentChannelsForMonitoring) {
    RackGraph graph;
    configure(graph);
    const RackPathId first = graph.getTracks().front().id;
    const RackPathId second = graph.addTrack();
    ASSERT_TRUE(graph.setTrackInputArmed(first, true));
    ASSERT_TRUE(graph.setTrackInputArmed(second, true));
    ASSERT_TRUE(graph.setTrackInputChannel(second, 1));

    StereoBuffers buffers;
    clearBuffers(buffers);
    buffers.left[0] = 0.25f;
    buffers.right[0] = -0.5f;
    graph.process(buffers.inputs, 2, buffers.outputs, 1);
    EXPECT_FLOAT_EQ(buffers.outputLeft[0], -0.25f);
    EXPECT_FLOAT_EQ(buffers.outputRight[0], -0.25f);
}

TEST(RackGraphInputChannelTest, LoopRecordingUsesEachTracksSelectedChannel) {
    RackGraph graph;
    configure(graph);
    const RackPathId first = graph.getTracks().front().id;
    const RackPathId second = graph.addTrack();
    ASSERT_TRUE(graph.setTrackInputArmed(first, true));
    ASSERT_TRUE(graph.setTrackInputArmed(second, true));
    ASSERT_TRUE(graph.setTrackTransportLooping(first, true));
    ASSERT_TRUE(graph.setTrackTransportLooping(second, true));
    ASSERT_TRUE(graph.setTrackInputChannel(second, 1));
    ASSERT_TRUE(graph.setTransportPlaying(true));
    ASSERT_TRUE(graph.startTrackLoopRecording(
        first, 0.25, guitarrackcraft::LaunchQuantization::Sixteenth, false));
    ASSERT_TRUE(graph.startTrackLoopRecording(
        second, 0.25, guitarrackcraft::LaunchQuantization::Sixteenth, false));

    StereoBuffers buffers;
    clearBuffers(buffers);
    graph.process(buffers.inputs, 2, buffers.outputs, 15);
    for (uint32_t frame = 0; frame < 60; ++frame) {
        buffers.left[frame] = 1.0f;
        buffers.right[frame] = 10.0f;
    }
    graph.process(buffers.inputs, 2, buffers.outputs, 60);
    clearBuffers(buffers);
    graph.process(buffers.inputs, 2, buffers.outputs, 1);
    EXPECT_FLOAT_EQ(buffers.outputLeft[0], 11.0f);
    EXPECT_FLOAT_EQ(buffers.outputRight[0], 11.0f);
}

TEST(RackGraphInputChannelTest, PunchUsesSelectedChannelPerTrack) {
    RackGraph graph;
    configure(graph);
    const RackPathId first = graph.getTracks().front().id;
    const RackPathId second = graph.addTrack();
    ASSERT_TRUE(graph.setTrackInputArmed(first, true));
    ASSERT_TRUE(graph.setTrackInputArmed(second, true));
    ASSERT_TRUE(graph.setTrackTransportLooping(first, true));
    ASSERT_TRUE(graph.setTrackTransportLooping(second, true));
    ASSERT_TRUE(graph.setTrackInputChannel(second, 1));
    ASSERT_TRUE(graph.startTrackLoopRecording(
        first, 0.25, guitarrackcraft::LaunchQuantization::Quarter, true));
    ASSERT_TRUE(graph.startTrackLoopRecording(
        second, 0.25, guitarrackcraft::LaunchQuantization::Quarter, true));
    StereoBuffers buffers;
    clearBuffers(buffers);
    // Each track calibrates the channel it selected (left for first, right for
    // second) before transient detection begins.
    buffers.left[0] = 0.01f;
    buffers.right[0] = 0.03f;
    graph.process(buffers.inputs, 2, buffers.outputs, 1);
    std::vector<guitarrackcraft::TrackSnapshot> tracks;
    EXPECT_TRUE(trackSnapshot(graph, first, tracks).punchArmed);
    EXPECT_TRUE(trackSnapshot(graph, second, tracks).punchArmed);

    clearBuffers(buffers);
    buffers.left[0] = 0.01f;
    buffers.right[0] = 0.08f;
    graph.process(buffers.inputs, 2, buffers.outputs, 1);
    EXPECT_TRUE(trackSnapshot(graph, first, tracks).punchArmed);
    EXPECT_FALSE(trackSnapshot(graph, first, tracks).recording);
    EXPECT_FALSE(trackSnapshot(graph, second, tracks).punchArmed);
    EXPECT_TRUE(trackSnapshot(graph, second, tracks).recording);
}
TEST(RackGraphInputArmingTest, ExclusiveSelectionLeavesOnlySelectedTrackArmed) {
    RackGraph graph;
    configure(graph);
    const RackPathId first = graph.getTracks().front().id;
    const RackPathId second = graph.addTrack();
    const RackPathId third = graph.addTrack();

    ASSERT_TRUE(graph.setTrackInputArmed(first, true));
    ASSERT_TRUE(graph.setTrackInputArmed(second, true));
    ASSERT_TRUE(graph.setTrackInputArmed(third, true));

    ASSERT_TRUE(graph.setTrackInputArmedExclusive(second));

    std::vector<guitarrackcraft::TrackSnapshot> tracks;
    EXPECT_FALSE(trackSnapshot(graph, first, tracks).inputArmed);
    EXPECT_TRUE(trackSnapshot(graph, second, tracks).inputArmed);
    EXPECT_FALSE(trackSnapshot(graph, third, tracks).inputArmed);
}

TEST(RackGraphInputArmingTest, ExclusiveSelectionRejectsInvalidTrackAndPreservesArmState) {
    RackGraph graph;
    configure(graph);
    const RackPathId first = graph.getTracks().front().id;
    const RackPathId second = graph.addTrack();
    const RackPathId third = graph.addTrack();

    ASSERT_TRUE(graph.setTrackInputArmed(first, true));
    ASSERT_TRUE(graph.setTrackInputArmed(second, true));
    ASSERT_TRUE(graph.setTrackInputArmed(third, false));

    EXPECT_FALSE(graph.setTrackInputArmedExclusive(guitarrackcraft::kMasterPathId));

    std::vector<guitarrackcraft::TrackSnapshot> tracks;
    EXPECT_TRUE(trackSnapshot(graph, first, tracks).inputArmed);
    EXPECT_TRUE(trackSnapshot(graph, second, tracks).inputArmed);
    EXPECT_FALSE(trackSnapshot(graph, third, tracks).inputArmed);
}

TEST(RackGraphInputArmingTest, LockedTrackSurvivesExclusiveSelectionAndUnlockAllowsDisarm) {
    RackGraph graph;
    configure(graph);
    const RackPathId first = graph.getTracks().front().id;
    const RackPathId second = graph.addTrack();
    const RackPathId third = graph.addTrack();

    ASSERT_TRUE(graph.setTrackInputArmed(first, true));
    ASSERT_TRUE(graph.setTrackInputArmed(second, true));
    ASSERT_TRUE(graph.setTrackInputArmed(third, true));
    ASSERT_TRUE(graph.setTrackInputArmLocked(first, true));

    std::vector<guitarrackcraft::TrackSnapshot> tracks;
    const auto& locked = trackSnapshot(graph, first, tracks);
    EXPECT_TRUE(locked.inputArmed);
    EXPECT_TRUE(locked.inputArmLocked);

    ASSERT_TRUE(graph.setTrackInputArmedExclusive(second));

    EXPECT_TRUE(trackSnapshot(graph, first, tracks).inputArmed);
    EXPECT_TRUE(trackSnapshot(graph, first, tracks).inputArmLocked);
    EXPECT_TRUE(trackSnapshot(graph, second, tracks).inputArmed);
    EXPECT_FALSE(trackSnapshot(graph, second, tracks).inputArmLocked);
    EXPECT_FALSE(trackSnapshot(graph, third, tracks).inputArmed);
    EXPECT_FALSE(trackSnapshot(graph, third, tracks).inputArmLocked);

    ASSERT_TRUE(graph.setTrackInputArmLocked(first, false));
    EXPECT_FALSE(trackSnapshot(graph, first, tracks).inputArmLocked);
    EXPECT_TRUE(trackSnapshot(graph, first, tracks).inputArmed);

    ASSERT_TRUE(graph.setTrackInputArmedExclusive(third));

    EXPECT_FALSE(trackSnapshot(graph, first, tracks).inputArmed);
    EXPECT_FALSE(trackSnapshot(graph, second, tracks).inputArmed);
    EXPECT_TRUE(trackSnapshot(graph, third, tracks).inputArmed);
}

TEST(RackGraphInputArmingTest, InvalidArmLockIdReturnsFalseWithoutMutatingTrackState) {
    RackGraph graph;
    configure(graph);
    const RackPathId first = graph.getTracks().front().id;
    const RackPathId second = graph.addTrack();
    const RackPathId third = graph.addTrack();

    ASSERT_TRUE(graph.setTrackInputArmed(first, true));
    ASSERT_TRUE(graph.setTrackInputArmLocked(first, true));
    ASSERT_TRUE(graph.setTrackInputArmed(second, false));
    ASSERT_TRUE(graph.setTrackInputArmLocked(second, false));
    ASSERT_TRUE(graph.setTrackInputArmed(third, true));
    ASSERT_TRUE(graph.setTrackInputArmLocked(third, false));

    EXPECT_FALSE(graph.setTrackInputArmLocked(guitarrackcraft::kMasterPathId, false));

    std::vector<guitarrackcraft::TrackSnapshot> tracks;
    EXPECT_TRUE(trackSnapshot(graph, first, tracks).inputArmed);
    EXPECT_TRUE(trackSnapshot(graph, first, tracks).inputArmLocked);
    EXPECT_FALSE(trackSnapshot(graph, second, tracks).inputArmed);
    EXPECT_FALSE(trackSnapshot(graph, second, tracks).inputArmLocked);
    EXPECT_TRUE(trackSnapshot(graph, third, tracks).inputArmed);
    EXPECT_FALSE(trackSnapshot(graph, third, tracks).inputArmLocked);
}
TEST(RackGraphPluginRoutingTest, TrackChainRoutesProcessedStereoToDirectOutput) {
    RackGraph graph;
    configure(graph);
    const RackPathId track = graph.getTracks().front().id;

    ASSERT_TRUE(graph.setTrackInputArmed(track, true));
    const auto chain = graph.getChain(track);
    ASSERT_NE(chain, nullptr);
    ASSERT_EQ(chain->addPlugin(std::make_unique<StereoOffsetPlugin>()), 0);

    StereoBuffers buffers;
    clearBuffers(buffers);
    buffers.left.fill(1.5f);
    buffers.right.fill(-7.0f);
    graph.process(buffers.inputs, 2, buffers.outputs, 4);

    for (uint32_t frame = 0; frame < 4; ++frame) {
        EXPECT_FLOAT_EQ(buffers.outputLeft[frame], 11.5f) << "frame " << frame;
        EXPECT_FLOAT_EQ(buffers.outputRight[frame], 21.5f) << "frame " << frame;
    }
}
