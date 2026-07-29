#include <gtest/gtest.h>

#include "plugin/RackGraph.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <string>
#include <utility>
#include <vector>
#include <thread>
#include <atomic>
#include <limits>

namespace allocation_probe {
extern thread_local bool enabled;
extern thread_local std::size_t allocations;
}

namespace {

using guitarrackcraft::IPlugin;
using guitarrackcraft::RackGraph;
using guitarrackcraft::RackPathId;
using guitarrackcraft::WavClip;
using guitarrackcraft::kMasterPathId;

class AffinePlugin final : public IPlugin {
public:
    explicit AffinePlugin(float gain, float offset = 0.0f) : gain_(gain), offset_(offset) {}

    void activate(float, uint32_t) override {}
    void deactivate() override {}

    void process(const float* const* inputs, float* const* outputs, uint32_t frames,
                 const guitarrackcraft::AudioProcessContext&) override {
        for (uint32_t frame = 0; frame < frames; ++frame) {
            outputs[0][frame] = inputs[0][frame] * gain_ + offset_;
            outputs[1][frame] = inputs[1][frame] * gain_ + offset_;
        }
    }

    guitarrackcraft::PluginInfo getInfo() const override {
        guitarrackcraft::PluginInfo info;
        info.id = "test:affine";
        info.name = "Affine test plugin";
        info.format = "test";
        return info;
    }
    void setParameter(uint32_t, float) override {}
    float getParameter(uint32_t) const override { return 0.0f; }
    uint32_t getNumInputPorts() const override { return 2; }
    uint32_t getNumOutputPorts() const override { return 2; }

private:
    float gain_;
    float offset_;
};

struct ContextCapture {
    std::array<guitarrackcraft::AudioProcessContext, 8> blocks{};
    uint32_t count = 0;
};

class ContextCapturePlugin final : public IPlugin {
public:
    explicit ContextCapturePlugin(ContextCapture& capture) : capture_(capture) {}

    void activate(float, uint32_t) override {}
    void deactivate() override {}

    void process(const float* const* inputs, float* const* outputs, uint32_t frames,
                 const guitarrackcraft::AudioProcessContext& context) override {
        if (capture_.count < capture_.blocks.size()) {
            capture_.blocks[capture_.count++] = context;
        }
        for (uint32_t frame = 0; frame < frames; ++frame) {
            outputs[0][frame] = inputs[0][frame];
            outputs[1][frame] = inputs[1][frame];
        }
    }

    guitarrackcraft::PluginInfo getInfo() const override { return {}; }
    void setParameter(uint32_t, float) override {}
    float getParameter(uint32_t) const override { return 0.0f; }
    uint32_t getNumInputPorts() const override { return 2; }
    uint32_t getNumOutputPorts() const override { return 2; }

private:
    ContextCapture& capture_;
};

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
    std::array<float, 2048> left{};
    std::array<float, 2048> right{};
    std::array<float, 2048> outputLeft{};
    std::array<float, 2048> outputRight{};

    const float* inputs[2] = {left.data(), right.data()};
    float* outputs[2] = {outputLeft.data(), outputRight.data()};
};

void fillInput(StereoBuffers& buffers, float left, float right) {
    buffers.left.fill(left);
    buffers.right.fill(right);
}

void fillOutput(StereoBuffers& buffers, float value = -99.0f) {
    buffers.outputLeft.fill(value);
    buffers.outputRight.fill(value);
}

void expectStereo(const StereoBuffers& buffers, uint32_t frames, float left, float right) {
    for (uint32_t frame = 0; frame < frames; ++frame) {
        EXPECT_FLOAT_EQ(buffers.outputLeft[frame], left) << "frame " << frame;
        EXPECT_FLOAT_EQ(buffers.outputRight[frame], right) << "frame " << frame;
    }
}

void expectStereoAt(const StereoBuffers& buffers, uint32_t frame, float left, float right) {
    EXPECT_FLOAT_EQ(buffers.outputLeft[frame], left);
    EXPECT_FLOAT_EQ(buffers.outputRight[frame], right);
}

void configure(RackGraph& graph, uint32_t capacity = 16) {
    graph.setSampleRate(48000.0f, capacity);
}

} // namespace

TEST(RackGraphContractTest, ArmedLivePassesThroughAndDisarmedChainStillRunsOnZeros) {
    RackGraph graph;
    configure(graph, 8);
    const RackPathId track = graph.getTracks().front().id;
    StereoBuffers buffers;
    fillInput(buffers, 0.25f, -0.5f);

    ASSERT_TRUE(graph.setTrackInputArmed(track, true));
    graph.process(buffers.inputs, buffers.outputs, 4);
    expectStereo(buffers, 4, 0.25f, -0.5f);

    ASSERT_TRUE(graph.setTrackInputArmed(track, false));
    auto chain = graph.getChain(track);
    ASSERT_NE(chain, nullptr);
    ASSERT_EQ(chain->addPlugin(std::make_unique<AffinePlugin>(1.0f, 0.75f)), 0);
    fillOutput(buffers);
    graph.process(buffers.inputs, buffers.outputs, 4);
    expectStereo(buffers, 4, 0.75f, 0.75f);
}

