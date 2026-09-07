#include <gtest/gtest.h>
#include "android/native_window.h"

#include "jsfx/IJsfxUiTarget.h"
#include "jsfx/JsfxPlugin.h"
#include "jsfx/JsfxUiHost.h"
#include "jsfx/JsfxPluginFactory.h"
#include "plugin/PluginChain.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#ifndef NNAGA_FIXTURE_ROOT
#error "NNAGA_FIXTURE_ROOT must point at the JSFX contract fixtures"
#endif
#ifndef NNAGA_SMOKE_PATH
#error "NNAGA_SMOKE_PATH must point at the bundled smoke effect"
#endif

// JsfxUiHost is built for Android in the app. The host-side contract target
// supplies the ABI symbols; no test attaches a native window.
extern "C" void ANativeWindow_acquire(ANativeWindow*) {}
extern "C" void ANativeWindow_release(ANativeWindow*) {}
extern "C" int32_t ANativeWindow_setBuffersGeometry(ANativeWindow*, int32_t, int32_t,
                                                      int32_t) {
    return 0;
}
extern "C" int32_t ANativeWindow_lock(ANativeWindow*, ANativeWindow_Buffer*, ARect*) {
    return -1;
}
extern "C" int32_t ANativeWindow_unlockAndPost(ANativeWindow*) {
    return 0;
}

namespace {

using guitarrackcraft::AudioProcessContext;
using guitarrackcraft::IJsfxUiTarget;
using guitarrackcraft::IPlugin;
using guitarrackcraft::JsfxPlugin;
using guitarrackcraft::JsfxPluginFactory;
using guitarrackcraft::MidiBuffer;
using guitarrackcraft::MidiOutputDisposition;
using guitarrackcraft::PluginChain;
using guitarrackcraft::PluginInfo;
using guitarrackcraft::RealtimeClass;

constexpr uint32_t kFrames = 32;

JsfxPluginFactory makeFactory() {
    return JsfxPluginFactory(NNAGA_FIXTURE_ROOT, NNAGA_FIXTURE_ROOT);
}

const PluginInfo* findInfo(const std::vector<PluginInfo>& infos, const std::string& id) {
    const auto it = std::find_if(infos.begin(), infos.end(), [&](const PluginInfo& info) {
        return info.id == id;
    });
    return it == infos.end() ? nullptr : &*it;
}

void expectStereoEquals(const std::array<float, kFrames>& left,
                        const std::array<float, kFrames>& right,
                        const std::array<float, kFrames>& expectedLeft,
                        const std::array<float, kFrames>& expectedRight) {
    for (uint32_t frame = 0; frame < kFrames; ++frame) {
        EXPECT_NEAR(left[frame], expectedLeft[frame], 1e-6f);
        EXPECT_NEAR(right[frame], expectedRight[frame], 1e-6f);
    }
}
template <typename Processor>
MidiOutputDisposition processNoMidi(
        Processor& processor,
        const float* const* inputs,
        float* const* outputs,
        uint32_t numFrames,
        const AudioProcessContext& context) {
    MidiBuffer inputMidi;
    MidiBuffer outputMidi;
    return processor.process(inputs, outputs, numFrames, context, inputMidi,
                             outputMidi);
}

template <typename Processor>
void processChainNoMidi(
        Processor& processor,
        const float* const* inputs,
        float* const* outputs,
        uint32_t numFrames,
        const AudioProcessContext& context) {
    MidiBuffer inputMidi;
    MidiBuffer outputMidi;
    processor.process(inputs, outputs, numFrames, context, inputMidi,
                      outputMidi);
}

} // namespace

