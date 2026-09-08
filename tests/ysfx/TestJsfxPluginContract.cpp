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
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>
#include <unistd.h>

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
// Slider values and @slider are applied on JsfxPlugin's worker thread now, so
// a parameter set is observable only after that thread has run. Process blocks
// until the expected gain shows up, or give up.
template <typename Processor, typename Predicate>
bool processUntil(Processor& processor, const float* const* inputs,
                  float* const* outputs, uint32_t numFrames,
                  const AudioProcessContext& context, Predicate&& ready) {
    for (int attempt = 0; attempt < 400; ++attempt) {
        MidiBuffer in;
        MidiBuffer out;
        processor.process(inputs, outputs, numFrames, context, in, out);
        if (ready()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
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

TEST(JsfxPluginContractTest, SliderChangesApplyAcrossEveryChangeMaskWord) {
    auto factory = makeFactory();
    ASSERT_TRUE(factory.initialize());
    auto plugin = factory.createPlugin("SliderGroups.jsfx");
    ASSERT_NE(plugin, nullptr);
    plugin->activate(48000.0f, kFrames);
    ASSERT_TRUE(plugin->isReadyForRealtime());

    std::array<float, kFrames> inputLeft{};
    std::array<float, kFrames> inputRight{};
    inputLeft.fill(1.0f);
    inputRight.fill(-1.0f);
    std::array<float, kFrames> outputLeft{};
    std::array<float, kFrames> outputRight{};
    const float* inputs[] = {inputLeft.data(), inputRight.data()};
    float* outputs[] = {outputLeft.data(), outputRight.data()};
    AudioProcessContext context;
    context.sampleRate = 48000.0;

    // One slider per change-mask word, hitting bit 0 and bit 63 of a word.
    // gain = 1*1 + 1*2 + 1*4 + 1*8 = 15
    plugin->setParameter(0, 1.0f);
    plugin->setParameter(63, 1.0f);
    plugin->setParameter(64, 1.0f);
    plugin->setParameter(255, 1.0f);
    ASSERT_TRUE(processUntil(*plugin, inputs, outputs, kFrames, context,
                             [&] { return std::abs(outputLeft[0] - 15.0f) < 1e-6f; }))
        << "all four change-mask words must reach the script";
    for (uint32_t frame = 0; frame < kFrames; ++frame) {
        EXPECT_NEAR(outputLeft[frame], 15.0f, 1e-6f);
        EXPECT_NEAR(outputRight[frame], -15.0f, 1e-6f);
    }

    // A block with no parameter change must keep the values already applied,
    // i.e. clearing the mask must not clear the slider state.
    outputLeft.fill(0.0f);
    outputRight.fill(0.0f);
    (void)processNoMidi(*plugin, inputs, outputs, kFrames, context);
    for (uint32_t frame = 0; frame < kFrames; ++frame) {
        EXPECT_NEAR(outputLeft[frame], 15.0f, 1e-6f);
        EXPECT_NEAR(outputRight[frame], -15.0f, 1e-6f);
    }

    // Changing one slider in the last word must not disturb the other words.
    // gain = 1 + 2 + 4 + 0 = 7
    plugin->setParameter(255, 0.0f);
    ASSERT_TRUE(processUntil(*plugin, inputs, outputs, kFrames, context,
                             [&] { return std::abs(outputLeft[0] - 7.0f) < 1e-6f; }))
        << "a change in the last mask word must not disturb the others";
    for (uint32_t frame = 0; frame < kFrames; ++frame) {
        EXPECT_NEAR(outputLeft[frame], 7.0f, 1e-6f);
        EXPECT_NEAR(outputRight[frame], -7.0f, 1e-6f);
    }
    EXPECT_FLOAT_EQ(plugin->getParameter(0), 1.0f);
    EXPECT_FLOAT_EQ(plugin->getParameter(255), 0.0f);

    // Out-of-range indices are ignored rather than corrupting a mask word.
    plugin->setParameter(256, 1.0f);
    plugin->setParameter(4096, 1.0f);
    for (int i = 0; i < 20; ++i) {
        (void)processNoMidi(*plugin, inputs, outputs, kFrames, context);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    for (uint32_t frame = 0; frame < kFrames; ++frame)
        EXPECT_NEAR(outputLeft[frame], 7.0f, 1e-6f);

    plugin->deactivate();
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

// activate() must commit script RAM up front, so EEL never callocs or takes a
// first-touch page fault from inside @sample or @gfx.
TEST(JsfxPluginContractTest, ActivateCommitsScriptRamUpFront) {
    auto readRssKb = [] {
        long rssPages = 0;
        FILE* f = std::fopen("/proc/self/statm", "r");
        if (!f) return 0L;
        long total = 0;
        if (std::fscanf(f, "%ld %ld", &total, &rssPages) != 2) rssPages = 0;
        std::fclose(f);
        return rssPages * (sysconf(_SC_PAGESIZE) / 1024);
    };
    auto factory = makeFactory();
    ASSERT_TRUE(factory.initialize());
    auto plugin = factory.createPlugin("SparseSlider.jsfx");
    ASSERT_NE(plugin, nullptr);

    const long before = readRssKb();
    plugin->activate(48000.0f, kFrames);
    const long after = readRssKb();
    ASSERT_TRUE(plugin->isReadyForRealtime());

    // kJsfxPreallocItems is 256Ki doubles = 2 MiB. Allow slack for allocator
    // and unrelated activity, but require most of it to be resident.
    const long grewKb = after - before;
    printf("[prealloc] RSS grew %ld KiB across activate()\n", grewKb);
    EXPECT_GE(grewKb, 1536);
    plugin->deactivate();
}

// A JSFX rewriting pdc_delay every block must not surface as a per-block
// latency change: RackGraph restarts delay compensation on every change and
// emits silence on the other tracks while it refills, so an oscillating value
// would hold the rack silent for as long as the script kept moving.
TEST(JsfxPluginContractTest, OscillatingScriptPdcDoesNotJitterReportedLatency) {
    auto factory = makeFactory();
    ASSERT_TRUE(factory.initialize());
    auto plugin = factory.createPlugin("JitterPdc.jsfx");
    ASSERT_NE(plugin, nullptr);
    plugin->activate(48000.0f, kFrames);
    ASSERT_TRUE(plugin->isReadyForRealtime());

    std::array<float, kFrames> inputLeft{};
    std::array<float, kFrames> inputRight{};
    inputLeft.fill(0.25f);
    inputRight.fill(-0.25f);
    std::array<float, kFrames> outputLeft{};
    std::array<float, kFrames> outputRight{};
    const float* inputs[] = {inputLeft.data(), inputRight.data()};
    float* outputs[] = {outputLeft.data(), outputRight.data()};
    AudioProcessContext context;
    context.sampleRate = 48000.0;

    auto runAndCountChanges = [&](int blocks) {
        uint32_t previous = plugin->getLatencyFrames();
        int changes = 0;
        for (int i = 0; i < blocks; ++i) {
            (void)processNoMidi(*plugin, inputs, outputs, kFrames, context);
            const uint32_t now = plugin->getLatencyFrames();
            if (now != previous) ++changes;
            previous = now;
        }
        return changes;
    };

    // Steady pdc_delay: settles to the script's value and stays there.
    (void)runAndCountChanges(64);
    EXPECT_EQ(plugin->getLatencyFrames(), 128u);

    // Now make the script oscillate pdc_delay every single block.
    plugin->setParameter(0, 1.0f);
    const int changes = runAndCountChanges(200);
    printf("[pdc] reported-latency changes over 200 oscillating blocks: %d\n", changes);
    EXPECT_EQ(changes, 0);
    EXPECT_EQ(plugin->getLatencyFrames(), 128u);

    // A genuine, stable change must still be honoured.
    plugin->setParameter(0, 0.0f);
    (void)runAndCountChanges(64);
    EXPECT_EQ(plugin->getLatencyFrames(), 128u);
    plugin->deactivate();
}

// Measurement: @slider runs on the audio thread inside ysfx_process_float, so a
// script whose @slider rebuilds tables costs that time in the audio callback on
// every block that carries a parameter change -- i.e. continuously while the
// user drags a slider.
TEST(JsfxPluginContractTest, HeavySliderCostStaysOffTheAudioThread) {
    auto factory = makeFactory();
    ASSERT_TRUE(factory.initialize());
    auto plugin = factory.createPlugin("HeavySlider.jsfx");
    ASSERT_NE(plugin, nullptr);
    plugin->activate(48000.0f, kFrames);
    ASSERT_TRUE(plugin->isReadyForRealtime());

    std::array<float, kFrames> inputLeft{};
    std::array<float, kFrames> inputRight{};
    inputLeft.fill(0.1f);
    inputRight.fill(0.1f);
    std::array<float, kFrames> outputLeft{};
    std::array<float, kFrames> outputRight{};
    const float* inputs[] = {inputLeft.data(), inputRight.data()};
    float* outputs[] = {outputLeft.data(), outputRight.data()};
    AudioProcessContext context;
    context.sampleRate = 48000.0;

    auto timeBlock = [&] {
        const auto start = std::chrono::steady_clock::now();
        (void)processNoMidi(*plugin, inputs, outputs, kFrames, context);
        return std::chrono::duration<double, std::micro>(
                   std::chrono::steady_clock::now() - start).count();
    };

    for (int i = 0; i < 50; ++i) (void)timeBlock();
    double steady = 1e9;
    for (int i = 0; i < 50; ++i) steady = std::min(steady, timeBlock());

    double withChange = 1e9;
    for (int i = 0; i < 20; ++i) {
        plugin->setParameter(0, 0.1f + 0.01f * static_cast<float>(i));
        withChange = std::min(withChange, timeBlock());
    }
    // 32 frames @ 48 kHz = 667 us of budget.
    const double budget = 1e6 * kFrames / 48000.0;
    printf("[@slider] steady block %.1f us, block carrying a slider change %.1f us"
           " (budget %.0f us for %u frames)\n", steady, withChange, budget, kFrames);
    // Before @slider was moved to the worker thread this measured 10580 us
    // against a 667 us budget. No audio block may carry that work.
    EXPECT_LT(withChange, budget);
    plugin->deactivate();
}

TEST(JsfxGfxGateContractTest, DSPKeepsProcessingWhilePublicUiGateIsHeld) {
    auto factory = makeFactory();
    ASSERT_TRUE(factory.initialize());
    // Gain of 2, so a processed block and a passed-through block are distinct.
    auto plugin = factory.createPlugin("GfxContention.jsfx");
    ASSERT_NE(plugin, nullptr);
    auto* target = dynamic_cast<IJsfxUiTarget*>(plugin.get());
    ASSERT_NE(target, nullptr);
    ASSERT_TRUE(target->hasJsfxGfx());
    ASSERT_NE(target->jsfxUiHost(), nullptr);

    plugin->activate(48000.0f, kFrames);
    ASSERT_TRUE(plugin->isReadyForRealtime());
    std::array<float, kFrames> inputLeft{};
    std::array<float, kFrames> inputRight{};
    inputLeft.fill(0.4f);
    inputRight.fill(-0.2f);
    std::array<float, kFrames> outputLeft{};
    std::array<float, kFrames> outputRight{};
    const float* inputs[] = {inputLeft.data(), inputRight.data()};
    float* outputs[] = {outputLeft.data(), outputRight.data()};
    AudioProcessContext context;
    context.sampleRate = 48000.0;

    // Holding the UI gate must not cost the audio thread its block. The gate
    // now coordinates the gfx thread only; audio owns the VM unconditionally.
    target->jsfxUiHost()->pauseEffect();
    ASSERT_TRUE(target->jsfxUiHost()->tryAcquireEffect());
    (void)processNoMidi(*plugin, inputs, outputs, kFrames, context);
    target->jsfxUiHost()->releaseEffect();
    target->jsfxUiHost()->resumeEffect();
    for (uint32_t frame = 0; frame < kFrames; ++frame) {
        EXPECT_NEAR(outputLeft[frame], inputLeft[frame] * 2.0f, 1e-6f);
        EXPECT_NEAR(outputRight[frame], inputRight[frame] * 2.0f, 1e-6f);
    }
    plugin->deactivate();
}

// The audio thread must never lose a block to UI activity. Before the gate was
// removed from the audio path this measured 0.23% of blocks lost with the
// editor closed and 8.90% with it open at 30 fps.
TEST(JsfxGfxGateContractTest, AudioLosesNoBlocksToUiContention) {
    auto factory = makeFactory();
    ASSERT_TRUE(factory.initialize());
    auto plugin = factory.createPlugin("GfxContention.jsfx");
    ASSERT_NE(plugin, nullptr);
    auto* target = dynamic_cast<IJsfxUiTarget*>(plugin.get());
    ASSERT_NE(target, nullptr);
    ASSERT_TRUE(target->hasJsfxGfx());
    plugin->activate(48000.0f, kFrames);
    ASSERT_TRUE(plugin->isReadyForRealtime());

    std::array<float, kFrames> inputLeft{};
    std::array<float, kFrames> inputRight{};
    inputLeft.fill(0.25f);
    inputRight.fill(-0.25f);
    std::array<float, kFrames> outputLeft{};
    std::array<float, kFrames> outputRight{};
    const float* inputs[] = {inputLeft.data(), inputRight.data()};
    float* outputs[] = {outputLeft.data(), outputRight.data()};
    AudioProcessContext context;
    context.sampleRate = 48000.0;

    auto runBlocks = [&](const char* label) {
        // 32 frames @ 48 kHz = 667 us per block; pace roughly to real time.
        const int blocks = 1500;
        int lost = 0;
        for (int b = 0; b < blocks; ++b) {
            outputLeft.fill(0.0f);
            (void)processNoMidi(*plugin, inputs, outputs, kFrames, context);
            // gain is 2, so an unprocessed (passthrough) block shows the input.
            if (std::abs(outputLeft[0] - inputLeft[0]) < 1e-6f) ++lost;
            std::this_thread::sleep_for(std::chrono::microseconds(667));
        }
        printf("[gfx-gate] %-22s %4d / %d blocks lost to UI contention (%.2f%%)\n",
               label, lost, blocks, 100.0 * lost / blocks);
        return lost;
    };

    const int lostClosed = runBlocks("UI closed");
    target->jsfxUiHost()->resize(400, 200, 1.0f);
    target->jsfxUiHost()->setVisible(true);
    const int lostOpen = runBlocks("UI visible @30fps");
    target->jsfxUiHost()->setVisible(false);

    EXPECT_EQ(lostClosed, 0);
    EXPECT_EQ(lostOpen, 0);
    plugin->deactivate();
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