TEST(RackGraphContractTest, ParallelTracksApplyIndependentVolumeThenMaster) {
    RackGraph graph;
    configure(graph, 8);
    const RackPathId first = graph.getTracks().front().id;
    const RackPathId second = graph.addTrack();
    ASSERT_NE(second, 0u);
    ASSERT_TRUE(graph.setTrackInputArmed(first, true));
    ASSERT_TRUE(graph.setTrackInputArmed(second, true));
    ASSERT_TRUE(graph.setTrackVolume(first, 0.5f));
    ASSERT_TRUE(graph.setTrackVolume(second, 0.25f));

    auto master = graph.getChain(kMasterPathId);
    ASSERT_NE(master, nullptr);
    ASSERT_EQ(master->addPlugin(std::make_unique<AffinePlugin>(2.0f)), 0);

    StereoBuffers buffers;
    fillInput(buffers, 0.4f, 0.4f);
    graph.process(buffers.inputs, buffers.outputs, 4);
    expectStereo(buffers, 4, 0.6f, 0.6f);
    ASSERT_TRUE(graph.setTrackVolume(first, 2.0f));
    fillOutput(buffers);
    graph.process(buffers.inputs, buffers.outputs, 4);
    expectStereo(buffers, 4, 1.0f, 1.0f);

    ASSERT_TRUE(graph.setTrackVolume(first, -1.0f));
    fillOutput(buffers);
    graph.process(buffers.inputs, buffers.outputs, 4);
    expectStereo(buffers, 4, 0.2f, 0.2f);
}

TEST(RackGraphContractTest, AttachedWavOverridesArmAndUnloadRestoresLiveSource) {
    RackGraph graph;
    configure(graph, 8);
    const RackPathId track = graph.getTracks().front().id;
    ASSERT_TRUE(graph.setTrackInputArmed(track, true));
    ASSERT_TRUE(graph.attachTrackWav(track, makeClip({0.1f, 0.2f, 0.3f}, 48000, {}, "override.wav")));
    const auto tracks = graph.getTracks();
    ASSERT_EQ(tracks.size(), 1u);
    EXPECT_TRUE(tracks[0].wavLoaded);
    EXPECT_EQ(tracks[0].wavDisplayName, "override.wav");
    EXPECT_DOUBLE_EQ(tracks[0].wavDurationSec, 3.0 / 48000.0);

    StereoBuffers buffers;
    fillInput(buffers, 0.9f, -0.9f);
    ASSERT_TRUE(graph.restartTransport());
    graph.process(buffers.inputs, buffers.outputs, 1);
    expectStereoAt(buffers, 0, 0.1f, 0.1f);
    ASSERT_TRUE(graph.setTransportPlaying(false));
    fillOutput(buffers);
    graph.process(buffers.inputs, buffers.outputs, 3);
    expectStereo(buffers, 3, 0.0f, 0.0f);

    ASSERT_TRUE(graph.setTrackInputArmed(track, false));
    ASSERT_TRUE(graph.unloadTrackWav(track));
    ASSERT_TRUE(graph.unloadTrackWav(track));
    fillOutput(buffers);
    graph.process(buffers.inputs, buffers.outputs, 3);
    expectStereo(buffers, 3, 0.0f, 0.0f);

    ASSERT_TRUE(graph.setTrackInputArmed(track, true));
    fillOutput(buffers);
    graph.process(buffers.inputs, buffers.outputs, 3);
    expectStereo(buffers, 3, 0.9f, -0.9f);
}

TEST(RackGraphContractTest, StereoClipResamplesLinearlyWithoutChannelCollapse) {
    RackGraph graph;
    configure(graph, 8);
    const RackPathId track = graph.getTracks().front().id;
    ASSERT_TRUE(graph.attachTrackWav(track, makeClip({0.0f, 1.0f, 2.0f}, 24000,
                                                       {10.0f, 20.0f, 30.0f}, "stereo.wav")));
    ASSERT_TRUE(graph.restartTransport());

    StereoBuffers buffers;
    fillInput(buffers, 99.0f, 99.0f);
    graph.process(buffers.inputs, buffers.outputs, 3);
    expectStereoAt(buffers, 0, 0.0f, 10.0f);
    expectStereoAt(buffers, 1, 0.5f, 15.0f);
    expectStereoAt(buffers, 2, 1.0f, 20.0f);
}

