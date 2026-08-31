#include <gtest/gtest.h>

#include "plugin/RackGraph.h"
#include "utils/WavIO.h"

#include <filesystem>
#include <atomic>
#include <limits>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>
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
using guitarrackcraft::MidiClip;


using guitarrackcraft::IPlugin;

constexpr float kTestSampleRate = 60.0f;
constexpr double kTestBpm = 60.0;

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
class FakeLatencyPlugin final : public IPlugin {
public:
    explicit FakeLatencyPlugin(uint32_t latencyFrames)
        : FakeLatencyPlugin(latencyFrames, latencyFrames) {}

    FakeLatencyPlugin(uint32_t physicalLatencyFrames,
                      uint32_t reportedLatencyFrames)
        : physicalLatencyFrames_(physicalLatencyFrames),
          reportedLatencyFrames_(reportedLatencyFrames) {}

    void activate(float, uint32_t) override {
        delayLeft_.fill(0.0f);
        delayRight_.fill(0.0f);
        cursor_ = 0;
    }
    void deactivate() override {}

    uint32_t process(const float* const* inputs, float* const* outputs,
                     uint32_t numFrames,
                     const guitarrackcraft::AudioProcessContext&,
                     const guitarrackcraft::MidiEvent*, uint32_t,
                     guitarrackcraft::MidiEvent*, uint32_t) override {
        if (physicalLatencyFrames_ == 0) {
            for (uint32_t frame = 0; frame < numFrames; ++frame) {
                outputs[0][frame] = inputs[0][frame];
                outputs[1][frame] = inputs[1][frame];
            }
            return 0;
        }
        for (uint32_t frame = 0; frame < numFrames; ++frame) {
            outputs[0][frame] = delayLeft_[cursor_];
            outputs[1][frame] = delayRight_[cursor_];
            delayLeft_[cursor_] = inputs[0][frame];
            delayRight_[cursor_] = inputs[1][frame];
            cursor_ = (cursor_ + 1) % physicalLatencyFrames_;
        }
        return 0;
    }

    guitarrackcraft::PluginInfo getInfo() const override { return {}; }
    void setParameter(uint32_t, float) override {}
    float getParameter(uint32_t) const override { return 0.0f; }
    uint32_t getNumInputPorts() const override { return 2; }
    uint32_t getNumOutputPorts() const override { return 2; }
    uint32_t getLatencyFrames() const noexcept override {
        return reportedLatencyFrames_;
    }

private:
    static constexpr uint32_t kDelayCapacity = 64;
    const uint32_t physicalLatencyFrames_;
    const uint32_t reportedLatencyFrames_;
    std::array<float, kDelayCapacity> delayLeft_{};
    std::array<float, kDelayCapacity> delayRight_{};
    uint32_t cursor_ = 0;
};
class MutableLatencyPlugin final : public IPlugin {
public:
    explicit MutableLatencyPlugin(uint32_t latencyFrames)
        : latencyFrames_(latencyFrames) {}

    void activate(float, uint32_t) override {}
    void deactivate() override {}

    uint32_t process(const float* const* inputs, float* const* outputs,
                     uint32_t numFrames,
                     const guitarrackcraft::AudioProcessContext&,
                     const guitarrackcraft::MidiEvent*, uint32_t,
                     guitarrackcraft::MidiEvent*, uint32_t) override {
        for (uint32_t frame = 0; frame < numFrames; ++frame) {
            outputs[0][frame] = inputs[0][frame];
            outputs[1][frame] = inputs[1][frame];
        }
        return 0;
    }

    guitarrackcraft::PluginInfo getInfo() const override { return {}; }
    void setParameter(uint32_t, float) override {}
    float getParameter(uint32_t) const override { return 0.0f; }
    uint32_t getNumInputPorts() const override { return 2; }
    uint32_t getNumOutputPorts() const override { return 2; }
    uint32_t getLatencyFrames() const noexcept override { return latencyFrames_; }

    void setLatencyFrames(uint32_t latencyFrames) { latencyFrames_ = latencyFrames; }

private:
    uint32_t latencyFrames_;
};


class SnapshotGatePlugin final : public IPlugin {
public:
    SnapshotGatePlugin(std::atomic<bool>& blockNext,
                       std::atomic<bool>& entered,
                       std::atomic<bool>& release)
        : blockNext_(blockNext), entered_(entered), release_(release) {}

    void activate(float, uint32_t) override {}
    void deactivate() override {}

    uint32_t process(const float* const* inputs, float* const* outputs,
                     uint32_t numFrames,
                     const guitarrackcraft::AudioProcessContext&,
                     const guitarrackcraft::MidiEvent*, uint32_t,
                     guitarrackcraft::MidiEvent*, uint32_t) override {
        if (blockNext_.exchange(false, std::memory_order_acq_rel)) {
            entered_.store(true, std::memory_order_release);
            while (!release_.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
        }
        for (uint32_t frame = 0; frame < numFrames; ++frame) {
            outputs[0][frame] = inputs[0][frame];
            outputs[1][frame] = inputs[1][frame];
        }
        return 0;
    }

    guitarrackcraft::PluginInfo getInfo() const override { return {}; }
    void setParameter(uint32_t, float) override {}
    float getParameter(uint32_t) const override { return 0.0f; }
    uint32_t getNumInputPorts() const override { return 2; }
    uint32_t getNumOutputPorts() const override { return 2; }

private:
    std::atomic<bool>& blockNext_;
    std::atomic<bool>& entered_;
    std::atomic<bool>& release_;
};
class ReentrantTrackSnapshotPlugin final : public IPlugin {
public:
    ReentrantTrackSnapshotPlugin(RackGraph& graph, size_t& observedTrackCount)
        : graph_(graph), observedTrackCount_(observedTrackCount) {}

    void activate(float, uint32_t) override {}
    void deactivate() override {}

    uint32_t process(const float* const* inputs, float* const* outputs,
                     uint32_t numFrames,
                     const guitarrackcraft::AudioProcessContext&,
                     const guitarrackcraft::MidiEvent*, uint32_t,
                     guitarrackcraft::MidiEvent*, uint32_t) override {
        observedTrackCount_ = graph_.getTracks().size();
        for (uint32_t frame = 0; frame < numFrames; ++frame) {
            outputs[0][frame] = inputs[0][frame];
            outputs[1][frame] = inputs[1][frame];
        }
        return 0;
    }

    guitarrackcraft::PluginInfo getInfo() const override { return {}; }
    void setParameter(uint32_t, float) override {}
    float getParameter(uint32_t) const override { return 0.0f; }
    uint32_t getNumInputPorts() const override { return 2; }
    uint32_t getNumOutputPorts() const override { return 2; }

private:
    RackGraph& graph_;
    size_t& observedTrackCount_;
};

class MidiCapturingPlugin final : public IPlugin {
public:
    void activate(float, uint32_t) override {}
    void deactivate() override {}

    uint32_t process(const float* const* inputs, float* const* outputs,
                     uint32_t numFrames,
                     const guitarrackcraft::AudioProcessContext&,
                     const guitarrackcraft::MidiEvent* inputEvents,
                     uint32_t inputCount,
                     guitarrackcraft::MidiEvent*, uint32_t) override {
        for (uint32_t i = 0; i < inputCount && capturedCount_ < captured_.size(); ++i) {
            captured_[capturedCount_++] = inputEvents[i];
        }
        for (uint32_t frame = 0; frame < numFrames; ++frame) {
            outputs[0][frame] = inputs[0][frame];
            outputs[1][frame] = inputs[1][frame];
        }
        return 0;
    }

    guitarrackcraft::PluginInfo getInfo() const override { return {}; }
    void setParameter(uint32_t, float) override {}
    float getParameter(uint32_t) const override { return 0.0f; }
    uint32_t getNumInputPorts() const override { return 2; }
    uint32_t getNumOutputPorts() const override { return 2; }