TEST(JsfxPluginFactoryContractTest, EnumeratesValidScriptAndExposesSparseControls) {
    auto factory = makeFactory();
    ASSERT_TRUE(factory.initialize());

    const auto infos = factory.enumeratePlugins();
    const PluginInfo* info = findInfo(infos, "SparseSlider.jsfx");
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(info->format, "JSFX");
    EXPECT_EQ(info->name, "Sparse Slider Contract");
    EXPECT_EQ(info->realtimeClass, RealtimeClass::CertifiedInProcess);
    ASSERT_EQ(info->ports.size(), 2u);
    EXPECT_EQ(info->ports[0].index, 0u);
    EXPECT_EQ(info->ports[0].symbol, "slider0");
    EXPECT_EQ(info->ports[0].name, "First control");
    EXPECT_EQ(info->ports[1].index, 2u);
    EXPECT_EQ(info->ports[1].symbol, "slider2");
    EXPECT_EQ(info->ports[1].name, "Third control");

    std::unique_ptr<IPlugin> plugin;
    EXPECT_NO_THROW(plugin = factory.createPlugin("SparseSlider.jsfx"));
    ASSERT_NE(plugin, nullptr);
    auto* wrapper = dynamic_cast<JsfxPlugin*>(plugin.get());
    ASSERT_NE(wrapper, nullptr);
    EXPECT_TRUE(wrapper->loaded());

    std::unique_ptr<IPlugin> malformed;
    EXPECT_NO_THROW(malformed = factory.createPlugin("Malformed.jsfx"));
    EXPECT_EQ(malformed, nullptr);

    std::unique_ptr<IPlugin> missing;
    EXPECT_NO_THROW(missing = factory.createPlugin("does-not-exist.jsfx"));
    EXPECT_EQ(missing, nullptr);

    std::unique_ptr<IPlugin> traversal;
    EXPECT_NO_THROW(traversal = factory.createPlugin("../SparseSlider.jsfx"));
    EXPECT_EQ(traversal, nullptr);
}

TEST(JsfxPluginContractTest, ActivationReadinessAndBoundedProcessArePubliclyObservable) {
    auto factory = makeFactory();
    ASSERT_TRUE(factory.initialize());
    auto plugin = factory.createPlugin("SparseSlider.jsfx");
    ASSERT_NE(plugin, nullptr);
    ASSERT_EQ(plugin->getInfo().realtimeClass, RealtimeClass::CertifiedInProcess);
    EXPECT_FALSE(plugin->isReadyForRealtime());

    plugin->activate(48000.0f, kFrames);
    ASSERT_TRUE(plugin->isReadyForRealtime());

    plugin->setParameter(0, 0.25f);
    plugin->setParameter(2, 0.75f);
    const std::array<float, kFrames> inputLeft = [] {
        std::array<float, kFrames> values{};
        for (uint32_t frame = 0; frame < kFrames; ++frame)
            values[frame] = static_cast<float>(frame) / 31.0f;
        return values;
    }();
    const std::array<float, kFrames> inputRight = [] {
        std::array<float, kFrames> values{};
        for (uint32_t frame = 0; frame < kFrames; ++frame)
            values[frame] = -static_cast<float>(frame) / 31.0f;
        return values;
    }();
    std::array<float, kFrames> outputLeft{};
    std::array<float, kFrames> outputRight{};
    const float* inputs[] = {inputLeft.data(), inputRight.data()};
    float* outputs[] = {outputLeft.data(), outputRight.data()};
    AudioProcessContext context;
    context.sampleRate = 48000.0;

    (void)processNoMidi(*plugin, inputs, outputs, kFrames, context);
    expectStereoEquals(outputLeft, outputRight, inputLeft, inputRight);
    EXPECT_FLOAT_EQ(plugin->getParameter(0), 0.25f);
    EXPECT_FLOAT_EQ(plugin->getParameter(2), 0.75f);

    plugin->setParameter(0, 0.0f);
    plugin->setParameter(2, 0.0f);
    std::array<float, kFrames + 1> oversizedInputLeft;
    std::array<float, kFrames + 1> oversizedInputRight;
    std::array<float, kFrames + 1> oversizedOutputLeft;
    std::array<float, kFrames + 1> oversizedOutputRight;
    for (uint32_t frame = 0; frame <= kFrames; ++frame) {
        oversizedInputLeft[frame] = 0.25f;
        oversizedInputRight[frame] = -0.5f;
        oversizedOutputLeft[frame] = 99.0f;
        oversizedOutputRight[frame] = -99.0f;
    }
    const float* oversizedInputs[] = {oversizedInputLeft.data(), oversizedInputRight.data()};
    float* oversizedOutputs[] = {oversizedOutputLeft.data(), oversizedOutputRight.data()};
    (void)processNoMidi(*plugin, oversizedInputs, oversizedOutputs,
                        kFrames + 1, context);
    for (uint32_t frame = 0; frame <= kFrames; ++frame) {
        EXPECT_FLOAT_EQ(oversizedOutputLeft[frame], oversizedInputLeft[frame]);
        EXPECT_FLOAT_EQ(oversizedOutputRight[frame], oversizedInputRight[frame]);
    }

    outputLeft.fill(123.0f);
    outputRight.fill(-123.0f);
    (void)processNoMidi(*plugin, nullptr, outputs, kFrames, context);
    for (uint32_t frame = 0; frame < kFrames; ++frame) {
        EXPECT_FLOAT_EQ(outputLeft[frame], 0.0f);
        EXPECT_FLOAT_EQ(outputRight[frame], 0.0f);
    }

    (void)processNoMidi(*plugin, inputs, nullptr, kFrames, context);
    (void)processNoMidi(*plugin, inputs, outputs, 0, context);

    plugin->deactivate();
    EXPECT_FALSE(plugin->isReadyForRealtime());
}