TEST(RackGraphContractTest, SharedTransportUsesLongestClipAndLoopsAtBoundary) {
    RackGraph graph;
    configure(graph, 8);
    const RackPathId first = graph.getTracks().front().id;
    const RackPathId second = graph.addTrack();
    ASSERT_TRUE(graph.attachTrackWav(first, makeClip({1.0f, 2.0f}, 48000, {}, "short.wav")));
    ASSERT_TRUE(graph.attachTrackWav(second, makeClip({10.0f, 20.0f, 30.0f, 40.0f, 50.0f}, 48000, {}, "long.wav")));
    ContextCapture loopingTrackCapture;
    ContextCapture loopingMasterCapture;
    ASSERT_EQ(graph.getChain(first)->addPlugin(
                  std::make_unique<ContextCapturePlugin>(loopingTrackCapture)),
              0);
    ASSERT_EQ(graph.getChain(kMasterPathId)->addPlugin(
                  std::make_unique<ContextCapturePlugin>(loopingMasterCapture)),
              0);
    ASSERT_TRUE(graph.restartTransport());

    StereoBuffers buffers;
    fillInput(buffers, 0.0f, 0.0f);
    graph.process(buffers.inputs, buffers.outputs, 2);
    expectStereoAt(buffers, 0, 11.0f, 11.0f);
    expectStereoAt(buffers, 1, 22.0f, 22.0f);
    graph.process(buffers.inputs, buffers.outputs, 2);
    expectStereoAt(buffers, 0, 30.0f, 30.0f);
    expectStereoAt(buffers, 1, 40.0f, 40.0f);
    graph.process(buffers.inputs, buffers.outputs, 2);
    expectStereoAt(buffers, 0, 50.0f, 50.0f);
    expectStereoAt(buffers, 1, 0.0f, 0.0f);

    const auto ended = graph.getTransportSnapshot();
    EXPECT_FALSE(ended.playing);
    EXPECT_DOUBLE_EQ(ended.positionSec, 5.0 / 48000.0);
    EXPECT_DOUBLE_EQ(ended.durationSec, 5.0 / 48000.0);
    EXPECT_EQ(ended.loadedTrackCount, 2u);

    graph.setTransportLooping(true);
    fillOutput(buffers);
    graph.process(buffers.inputs, buffers.outputs, 2);
    expectStereo(buffers, 2, 0.0f, 0.0f);
    EXPECT_FALSE(graph.getTransportSnapshot().playing);

    ASSERT_TRUE(graph.restartTransport());
    graph.process(buffers.inputs, buffers.outputs, 6);
    expectStereoAt(buffers, 0, 11.0f, 11.0f);
    expectStereoAt(buffers, 1, 22.0f, 22.0f);
    expectStereoAt(buffers, 2, 31.0f, 31.0f);
    expectStereoAt(buffers, 3, 42.0f, 42.0f);
    expectStereoAt(buffers, 4, 51.0f, 51.0f);
    expectStereoAt(buffers, 5, 12.0f, 12.0f);
    ASSERT_EQ(loopingTrackCapture.count, 5u);
    for (std::size_t block = 0; block < 3; ++block) {
        EXPECT_EQ(loopingTrackCapture.blocks[block].loopEndFrame, 0u);
        EXPECT_EQ(loopingMasterCapture.blocks[block].loopEndFrame, 0u);
    }
    ASSERT_EQ(loopingMasterCapture.count, 5u);
    EXPECT_EQ(loopingTrackCapture.blocks[4].loopEndFrame, 5u);
    EXPECT_EQ(loopingTrackCapture.blocks[4].transportFrame, 0u);
    EXPECT_TRUE(loopingTrackCapture.blocks[4].playing);
    EXPECT_EQ(loopingMasterCapture.blocks[4].loopEndFrame, 5u);
    EXPECT_EQ(loopingMasterCapture.blocks[4].transportFrame, 0u);
    EXPECT_EQ(loopingMasterCapture.blocks[4].loopEndFrame,
              loopingTrackCapture.blocks[4].loopEndFrame);
    EXPECT_EQ(graph.getTransportSnapshot().transportFrame, 1u);
    EXPECT_TRUE(graph.getTransportSnapshot().playing);
}