    uint32_t capturedCount() const { return capturedCount_; }
    const guitarrackcraft::MidiEvent& captured(uint32_t index) const {
        return captured_[index];
    }

private:
    std::array<guitarrackcraft::MidiEvent, 128> captured_{};
    uint32_t capturedCount_ = 0;
};

std::shared_ptr<const MidiClip> makeScheduledMidiClip(
    uint64_t durationMicroseconds,
    std::initializer_list<uint64_t> eventMicroseconds) {
    auto clip = std::make_shared<MidiClip>();
    clip->durationMicroseconds = durationMicroseconds;
    uint8_t note = 60;
    for (const uint64_t microseconds : eventMicroseconds) {
        clip->events.push_back({
            microseconds,
            {0u, 0x90u, note++, 100u}});
    }
    return clip;
}

std::shared_ptr<const MidiClip> makeMidiClip(uint64_t durationMicroseconds,
                                             std::string name = "test.mid") {
    auto clip = std::make_shared<MidiClip>();
    clip->durationMicroseconds = durationMicroseconds;
    clip->events.push_back({0u, {0u, 0x90u, 60u, 100u}});
    clip->displayName = std::move(name);
    return clip;
}



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

std::shared_ptr<const WavClip> makeRampClip(uint32_t frames, float firstSample = 1.0f,
                                            std::string name = "ramp.wav") {
    auto clip = std::make_shared<WavClip>();
    clip->left.resize(frames);
    for (uint32_t frame = 0; frame < frames; ++frame) {
        clip->left[frame] = firstSample + static_cast<float>(frame);
    }
    clip->sampleRate = static_cast<uint32_t>(kTestSampleRate);
    clip->displayName = std::move(name);
    return clip;
}
std::shared_ptr<const WavClip> makeTempoRampClip(
    uint32_t frames, double sourceBpm, std::string name = "tempo.wav") {
    auto clip = std::make_shared<WavClip>();
    clip->left.resize(frames);
    for (uint32_t frame = 0; frame < frames; ++frame) {
        clip->left[frame] = 1.0f + static_cast<float>(frame);
    }
    clip->sampleRate = static_cast<uint32_t>(kTestSampleRate);
    clip->sourceBpm = sourceBpm;
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

struct FourChannelBuffers {
    std::array<float, 512> channel0{};
    std::array<float, 512> channel1{};
    std::array<float, 512> channel2{};
    std::array<float, 512> channel3{};
    std::array<float, 512> outputLeft{};
    std::array<float, 512> outputRight{};

    const float* inputs[4] = {
        channel0.data(), channel1.data(), channel2.data(), channel3.data()};
    float* outputs[2] = {outputLeft.data(), outputRight.data()};
};

void clearBuffers(FourChannelBuffers& buffers) {
    buffers.channel0.fill(0.0f);
    buffers.channel1.fill(0.0f);
    buffers.channel2.fill(0.0f);
    buffers.channel3.fill(0.0f);
    buffers.outputLeft.fill(-99.0f);
    buffers.outputRight.fill(-99.0f);
}
void configure(RackGraph& graph, uint32_t capacity = 512,
               float sampleRate = kTestSampleRate, double bpm = kTestBpm) {
    graph.setSampleRate(sampleRate, capacity);
    graph.setBeatsPerMinute(bpm);
}

void clearBuffers(StereoBuffers& buffers) {
    buffers.left.fill(0.0f);
    buffers.right.fill(0.0f);
    buffers.outputLeft.fill(-99.0f);
    buffers.outputRight.fill(-99.0f);
}
bool configureClipLoop(RackGraph& graph, RackPathId track, uint32_t slot,
                       double bars, bool enterOnPunch,
                       guitarrackcraft::LaunchQuantization quantization) {
    return graph.setSlotDefaultLoopLength(track, slot, bars) &&
           graph.setSlotEnterOnPunch(track, slot, enterOnPunch, quantization);
}

bool startConfiguredClipRecording(
    RackGraph& graph, RackPathId track, double bars,
    guitarrackcraft::LaunchQuantization quantization, bool enterOnPunch) {
    return configureClipLoop(graph, track, 0, bars, enterOnPunch, quantization) &&
           graph.startTrackClipRecording(track, 0, quantization);
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

TEST(RackGraphTransportTest, GlobalBpmSnapshotUpdatesWhileTransportStopped) {
    RackGraph graph;
    constexpr double requestedBpm = 137.5;

    graph.setBeatsPerMinute(requestedBpm);

    EXPECT_DOUBLE_EQ(graph.getTransportSnapshot().beatsPerMinute, requestedBpm);
}

TEST(RackGraphTransportTest, EquivalentWallClockProgressHasEqualGlobalQuarterNotes) {
    struct RateCase {
        float sampleRate;
        uint32_t frames;
    };
    const std::array<RateCase, 2> rates = {{
        {44'100.0f, 441u},
        {48'000.0f, 480u},
    }};

    std::array<guitarrackcraft::TransportSnapshot, 2> snapshots{};
    for (size_t index = 0; index < rates.size(); ++index) {
        RackGraph graph;
        configure(graph, 512, rates[index].sampleRate, 60.0);
        ASSERT_TRUE(graph.setTransportPlaying(true));
        StereoBuffers buffers;
        clearBuffers(buffers);
        graph.process(buffers.inputs, 2, buffers.outputs, rates[index].frames);
        snapshots[index] = graph.getTransportSnapshot();
    }

    EXPECT_NEAR(snapshots[0].positionSec, 0.01, 1e-12);
    EXPECT_NEAR(snapshots[1].positionSec, 0.01, 1e-12);
    EXPECT_NEAR(snapshots[0].musicalQuarterNotes, 0.01, 1e-12);
    EXPECT_NEAR(snapshots[1].musicalQuarterNotes, 0.01, 1e-12);
    EXPECT_NEAR(snapshots[0].musicalQuarterNotes,
                snapshots[1].musicalQuarterNotes, 1e-12);
}

TEST(RackGraphTransportTest, PausingFreezesElapsedTimeAndGlobalQuarterNotes) {
    RackGraph graph;
    configure(graph, 512, 48'000.0f, 120.0);
    ASSERT_TRUE(graph.setTransportPlaying(true));

    StereoBuffers buffers;
    clearBuffers(buffers);
    graph.process(buffers.inputs, 2, buffers.outputs, 480);
    const auto beforePause = graph.getTransportSnapshot();
    ASSERT_TRUE(graph.setTransportPlaying(false));
    clearBuffers(buffers);
    graph.process(buffers.inputs, 2, buffers.outputs, 480);
    const auto paused = graph.getTransportSnapshot();

    EXPECT_FALSE(paused.playing);
    EXPECT_EQ(paused.samplePosition, 960u);
    EXPECT_EQ(paused.transportFrame, beforePause.transportFrame);
    EXPECT_DOUBLE_EQ(paused.positionSec, beforePause.positionSec);
    EXPECT_DOUBLE_EQ(paused.musicalQuarterNotes,
                     beforePause.musicalQuarterNotes);
}

TEST(RackGraphTransportTest, SampleRateSwitchPreservesElapsedAndMusicalProgress) {
    RackGraph graph;
    configure(graph, 512, 48'000.0f, 60.0);
    ASSERT_TRUE(graph.setTransportPlaying(true));

    StereoBuffers buffers;
    clearBuffers(buffers);
    graph.process(buffers.inputs, 2, buffers.outputs, 480);
    const auto beforeSwitch = graph.getTransportSnapshot();

    graph.setSampleRate(44'100.0f, 512);
    clearBuffers(buffers);
    graph.process(buffers.inputs, 2, buffers.outputs, 441);
    const auto afterSwitch = graph.getTransportSnapshot();

    EXPECT_NEAR(beforeSwitch.positionSec, 0.01, 1e-12);
    EXPECT_NEAR(afterSwitch.positionSec, 0.02, 1e-12);
    EXPECT_NEAR(beforeSwitch.musicalQuarterNotes, 0.01, 1e-12);
    EXPECT_NEAR(afterSwitch.musicalQuarterNotes, 0.02, 1e-12);
    EXPECT_EQ(afterSwitch.transportFrame, 921u);
    EXPECT_EQ(afterSwitch.samplePosition, 921u);
    EXPECT_FLOAT_EQ(static_cast<float>(afterSwitch.sampleRate), 44'100.0f);
}

TEST(RackGraphTransportTest, GlobalClockIsUnboundedAndHasNoClipEnd) {
    RackGraph graph;
    configure(graph);
    const RackPathId track = graph.getTracks().front().id;
    ASSERT_TRUE(graph.attachTrackWav(track, makeClip({1.0f, 2.0f}, 60)));

    StereoBuffers buffers;
    clearBuffers(buffers);
    graph.setTransportPlaying(true);
    graph.setClipTransportPlaying(track, 0, true, guitarrackcraft::LaunchQuantization::Sixteenth);
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
    graph.setClipLooping(track, 0, true);
    graph.setClipTransportPlaying(track, 0, true, guitarrackcraft::LaunchQuantization::Sixteenth);
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
    graph.setClipTransportPlaying(track, 0, true, guitarrackcraft::LaunchQuantization::Sixteenth);
    clearBuffers(buffers);
    graph.process(buffers.inputs, 2, buffers.outputs, 11);
    EXPECT_FLOAT_EQ(buffers.outputLeft[9], 0.0f);
    EXPECT_FLOAT_EQ(buffers.outputLeft[10], 1.0f);
    EXPECT_TRUE(trackSnapshot(graph, track, tracks).playing);
}
TEST(RackGraphTransportTest, PauseAndResetAtomicallyStopsAndResetsActiveClip) {
    RackGraph graph;
    configure(graph);
    const RackPathId track = graph.getTracks().front().id;
    ASSERT_TRUE(graph.attachTrackWavSlot(track, 0, makeRampClip(64)));

    StereoBuffers buffers;
    clearBuffers(buffers);
    ASSERT_TRUE(graph.setTransportPlaying(true));
    ASSERT_TRUE(graph.setClipTransportPlaying(
        track, 0, true, guitarrackcraft::LaunchQuantization::None));
    graph.process(buffers.inputs, 2, buffers.outputs, 8);

    const auto beforeStop = graph.getTransportSnapshot();
    ASSERT_TRUE(beforeStop.playing);
    ASSERT_EQ(beforeStop.transportFrame, 8u);
    std::vector<TrackClipSlotInfo> slots = graph.getTrackClipSlots(track);
    const auto* active = findClipSlot(slots, 0);
    ASSERT_NE(active, nullptr);
    ASSERT_TRUE(active->active);
    ASSERT_TRUE(active->playing);

    graph.pauseAndResetTransport();
    clearBuffers(buffers);
    graph.process(buffers.inputs, 2, buffers.outputs, 4);
    expectSilence(buffers, 4);

    auto stopped = graph.getTransportSnapshot();
    EXPECT_FALSE(stopped.playing);
    EXPECT_EQ(stopped.transportFrame, 0u);
    slots = graph.getTrackClipSlots(track);
    const auto* paused = findClipSlot(slots, 0);
    ASSERT_NE(paused, nullptr);
    EXPECT_FALSE(paused->playing);
    std::vector<guitarrackcraft::TrackSnapshot> tracks;
    const auto& stoppedTrack = trackSnapshot(graph, track, tracks);
    EXPECT_FALSE(stoppedTrack.playing);
    EXPECT_EQ(stoppedTrack.activeSlot, -1);
    EXPECT_EQ(stoppedTrack.transportFrame, 0u);

    // Every callback remains stopped until an explicit global and clip launch.
    clearBuffers(buffers);
    graph.process(buffers.inputs, 2, buffers.outputs, 4);
    expectSilence(buffers, 4);
    stopped = graph.getTransportSnapshot();
    EXPECT_FALSE(stopped.playing);
    EXPECT_EQ(stopped.transportFrame, 0u);
    slots = graph.getTrackClipSlots(track);
    paused = findClipSlot(slots, 0);
    ASSERT_NE(paused, nullptr);
    EXPECT_FALSE(paused->playing);
    const auto& stillStoppedTrack = trackSnapshot(graph, track, tracks);
    EXPECT_FALSE(stillStoppedTrack.playing);
    EXPECT_EQ(stillStoppedTrack.activeSlot, -1);
    EXPECT_EQ(stillStoppedTrack.transportFrame, 0u);

    ASSERT_TRUE(graph.setTransportPlaying(true));
    ASSERT_TRUE(graph.setClipTransportPlaying(
        track, 0, true, guitarrackcraft::LaunchQuantization::None));
    clearBuffers(buffers);
    graph.process(buffers.inputs, 2, buffers.outputs, 1);
    const auto relaunched = graph.getTransportSnapshot();
    EXPECT_TRUE(relaunched.playing);
    EXPECT_EQ(relaunched.transportFrame, 1u);
    slots = graph.getTrackClipSlots(track);
    const auto* resumed = findClipSlot(slots, 0);
    ASSERT_NE(resumed, nullptr);
    EXPECT_TRUE(resumed->active);
    EXPECT_TRUE(resumed->playing);
    EXPECT_EQ(resumed->transportFrame, 1u);
}

TEST(RackGraphTransportTest, TracksKeepIndependentPlayheadsAndLoopLocally) {
    RackGraph graph;
    configure(graph);
    const RackPathId first = graph.getTracks().front().id;
    const RackPathId second = graph.addTrack();
    ASSERT_NE(second, 0u);
    ASSERT_TRUE(graph.attachTrackWav(first, makeRampClip(90, 1.0f, "loop.wav")));
    ASSERT_TRUE(graph.attachTrackWav(second, makeClip(
        {10.0f, 20.0f, 30.0f}, 60, {}, "one-shot.wav")));

    StereoBuffers buffers;
    clearBuffers(buffers);
    graph.setTransportPlaying(true);
    ASSERT_TRUE(graph.setClipLoopLength(first, 0, 0.25));
    ASSERT_TRUE(graph.setClipLooping(first, 0, true));
    ASSERT_TRUE(graph.setClipLooping(second, 0, false));
    graph.setClipTransportPlaying(first, 0, true, guitarrackcraft::LaunchQuantization::Sixteenth);
    graph.setClipTransportPlaying(second, 0, true, guitarrackcraft::LaunchQuantization::Eighth);
    graph.process(buffers.inputs, 2, buffers.outputs, 77);

    // First launches at global frame 15 and wraps at its 60-frame musical boundary;
    // second launches at frame 30 and stops at its EOF.
    EXPECT_FLOAT_EQ(buffers.outputLeft[14], 0.0f);
    EXPECT_FLOAT_EQ(buffers.outputLeft[15], 1.0f);
    EXPECT_FLOAT_EQ(buffers.outputLeft[16], 2.0f);
    EXPECT_FLOAT_EQ(buffers.outputLeft[29], 15.0f);
    EXPECT_FLOAT_EQ(buffers.outputLeft[30], 26.0f);
    EXPECT_FLOAT_EQ(buffers.outputLeft[31], 37.0f);
    EXPECT_FLOAT_EQ(buffers.outputLeft[74], 60.0f);
    EXPECT_FLOAT_EQ(buffers.outputLeft[75], 1.0f);
    EXPECT_FLOAT_EQ(buffers.outputLeft[76], 2.0f);

    std::vector<guitarrackcraft::TrackSnapshot> tracks;
    const auto& firstState = trackSnapshot(graph, first, tracks);
    const uint64_t firstFrame = firstState.transportFrame;
    const double firstPosition = firstState.positionSec;
    EXPECT_TRUE(firstState.playing);
    EXPECT_TRUE(firstState.looping);
    EXPECT_EQ(firstFrame, 2u);
    EXPECT_DOUBLE_EQ(firstPosition, 2.0 / 60.0);

    const auto& secondState = trackSnapshot(graph, second, tracks);
    EXPECT_FALSE(secondState.playing);
    EXPECT_FALSE(secondState.looping);
    EXPECT_EQ(secondState.transportFrame, 3u);
    EXPECT_DOUBLE_EQ(secondState.positionSec, 3.0 / 60.0);
    EXPECT_NE(firstFrame, secondState.transportFrame);
    EXPECT_FALSE(firstState.recordPending);
    EXPECT_FALSE(firstState.recording);
    EXPECT_FALSE(secondState.recordPending);
    EXPECT_FALSE(secondState.recording);
    EXPECT_NE(firstPosition, secondState.positionSec);

    graph.process(buffers.inputs, 2, buffers.outputs, 3);
    EXPECT_FLOAT_EQ(buffers.outputLeft[0], 3.0f);
    EXPECT_FLOAT_EQ(buffers.outputLeft[1], 4.0f);
    EXPECT_FLOAT_EQ(buffers.outputLeft[2], 5.0f);
    EXPECT_TRUE(trackSnapshot(graph, first, tracks).playing);
    EXPECT_FALSE(trackSnapshot(graph, second, tracks).playing);
}

TEST(RackGraphTransportTest, CrossSlotLaunchesAlwaysStartTargetAtFrameZero) {
    RackGraph graph;
    configure(graph);
    const RackPathId track = graph.getTracks().front().id;
    ASSERT_TRUE(graph.attachTrackWavSlot(track, 0, makeClip({1.0f, 2.0f, 3.0f, 4.0f}, 60)));
    ASSERT_TRUE(graph.attachTrackWavSlot(track, 1, makeClip({10.0f, 20.0f, 30.0f, 40.0f}, 60)));
    ASSERT_TRUE(graph.setClipLooping(track, 0, true));
    ASSERT_TRUE(graph.setClipLooping(track, 1, true));
    ASSERT_TRUE(graph.setTransportPlaying(true));
    ASSERT_TRUE(graph.setClipTransportPlaying(
        track, 0, true, guitarrackcraft::LaunchQuantization::None));

    StereoBuffers buffers;
    clearBuffers(buffers);
    graph.process(buffers.inputs, 2, buffers.outputs, 2);
    EXPECT_FLOAT_EQ(buffers.outputLeft[0], 1.0f);
    EXPECT_FLOAT_EQ(buffers.outputLeft[1], 2.0f);

    ASSERT_TRUE(graph.setClipTransportPlaying(
        track, 0, false, guitarrackcraft::LaunchQuantization::None));
    ASSERT_TRUE(graph.selectTrackClipSlot(track, 1));
    ASSERT_TRUE(graph.setClipTransportPlaying(
        track, 1, true, guitarrackcraft::LaunchQuantization::None));
    clearBuffers(buffers);
    graph.process(buffers.inputs, 2, buffers.outputs, 1);
    EXPECT_FLOAT_EQ(buffers.outputLeft[0], 10.0f);

    ASSERT_TRUE(graph.setClipTransportPlaying(
        track, 1, false, guitarrackcraft::LaunchQuantization::None));
    ASSERT_TRUE(graph.selectTrackClipSlot(track, 0));
    ASSERT_TRUE(graph.setClipTransportPlaying(
        track, 0, true, guitarrackcraft::LaunchQuantization::None));
    clearBuffers(buffers);
    graph.process(buffers.inputs, 2, buffers.outputs, 1);
    EXPECT_FLOAT_EQ(buffers.outputLeft[0], 1.0f);

    const auto slots = graph.getTrackClipSlots(track);
    const auto* first = findClipSlot(slots, 0);
    const auto* second = findClipSlot(slots, 1);
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    EXPECT_TRUE(first->active);
    EXPECT_TRUE(first->playing);
    EXPECT_EQ(first->transportFrame, 1u);
    EXPECT_DOUBLE_EQ(first->positionSec, 1.0 / 60.0);
    EXPECT_FALSE(second->active);
    EXPECT_FALSE(second->playing);
    EXPECT_EQ(second->transportFrame, 1u);
    EXPECT_DOUBLE_EQ(second->positionSec, 1.0 / 60.0);
}

TEST(RackGraphTransportTest, LoopingUsesShorterPerSlotMusicalBoundaryThanEof) {
    RackGraph graph;
    configure(graph);
    const RackPathId track = graph.getTracks().front().id;
    ASSERT_TRUE(graph.attachTrackWavSlot(track, 0, makeRampClip(90)));
    ASSERT_TRUE(graph.setClipLoopLength(track, 0, 0.25));
    ASSERT_TRUE(graph.setClipLooping(track, 0, true));
    ASSERT_TRUE(graph.setTransportPlaying(true));
    ASSERT_TRUE(graph.setClipTransportPlaying(
        track, 0, true, guitarrackcraft::LaunchQuantization::None));

    StereoBuffers buffers;
    clearBuffers(buffers);
    graph.process(buffers.inputs, 2, buffers.outputs, 61);
    for (uint32_t frame = 0; frame < 60; ++frame) {
        EXPECT_FLOAT_EQ(buffers.outputLeft[frame], 1.0f + static_cast<float>(frame))
            << "frame " << frame;
    }
    EXPECT_FLOAT_EQ(buffers.outputLeft[60], 1.0f);

    const auto slots = graph.getTrackClipSlots(track);
    const auto* state = findClipSlot(slots, 0);
    ASSERT_NE(state, nullptr);
    EXPECT_TRUE(state->active);
    EXPECT_TRUE(state->playing);
    EXPECT_TRUE(state->looping);
    EXPECT_DOUBLE_EQ(state->loopLengthBars, 0.25);
    EXPECT_EQ(state->transportFrame, 1u);
    EXPECT_DOUBLE_EQ(state->positionSec, 1.0 / 60.0);
}

TEST(RackGraphTransportTest, LoopStartOnlyAffectsLoopingAndQuarterNoteMinimumIsExact) {
    RackGraph graph;
    configure(graph);
    const RackPathId track = graph.getTracks().front().id;
    ASSERT_TRUE(graph.attachTrackWavSlot(
        track, 0, makeTempoRampClip(120, 60.0, "qn-loop.wav")));
    ASSERT_TRUE(graph.setClipLoopStartQuarterNotes(track, 0, 1.0));
    EXPECT_FALSE(graph.setClipLoopLengthQuarterNotes(track, 0, 0.249));
    ASSERT_TRUE(graph.setClipLoopLengthQuarterNotes(track, 0, 0.25));
    ASSERT_TRUE(graph.setClipLooping(track, 0, true));
    ASSERT_TRUE(graph.setTransportPlaying(true));
    ASSERT_TRUE(graph.setClipTransportPlaying(
        track, 0, true, guitarrackcraft::LaunchQuantization::None));

    StereoBuffers buffers;
    clearBuffers(buffers);
    graph.process(buffers.inputs, 2, buffers.outputs, 16);
    for (uint32_t frame = 0; frame < 15; ++frame) {
        EXPECT_FLOAT_EQ(buffers.outputLeft[frame], 61.0f + frame)
            << "frame " << frame;
    }
    EXPECT_FLOAT_EQ(buffers.outputLeft[15], 61.0f);

    auto slots = graph.getTrackClipSlots(track);
    const auto* looping = findClipSlot(slots, 0);
    ASSERT_NE(looping, nullptr);
    EXPECT_DOUBLE_EQ(looping->loopStartQuarterNotes, 1.0);
    EXPECT_DOUBLE_EQ(looping->loopLengthQuarterNotes, 0.25);

    ASSERT_TRUE(graph.setClipTransportPlaying(
        track, 0, false, guitarrackcraft::LaunchQuantization::None));
    clearBuffers(buffers);
    graph.process(buffers.inputs, 2, buffers.outputs, 1);
    ASSERT_TRUE(graph.setClipLooping(track, 0, false));
    ASSERT_TRUE(graph.setClipTransportPlaying(
        track, 0, true, guitarrackcraft::LaunchQuantization::None));
    clearBuffers(buffers);
    graph.process(buffers.inputs, 2, buffers.outputs, 1);
    EXPECT_FLOAT_EQ(buffers.outputLeft[0], 1.0f);
}

TEST(RackGraphTransportTest, LoopingLongerThanEofLeavesSilenceUntilNextBoundary) {
    RackGraph graph;
    configure(graph);
    const RackPathId track = graph.getTracks().front().id;
    ASSERT_TRUE(graph.attachTrackWavSlot(track, 0, makeRampClip(30)));
    ASSERT_TRUE(graph.setClipLoopLength(track, 0, 0.25));
    ASSERT_TRUE(graph.setClipLooping(track, 0, true));
    ASSERT_TRUE(graph.setTransportPlaying(true));
    ASSERT_TRUE(graph.setClipTransportPlaying(
        track, 0, true, guitarrackcraft::LaunchQuantization::None));

    StereoBuffers buffers;
    clearBuffers(buffers);
    graph.process(buffers.inputs, 2, buffers.outputs, 61);
    for (uint32_t frame = 0; frame < 30; ++frame) {
        EXPECT_FLOAT_EQ(buffers.outputLeft[frame], 1.0f + static_cast<float>(frame))
            << "frame " << frame;
    }
    for (uint32_t frame = 30; frame < 60; ++frame) {
        EXPECT_FLOAT_EQ(buffers.outputLeft[frame], 0.0f) << "frame " << frame;
    }
    EXPECT_FLOAT_EQ(buffers.outputLeft[60], 1.0f);

    const auto slots = graph.getTrackClipSlots(track);
    const auto* state = findClipSlot(slots, 0);
    ASSERT_NE(state, nullptr);
    EXPECT_TRUE(state->playing);
    EXPECT_TRUE(state->looping);
    EXPECT_EQ(state->transportFrame, 1u);
}

TEST(RackGraphTransportTest, DisablingSlotLoopingStopsAtEof) {
    RackGraph graph;
    configure(graph);
    const RackPathId track = graph.getTracks().front().id;
    ASSERT_TRUE(graph.attachTrackWavSlot(track, 0, makeClip({4.0f, 5.0f, 6.0f}, 60)));
    ASSERT_TRUE(graph.setClipLoopLength(track, 0, 0.25));
    ASSERT_TRUE(graph.setClipLooping(track, 0, false));
    ASSERT_TRUE(graph.setTransportPlaying(true));
    ASSERT_TRUE(graph.setClipTransportPlaying(
        track, 0, true, guitarrackcraft::LaunchQuantization::None));

    StereoBuffers buffers;
    clearBuffers(buffers);
    graph.process(buffers.inputs, 2, buffers.outputs, 4);
    EXPECT_FLOAT_EQ(buffers.outputLeft[0], 4.0f);
    EXPECT_FLOAT_EQ(buffers.outputLeft[1], 5.0f);
    EXPECT_FLOAT_EQ(buffers.outputLeft[2], 6.0f);
    EXPECT_FLOAT_EQ(buffers.outputLeft[3], 0.0f);

    const auto slots = graph.getTrackClipSlots(track);
    const auto* state = findClipSlot(slots, 0);
    ASSERT_NE(state, nullptr);
    EXPECT_FALSE(state->playing);
    EXPECT_FALSE(state->looping);
    EXPECT_EQ(state->transportFrame, 3u);
    EXPECT_DOUBLE_EQ(state->positionSec, 3.0 / 60.0);
}

TEST(RackGraphTransportTest, ClipLoopConfigurationsRemainIndependent) {
    RackGraph graph;
    configure(graph);
    const RackPathId track = graph.getTracks().front().id;
    ASSERT_TRUE(graph.attachTrackWavSlot(track, 0, makeRampClip(300, 1.0f, "short.wav")));
    ASSERT_TRUE(graph.attachTrackWavSlot(track, 1, makeRampClip(300, 101.0f, "long.wav")));
    ASSERT_TRUE(graph.setClipLoopLength(track, 0, 0.25));
    ASSERT_TRUE(graph.setClipLoopLength(track, 1, 1.0));
    ASSERT_TRUE(graph.setClipLooping(track, 0, true));
    ASSERT_TRUE(graph.setClipLooping(track, 1, true));

    const auto configured = graph.getTrackClipSlots(track);
    const auto* shortLoop = findClipSlot(configured, 0);
    const auto* longLoop = findClipSlot(configured, 1);
    ASSERT_NE(shortLoop, nullptr);
    ASSERT_NE(longLoop, nullptr);
    EXPECT_DOUBLE_EQ(shortLoop->loopLengthBars, 0.25);
    EXPECT_DOUBLE_EQ(longLoop->loopLengthBars, 1.0);

    ASSERT_TRUE(graph.setTransportPlaying(true));
    ASSERT_TRUE(graph.setClipTransportPlaying(
        track, 0, true, guitarrackcraft::LaunchQuantization::None));
    StereoBuffers buffers;
    clearBuffers(buffers);
    graph.process(buffers.inputs, 2, buffers.outputs, 61);
    EXPECT_FLOAT_EQ(buffers.outputLeft[60], 1.0f);

    ASSERT_TRUE(graph.setClipTransportPlaying(
        track, 0, false, guitarrackcraft::LaunchQuantization::None));
    ASSERT_TRUE(graph.selectTrackClipSlot(track, 1));
    ASSERT_TRUE(graph.setClipTransportPlaying(
        track, 1, true, guitarrackcraft::LaunchQuantization::None));
    clearBuffers(buffers);
    graph.process(buffers.inputs, 2, buffers.outputs, 61);
    EXPECT_FLOAT_EQ(buffers.outputLeft[60], 161.0f);
}

TEST(RackGraphTransportTest, SlotDefaultLoopLengthSeedsClipRuntimePerSlot) {
    RackGraph graph;
    configure(graph);
    const RackPathId track = graph.getTracks().front().id;

    std::vector<guitarrackcraft::TrackSnapshot> tracks;
    const auto& initial = trackSnapshot(graph, track, tracks);
    EXPECT_EQ(initial.selectedSlot, 0u);
    EXPECT_DOUBLE_EQ(initial.defaultLoopLengthBars, 1.0);
    ASSERT_TRUE(graph.setSlotDefaultLoopLength(track, 2, 4.0));
    ASSERT_TRUE(graph.attachTrackWavSlot(track, 2, makeClip({1.0f, 2.0f}, 60)));

    auto slots = graph.getTrackClipSlots(track);
    const auto* firstConfigured = findClipSlot(slots, 2);
    ASSERT_NE(firstConfigured, nullptr);
    EXPECT_EQ(firstConfigured->trackId, track);
    EXPECT_EQ(firstConfigured->slot, 2u);
    EXPECT_DOUBLE_EQ(firstConfigured->loopLengthBars, 4.0);
    EXPECT_DOUBLE_EQ(firstConfigured->defaultLoopLengthBars, 4.0);

    ASSERT_TRUE(graph.setSlotDefaultLoopLength(track, 3, 8.0));
    ASSERT_TRUE(graph.attachTrackWavSlot(track, 3, makeClip({3.0f, 4.0f}, 60)));
    slots = graph.getTrackClipSlots(track);
    const auto* oldConfigured = findClipSlot(slots, 2);
    const auto* secondConfigured = findClipSlot(slots, 3);
    ASSERT_NE(oldConfigured, nullptr);
    ASSERT_NE(secondConfigured, nullptr);
    EXPECT_DOUBLE_EQ(oldConfigured->loopLengthBars, 4.0);
    EXPECT_DOUBLE_EQ(oldConfigured->defaultLoopLengthBars, 4.0);
    EXPECT_DOUBLE_EQ(secondConfigured->loopLengthBars, 8.0);
    EXPECT_DOUBLE_EQ(secondConfigured->defaultLoopLengthBars, 8.0);
}
TEST(RackGraphClipSlotConfigTest, ImportedMediaLoopLengthUsesFullSourceDurationForWavAndMidi) {
    RackGraph graph;
    configure(graph);
    const RackPathId track = graph.getTracks().front().id;
    constexpr double defaultLoopBars = 4.0;
    constexpr double expectedSourceQuarterNotes = 3.0;

    ASSERT_TRUE(graph.setSlotDefaultLoopLength(track, 0, defaultLoopBars));
    ASSERT_TRUE(graph.setSlotDefaultLoopLength(track, 1, defaultLoopBars));
    ASSERT_TRUE(graph.attachTrackWavSlot(
        track, 0, makeTempoRampClip(90, 120.0, "source-duration.wav")));
    ASSERT_TRUE(graph.attachTrackMidiSlot(
        track, 1, makeMidiClip(1'500'000, "source-duration.mid")));

    const auto slots = graph.getTrackClipSlots(track);
    const auto* wav = findClipSlot(slots, 0);
    const auto* midi = findClipSlot(slots, 1);
    ASSERT_NE(wav, nullptr);
    ASSERT_NE(midi, nullptr);

    EXPECT_DOUBLE_EQ(wav->defaultLoopLengthBars, defaultLoopBars);
    EXPECT_DOUBLE_EQ(wav->loopLengthBars, defaultLoopBars);
    EXPECT_DOUBLE_EQ(wav->loopStartQuarterNotes, 0.0);
    EXPECT_DOUBLE_EQ(wav->loopLengthQuarterNotes, expectedSourceQuarterNotes);

    EXPECT_DOUBLE_EQ(midi->defaultLoopLengthBars, defaultLoopBars);
    EXPECT_DOUBLE_EQ(midi->loopLengthBars, defaultLoopBars);
    EXPECT_DOUBLE_EQ(midi->loopStartQuarterNotes, 0.0);
    EXPECT_DOUBLE_EQ(midi->loopLengthQuarterNotes, expectedSourceQuarterNotes);
}

TEST(RackGraphClipSlotConfigTest, SlotConfigSurvivesClipLifecycleAndClipStateStaysIndependent) {
    RackGraph graph;
    configure(graph);
    const RackPathId track = graph.getTracks().front().id;
    constexpr uint32_t slot = 2;

    // Clip controls have no target until a clip exists; slot controls do not.
    EXPECT_FALSE(graph.setClipLoopLength(track, slot, 2.0));
    EXPECT_FALSE(graph.setClipLooping(track, slot, true));
    ASSERT_TRUE(graph.setSlotDefaultLoopLength(track, slot, 4.0));
    ASSERT_TRUE(graph.setSlotEnterOnPunch(
        track, slot, true, guitarrackcraft::LaunchQuantization::Quarter));
    constexpr uint32_t otherSlot = 3;
    ASSERT_TRUE(graph.setSlotDefaultLoopLength(track, otherSlot, 8.0));
    ASSERT_TRUE(graph.setSlotEnterOnPunch(
        track, otherSlot, false, guitarrackcraft::LaunchQuantization::None));
    ASSERT_TRUE(graph.attachTrackWavSlot(
        track, otherSlot, makeClip({11.0f, 12.0f}, 60, {}, "other.wav")));

    auto slots = graph.getTrackClipSlots(track);
    const auto* empty = findClipSlot(slots, slot);
    ASSERT_NE(empty, nullptr);
    EXPECT_FALSE(empty->wavLoaded);
    EXPECT_FALSE(empty->looping);
    EXPECT_DOUBLE_EQ(empty->loopLengthBars, 4.0);
    const auto* other = findClipSlot(slots, otherSlot);
    ASSERT_NE(other, nullptr);
    EXPECT_TRUE(other->wavLoaded);
    EXPECT_FALSE(other->looping);
    EXPECT_DOUBLE_EQ(other->loopLengthBars, 8.0);
    EXPECT_DOUBLE_EQ(other->defaultLoopLengthBars, 8.0);
    EXPECT_FALSE(other->enterOnPunch);
    EXPECT_DOUBLE_EQ(empty->defaultLoopLengthBars, 4.0);
    EXPECT_TRUE(empty->enterOnPunch);

    ASSERT_TRUE(graph.attachTrackWavSlot(
        track, slot, makeClip({3.0f, 4.0f}, 60, {}, "imported.wav")));
    slots = graph.getTrackClipSlots(track);
    const auto* imported = findClipSlot(slots, slot);
    ASSERT_NE(imported, nullptr);
    EXPECT_TRUE(imported->wavLoaded);
    EXPECT_FALSE(imported->looping);
    EXPECT_DOUBLE_EQ(imported->loopLengthBars, 4.0);
    EXPECT_DOUBLE_EQ(imported->defaultLoopLengthBars, 4.0);
    EXPECT_TRUE(imported->enterOnPunch);

    ASSERT_TRUE(graph.setClipLoopLength(track, slot, 2.0));
    ASSERT_TRUE(graph.setClipLooping(track, slot, true));
    slots = graph.getTrackClipSlots(track);
    const auto* changed = findClipSlot(slots, slot);
    ASSERT_NE(changed, nullptr);
    EXPECT_TRUE(changed->looping);
    EXPECT_DOUBLE_EQ(changed->loopLengthBars, 2.0);
    EXPECT_DOUBLE_EQ(changed->defaultLoopLengthBars, 4.0);
    EXPECT_TRUE(changed->enterOnPunch);
    // Replacing media must create a fresh runtime, not retain clip toggles.
    ASSERT_TRUE(graph.attachTrackWavSlot(
        track, slot, makeClip({5.0f, 6.0f}, 60, {}, "direct-replacement.wav")));
    slots = graph.getTrackClipSlots(track);
    const auto* directReplacement = findClipSlot(slots, slot);
    ASSERT_NE(directReplacement, nullptr);
    EXPECT_FALSE(directReplacement->looping);
    EXPECT_DOUBLE_EQ(directReplacement->loopLengthBars, 4.0);
    EXPECT_DOUBLE_EQ(directReplacement->defaultLoopLengthBars, 4.0);
    EXPECT_TRUE(directReplacement->enterOnPunch);

    ASSERT_TRUE(graph.unloadTrackWavSlot(track, slot));
    slots = graph.getTrackClipSlots(track);
    const auto* unloaded = findClipSlot(slots, slot);
    ASSERT_NE(unloaded, nullptr);
    EXPECT_FALSE(unloaded->wavLoaded);
    EXPECT_FALSE(unloaded->looping);
    EXPECT_DOUBLE_EQ(unloaded->loopLengthBars, 4.0);
    EXPECT_DOUBLE_EQ(unloaded->defaultLoopLengthBars, 4.0);
    EXPECT_TRUE(unloaded->enterOnPunch);

    ASSERT_TRUE(graph.attachTrackWavSlot(
        track, slot, makeClip({7.0f, 8.0f}, 60, {}, "replacement.wav")));
    slots = graph.getTrackClipSlots(track);
    const auto* replacement = findClipSlot(slots, slot);
    ASSERT_NE(replacement, nullptr);
    EXPECT_TRUE(replacement->wavLoaded);
    EXPECT_FALSE(replacement->looping);
    EXPECT_DOUBLE_EQ(replacement->loopLengthBars, 4.0);
    EXPECT_DOUBLE_EQ(replacement->defaultLoopLengthBars, 4.0);
    EXPECT_TRUE(replacement->enterOnPunch);

    ASSERT_TRUE(graph.setTransportPlaying(true));
    ASSERT_TRUE(graph.setClipTransportPlaying(
        track, slot, true, guitarrackcraft::LaunchQuantization::None));
    StereoBuffers buffers;
    clearBuffers(buffers);
    allocation_probe::allocations = 0;
    allocation_probe::enabled = true;
    graph.process(buffers.inputs, 2, buffers.outputs, 2);
    allocation_probe::enabled = false;
    EXPECT_EQ(allocation_probe::allocations, 0u);
    EXPECT_FLOAT_EQ(buffers.outputLeft[0], 7.0f);
}

TEST(RackGraphTransportTest, LaunchRequestWhileGlobalStoppedDoesNotQueueAnImplicitLaunch) {
    RackGraph graph;
    configure(graph);
    const RackPathId track = graph.getTracks().front().id;
    ASSERT_TRUE(graph.attachTrackWav(track, makeClip({5.0f, 6.0f}, 60)));

    StereoBuffers buffers;
    clearBuffers(buffers);
    graph.setClipTransportPlaying(track, 0, true, guitarrackcraft::LaunchQuantization::Quarter);
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
        graph.setClipTransportPlaying(track, 0, true, testCase.quantization);
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
TEST(RackGraphTransportTest,
     QuantizedNewLaunchReportsPendingUntilBoundaryAndCancelClearsIt) {
    RackGraph graph;
    configure(graph);
    const RackPathId targetTrack = graph.getTracks().front().id;
    const RackPathId otherTrack = graph.addTrack();
    ASSERT_NE(otherTrack, 0u);
    ASSERT_TRUE(graph.attachTrackWavSlot(
        targetTrack, 0, makeRampClip(128, 1.0f, "target.wav")));
    ASSERT_TRUE(graph.attachTrackWavSlot(
        otherTrack, 0, makeRampClip(128, 101.0f, "other.wav")));
    ASSERT_TRUE(graph.setClipLooping(targetTrack, 0, true));
    ASSERT_TRUE(graph.setClipLooping(otherTrack, 0, true));
    ASSERT_TRUE(graph.setTransportPlaying(true));
    ASSERT_TRUE(graph.setClipTransportPlaying(
        otherTrack, 0, true, guitarrackcraft::LaunchQuantization::None));

    StereoBuffers buffers;
    clearBuffers(buffers);
    graph.process(buffers.inputs, 2, buffers.outputs, 1);
    auto otherSlots = graph.getTrackClipSlots(otherTrack);
    const auto* otherPlaying = findClipSlot(otherSlots, 0);
    ASSERT_NE(otherPlaying, nullptr);
    ASSERT_TRUE(otherPlaying->playing);
    EXPECT_FALSE(otherPlaying->launchPending);

    ASSERT_TRUE(graph.setClipTransportPlaying(
        targetTrack, 0, true, guitarrackcraft::LaunchQuantization::Quarter));
    auto targetSlots = graph.getTrackClipSlots(targetTrack);
    const auto* targetPending = findClipSlot(targetSlots, 0);
    ASSERT_NE(targetPending, nullptr);
    EXPECT_TRUE(targetPending->launchPending);
    EXPECT_FALSE(targetPending->playing);

    clearBuffers(buffers);
    graph.process(buffers.inputs, 2, buffers.outputs, 1);
    targetSlots = graph.getTrackClipSlots(targetTrack);
    targetPending = findClipSlot(targetSlots, 0);
    ASSERT_NE(targetPending, nullptr);
    EXPECT_TRUE(targetPending->launchPending);
    EXPECT_FALSE(targetPending->playing);
    otherSlots = graph.getTrackClipSlots(otherTrack);
    otherPlaying = findClipSlot(otherSlots, 0);
    ASSERT_NE(otherPlaying, nullptr);
    EXPECT_FALSE(otherPlaying->launchPending);
    EXPECT_TRUE(otherPlaying->playing);

    ASSERT_TRUE(graph.setClipTransportPlaying(
        targetTrack, 0, false, guitarrackcraft::LaunchQuantization::Quarter));
    targetSlots = graph.getTrackClipSlots(targetTrack);
    targetPending = findClipSlot(targetSlots, 0);
    ASSERT_NE(targetPending, nullptr);
    EXPECT_FALSE(targetPending->launchPending);
    EXPECT_FALSE(targetPending->playing);

    ASSERT_TRUE(graph.setClipTransportPlaying(
        targetTrack, 0, true, guitarrackcraft::LaunchQuantization::Quarter));
    targetSlots = graph.getTrackClipSlots(targetTrack);
    targetPending = findClipSlot(targetSlots, 0);
    ASSERT_NE(targetPending, nullptr);
    EXPECT_TRUE(targetPending->launchPending);

    // The request at transport frame 2 launches at the strict next quarter
    // boundary, global frame 60.
    clearBuffers(buffers);
    graph.process(buffers.inputs, 2, buffers.outputs, 58);
    targetSlots = graph.getTrackClipSlots(targetTrack);
    targetPending = findClipSlot(targetSlots, 0);
    ASSERT_NE(targetPending, nullptr);
    EXPECT_TRUE(targetPending->launchPending);
    EXPECT_FALSE(targetPending->playing);

    clearBuffers(buffers);
    graph.process(buffers.inputs, 2, buffers.outputs, 1);
    targetSlots = graph.getTrackClipSlots(targetTrack);
    targetPending = findClipSlot(targetSlots, 0);
    ASSERT_NE(targetPending, nullptr);
    EXPECT_FALSE(targetPending->launchPending);
    EXPECT_TRUE(targetPending->playing);
    EXPECT_EQ(targetPending->transportFrame, 1u);
    otherSlots = graph.getTrackClipSlots(otherTrack);
    otherPlaying = findClipSlot(otherSlots, 0);
    ASSERT_NE(otherPlaying, nullptr);
    EXPECT_FALSE(otherPlaying->launchPending);
    EXPECT_TRUE(otherPlaying->playing);
}

TEST(RackGraphTransportTest,
     QuantizedActiveRestartKeepsPlayingAndPendingUntilBoundary) {
    RackGraph graph;
    configure(graph);
    const RackPathId track = graph.getTracks().front().id;
    ASSERT_TRUE(graph.attachTrackWavSlot(
        track, 0, makeRampClip(128, 1.0f, "restart.wav")));
    ASSERT_TRUE(graph.setClipLooping(track, 0, true));
    ASSERT_TRUE(graph.setTransportPlaying(true));
    ASSERT_TRUE(graph.setClipTransportPlaying(
        track, 0, true, guitarrackcraft::LaunchQuantization::None));

    StereoBuffers buffers;
    clearBuffers(buffers);
    graph.process(buffers.inputs, 2, buffers.outputs, 4);
    auto slots = graph.getTrackClipSlots(track);
    const auto* state = findClipSlot(slots, 0);
    ASSERT_NE(state, nullptr);
    ASSERT_TRUE(state->playing);
    EXPECT_FALSE(state->launchPending);

    ASSERT_TRUE(graph.setClipTransportPlaying(
        track, 0, true, guitarrackcraft::LaunchQuantization::Quarter));
    slots = graph.getTrackClipSlots(track);
    state = findClipSlot(slots, 0);
    ASSERT_NE(state, nullptr);
    EXPECT_TRUE(state->playing);
    EXPECT_TRUE(state->launchPending);

    clearBuffers(buffers);
    graph.process(buffers.inputs, 2, buffers.outputs, 1);
    slots = graph.getTrackClipSlots(track);
    state = findClipSlot(slots, 0);
    ASSERT_NE(state, nullptr);
    EXPECT_TRUE(state->playing);
    EXPECT_TRUE(state->launchPending);

    // The active restart is applied at global frame 60.
    clearBuffers(buffers);
    graph.process(buffers.inputs, 2, buffers.outputs, 55);
    slots = graph.getTrackClipSlots(track);
    state = findClipSlot(slots, 0);
    ASSERT_NE(state, nullptr);
    EXPECT_TRUE(state->playing);
    EXPECT_TRUE(state->launchPending);

    clearBuffers(buffers);
    graph.process(buffers.inputs, 2, buffers.outputs, 1);
    slots = graph.getTrackClipSlots(track);
    state = findClipSlot(slots, 0);
    ASSERT_NE(state, nullptr);
    EXPECT_TRUE(state->playing);
    EXPECT_FALSE(state->launchPending);
    EXPECT_EQ(state->transportFrame, 1u);
}

TEST(RackGraphTransportTest, RelaunchingActiveClipWithNoneRestartsAtFrameZero) {
    RackGraph graph;
    configure(graph);
    const RackPathId track = graph.getTracks().front().id;
    ASSERT_TRUE(graph.attachTrackWavSlot(
        track, 0, makeRampClip(128, 1.0f, "relaunch.wav")));
    ASSERT_TRUE(graph.setClipLooping(track, 0, true));
    ASSERT_TRUE(graph.setTransportPlaying(true));
    ASSERT_TRUE(graph.setClipTransportPlaying(
        track, 0, true, guitarrackcraft::LaunchQuantization::None));

    StereoBuffers buffers;
    clearBuffers(buffers);
    graph.process(buffers.inputs, 2, buffers.outputs, 4);
    EXPECT_FLOAT_EQ(buffers.outputLeft[0], 1.0f);
    EXPECT_FLOAT_EQ(buffers.outputLeft[3], 4.0f);

    // Repeating the launch of the active slot is a restart, not a stop.
    ASSERT_TRUE(graph.setClipTransportPlaying(
        track, 0, true, guitarrackcraft::LaunchQuantization::None));
    clearBuffers(buffers);
    graph.process(buffers.inputs, 2, buffers.outputs, 1);

    EXPECT_FLOAT_EQ(buffers.outputLeft[0], 1.0f);
    std::vector<TrackClipSlotInfo> slots = graph.getTrackClipSlots(track);
    const auto* state = findClipSlot(slots, 0);
    ASSERT_NE(state, nullptr);
    EXPECT_TRUE(state->playing);
    EXPECT_EQ(state->transportFrame, 1u);
    EXPECT_NEAR(state->musicalQuarterNotes, 1.0 / 60.0, 1e-12);
}

TEST(RackGraphTransportTest,
     QuantizedRelaunchKeepsActiveAndOtherTrackPlayingUntilBoundary) {
    RackGraph graph;
    configure(graph);
    const RackPathId relaunchedTrack = graph.getTracks().front().id;
    const RackPathId otherTrack = graph.addTrack();
    ASSERT_NE(otherTrack, 0u);
    ASSERT_TRUE(graph.attachTrackWavSlot(
        relaunchedTrack, 0, makeRampClip(128, 1.0f, "relaunch.wav")));
    ASSERT_TRUE(graph.attachTrackWavSlot(
        otherTrack, 0, makeRampClip(128, 201.0f, "other.wav")));
    ASSERT_TRUE(graph.setClipLooping(relaunchedTrack, 0, true));
    ASSERT_TRUE(graph.setClipLooping(otherTrack, 0, true));
    ASSERT_TRUE(graph.setTransportPlaying(true));
    ASSERT_TRUE(graph.setClipTransportPlaying(
        relaunchedTrack, 0, true, guitarrackcraft::LaunchQuantization::None));
    ASSERT_TRUE(graph.setClipTransportPlaying(
        otherTrack, 0, true, guitarrackcraft::LaunchQuantization::None));

    StereoBuffers buffers;
    clearBuffers(buffers);
    graph.process(buffers.inputs, 2, buffers.outputs, 10);
    EXPECT_FLOAT_EQ(buffers.outputLeft[0], 202.0f);
    EXPECT_FLOAT_EQ(buffers.outputLeft[9], 220.0f);

    ASSERT_TRUE(graph.setClipTransportPlaying(
        relaunchedTrack, 0, true, guitarrackcraft::LaunchQuantization::Quarter));

    // The active clip keeps producing its old samples while the restart is
    // pending; the other track remains active as well.
    clearBuffers(buffers);
    graph.process(buffers.inputs, 2, buffers.outputs, 1);
    EXPECT_FLOAT_EQ(buffers.outputLeft[0], 222.0f);
    std::vector<guitarrackcraft::TrackSnapshot> tracks;
    EXPECT_TRUE(trackSnapshot(graph, relaunchedTrack, tracks).playing);
    EXPECT_TRUE(trackSnapshot(graph, otherTrack, tracks).playing);

    // At 60 BPM/60 Hz, Quarter is the strict next global boundary (frame 60).
    clearBuffers(buffers);
    graph.process(buffers.inputs, 2, buffers.outputs, 50);
    EXPECT_FLOAT_EQ(buffers.outputLeft[0], 224.0f);
    EXPECT_FLOAT_EQ(buffers.outputLeft[48], 320.0f);
    EXPECT_FLOAT_EQ(buffers.outputLeft[49], 262.0f);
    EXPECT_TRUE(trackSnapshot(graph, relaunchedTrack, tracks).playing);
    EXPECT_TRUE(trackSnapshot(graph, otherTrack, tracks).playing);
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

    ASSERT_TRUE(graph.setClipTransportPlaying(
        track, 0, true, guitarrackcraft::LaunchQuantization::None));
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
    ASSERT_TRUE(graph.setTrackInputHardwarePair(track, 0));

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
        EXPECT_FLOAT_EQ(buffers.outputRight[frame], -0.5f) << "frame " << frame;
    }
}

TEST(RackGraphTransportTest, RejectsInvalidLoopRecordingRequests) {
    struct InvalidRequest {
        const char* name;
        bool armed;
        uint32_t bars;
        bool attachClip;
    };
    const std::array<InvalidRequest, 5> cases = {{
        {"not armed", false, 1, false},
        {"zero bars", true, 0, false},
        {"three bars", true, 3, false},
        {"too many bars", true, 32, false},
        {"existing clip", true, 1, true},
    }};

    for (const auto& testCase : cases) {
        SCOPED_TRACE(testCase.name);
        RackGraph graph;
        configure(graph);
        const RackPathId track = graph.getTracks().front().id;
        ASSERT_TRUE(graph.setTrackInputArmed(track, testCase.armed));
        const bool validBars = testCase.bars == 1 || testCase.bars == 2 ||
                               testCase.bars == 4 || testCase.bars == 8 ||
                               testCase.bars == 16;
        if (!validBars) {
            EXPECT_FALSE(graph.setSlotDefaultLoopLength(track, 0, testCase.bars));
            EXPECT_FALSE(graph.setClipLoopLength(track, 0, testCase.bars));
            continue;
        }
        ASSERT_TRUE(graph.setSlotDefaultLoopLength(track, 0, testCase.bars));
        if (testCase.attachClip) {
            ASSERT_TRUE(graph.attachTrackWav(track, makeClip({9.0f, 8.0f}, 60)));
            ASSERT_TRUE(graph.setClipLoopLength(track, 0, testCase.bars));
            ASSERT_TRUE(graph.setClipLooping(track, 0, true));
        } else {
            EXPECT_FALSE(graph.setClipLoopLength(track, 0, testCase.bars));
            EXPECT_FALSE(graph.setClipLooping(track, 0, true));
        }

        EXPECT_FALSE(graph.startTrackClipRecording(
            track, 0, guitarrackcraft::LaunchQuantization::Quarter));
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
    ASSERT_TRUE(graph.setTransportPlaying(true));

    StereoBuffers buffers;
    clearBuffers(buffers);
    graph.process(buffers.inputs, 2, buffers.outputs, 3);
    ASSERT_TRUE(startConfiguredClipRecording(
        graph, track, 1, guitarrackcraft::LaunchQuantization::Quarter, false));
    clearBuffers(buffers);
    graph.process(buffers.inputs, 2, buffers.outputs, 56);
    expectSilence(buffers, 56);
    std::vector<guitarrackcraft::TrackSnapshot> tracks;
    const auto& pending = trackSnapshot(graph, track, tracks);
    EXPECT_TRUE(pending.wavLoaded);
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
    EXPECT_TRUE(recording.wavLoaded);
    EXPECT_FALSE(recording.recordPending);
    EXPECT_TRUE(recording.recording);
}
TEST(RackGraphTransportTest, NoneStartsLoopRecordingAtCurrentTransportFrame) {
    RackGraph graph;
    configure(graph);
    const RackPathId track = graph.getTracks().front().id;
    ASSERT_TRUE(graph.setTrackInputArmed(track, true));
    ASSERT_TRUE(graph.setTransportPlaying(true));

    StereoBuffers buffers;
    clearBuffers(buffers);
    graph.process(buffers.inputs, 2, buffers.outputs, 3);
    ASSERT_EQ(graph.getTransportSnapshot().transportFrame, 3u);
    ASSERT_TRUE(startConfiguredClipRecording(
        graph, track, 1, guitarrackcraft::LaunchQuantization::None, false));
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
    EXPECT_TRUE(recording.wavLoaded);
    EXPECT_FALSE(recording.recordPending);
    EXPECT_TRUE(recording.recording);
}
TEST(RackGraphTransportTest, StoppedLoopRecordingStartsAtFrameZeroForEveryQuantization) {
    const std::array<guitarrackcraft::LaunchQuantization, 5> quantizations = {{
        guitarrackcraft::LaunchQuantization::Bar,
        guitarrackcraft::LaunchQuantization::Quarter,
        guitarrackcraft::LaunchQuantization::Eighth,
        guitarrackcraft::LaunchQuantization::Sixteenth,
        guitarrackcraft::LaunchQuantization::None,
    }};
    for (const auto quantization : quantizations) {
        SCOPED_TRACE(static_cast<int>(quantization));
        RackGraph graph;
        configure(graph);
        const RackPathId track = graph.getTracks().front().id;
        ASSERT_TRUE(graph.setTrackInputArmed(track, true));
        ASSERT_TRUE(startConfiguredClipRecording(
            graph, track, 0.25, quantization, false));
        std::vector<guitarrackcraft::TrackSnapshot> tracks;
        EXPECT_TRUE(trackSnapshot(graph, track, tracks).recordPending);
        EXPECT_FALSE(graph.getTransportSnapshot().playing);

        ASSERT_TRUE(graph.setTransportPlaying(true));
        StereoBuffers buffers;
        clearBuffers(buffers);
        buffers.left[0] = 0.25f;
        buffers.right[0] = -0.5f;
        graph.process(buffers.inputs, 2, buffers.outputs, 1);

        const auto& recording = trackSnapshot(graph, track, tracks);
        EXPECT_FALSE(recording.recordPending);
        EXPECT_TRUE(recording.recording);
        EXPECT_EQ(graph.getTransportSnapshot().transportFrame, 1u);
        EXPECT_FLOAT_EQ(buffers.outputLeft[0], 0.25f);
        EXPECT_FLOAT_EQ(buffers.outputRight[0], -0.5f);
    }
}
TEST(RackGraphTransportTest, StoppedRecordingStartsGlobalTransportWithoutUiPlay) {
    RackGraph graph;
    configure(graph);
    const RackPathId track = graph.getTracks().front().id;
    ASSERT_TRUE(graph.setTrackInputArmed(track, true));
    ASSERT_TRUE(startConfiguredClipRecording(
        graph, track, 0.25, guitarrackcraft::LaunchQuantization::None, false));

    // The native recording request reserves the loop while stopped; no
    // separate transport Play command is issued by this test.
    EXPECT_FALSE(graph.getTransportSnapshot().playing);

    StereoBuffers buffers;
    clearBuffers(buffers);
    buffers.left[0] = 0.25f;
    buffers.right[0] = -0.5f;
    graph.process(buffers.inputs, 2, buffers.outputs, 1);

    std::vector<guitarrackcraft::TrackSnapshot> tracks;
    const auto& recording = trackSnapshot(graph, track, tracks);
    EXPECT_FALSE(recording.recordPending);
    EXPECT_TRUE(recording.recording);
    EXPECT_TRUE(graph.getTransportSnapshot().playing);
    EXPECT_EQ(graph.getTransportSnapshot().transportFrame, 1u);
    EXPECT_FLOAT_EQ(buffers.outputLeft[0], 0.25f);
    EXPECT_FLOAT_EQ(buffers.outputRight[0], -0.5f);
}

TEST(RackGraphTransportTest, QuantizedRecordingFollowsMusicalPhaseAfterBpmChange) {
    struct QuantizationCase {
        const char* name;
        guitarrackcraft::LaunchQuantization quantization;
        uint64_t expectedBoundary;
    };
    const std::array<QuantizationCase, 4> cases = {{
        {"quarter", guitarrackcraft::LaunchQuantization::Quarter, 45},
        {"eighth", guitarrackcraft::LaunchQuantization::Eighth, 45},
        {"sixteenth", guitarrackcraft::LaunchQuantization::Sixteenth, 38},
        {"bar", guitarrackcraft::LaunchQuantization::Bar, 135},
    }};

    for (const auto& testCase : cases) {
        SCOPED_TRACE(testCase.name);
        RackGraph graph;
        configure(graph);
        const RackPathId track = graph.getTracks().front().id;
        ASSERT_TRUE(graph.setTrackInputArmed(track, true));
        ASSERT_TRUE(graph.setTransportPlaying(true));
        StereoBuffers buffers;
        clearBuffers(buffers);
        graph.process(buffers.inputs, 2, buffers.outputs, 30);
        graph.setBeatsPerMinute(120.0);
        graph.process(buffers.inputs, 2, buffers.outputs, 1);

        const auto phase = graph.getTransportSnapshot();
        ASSERT_EQ(phase.transportFrame, 31u);
        EXPECT_NEAR(phase.musicalQuarterNotes, 8.0 / 15.0, 1e-12);
        ASSERT_TRUE(startConfiguredClipRecording(
            graph, track, 0.25, testCase.quantization, false));
        std::vector<guitarrackcraft::TrackSnapshot> tracks;
        EXPECT_TRUE(trackSnapshot(graph, track, tracks).recordPending);

        while (graph.getTransportSnapshot().transportFrame <
               testCase.expectedBoundary) {
            graph.process(buffers.inputs, 2, buffers.outputs, 1);
            EXPECT_TRUE(trackSnapshot(graph, track, tracks).recordPending);
            EXPECT_FALSE(trackSnapshot(graph, track, tracks).recording);
        }
        ASSERT_EQ(graph.getTransportSnapshot().transportFrame,
                  testCase.expectedBoundary);
        graph.process(buffers.inputs, 2, buffers.outputs, 1);
        EXPECT_FALSE(trackSnapshot(graph, track, tracks).recordPending);
        EXPECT_TRUE(trackSnapshot(graph, track, tracks).recording);
    }
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
    ASSERT_TRUE(configureClipLoop(
        graph, track, 2, 0.25, false, guitarrackcraft::LaunchQuantization::None));
    ASSERT_TRUE(graph.startTrackClipRecording(
        track, 2, guitarrackcraft::LaunchQuantization::None));

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

TEST(RackGraphTransportTest, RejectsSecondRecordingWithoutCorruptingFirst) {
    struct Phase {
        const char* name;
        bool startRecording;
    };
    const std::array<Phase, 2> phases = {{
        {"pending", false},
        {"recording", true},
    }};

    for (const auto& phase : phases) {
        SCOPED_TRACE(phase.name);
        RackGraph graph;
        configure(graph);
        const RackPathId track = graph.getTracks().front().id;
        ASSERT_TRUE(graph.setTrackInputArmed(track, true));
        ASSERT_TRUE(graph.setTransportPlaying(true));

        StereoBuffers buffers;
        clearBuffers(buffers);
        graph.process(buffers.inputs, 2, buffers.outputs, 3);
        ASSERT_TRUE(configureClipLoop(
            graph, track, 0, 0.25, false,
            guitarrackcraft::LaunchQuantization::Quarter));
        ASSERT_TRUE(configureClipLoop(
            graph, track, 1, 0.25, false,
            guitarrackcraft::LaunchQuantization::Quarter));
        ASSERT_TRUE(graph.startTrackClipRecording(
            track, 0, guitarrackcraft::LaunchQuantization::Quarter));

        // Keep A pending, or advance it to active recording, before trying B.
        clearBuffers(buffers);
        graph.process(buffers.inputs, 2, buffers.outputs, 57);
        if (phase.startRecording) {
            clearBuffers(buffers);
            graph.process(buffers.inputs, 2, buffers.outputs, 1);
        }
        std::vector<guitarrackcraft::TrackSnapshot> tracks;
        const auto& before = trackSnapshot(graph, track, tracks);
        EXPECT_EQ(before.recordPending, !phase.startRecording);
        EXPECT_EQ(before.recording, phase.startRecording);

        EXPECT_FALSE(graph.startTrackClipRecording(
            track, 1, guitarrackcraft::LaunchQuantization::None));
        const auto& afterReject = trackSnapshot(graph, track, tracks);
        EXPECT_EQ(afterReject.selectedSlot, 0u);
        EXPECT_EQ(afterReject.recordPending, !phase.startRecording);
        EXPECT_EQ(afterReject.recording, phase.startRecording);

        // Finish A and verify that B did not take over its reservation.
        clearBuffers(buffers);
        buffers.left[0] = 1.0f;
        buffers.right[0] = 1.0f;
        graph.process(buffers.inputs, 2, buffers.outputs, 1);
        graph.process(buffers.inputs, 2, buffers.outputs, 59);

        const auto slots = graph.getTrackClipSlots(track);
        const auto* first = findClipSlot(slots, 0);
        const auto* second = findClipSlot(slots, 1);
        ASSERT_NE(first, nullptr);
        ASSERT_NE(second, nullptr);
        EXPECT_TRUE(first->wavLoaded);
        EXPECT_DOUBLE_EQ(first->durationSec, 1.0);
        EXPECT_FALSE(second->wavLoaded);
        const auto& completed = trackSnapshot(graph, track, tracks);
        EXPECT_FALSE(completed.recordPending);
        EXPECT_FALSE(completed.recording);
        EXPECT_EQ(completed.selectedSlot, 0u);
    }
}

TEST(RackGraphTransportTest, RecordingCompletionMakesReservedSlotAudibleAfterSelectionChange) {
    RackGraph graph;
    configure(graph);
    const RackPathId track = graph.getTracks().front().id;
    ASSERT_TRUE(graph.setTrackInputArmed(track, true));
    ASSERT_TRUE(graph.setTransportPlaying(true));
    ASSERT_TRUE(configureClipLoop(
        graph, track, 2, 0.25, false,
        guitarrackcraft::LaunchQuantization::None));
    ASSERT_TRUE(graph.startTrackClipRecording(
        track, 2, guitarrackcraft::LaunchQuantization::None));

    StereoBuffers buffers;
    clearBuffers(buffers);
    buffers.left[0] = 1.0f;
    buffers.right[0] = 1.0f;
    graph.process(buffers.inputs, 2, buffers.outputs, 1);
    ASSERT_TRUE(graph.selectTrackClipSlot(track, 0));

    clearBuffers(buffers);
    for (uint32_t frame = 0; frame < 59; ++frame) {
        buffers.left[frame] = 2.0f + static_cast<float>(frame);
        buffers.right[frame] = buffers.left[frame];
    }
    graph.process(buffers.inputs, 2, buffers.outputs, 59);

    const auto slots = graph.getTrackClipSlots(track);
    const auto* selected = findClipSlot(slots, 0);
    const auto* reserved = findClipSlot(slots, 2);
    ASSERT_NE(selected, nullptr);
    ASSERT_NE(reserved, nullptr);
    EXPECT_TRUE(selected->active);
    EXPECT_FALSE(selected->playing);
    EXPECT_TRUE(reserved->wavLoaded);
    EXPECT_FALSE(reserved->active);
    EXPECT_TRUE(reserved->playing);
    EXPECT_EQ(reserved->transportFrame, 0u);
    EXPECT_DOUBLE_EQ(reserved->durationSec, 1.0);

    std::vector<guitarrackcraft::TrackSnapshot> tracks;
    const auto& completed = trackSnapshot(graph, track, tracks);
    EXPECT_EQ(completed.selectedSlot, 0u);
    EXPECT_EQ(completed.activeSlot, 2);
    EXPECT_TRUE(completed.playing);
    EXPECT_TRUE(completed.wavLoaded);
    EXPECT_EQ(completed.wavDisplayName, "Recorded loop");
    EXPECT_DOUBLE_EQ(completed.wavDurationSec, 1.0);
    EXPECT_FALSE(completed.recordPending);
    EXPECT_FALSE(completed.recording);
    EXPECT_FALSE(completed.midiLoaded);

    ASSERT_TRUE(graph.selectTrackClipSlot(track, 2));
    clearBuffers(buffers);
    graph.process(buffers.inputs, 2, buffers.outputs, 1);
    EXPECT_FLOAT_EQ(buffers.outputLeft[0], 1.0f);
    EXPECT_FLOAT_EQ(buffers.outputRight[0], 1.0f);
}

TEST(RackGraphTransportTest, RapidSlotLaunchesKeepIndependentClipMetadata) {
    RackGraph graph;
    configure(graph);
    const RackPathId track = graph.getTracks().front().id;
    ASSERT_TRUE(graph.attachTrackWavSlot(
        track, 0, makeRampClip(8, 1.0f, "first.wav")));
    ASSERT_TRUE(graph.attachTrackWavSlot(
        track, 1, makeRampClip(8, 101.0f, "second.wav")));
    ASSERT_TRUE(graph.setClipLooping(track, 0, true));
    ASSERT_TRUE(graph.setClipLooping(track, 1, true));
    ASSERT_TRUE(graph.setTransportPlaying(true));

    StereoBuffers buffers;
    clearBuffers(buffers);
    ASSERT_TRUE(graph.setClipTransportPlaying(
        track, 0, true, guitarrackcraft::LaunchQuantization::None));
    graph.process(buffers.inputs, 2, buffers.outputs, 2);
    EXPECT_FLOAT_EQ(buffers.outputLeft[0], 1.0f);
    EXPECT_FLOAT_EQ(buffers.outputLeft[1], 2.0f);

    // Each cross-slot launch uses the target media and restarts at frame zero.
    ASSERT_TRUE(graph.setClipTransportPlaying(
        track, 0, false, guitarrackcraft::LaunchQuantization::None));
    ASSERT_TRUE(graph.selectTrackClipSlot(track, 1));
    ASSERT_TRUE(graph.setClipTransportPlaying(
        track, 1, true, guitarrackcraft::LaunchQuantization::None));
    clearBuffers(buffers);
    graph.process(buffers.inputs, 2, buffers.outputs, 2);
    EXPECT_FLOAT_EQ(buffers.outputLeft[0], 101.0f);
    EXPECT_FLOAT_EQ(buffers.outputLeft[1], 102.0f);

    ASSERT_TRUE(graph.setClipTransportPlaying(
        track, 1, false, guitarrackcraft::LaunchQuantization::None));
    ASSERT_TRUE(graph.selectTrackClipSlot(track, 0));
    ASSERT_TRUE(graph.setClipTransportPlaying(
        track, 0, true, guitarrackcraft::LaunchQuantization::None));
    clearBuffers(buffers);
    graph.process(buffers.inputs, 2, buffers.outputs, 1);
    EXPECT_FLOAT_EQ(buffers.outputLeft[0], 1.0f);

    ASSERT_TRUE(graph.setClipTransportPlaying(
        track, 1, true, guitarrackcraft::LaunchQuantization::None));
    clearBuffers(buffers);
    graph.process(buffers.inputs, 2, buffers.outputs, 1);
    EXPECT_FLOAT_EQ(buffers.outputLeft[0], 101.0f);

    const auto slots = graph.getTrackClipSlots(track);
    const auto* first = findClipSlot(slots, 0);
    const auto* second = findClipSlot(slots, 1);
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    EXPECT_TRUE(first->active);
    EXPECT_FALSE(first->playing);
    EXPECT_EQ(first->transportFrame, 1u);
    EXPECT_FALSE(second->active);
    EXPECT_TRUE(second->playing);
    EXPECT_EQ(second->transportFrame, 1u);
}

TEST(RackGraphTransportTest, QuantizedCrossSlotSwitchIsSampleAccurateInsideCallback) {
    RackGraph graph;
    configure(graph);
    const RackPathId track = graph.getTracks().front().id;
    ASSERT_TRUE(graph.attachTrackWavSlot(track, 0, makeRampClip(128, 1.0f, "old.wav")));
    ASSERT_TRUE(graph.attachTrackWavSlot(track, 1, makeRampClip(128, 101.0f, "target.wav")));
    ASSERT_TRUE(graph.setTransportPlaying(true));
    ASSERT_TRUE(graph.setClipTransportPlaying(
        track, 0, true, guitarrackcraft::LaunchQuantization::None));

    StereoBuffers buffers;
    clearBuffers(buffers);
    graph.process(buffers.inputs, 2, buffers.outputs, 4);
    ASSERT_TRUE(graph.selectTrackClipSlot(track, 0));
    ASSERT_TRUE(graph.setClipTransportPlaying(
        track, 1, true, guitarrackcraft::LaunchQuantization::Quarter));

    clearBuffers(buffers);
    graph.process(buffers.inputs, 2, buffers.outputs, 58);
    EXPECT_FLOAT_EQ(buffers.outputLeft[55], 60.0f);
    EXPECT_FLOAT_EQ(buffers.outputLeft[56], 101.0f);
    EXPECT_FLOAT_EQ(buffers.outputLeft[57], 102.0f);

    const auto slots = graph.getTrackClipSlots(track);
    const auto* oldSlot = findClipSlot(slots, 0);
    const auto* targetSlot = findClipSlot(slots, 1);
    ASSERT_NE(oldSlot, nullptr);
    ASSERT_NE(targetSlot, nullptr);
    EXPECT_FALSE(oldSlot->playing);
    EXPECT_TRUE(targetSlot->playing);
    EXPECT_TRUE(oldSlot->active);
    EXPECT_FALSE(targetSlot->active);

    std::vector<guitarrackcraft::TrackSnapshot> tracks;
    const auto& snapshot = trackSnapshot(graph, track, tracks);
    EXPECT_EQ(snapshot.selectedSlot, 0u);
    EXPECT_EQ(snapshot.activeSlot, 1);
    EXPECT_TRUE(snapshot.playing);
    EXPECT_EQ(snapshot.wavDisplayName, "target.wav");
}

TEST(RackGraphTransportTest, NewerQueuedCrossSlotLaunchReplacesOlderTarget) {
    RackGraph graph;
    configure(graph);
    const RackPathId track = graph.getTracks().front().id;
    ASSERT_TRUE(graph.attachTrackWavSlot(track, 0, makeRampClip(128, 1.0f, "old.wav")));
    ASSERT_TRUE(graph.attachTrackWavSlot(track, 1, makeRampClip(128, 101.0f, "superseded.wav")));
    ASSERT_TRUE(graph.attachTrackWavSlot(track, 2, makeRampClip(128, 201.0f, "newest.wav")));
    ASSERT_TRUE(graph.setTransportPlaying(true));
    ASSERT_TRUE(graph.setClipTransportPlaying(
        track, 0, true, guitarrackcraft::LaunchQuantization::None));

    StereoBuffers buffers;
    clearBuffers(buffers);
    graph.process(buffers.inputs, 2, buffers.outputs, 4);
    ASSERT_TRUE(graph.selectTrackClipSlot(track, 0));
    ASSERT_TRUE(graph.setClipTransportPlaying(
        track, 1, true, guitarrackcraft::LaunchQuantization::Quarter));
    ASSERT_TRUE(graph.setClipTransportPlaying(
        track, 2, true, guitarrackcraft::LaunchQuantization::Quarter));

    clearBuffers(buffers);
    graph.process(buffers.inputs, 2, buffers.outputs, 57);
    EXPECT_FLOAT_EQ(buffers.outputLeft[55], 60.0f);
    EXPECT_FLOAT_EQ(buffers.outputLeft[56], 201.0f);

    const auto slots = graph.getTrackClipSlots(track);
    const auto* superseded = findClipSlot(slots, 1);
    const auto* newest = findClipSlot(slots, 2);
    ASSERT_NE(superseded, nullptr);
    ASSERT_NE(newest, nullptr);
    EXPECT_FALSE(superseded->playing);
    EXPECT_TRUE(newest->playing);

    std::vector<guitarrackcraft::TrackSnapshot> tracks;
    const auto& snapshot = trackSnapshot(graph, track, tracks);
    EXPECT_EQ(snapshot.selectedSlot, 0u);
    EXPECT_EQ(snapshot.activeSlot, 2);
    EXPECT_TRUE(snapshot.playing);
    EXPECT_EQ(snapshot.wavDisplayName, "newest.wav");
}

TEST(RackGraphTransportTest, CrossTrackReplacementDoesNotDisturbOtherTrack) {
    struct LaunchCase {
        const char* name;
        guitarrackcraft::LaunchQuantization quantization;
    };
    const std::array<LaunchCase, 2> cases = {{
        {"immediate", guitarrackcraft::LaunchQuantization::None},
        {"quantized", guitarrackcraft::LaunchQuantization::Quarter},
    }};

    for (const auto& testCase : cases) {
        SCOPED_TRACE(testCase.name);
        RackGraph graph;
        configure(graph);
        const RackPathId trackA = graph.getTracks().front().id;
        const RackPathId trackB = graph.addTrack();
        ASSERT_NE(trackB, 0u);
        ASSERT_TRUE(graph.attachTrackWavSlot(
            trackA, 0, makeRampClip(128, 1.0f, "a-old.wav")));
        ASSERT_TRUE(graph.attachTrackWavSlot(
            trackA, 1, makeRampClip(128, 101.0f, "a-new.wav")));
        ASSERT_TRUE(graph.attachTrackWavSlot(
            trackB, 0, makeRampClip(128, 201.0f, "b.wav")));
        ASSERT_TRUE(graph.setClipLooping(trackA, 0, true));
        ASSERT_TRUE(graph.setClipLooping(trackA, 1, true));
        ASSERT_TRUE(graph.setClipLooping(trackB, 0, true));
        ASSERT_TRUE(graph.setTransportPlaying(true));
        ASSERT_TRUE(graph.setClipTransportPlaying(
            trackA, 0, true, guitarrackcraft::LaunchQuantization::None));
        ASSERT_TRUE(graph.setClipTransportPlaying(
            trackB, 0, true, guitarrackcraft::LaunchQuantization::None));

        StereoBuffers buffers;
        clearBuffers(buffers);
        graph.process(buffers.inputs, 2, buffers.outputs, 4);
        ASSERT_TRUE(graph.setClipTransportPlaying(
            trackA, 1, true, testCase.quantization));

        clearBuffers(buffers);
        if (testCase.quantization == guitarrackcraft::LaunchQuantization::None) {
            graph.process(buffers.inputs, 2, buffers.outputs, 2);
            EXPECT_FLOAT_EQ(buffers.outputLeft[0], 306.0f);
            EXPECT_FLOAT_EQ(buffers.outputLeft[1], 308.0f);
            EXPECT_FLOAT_EQ(buffers.outputRight[0], 306.0f);
            EXPECT_FLOAT_EQ(buffers.outputRight[1], 308.0f);
        } else {
            graph.process(buffers.inputs, 2, buffers.outputs, 58);
            EXPECT_FLOAT_EQ(buffers.outputLeft[55], 320.0f);
            EXPECT_FLOAT_EQ(buffers.outputLeft[56], 362.0f);
            EXPECT_FLOAT_EQ(buffers.outputRight[55], 320.0f);
            EXPECT_FLOAT_EQ(buffers.outputRight[56], 362.0f);
        }

        auto slotsA = graph.getTrackClipSlots(trackA);
        const auto* oldA = findClipSlot(slotsA, 0);
        const auto* newA = findClipSlot(slotsA, 1);
        auto slotsB = graph.getTrackClipSlots(trackB);
        const auto* playingB = findClipSlot(slotsB, 0);
        ASSERT_NE(oldA, nullptr);
        ASSERT_NE(newA, nullptr);
        ASSERT_NE(playingB, nullptr);
        EXPECT_FALSE(oldA->playing);
        EXPECT_TRUE(newA->playing);
        EXPECT_TRUE(playingB->active);
        EXPECT_TRUE(playingB->playing);
        EXPECT_TRUE(oldA->active);
        EXPECT_FALSE(newA->active);

        std::vector<guitarrackcraft::TrackSnapshot> tracks;
        EXPECT_EQ(trackSnapshot(graph, trackA, tracks).activeSlot, 1);
        EXPECT_EQ(trackSnapshot(graph, trackB, tracks).activeSlot, 0);

        ASSERT_TRUE(graph.setClipTransportPlaying(
            trackA, 1, false, guitarrackcraft::LaunchQuantization::None));
        clearBuffers(buffers);
        graph.process(buffers.inputs, 2, buffers.outputs, 2);
        EXPECT_FLOAT_EQ(buffers.outputLeft[0],
                        testCase.quantization == guitarrackcraft::LaunchQuantization::None
                            ? 207.0f : 263.0f);
        EXPECT_FLOAT_EQ(buffers.outputLeft[1],
                        testCase.quantization == guitarrackcraft::LaunchQuantization::None
                            ? 208.0f : 264.0f);
        EXPECT_FLOAT_EQ(buffers.outputRight[0],
                        testCase.quantization == guitarrackcraft::LaunchQuantization::None
                            ? 207.0f : 263.0f);
        EXPECT_FLOAT_EQ(buffers.outputRight[1],
                        testCase.quantization == guitarrackcraft::LaunchQuantization::None
                            ? 208.0f : 264.0f);
        slotsA = graph.getTrackClipSlots(trackA);
        slotsB = graph.getTrackClipSlots(trackB);
        oldA = findClipSlot(slotsA, 0);
        newA = findClipSlot(slotsA, 1);
        playingB = findClipSlot(slotsB, 0);
        ASSERT_NE(oldA, nullptr);
        ASSERT_NE(newA, nullptr);
        ASSERT_NE(playingB, nullptr);
        EXPECT_TRUE(oldA->active);
        EXPECT_FALSE(newA->active);
        EXPECT_FALSE(newA->playing);
        EXPECT_TRUE(playingB->playing);
        EXPECT_TRUE(playingB->active);
    }
}

TEST(RackGraphTransportTest, SelectingSlotDoesNotSwitchAudibleClip) {
    RackGraph graph;
    configure(graph);
    const RackPathId track = graph.getTracks().front().id;
    ASSERT_TRUE(graph.attachTrackWavSlot(track, 0, makeRampClip(64, 1.0f, "audible.wav")));
    ASSERT_TRUE(graph.attachTrackWavSlot(track, 1, makeRampClip(64, 101.0f, "selected.wav")));
    ASSERT_TRUE(graph.setTransportPlaying(true));
    ASSERT_TRUE(graph.setClipTransportPlaying(
        track, 0, true, guitarrackcraft::LaunchQuantization::None));

    StereoBuffers buffers;
    clearBuffers(buffers);
    graph.process(buffers.inputs, 2, buffers.outputs, 2);
    ASSERT_TRUE(graph.selectTrackClipSlot(track, 1));
    clearBuffers(buffers);
    graph.process(buffers.inputs, 2, buffers.outputs, 2);

    EXPECT_FLOAT_EQ(buffers.outputLeft[0], 3.0f);
    EXPECT_FLOAT_EQ(buffers.outputLeft[1], 4.0f);
    const auto slots = graph.getTrackClipSlots(track);
    const auto* audible = findClipSlot(slots, 0);
    const auto* selected = findClipSlot(slots, 1);
    ASSERT_NE(audible, nullptr);
    ASSERT_NE(selected, nullptr);
    EXPECT_TRUE(audible->playing);
    EXPECT_FALSE(selected->playing);
    EXPECT_FALSE(audible->active);
    EXPECT_TRUE(selected->active);

    std::vector<guitarrackcraft::TrackSnapshot> tracks;
    const auto& snapshot = trackSnapshot(graph, track, tracks);
    EXPECT_EQ(snapshot.selectedSlot, 1u);
    EXPECT_EQ(snapshot.activeSlot, 0);
    EXPECT_EQ(snapshot.wavDisplayName, "audible.wav");
}

TEST(RackGraphTransportTest, TempoModesExposeMetadataAndEffectiveEof) {
    struct TempoCase {
        const char* name;
        guitarrackcraft::ClipTempoMode mode;
        int modeValue;
        float first;
        float second;
        float lastFirstGrain;
        uint32_t effectiveLength;
    };
    const std::array<TempoCase, 3> cases = {{
        {"original", guitarrackcraft::ClipTempoMode::Original, 0, 1.0f, 2.0f, 512.0f, 2048u},
        {"stretch", guitarrackcraft::ClipTempoMode::Stretch, 1, 1.0f, 2.0f, 512.0f, 1024u},
        {"repitch", guitarrackcraft::ClipTempoMode::Repitch, 2, 1.0f, 3.0f, 1023.0f, 1024u},
    }};

    for (const auto& testCase : cases) {
        SCOPED_TRACE(testCase.name);
        RackGraph graph;
        configure(graph);
        graph.setBeatsPerMinute(120.0);
        const RackPathId track = graph.getTracks().front().id;
        ASSERT_TRUE(graph.attachTrackWavSlot(
            track, 0, makeTempoRampClip(2048, 60.0, testCase.name)));
        ASSERT_TRUE(graph.setClipTempoMode(track, 0, testCase.mode));

        const auto configured = graph.getTrackClipSlots(track);
        const auto* metadata = findClipSlot(configured, 0);
        ASSERT_NE(metadata, nullptr);
        EXPECT_DOUBLE_EQ(metadata->sourceBpm, 60.0);
        EXPECT_EQ(metadata->tempoMode, testCase.modeValue);

        ASSERT_TRUE(graph.setTransportPlaying(true));
        ASSERT_TRUE(graph.setClipTransportPlaying(
            track, 0, true, guitarrackcraft::LaunchQuantization::None));
        StereoBuffers buffers;
        clearBuffers(buffers);
        graph.process(buffers.inputs, 2, buffers.outputs, 512);
        EXPECT_FLOAT_EQ(buffers.outputLeft[0], testCase.first);
        EXPECT_FLOAT_EQ(buffers.outputLeft[1], testCase.second);
        EXPECT_FLOAT_EQ(buffers.outputLeft[511], testCase.lastFirstGrain);

        uint32_t remaining = testCase.effectiveLength - 512u;
        while (remaining != 0) {
            const uint32_t chunk = std::min<uint32_t>(remaining, 512u);
            clearBuffers(buffers);
            graph.process(buffers.inputs, 2, buffers.outputs, chunk);
            remaining -= chunk;
        }
        clearBuffers(buffers);
        graph.process(buffers.inputs, 2, buffers.outputs, 1);
        EXPECT_FLOAT_EQ(buffers.outputLeft[0], 0.0f);
        std::vector<guitarrackcraft::TrackSnapshot> tracks;
        const auto& snapshot = trackSnapshot(graph, track, tracks);
        EXPECT_FALSE(snapshot.playing);
        EXPECT_EQ(snapshot.transportFrame, testCase.effectiveLength);
    }
}

TEST(RackGraphTransportTest, TempoAdaptersDoNotAllocateInAudioCallback) {
    const std::array<guitarrackcraft::ClipTempoMode, 2> modes = {{
        guitarrackcraft::ClipTempoMode::Stretch,
        guitarrackcraft::ClipTempoMode::Repitch,
    }};
    for (const auto mode : modes) {
        RackGraph graph;
        configure(graph);
        graph.setBeatsPerMinute(120.0);
        const RackPathId track = graph.getTracks().front().id;
        ASSERT_TRUE(graph.attachTrackWavSlot(
            track, 0, makeTempoRampClip(2048, 60.0, "allocation.wav")));
        ASSERT_TRUE(graph.setClipTempoMode(track, 0, mode));
        ASSERT_TRUE(graph.setTransportPlaying(true));
        ASSERT_TRUE(graph.setClipTransportPlaying(
            track, 0, true, guitarrackcraft::LaunchQuantization::None));

        StereoBuffers buffers;
        clearBuffers(buffers);
        graph.process(buffers.inputs, 2, buffers.outputs, 1);
        allocation_probe::allocations = 0;
        allocation_probe::enabled = true;
        clearBuffers(buffers);
        graph.process(buffers.inputs, 2, buffers.outputs, 512);
        clearBuffers(buffers);
        graph.process(buffers.inputs, 2, buffers.outputs, 1);
        allocation_probe::enabled = false;

        EXPECT_EQ(allocation_probe::allocations, 0u);
        const float expectedFrame513 = mode == guitarrackcraft::ClipTempoMode::Stretch
            ? 515.0f : 1027.0f;
        EXPECT_FLOAT_EQ(buffers.outputLeft[0], expectedFrame513);
    }
}
TEST(RackGraphTransportTest, ClipSourceBpmUpdateAppearsInSlotInfoWithoutMutatingClipPayload) {
    RackGraph graph;
    configure(graph);
    const RackPathId track = graph.getTracks().front().id;
    const auto clip = makeTempoRampClip(2048, 60.0, "source-bpm.wav");
    ASSERT_TRUE(graph.attachTrackWavSlot(track, 1, clip));
    ASSERT_TRUE(graph.selectTrackClipSlot(track, 1));

    const auto before = graph.getTrackClipSlots(track);
    const auto* beforeInfo = findClipSlot(before, 1);
    ASSERT_NE(beforeInfo, nullptr);
    EXPECT_DOUBLE_EQ(beforeInfo->sourceBpm, 60.0);

    for (const double requestedBpm : {20.0, 137.5, 400.0}) {
        ASSERT_TRUE(graph.setClipSourceBpm(track, 1, requestedBpm));

        const auto after = graph.getTrackClipSlots(track);
        const auto* afterInfo = findClipSlot(after, 1);
        ASSERT_NE(afterInfo, nullptr);
        EXPECT_TRUE(afterInfo->wavLoaded);
        EXPECT_FALSE(afterInfo->midiLoaded);
        EXPECT_TRUE(afterInfo->active);
        EXPECT_DOUBLE_EQ(afterInfo->sourceBpm, requestedBpm);
        EXPECT_DOUBLE_EQ(clip->sourceBpm, 60.0);
    }
}

TEST(RackGraphTransportTest, ClipSourceBpmRejectsInvalidValuesAndNonWavSlots) {
    RackGraph graph;
    configure(graph);
    const RackPathId track = graph.getTracks().front().id;
    ASSERT_TRUE(graph.attachTrackWavSlot(track, 0, makeTempoRampClip(128, 60.0, "empty.wav")));
    ASSERT_TRUE(graph.unloadTrackWavSlot(track, 0));
    ASSERT_TRUE(graph.attachTrackMidiSlot(track, 1, makeMidiClip(1'000, "midi.mid")));
    const auto midiSlots = graph.getTrackClipSlots(track);
    const auto* midiInfo = findClipSlot(midiSlots, 1);
    ASSERT_NE(midiInfo, nullptr);
    EXPECT_DOUBLE_EQ(midiInfo->durationSec, 0.001);

    struct InvalidValue {
        const char* name;
        double value;
    };
    const std::array<InvalidValue, 5> invalidValues = {{
        {"below minimum", 19.999},
        {"above maximum", 400.001},
        {"not a number", std::numeric_limits<double>::quiet_NaN()},
        {"positive infinity", std::numeric_limits<double>::infinity()},
        {"negative infinity", -std::numeric_limits<double>::infinity()},
    }};
    for (const auto& invalid : invalidValues) {
        SCOPED_TRACE(invalid.name);
        EXPECT_FALSE(graph.setClipSourceBpm(track, 0, invalid.value));
        EXPECT_FALSE(graph.setClipSourceBpm(track, 1, invalid.value));
    }

    EXPECT_FALSE(graph.setClipSourceBpm(track, 0, 120.0)); // empty slot
    EXPECT_FALSE(graph.setClipSourceBpm(track, 1, 120.0)); // MIDI-only slot
    EXPECT_FALSE(graph.setClipSourceBpm(track, 2, 120.0)); // missing slot
    EXPECT_FALSE(graph.setClipSourceBpm(track, 99, 120.0)); // missing slot
}

TEST(RackGraphTransportTest, ClipSourceBpmChangesStretchAndRepitchTimingWithoutReset) {
    struct TempoCase {
        const char* name;
        guitarrackcraft::ClipTempoMode mode;
        uint32_t warmupFrames;
        uint32_t framesAfterUpdate;
        float expectedLastSample;
    };
    const std::array<TempoCase, 2> cases = {{
        // At the second stretch grain, source BPM 120 changes the old ratio-2
        // interpolation from 515 to the source-ramp sample at frame 513 (514).
        {"stretch", guitarrackcraft::ClipTempoMode::Stretch, 512, 2, 514.0f},
        // Repitch changes the source-ramp position immediately at the current frame.
        {"repitch", guitarrackcraft::ClipTempoMode::Repitch, 100, 1, 101.0f},
    }};

    for (const auto& testCase : cases) {
        SCOPED_TRACE(testCase.name);
        RackGraph graph;
        configure(graph);
        graph.setBeatsPerMinute(120.0);
        const RackPathId track = graph.getTracks().front().id;
        const auto clip = makeTempoRampClip(2048, 60.0, testCase.name);
        ASSERT_TRUE(graph.attachTrackWavSlot(track, 1, clip));
        ASSERT_TRUE(graph.selectTrackClipSlot(track, 1));
        ASSERT_TRUE(graph.setClipTempoMode(track, 1, testCase.mode));
        ASSERT_TRUE(graph.setTransportPlaying(true));
        ASSERT_TRUE(graph.setClipTransportPlaying(
            track, 1, true, guitarrackcraft::LaunchQuantization::None));

        StereoBuffers buffers;
        clearBuffers(buffers);
        graph.process(buffers.inputs, 2, buffers.outputs, testCase.warmupFrames);
        const auto before = graph.getTrackClipSlots(track);
        const auto* beforeInfo = findClipSlot(before, 1);
        ASSERT_NE(beforeInfo, nullptr);
        ASSERT_TRUE(beforeInfo->playing);
        const uint64_t playhead = beforeInfo->transportFrame;
        ASSERT_EQ(playhead, testCase.warmupFrames);
        std::vector<guitarrackcraft::TrackSnapshot> tracks;
        const auto& beforeTrack = trackSnapshot(graph, track, tracks);
        ASSERT_EQ(beforeTrack.selectedSlot, 1u);
        ASSERT_EQ(beforeTrack.activeSlot, 1);

        ASSERT_TRUE(graph.setClipSourceBpm(track, 1, 120.0));

        const auto after = graph.getTrackClipSlots(track);
        const auto* afterInfo = findClipSlot(after, 1);
        ASSERT_NE(afterInfo, nullptr);
        EXPECT_DOUBLE_EQ(afterInfo->sourceBpm, 120.0);
        EXPECT_TRUE(afterInfo->playing);
        EXPECT_TRUE(afterInfo->active);
        EXPECT_EQ(afterInfo->transportFrame, playhead);
        const auto& afterTrack = trackSnapshot(graph, track, tracks);
        EXPECT_EQ(afterTrack.selectedSlot, 1u);
        EXPECT_EQ(afterTrack.activeSlot, 1);

        clearBuffers(buffers);
        graph.process(buffers.inputs, 2, buffers.outputs, testCase.framesAfterUpdate);
        EXPECT_FLOAT_EQ(
            buffers.outputLeft[testCase.framesAfterUpdate - 1],
            testCase.expectedLastSample);
        const auto advanced = graph.getTrackClipSlots(track);
        const auto* advancedInfo = findClipSlot(advanced, 1);
        ASSERT_NE(advancedInfo, nullptr);
        EXPECT_EQ(advancedInfo->transportFrame,
                  playhead + testCase.framesAfterUpdate);
    }
}

TEST(RackGraphTransportTest, ClipSourceBpmUpdateKeepsAudioCallbackAllocationFree) {
    const std::array<guitarrackcraft::ClipTempoMode, 2> modes = {{
        guitarrackcraft::ClipTempoMode::Stretch,
        guitarrackcraft::ClipTempoMode::Repitch,
    }};
    for (const auto mode : modes) {
        SCOPED_TRACE(static_cast<int>(mode));
        RackGraph graph;
        configure(graph);
        graph.setBeatsPerMinute(120.0);
        const RackPathId track = graph.getTracks().front().id;
        ASSERT_TRUE(graph.attachTrackWavSlot(
            track, 1, makeTempoRampClip(2048, 60.0, "source-bpm-allocation.wav")));
        ASSERT_TRUE(graph.setClipTempoMode(track, 1, mode));
        ASSERT_TRUE(graph.setTransportPlaying(true));
        ASSERT_TRUE(graph.setClipTransportPlaying(
            track, 1, true, guitarrackcraft::LaunchQuantization::None));

        StereoBuffers buffers;
        clearBuffers(buffers);
        graph.process(buffers.inputs, 2, buffers.outputs, 1);
        ASSERT_TRUE(graph.setClipSourceBpm(track, 1, 120.0));

        allocation_probe::allocations = 0;
        allocation_probe::enabled = true;
        clearBuffers(buffers);
        graph.process(buffers.inputs, 2, buffers.outputs, 512);
        clearBuffers(buffers);
        graph.process(buffers.inputs, 2, buffers.outputs, 1);
        allocation_probe::enabled = false;

        EXPECT_EQ(allocation_probe::allocations, 0u);
    }
}


TEST(RackGraphTransportTest, MidiLoopUsesConfiguredLengthShorterAndLongerThanFile) {
    struct LoopCase {
        const char* name;
        double bars;
        uint32_t frames;
        uint32_t expectedFrame;
    };
    const std::array<LoopCase, 2> cases = {{
        {"shorter than file", 0.25, 61, 1},
        {"longer than file", 1.0, 121, 121},
    }};

    for (const auto& testCase : cases) {
        SCOPED_TRACE(testCase.name);
        RackGraph graph;
        configure(graph);
        const RackPathId track = graph.getTracks().front().id;
        // One second of MIDI timeline data; events are specified in microseconds.
        ASSERT_TRUE(graph.attachTrackMidiSlot(
            track, 0, makeMidiClip(1'000'000)));
        ASSERT_TRUE(graph.setClipLoopLength(track, 0, testCase.bars));
        ASSERT_TRUE(graph.setClipLooping(track, 0, true));
        ASSERT_TRUE(graph.setTransportPlaying(true));
        ASSERT_TRUE(graph.setClipTransportPlaying(
            track, 0, true, guitarrackcraft::LaunchQuantization::None));

        StereoBuffers buffers;
        clearBuffers(buffers);
        graph.process(buffers.inputs, 2, buffers.outputs, testCase.frames);

        const auto slots = graph.getTrackClipSlots(track);
        const auto* state = findClipSlot(slots, 0);
        ASSERT_NE(state, nullptr);
        EXPECT_TRUE(state->midiLoaded);
        EXPECT_TRUE(state->playing);
        EXPECT_TRUE(state->looping);
        EXPECT_DOUBLE_EQ(state->loopLengthBars, testCase.bars);
        EXPECT_EQ(state->transportFrame, testCase.expectedFrame);
    }
}

TEST(RackGraphTransportTest, MidiEventsCaptureTailAndFrameZeroAcrossLoopBoundary) {
    RackGraph graph;
    configure(graph);
    const RackPathId track = graph.getTracks().front().id;
    ASSERT_TRUE(graph.attachTrackMidiSlot(
        track, 0, makeScheduledMidiClip(
            2'000'000u,
            {0u, 483'334u})));
    ASSERT_TRUE(graph.setClipLoopLength(track, 0, 0.25));
    ASSERT_TRUE(graph.setClipLooping(track, 0, true));
    const auto chain = graph.getChain(track);
    ASSERT_NE(chain, nullptr);
    auto capture = std::make_unique<MidiCapturingPlugin>();
    auto* capturePtr = capture.get();
    ASSERT_EQ(chain->addPlugin(std::move(capture)), 0);
    ASSERT_TRUE(graph.setTransportPlaying(true));
    ASSERT_TRUE(graph.setClipTransportPlaying(
        track, 0, true, guitarrackcraft::LaunchQuantization::None));

    StereoBuffers buffers;
    clearBuffers(buffers);
    graph.process(buffers.inputs, 2, buffers.outputs, 61);

    ASSERT_EQ(capturePtr->capturedCount(), 3u);
    EXPECT_EQ(capturePtr->captured(0).frameOffset, 0u);
    EXPECT_EQ(capturePtr->captured(1).frameOffset, 29u);
    EXPECT_EQ(capturePtr->captured(2).frameOffset, 60u);
    EXPECT_EQ(capturePtr->captured(0).data1, 60u);
    EXPECT_EQ(capturePtr->captured(1).data1, 61u);
    EXPECT_EQ(capturePtr->captured(2).data1, 60u);
}

TEST(RackGraphTransportTest, MidiEventsBeyondShorterConfiguredLoopAreOmitted) {
    RackGraph graph;
    configure(graph);
    const RackPathId track = graph.getTracks().front().id;
    ASSERT_TRUE(graph.attachTrackMidiSlot(
        track, 0, makeScheduledMidiClip(
            2'000'000u,
            {166'667u, 1'166'667u})));
    ASSERT_TRUE(graph.setClipLoopLength(track, 0, 0.25));
    ASSERT_TRUE(graph.setClipLooping(track, 0, true));
    const auto chain = graph.getChain(track);
    ASSERT_NE(chain, nullptr);
    auto capture = std::make_unique<MidiCapturingPlugin>();
    auto* capturePtr = capture.get();
    ASSERT_EQ(chain->addPlugin(std::move(capture)), 0);
    ASSERT_TRUE(graph.setTransportPlaying(true));
    ASSERT_TRUE(graph.setClipTransportPlaying(
        track, 0, true, guitarrackcraft::LaunchQuantization::None));

    StereoBuffers buffers;
    clearBuffers(buffers);
    graph.process(buffers.inputs, 2, buffers.outputs, 40);

    ASSERT_EQ(capturePtr->capturedCount(), 1u);
    EXPECT_EQ(capturePtr->captured(0).frameOffset, 10u);
    EXPECT_EQ(capturePtr->captured(0).data1, 60u);
}

TEST(RackGraphTransportTest, PunchUsesConfiguredQuantizationWhenStartArgumentDiffers) {
    RackGraph graph;
    configure(graph);
    const RackPathId track = graph.getTracks().front().id;
    ASSERT_TRUE(graph.setTrackInputArmed(track, true));
    ASSERT_TRUE(graph.setTransportPlaying(true));
    StereoBuffers buffers;
    clearBuffers(buffers);
    graph.process(buffers.inputs, 2, buffers.outputs, 1);
    ASSERT_TRUE(graph.getTransportSnapshot().playing);
    ASSERT_TRUE(graph.setSlotDefaultLoopLength(track, 0, 0.25));
    ASSERT_TRUE(graph.setSlotEnterOnPunch(
        track, 0, true, guitarrackcraft::LaunchQuantization::Quarter));

    // Enter-on-punch uses the slot quantum; None cannot bypass it.
    ASSERT_TRUE(graph.startTrackClipRecording(
        track, 0, guitarrackcraft::LaunchQuantization::None));
    std::vector<guitarrackcraft::TrackSnapshot> tracks;
    const auto& state = trackSnapshot(graph, track, tracks);
    EXPECT_FALSE(state.recordPending);
    EXPECT_FALSE(state.recording);
    EXPECT_TRUE(state.punchArmed);
    const auto slots = graph.getTrackClipSlots(track);
    const auto* slot = findClipSlot(slots, 0);
    ASSERT_NE(slot, nullptr);
    EXPECT_TRUE(slot->wavLoaded);
}

TEST(RackGraphTransportTest, CancelClipRecordingRemovesReservedSlot) {
    RackGraph graph;
    configure(graph);
    const RackPathId track = graph.getTracks().front().id;
    ASSERT_TRUE(graph.setTrackInputArmed(track, true));
    ASSERT_TRUE(configureClipLoop(
        graph, track, 3, 0.25, false, guitarrackcraft::LaunchQuantization::None));
    ASSERT_TRUE(graph.startTrackClipRecording(
        track, 3, guitarrackcraft::LaunchQuantization::None));

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
TEST(RackGraphClipLabelTest, RenameLoadedWavClipPreservesPayloadAndClearsOverride) {
    RackGraph graph;
    configure(graph);
    const RackPathId track = graph.getTracks().front().id;
    constexpr uint32_t slot = 1;

    ASSERT_TRUE(graph.attachTrackWavSlot(
        track, slot,
        makeClip({1.0f, -3.0f, 2.0f}, 60, {-4.0f, 1.0f, 2.0f}, "source.wav")));
    ASSERT_TRUE(graph.selectTrackClipSlot(track, slot));

    const auto originalPeaks = graph.getTrackWaveformPeaks(track, 3);
    ASSERT_EQ(originalPeaks, (std::vector<float>{4.0f, 3.0f, 2.0f}));

    // A label cannot be applied to an empty slot or to a blank name.
    EXPECT_FALSE(graph.renameTrackClip(track, 0, "empty-slot"));
    EXPECT_FALSE(graph.renameTrackClip(track, static_cast<int32_t>(slot), " \t"));

    ASSERT_TRUE(graph.renameTrackClip(track, static_cast<int32_t>(slot), "Renamed take"));
    auto slots = graph.getTrackClipSlots(track);
    const auto* renamed = findClipSlot(slots, slot);
    ASSERT_NE(renamed, nullptr);
    EXPECT_TRUE(renamed->wavLoaded);
    EXPECT_EQ(renamed->displayName, "Renamed take");
    EXPECT_EQ(graph.getTrackWaveformPeaks(track, 3), originalPeaks);

    std::vector<guitarrackcraft::TrackSnapshot> tracks;
    EXPECT_EQ(trackSnapshot(graph, track, tracks).wavDisplayName, "Renamed take");

    // Loading a replacement drops the old slot-specific override.
    ASSERT_TRUE(graph.attachTrackWavSlot(
        track, slot, makeClip({9.0f, 8.0f}, 60, {}, "replacement.wav")));
    slots = graph.getTrackClipSlots(track);
    renamed = findClipSlot(slots, slot);
    ASSERT_NE(renamed, nullptr);
    EXPECT_EQ(renamed->displayName, "replacement.wav");
    EXPECT_EQ(trackSnapshot(graph, track, tracks).wavDisplayName, "replacement.wav");

    // Unloading also removes any override and leaves the slot empty.
    ASSERT_TRUE(graph.renameTrackClip(track, static_cast<int32_t>(slot), "Temporary"));
    ASSERT_TRUE(graph.unloadTrackWavSlot(track, slot));
    slots = graph.getTrackClipSlots(track);
    renamed = findClipSlot(slots, slot);
    ASSERT_NE(renamed, nullptr);
    EXPECT_FALSE(renamed->wavLoaded);
    EXPECT_TRUE(renamed->displayName.empty());
    EXPECT_TRUE(trackSnapshot(graph, track, tracks).wavDisplayName.empty());
}

TEST(RackGraphTransportTest, LoopRecordingCapturesOneBarMonitorsAndLoopsImmediately) {
    RackGraph graph;
    configure(graph);
    const RackPathId track = graph.getTracks().front().id;
    ASSERT_TRUE(graph.setTrackInputArmed(track, true));
    ASSERT_TRUE(graph.setTransportPlaying(true));

    StereoBuffers buffers;
    clearBuffers(buffers);
    graph.process(buffers.inputs, 2, buffers.outputs, 3);
    ASSERT_TRUE(startConfiguredClipRecording(
        graph, track, 1, guitarrackcraft::LaunchQuantization::Bar, false));
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

TEST(RackGraphTransportTest, GlobalStopCancelsRecordingRemovesReservationAndAllowsSecondStart) {
    {
        RackGraph graph;
        configure(graph);
        const RackPathId track = graph.getTracks().front().id;
        ASSERT_TRUE(graph.setTrackInputArmed(track, true));
        ASSERT_TRUE(graph.setTransportPlaying(true));
        StereoBuffers buffers;
        clearBuffers(buffers);
        graph.process(buffers.inputs, 2, buffers.outputs, 1);
        ASSERT_TRUE(graph.getTransportSnapshot().playing);
        ASSERT_TRUE(startConfiguredClipRecording(
            graph, track, 1, guitarrackcraft::LaunchQuantization::Quarter, false));
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
        const auto stoppedSlots = graph.getTrackClipSlots(track);
        const auto* stoppedSlot = findClipSlot(stoppedSlots, 0);
        ASSERT_NE(stoppedSlot, nullptr);
        EXPECT_FALSE(stoppedSlot->wavLoaded);
        ASSERT_TRUE(graph.startTrackClipRecording(
            track, 0, guitarrackcraft::LaunchQuantization::Quarter));
        EXPECT_TRUE(trackSnapshot(graph, track, tracks).wavLoaded);
        EXPECT_TRUE(trackSnapshot(graph, track, tracks).recordPending);
        const auto replacementSlots = graph.getTrackClipSlots(track);
        const auto* replacementSlot = findClipSlot(replacementSlots, 0);
        ASSERT_NE(replacementSlot, nullptr);
        EXPECT_TRUE(replacementSlot->wavLoaded);
    }

    {
        RackGraph graph;
        configure(graph);
        const RackPathId track = graph.getTracks().front().id;
        ASSERT_TRUE(graph.setTrackInputArmed(track, true));
        ASSERT_TRUE(graph.setTransportPlaying(true));
        ASSERT_TRUE(startConfiguredClipRecording(
            graph, track, 1, guitarrackcraft::LaunchQuantization::Quarter, false));
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
        const auto stoppedSlots = graph.getTrackClipSlots(track);
        const auto* stoppedSlot = findClipSlot(stoppedSlots, 0);
        ASSERT_NE(stoppedSlot, nullptr);
        EXPECT_FALSE(stoppedSlot->wavLoaded);
        ASSERT_TRUE(graph.startTrackClipRecording(
            track, 0, guitarrackcraft::LaunchQuantization::Quarter));
        EXPECT_TRUE(trackSnapshot(graph, track, tracks).wavLoaded);
        EXPECT_TRUE(trackSnapshot(graph, track, tracks).recordPending);
        const auto replacementSlots = graph.getTrackClipSlots(track);
        const auto* replacementSlot = findClipSlot(replacementSlots, 0);
        ASSERT_NE(replacementSlot, nullptr);
        EXPECT_TRUE(replacementSlot->wavLoaded);
    }
}

TEST(RackGraphTransportTest, LoopRecordingAudioCallbackDoesNotAllocate) {
    RackGraph graph;
    configure(graph);
    const RackPathId track = graph.getTracks().front().id;
    ASSERT_TRUE(graph.setTrackInputArmed(track, true));
    ASSERT_TRUE(graph.setTrackInputHardwarePair(track, 0));
    ASSERT_TRUE(graph.setTransportPlaying(true));
    ASSERT_TRUE(startConfiguredClipRecording(
        graph, track, 1, guitarrackcraft::LaunchQuantization::Sixteenth, false));
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
        EXPECT_FLOAT_EQ(buffers.outputRight[frame], -0.5f) << "frame " << frame;
    }
}

TEST(RackGraphTransportTest, CompletedLoopSurvivesGlobalStopAndExplicitRelaunch) {
    RackGraph graph;
    configure(graph);
    const RackPathId track = graph.getTracks().front().id;
    ASSERT_TRUE(graph.setTrackInputArmed(track, true));
    ASSERT_TRUE(graph.setTransportPlaying(true));
    StereoBuffers buffers;
    clearBuffers(buffers);
    graph.process(buffers.inputs, 2, buffers.outputs, 1);
    ASSERT_EQ(graph.getTransportSnapshot().transportFrame, 1u);
    ASSERT_TRUE(startConfiguredClipRecording(
        graph, track, 1, guitarrackcraft::LaunchQuantization::Bar, false));
    clearBuffers(buffers);
    graph.process(buffers.inputs, 2, buffers.outputs, 239);
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

    ASSERT_TRUE(graph.setClipTransportPlaying(
        track, 0, true, guitarrackcraft::LaunchQuantization::Sixteenth));
    graph.process(buffers.inputs, 2, buffers.outputs, 15);
    EXPECT_FLOAT_EQ(buffers.outputLeft[14], 300.0f);
    EXPECT_FLOAT_EQ(buffers.outputRight[14], 300.0f);
    EXPECT_TRUE(trackSnapshot(graph, track, tracks).playing);
}

TEST(RackGraphTransportTest, LoopRecordingCapturesExactQuarterNoteLengthAtSixtyBpm) {
    RackGraph graph;
    configure(graph);
    const RackPathId track = graph.getTracks().front().id;
    ASSERT_TRUE(graph.setTrackInputArmed(track, true));
    ASSERT_TRUE(graph.setTransportPlaying(true));

    StereoBuffers buffers;
    clearBuffers(buffers);
    ASSERT_TRUE(startConfiguredClipRecording(
        graph, track, 0.25, guitarrackcraft::LaunchQuantization::None, false));
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
    const auto slots = graph.getTrackClipSlots(track);
    const auto* recorded = findClipSlot(slots, 0);
    ASSERT_NE(recorded, nullptr);
    EXPECT_DOUBLE_EQ(recorded->loopStartQuarterNotes, 0.0);
    EXPECT_DOUBLE_EQ(recorded->loopLengthQuarterNotes, 1.0);
    EXPECT_FLOAT_EQ(buffers.outputLeft[59], 159.0f);
    EXPECT_FLOAT_EQ(buffers.outputRight[59], 159.0f);

    clearBuffers(buffers);
    graph.process(buffers.inputs, 2, buffers.outputs, 1);
    EXPECT_FLOAT_EQ(buffers.outputLeft[0], 100.0f);
    EXPECT_FLOAT_EQ(buffers.outputRight[0], 100.0f);
}

TEST(RackGraphTransportTest, LoopRecordingCanRestartAfterCompletedSlotUnload) {
    RackGraph graph;
    configure(graph);
    const RackPathId track = graph.getTracks().front().id;
    ASSERT_TRUE(graph.setTrackInputArmed(track, true));
    ASSERT_TRUE(graph.setTransportPlaying(true));

    StereoBuffers buffers;
    clearBuffers(buffers);
    ASSERT_TRUE(startConfiguredClipRecording(
        graph, track, 0.25, guitarrackcraft::LaunchQuantization::None, false));
    for (uint32_t frame = 0; frame < 60; ++frame) {
        buffers.left[frame] = 1.0f + static_cast<float>(frame);
        buffers.right[frame] = 1.0f + static_cast<float>(frame);
    }
    graph.process(buffers.inputs, 2, buffers.outputs, 60);

    std::vector<guitarrackcraft::TrackSnapshot> tracks;
    const auto& completed = trackSnapshot(graph, track, tracks);
    ASSERT_TRUE(completed.wavLoaded);
    EXPECT_FALSE(completed.recordPending);
    EXPECT_FALSE(completed.recording);
    EXPECT_DOUBLE_EQ(completed.wavDurationSec, 1.0);

    ASSERT_TRUE(graph.unloadTrackWavSlot(track, 0));
    auto slots = graph.getTrackClipSlots(track);
    const auto* unloaded = findClipSlot(slots, 0);
    ASSERT_NE(unloaded, nullptr);
    EXPECT_FALSE(unloaded->wavLoaded);
    EXPECT_FALSE(unloaded->looping);
    EXPECT_FALSE(trackSnapshot(graph, track, tracks).wavLoaded);

    ASSERT_TRUE(configureClipLoop(
        graph, track, 0, 0.25, false, guitarrackcraft::LaunchQuantization::None));
    EXPECT_TRUE(graph.startTrackClipRecording(
        track, 0, guitarrackcraft::LaunchQuantization::None));

    slots = graph.getTrackClipSlots(track);
    const auto* secondReservation = findClipSlot(slots, 0);
    ASSERT_NE(secondReservation, nullptr);
    EXPECT_TRUE(secondReservation->wavLoaded);
    EXPECT_TRUE(secondReservation->active);
    EXPECT_TRUE(secondReservation->looping);
    EXPECT_DOUBLE_EQ(secondReservation->durationSec, 1.0);
    const auto& secondPending = trackSnapshot(graph, track, tracks);
    EXPECT_TRUE(secondPending.wavLoaded);
    EXPECT_TRUE(secondPending.recordPending);
    EXPECT_FALSE(secondPending.recording);
}


TEST(RackGraphTransportTest, PunchRecordingRejectsInvalidPrerequisites) {
    struct PunchCase {
        const char* name;
        bool inputArmed;
        bool attachClip;
        bool accepted;
    };
    const std::array<PunchCase, 2> cases = {{
        {"input not armed", false, false, false},
        {"existing clip", true, true, false},
    }};

    for (const auto& testCase : cases) {
        SCOPED_TRACE(testCase.name);
        RackGraph graph;
        configure(graph);
        const RackPathId track = graph.getTracks().front().id;
        ASSERT_TRUE(graph.setTrackInputArmed(track, testCase.inputArmed));
        ASSERT_TRUE(graph.setSlotDefaultLoopLength(track, 0, 0.25));
        if (testCase.attachClip) {
            ASSERT_TRUE(graph.attachTrackWav(track, makeClip({9.0f, 8.0f}, 60)));
        } else {
            ASSERT_TRUE(graph.setSlotEnterOnPunch(
                track, 0, true, guitarrackcraft::LaunchQuantization::Quarter));
        }

        EXPECT_EQ(graph.startTrackClipRecording(
                      track, 0, guitarrackcraft::LaunchQuantization::Quarter),
                  testCase.accepted);
        std::vector<guitarrackcraft::TrackSnapshot> tracks;
        const auto& state = trackSnapshot(graph, track, tracks);
        EXPECT_EQ(state.wavLoaded, testCase.attachClip);
        EXPECT_FALSE(state.recordPending);
        EXPECT_FALSE(state.recording);
        EXPECT_FALSE(state.punchArmed);
    }
}

TEST(RackGraphTransportTest, PunchTriggerWhilePlayingHonorsEveryQuantizationBoundary) {
    struct QuantizationCase {
        const char* name;
        guitarrackcraft::LaunchQuantization quantization;
        uint32_t boundary;
    };
    const std::array<QuantizationCase, 5> cases = {{
        {"none", guitarrackcraft::LaunchQuantization::None, 0},
        {"bar", guitarrackcraft::LaunchQuantization::Bar, 240},
        {"quarter", guitarrackcraft::LaunchQuantization::Quarter, 60},
        {"eighth", guitarrackcraft::LaunchQuantization::Eighth, 30},
        {"sixteenth", guitarrackcraft::LaunchQuantization::Sixteenth, 15},
    }};

    for (const auto& testCase : cases) {
        SCOPED_TRACE(testCase.name);
        RackGraph graph;
        configure(graph);
        const RackPathId track = graph.getTracks().front().id;
        ASSERT_TRUE(graph.setTrackInputArmed(track, true));
        ASSERT_TRUE(graph.setSlotDefaultLoopLength(track, 0, 0.25));
        ASSERT_TRUE(graph.setSlotEnterOnPunch(
            track, 0, true, testCase.quantization));

        StereoBuffers buffers;
        clearBuffers(buffers);
        ASSERT_TRUE(graph.setTransportPlaying(true));
        graph.process(buffers.inputs, 2, buffers.outputs, 2);
        ASSERT_TRUE(graph.startTrackClipRecording(
            track, 0, guitarrackcraft::LaunchQuantization::None));

        // One quiet sample completes detector calibration; the next is the trigger.
        clearBuffers(buffers);
        graph.process(buffers.inputs, 2, buffers.outputs, 1);
        buffers.left[0] = 0.08f;
        buffers.right[0] = -0.08f;
        graph.process(buffers.inputs, 2, buffers.outputs, 1);

        std::vector<guitarrackcraft::TrackSnapshot> tracks;
        const auto& afterTrigger = trackSnapshot(graph, track, tracks);
        if (testCase.quantization == guitarrackcraft::LaunchQuantization::None) {
            EXPECT_FALSE(afterTrigger.punchArmed);
            EXPECT_FALSE(afterTrigger.recordPending);
            EXPECT_TRUE(afterTrigger.recording);
            continue;
        }

        EXPECT_FALSE(afterTrigger.punchArmed);
        EXPECT_TRUE(afterTrigger.recordPending);
        const auto now = graph.getTransportSnapshot().transportFrame;
        ASSERT_LT(now, testCase.boundary);

        clearBuffers(buffers);
        graph.process(buffers.inputs, 2, buffers.outputs,
                      testCase.boundary - now);
        const auto& beforeBoundary = trackSnapshot(graph, track, tracks);
        EXPECT_TRUE(beforeBoundary.recordPending);
        EXPECT_FALSE(beforeBoundary.recording);

        allocation_probe::allocations = 0;
        allocation_probe::enabled = true;
        graph.process(buffers.inputs, 2, buffers.outputs, 1);
        allocation_probe::enabled = false;
        EXPECT_EQ(allocation_probe::allocations, 0u);
        const auto& atBoundary = trackSnapshot(graph, track, tracks);
        EXPECT_FALSE(atBoundary.recordPending);
        EXPECT_TRUE(atBoundary.recording);
    }
}

TEST(RackGraphTransportTest, PunchTriggerWhileStoppedStartsRecordingAtFrameZeroForEveryQuantization) {
    const std::array<guitarrackcraft::LaunchQuantization, 5> quantizations = {{
        guitarrackcraft::LaunchQuantization::Bar,
        guitarrackcraft::LaunchQuantization::Quarter,
        guitarrackcraft::LaunchQuantization::Eighth,
        guitarrackcraft::LaunchQuantization::Sixteenth,
        guitarrackcraft::LaunchQuantization::None,
    }};
    for (const auto quantization : quantizations) {
        SCOPED_TRACE(static_cast<int>(quantization));
        RackGraph graph;
        configure(graph);
        const RackPathId track = graph.getTracks().front().id;
        ASSERT_TRUE(graph.setTrackInputArmed(track, true));
        ASSERT_TRUE(graph.setSlotDefaultLoopLength(track, 0, 0.25));
        ASSERT_TRUE(graph.setSlotEnterOnPunch(track, 0, true, quantization));
        ASSERT_TRUE(graph.startTrackClipRecording(
            track, 0, guitarrackcraft::LaunchQuantization::Quarter));

        StereoBuffers buffers;
        clearBuffers(buffers);
        graph.process(buffers.inputs, 2, buffers.outputs, 1);
        std::vector<guitarrackcraft::TrackSnapshot> tracks;
        EXPECT_TRUE(trackSnapshot(graph, track, tracks).punchArmed);
        EXPECT_FALSE(graph.getTransportSnapshot().playing);

        buffers.left[0] = 0.08f;
        buffers.right[0] = -0.08f;
        graph.process(buffers.inputs, 2, buffers.outputs, 1);
        const auto& recording = trackSnapshot(graph, track, tracks);
        EXPECT_FALSE(recording.punchArmed);
        EXPECT_FALSE(recording.recordPending);
        EXPECT_TRUE(recording.recording);
        EXPECT_TRUE(graph.getTransportSnapshot().playing);
        EXPECT_EQ(graph.getTransportSnapshot().transportFrame, 1u);
    }
}
TEST(RackGraphTransportTest, StoppedPunchCapturesNonzeroOffsetTriggerAsFrameZeroAndCountsSuffix) {
    RackGraph graph;
    configure(graph);
    const RackPathId track = graph.getTracks().front().id;
    ASSERT_TRUE(graph.setTrackInputArmed(track, true));
    ASSERT_TRUE(graph.setTrackInputHardwarePair(track, 0));
    ASSERT_TRUE(startConfiguredClipRecording(
        graph, track, 0.25, guitarrackcraft::LaunchQuantization::Quarter, true));

    StereoBuffers buffers;
    clearBuffers(buffers);
    buffers.left[0] = 0.0f;   // detector calibration
    buffers.left[1] = 0.0f;   // quiet prefix
    buffers.left[2] = 0.8f;   // trigger at a nonzero callback offset
    buffers.left[3] = -0.4f;  // suffix
    graph.process(buffers.inputs, 2, buffers.outputs, 4);

    EXPECT_FLOAT_EQ(buffers.outputLeft[2], 0.8f);
    EXPECT_FLOAT_EQ(buffers.outputLeft[3], -0.4f);
    const auto afterTrigger = graph.getTransportSnapshot();
    EXPECT_TRUE(afterTrigger.playing);
    EXPECT_EQ(afterTrigger.transportFrame, 2u);
    std::vector<guitarrackcraft::TrackSnapshot> tracks;
    EXPECT_TRUE(trackSnapshot(graph, track, tracks).recording);
    EXPECT_TRUE(graph.getTrackWaveformPeaks(track, 4).empty());

    clearBuffers(buffers);
    graph.process(buffers.inputs, 2, buffers.outputs, 58);
    const auto& completed = trackSnapshot(graph, track, tracks);
    EXPECT_FALSE(completed.recording);
    EXPECT_FALSE(completed.recordPending);
    EXPECT_EQ(graph.getTransportSnapshot().transportFrame, 60u);
    const auto peaks = graph.getTrackWaveformPeaks(track, 4);
    ASSERT_EQ(peaks.size(), 4u);
    EXPECT_FLOAT_EQ(peaks[0], 0.8f);
    EXPECT_FLOAT_EQ(peaks[1], 0.0f);
    EXPECT_FLOAT_EQ(peaks[2], 0.0f);
    EXPECT_FLOAT_EQ(peaks[3], 0.0f);
}



TEST(RackGraphTransportTest, PunchReservationsCanArmOnMultipleTracks) {
    RackGraph graph;
    configure(graph);
    const RackPathId first = graph.getTracks().front().id;
    const RackPathId second = graph.addTrack();
    ASSERT_NE(second, 0u);
    ASSERT_TRUE(graph.setTrackInputArmed(first, true));
    ASSERT_TRUE(graph.setTrackInputArmed(second, true));
    ASSERT_TRUE(graph.setSlotDefaultLoopLength(first, 0, 0.25));
    ASSERT_TRUE(graph.setSlotDefaultLoopLength(second, 0, 0.25));
    ASSERT_TRUE(graph.setSlotEnterOnPunch(
        first, 0, true, guitarrackcraft::LaunchQuantization::Quarter));
    ASSERT_TRUE(graph.setSlotEnterOnPunch(
        second, 0, true, guitarrackcraft::LaunchQuantization::Quarter));
    ASSERT_TRUE(graph.startTrackClipRecording(
        first, 0, guitarrackcraft::LaunchQuantization::Quarter));
    ASSERT_TRUE(graph.startTrackClipRecording(
        second, 0, guitarrackcraft::LaunchQuantization::Quarter));

    const auto firstSlots = graph.getTrackClipSlots(first);
    const auto secondSlots = graph.getTrackClipSlots(second);
    const auto* firstState = findClipSlot(firstSlots, 0);
    const auto* secondState = findClipSlot(secondSlots, 0);
    ASSERT_NE(firstState, nullptr);
    ASSERT_NE(secondState, nullptr);
    EXPECT_TRUE(firstState->wavLoaded);
    EXPECT_TRUE(firstState->active);
    EXPECT_TRUE(firstState->enterOnPunch);
    EXPECT_TRUE(secondState->wavLoaded);
    EXPECT_TRUE(secondState->active);
    EXPECT_TRUE(secondState->enterOnPunch);

    std::vector<guitarrackcraft::TrackSnapshot> tracks;
    EXPECT_TRUE(trackSnapshot(graph, first, tracks).punchArmed);
    EXPECT_TRUE(trackSnapshot(graph, second, tracks).punchArmed);
}

TEST(RackGraphTransportTest, PunchReservationMovesToSecondSlotOnSameTrack) {
    RackGraph graph;
    configure(graph);
    const RackPathId track = graph.getTracks().front().id;
    ASSERT_TRUE(graph.setTrackInputArmed(track, true));
    ASSERT_TRUE(graph.setSlotDefaultLoopLength(track, 0, 0.25));
    ASSERT_TRUE(graph.setSlotEnterOnPunch(
        track, 0, true, guitarrackcraft::LaunchQuantization::Quarter));
    ASSERT_TRUE(graph.startTrackClipRecording(
        track, 0, guitarrackcraft::LaunchQuantization::Quarter));

    auto slots = graph.getTrackClipSlots(track);
    const auto* firstReservation = findClipSlot(slots, 0);
    ASSERT_NE(firstReservation, nullptr);
    EXPECT_TRUE(firstReservation->wavLoaded);
    EXPECT_TRUE(firstReservation->enterOnPunch);

    ASSERT_TRUE(graph.setSlotDefaultLoopLength(track, 1, 0.25));
    ASSERT_TRUE(graph.setSlotEnterOnPunch(
        track, 1, true, guitarrackcraft::LaunchQuantization::Quarter));
    ASSERT_TRUE(graph.startTrackClipRecording(
        track, 1, guitarrackcraft::LaunchQuantization::Quarter));

    slots = graph.getTrackClipSlots(track);
    const auto* cancelled = findClipSlot(slots, 0);
    const auto* replacement = findClipSlot(slots, 1);
    ASSERT_NE(cancelled, nullptr);
    ASSERT_NE(replacement, nullptr);
    EXPECT_FALSE(cancelled->wavLoaded);
    EXPECT_FALSE(cancelled->active);
    EXPECT_FALSE(cancelled->enterOnPunch);
    EXPECT_TRUE(replacement->wavLoaded);
    EXPECT_TRUE(replacement->active);
    EXPECT_TRUE(replacement->enterOnPunch);

    std::vector<guitarrackcraft::TrackSnapshot> tracks;
    const auto& state = trackSnapshot(graph, track, tracks);
    EXPECT_EQ(state.selectedSlot, 1u);
    EXPECT_TRUE(state.punchArmed);
}

TEST(RackGraphTransportTest, PunchIgnoresSubThresholdInputWhilePaused) {
    RackGraph graph;
    configure(graph);
    const RackPathId track = graph.getTracks().front().id;
    ASSERT_TRUE(graph.setTrackInputArmed(track, true));
    ASSERT_TRUE(startConfiguredClipRecording(
        graph, track, 0.25, guitarrackcraft::LaunchQuantization::Quarter, true));
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
        EXPECT_TRUE(state.wavLoaded);
    }
    const auto after = graph.getTransportSnapshot();
    EXPECT_FALSE(after.playing);
    EXPECT_EQ(after.transportFrame, before.transportFrame);
}

TEST(RackGraphTransportTest, PunchCalibratesSteadyNoiseBeforeTransient) {
    RackGraph graph;
    graph.setSampleRate(1000.0f, 512);
    graph.setBeatsPerMinute(60.0);
    const RackPathId track = graph.getTracks().front().id;
    ASSERT_TRUE(graph.setTrackInputArmed(track, true));
    ASSERT_TRUE(graph.setTrackInputHardwarePair(track, 0));
    ASSERT_TRUE(startConfiguredClipRecording(
        graph, track, 0.25, guitarrackcraft::LaunchQuantization::Quarter, true));
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
    EXPECT_TRUE(stillArmed.wavLoaded);
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
    ASSERT_TRUE(graph.setTrackInputHardwarePair(track, 0));
    ASSERT_TRUE(startConfiguredClipRecording(
        graph, track, 0.25, guitarrackcraft::LaunchQuantization::Quarter, true));
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
    const auto afterHit = graph.getTransportSnapshot();
    EXPECT_FLOAT_EQ(buffers.outputLeft[0], triggerLeft);
    EXPECT_FLOAT_EQ(buffers.outputRight[0], triggerRight);
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
    EXPECT_FLOAT_EQ(buffers.outputRight[0], triggerRight);
}

TEST(RackGraphTransportTest, ManualGlobalPlayPreservesArmedPunchAndQuantizesQuarterTrigger) {
    RackGraph graph;
    configure(graph);
    const RackPathId track = graph.getTracks().front().id;
    ASSERT_TRUE(graph.setTrackInputArmed(track, true));
    ASSERT_TRUE(startConfiguredClipRecording(
        graph, track, 0.25, guitarrackcraft::LaunchQuantization::Quarter, true));
    std::vector<guitarrackcraft::TrackSnapshot> tracks;
    ASSERT_TRUE(trackSnapshot(graph, track, tracks).punchArmed);

    // Manual global Play starts the clock but must leave the armed punch live.
    ASSERT_TRUE(graph.setTransportPlaying(true));
    EXPECT_TRUE(trackSnapshot(graph, track, tracks).punchArmed);

    StereoBuffers buffers;
    clearBuffers(buffers);
    graph.process(buffers.inputs, 2, buffers.outputs, 1);
    buffers.left[0] = 0.08f;
    buffers.right[0] = -0.08f;
    graph.process(buffers.inputs, 2, buffers.outputs, 1);

    const auto& pending = trackSnapshot(graph, track, tracks);
    EXPECT_FALSE(pending.punchArmed);
    EXPECT_TRUE(pending.recordPending);
    EXPECT_FALSE(pending.recording);
    const uint64_t boundary = 60;
    EXPECT_LT(graph.getTransportSnapshot().transportFrame, boundary);

    graph.process(buffers.inputs, 2, buffers.outputs,
                 boundary - graph.getTransportSnapshot().transportFrame);
    EXPECT_TRUE(trackSnapshot(graph, track, tracks).recordPending);
    EXPECT_FALSE(trackSnapshot(graph, track, tracks).recording);
    graph.process(buffers.inputs, 2, buffers.outputs, 1);
    EXPECT_FALSE(trackSnapshot(graph, track, tracks).recordPending);
    EXPECT_TRUE(trackSnapshot(graph, track, tracks).recording);
}

TEST(RackGraphTransportTest, CancelTrackLoopRecordingDisarmsPunch) {
    RackGraph graph;
    configure(graph);
    const RackPathId track = graph.getTracks().front().id;
    ASSERT_TRUE(graph.setTrackInputArmed(track, true));
    ASSERT_TRUE(startConfiguredClipRecording(
        graph, track, 0.25, guitarrackcraft::LaunchQuantization::Quarter, true));
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
    ASSERT_TRUE(graph.setTrackInputHardwarePair(track, 0));
    ASSERT_TRUE(startConfiguredClipRecording(
        graph, track, 0.25, guitarrackcraft::LaunchQuantization::Quarter, true));
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
    EXPECT_FLOAT_EQ(buffers.outputRight[0], -0.08f);
    EXPECT_FLOAT_EQ(buffers.outputLeft[60], 0.08f);
    EXPECT_FLOAT_EQ(buffers.outputRight[60], -0.08f);
}

TEST(RackGraphHardwarePairTest, ArmedTracksSelectDistinctHardwarePairsForMonitoring) {
    RackGraph graph;
    configure(graph);
    const RackPathId first = graph.getTracks().front().id;
    const RackPathId second = graph.addTrack();
    ASSERT_TRUE(graph.setTrackInputHardwarePair(first, 0));
    ASSERT_TRUE(graph.setTrackInputHardwarePair(second, 2));
    ASSERT_TRUE(graph.setTrackInputArmed(first, true));
    ASSERT_TRUE(graph.setTrackInputArmed(second, true));

    FourChannelBuffers buffers;
    clearBuffers(buffers);
    buffers.channel0[0] = 0.25f;
    buffers.channel1[0] = -0.5f;
    buffers.channel2[0] = 1.25f;
    buffers.channel3[0] = -2.0f;
    graph.process(buffers.inputs, 4, buffers.outputs, 1);

    EXPECT_FLOAT_EQ(buffers.outputLeft[0], 1.5f);
    EXPECT_FLOAT_EQ(buffers.outputRight[0], -2.5f);
}

TEST(RackGraphHardwarePairTest, LoopRecordingUsesEachTracksStereoPair) {
    RackGraph graph;
    configure(graph);
    const RackPathId first = graph.getTracks().front().id;
    const RackPathId second = graph.addTrack();
    ASSERT_TRUE(graph.setTrackInputHardwarePair(first, 0));
    ASSERT_TRUE(graph.setTrackInputHardwarePair(second, 2));
    ASSERT_TRUE(graph.setTrackInputArmed(first, true));
    ASSERT_TRUE(graph.setTrackInputArmed(second, true));
    ASSERT_TRUE(graph.setTransportPlaying(true));
    ASSERT_TRUE(startConfiguredClipRecording(
        graph, first, 0.25, guitarrackcraft::LaunchQuantization::Sixteenth, false));
    ASSERT_TRUE(startConfiguredClipRecording(
        graph, second, 0.25, guitarrackcraft::LaunchQuantization::Sixteenth, false));

    FourChannelBuffers buffers;
    clearBuffers(buffers);
    graph.process(buffers.inputs, 4, buffers.outputs, 15);
    for (uint32_t frame = 0; frame < 60; ++frame) {
        buffers.channel0[frame] = 1.0f;
        buffers.channel1[frame] = 2.0f;
        buffers.channel2[frame] = 11.0f;
        buffers.channel3[frame] = 20.0f;
    }
    graph.process(buffers.inputs, 4, buffers.outputs, 60);
    clearBuffers(buffers);
    graph.process(buffers.inputs, 4, buffers.outputs, 1);

    EXPECT_FLOAT_EQ(buffers.outputLeft[0], 12.0f);
    EXPECT_FLOAT_EQ(buffers.outputRight[0], 22.0f);
}

TEST(RackGraphHardwarePairTest, PunchUsesMaximumMagnitudeAcrossEachStereoPair) {
    RackGraph graph;
    configure(graph);
    const RackPathId first = graph.getTracks().front().id;
    const RackPathId second = graph.addTrack();
    ASSERT_TRUE(graph.setTrackInputHardwarePair(first, 0));
    ASSERT_TRUE(graph.setTrackInputHardwarePair(second, 2));
    ASSERT_TRUE(graph.setTrackInputArmed(first, true));
    ASSERT_TRUE(graph.setTrackInputArmed(second, true));
    ASSERT_TRUE(startConfiguredClipRecording(
        graph, first, 0.25, guitarrackcraft::LaunchQuantization::Quarter, true));
    ASSERT_TRUE(startConfiguredClipRecording(
        graph, second, 0.25, guitarrackcraft::LaunchQuantization::Quarter, true));

    FourChannelBuffers buffers;
    clearBuffers(buffers);
    // Calibrate both pair detectors with low-level stereo input.
    buffers.channel0[0] = 0.01f;
    buffers.channel1[0] = 0.02f;
    buffers.channel2[0] = 0.03f;
    buffers.channel3[0] = 0.04f;
    graph.process(buffers.inputs, 4, buffers.outputs, 1);
    std::vector<guitarrackcraft::TrackSnapshot> tracks;
    EXPECT_TRUE(trackSnapshot(graph, first, tracks).punchArmed);
    EXPECT_TRUE(trackSnapshot(graph, second, tracks).punchArmed);

    clearBuffers(buffers);
    // The second pair punches from its right channel only.
    buffers.channel0[0] = 0.01f;
    buffers.channel1[0] = 0.01f;
    buffers.channel2[0] = 0.01f;
    buffers.channel3[0] = 0.08f;
    graph.process(buffers.inputs, 4, buffers.outputs, 1);

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
    ASSERT_TRUE(graph.setTrackInputHardwarePair(track, 0));
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
        EXPECT_FLOAT_EQ(buffers.outputRight[frame], 13.0f) << "frame " << frame;
    }
}

TEST(RackGraphInputRoutingTest, HardwareStereoPairZeroFillsMissingRightChannel) {
    RackGraph graph;
    configure(graph);
    const RackPathId track = graph.getTracks().front().id;
    ASSERT_TRUE(graph.setTrackInputArmed(track, true));
    ASSERT_TRUE(graph.setTrackInputHardwarePair(track, 0));

    StereoBuffers buffers;
    clearBuffers(buffers);
    buffers.left.fill(1.25f);
    buffers.right.fill(-2.5f);
    graph.process(buffers.inputs, 2, buffers.outputs, 4);
    for (uint32_t frame = 0; frame < 4; ++frame) {
        EXPECT_FLOAT_EQ(buffers.outputLeft[frame], 1.25f);
        EXPECT_FLOAT_EQ(buffers.outputRight[frame], -2.5f);
    }

    clearBuffers(buffers);
    buffers.left.fill(3.5f);
    buffers.right.fill(99.0f);
    graph.process(buffers.inputs, 1, buffers.outputs, 4);
    for (uint32_t frame = 0; frame < 4; ++frame) {
        EXPECT_FLOAT_EQ(buffers.outputLeft[frame], 3.5f);
        EXPECT_FLOAT_EQ(buffers.outputRight[frame], 0.0f);
    }
}
TEST(RackGraphHardwareMonoTest, SelectedChannelIsDuplicatedToBothTrackOutputs) {
    RackGraph graph;
    configure(graph);
    const RackPathId track = graph.getTracks().front().id;
    ASSERT_TRUE(graph.setTrackInputArmed(track, true));

    FourChannelBuffers buffers;
    clearBuffers(buffers);
    for (uint32_t frame = 0; frame < 4; ++frame) {
        buffers.channel0[frame] = 1.0f + static_cast<float>(frame);
        buffers.channel1[frame] = 10.0f + static_cast<float>(frame);
    }

    ASSERT_TRUE(graph.setTrackInputHardwareMono(track, 0));
    graph.process(buffers.inputs, 4, buffers.outputs, 4);
    for (uint32_t frame = 0; frame < 4; ++frame) {
        EXPECT_FLOAT_EQ(buffers.outputLeft[frame], buffers.channel0[frame])
            << "channel 0 left frame " << frame;
        EXPECT_FLOAT_EQ(buffers.outputRight[frame], buffers.channel0[frame])
            << "channel 0 right frame " << frame;
    }

    ASSERT_TRUE(graph.setTrackInputHardwareMono(track, 1));
    graph.process(buffers.inputs, 4, buffers.outputs, 4);
    for (uint32_t frame = 0; frame < 4; ++frame) {
        EXPECT_FLOAT_EQ(buffers.outputLeft[frame], buffers.channel1[frame])
            << "channel 1 left frame " << frame;
        EXPECT_FLOAT_EQ(buffers.outputRight[frame], buffers.channel1[frame])
            << "channel 1 right frame " << frame;
    }

    const auto chain = graph.getChain(track);
    ASSERT_NE(chain, nullptr);
    ASSERT_EQ(chain->addPlugin(std::make_unique<StereoOffsetPlugin>()), 0);
    graph.process(buffers.inputs, 4, buffers.outputs, 4);
    for (uint32_t frame = 0; frame < 4; ++frame) {
        EXPECT_FLOAT_EQ(buffers.outputLeft[frame], buffers.channel1[frame] + 10.0f)
            << "plugin left frame " << frame;
        EXPECT_FLOAT_EQ(buffers.outputRight[frame], buffers.channel1[frame] + 20.0f)
            << "plugin right frame " << frame;
    }

    const auto tracks = graph.getTracks();
    ASSERT_EQ(tracks.size(), 1u);
    EXPECT_EQ(tracks.front().inputSourceKind, 2u);
    EXPECT_EQ(tracks.front().inputSourceFirstChannel, 1);
}

TEST(RackGraphHardwareMonoTest, ChannelValidationAllowsPreconfigurationAndRejectsUnavailableChannels) {
    RackGraph graph;
    configure(graph);
    const RackPathId track = graph.getTracks().front().id;

    graph.setAvailableInputChannelCount(0);
    ASSERT_TRUE(graph.setTrackInputHardwareMono(track, 7));
    EXPECT_FALSE(graph.setTrackInputHardwareMono(track, -1));
    EXPECT_FALSE(graph.setTrackInputHardwareMono(track, 8));

    auto tracks = graph.getTracks();
    ASSERT_EQ(tracks.size(), 1u);
    EXPECT_EQ(tracks.front().inputSourceKind, 2u);
    EXPECT_EQ(tracks.front().inputSourceFirstChannel, 7);

    graph.setAvailableInputChannelCount(2);
    ASSERT_TRUE(graph.setTrackInputHardwareMono(track, 1));
    EXPECT_FALSE(graph.setTrackInputHardwareMono(track, 2));
    tracks = graph.getTracks();
    ASSERT_EQ(tracks.size(), 1u);
    EXPECT_EQ(tracks.front().inputSourceFirstChannel, 1);
    graph.setAvailableInputChannelCount(0);
    ASSERT_TRUE(graph.setTrackInputHardwareMono(track, 7));
    tracks = graph.getTracks();
    ASSERT_EQ(tracks.size(), 1u);
    EXPECT_EQ(tracks.front().inputSourceFirstChannel, 7);
}


TEST(RackGraphInputRoutingTest, TrackRoutingHonorsPreAndPostFaderTaps) {
    const std::array<guitarrackcraft::TrackInputTap, 2> taps = {{
        guitarrackcraft::TrackInputTap::PreFader,
        guitarrackcraft::TrackInputTap::PostFader,
    }};
    for (const auto tap : taps) {
        RackGraph graph;
        configure(graph);
        const RackPathId source = graph.getTracks().front().id;
        const RackPathId destination = graph.addTrack();
        ASSERT_NE(destination, 0u);
        ASSERT_TRUE(graph.setTrackInputArmed(source, true));
        ASSERT_TRUE(graph.setTrackInputHardwarePair(source, 0));
        ASSERT_TRUE(graph.setTrackInputArmed(destination, true));
        ASSERT_TRUE(graph.setTrackVolume(source, 0.5f));
        ASSERT_TRUE(graph.setTrackInputTrack(destination, source, tap));

        StereoBuffers buffers;
        clearBuffers(buffers);
        buffers.left.fill(2.0f);
        buffers.right.fill(-3.0f);
        graph.process(buffers.inputs, 2, buffers.outputs, 4);

        const float expectedLeft =
            tap == guitarrackcraft::TrackInputTap::PreFader ? 3.0f : 2.0f;
        const float expectedRight =
            tap == guitarrackcraft::TrackInputTap::PreFader ? -4.5f : -3.0f;
        for (uint32_t frame = 0; frame < 4; ++frame) {
            EXPECT_FLOAT_EQ(buffers.outputLeft[frame], expectedLeft);
            EXPECT_FLOAT_EQ(buffers.outputRight[frame], expectedRight);
        }
    }
}

TEST(RackGraphInputRoutingTest, TrackRoutingRejectsSelfAndCyclicConnections) {
    RackGraph graph;
    configure(graph);
    const RackPathId first = graph.getTracks().front().id;
    const RackPathId second = graph.addTrack();
    ASSERT_NE(second, 0u);

    EXPECT_FALSE(graph.setTrackInputTrack(
        first, first, guitarrackcraft::TrackInputTap::PreFader));
    ASSERT_TRUE(graph.setTrackInputTrack(
        second, first, guitarrackcraft::TrackInputTap::PreFader));
    EXPECT_FALSE(graph.setTrackInputTrack(
        first, second, guitarrackcraft::TrackInputTap::PostFader));

    const auto firstSource = graph.getTrackInputSource(first);
    const auto secondSource = graph.getTrackInputSource(second);
    EXPECT_EQ(firstSource.kind,
              guitarrackcraft::TrackInputSource::Kind::HardwarePair);
    EXPECT_EQ(secondSource.kind,
              guitarrackcraft::TrackInputSource::Kind::TrackOutput);
    EXPECT_EQ(secondSource.trackId, first);
}

TEST(RackGraphInputRoutingTest, RejectedRoutedSourceRemovalPreservesGraphAndChain) {
    RackGraph graph;
    configure(graph);
    const RackPathId source = graph.getTracks().front().id;
    const RackPathId destination = graph.addTrack();
    ASSERT_NE(destination, 0u);
    ASSERT_TRUE(graph.setTrackInputArmed(source, true));
    ASSERT_TRUE(graph.setTrackInputHardwarePair(source, 0));
    ASSERT_TRUE(graph.setTrackInputArmed(destination, true));
    ASSERT_TRUE(graph.setTrackInputTrack(
        destination, source, guitarrackcraft::TrackInputTap::PreFader));

    const auto sourceChain = graph.getChain(source);
    ASSERT_NE(sourceChain, nullptr);
    ASSERT_EQ(sourceChain->addPlugin(std::make_unique<StereoOffsetPlugin>()), 0);

    StereoBuffers beforeBuffers;
    clearBuffers(beforeBuffers);
    beforeBuffers.left.fill(1.5f);
    beforeBuffers.right.fill(-2.0f);
    graph.process(beforeBuffers.inputs, 2, beforeBuffers.outputs, 4);
    const auto beforeLeft = beforeBuffers.outputLeft;
    const auto beforeRight = beforeBuffers.outputRight;

    EXPECT_FALSE(graph.removeTrack(source));

    const auto tracks = graph.getTracks();
    ASSERT_EQ(tracks.size(), 2u);
    EXPECT_EQ(tracks[0].id, source);
    EXPECT_EQ(tracks[1].id, destination);
    const auto destinationSource = graph.getTrackInputSource(destination);
    EXPECT_EQ(destinationSource.kind,
              guitarrackcraft::TrackInputSource::Kind::TrackOutput);
    EXPECT_EQ(destinationSource.trackId, source);
    EXPECT_EQ(destinationSource.tap, guitarrackcraft::TrackInputTap::PreFader);
    EXPECT_EQ(graph.getChain(source), sourceChain);

    StereoBuffers afterBuffers;
    clearBuffers(afterBuffers);
    afterBuffers.left.fill(1.5f);
    afterBuffers.right.fill(-2.0f);
    graph.process(afterBuffers.inputs, 2, afterBuffers.outputs, 4);
    for (uint32_t frame = 0; frame < 4; ++frame) {
        EXPECT_FLOAT_EQ(afterBuffers.outputLeft[frame], beforeLeft[frame])
            << "left frame " << frame;
        EXPECT_FLOAT_EQ(afterBuffers.outputRight[frame], beforeRight[frame])
            << "right frame " << frame;
    }
}
TEST(RackGraphInputRoutingTest, RemovingEarlierActiveTrackKeepsSurvivorMediaAligned) {
    RackGraph graph;
    configure(graph);
    const RackPathId deleted = graph.getTracks().front().id;
    const RackPathId survivor = graph.addTrack();
    ASSERT_NE(survivor, guitarrackcraft::kMasterPathId);

    const auto survivorChain = graph.getChain(survivor);
    ASSERT_NE(survivorChain, nullptr);
    ASSERT_TRUE(graph.attachTrackWavSlot(
        deleted, 0, makeRampClip(64, 10.0f, "deleted.wav")));
    ASSERT_TRUE(graph.attachTrackWavSlot(
        survivor, 0, makeRampClip(64, 100.0f, "survivor.wav")));
    ASSERT_TRUE(graph.setClipLooping(deleted, 0, true));
    ASSERT_TRUE(graph.setClipLooping(survivor, 0, true));
    ASSERT_TRUE(graph.setTransportPlaying(true));
    ASSERT_TRUE(graph.setClipTransportPlaying(
        deleted, 0, true, guitarrackcraft::LaunchQuantization::None));
    ASSERT_TRUE(graph.setClipTransportPlaying(
        survivor, 0, true, guitarrackcraft::LaunchQuantization::None));

    StereoBuffers beforeBuffers;
    clearBuffers(beforeBuffers);
    graph.process(beforeBuffers.inputs, 2, beforeBuffers.outputs, 1);
    EXPECT_FLOAT_EQ(beforeBuffers.outputLeft[0], 110.0f);
    EXPECT_FLOAT_EQ(beforeBuffers.outputRight[0], 110.0f);

    ASSERT_TRUE(graph.removeTrack(deleted));

    const auto tracks = graph.getTracks();
    ASSERT_EQ(tracks.size(), 1u);
    EXPECT_EQ(tracks.front().id, survivor);
    EXPECT_TRUE(tracks.front().wavLoaded);
    EXPECT_EQ(tracks.front().wavDisplayName, "survivor.wav");
    EXPECT_EQ(tracks.front().activeSlot, 0);
    EXPECT_TRUE(tracks.front().playing);
    EXPECT_EQ(graph.getChain(survivor), survivorChain);
    EXPECT_EQ(graph.getChain(deleted), nullptr);

    const auto slots = graph.getTrackClipSlots(survivor);
    const auto* survivorSlot = findClipSlot(slots, 0);
    ASSERT_NE(survivorSlot, nullptr);
    EXPECT_EQ(survivorSlot->trackId, survivor);
    EXPECT_TRUE(survivorSlot->wavLoaded);
    EXPECT_EQ(survivorSlot->displayName, "survivor.wav");
    EXPECT_TRUE(survivorSlot->active);
    EXPECT_TRUE(survivorSlot->playing);

    StereoBuffers afterBuffers;
    clearBuffers(afterBuffers);
    graph.process(afterBuffers.inputs, 2, afterBuffers.outputs, 1);
    EXPECT_FLOAT_EQ(afterBuffers.outputLeft[0], 101.0f);
    EXPECT_FLOAT_EQ(afterBuffers.outputRight[0], 101.0f);
}

TEST(RackGraphSnapshotTest, TrackAndSlotSnapshotsDoNotMixCallbackGenerations) {
    RackGraph graph;
    configure(graph);
    const RackPathId track = graph.getTracks().front().id;
    ASSERT_TRUE(graph.attachTrackWavSlot(track, 0, makeRampClip(128)));
    ASSERT_TRUE(graph.setTransportPlaying(true));
    ASSERT_TRUE(graph.setClipTransportPlaying(
        track, 0, true, guitarrackcraft::LaunchQuantization::None));

    std::atomic<bool> blockNext{false};
    std::atomic<bool> callbackEntered{false};
    std::atomic<bool> releaseCallback{false};
    const auto chain = graph.getChain(track);
    ASSERT_NE(chain, nullptr);
    ASSERT_EQ(chain->addPlugin(std::make_unique<SnapshotGatePlugin>(
                                   blockNext, callbackEntered, releaseCallback)),
              0);

    StereoBuffers buffers;
    clearBuffers(buffers);
    graph.process(buffers.inputs, 2, buffers.outputs, 1);
    const auto beforeTracks = graph.getTracks();
    ASSERT_EQ(beforeTracks.size(), 1u);
    const auto beforeSlots = graph.getTrackClipSlots(track);
    const auto* beforeSlot = findClipSlot(beforeSlots, 0);
    ASSERT_NE(beforeSlot, nullptr);
    ASSERT_TRUE(beforeSlot->playing);

    blockNext.store(true, std::memory_order_release);
    std::thread audio([&] {
        graph.process(buffers.inputs, 2, buffers.outputs, 1);
    });
    for (uint32_t spin = 0;
         spin < 100'000 && !callbackEntered.load(std::memory_order_acquire);
         ++spin) {
        std::this_thread::yield();
    }
    if (!callbackEntered.load(std::memory_order_acquire)) {
        releaseCallback.store(true, std::memory_order_release);
        audio.join();
        FAIL() << "audio callback did not reach the snapshot barrier";
        return;
    }

    std::vector<guitarrackcraft::TrackSnapshot> racedTracks;
    std::vector<TrackClipSlotInfo> racedSlots;
    std::atomic<bool> readerStarted{false};
    std::thread reader([&] {
        readerStarted.store(true, std::memory_order_release);
        racedTracks = graph.getTracks();
        racedSlots = graph.getTrackClipSlots(track);
    });
    for (uint32_t spin = 0;
         spin < 100'000 && !readerStarted.load(std::memory_order_acquire);
         ++spin) {
        std::this_thread::yield();
    }
    if (!readerStarted.load(std::memory_order_acquire)) {
        releaseCallback.store(true, std::memory_order_release);
        audio.join();
        reader.join();
        FAIL() << "snapshot reader did not reach the callback barrier";
        return;
    }

    // Let the reader contend with the callback's post-runtime/pre-publication
    // window, then complete the callback so a seqlock reader can retry.
    for (uint32_t spin = 0; spin < 10'000; ++spin) {
        std::this_thread::yield();
    }

    releaseCallback.store(true, std::memory_order_release);
    audio.join();
    reader.join();

    ASSERT_EQ(racedTracks.size(), 1u);
    ASSERT_EQ(racedSlots.size(), 1u);
    const auto& afterTrack = graph.getTracks().front();
    const auto afterSlots = graph.getTrackClipSlots(track);
    const auto* afterSlot = findClipSlot(afterSlots, 0);
    ASSERT_NE(afterSlot, nullptr);
    const auto& racedTrack = racedTracks.front();
    const auto& racedSlot = racedSlots.front();

    // These getters perform independent bounded status reads. Under callback
    // contention, they may legitimately observe adjacent generations, so
    // validate each result against the callback's monotonic before/after
    // window instead of requiring cross-call timestamp equality.
    const auto expectStatusInWindow = [](const auto& raced,
                                         const auto& before,
                                         const auto& after,
                                         const char* getter) {
        EXPECT_GE(raced.transportFrame, before.transportFrame) << getter;
        EXPECT_LE(raced.transportFrame, after.transportFrame) << getter;
        EXPECT_GE(raced.musicalQuarterNotes,
                  before.musicalQuarterNotes)
            << getter;
        EXPECT_LE(raced.musicalQuarterNotes,
                  after.musicalQuarterNotes)
            << getter;
        EXPECT_GT(raced.sampleRate, 0.0) << getter;
        EXPECT_GE(raced.sampleRate, before.sampleRate) << getter;
        EXPECT_LE(raced.sampleRate, after.sampleRate) << getter;
        EXPECT_GT(raced.capturedAtMonotonicNanos, 0u) << getter;
        EXPECT_GE(raced.capturedAtMonotonicNanos,
                  before.capturedAtMonotonicNanos)
            << getter;
        EXPECT_LE(raced.capturedAtMonotonicNanos,
                  after.capturedAtMonotonicNanos)
            << getter;
    };

    expectStatusInWindow(racedTrack, beforeTracks.front(), afterTrack,
                         "getTracks");
    expectStatusInWindow(racedSlot, *beforeSlot, *afterSlot,
                         "getTrackClipSlots");
}

TEST(RackGraphStatusSnapshotTest,
     GetTracksTerminatesWhenAudioStatusSequenceRemainsOdd) {
    RackGraph graph;
    configure(graph);
    const RackPathId track = graph.getTracks().front().id;
    size_t observedTrackCount = 0;

    const auto chain = graph.getChain(track);
    ASSERT_NE(chain, nullptr);
    ASSERT_EQ(chain->addPlugin(std::make_unique<ReentrantTrackSnapshotPlugin>(
                                   graph, observedTrackCount)),
              0);

    StereoBuffers buffers;
    clearBuffers(buffers);
    graph.process(buffers.inputs, 2, buffers.outputs, 1);

    EXPECT_EQ(observedTrackCount, 1u);
    EXPECT_EQ(graph.getTracks().size(), 1u);
}
namespace {
using guitarrackcraft::RackGraph;
using guitarrackcraft::RackPathId;


class RestorableTestPlugin final : public guitarrackcraft::IPlugin {
public:
    explicit RestorableTestPlugin(std::string id, uint32_t latencyFrames = 0)
        : id_(std::move(id)), latencyFrames_(latencyFrames) {}

    uint32_t getLatencyFrames() const noexcept override { return latencyFrames_; }

    void activate(float, uint32_t) override {}
    void deactivate() override {}

    uint32_t process(const float* const* inputs, float* const* outputs,
                     uint32_t numFrames,
                     const guitarrackcraft::AudioProcessContext&,
                     const guitarrackcraft::MidiEvent*, uint32_t,
                     guitarrackcraft::MidiEvent*, uint32_t) override {
        for (uint32_t frame = 0; frame < numFrames; ++frame) {
            outputs[0][frame] = inputs[0][frame];
            outputs[1][frame] = inputs[1][frame];
        }
        return 0;
    }

    guitarrackcraft::PluginInfo getInfo() const override {
        guitarrackcraft::PluginInfo info;
        info.id = id_;
        info.format = "TEST";
        return info;
    }
    void setParameter(uint32_t, float) override {}
    float getParameter(uint32_t) const override { return 0.0f; }
    uint32_t getNumInputPorts() const override { return 2; }
    uint32_t getNumOutputPorts() const override { return 2; }

    guitarrackcraft::PluginState saveState() override {
        guitarrackcraft::PluginState state;
        state.format = "TEST";
        state.pluginUri = id_;
        return state;
    }
    bool restoreState(const guitarrackcraft::PluginState& state) override {
        return state.format == "TEST" && state.pluginUri == id_;
    }

private:
    std::string id_;
    const uint32_t latencyFrames_;
};
class RestorableTestFactory final : public guitarrackcraft::IPluginFactory {
public:
    std::string getFormat() const override { return "TEST"; }
    std::vector<guitarrackcraft::PluginInfo> enumeratePlugins() override { return {}; }
    std::unique_ptr<guitarrackcraft::IPlugin> createPlugin(const std::string& pluginId) override {
        if (pluginId == "new" || pluginId == "replacement" ||
            pluginId == "other" || pluginId == "master") {
            return std::make_unique<RestorableTestPlugin>(pluginId);
        }
        return nullptr;
    }
    bool initialize() override { return true; }
};
std::string firstPluginId(const std::shared_ptr<guitarrackcraft::PluginChain>& chain) {
    if (!chain || chain->getSize() == 0) return {};
    const auto* plugin = chain->getPlugin(0);
    return plugin ? plugin->getInfo().id : std::string{};
}


guitarrackcraft::PluginState serializedPlugin(const char* id) {
    guitarrackcraft::PluginState state;
    state.format = "TEST";
    state.pluginUri = id;
    return state;
}


TEST(RackGraphStateRestoreTest,
     StartupRestorePreservesTracksButLeavesEveryPluginChainEmpty) {
    RackGraph graph;

    RackGraph::State saved;
    RackGraph::State::Track first;
    first.id = 101;
    first.volume = 0.25f;
    first.chain.plugins.push_back(serializedPlugin("first"));
    RackGraph::State::Track second;
    second.id = 202;
    second.volume = 0.75f;
    saved.tracks = {first, second};
    saved.beatsPerMinute = 137.5;
    saved.transportFrame = 55;
    saved.samplePosition = 110;
    saved.musicalQuarterNotes = 2.75;
    saved.master.plugins.push_back(serializedPlugin("master"));

    // Startup restoration deliberately does not need a plugin registry: it
    // restores rack metadata while deferring all plugin instantiation.
    guitarrackcraft::PluginRegistry registry;
    std::string diagnostic;
    ASSERT_TRUE(graph.restoreState(saved, registry, diagnostic, false)) << diagnostic;

    // Read the transport immediately after startup restoration. This must
    // complete with the restore publication's seqlock balanced.
    const auto transport = graph.getTransportSnapshot();
    EXPECT_DOUBLE_EQ(transport.beatsPerMinute, 137.5);
    EXPECT_EQ(transport.transportFrame, 55u);
    EXPECT_EQ(transport.samplePosition, 110u);
    EXPECT_DOUBLE_EQ(transport.musicalQuarterNotes, 2.75);

    const auto tracks = graph.getTracks();
    ASSERT_EQ(tracks.size(), 2u);
    EXPECT_EQ(tracks[0].id, 101u);
    EXPECT_FLOAT_EQ(tracks[0].volume, 0.25f);
    EXPECT_EQ(tracks[1].id, 202u);
    EXPECT_FLOAT_EQ(tracks[1].volume, 0.75f);
    EXPECT_EQ(graph.getChain(101)->getSize(), 0u);
    EXPECT_EQ(graph.getChain(202)->getSize(), 0u);
    EXPECT_EQ(graph.getChain(guitarrackcraft::kMasterPathId)->getSize(), 0u);
}
TEST(RackGraphStateRestoreTest,
     FullRackRestoreStillRestoresPluginsWhenFlagIsOmitted) {
    RackGraph graph;
    RackGraph::State saved;
    RackGraph::State::Track track;
    track.id = 303;
    track.chain.plugins.push_back(serializedPlugin("new"));
    saved.tracks.push_back(track);

    guitarrackcraft::PluginRegistry registry;
    registry.registerFactory(std::make_unique<RestorableTestFactory>());
    std::string diagnostic;
    ASSERT_TRUE(graph.restoreState(saved, registry, diagnostic)) << diagnostic;
    ASSERT_EQ(graph.getChain(303)->getSize(), 1u);
    EXPECT_EQ(firstPluginId(graph.getChain(303)), "new");
}

TEST(RackGraphStateRestoreTest,
     DeviceChainImportIsScopedAndFailedReplacementIsAtomic) {
    RackGraph graph;
    const RackPathId target = graph.getTracks().front().id;
    const RackPathId other = graph.addTrack();
    ASSERT_NE(other, guitarrackcraft::kMasterPathId);

    ASSERT_EQ(graph.getChain(target)->addPlugin(
                  std::make_unique<RestorableTestPlugin>("old")),
              0);
    ASSERT_EQ(graph.getChain(other)->addPlugin(
                  std::make_unique<RestorableTestPlugin>("other")),
              0);
    ASSERT_EQ(graph.getChain(guitarrackcraft::kMasterPathId)->addPlugin(
                  std::make_unique<RestorableTestPlugin>("master")),
              0);
    guitarrackcraft::PluginRegistry registry;
    registry.registerFactory(std::make_unique<RestorableTestFactory>());

    guitarrackcraft::PluginChain::ChainState exported;
    std::string diagnostic;
    ASSERT_TRUE(graph.exportDeviceChain(target, exported, diagnostic)) << diagnostic;
    ASSERT_EQ(exported.plugins.size(), 1u);
    EXPECT_EQ(exported.plugins.front().format, "TEST");
    EXPECT_EQ(exported.plugins.front().pluginUri, "old");

    guitarrackcraft::PluginChain::ChainState replacement;
    replacement.plugins.push_back(serializedPlugin("new"));

    ASSERT_TRUE(graph.importDeviceChain(target, replacement, registry, diagnostic))
        << diagnostic;
    EXPECT_EQ(firstPluginId(graph.getChain(target)), "new");
    EXPECT_EQ(firstPluginId(graph.getChain(other)), "other");
    EXPECT_EQ(firstPluginId(graph.getChain(guitarrackcraft::kMasterPathId)),
              "master");

    // The second plugin cannot be created. The first one must not leak into
    // the live chain, and unrelated paths must remain untouched.
    guitarrackcraft::PluginChain::ChainState malformed;
    malformed.plugins.push_back(serializedPlugin("replacement"));
    malformed.plugins.push_back(serializedPlugin("missing"));
    diagnostic.clear();
    EXPECT_FALSE(graph.importDeviceChain(target, malformed, registry, diagnostic));
    EXPECT_EQ(diagnostic, "plugin-create-failed:TEST:missing");
    EXPECT_EQ(firstPluginId(graph.getChain(target)), "new");
    EXPECT_EQ(graph.getChain(target)->getSize(), 1u);
    EXPECT_EQ(firstPluginId(graph.getChain(other)), "other");
    EXPECT_EQ(firstPluginId(graph.getChain(guitarrackcraft::kMasterPathId)),
              "master");
}
TEST(RackGraphPdcTest, DynamicReportedLatencyDropClearsChainCache) {
    RackGraph graph;
    configure(graph, 64);
    const RackPathId track = graph.getTracks().front().id;
    const auto chain = graph.getChain(track);
    ASSERT_NE(chain, nullptr);

    auto plugin = std::make_unique<MutableLatencyPlugin>(4);
    auto* pluginPtr = plugin.get();
    ASSERT_EQ(chain->addPlugin(std::move(plugin)), 0);
    EXPECT_EQ(chain->getLatencyFrames(), 4u);

    StereoBuffers buffers;
    clearBuffers(buffers);
    graph.process(buffers.inputs, 2, buffers.outputs, 1);

    pluginPtr->setLatencyFrames(0);
    clearBuffers(buffers);
    graph.process(buffers.inputs, 2, buffers.outputs, 1);

    // The public reported-latency seam can change at runtime; the aggregate
    // cache must publish zero rather than retaining the previous value.
    EXPECT_EQ(chain->getLatencyFrames(), 0u);
    EXPECT_FALSE(chain->hasLatencyOverflow());
}

TEST(RackGraphPdcTest, ChainMutationAndRestoreUpdateLatencyCacheAndRollback) {
    RackGraph graph;
    configure(graph, 64);
    const RackPathId track = graph.getTracks().front().id;
    const auto chain = graph.getChain(track);
    ASSERT_NE(chain, nullptr);

    constexpr uint32_t maxPdc =
        guitarrackcraft::PluginChain::kMaxSupportedPdcFrames;
    ASSERT_EQ(chain->addPlugin(
                  std::make_unique<RestorableTestPlugin>("latency-a", 7)),
              0);
    EXPECT_EQ(chain->getLatencyFrames(), 7u);
    EXPECT_FALSE(chain->hasLatencyOverflow());

    guitarrackcraft::PluginState restored;
    restored.format = "TEST";
    restored.pluginUri = "latency-a";
    restored.manualLatencyFrames = 13;
    ASSERT_TRUE(chain->restorePluginState(0, restored));
    EXPECT_EQ(chain->getLatencyFrames(), 20u);
    EXPECT_EQ(chain->getManualLatencyFrames(0), 13u);

    // Bring the chain exactly to the supported limit, then reject a restore
    // that would overflow it. The old manual value and cache must survive.
    ASSERT_EQ(chain->addPlugin(std::make_unique<RestorableTestPlugin>(
                  "latency-b", maxPdc - 20)),
              1);
    EXPECT_EQ(chain->getLatencyFrames(), maxPdc);
    EXPECT_FALSE(chain->hasLatencyOverflow());

    restored.manualLatencyFrames = 14;
    EXPECT_FALSE(chain->restorePluginState(0, restored));
    EXPECT_EQ(chain->getManualLatencyFrames(0), 13u);
    EXPECT_EQ(chain->getLatencyFrames(), maxPdc);
    EXPECT_FALSE(chain->hasLatencyOverflow());

    ASSERT_TRUE(chain->removePlugin(1));
    EXPECT_EQ(chain->getLatencyFrames(), 20u);
    EXPECT_FALSE(chain->hasLatencyOverflow());
    ASSERT_TRUE(chain->removePlugin(0));
    EXPECT_EQ(chain->getLatencyFrames(), 0u);
    EXPECT_FALSE(chain->hasLatencyOverflow());
}

TEST(RackGraphPdcTest,
     OverflowingPathDoesNotSuppressValidMixedPathOrUnderflowItsDelay) {
    RackGraph graph;
    configure(graph, 64);
    const RackPathId overflowingTrack = graph.getTracks().front().id;
    const RackPathId validTrack = graph.addTrack();
    ASSERT_NE(validTrack, guitarrackcraft::kMasterPathId);

    ASSERT_TRUE(graph.setTrackInputArmed(overflowingTrack, true));
    ASSERT_TRUE(graph.setTrackInputHardwareMono(overflowingTrack, 0));
    ASSERT_TRUE(graph.setTrackInputArmed(validTrack, true));
    ASSERT_TRUE(graph.setTrackInputHardwareMono(validTrack, 1));

    const auto overflowingChain = graph.getChain(overflowingTrack);
    const auto validChain = graph.getChain(validTrack);
    ASSERT_NE(overflowingChain, nullptr);
    ASSERT_NE(validChain, nullptr);
    constexpr uint32_t maxPdc =
        guitarrackcraft::PluginChain::kMaxSupportedPdcFrames;
    ASSERT_EQ(overflowingChain->addPlugin(
                  std::make_unique<FakeLatencyPlugin>(0, maxPdc)),
              0);
    ASSERT_EQ(overflowingChain->addPlugin(
                  std::make_unique<FakeLatencyPlugin>(0, maxPdc)),
              1);
    ASSERT_EQ(validChain->addPlugin(
                  std::make_unique<FakeLatencyPlugin>(0, 0)),
              0);
    ASSERT_TRUE(overflowingChain->hasLatencyOverflow());
    EXPECT_TRUE(graph.hasPluginLatencyOverflow(overflowingTrack));
    EXPECT_FALSE(validChain->hasLatencyOverflow());
    EXPECT_FALSE(graph.hasPluginLatencyOverflow(validTrack));

    StereoBuffers buffers;
    clearBuffers(buffers);
    for (uint32_t frame = 0; frame < 8; ++frame) {
        buffers.left[frame] = 10.0f + static_cast<float>(frame);
        buffers.right[frame] = 100.0f + static_cast<float>(frame);
    }
    graph.process(buffers.inputs, 2, buffers.outputs, 8);

    // Overflow disables alignment rather than silencing the graph. Both
    // independent paths remain audible, including the valid path.
    for (uint32_t frame = 0; frame < 8; ++frame) {
        const float expected =
            buffers.left[frame] + buffers.right[frame];
        EXPECT_FLOAT_EQ(buffers.outputLeft[frame], expected)
            << "left frame " << frame;
        EXPECT_FLOAT_EQ(buffers.outputRight[frame], expected)
            << "right frame " << frame;
    }
}

TEST(RackGraphPdcTest, ManualLatencyOverrideRejectsCumulativeChainBudget) {
    RackGraph graph;
    configure(graph, 64);
    const RackPathId track = graph.getTracks().front().id;
    const auto chain = graph.getChain(track);
    ASSERT_NE(chain, nullptr);

    ASSERT_EQ(chain->addPlugin(std::make_unique<FakeLatencyPlugin>(0, 0)), 0);
    ASSERT_EQ(chain->addPlugin(std::make_unique<FakeLatencyPlugin>(0, 0)), 1);

    constexpr uint32_t maxPdc = guitarrackcraft::PluginChain::kMaxSupportedPdcFrames;
    ASSERT_TRUE(chain->setManualLatencyFrames(0, maxPdc / 2));
    ASSERT_TRUE(chain->setManualLatencyFrames(1, maxPdc - maxPdc / 2));
    EXPECT_EQ(chain->getLatencyFrames(), maxPdc);

    EXPECT_FALSE(chain->setManualLatencyFrames(
        1, maxPdc - maxPdc / 2 + 1));
    EXPECT_EQ(chain->getManualLatencyFrames(0), maxPdc / 2);
    EXPECT_EQ(chain->getManualLatencyFrames(1), maxPdc - maxPdc / 2);
    EXPECT_EQ(chain->getLatencyFrames(), maxPdc);
}

TEST(RackGraphPdcTest, RoutedPathBudgetRejectionPreservesRouteAndManualState) {
    RackGraph graph;
    configure(graph, 64);
    const RackPathId sourceTrack = graph.getTracks().front().id;
    const RackPathId routedTrack = graph.addTrack();
    ASSERT_NE(routedTrack, guitarrackcraft::kMasterPathId);

    const auto sourceChain = graph.getChain(sourceTrack);
    const auto routedChain = graph.getChain(routedTrack);
    ASSERT_NE(sourceChain, nullptr);
    ASSERT_NE(routedChain, nullptr);
    constexpr uint32_t maxPdc = guitarrackcraft::PluginChain::kMaxSupportedPdcFrames;
    ASSERT_EQ(sourceChain->addPlugin(std::make_unique<FakeLatencyPlugin>(0, 0)), 0);
    ASSERT_EQ(routedChain->addPlugin(std::make_unique<FakeLatencyPlugin>(0, 0)), 0);

    ASSERT_TRUE(graph.setManualLatencyFrames(sourceTrack, 0, maxPdc));
    EXPECT_EQ(graph.getManualLatencyFrames(sourceTrack, 0), maxPdc);
    EXPECT_EQ(sourceChain->getLatencyFrames(), maxPdc);

    ASSERT_TRUE(graph.setTrackInputArmed(routedTrack, true));
    ASSERT_TRUE(graph.setTrackInputTrack(
        routedTrack, sourceTrack, guitarrackcraft::TrackInputTap::PostFader));

    EXPECT_FALSE(graph.setManualLatencyFrames(routedTrack, 0, 1));
    EXPECT_EQ(graph.getManualLatencyFrames(routedTrack, 0), 0u);
    EXPECT_EQ(routedChain->getLatencyFrames(), 0u);

    const auto destinationSource = graph.getTrackInputSource(routedTrack);
    EXPECT_EQ(destinationSource.kind,
              guitarrackcraft::TrackInputSource::Kind::TrackOutput);
    EXPECT_EQ(destinationSource.trackId, sourceTrack);
    EXPECT_EQ(destinationSource.tap,
              guitarrackcraft::TrackInputTap::PostFader);
}

TEST(RackGraphPdcTest, UnequalTrackLatenciesAlignAtFinalMix) {
    RackGraph graph;
    configure(graph, 64);
    const RackPathId delayedTrack = graph.getTracks().front().id;
    const RackPathId directTrack = graph.addTrack();
    ASSERT_NE(directTrack, guitarrackcraft::kMasterPathId);

    ASSERT_TRUE(graph.setTrackInputArmed(delayedTrack, true));
    ASSERT_TRUE(graph.setTrackInputHardwareMono(delayedTrack, 0));
    ASSERT_TRUE(graph.setTrackInputArmed(directTrack, true));
    ASSERT_TRUE(graph.setTrackInputHardwareMono(directTrack, 1));

    const auto delayedChain = graph.getChain(delayedTrack);
    const auto directChain = graph.getChain(directTrack);
    ASSERT_NE(delayedChain, nullptr);
    ASSERT_NE(directChain, nullptr);
    ASSERT_EQ(delayedChain->addPlugin(std::make_unique<FakeLatencyPlugin>(4)), 0);
    ASSERT_EQ(directChain->addPlugin(std::make_unique<FakeLatencyPlugin>(0)), 0);

    StereoBuffers buffers;
    clearBuffers(buffers);
    graph.process(buffers.inputs, 2, buffers.outputs, 8);

    clearBuffers(buffers);
    buffers.left[0] = 1.0f;
    buffers.right[0] = 2.0f;
    graph.process(buffers.inputs, 2, buffers.outputs, 8);

    for (uint32_t frame = 0; frame < 8; ++frame) {
        const float expected = frame == 4 ? 3.0f : 0.0f;
        EXPECT_FLOAT_EQ(buffers.outputLeft[frame], expected) << "left frame " << frame;
        EXPECT_FLOAT_EQ(buffers.outputRight[frame], expected) << "right frame " << frame;
    }
}

TEST(RackGraphPdcTest, ManualLatencyOverrideAlignsParallelTracksAndClearRestoresDirectMix) {
    RackGraph graph;
    configure(graph, 64);
    const RackPathId delayedTrack = graph.getTracks().front().id;
    const RackPathId directTrack = graph.addTrack();
    ASSERT_NE(directTrack, guitarrackcraft::kMasterPathId);

    ASSERT_TRUE(graph.setTrackInputArmed(delayedTrack, true));
    ASSERT_TRUE(graph.setTrackInputHardwareMono(delayedTrack, 0));
    ASSERT_TRUE(graph.setTrackInputArmed(directTrack, true));
    ASSERT_TRUE(graph.setTrackInputHardwareMono(directTrack, 1));

    const auto delayedChain = graph.getChain(delayedTrack);
    const auto directChain = graph.getChain(directTrack);
    ASSERT_NE(delayedChain, nullptr);
    ASSERT_NE(directChain, nullptr);
    ASSERT_EQ(delayedChain->addPlugin(
                  std::make_unique<FakeLatencyPlugin>(4, 0)),
              0);
    ASSERT_EQ(directChain->addPlugin(std::make_unique<FakeLatencyPlugin>(0)),
              0);

    StereoBuffers buffers;
    clearBuffers(buffers);
    graph.process(buffers.inputs, 2, buffers.outputs, 8);
    EXPECT_FALSE(graph.setManualLatencyFrames(delayedTrack, 1, 4));
    EXPECT_EQ(graph.getManualLatencyFrames(delayedTrack, 1), 0u);
    EXPECT_FALSE(graph.setManualLatencyFrames(0xfeed, 0, 4));

    ASSERT_TRUE(graph.setManualLatencyFrames(delayedTrack, 0, 4));
    EXPECT_EQ(graph.getManualLatencyFrames(delayedTrack, 0), 4u);
    EXPECT_EQ(delayedChain->getLatencyFrames(), 4u);

    clearBuffers(buffers);
    buffers.left[0] = 1.0f;
    buffers.right[0] = 2.0f;
    graph.process(buffers.inputs, 2, buffers.outputs, 8);
    for (uint32_t frame = 0; frame < 8; ++frame) {
        const float expected = frame == 4 ? 3.0f : 0.0f;
        EXPECT_FLOAT_EQ(buffers.outputLeft[frame], expected)
            << "left frame " << frame;
        EXPECT_FLOAT_EQ(buffers.outputRight[frame], expected)
            << "right frame " << frame;
    }

    ASSERT_TRUE(graph.setManualLatencyFrames(delayedTrack, 0, 0));
    EXPECT_EQ(graph.getManualLatencyFrames(delayedTrack, 0), 0u);
    EXPECT_EQ(delayedChain->getLatencyFrames(), 0u);

    clearBuffers(buffers);
    buffers.left.fill(3.0f);
    buffers.right.fill(4.0f);
    graph.process(buffers.inputs, 2, buffers.outputs, 8);
    for (uint32_t frame = 0; frame < 8; ++frame) {
        const float expected = frame < 4 ? 4.0f : 7.0f;
        EXPECT_FLOAT_EQ(buffers.outputLeft[frame], expected)
            << "left frame " << frame;
        EXPECT_FLOAT_EQ(buffers.outputRight[frame], expected)
            << "right frame " << frame;
    }
}

TEST(RackGraphPdcTest, EffectiveLatencyCombinesReportedAndManualComponentsPerPlugin) {
    RackGraph graph;
    configure(graph, 64);
    const RackPathId track = graph.getTracks().front().id;
    const auto chain = graph.getChain(track);
    ASSERT_NE(chain, nullptr);

    ASSERT_EQ(chain->addPlugin(std::make_unique<FakeLatencyPlugin>(2, 7)), 0);
    ASSERT_EQ(chain->addPlugin(std::make_unique<FakeLatencyPlugin>(3, 11)), 1);

    EXPECT_EQ(graph.getPluginLatencyFrames(track, 0), 7u);
    EXPECT_EQ(graph.getPluginLatencyFrames(track, 1), 11u);
    EXPECT_EQ(graph.getPluginEffectiveLatencyFrames(track, 0), 7u);
    EXPECT_EQ(graph.getPluginEffectiveLatencyFrames(track, 1), 11u);

    ASSERT_TRUE(graph.setManualLatencyFrames(track, 0, 5));
    ASSERT_TRUE(graph.setManualLatencyFrames(track, 1, 4));

    EXPECT_EQ(graph.getManualLatencyFrames(track, 0), 5u);
    EXPECT_EQ(graph.getManualLatencyFrames(track, 1), 4u);
    EXPECT_EQ(graph.getPluginEffectiveLatencyFrames(track, 0), 12u);
    EXPECT_EQ(graph.getPluginEffectiveLatencyFrames(track, 1), 15u);
}

TEST(RackGraphPdcTest, RoutedLatencyUsesRawPostChainTapsAndAccumulatedPath) {
    const std::array<guitarrackcraft::TrackInputTap, 2> taps = {{
        guitarrackcraft::TrackInputTap::PreFader,
        guitarrackcraft::TrackInputTap::PostFader,
    }};
    for (const auto tap : taps) {
        SCOPED_TRACE(tap == guitarrackcraft::TrackInputTap::PreFader
                         ? "pre-fader"
                         : "post-fader");
        RackGraph graph;
        configure(graph, 64);
        const RackPathId sourceTrack = graph.getTracks().front().id;
        const RackPathId routedTrack = graph.addTrack();
        const RackPathId independentTrack = graph.addTrack();
        ASSERT_NE(routedTrack, guitarrackcraft::kMasterPathId);
        ASSERT_NE(independentTrack, guitarrackcraft::kMasterPathId);

        ASSERT_TRUE(graph.setTrackInputArmed(sourceTrack, true));
        ASSERT_TRUE(graph.setTrackInputHardwareMono(sourceTrack, 0));
        ASSERT_TRUE(graph.setTrackVolume(sourceTrack, 0.5f));
        ASSERT_TRUE(graph.setTrackInputArmed(routedTrack, true));
        ASSERT_TRUE(graph.setTrackInputTrack(routedTrack, sourceTrack, tap));
        ASSERT_TRUE(graph.setTrackInputArmed(independentTrack, true));
        ASSERT_TRUE(graph.setTrackInputHardwareMono(independentTrack, 1));

        const auto sourceChain = graph.getChain(sourceTrack);
        const auto routedChain = graph.getChain(routedTrack);
        const auto independentChain = graph.getChain(independentTrack);
        ASSERT_NE(sourceChain, nullptr);
        ASSERT_NE(routedChain, nullptr);
        ASSERT_NE(independentChain, nullptr);
        ASSERT_EQ(sourceChain->addPlugin(std::make_unique<FakeLatencyPlugin>(3)), 0);
        ASSERT_EQ(routedChain->addPlugin(std::make_unique<FakeLatencyPlugin>(2)), 0);
        ASSERT_EQ(independentChain->addPlugin(std::make_unique<FakeLatencyPlugin>(5)), 0);

        StereoBuffers buffers;
        clearBuffers(buffers);
        graph.process(buffers.inputs, 2, buffers.outputs, 8);

        clearBuffers(buffers);
        buffers.left[0] = 1.0f;
        buffers.right[0] = 4.0f;
        graph.process(buffers.inputs, 2, buffers.outputs, 8);

        const float routedContribution =
            tap == guitarrackcraft::TrackInputTap::PreFader ? 1.0f : 0.5f;
        const float expectedAtAlignment = 0.5f + routedContribution + 4.0f;
        for (uint32_t frame = 0; frame < 8; ++frame) {
            const float expected = frame == 5 ? expectedAtAlignment : 0.0f;
            EXPECT_FLOAT_EQ(buffers.outputLeft[frame], expected)
                << "left frame " << frame;
            EXPECT_FLOAT_EQ(buffers.outputRight[frame], expected)
                << "right frame " << frame;
        }
    }
}

TEST(RackGraphPdcTest, CompensatedProcessingDoesNotAllocate) {
    RackGraph graph;
    configure(graph, 64);
    const RackPathId delayedTrack = graph.getTracks().front().id;
    const RackPathId directTrack = graph.addTrack();
    ASSERT_NE(directTrack, guitarrackcraft::kMasterPathId);

    ASSERT_TRUE(graph.setTrackInputArmed(delayedTrack, true));
    ASSERT_TRUE(graph.setTrackInputHardwareMono(delayedTrack, 0));
    ASSERT_TRUE(graph.setTrackInputArmed(directTrack, true));
    ASSERT_TRUE(graph.setTrackInputHardwareMono(directTrack, 1));

    const auto delayedChain = graph.getChain(delayedTrack);
    const auto directChain = graph.getChain(directTrack);
    ASSERT_NE(delayedChain, nullptr);
    ASSERT_NE(directChain, nullptr);
    ASSERT_EQ(delayedChain->addPlugin(std::make_unique<FakeLatencyPlugin>(6)), 0);
    ASSERT_EQ(directChain->addPlugin(std::make_unique<FakeLatencyPlugin>(0)), 0);

    StereoBuffers buffers;
    clearBuffers(buffers);
    graph.process(buffers.inputs, 2, buffers.outputs, 8);
    clearBuffers(buffers);
    graph.process(buffers.inputs, 2, buffers.outputs, 8);

    clearBuffers(buffers);
    buffers.left[0] = 1.0f;
    buffers.right[0] = 2.0f;
    allocation_probe::allocations = 0;
    allocation_probe::enabled = true;
    graph.process(buffers.inputs, 2, buffers.outputs, 8);
    allocation_probe::enabled = false;

    EXPECT_EQ(allocation_probe::allocations, 0u);
    for (uint32_t frame = 0; frame < 8; ++frame) {
        const float expected = frame == 6 ? 3.0f : 0.0f;
        EXPECT_FLOAT_EQ(buffers.outputLeft[frame], expected)
            << "left frame " << frame;
        EXPECT_FLOAT_EQ(buffers.outputRight[frame], expected)
            << "right frame " << frame;
    }
}

TEST(RackGraphPdcTest,
     LatencyChangeResetsHistoryBeforeCompensatedBlockWithoutAudioAllocation) {
    RackGraph graph;
    configure(graph, 64);
    const RackPathId changingTrack = graph.getTracks().front().id;
    const RackPathId directTrack = graph.addTrack();
    ASSERT_NE(directTrack, guitarrackcraft::kMasterPathId);

    ASSERT_TRUE(graph.setTrackInputArmed(changingTrack, true));
    ASSERT_TRUE(graph.setTrackInputHardwareMono(changingTrack, 0));
    ASSERT_TRUE(graph.setTrackInputArmed(directTrack, true));
    ASSERT_TRUE(graph.setTrackInputHardwareMono(directTrack, 1));

    const auto changingChain = graph.getChain(changingTrack);
    ASSERT_NE(changingChain, nullptr);
    auto changingPlugin = std::make_unique<MutableLatencyPlugin>(0);
    auto* changingPluginPtr = changingPlugin.get();
    ASSERT_EQ(changingChain->addPlugin(std::move(changingPlugin)), 0);

    StereoBuffers buffers;
    clearBuffers(buffers);
    for (uint32_t frame = 0; frame < 8; ++frame) {
        buffers.left[frame] = 10.0f + static_cast<float>(frame);
        buffers.right[frame] = 100.0f + static_cast<float>(frame);
    }
    graph.process(buffers.inputs, 2, buffers.outputs, 8);

    // The chain publishes its changed latency after processing the staging
    // block. The next boundary must discard both paths' old timelines.
    changingPluginPtr->setLatencyFrames(4);
    clearBuffers(buffers);
    for (uint32_t frame = 0; frame < 8; ++frame) {
        buffers.left[frame] = 20.0f + static_cast<float>(frame);
        buffers.right[frame] = 200.0f + static_cast<float>(frame);
    }
    graph.process(buffers.inputs, 2, buffers.outputs, 8);

    clearBuffers(buffers);
    for (uint32_t frame = 0; frame < 8; ++frame) {
        buffers.left[frame] = 30.0f + static_cast<float>(frame);
        buffers.right[frame] = 300.0f + static_cast<float>(frame);
    }
    allocation_probe::allocations = 0;
    allocation_probe::enabled = true;
    graph.process(buffers.inputs, 2, buffers.outputs, 8);
    allocation_probe::enabled = false;

    EXPECT_EQ(allocation_probe::allocations, 0u);
    // The changing path is already at the global maximum, while the
    // previously direct path warms up from an empty history.
    for (uint32_t frame = 0; frame < 8; ++frame) {
        const float expected = frame < 4
            ? 30.0f + static_cast<float>(frame)
            : 30.0f + static_cast<float>(frame) +
                  300.0f + static_cast<float>(frame - 4);
        EXPECT_FLOAT_EQ(buffers.outputLeft[frame], expected)
            << "left frame " << frame;
        EXPECT_FLOAT_EQ(buffers.outputRight[frame], expected)
            << "right frame " << frame;
    }

    clearBuffers(buffers);
    for (uint32_t frame = 0; frame < 8; ++frame) {
        buffers.left[frame] = 40.0f + static_cast<float>(frame);
        buffers.right[frame] = 400.0f + static_cast<float>(frame);
    }
    graph.process(buffers.inputs, 2, buffers.outputs, 8);

    // History starts at the transition boundary: no stale pre-transition
    // sample or duplicated read-head value may precede the aligned signal.
    for (uint32_t frame = 0; frame < 8; ++frame) {
        const float expected = frame < 4
            ? 40.0f + static_cast<float>(frame) +
                  300.0f + static_cast<float>(frame + 4)
            : 40.0f + static_cast<float>(frame) +
                  400.0f + static_cast<float>(frame - 4);
        EXPECT_FLOAT_EQ(buffers.outputLeft[frame], expected)
            << "left frame " << frame;
        EXPECT_FLOAT_EQ(buffers.outputRight[frame], expected)
            << "right frame " << frame;
    }
}

TEST(RackGraphPdcTest, DirectShortcutTransitionUsesSeededHistoryAtBlockBoundary) {
    RackGraph graph;
    configure(graph, 64);
    const RackPathId track = graph.getTracks().front().id;
    ASSERT_TRUE(graph.setTrackInputArmed(track, true));
    ASSERT_TRUE(graph.setTrackInputHardwareMono(track, 0));

    const auto chain = graph.getChain(track);
    ASSERT_NE(chain, nullptr);
    auto changingPlugin = std::make_unique<MutableLatencyPlugin>(0);
    auto* changingPluginPtr = changingPlugin.get();
    ASSERT_EQ(chain->addPlugin(std::move(changingPlugin)), 0);

    StereoBuffers buffers;
    clearBuffers(buffers);
    buffers.left.fill(3.0f);
    graph.process(buffers.inputs, 2, buffers.outputs, 8);

    // This block still uses the direct shortcut because the chain's cached
    // latency is updated at the end of processing. A single path is already
    // the global maximum, so it must not self-delay while entering the next block.
    changingPluginPtr->setLatencyFrames(4);
    clearBuffers(buffers);
    buffers.left.fill(7.0f);
    graph.process(buffers.inputs, 2, buffers.outputs, 8);

    clearBuffers(buffers);
    buffers.left.fill(11.0f);
    allocation_probe::allocations = 0;
    allocation_probe::enabled = true;
    graph.process(buffers.inputs, 2, buffers.outputs, 8);
    allocation_probe::enabled = false;

    EXPECT_EQ(allocation_probe::allocations, 0u);
    // With no parallel lower-latency path, PDC adds no self-delay.
    for (uint32_t frame = 0; frame < 8; ++frame) {
        EXPECT_FLOAT_EQ(buffers.outputLeft[frame], 11.0f)
            << "left frame " << frame;
        EXPECT_FLOAT_EQ(buffers.outputRight[frame], 11.0f)
            << "right frame " << frame;
    }
}

class TempMediaDirectory {
public:
    TempMediaDirectory() {
        char pattern[] = "/tmp/grc_rack_media_tests_XXXXXX";
        if (const char* path = ::mkdtemp(pattern)) path_ = path;
    }

    ~TempMediaDirectory() {
        if (!path_.empty()) {
            std::error_code error;
            std::filesystem::remove_all(path_, error);
        }
    }

    const std::string& path() const { return path_; }

private:
    std::string path_;
};

bool isOpaqueWavAssetId(const std::string& id) {
    if (id.size() != 36 || id.compare(32, 4, ".wav") != 0) return false;
    for (size_t i = 0; i < 32; ++i) {
        const char c = id[i];
        if (!((c >= '0' && c <= '9') ||
              (c >= 'a' && c <= 'f') ||
              (c >= 'A' && c <= 'F'))) {
            return false;
        }
    }
    return true;
}

size_t regularFileCount(const std::string& directory) {
    size_t count = 0;
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(directory, error)) {
        if (entry.is_regular_file(error)) ++count;
    }
    return count;
}

TEST(RackGraphProjectMediaTest, MaterializesMissingWavAssetAndReusesPublishedId) {
    TempMediaDirectory directory;
    ASSERT_FALSE(directory.path().empty());

    RackGraph graph;
    const RackPathId track = graph.getTracks().front().id;
    ASSERT_TRUE(graph.attachTrackWavSlot(
        track, 0,
        makeClip({0.0f, 0.5f, -0.5f, 1.0f}, 48'000,
                 {-1.0f, 0.25f, 0.75f, 0.0f}, "recorded.wav")));
    ASSERT_TRUE(graph.getProjectClipMediaRefs().empty());

    std::string diagnostic = "stale-diagnostic";
    ASSERT_TRUE(graph.materializeProjectMedia(directory.path(), diagnostic));
    EXPECT_TRUE(diagnostic.empty());

    const auto firstRefs = graph.getProjectClipMediaRefs();
    ASSERT_EQ(firstRefs.size(), 1u);
    const auto& [firstTrack, firstSlot, assetId, isMidi] = firstRefs.front();
    EXPECT_EQ(firstTrack, track);
    EXPECT_EQ(firstSlot, 0u);
    EXPECT_FALSE(isMidi);
    ASSERT_TRUE(isOpaqueWavAssetId(assetId));

    const auto materialized = std::filesystem::path(directory.path()) / assetId;
    std::error_code error;
    ASSERT_TRUE(std::filesystem::is_regular_file(materialized, error));
    ASSERT_FALSE(error);
    ASSERT_GT(std::filesystem::file_size(materialized, error), 44u);
    ASSERT_FALSE(error);

    std::vector<float> samples;
    uint32_t sampleRate = 0;
    uint32_t channels = 0;
    ASSERT_TRUE(guitarrackcraft::readWavFile(
        materialized.string(), samples, sampleRate, channels));
    EXPECT_EQ(sampleRate, 48'000u);
    EXPECT_EQ(channels, 2u);
    ASSERT_EQ(samples.size(), 8u);
    constexpr float kPcm16ReadbackTolerance = 1.0f / 32768.0f + 1e-6f;
    EXPECT_NEAR(samples[0], 0.0f, kPcm16ReadbackTolerance);
    EXPECT_NEAR(samples[1], -1.0f, kPcm16ReadbackTolerance);
    EXPECT_NEAR(samples[2], 0.5f, kPcm16ReadbackTolerance);
    EXPECT_NEAR(samples[3], 0.25f, kPcm16ReadbackTolerance);
    EXPECT_EQ(regularFileCount(directory.path()), 1u);

    diagnostic = "stale-diagnostic";
    ASSERT_TRUE(graph.materializeProjectMedia(directory.path(), diagnostic));
    EXPECT_TRUE(diagnostic.empty());
    const auto secondRefs = graph.getProjectClipMediaRefs();
    ASSERT_EQ(secondRefs.size(), 1u);
    EXPECT_EQ(std::get<2>(secondRefs.front()), assetId);
    EXPECT_EQ(regularFileCount(directory.path()), 1u);
}

TEST(RackGraphProjectMediaTest, InvalidMissingWavDoesNotPublishAssetId) {
    TempMediaDirectory directory;
    ASSERT_FALSE(directory.path().empty());

    RackGraph graph;
    const RackPathId track = graph.getTracks().front().id;
    ASSERT_TRUE(graph.attachTrackWavSlot(
        track, 0, makeClip({0.25f, -0.25f}, 48'000, {}, "invalid.wav")));

    std::string diagnostic;
    EXPECT_FALSE(graph.materializeProjectMedia(directory.path(), diagnostic));
    EXPECT_EQ(diagnostic, "invalid-wav-clip");
    EXPECT_TRUE(graph.getProjectClipMediaRefs().empty());
    EXPECT_EQ(regularFileCount(directory.path()), 0u);
}

TEST(RackGraphProjectMediaTest, ExistingWavAssetIdIsNotRematerialized) {
    TempMediaDirectory directory;
    ASSERT_FALSE(directory.path().empty());

    RackGraph graph;
    const RackPathId track = graph.getTracks().front().id;
    ASSERT_TRUE(graph.attachTrackWavSlot(
        track, 0, makeClip({0.25f, -0.25f}, 48'000,
                           {-0.5f, 0.5f}, "existing.wav")));
    ASSERT_TRUE(graph.setTrackClipAssetId(track, 0, false, "existing.wav"));

    std::string diagnostic;
    ASSERT_TRUE(graph.materializeProjectMedia(directory.path(), diagnostic));
    EXPECT_TRUE(diagnostic.empty());
    const auto refs = graph.getProjectClipMediaRefs();
    ASSERT_EQ(refs.size(), 1u);
    EXPECT_EQ(std::get<0>(refs.front()), track);
    EXPECT_EQ(std::get<1>(refs.front()), 0u);
    EXPECT_EQ(std::get<2>(refs.front()), "existing.wav");
    EXPECT_FALSE(std::get<3>(refs.front()));
    EXPECT_EQ(regularFileCount(directory.path()), 0u);
}


} // namespace
