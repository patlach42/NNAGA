/* Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * This file is part of NNAGA.
 * NNAGA is free software under the GNU General Public License version 3.
 */
#include "JsfxPlugin.h"
#include "JsfxUiHost.h"
#include <algorithm>
#include <cmath>

namespace guitarrackcraft {

namespace {
class UiPauseGuard {
public:
    explicit UiPauseGuard(JsfxUiHost* host) : host_(host) {
        if (host_) host_->pauseEffect();
    }
    ~UiPauseGuard() {
        if (host_) host_->resumeEffect();
    }
private:
    JsfxUiHost* host_;
};
} // namespace

JsfxPlugin::JsfxPlugin(std::shared_ptr<ysfx_config_t> config, std::string path, std::string id)
    : config_(std::move(config)), path_(std::move(path)), id_(std::move(id)) {
    if (!config_) return;
    fx_ = ysfx_new(config_.get());
    if (!fx_ || !ysfx_load_file(fx_, path_.c_str(), 0) || !ysfx_compile(fx_, 0)) {
        if (fx_) ysfx_free(fx_);
        fx_ = nullptr;
        return;
    }
    uiHost_ = ysfx_has_section(fx_, ysfx_section_gfx)
        ? std::make_unique<JsfxUiHost>(fx_) : nullptr;
    info_.id = id_;
    info_.name = ysfx_get_name(fx_) ? ysfx_get_name(fx_) : path_;
    info_.format = "JSFX";
    for (uint32_t index = 0;
         index < kMaxSliders && ysfx_slider_exists(fx_, index);
         ++index) {
        ysfx_slider_curve_t curve{};
        if (!ysfx_slider_get_curve(fx_, index, &curve)) continue;
        PortInfo port{};
        port.index = index;
        const char* name = ysfx_slider_get_name(fx_, index);
        port.name = name ? name : "slider";
        port.symbol = "slider" + std::to_string(index);
        port.isInput = true;
        port.isControl = true;
        port.defaultValue = static_cast<float>(curve.def);
        port.minValue = static_cast<float>(curve.min);
        port.maxValue = static_cast<float>(curve.max);
        port.stepCount = curve.inc > 0
            ? static_cast<int32_t>((curve.max - curve.min) / curve.inc)
            : 0;
        port.isToggle = port.stepCount == 1 &&
            curve.min == 0.0 && curve.max == 1.0;
        if (ysfx_slider_is_enum(fx_, index)) {
            const uint32_t count = ysfx_slider_get_enum_size(fx_, index);
            std::vector<const char*> names(count);
            const uint32_t received = ysfx_slider_get_enum_names(
                fx_, index, names.data(), count);
            port.scalePoints.reserve(received);
            const double increment = curve.inc > 0.0 ? curve.inc : 1.0;
            for (uint32_t item = 0; item < received; ++item) {
                port.scalePoints.push_back({
                    names[item] ? names[item] : std::to_string(item),
                    static_cast<float>(curve.min + increment * item),
                });
            }
            port.isToggle = received == 2 &&
                curve.min == 0.0 && curve.max == 1.0;
        }
        info_.ports.push_back(std::move(port));
        pending_[index].store(static_cast<float>(curve.def), std::memory_order_relaxed);
    }
}

JsfxPlugin::~JsfxPlugin() {
    uiHost_.reset();
    if (fx_) ysfx_free(fx_);
}

void JsfxPlugin::activate(float sampleRate, uint32_t bufferSize) {
    std::lock_guard lock(controlMutex_);
    latencyFrames_.store(0, std::memory_order_relaxed);
    if (!fx_) return;
    UiPauseGuard pause(uiHost_.get());
    ysfx_set_sample_rate(fx_, sampleRate);
    ysfx_set_block_size(fx_, bufferSize);
    ysfx_set_midi_capacity(fx_, 256, false);
    ysfx_init(fx_);
    active_.store(true, std::memory_order_release);
}

void JsfxPlugin::deactivate() {
    active_.store(false, std::memory_order_release);
    latencyFrames_.store(0, std::memory_order_release);
}

uint32_t JsfxPlugin::process(const float* const* inputs, float* const* outputs, uint32_t frames,
                             const AudioProcessContext& context, const MidiEvent* inputEvents,
                             uint32_t inputCount, MidiEvent* outputEvents, uint32_t outputCapacity) {
    if (!fx_ || !active_.load(std::memory_order_acquire)) return 0;
    for (uint32_t i = 0; i < kMaxSliders; ++i)
        if (dirty_[i].exchange(false, std::memory_order_acq_rel))
            ysfx_slider_set_value(fx_, i, pending_[i].load(std::memory_order_relaxed), true);
    ysfx_time_info_t ti{}; ti.tempo = context.beatsPerMinute; ti.beat_position = context.beatPosition;
    ti.time_position = context.sampleRate > 0 ? context.samplePosition / context.sampleRate : 0;
    ti.playback_state = context.playing ? ysfx_playback_playing : ysfx_playback_stopped;
    ysfx_set_time_info(fx_, &ti);
    for (uint32_t i = 0; i < inputCount; ++i) {
        uint8_t bytes[3] = {inputEvents[i].status, inputEvents[i].data1, inputEvents[i].data2};
        ysfx_midi_event_t ev{0, inputEvents[i].frameOffset, 3, bytes}; ysfx_send_midi(fx_, &ev);
    }
    ysfx_process_float(fx_, inputs, outputs, 2, 2, frames);
    const ysfx_real pdc = ysfx_get_pdc_delay(fx_);
    uint32_t channels[2] = {0, 0};
    ysfx_get_pdc_channels(fx_, channels);
    const bool defaultChannels = channels[0] == 0 && channels[1] == 0;
    const bool stereoChannels = channels[0] == 0 && channels[1] >= 2;
    if (std::isfinite(static_cast<double>(pdc)) && pdc >= 0 &&
        (defaultChannels || stereoChannels)) {
        latencyFrames_.store(static_cast<uint32_t>(pdc), std::memory_order_relaxed);
    }
    uint32_t count = 0; ysfx_midi_event_t ev{};
    while (count < outputCapacity && ysfx_receive_midi(fx_, &ev)) {
        if (ev.size >= 3 && ev.data) outputEvents[count++] = {ev.offset, ev.data[0], ev.data[1], ev.data[2]};
    }
    return count;
}

PluginInfo JsfxPlugin::getInfo() const { return info_; }
void JsfxPlugin::setParameter(uint32_t i, float v) { if (i < kMaxSliders) { pending_[i].store(v, std::memory_order_relaxed); dirty_[i].store(true, std::memory_order_release); } }
float JsfxPlugin::getParameter(uint32_t i) const { return fx_ && i < kMaxSliders ? static_cast<float>(ysfx_slider_get_value(fx_, i)) : 0.f; }
uint32_t JsfxPlugin::getNumInputPorts() const { return fx_ ? std::min(ysfx_get_num_inputs(fx_), 2u) : 0; }
uint32_t JsfxPlugin::getNumOutputPorts() const { return fx_ ? std::min(ysfx_get_num_outputs(fx_), 2u) : 0; }

PluginState JsfxPlugin::saveState() {
    std::lock_guard lock(controlMutex_);
    PluginState state;
    state.format = "JSFX";
    state.pluginUri = id_;
    if (!fx_) return state;
    UiPauseGuard pause(uiHost_.get());
    ysfx_state_t* saved = ysfx_save_state(fx_);
    if (!saved) return state;
    state.controlPortValues.reserve(saved->slider_count);
    for (uint32_t index = 0; index < saved->slider_count; ++index) {
        state.controlPortValues.emplace_back(
            saved->sliders[index].index,
            static_cast<float>(saved->sliders[index].value));
    }
    if (saved->data && saved->data_size) {
        state.properties.push_back({
            "urn:nnaga:ysfx:serialize",
            {saved->data, saved->data + saved->data_size},
            "application/octet-stream",
            0,
        });
    }
    ysfx_state_free(saved);
    return state;
}

bool JsfxPlugin::restoreState(const PluginState& state) {
    std::lock_guard lock(controlMutex_);
    if (!fx_ || state.pluginUri != id_) return false;
    UiPauseGuard pause(uiHost_.get());
    ysfx_state_t saved{};
    std::vector<ysfx_state_slider_t> sliders;
    sliders.reserve(state.controlPortValues.size());
    for (const auto& [index, value] : state.controlPortValues) {
        sliders.push_back({index, value});
    }
    saved.sliders = sliders.data();
    saved.slider_count = sliders.size();
    for (const auto& property : state.properties) {
        if (property.keyUri == "urn:nnaga:ysfx:serialize") {
            saved.data = const_cast<uint8_t*>(property.value.data());
            saved.data_size = property.value.size();
            break;
        }
    }
    const bool restored = ysfx_load_state(fx_, &saved);
    if (restored) {
        for (const auto& [index, value] : state.controlPortValues) {
            if (index < kMaxSliders) {
                pending_[index].store(value, std::memory_order_relaxed);
                dirty_[index].store(true, std::memory_order_release);
            }
        }
    }
    return restored;
}
bool JsfxPlugin::hasJsfxGfx() const noexcept { return fx_ && ysfx_has_section(fx_, ysfx_section_gfx); }
JsfxUiHost* JsfxPlugin::jsfxUiHost() noexcept { return uiHost_.get(); }
}