TEST(RackGraphContractTest, TrackAndPluginIdentityRemainStableAcrossReorderAndRemoval) {
    RackGraph graph;
    configure(graph, 8);
    const RackPathId first = graph.getTracks().front().id;
    const RackPathId second = graph.addTrack();
    ASSERT_NE(second, 0u);
    EXPECT_GT(second, first);
    const RackPathId third = graph.addTrack();
    EXPECT_EQ(third, second + 1);

    EXPECT_FALSE(graph.removeTrack(kMasterPathId));
    EXPECT_FALSE(graph.removeTrack(999999));
    EXPECT_TRUE(graph.removeTrack(second));
    const RackPathId next = graph.addTrack();
    EXPECT_GT(next, third);

    auto chain = graph.getChain(first);
    ASSERT_NE(chain, nullptr);
    EXPECT_EQ(chain->getPluginInstanceId(-1), 0u);
    EXPECT_EQ(chain->getPluginInstanceId(0), 0u);
    ASSERT_EQ(chain->addPlugin(std::make_unique<AffinePlugin>(1.0f)), 0);
    const auto firstId = chain->getPluginInstanceId(0);
    ASSERT_EQ(chain->addPlugin(std::make_unique<AffinePlugin>(1.0f)), 1);
    const auto secondId = chain->getPluginInstanceId(1);
    EXPECT_NE(firstId, 0u);
    EXPECT_NE(secondId, 0u);
    EXPECT_NE(firstId, secondId);
    ASSERT_TRUE(chain->reorderPlugins(0, 1));
    EXPECT_EQ(chain->getPluginInstanceId(0), secondId);
    EXPECT_EQ(chain->getPluginInstanceId(1), firstId);
    auto master = graph.getChain(kMasterPathId);
    ASSERT_NE(master, nullptr);
    ASSERT_EQ(master->addPlugin(std::make_unique<AffinePlugin>(1.0f)), 0);
    const auto masterId = master->getPluginInstanceId(0);
    EXPECT_NE(masterId, 0u);
    EXPECT_NE(masterId, firstId);
    EXPECT_NE(masterId, secondId);

    ASSERT_TRUE(graph.removeTrack(first));
    EXPECT_EQ(graph.getChain(first), nullptr);
    ASSERT_TRUE(graph.removeTrack(third));
    EXPECT_FALSE(graph.removeTrack(next));
    EXPECT_EQ(graph.getChain(kMasterPathId)->getPluginInstanceId(0), masterId);
    EXPECT_EQ(graph.getChain(999999), nullptr);
}

TEST(RackGraphContractTest, SupportedQuantaDoNotAllocateAndOversizedBlocksFailClosed) {
    RackGraph graph;
    constexpr uint32_t capacity = 1024;
    configure(graph, capacity);
    StereoBuffers buffers;
    fillInput(buffers, 0.25f, -0.5f);
    ASSERT_TRUE(graph.setTrackInputArmed(graph.getTracks().front().id, true));
    const std::array<uint32_t, 4> quanta = {16, 64, 512, 1024};

    for (const uint32_t frames : quanta) {
        SCOPED_TRACE(frames);
        fillOutput(buffers);
        allocation_probe::allocations = 0;
        allocation_probe::enabled = true;
        graph.process(buffers.inputs, buffers.outputs, frames);
        allocation_probe::enabled = false;
        EXPECT_EQ(allocation_probe::allocations, 0u);
        expectStereo(buffers, frames, 0.25f, -0.5f);
    }

    fillOutput(buffers);
    allocation_probe::allocations = 0;
    allocation_probe::enabled = true;
    graph.process(buffers.inputs, buffers.outputs, capacity + 1);
    allocation_probe::enabled = false;
    EXPECT_EQ(allocation_probe::allocations, 0u);
    expectStereo(buffers, capacity + 1, 0.0f, 0.0f);
}

TEST(RackGraphContractTest, InvalidWavAndTransportOperationsPreserveObservableState) {
    RackGraph graph;
    configure(graph, 8);
    const RackPathId track = graph.getTracks().front().id;
    const auto before = graph.getTransportSnapshot();

    EXPECT_FALSE(graph.attachTrackWav(track, nullptr));
    EXPECT_FALSE(graph.attachTrackWav(track, makeClip({}, 48000)));
    EXPECT_FALSE(graph.attachTrackWav(track, makeClip({1.0f}, 0)));
    EXPECT_FALSE(graph.attachTrackWav(track, makeClip({1.0f}, 48000, {1.0f, 2.0f})));
    EXPECT_FALSE(graph.attachTrackWav(123456, makeClip({1.0f}, 48000)));
    EXPECT_TRUE(graph.setTransportPlaying(true));
    EXPECT_TRUE(graph.restartTransport());
    EXPECT_FALSE(graph.setTrackVolume(123456, 0.5f));
    EXPECT_FALSE(graph.setTrackInputArmed(123456, true));
    EXPECT_EQ(graph.getTransportSnapshot().loadedTrackCount, before.loadedTrackCount);
    EXPECT_DOUBLE_EQ(graph.getTransportSnapshot().positionSec, before.positionSec);
    EXPECT_DOUBLE_EQ(graph.getTransportSnapshot().durationSec, before.durationSec);
    ASSERT_TRUE(graph.attachTrackWav(track, makeClip({0.4f, 0.5f}, 48000, {}, "good.wav")));
    const auto loaded = graph.getTracks().front();
    ASSERT_FALSE(graph.attachTrackWav(track, makeClip({0.8f}, 48000, {0.9f, 1.0f}, "bad.wav")));
    const auto preserved = graph.getTracks().front();
    EXPECT_TRUE(preserved.wavLoaded);
    EXPECT_EQ(preserved.wavDisplayName, loaded.wavDisplayName);
    EXPECT_DOUBLE_EQ(preserved.wavDurationSec, loaded.wavDurationSec);
    EXPECT_EQ(graph.getTransportSnapshot().loadedTrackCount, 1u);
    EXPECT_DOUBLE_EQ(graph.getTransportSnapshot().positionSec, 0.0);
}

