#include <gtest/gtest.h>

#include "plugin/PluginChain.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace {

using guitarrackcraft::AudioProcessContext;
using guitarrackcraft::IPlugin;
using guitarrackcraft::MidiEvent;
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

    uint32_t process(const float* const*, float* const*, uint32_t,
                     const AudioProcessContext&, const MidiEvent*, uint32_t,
                     MidiEvent*, uint32_t) override {
        return 0;
    }

    PluginInfo getInfo() const override { return {}; }
    void setParameter(uint32_t, float) override {}
    float getParameter(uint32_t) const override { return 0.0f; }
    uint32_t getNumInputPorts() const override { return 0; }
    uint32_t getNumOutputPorts() const override { return 0; }

private:
    std::shared_ptr<LifecycleState> state_;
};

} // namespace

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
