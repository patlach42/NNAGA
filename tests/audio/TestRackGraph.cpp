#include <gtest/gtest.h>

#include "plugin/RackGraph.h"

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
    graph.process(buffers.inputs, buffers.outputs, 30);

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
    graph.process(buffers.inputs, buffers.outputs, 16);
    EXPECT_FLOAT_EQ(buffers.outputLeft[15], 1.0f);

    graph.setTransportPlaying(false);
    clearBuffers(buffers);
    graph.process(buffers.inputs, buffers.outputs, 4);
    expectSilence(buffers, 4);
    std::vector<guitarrackcraft::TrackSnapshot> tracks;
    EXPECT_FALSE(trackSnapshot(graph, track, tracks).playing);
    EXPECT_FALSE(graph.getTransportSnapshot().playing);
    EXPECT_EQ(graph.getTransportSnapshot().transportFrame, 16u);

    // Resuming the global clock does not relaunch a stopped track.
    graph.setTransportPlaying(true);
    clearBuffers(buffers);
    graph.process(buffers.inputs, buffers.outputs, 4);
    expectSilence(buffers, 4);
    EXPECT_FALSE(trackSnapshot(graph, track, tracks).playing);

    // A new explicit launch is required, and is quantized from the resumed global frame.
    graph.setTrackTransportPlaying(track, true, guitarrackcraft::LaunchQuantization::Sixteenth);
    clearBuffers(buffers);
    graph.process(buffers.inputs, buffers.outputs, 11);
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
    graph.process(buffers.inputs, buffers.outputs, 32);

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
    EXPECT_NE(firstPosition, secondState.positionSec);

    clearBuffers(buffers);
    graph.process(buffers.inputs, buffers.outputs, 3);
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
    graph.process(buffers.inputs, buffers.outputs, 10);
    expectSilence(buffers, 10);
    std::vector<guitarrackcraft::TrackSnapshot> tracks;
    EXPECT_FALSE(trackSnapshot(graph, track, tracks).playing);

    graph.setTransportPlaying(true);
    clearBuffers(buffers);
    graph.process(buffers.inputs, buffers.outputs, 61);
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
        graph.process(buffers.inputs, buffers.outputs, 3);
        graph.setTrackTransportPlaying(track, true, testCase.quantization);
        clearBuffers(buffers);
        const uint32_t framesThroughBoundary = testCase.boundary - 2;
        graph.process(buffers.inputs, buffers.outputs, framesThroughBoundary);

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
    }
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
    graph.process(buffers.inputs, buffers.outputs, 64);
    allocation_probe::enabled = false;

    EXPECT_EQ(allocation_probe::allocations, 0u);
    for (uint32_t frame = 0; frame < 64; ++frame) {
        EXPECT_FLOAT_EQ(buffers.outputLeft[frame], 0.25f) << "frame " << frame;
        EXPECT_FLOAT_EQ(buffers.outputRight[frame], -0.5f) << "frame " << frame;
    }
}