TEST(RackGraphTransportTest, WorldClockAdvancesWhenStoppedAndRestartOnlyResetsPlayhead) {
    RackGraph graph;
    configure(graph, 16);
    ContextCapture capture;
    auto master = graph.getChain(kMasterPathId);
    ASSERT_NE(master, nullptr);
    ASSERT_EQ(master->addPlugin(std::make_unique<ContextCapturePlugin>(capture)), 0);

    StereoBuffers buffers;
    fillInput(buffers, 0.0f, 0.0f);
    graph.setTransportPlaying(false);
    graph.process(buffers.inputs, buffers.outputs, 3);
    graph.process(buffers.inputs, buffers.outputs, 5);

    auto stopped = graph.getTransportSnapshot();
    EXPECT_FALSE(stopped.playing);
    EXPECT_EQ(stopped.samplePosition, 8u);
    EXPECT_EQ(stopped.transportFrame, 0u);
    ASSERT_EQ(capture.count, 2u);
    EXPECT_EQ(capture.blocks[0].samplePosition, 0u);
    EXPECT_EQ(capture.blocks[0].transportFrame, 0u);
    EXPECT_EQ(capture.blocks[0].loopEndFrame, 0u);
    EXPECT_EQ(capture.blocks[1].samplePosition, 3u);
    EXPECT_EQ(capture.blocks[1].transportFrame, 0u);
    EXPECT_EQ(capture.blocks[1].loopEndFrame, 0u);

    graph.setTransportPlaying(true);
    graph.process(buffers.inputs, buffers.outputs, 3);
    graph.process(buffers.inputs, buffers.outputs, 5);
    auto playing = graph.getTransportSnapshot();
    EXPECT_TRUE(playing.playing);
    EXPECT_EQ(playing.samplePosition, 16u);
    EXPECT_EQ(playing.transportFrame, 8u);
    ASSERT_EQ(capture.count, 4u);
    EXPECT_EQ(capture.blocks[2].samplePosition, 8u);
    EXPECT_EQ(capture.blocks[2].transportFrame, 0u);
    EXPECT_EQ(capture.blocks[2].loopEndFrame, 0u);
    EXPECT_TRUE(capture.blocks[2].playing);
    EXPECT_EQ(capture.blocks[3].samplePosition, 11u);
    EXPECT_EQ(capture.blocks[3].transportFrame, 3u);
    EXPECT_EQ(capture.blocks[3].loopEndFrame, 0u);

    graph.setTransportPlaying(false);
    graph.process(buffers.inputs, buffers.outputs, 2);
    graph.restartTransport();
    graph.process(buffers.inputs, buffers.outputs, 4);
    auto restarted = graph.getTransportSnapshot();
    EXPECT_TRUE(restarted.playing);
    EXPECT_EQ(restarted.samplePosition, 22u);
    EXPECT_EQ(restarted.transportFrame, 4u);
    ASSERT_EQ(capture.count, 6u);
    EXPECT_EQ(capture.blocks[4].samplePosition, 16u);
    EXPECT_EQ(capture.blocks[4].transportFrame, 8u);
    EXPECT_FALSE(capture.blocks[4].playing);
    EXPECT_EQ(capture.blocks[4].loopEndFrame, 0u);
    EXPECT_EQ(capture.blocks[5].samplePosition, 18u);
    EXPECT_EQ(capture.blocks[5].transportFrame, 0u);
    EXPECT_TRUE(capture.blocks[5].playing);
    EXPECT_EQ(capture.blocks[5].loopEndFrame, 0u);
}

