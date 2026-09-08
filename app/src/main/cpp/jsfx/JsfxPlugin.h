/* Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * This file is part of NNAGA.
 * NNAGA is free software under the GNU General Public License version 3.
 */
#pragma once

#include "../plugin/IPlugin.h"
#include "IJsfxUiTarget.h"
#include <ysfx.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <string>
#include <vector>

namespace guitarrackcraft {

class JsfxPlugin final : public IPlugin, public IJsfxUiTarget {
public:
    JsfxPlugin(std::shared_ptr<ysfx_config_t> config, std::string path, std::string id);
    ~JsfxPlugin() override;
    bool loaded() const noexcept { return fx_ != nullptr; }
    void prepare() override {}
    void activate(float sampleRate, uint32_t bufferSize = 0) override;
    bool isReadyForRealtime() const noexcept override { return ready_.load(std::memory_order_acquire); }
    void deactivate() override;
    MidiOutputDisposition process(const float* const* inputs, float* const* outputs, uint32_t numFrames,
                                   const AudioProcessContext& context, const MidiBuffer& inputMidi,
                                   MidiBuffer& outputMidi) override;
    PluginInfo getInfo() const override;
    uint32_t getLatencyFrames() const noexcept override { return latencyFrames_.load(std::memory_order_relaxed); }
    void setParameter(uint32_t portIndex, float value) override;
    float getParameter(uint32_t portIndex) const override;
    uint32_t getNumInputPorts() const override;
    uint32_t getNumOutputPorts() const override;
    PluginState saveState() override;
    bool restoreState(const PluginState& state) override;
    bool hasJsfxGfx() const noexcept override;
    JsfxUiHost* jsfxUiHost() noexcept override;

private:
    static constexpr uint32_t kMaxSliders = ysfx_max_sliders;
    // One bit per slider, packed into 64-bit words, matching NativePlugin, so
    // the audio thread scans four atomic words per block instead of 256 atomic
    // read-modify-writes.
    static constexpr uint32_t kSliderWords = ysfx_max_slider_groups;
    static_assert(kSliderWords * 64 == kMaxSliders,
                  "slider change mask must cover every slider exactly once");
    // A JSFX may rewrite its PDC from any section, so a reported-latency change
    // is only published once the script has held the same value this many
    // consecutive blocks. At 48-frame blocks/48 kHz this is ~16 ms, short
    // enough to be inaudible for a real change and long enough that per-block
    // jitter never reaches the graph's delay compensation.
    static constexpr uint32_t kLatencyStableBlocks = 16;
    static constexpr uint32_t kMaxQuantum = 8192;
    static constexpr uint32_t kMidiCapacityBytes = 67'584;
    std::shared_ptr<ysfx_config_t> config_;
    ysfx_t* fx_ = nullptr;
    std::unique_ptr<JsfxUiHost> uiHost_;
    std::string path_;
    std::string id_;
    PluginInfo info_;
    std::array<std::atomic<float>, kMaxSliders> pending_{};
    std::array<std::atomic<uint64_t>, kSliderWords> dirty_{};
    std::atomic<bool> active_{false};
    std::atomic<bool> ready_{false};
    std::atomic<bool> callbackFaulted_{false};
    std::atomic<uint32_t> quantum_{0};
    std::atomic<uint32_t> latencyFrames_{0};
    // Set when ysfx has flagged @init (transport restart). The audio thread
    // bypasses this plugin until the worker has run it, because ysfx_init()
    // destroys open file objects and cannot run beside @sample.
    std::atomic<bool> initPending_{false};
    // Audio-thread only; guarded by the single-writer process() path.
    uint32_t latencyCandidate_ = 0;
    uint32_t latencyStableBlocks_ = 0;
    mutable std::mutex controlMutex_;

    // @slider is script-defined and unbounded, so it runs here rather than on
    // the audio thread. The worker coalesces: whatever slider values are
    // pending when it wakes are applied together, then @slider runs once. The
    // audio thread keeps processing with the previous coefficients meanwhile.
    void sliderWorkerLoop();
    void applyPendingSliders();
    std::thread sliderWorker_;
    std::mutex sliderMutex_;
    std::condition_variable sliderSignal_;
    bool sliderPending_ = false;
    bool sliderWorkerStopping_ = false;
};

} // namespace guitarrackcraft
