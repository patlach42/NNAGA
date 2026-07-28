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

    void process(const float* const* inputs, float* const* outputs, uint32_t frames) override {
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
    expectStereoAt(buffers, 2, 30.0f, 30.0f);
    expectStereoAt(buffers, 3, 40.0f, 40.0f);
    expectStereoAt(buffers, 4, 50.0f, 50.0f);
    expectStereoAt(buffers, 5, 11.0f, 11.0f);
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
    EXPECT_FALSE(graph.setTransportPlaying(true));
    EXPECT_FALSE(graph.restartTransport());
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