TEST(RackGraphTransportTest, BpmChangesAreClampedAndAppliedAtNextBlock) {
    RackGraph graph;
    configure(graph, 8);
    ContextCapture capture;
    auto master = graph.getChain(kMasterPathId);
    ASSERT_NE(master, nullptr);
    ASSERT_EQ(master->addPlugin(std::make_unique<ContextCapturePlugin>(capture)), 0);
    StereoBuffers buffers;
    fillInput(buffers, 0.0f, 0.0f);

    graph.setTransportPlaying(true);
    graph.process(buffers.inputs, buffers.outputs, 1);
    ASSERT_EQ(capture.count, 1u);
    EXPECT_DOUBLE_EQ(capture.blocks[0].beatsPerMinute, 120.0);

    graph.setBeatsPerMinute(999.0);
    graph.process(buffers.inputs, buffers.outputs, 2);
    ASSERT_EQ(capture.count, 2u);
    EXPECT_DOUBLE_EQ(capture.blocks[1].beatsPerMinute, 400.0);
    EXPECT_DOUBLE_EQ(graph.getTransportSnapshot().beatsPerMinute, 400.0);

    graph.setBeatsPerMinute(0.0);
    graph.process(buffers.inputs, buffers.outputs, 1);
    ASSERT_EQ(capture.count, 3u);
    EXPECT_DOUBLE_EQ(capture.blocks[2].beatsPerMinute, 20.0);

    graph.setBeatsPerMinute(std::numeric_limits<double>::quiet_NaN());
    graph.process(buffers.inputs, buffers.outputs, 1);
    ASSERT_EQ(capture.count, 4u);
    EXPECT_DOUBLE_EQ(capture.blocks[3].beatsPerMinute, 120.0);
}

TEST(RackGraphTransportTest, ParallelTracksAndMasterReceiveIdenticalBlockContext) {
    RackGraph graph;
    configure(graph, 8);
    const RackPathId first = graph.getTracks().front().id;
    const RackPathId second = graph.addTrack();
    ASSERT_NE(second, 0u);
    ASSERT_TRUE(graph.setTrackInputArmed(first, true));
    ASSERT_TRUE(graph.setTrackInputArmed(second, true));

    ContextCapture firstCapture;
    ContextCapture secondCapture;
    ContextCapture masterCapture;
    ASSERT_EQ(graph.getChain(first)->addPlugin(
                  std::make_unique<ContextCapturePlugin>(firstCapture)),
              0);
    ASSERT_EQ(graph.getChain(second)->addPlugin(
                  std::make_unique<ContextCapturePlugin>(secondCapture)),
              0);
    ASSERT_EQ(graph.getChain(kMasterPathId)->addPlugin(
                  std::make_unique<ContextCapturePlugin>(masterCapture)),
              0);

    StereoBuffers buffers;
    fillInput(buffers, 0.25f, -0.5f);
    graph.setTransportPlaying(true);
    graph.process(buffers.inputs, buffers.outputs, 4);

    ASSERT_EQ(firstCapture.count, 1u);
    ASSERT_EQ(secondCapture.count, 1u);
    ASSERT_EQ(masterCapture.count, 1u);
    const auto& firstContext = firstCapture.blocks[0];
    const auto& secondContext = secondCapture.blocks[0];
    const auto& masterContext = masterCapture.blocks[0];
    EXPECT_EQ(firstContext.samplePosition, secondContext.samplePosition);
    EXPECT_EQ(firstContext.samplePosition, masterContext.samplePosition);
    EXPECT_EQ(firstContext.transportFrame, secondContext.transportFrame);
    EXPECT_EQ(firstContext.transportFrame, masterContext.transportFrame);
    EXPECT_DOUBLE_EQ(firstContext.sampleRate, secondContext.sampleRate);
    EXPECT_DOUBLE_EQ(firstContext.sampleRate, masterContext.sampleRate);
    EXPECT_EQ(firstContext.loopEndFrame, secondContext.loopEndFrame);
    EXPECT_EQ(firstContext.loopEndFrame, masterContext.loopEndFrame);
    EXPECT_DOUBLE_EQ(firstContext.beatsPerMinute, secondContext.beatsPerMinute);
    EXPECT_DOUBLE_EQ(firstContext.beatsPerMinute, masterContext.beatsPerMinute);
    EXPECT_EQ(firstContext.playing, secondContext.playing);
    EXPECT_EQ(firstContext.playing, masterContext.playing);
    EXPECT_EQ(firstContext.looping, secondContext.looping);
    EXPECT_EQ(firstContext.looping, masterContext.looping);
    expectStereo(buffers, 4, 0.5f, -1.0f);
}

