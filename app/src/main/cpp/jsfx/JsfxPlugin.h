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
#include <memory>
#include <mutex>
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
    void deactivate() override;
    uint32_t process(const float* const* inputs, float* const* outputs, uint32_t numFrames,
                     const AudioProcessContext& context, const MidiEvent* inputEvents,
                     uint32_t inputCount, MidiEvent* outputEvents, uint32_t outputCapacity) override;
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
    std::shared_ptr<ysfx_config_t> config_;
    ysfx_t* fx_ = nullptr;
    std::unique_ptr<JsfxUiHost> uiHost_;
    std::string path_;
    std::string id_;
    PluginInfo info_;
    std::array<std::atomic<float>, kMaxSliders> pending_{};
    std::array<std::atomic<bool>, kMaxSliders> dirty_{};
    std::atomic<bool> active_{false};
    std::atomic<uint32_t> latencyFrames_{0};
    mutable std::mutex controlMutex_;
};

} // namespace guitarrackcraft