TEST(JsfxPluginChainContractTest, ActivatesCertifiedWrapperAndHandlesDryBypassBoundaries) {
    auto factory = makeFactory();
    ASSERT_TRUE(factory.initialize());
    auto plugin = factory.createPlugin("SparseSlider.jsfx");
    ASSERT_NE(plugin, nullptr);
    EXPECT_FALSE(plugin->isReadyForRealtime());

    PluginChain chain;
    ASSERT_EQ(chain.addPlugin(std::move(plugin)), 0);
    const uint64_t instanceId = chain.getPluginInstanceId(0);
    ASSERT_NE(instanceId, 0u);
    chain.setSampleRate(48000.0f, kFrames);
    ASSERT_TRUE(chain.visitPlugin(0, [](const IPlugin& item) {
        return item.isReadyForRealtime();
    }));
    chain.activate();
    EXPECT_TRUE(chain.getRealtimeDiagnostic().empty());

    ASSERT_TRUE(chain.submitParameter(instanceId, 0, 0.25f));
    ASSERT_TRUE(chain.submitParameter(instanceId, 2, 0.75f));
    std::array<float, kFrames> inputLeft{};
    std::array<float, kFrames> inputRight{};
    for (uint32_t frame = 0; frame < kFrames; ++frame) {
        inputLeft[frame] = 0.5f;
        inputRight[frame] = -0.25f;
    }
    std::array<float, kFrames> outputLeft{};
    std::array<float, kFrames> outputRight{};
    const float* inputs[] = {inputLeft.data(), inputRight.data()};
    float* outputs[] = {outputLeft.data(), outputRight.data()};
    AudioProcessContext context;
    context.sampleRate = 48000.0;
    processChainNoMidi(chain, inputs, outputs, kFrames, context);
    expectStereoEquals(outputLeft, outputRight, inputLeft, inputRight);

    std::array<float, kFrames + 1> oversizedLeft;
    std::array<float, kFrames + 1> oversizedRight;
    oversizedLeft.fill(7.0f);
    oversizedRight.fill(-7.0f);
    const float* oversizedInputs[] = {oversizedLeft.data(), oversizedRight.data()};
    float* oversizedOutputs[] = {oversizedLeft.data(), oversizedRight.data()};
    processChainNoMidi(chain, oversizedInputs, oversizedOutputs,
                       kFrames + 1, context);
    for (uint32_t frame = 0; frame <= kFrames; ++frame) {
        EXPECT_FLOAT_EQ(oversizedLeft[frame], 0.0f);
        EXPECT_FLOAT_EQ(oversizedRight[frame], 0.0f);
    }

    outputLeft.fill(3.0f);
    outputRight.fill(-3.0f);
    processChainNoMidi(chain, nullptr, outputs, kFrames, context);
    for (uint32_t frame = 0; frame < kFrames; ++frame) {
        EXPECT_FLOAT_EQ(outputLeft[frame], 0.0f);
        EXPECT_FLOAT_EQ(outputRight[frame], 0.0f);
    }
    processChainNoMidi(chain, inputs, nullptr, kFrames, context);

}