TEST(RackGraphTransportTest, FractionalRateLoopingIsChunkingInvariant) {
    auto clip = makeClip({0.0f, 1.0f, 2.0f, 3.0f, 4.0f}, 44100);

    RackGraph oneBlock;
    configure(oneBlock, 16);
    const RackPathId oneTrack = oneBlock.getTracks().front().id;
    ASSERT_TRUE(oneBlock.attachTrackWav(oneTrack, clip));
    oneBlock.setTransportLooping(true);
    ASSERT_TRUE(oneBlock.restartTransport());
    StereoBuffers oneBuffers;
    fillInput(oneBuffers, 0.0f, 0.0f);
    oneBlock.process(oneBuffers.inputs, oneBuffers.outputs, 16);

    RackGraph chunks;
    configure(chunks, 16);
    const RackPathId chunkTrack = chunks.getTracks().front().id;
    ASSERT_TRUE(chunks.attachTrackWav(chunkTrack, clip));
    chunks.setTransportLooping(true);
    ASSERT_TRUE(chunks.restartTransport());
    StereoBuffers chunkBuffers;
    fillInput(chunkBuffers, 0.0f, 0.0f);
    std::size_t offset = 0;
    for (const uint32_t frames : {5u, 11u}) {
        const float* inputs[] = {chunkBuffers.left.data(), chunkBuffers.right.data()};
        float* outputs[] = {chunkBuffers.outputLeft.data() + offset,
                            chunkBuffers.outputRight.data() + offset};
        chunks.process(inputs, outputs, frames);
        offset += frames;
    }

    for (std::size_t frame = 0; frame < 16; ++frame) {
        EXPECT_FLOAT_EQ(chunkBuffers.outputLeft[frame], oneBuffers.outputLeft[frame])
            << "frame " << frame;
        EXPECT_FLOAT_EQ(chunkBuffers.outputRight[frame], oneBuffers.outputRight[frame])
            << "frame " << frame;
    }
}

TEST(RackGraphTransportTest, AdvanceTransportUpdatesClockWithoutInvokingPlugins) {
    RackGraph graph;
    configure(graph, 16);
    ContextCapture capture;
    ASSERT_EQ(graph.getChain(kMasterPathId)->addPlugin(
                  std::make_unique<ContextCapturePlugin>(capture)),
              0);
    graph.setTransportPlaying(true);
    graph.advanceTransport(7);

    const auto advanced = graph.getTransportSnapshot();
    EXPECT_TRUE(advanced.playing);
    EXPECT_EQ(advanced.samplePosition, 7u);
    EXPECT_EQ(advanced.transportFrame, 7u);
    EXPECT_EQ(capture.count, 0u);
}

TEST(RackGraphTransportTest, ConcurrentSnapshotsRemainMonotonicAndConsistent) {
    RackGraph graph;
    configure(graph, 16);
    graph.setTransportPlaying(true);
    StereoBuffers buffers;
    fillInput(buffers, 0.0f, 0.0f);

    constexpr uint32_t blocks = 20000;
    std::atomic<bool> done{false};
    std::atomic<bool> invalid{false};
    std::thread reader([&] {
        auto previous = graph.getTransportSnapshot();
        while (!done.load(std::memory_order_acquire)) {
            const auto current = graph.getTransportSnapshot();
            if (current.samplePosition < previous.samplePosition ||
                current.transportFrame < previous.transportFrame ||
                current.samplePosition != current.transportFrame) {
                invalid.store(true, std::memory_order_release);
            }
            previous = current;
        }
    });

    for (uint32_t block = 0; block < blocks; ++block) {
        graph.process(buffers.inputs, buffers.outputs, 1);
        if ((block & 31u) == 0u) std::this_thread::yield();
    }
    done.store(true, std::memory_order_release);
    reader.join();

    EXPECT_FALSE(invalid.load(std::memory_order_acquire));
    const auto final = graph.getTransportSnapshot();
    EXPECT_EQ(final.samplePosition, blocks);
    EXPECT_EQ(final.transportFrame, blocks);
}

TEST(RackGraphTransportTest, FinalConcurrentMailboxCommandIsAppliedAsOneBlockState) {
    RackGraph graph;
    configure(graph, 16);
    StereoBuffers buffers;
    fillInput(buffers, 0.0f, 0.0f);
    std::atomic<bool> updatesDone{false};
    std::thread updates([&] {
        for (int iteration = 0; iteration < 200; ++iteration) {
            graph.setTransportPlaying((iteration & 1) != 0);
            graph.setTransportLooping((iteration & 1) == 0);
            graph.setBeatsPerMinute(40.0 + iteration);
            if ((iteration & 7) == 0) std::this_thread::yield();
        }
        graph.setTransportPlaying(true);
        graph.setTransportLooping(true);
        graph.setBeatsPerMinute(333.0);
        updatesDone.store(true, std::memory_order_release);
    });

    while (!updatesDone.load(std::memory_order_acquire)) {
        graph.process(buffers.inputs, buffers.outputs, 1);
    }
    updates.join();
    graph.process(buffers.inputs, buffers.outputs, 1);

    const auto final = graph.getTransportSnapshot();
    EXPECT_TRUE(final.playing);
    EXPECT_TRUE(final.looping);
    EXPECT_DOUBLE_EQ(final.beatsPerMinute, 333.0);
}

