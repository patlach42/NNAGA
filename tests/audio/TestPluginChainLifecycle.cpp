#include <gtest/gtest.h>

#include "plugin/PluginChain.h"

#include <array>
#include <algorithm>
#include <cstdint>
#include <memory>
#include <vector>

namespace {

using guitarrackcraft::AudioProcessContext;
using guitarrackcraft::IPlugin;
using guitarrackcraft::MidiBuffer;
using guitarrackcraft::MidiOutputDisposition;
using guitarrackcraft::PluginChain;
using guitarrackcraft::PluginInfo;

struct Activation {
    float sampleRate;
    uint32_t bufferSize;
};

enum class LifecycleEvent {
    Prepare,
    Activate,
};

struct LifecycleState {
    uint32_t prepareCount = 0;
    std::vector<Activation> activations;
    std::vector<LifecycleEvent> events;
};

class LifecyclePlugin final : public IPlugin {
public:
    explicit LifecyclePlugin(std::shared_ptr<LifecycleState> state)
        : state_(std::move(state)) {}

    void prepare() override {
        ++state_->prepareCount;
        state_->events.push_back(LifecycleEvent::Prepare);
    }

    void activate(float sampleRate, uint32_t bufferSize) override {
        state_->activations.push_back({sampleRate, bufferSize});
        state_->events.push_back(LifecycleEvent::Activate);
    }

    void deactivate() override {}

    MidiOutputDisposition process(const float* const*, float* const*,
                                  uint32_t, const AudioProcessContext&,
                                  const MidiBuffer&, MidiBuffer&) override {
        return MidiOutputDisposition::Passthrough;
    }

    PluginInfo getInfo() const override {
        PluginInfo info;
        info.realtimeClass = guitarrackcraft::RealtimeClass::CertifiedInProcess;
        return info;
    }
    void setParameter(uint32_t, float) override {}
    float getParameter(uint32_t) const override { return 0.0f; }
    uint32_t getNumInputPorts() const override { return 0; }
    uint32_t getNumOutputPorts() const override { return 0; }

private:
    std::shared_ptr<LifecycleState> state_;
};

class MidiDispositionPlugin final : public IPlugin {
public:
    enum class Mode { Passthrough, ReplaceEmpty, Echo, InvalidFrame };

    explicit MidiDispositionPlugin(Mode mode) : mode_(mode) {}

    void activate(float, uint32_t) override {}
    void deactivate() override {}

    MidiOutputDisposition process(const float* const*,
                                  float* const*,
                                  uint32_t numFrames,
                                  const AudioProcessContext&,
                                  const MidiBuffer& inputMidi,
                                  MidiBuffer& outputMidi) override {
        if (mode_ == Mode::Echo) {
            EXPECT_TRUE(outputMidi.copyFrom(inputMidi));
        } else if (mode_ == Mode::InvalidFrame) {
            const uint8_t byte = 0x90;
            EXPECT_TRUE(outputMidi.append(numFrames, &byte, 1));
        }
        return mode_ == Mode::Passthrough
                       ? MidiOutputDisposition::Passthrough
                       : MidiOutputDisposition::Replace;
    }

    PluginInfo getInfo() const override {
        PluginInfo info;
        info.realtimeClass = guitarrackcraft::RealtimeClass::CertifiedInProcess;
        return info;
    }
    void setParameter(uint32_t, float) override {}
    float getParameter(uint32_t) const override { return 0.0f; }
    uint32_t getNumInputPorts() const override { return 0; }
    uint32_t getNumOutputPorts() const override { return 0; }

private:
    Mode mode_;
};


} // namespace

