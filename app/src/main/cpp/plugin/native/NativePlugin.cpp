#include "NativePlugin.h"

#include <algorithm>
#include <android/log.h>
#include <cctype>
#include <cmath>
#include <cstring>
#include <dlfcn.h>
#include <unordered_set>
namespace guitarrackcraft {
namespace {
constexpr char kTag[] = "NativePlugin";
constexpr uint32_t kMissingOrdinal = UINT32_MAX;

bool validText(const char* value) { return value && *value; }
bool validId(const char* value) {
    if (!validText(value) || std::strlen(value) > 128) return false;
    const auto first = static_cast<unsigned char>(value[0]);
    if (!std::isalnum(first)) return false;
    for (const char* p = value; *p; ++p) {
        const unsigned char c = static_cast<unsigned char>(*p);
        if (!std::isalnum(c) && c != '.' && c != '_' && c != '-') return false;
    }
    return true;
}

float clampNormalized(float value) noexcept {
    if (!std::isfinite(value)) return 0.0f;
    return std::max(0.0f, std::min(1.0f, value));
}

PortInfo audioPort(uint32_t index, const char* name, bool input) {
    PortInfo port{};
    port.index = index;
    port.name = name;
    port.symbol = input ? (index == 0 ? "in_l" : "in_r") : (index == 2 ? "out_l" : "out_r");
    port.isInput = input;
    port.isAudio = true;
    return port;
}
} // namespace

NativePluginLibrary::~NativePluginLibrary() {
    if (handle) dlclose(handle);
}

bool validateNativePluginLibrary(const std::shared_ptr<NativePluginLibrary>& library,
                                 std::vector<const NnagaPluginDescriptorV1*>* descriptors,
                                 std::string* error) {
    auto fail = [&](const char* message) {
        if (error) *error = message;
        return false;
    };
    if (!library || !library->abi) return fail("missing library entry");
    const auto* abi = library->abi;
    if (abi->struct_size < sizeof(NnagaPluginLibraryV1) || abi->abi_version != NNAGA_NATIVE_ABI_VERSION ||
        abi->plugin_count == 0 || !abi->get_plugin) return fail("invalid library ABI");
    std::unordered_set<std::string> ids;
    std::unordered_set<uint32_t> ports;
    for (uint32_t i = 0; i < abi->plugin_count; ++i) {
        const auto* descriptor = abi->get_plugin(i);
        if (!descriptor || descriptor->struct_size < sizeof(NnagaPluginDescriptorV1) ||
            !validId(descriptor->id) || !validText(descriptor->name) || !validText(descriptor->vendor) ||
            !validText(descriptor->version) || descriptor->audio_inputs != 2 || descriptor->audio_outputs != 2 ||
            descriptor->parameter_count > NNAGA_NATIVE_MAX_PARAMETERS || !descriptor->parameters ||
            !descriptor->create || !descriptor->destroy || !descriptor->activate || !descriptor->deactivate ||
            !descriptor->reset || !descriptor->set_parameter || !descriptor->process || !ids.emplace(descriptor->id).second)
            return fail("invalid plugin descriptor");
        ports.clear();
        std::unordered_set<std::string> symbols;
        for (uint32_t p = 0; p < descriptor->parameter_count; ++p) {
            const auto& parameter = descriptor->parameters[p];
            if (parameter.struct_size < sizeof(NnagaParameterV1) || parameter.port_index < 4 ||
                !validText(parameter.name) || !validText(parameter.symbol) || !std::isfinite(parameter.default_normalized) ||
                parameter.default_normalized < 0.0f || parameter.default_normalized > 1.0f ||
                !ports.emplace(parameter.port_index).second || !symbols.emplace(parameter.symbol).second)
                return fail("invalid parameter descriptor");
            if (parameter.scale_point_count != 0 && !parameter.scale_points)
                return fail("missing parameter scale points");
            for (uint32_t s = 0; s < parameter.scale_point_count; ++s) {
                const auto& point = parameter.scale_points[s];
                if (point.struct_size < sizeof(NnagaScalePointV1) || !validText(point.label) ||
                    !std::isfinite(point.normalized_value) || point.normalized_value < 0.0f ||
                    point.normalized_value > 1.0f)
                    return fail("invalid parameter scale point");
            }
        }
        if (descriptors) descriptors->push_back(descriptor);
    }
    return true;
}

NativePlugin::NativePlugin(std::shared_ptr<NativePluginLibrary> library, const NnagaPluginDescriptorV1* descriptor)
    : library_(std::move(library)), descriptor_(descriptor), handle_(descriptor ? descriptor->create() : nullptr) {
    if (!descriptor_ || !handle_) return;
    info_.id = descriptor_->id;
    info_.name = descriptor_->name;
    info_.format = "NATIVE";
    info_.originPath = library_->path;
    info_.ports = {audioPort(0, "Input L", true), audioPort(1, "Input R", true),
                   audioPort(2, "Output L", false), audioPort(3, "Output R", false)};
    parameterCount_ = descriptor_->parameter_count;
    for (uint32_t i = 0; i < parameterCount_; ++i) {
        const auto& parameter = descriptor_->parameters[i];
        ports_[i] = parameter.port_index;
        values_[i].store(parameter.default_normalized, std::memory_order_relaxed);
        dirty_[i / 64].fetch_or(UINT64_C(1) << (i % 64), std::memory_order_relaxed);
        PortInfo port{};
        port.index = parameter.port_index;
        port.name = parameter.name;
        port.symbol = parameter.symbol;
        port.isInput = true;
        port.isControl = true;
        port.isToggle = (parameter.flags & NNAGA_PARAMETER_TOGGLE) != 0;
        port.defaultValue = parameter.default_normalized;
        port.minValue = 0.0f;
        port.maxValue = 1.0f;
        port.unit = parameter.unit ? parameter.unit : "";
        port.stepCount = static_cast<int32_t>(parameter.scale_point_count == 0 ? 0 : parameter.scale_point_count - 1);
        for (uint32_t s = 0; s < parameter.scale_point_count; ++s)
            port.scalePoints.push_back({parameter.scale_points[s].label, parameter.scale_points[s].normalized_value});
        info_.ports.push_back(std::move(port));
    }
}

NativePlugin::~NativePlugin() {
    if (handle_ && descriptor_) descriptor_->destroy(handle_);
}

void NativePlugin::activate(float sampleRate, uint32_t bufferSize) {
    if (handle_ && !descriptor_->activate(handle_, sampleRate, bufferSize))
        __android_log_print(ANDROID_LOG_ERROR, kTag, "activation failed for %s", descriptor_->id);
}

void NativePlugin::deactivate() {
    if (handle_) descriptor_->deactivate(handle_);
}

uint32_t NativePlugin::process(const float* const* inputs, float* const* outputs, uint32_t numFrames,
                               const AudioProcessContext& context, const MidiEvent*, uint32_t,
                               MidiEvent*, uint32_t) {
    if (!handle_ || !inputs || !outputs || !inputs[0] || !inputs[1] || !outputs[0] || !outputs[1]) return 0;
    for (uint32_t word = 0; word < dirty_.size(); ++word) {
        uint64_t changed = dirty_[word].exchange(0, std::memory_order_acquire);
        while (changed) {
            const uint32_t bit = static_cast<uint32_t>(__builtin_ctzll(changed));
            const uint32_t ordinal = word * 64 + bit;
            if (ordinal < parameterCount_)
                descriptor_->set_parameter(handle_, ports_[ordinal], values_[ordinal].load(std::memory_order_relaxed));
            changed &= changed - 1;
        }
    }
    const NnagaProcessContextV1 nativeContext{
        sizeof(NnagaProcessContextV1), context.samplePosition, context.transportFrame, context.loopEndFrame,
        context.sampleRate, context.beatsPerMinute, static_cast<uint8_t>(context.playing), static_cast<uint8_t>(context.looping), 0,
        context.beatPosition, context.bar, context.barBeat, context.musicalQuarterNotes, context.beatsPerBar, context.beatUnit};
    descriptor_->process(handle_, inputs[0], inputs[1], outputs[0], outputs[1], numFrames, &nativeContext);
    return 0;
}

uint32_t NativePlugin::ordinalForPort(uint32_t portIndex) const noexcept {
    for (uint32_t i = 0; i < parameterCount_; ++i) if (ports_[i] == portIndex) return i;
    return kMissingOrdinal;
}

void NativePlugin::setParameter(uint32_t portIndex, float value) {
    const uint32_t ordinal = ordinalForPort(portIndex);
    if (ordinal == kMissingOrdinal) return;
    values_[ordinal].store(clampNormalized(value), std::memory_order_relaxed);
    dirty_[ordinal / 64].fetch_or(UINT64_C(1) << (ordinal % 64), std::memory_order_release);
}

float NativePlugin::getParameter(uint32_t portIndex) const {
    const uint32_t ordinal = ordinalForPort(portIndex);
    return ordinal == kMissingOrdinal ? 0.0f : values_[ordinal].load(std::memory_order_relaxed);
}

std::string NativePlugin::getParameterDisplay(uint32_t portIndex) const {
    if (!descriptor_->format_parameter) return {};
    const uint32_t ordinal = ordinalForPort(portIndex);
    if (ordinal == kMissingOrdinal) return {};
    char buffer[64]{};
    const uint32_t written = descriptor_->format_parameter(handle_, portIndex,
        values_[ordinal].load(std::memory_order_relaxed), buffer, sizeof(buffer));
    return written ? std::string(buffer, std::min<uint32_t>(written, sizeof(buffer) - 1)) : std::string{};
}

uint32_t NativePlugin::getLatencyFrames() const noexcept {
    return descriptor_->latency_frames ? descriptor_->latency_frames(handle_) : 0;
}

PluginState NativePlugin::saveState() {
    PluginState state{};
    state.pluginUri = info_.id;
    state.format = info_.format;
    state.controlPortValues.reserve(parameterCount_);
    for (uint32_t i = 0; i < parameterCount_; ++i)
        state.controlPortValues.emplace_back(ports_[i], values_[i].load(std::memory_order_relaxed));
    return state;
}

bool NativePlugin::restoreState(const PluginState& state) {
    if (state.pluginUri != info_.id || (!state.format.empty() && state.format != info_.format)) return false;
    for (const auto& [port, value] : state.controlPortValues) {
        if (ordinalForPort(port) == kMissingOrdinal) return false;
        setParameter(port, value);
    }
    return true;
}
} // namespace guitarrackcraft