TEST(RackGraphContractTest, ConcurrentAttachAndClearPublishCoherentStatusPairs) {
    RackGraph graph;
    configure(graph, 16);
    const RackPathId track = graph.getTracks().front().id;
    auto clip = makeClip({0.0f, 0.25f, 0.5f, 0.75f}, 48000, {}, "status.wav");
    const double knownDuration = 4.0 / 48000.0;
    std::atomic<bool> start{false};
    std::atomic<bool> done{false};
    std::atomic<bool> invalid{false};

    std::thread reader([&] {
        while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
        while (!done.load(std::memory_order_acquire)) {
            const auto snapshot = graph.getTransportSnapshot();
            const bool empty = snapshot.loadedTrackCount == 0u &&
                               snapshot.durationSec == 0.0;
            const bool loaded = snapshot.loadedTrackCount == 1u &&
                                snapshot.durationSec == knownDuration;
            if (!empty && !loaded) invalid.store(true, std::memory_order_release);
        }
    });

    std::thread mutator([&] {
        while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
        for (int iteration = 0; iteration < 200; ++iteration) {
            if (!graph.attachTrackWav(track, clip) || !graph.clearTrackWavs()) {
                invalid.store(true, std::memory_order_release);
            }
            if ((iteration & 7) == 0) std::this_thread::yield();
        }
        done.store(true, std::memory_order_release);
    });

    start.store(true, std::memory_order_release);
    mutator.join();
    reader.join();

    EXPECT_FALSE(invalid.load(std::memory_order_acquire));
    ASSERT_TRUE(graph.attachTrackWav(track, clip));
    const auto final = graph.getTransportSnapshot();
    EXPECT_EQ(final.loadedTrackCount, 1u);
    EXPECT_DOUBLE_EQ(final.durationSec, knownDuration);
}

TEST(RackGraphTransportTest, EnablingLoopAtExactEndNormalizesProcessAndAdvanceClocks) {
    RackGraph graph;
    configure(graph, 8);
    const RackPathId track = graph.getTracks().front().id;
    ASSERT_TRUE(graph.attachTrackWav(
        track, makeClip({1.0f, 2.0f, 3.0f, 4.0f, 5.0f}, 48000)));
    ContextCapture capture;
    ASSERT_EQ(graph.getChain(kMasterPathId)->addPlugin(
                  std::make_unique<ContextCapturePlugin>(capture)),
              0);
    StereoBuffers buffers;
    fillInput(buffers, 0.0f, 0.0f);

    ASSERT_TRUE(graph.restartTransport());
    graph.process(buffers.inputs, buffers.outputs, 5);
    auto ended = graph.getTransportSnapshot();
    EXPECT_FALSE(ended.playing);
    EXPECT_EQ(ended.transportFrame, 5u);

    graph.setTransportLooping(true);
    ASSERT_TRUE(graph.setTransportPlaying(true));
    fillOutput(buffers);
    graph.process(buffers.inputs, buffers.outputs, 2);
    ASSERT_EQ(capture.count, 2u);
    EXPECT_TRUE(capture.blocks[1].playing);
    EXPECT_TRUE(capture.blocks[1].looping);
    EXPECT_EQ(capture.blocks[1].transportFrame, 0u);
    EXPECT_EQ(capture.blocks[1].loopEndFrame, 5u);
    expectStereoAt(buffers, 0, 1.0f, 1.0f);
    expectStereoAt(buffers, 1, 2.0f, 2.0f);
    EXPECT_EQ(graph.getTransportSnapshot().transportFrame, 2u);

    graph.setTransportLooping(false);
    ASSERT_TRUE(graph.setTransportPlaying(true));
    graph.process(buffers.inputs, buffers.outputs, 3);
    EXPECT_FALSE(graph.getTransportSnapshot().playing);
    EXPECT_EQ(graph.getTransportSnapshot().transportFrame, 5u);

    graph.setTransportLooping(true);
    ASSERT_TRUE(graph.setTransportPlaying(true));
    graph.advanceTransport(0);
    EXPECT_TRUE(graph.getTransportSnapshot().playing);
    EXPECT_EQ(graph.getTransportSnapshot().transportFrame, 0u);
    graph.advanceTransport(1);
    EXPECT_EQ(graph.getTransportSnapshot().transportFrame, 1u);
    EXPECT_EQ(graph.getTransportSnapshot().samplePosition, 11u);
}