TEST(PluginChainMidiContractTest, PreservesBytesOrderAndFramesThroughTwoDevices) {
    PluginChain chain;
    ASSERT_EQ(chain.addPlugin(
                  std::make_unique<MidiDispositionPlugin>(
                      MidiDispositionPlugin::Mode::Echo)),
              0);
    ASSERT_EQ(chain.addPlugin(
                  std::make_unique<MidiDispositionPlugin>(
                      MidiDispositionPlugin::Mode::Echo)),
              1);
    chain.setSampleRate(48'000.0f, 64);
    chain.activate();

    std::array<float, 64> inputAudio{};
    std::array<float, 64> outputAudio{};
    const float* inputs[] = {inputAudio.data(), inputAudio.data()};
    float* outputs[] = {outputAudio.data(), outputAudio.data()};

    MidiBuffer inputMidi;
    const std::array<uint8_t, 3> note = {0x90, 0x3c, 0x64};
    const std::array<uint8_t, 3> cc = {0xb0, 7, 127};
    ASSERT_TRUE(inputMidi.append(7, note.data(), note.size()));
    ASSERT_TRUE(inputMidi.append(7, cc.data(), cc.size()));

    MidiBuffer outputMidi;
    chain.process(inputs, outputs, 64, AudioProcessContext{}, inputMidi,
                  outputMidi);
    ASSERT_EQ(outputMidi.eventCount(), 2u);
    EXPECT_EQ(outputMidi.eventAt(0).frameOffset, 7u);
    EXPECT_EQ(outputMidi.eventAt(1).frameOffset, 7u);
    EXPECT_TRUE(std::equal(note.begin(), note.end(),
                           outputMidi.payloadFor(outputMidi.eventAt(0))));
    EXPECT_TRUE(std::equal(cc.begin(), cc.end(),
                           outputMidi.payloadFor(outputMidi.eventAt(1))));

    inputMidi.clear();
    std::vector<uint8_t> sysex(guitarrackcraft::kMaxMidiPayloadBytes);
    sysex.front() = 0xf0;
    for (size_t index = 1; index + 1 < sysex.size(); ++index)
        sysex[index] = static_cast<uint8_t>(index * 17u);
    sysex.back() = 0xf7;
    ASSERT_TRUE(inputMidi.append(31, sysex.data(), sysex.size()));
    chain.process(inputs, outputs, 64, AudioProcessContext{}, inputMidi,
                  outputMidi);

    ASSERT_EQ(outputMidi.eventCount(), 1u);
    EXPECT_EQ(outputMidi.payloadBytes(), sysex.size());
    EXPECT_EQ(outputMidi.eventAt(0).frameOffset, 31u);
    EXPECT_EQ(outputMidi.eventAt(0).payloadSize, sysex.size());
    EXPECT_TRUE(std::equal(sysex.begin(), sysex.end(),
                           outputMidi.payloadFor(outputMidi.eventAt(0))));
}

TEST(PluginChainMidiContractTest, ReplaceEmptyConsumesInput) {
    PluginChain chain;
    ASSERT_EQ(chain.addPlugin(std::make_unique<MidiDispositionPlugin>(
                                  MidiDispositionPlugin::Mode::ReplaceEmpty)),
              0);
    chain.setSampleRate(48'000.0f, 16);
    chain.activate();

    std::array<float, 16> audio{};
    const float* inputs[] = {audio.data(), audio.data()};
    float* outputs[] = {audio.data(), audio.data()};
    const std::array<uint8_t, 3> message = {0x80, 0x3c, 0};
    MidiBuffer inputMidi;
    ASSERT_TRUE(inputMidi.append(0, message.data(), message.size()));
    MidiBuffer outputMidi;
    chain.process(inputs, outputs, 16, AudioProcessContext{}, inputMidi,
                  outputMidi);
    EXPECT_EQ(outputMidi.eventCount(), 0u);
    EXPECT_EQ(chain.getMidiPluginOutputDrops(), 0u);
}

TEST(PluginChainMidiContractTest, PassthroughRetainsInput) {
    PluginChain chain;
    ASSERT_EQ(chain.addPlugin(std::make_unique<MidiDispositionPlugin>(
                                  MidiDispositionPlugin::Mode::Passthrough)),
              0);
    chain.setSampleRate(48'000.0f, 16);
    chain.activate();

    std::array<float, 16> audio{};
    const float* inputs[] = {audio.data(), audio.data()};
    float* outputs[] = {audio.data(), audio.data()};
    const std::array<uint8_t, 3> message = {0xb0, 1, 127};
    MidiBuffer inputMidi;
    ASSERT_TRUE(inputMidi.append(9, message.data(), message.size()));
    MidiBuffer outputMidi;
    chain.process(inputs, outputs, 16, AudioProcessContext{}, inputMidi,
                  outputMidi);
    ASSERT_EQ(outputMidi.eventCount(), 1u);
    EXPECT_EQ(outputMidi.eventAt(0).frameOffset, 9u);
    EXPECT_TRUE(std::equal(message.begin(), message.end(),
                           outputMidi.payloadFor(outputMidi.eventAt(0))));
}

TEST(PluginChainMidiContractTest, RejectsWholeMessagesAtEventAndPayloadBounds) {
    MidiBuffer events;
    const uint8_t byte = 0x90;
    for (uint32_t index = 0; index < guitarrackcraft::kMaxMidiEvents; ++index)
        ASSERT_TRUE(events.append(index, &byte, 1));
    EXPECT_FALSE(events.append(0, &byte, 1));
    EXPECT_EQ(events.eventCount(), guitarrackcraft::kMaxMidiEvents);
    EXPECT_EQ(events.payloadBytes(), guitarrackcraft::kMaxMidiEvents);
    EXPECT_EQ(events.rejectedMessages(), 1u);

    MidiBuffer payload;
    std::vector<uint8_t> maxPayload(guitarrackcraft::kMaxMidiPayloadBytes,
                                    0x7f);
    ASSERT_TRUE(payload.append(0, maxPayload.data(), maxPayload.size()));
    EXPECT_FALSE(payload.append(1, &byte, 1));
    EXPECT_EQ(payload.eventCount(), 1u);
    EXPECT_EQ(payload.payloadBytes(), guitarrackcraft::kMaxMidiPayloadBytes);
    EXPECT_EQ(payload.rejectedMessages(), 1u);
}

TEST(PluginChainMidiContractTest, RejectsInvalidPluginOutputAndCountsDrops) {
    PluginChain chain;
    ASSERT_EQ(chain.addPlugin(std::make_unique<MidiDispositionPlugin>(
                                  MidiDispositionPlugin::Mode::InvalidFrame)),
              0);
    chain.setSampleRate(48'000.0f, 16);
    chain.activate();

    std::array<float, 16> audio{};
    const float* inputs[] = {audio.data(), audio.data()};
    float* outputs[] = {audio.data(), audio.data()};
    MidiBuffer outputMidi;
    MidiBuffer inputMidi;
    chain.process(inputs, outputs, 16, AudioProcessContext{}, inputMidi,
                  outputMidi);
    EXPECT_EQ(outputMidi.eventCount(), 0u);
    EXPECT_EQ(chain.getMidiPluginOutputDrops(), 1u);
}

TEST(PluginChainLifecycleTest,
     PreparesOnAddBeforeSampleRateAndActivatesOnceAfterNegotiation) {
    auto state = std::make_shared<LifecycleState>();
    PluginChain chain;

    ASSERT_EQ(chain.addPlugin(std::make_unique<LifecyclePlugin>(state)), 0);
    EXPECT_EQ(state->prepareCount, 1u);
    EXPECT_TRUE(state->activations.empty());
    ASSERT_EQ(state->events.size(), 1u);
    EXPECT_EQ(state->events[0], LifecycleEvent::Prepare);

    constexpr float sampleRate = 48000.0f;
    constexpr uint32_t bufferSize = 256;
    chain.setSampleRate(sampleRate, bufferSize);

    EXPECT_EQ(state->prepareCount, 1u);
    ASSERT_EQ(state->activations.size(), 1u);
    EXPECT_FLOAT_EQ(state->activations[0].sampleRate, sampleRate);
    EXPECT_EQ(state->activations[0].bufferSize, bufferSize);
    ASSERT_EQ(state->events.size(), 2u);
    EXPECT_EQ(state->events[1], LifecycleEvent::Activate);
}
