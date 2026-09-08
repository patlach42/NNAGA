/* Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * This file is part of NNAGA.
 * NNAGA is free software under the GNU General Public License version 3.
 */
#include "JsfxPlugin.h"
#include "JsfxUiHost.h"
#include "JsfxYsfxInternals.h"
#include <algorithm>
#include <cmath>

namespace guitarrackcraft {

namespace {
// EEL's JIT entry sequence (GLUE_CALL_CODE, glue_aarch64.h) reads FPCR and,
// when flush-to-zero is clear, does msr/call/msr around *every* code execution.
// @sample runs once per frame, so that is 48k FPCR writes per second per plugin
// -- measured at 41-54 ns/sample on arm64. Android starts threads with FPCR=0,
// so the slow branch is always taken. Setting FZ for the duration of the block
// makes EEL take its fast branch instead; semantics are unchanged, because EEL
// already forces FZ=1 for the whole of every JIT call today. Scoped to the ysfx
// call so no other plugin format sees a different FP environment.
class ScopedFlushToZero {
public:
    ScopedFlushToZero() {
#if defined(__aarch64__)
        __asm__ __volatile__("mrs %0, fpcr" : "=r"(saved_));
        if ((saved_ & kFz) == 0) {
            const uint64_t enabled = saved_ | kFz;
            __asm__ __volatile__("msr fpcr, %0" : : "r"(enabled));
        }
#endif
    }
    ~ScopedFlushToZero() {
#if defined(__aarch64__)
        if ((saved_ & kFz) == 0)
            __asm__ __volatile__("msr fpcr, %0" : : "r"(saved_));
#endif
    }
    ScopedFlushToZero(const ScopedFlushToZero&) = delete;
    ScopedFlushToZero& operator=(const ScopedFlushToZero&) = delete;
private:
#if defined(__aarch64__)
    static constexpr uint64_t kFz = 1ull << 24;
    uint64_t saved_ = 0;
#endif
};

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
    info_.realtimeClass = RealtimeClass::CertifiedInProcess;
    info_.originPath = path_;
    for (uint32_t index = 0; index < kMaxSliders; ++index) {
        if (!ysfx_slider_exists(fx_, index)) continue;
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
            ? static_cast<int32_t>((curve.max - curve.min) / curve.inc) : 0;
        port.isToggle = port.stepCount == 1 && curve.min == 0.0 && curve.max == 1.0;
        if (ysfx_slider_is_enum(fx_, index)) {
            const uint32_t count = ysfx_slider_get_enum_size(fx_, index);
            std::vector<const char*> names(count);
            const uint32_t received = ysfx_slider_get_enum_names(fx_, index, names.data(), count);
            port.scalePoints.reserve(received);
            const double increment = curve.inc > 0.0 ? curve.inc : 1.0;
            for (uint32_t item = 0; item < received; ++item)
                port.scalePoints.push_back({names[item] ? names[item] : std::to_string(item),
                    static_cast<float>(curve.min + increment * item)});
            port.isToggle = received == 2 && curve.min == 0.0 && curve.max == 1.0;
        }
        info_.ports.push_back(std::move(port));
        pending_[index].store(static_cast<float>(curve.def), std::memory_order_relaxed);
    }
    sliderWorker_ = std::thread([this] { sliderWorkerLoop(); });
}

void JsfxPlugin::applyPendingSliders() {
    bool any = false;
    for (uint32_t word = 0; word < dirty_.size(); ++word) {
        if (dirty_[word].load(std::memory_order_relaxed) == 0) continue;
        uint64_t changed = dirty_[word].exchange(0, std::memory_order_acquire);
        while (changed) {
            const uint32_t bit = static_cast<uint32_t>(__builtin_ctzll(changed));
            const uint32_t index = word * 64 + bit;
            // notify=false: ysfx must not schedule @slider onto the audio
            // thread, we run it here ourselves once all values are in.
            ysfx_slider_set_value(fx_, index,
                                  pending_[index].load(std::memory_order_relaxed), false);
            changed &= changed - 1;
            any = true;
        }
    }
    if (any) (void)jsfxRunSliderCode(fx_);
}

void JsfxPlugin::sliderWorkerLoop() {
    for (;;) {
        {
            std::unique_lock lock(sliderMutex_);
            sliderSignal_.wait(lock, [&] { return sliderPending_ || sliderWorkerStopping_; });
            if (sliderWorkerStopping_) return;
            sliderPending_ = false;
        }
        if (initPending_.load(std::memory_order_acquire)) {
            {
                std::lock_guard lock(controlMutex_);
                if (fx_) {
                    UiPauseGuard pause(uiHost_.get());
                    jsfxRunInit(fx_);
                }
            }
            // Release the audio thread before applying sliders, so the bypass
            // lasts only as long as @init itself.
            initPending_.store(false, std::memory_order_release);
        }
        // controlMutex_ serialises against activate/save/restore, which also
        // drive the VM. Anything that arrives while @slider runs is picked up
        // on the next iteration, so a drag coalesces instead of queueing.
        std::lock_guard lock(controlMutex_);
        if (fx_) applyPendingSliders();
    }
}

JsfxPlugin::~JsfxPlugin() {
    if (sliderWorker_.joinable()) {
        {
            std::lock_guard lock(sliderMutex_);
            sliderWorkerStopping_ = true;
        }
        sliderSignal_.notify_all();
        sliderWorker_.join();
    }
    uiHost_.reset();
    if (fx_) ysfx_free(fx_);
}

void JsfxPlugin::activate(float sampleRate, uint32_t bufferSize) {
    std::lock_guard lock(controlMutex_);
    active_.store(false, std::memory_order_release);
    ready_.store(false, std::memory_order_release);
    callbackFaulted_.store(false, std::memory_order_release);
    quantum_.store(0, std::memory_order_release);
    latencyFrames_.store(0, std::memory_order_relaxed);
    if (!fx_ || !std::isfinite(sampleRate) || sampleRate <= 0.0f ||
        bufferSize == 0 || bufferSize > kMaxQuantum) return;
    try {
        UiPauseGuard pause(uiHost_.get());
        ysfx_set_sample_rate(fx_, sampleRate);
        ysfx_set_block_size(fx_, bufferSize);
        ysfx_set_midi_capacity(fx_, kMidiCapacityBytes, false);
        // Claim script RAM here, on the control thread, so EEL never callocs
        // from inside @sample or @gfx.
        jsfxPreallocRam(fx_, kJsfxPreallocItems);
        ysfx_init(fx_);
        // ysfx_init leaves @slider pending; run it here rather than letting the
        // first audio block pay for it.
        applyPendingSliders();
        (void)jsfxRunSliderCode(fx_);
        quantum_.store(bufferSize, std::memory_order_release);
        ready_.store(true, std::memory_order_release);
        active_.store(true, std::memory_order_release);
    } catch (...) {}
}

void JsfxPlugin::deactivate() {
    active_.store(false, std::memory_order_release);
    ready_.store(false, std::memory_order_release);
    quantum_.store(0, std::memory_order_release);
    latencyFrames_.store(0, std::memory_order_release);
}

MidiOutputDisposition JsfxPlugin::process(const float* const* inputs, float* const* outputs, uint32_t frames,
                                          const AudioProcessContext& context, const MidiBuffer& inputMidi,
                                          MidiBuffer& outputMidi) {
    auto passthrough = [&] {
        if (outputs) for (uint32_t ch = 0; ch < 2; ++ch) if (outputs[ch]) {
            if (inputs && inputs[ch]) std::copy_n(inputs[ch], frames, outputs[ch]);
            else std::fill_n(outputs[ch], frames, 0.0f);
        }
        return MidiOutputDisposition::Passthrough;
    };
    if (!fx_ || !active_.load(std::memory_order_acquire) ||
        callbackFaulted_.load(std::memory_order_acquire) || frames == 0 ||
        frames > quantum_.load(std::memory_order_acquire) ||
        frames > kMaxQuantum || !inputs || !inputs[0] || !inputs[1] ||
        !outputs || !outputs[0] || !outputs[1] ||
        initPending_.load(std::memory_order_acquire))
        return passthrough();
    // The audio thread never yields the VM to the UI. ysfx is designed for
    // @gfx and @sample to run on separate threads concurrently -- that is what
    // its thread-id guards are for, and it is what REAPER and ysfx's own plugin
    // do. Making the audio thread stand down instead cost 0.2% of blocks with
    // the editor closed and 8.9% with it open, and a lost block is silence for
    // a synth and a dry jump for a reverb. A stale UI frame is the cheaper loss.
    try {
        // Slider values and @slider are applied by sliderWorkerLoop(), off the
        // audio thread; nothing to do here.
        ysfx_time_info_t ti{};
        ti.tempo = context.beatsPerMinute;
        ti.beat_position = context.beatPosition;
        ti.time_position = context.sampleRate > 0 ? context.samplePosition / context.sampleRate : 0;
        ti.playback_state = context.playing ? ysfx_playback_playing : ysfx_playback_stopped;
        ysfx_set_time_info(fx_, &ti);
        // A transport restart makes ysfx want @init. It is unbounded and tears
        // down the script's file objects, so it cannot run here nor beside
        // @sample: take the flag, bypass this block, and let the worker run it.
        if (jsfxTakePendingInit(fx_)) {
            initPending_.store(true, std::memory_order_release);
            {
                std::lock_guard lock(sliderMutex_);
                sliderPending_ = true;
            }
            sliderSignal_.notify_one();
            return passthrough();
        }
        for (uint32_t i = 0; i < inputMidi.eventCount(); ++i) {
            const MidiEvent& midi = inputMidi.eventAt(i);
            if (midi.frameOffset >= frames || midi.payloadSize == 0 ||
                midi.payloadSize > kMaxMidiPayloadBytes ||
                midi.payloadOffset > inputMidi.payloadBytes() ||
                midi.payloadSize > inputMidi.payloadBytes() - midi.payloadOffset) {
                outputMidi.recordRejectedMessages();
                continue;
            }
            ysfx_midi_event_t ev{0, midi.frameOffset, midi.payloadSize,
                                 inputMidi.payloadFor(midi)};
            ysfx_send_midi(fx_, &ev);
        }
        {
            ScopedFlushToZero denormalsOff;
            ysfx_process_float(fx_, inputs, outputs, 2, 2, frames);
        }
        // pdc_delay/pdc_bot_ch/pdc_top_ch are ordinary script globals, writable
        // from @slider, @block, @sample and @gfx, so their value can move every
        // block. The graph treats a reported-latency change as a timeline
        // change and restarts delay compensation, which costs a burst of
        // silence on every other track; a value that keeps moving keeps the
        // rack silent. Latency is a control-rate property, so only publish a
        // new value once the script has held it for kLatencyStableBlocks.
        uint32_t candidate = 0;
        const ysfx_real pdc = ysfx_get_pdc_delay(fx_);
        uint32_t channels[2] = {0, 0};
        ysfx_get_pdc_channels(fx_, channels);
        const bool defaultChannels = channels[0] == 0 && channels[1] == 0;
        const bool stereoChannels = channels[0] == 0 && channels[1] >= 2;
        if (std::isfinite(static_cast<double>(pdc)) && pdc >= 0 &&
            (defaultChannels || stereoChannels))
            candidate = static_cast<uint32_t>(
                std::min(static_cast<double>(pdc), 65535.0));
        if (candidate == latencyFrames_.load(std::memory_order_relaxed)) {
            latencyCandidate_ = candidate;
            latencyStableBlocks_ = 0;
        } else if (candidate == latencyCandidate_) {
            if (++latencyStableBlocks_ >= kLatencyStableBlocks) {
                latencyFrames_.store(candidate, std::memory_order_relaxed);
                latencyStableBlocks_ = 0;
            }
        } else {
            latencyCandidate_ = candidate;
            latencyStableBlocks_ = 1;
        }
        bool emitted = false;
        ysfx_midi_event_t ev{};
        while (ysfx_receive_midi(fx_, &ev)) {
            emitted = true;
            if (!ev.data || ev.size == 0 || ev.size > kMaxMidiPayloadBytes ||
                ev.offset >= frames) {
                outputMidi.recordRejectedMessages();
                continue;
            }
            outputMidi.append(ev.offset, ev.data, ev.size);
        }
        return emitted ? MidiOutputDisposition::Replace : MidiOutputDisposition::Passthrough;
    } catch (...) {
        callbackFaulted_.store(true, std::memory_order_release);
        return MidiOutputDisposition::Passthrough;
    }
}

PluginInfo JsfxPlugin::getInfo() const { return info_; }
void JsfxPlugin::setParameter(uint32_t i, float v) {
    if (i >= kMaxSliders) return;
    pending_[i].store(v, std::memory_order_relaxed);
    dirty_[i / 64].fetch_or(UINT64_C(1) << (i % 64), std::memory_order_release);
    {
        std::lock_guard lock(sliderMutex_);
        sliderPending_ = true;
    }
    sliderSignal_.notify_one();
}
float JsfxPlugin::getParameter(uint32_t i) const {
    return i < kMaxSliders ? pending_[i].load(std::memory_order_relaxed) : 0.0f;
}
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
                dirty_[index / 64].fetch_or(UINT64_C(1) << (index % 64), std::memory_order_release);
            }
        }
    }
    return restored;
}
bool JsfxPlugin::hasJsfxGfx() const noexcept { return fx_ && ysfx_has_section(fx_, ysfx_section_gfx); }
JsfxUiHost* JsfxPlugin::jsfxUiHost() noexcept { return uiHost_.get(); }
}