TEST(JsfxGfxGateContractTest, DSPPassesThroughWhilePublicUiGateIsHeld) {
    auto factory = JsfxPluginFactory(
        std::filesystem::path(NNAGA_SMOKE_PATH).parent_path().parent_path().string(),
        NNAGA_FIXTURE_ROOT);
    ASSERT_TRUE(factory.initialize());
    auto plugin = factory.createPlugin("NNAGA/NNAGA_Smoke.jsfx");
    ASSERT_NE(plugin, nullptr);
    auto* target = dynamic_cast<IJsfxUiTarget*>(plugin.get());
    ASSERT_NE(target, nullptr);
    ASSERT_TRUE(target->hasJsfxGfx());
    ASSERT_NE(target->jsfxUiHost(), nullptr);

    plugin->activate(48000.0f, kFrames);
    ASSERT_TRUE(plugin->isReadyForRealtime());
    const std::array<float, kFrames> inputLeft = [] {
        std::array<float, kFrames> values{};
        values.fill(0.4f);
        return values;
    }();
    const std::array<float, kFrames> inputRight = [] {
        std::array<float, kFrames> values{};
        values.fill(-0.2f);
        return values;
    }();
    std::array<float, kFrames> outputLeft{};
    std::array<float, kFrames> outputRight{};
    const float* inputs[] = {inputLeft.data(), inputRight.data()};
    float* outputs[] = {outputLeft.data(), outputRight.data()};
    AudioProcessContext context;
    context.sampleRate = 48000.0;

    target->jsfxUiHost()->pauseEffect();
    ASSERT_TRUE(target->jsfxUiHost()->tryAcquireEffect());
    (void)processNoMidi(*plugin, inputs, outputs, kFrames, context);
    target->jsfxUiHost()->releaseEffect();
    target->jsfxUiHost()->resumeEffect();
    expectStereoEquals(outputLeft, outputRight, inputLeft, inputRight);
}

TEST(JsfxMidiContractTest, EchoesShortAndMaximumCompleteSysExByteExactly) {
    auto factory = makeFactory();
    ASSERT_TRUE(factory.initialize());
    auto plugin = factory.createPlugin("MidiEcho.jsfx");
    ASSERT_NE(plugin, nullptr);
    plugin->activate(48000.0f, 64);
    ASSERT_TRUE(plugin->isReadyForRealtime());

    MidiBuffer input;
    const std::array<uint8_t, 3> note = {0x90, 60, 100};
    const std::array<uint8_t, 5> smallSysex = {0xf0, 1, 2, 3, 0xf7};
    ASSERT_TRUE(input.append(3, note.data(), note.size()));
    ASSERT_TRUE(input.append(9, smallSysex.data(), smallSysex.size()));
    MidiBuffer output;
    std::array<float, kFrames> inputLeft{};
    std::array<float, kFrames> inputRight{};
    std::array<float, kFrames> outputLeft{};
    std::array<float, kFrames> outputRight{};
    const float* inputs[] = {inputLeft.data(), inputRight.data()};
    float* outputs[] = {outputLeft.data(), outputRight.data()};
    ASSERT_EQ(plugin->process(inputs, outputs, kFrames, AudioProcessContext{},
                              input, output),
              MidiOutputDisposition::Replace);
    ASSERT_EQ(output.eventCount(), 2u);
    for (uint32_t i = 0; i < output.eventCount(); ++i) {
        const auto& actual = output.eventAt(i);
        const auto& expected = input.eventAt(i);
        EXPECT_EQ(actual.frameOffset, expected.frameOffset);
        EXPECT_EQ(actual.payloadSize, expected.payloadSize);
        EXPECT_TRUE(std::equal(
            input.payloadFor(expected),
            input.payloadFor(expected) + expected.payloadSize,
            output.payloadFor(actual)));
    }

    std::vector<uint8_t> sysex(guitarrackcraft::kMaxMidiPayloadBytes);
    sysex.front() = 0xf0;
    for (uint32_t i = 1; i + 1 < sysex.size(); ++i)
        sysex[i] = static_cast<uint8_t>((i * 37u) % 127u);
    sysex.back() = 0xf7;
    input.clear();
    output.clear();
    ASSERT_TRUE(input.append(17, sysex.data(), sysex.size()));
    ASSERT_EQ(plugin->process(inputs, outputs, kFrames, AudioProcessContext{},
                              input, output),
              MidiOutputDisposition::Replace);
    ASSERT_EQ(output.eventCount(), 1u);
    const auto& actual = output.eventAt(0);
    EXPECT_EQ(actual.frameOffset, 17u);
    ASSERT_EQ(actual.payloadSize, sysex.size());
    EXPECT_TRUE(std::equal(sysex.begin(), sysex.end(),
                           output.payloadFor(actual)));
    plugin->deactivate();
}

TEST(JsfxMidiContractTest, GeneratedSysExIsAuthoritativeAtItsInQuantumFrame) {
    auto factory = makeFactory();
    ASSERT_TRUE(factory.initialize());
    auto plugin = factory.createPlugin("MidiGenerate.jsfx");
    ASSERT_NE(plugin, nullptr);
    plugin->activate(48000.0f, 64);
    ASSERT_TRUE(plugin->isReadyForRealtime());

    MidiBuffer input;
    MidiBuffer output;
    std::array<float, kFrames> inputLeft{};
    std::array<float, kFrames> inputRight{};
    std::array<float, kFrames> outputLeft{};
    std::array<float, kFrames> outputRight{};
    const float* inputs[] = {inputLeft.data(), inputRight.data()};
    float* outputs[] = {outputLeft.data(), outputRight.data()};
    const auto disposition = plugin->process(
        inputs, outputs, kFrames, AudioProcessContext{}, input, output);
    ASSERT_EQ(disposition, MidiOutputDisposition::Replace);
    ASSERT_EQ(output.eventCount(), 1u);
    const auto& event = output.eventAt(0);
    const std::array<uint8_t, 5> expected = {0xf0, 0x01, 0x7d, 0x55, 0xf7};
    EXPECT_EQ(event.frameOffset, 9u);
    ASSERT_EQ(event.payloadSize, expected.size());
    EXPECT_TRUE(std::equal(expected.begin(), expected.end(),
                           output.payloadFor(event)));
    plugin->deactivate();
}

TEST(JsfxMidiContractTest, EmptyEmissionUsesPassthroughAndPreservesNoOutput) {
    auto factory = makeFactory();
    ASSERT_TRUE(factory.initialize());
    auto plugin = factory.createPlugin("MidiEcho.jsfx");
    ASSERT_NE(plugin, nullptr);
    plugin->activate(48000.0f, kFrames);
    ASSERT_TRUE(plugin->isReadyForRealtime());
    MidiBuffer input;
    MidiBuffer output;
    std::array<float, kFrames> inputLeft{};
    std::array<float, kFrames> inputRight{};
    std::array<float, kFrames> outputLeft{};
    std::array<float, kFrames> outputRight{};
    const float* inputs[] = {inputLeft.data(), inputRight.data()};
    float* outputs[] = {outputLeft.data(), outputRight.data()};
    EXPECT_EQ(plugin->process(inputs, outputs, kFrames, AudioProcessContext{},
                              input, output),
              MidiOutputDisposition::Passthrough);
    EXPECT_EQ(output.eventCount(), 0u);
    plugin->deactivate();
}

TEST(JsfxMidiContractTest, EmittedPayloadOverCapacityIsDroppedAsOneMessage) {
    auto factory = makeFactory();
    ASSERT_TRUE(factory.initialize());
    auto plugin = factory.createPlugin("MidiOversize.jsfx");
    ASSERT_NE(plugin, nullptr);
    plugin->activate(48000.0f, kFrames);
    ASSERT_TRUE(plugin->isReadyForRealtime());

    MidiBuffer input;
    MidiBuffer output;
    std::array<float, kFrames> inputLeft{};
    std::array<float, kFrames> inputRight{};
    std::array<float, kFrames> outputLeft{};
    std::array<float, kFrames> outputRight{};
    const float* inputs[] = {inputLeft.data(), inputRight.data()};
    float* outputs[] = {outputLeft.data(), outputRight.data()};
    EXPECT_EQ(plugin->process(inputs, outputs, kFrames,
                              AudioProcessContext{}, input, output),
              MidiOutputDisposition::Replace);
    ASSERT_EQ(output.eventCount(), 1u);
    const auto& event = output.eventAt(0);
    ASSERT_EQ(event.payloadSize, 65530u);
    EXPECT_EQ(output.payloadFor(event)[0], 0xf0u);
    EXPECT_EQ(output.payloadFor(event)[event.payloadSize - 1], 0xf7u);
    plugin->deactivate();
}
