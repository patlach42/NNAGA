/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * This file is part of NNAGA.
 *
 * NNAGA is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * NNAGA is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with NNAGA. If not, see <https://www.gnu.org/licenses/>.
 */

#include "LV2Plugin.h"
#include "LV2Utils.h"
#include "../PluginUIGuard.h"
#include <android/log.h>
#include <cmath>
#include <cerrno>
#include <cstring>
#include <algorithm>
#include <array>
#include <semaphore.h>
#include <mutex>
#include <atomic>
#include <vector>

#define LOG_TAG "LV2Plugin"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

#if defined(HAVE_LV2) && HAVE_LV2 == 1
#include <lilv/lilv.h>
#include <lv2/urid/urid.h>
#include <lv2/atom/util.h>

// ---------- Process-lifetime fixed URID map (shared across DSP and UI) ------
namespace {
constexpr uint32_t kUridSlots = 65536;
constexpr uint32_t kUridArenaSize = 8u * 1024u * 1024u;
thread_local bool gLv2RealtimeMapContext = false;
thread_local bool gLv2RealtimeMapMiss = false;
struct UridSlot {
    std::atomic<uint64_t> published{0};  // high 32 bits ID, low 32 bits arena offset
};
struct UridMapImpl {
    std::array<UridSlot, kUridSlots> slots;
    std::array<char, kUridArenaSize> arena{};
    std::atomic<uint32_t> nextId{1};
    std::atomic<uint32_t> arenaUsed{1};
    std::mutex insertMutex;
    UridMapImpl() { arena[0] = '\0'; }
    static uint32_t hashUri(const char* uri, size_t length) {
        uint32_t h = 2166136261u;
        for (size_t index = 0; index < length; ++index)
            h = (h ^ static_cast<unsigned char>(uri[index])) * 16777619u;
        return h ? h : 1u;
    }
    LV2_URID lookup(const char* uri, uint32_t start) const {
        for (uint32_t n = 0; n < kUridSlots; ++n) {
            const UridSlot& slot = slots[(start + n) & (kUridSlots - 1)];
            const uint64_t published = slot.published.load(std::memory_order_acquire);
            if (published == 0) return 0;
            const uint32_t offset = static_cast<uint32_t>(published);
            if (std::strcmp(arena.data() + offset, uri) == 0) {
                return static_cast<uint32_t>(published >> 32U);
            }
        }
        return 0;
    }
    LV2_URID map(const char* uri) {
        if (!uri) return 0;
        const size_t length = strnlen(uri, kUridArenaSize);
        if (length >= kUridArenaSize) return 0;
        const uint32_t start = hashUri(uri, length) & (kUridSlots - 1);
        if (const LV2_URID existing = lookup(uri, start)) return existing;
        if (gLv2RealtimeMapContext) {
            gLv2RealtimeMapMiss = true;
            return 0;
        }
        std::lock_guard lock(insertMutex);
        if (const LV2_URID existing = lookup(uri, start)) return existing;
        for (uint32_t n = 0; n < kUridSlots; ++n) {
            UridSlot& slot = slots[(start + n) & (kUridSlots - 1)];
            if (slot.published.load(std::memory_order_relaxed) != 0) continue;
            const uint32_t bytes = static_cast<uint32_t>(length + 1);
            const uint32_t offset = arenaUsed.load(std::memory_order_relaxed);
            const uint32_t id = nextId.load(std::memory_order_relaxed);
            if (offset > kUridArenaSize - bytes || id >= kUridSlots) return 0;
            std::memcpy(arena.data() + offset, uri, bytes);
            arenaUsed.store(offset + bytes, std::memory_order_relaxed);
            nextId.store(id + 1, std::memory_order_relaxed);
            slot.published.store(
                (static_cast<uint64_t>(id) << 32U) | offset,
                std::memory_order_release);
            return id;
        }
        return 0;
    }
    const char* unmap(LV2_URID id) const {
        if (id == 0) return nullptr;
        for (const UridSlot& slot : slots) {
            const uint64_t published = slot.published.load(std::memory_order_acquire);
            if (published != 0 && static_cast<uint32_t>(published >> 32U) == id) {
                return arena.data() + static_cast<uint32_t>(published);
            }
        }
        return nullptr;
    }
};
UridMapImpl& getGlobalUridMap() { static UridMapImpl instance; return instance; }
LV2_URID uridMapCallback(LV2_URID_Map_Handle h, const char* uri) {
    return static_cast<UridMapImpl*>(h)->map(uri);
}
const char* uridUnmapCallback(LV2_URID_Unmap_Handle h, LV2_URID id) {
    return static_cast<UridMapImpl*>(h)->unmap(id);
}
}
LV2_URID_Map globalLv2UridMap = { &getGlobalUridMap(), uridMapCallback };
LV2_URID_Unmap globalLv2UridUnmap = { &getGlobalUridMap(), uridUnmapCallback };
namespace {
LV2_Feature uridMapFeature = { LV2_URID__map, &globalLv2UridMap };
LV2_Feature uridUnmapFeature = { LV2_URID__unmap, &globalLv2UridUnmap };
}

#endif

namespace guitarrackcraft {

#if defined(HAVE_LV2) && HAVE_LV2 == 1

// Check if we support all required features of a plugin
static bool checkRequiredFeatures(
        const LilvPlugin* plugin, LilvWorld* world, bool workerScheduleAvailable) {
    static const char* supportedFeatures[] = {
        LV2_URID__map,
        LV2_URID__unmap,
        LV2_WORKER__schedule,
        LV2_OPTIONS__options,
        LV2_BUF_SIZE__boundedBlockLength,
        LV2_STATE__mapPath,
        LV2_STATE__freePath,
        nullptr
    };

    LilvNodes* required = lilv_plugin_get_required_features(plugin);
    if (!required) return true;

    bool ok = true;
    LILV_FOREACH(nodes, i, required) {
        const LilvNode* feat = lilv_nodes_get(required, i);
        const char* uri = feat ? lilv_node_as_uri(feat) : nullptr;
        if (!uri) {
            ok = false;
            continue;
        }
        bool found = false;
        for (const char** s = supportedFeatures; *s; ++s) {
            if (strcmp(uri, *s) == 0) { found = true; break; }
        }
        if (found && strcmp(uri, LV2_WORKER__schedule) == 0 &&
            !workerScheduleAvailable) {
            found = false;
        }
        if (!found) {
            LOGE("Plugin requires unsupported feature: %s", uri);
            ok = false;
        }
    }
    lilv_nodes_free(required);
    return ok;
}

void LV2Plugin::buildFeatures() {
    auto& uridMap = getGlobalUridMap();

    // Initialize atom forge
    lv2_atom_forge_init(&forge_, &globalLv2UridMap);

    // Map patch/atom URIDs
    // All atom/time URIDs are mapped during construction, never on the RT path.
    atom_Path_ = uridMap.map(LV2_ATOM__Path);
    atom_URID_ = uridMap.map(LV2_ATOM__URID);
    patch_Set_ = uridMap.map(LV2_PATCH__Set);
    patch_Get_ = uridMap.map(LV2_PATCH__Get);
    patch_property_ = uridMap.map(LV2_PATCH__property);
    patch_value_ = uridMap.map(LV2_PATCH__value);
    time_Position_ = uridMap.map(LV2_TIME__Position);
    time_frame_ = uridMap.map(LV2_TIME__frame);
    time_speed_ = uridMap.map(LV2_TIME__speed);
    time_beatsPerMinute_ = uridMap.map(LV2_TIME__beatsPerMinute);
    time_beatsPerBar_ = uridMap.map(LV2_TIME__beatsPerBar);
    time_beatUnit_ = uridMap.map(LV2_TIME__beatUnit);
    time_bar_ = uridMap.map(LV2_TIME__bar);
    time_barBeat_ = uridMap.map(LV2_TIME__barBeat);
    atom_Float_ = uridMap.map(LV2_ATOM__Float);
    atom_Long_ = uridMap.map(LV2_ATOM__Long);
    atom_Int_ = uridMap.map(LV2_ATOM__Int);
    atom_Double_ = uridMap.map(LV2_ATOM__Double);
    atom_Sequence_ = uridMap.map(LV2_ATOM__Sequence);
    midi_MidiEvent_ = uridMap.map(LV2_MIDI__MidiEvent);
    atom_Chunk_ = uridMap.map(LV2_ATOM__Chunk);

    // Options: provide buffer size info
    LV2_URID bufsz_max = uridMap.map(LV2_BUF_SIZE__maxBlockLength);
    LV2_URID bufsz_nom = uridMap.map(LV2_BUF_SIZE__nominalBlockLength);
    LV2_URID atom_Int = uridMap.map(LV2_ATOM__Int);

    // maxBlockLength_ is set by activate() — use actual callback buffer size when available.
    // Provide both maxBlockLength and nominalBlockLength (same value) because some plugins
    // (e.g. GxCabinet) prefer nominalBlockLength for convolver quantum configuration.
    options_[0] = {LV2_OPTIONS_INSTANCE, 0, bufsz_max, sizeof(int32_t), atom_Int, &maxBlockLength_};
    options_[1] = {LV2_OPTIONS_INSTANCE, 0, bufsz_nom, sizeof(int32_t), atom_Int, &maxBlockLength_};
    options_[2] = {LV2_OPTIONS_INSTANCE, 0, 0, 0, 0, nullptr}; // terminator

    // Worker schedule (handle = this)
    workerSchedule_ = {this, scheduleWorkCallback};
    workerScheduleFeature_ = {LV2_WORKER__schedule, &workerSchedule_};

    // Per-instance feature structs
    optionsFeature_ = {LV2_OPTIONS__options, options_};
    boundedBlockFeature_ = {LV2_BUF_SIZE__boundedBlockLength, nullptr};

    // State path mapping features
    mapPathData_.handle = this;
    mapPathData_.abstract_path = mapAbstractPathCallback;
    mapPathData_.absolute_path = mapAbsolutePathCallback;
    mapPathFeature_ = {LV2_STATE__mapPath, &mapPathData_};

    freePathData_.handle = this;
    freePathData_.free_path = freePathCallback;
    freePathFeature_ = {LV2_STATE__freePath, &freePathData_};

    // Build null-terminated features pointer array
    instanceFeatures_.clear();
    instanceFeatures_.push_back(&uridMapFeature);         // global
    instanceFeatures_.push_back(&uridUnmapFeature);       // global
    if (workerSemInitialized_.load(std::memory_order_acquire)) {
        instanceFeatures_.push_back(&workerScheduleFeature_);  // per-instance
    }
    instanceFeatures_.push_back(&optionsFeature_);         // per-instance
    instanceFeatures_.push_back(&boundedBlockFeature_);    // static
    instanceFeatures_.push_back(&mapPathFeature_);         // per-instance
    instanceFeatures_.push_back(&freePathFeature_);        // per-instance
    instanceFeatures_.push_back(nullptr);
}

LV2Plugin::LV2Plugin(const LilvPlugin* plugin,
                     std::shared_ptr<const LV2PluginGeneration> generation,
                     float sampleRate, const std::string& filesDir)
    : generation_(std::move(generation))
    , plugin_(plugin)
    , world_(generation_ ? generation_->world : nullptr)
    , instance_(nullptr)
    , sampleRate_(sampleRate)
    , isActive_(false)
    , filesDir_(filesDir)
{
    processSemInitialized_ = sem_init(&processDone_, 0, 0) == 0;
    workerSemInitialized_.store(
        sem_init(&workerWake_, 0, 0) == 0, std::memory_order_release);
    if (!processSemInitialized_) {
        LOGE("Failed to initialize LV2 process acknowledgement semaphore: %d", errno);
        return;
    }
    if (!plugin_ || !world_) {
        LOGE("Invalid plugin or world");
        return;
    }

    if (!checkRequiredFeatures(
            plugin_, world_, workerSemInitialized_.load(std::memory_order_acquire))) {
        LOGE("Plugin has unsupported required features, skipping instantiation");
        return;
    }
    if (LilvNodes* required = lilv_plugin_get_required_features(plugin_)) {
        LILV_FOREACH(nodes, i, required) {
            const LilvNode* feature = lilv_nodes_get(required, i);
            const char* uri = feature ? lilv_node_as_uri(feature) : nullptr;
            if (uri && std::strcmp(uri, LV2_WORKER__schedule) == 0)
                requiredWorker_ = true;
        }
        lilv_nodes_free(required);
    }

    buildFeatures();

    instance_ = lilv_plugin_instantiate(plugin_, sampleRate, instanceFeatures_.data());
    if (!instance_) {
        LOGE("Failed to instantiate LV2 plugin");
        return;
    }
    if (!initializePorts()) {
        LOGE("Failed to initialize LV2 ports");
        lilv_instance_free(instance_);
        instance_ = nullptr;
        return;
    }
    connectPorts();
    structuralValid_.store(true, std::memory_order_release);

    // Query state:interface extension
    const void* si = lilv_instance_get_extension_data(instance_, LV2_STATE__interface);
    stateInterface_ = static_cast<const LV2_State_Interface*>(si);
    if (stateInterface_) {
        LOGI("Plugin provides state:interface (save=%p restore=%p)",
             (void*)stateInterface_->save, (void*)stateInterface_->restore);
    }
}

LV2Plugin::~LV2Plugin() {
    deactivate();
    if (instance_) {
        if (!guitarrackcraft::isCreatingPluginUI()) {
            lilv_instance_free(instance_);
        }
        instance_ = nullptr;
    }
    if (workerSemInitialized_.exchange(false, std::memory_order_acq_rel))
        sem_destroy(&workerWake_);
    if (processSemInitialized_) sem_destroy(&processDone_);
}

void LV2Plugin::activate(float sampleRate, uint32_t bufferSize) {
    LOGI("activate: sampleRate=%.0f bufferSize=%u", sampleRate, bufferSize);
    structuralValid_.store(false, std::memory_order_release);
    deactivate();
    if (bufferSize > kMaxLv2BufferFrames || sampleRate <= 0.0f) {
        LOGE("activate: invalid sample rate or buffer size");
        return;
    }
    const uint32_t effectiveBlock = bufferSize ? bufferSize : static_cast<uint32_t>(kMaxLv2BufferFrames);
    latencyFrames_.store(0, std::memory_order_relaxed);
    sampleRate_ = sampleRate;
    maxBlockLength_ = static_cast<int32_t>(effectiveBlock);

    PluginState savedState;
    bool hasSavedState = false;
    if (instance_) {
        savedState = saveState();
        hasSavedState = !savedState.properties.empty() || !savedState.controlPortValues.empty();
        uint8_t expected = 1;
        if (processState_.compare_exchange_strong(expected, 2, std::memory_order_acq_rel) &&
            !waitForProcessAcknowledgement()) return;
        stopWorker();
        lilv_instance_free(instance_);
        instance_ = nullptr;
    }
    if (!checkRequiredFeatures(plugin_, world_,
            workerSemInitialized_.load(std::memory_order_acquire))) return;
    buildFeatures();
    instance_ = lilv_plugin_instantiate(plugin_, sampleRate, instanceFeatures_.data());
    if (!instance_) return;
    if (!initializePorts()) {
        lilv_instance_free(instance_);
        instance_ = nullptr;
        return;
    }
    connectPorts();
    stateInterface_ = static_cast<const LV2_State_Interface*>(
        lilv_instance_get_extension_data(instance_, LV2_STATE__interface));
    lilv_instance_activate(instance_);
    if (!startWorker() && requiredWorker_) {
        lilv_instance_deactivate(instance_);
        lilv_instance_free(instance_);
        instance_ = nullptr;
        return;
    }
    structuralValid_.store(true, std::memory_order_release);
    uridFaulted_.store(false, std::memory_order_release);
    isActive_.store(true, std::memory_order_release);
    if (hasSavedState) restoreState(savedState);
}

void LV2Plugin::deactivate() {
    latencyFrames_.store(0, std::memory_order_relaxed);
    if (!isActive_.exchange(false, std::memory_order_acq_rel)) return;
    uint8_t expected = 1;
    if (processState_.compare_exchange_strong(expected, 2, std::memory_order_acq_rel) &&
        !waitForProcessAcknowledgement()) return;
    stopWorker();
    if (instance_) lilv_instance_deactivate(instance_);
}

uint32_t LV2Plugin::process(const float* const* inputs, float* const* outputs, uint32_t numFrames,
                            const AudioProcessContext& context,
                            const MidiEvent* inputEvents, uint32_t inputCount,
                            MidiEvent* outputEvents, uint32_t outputCapacity) {
    uint8_t expectedState = 0;
    if (!processState_.compare_exchange_strong(expectedState, 1, std::memory_order_acq_rel))
        return 0;
    auto passthroughMidi = [&]() -> uint32_t {
        if (!outputEvents || outputCapacity == 0 || !inputEvents) return 0;
        uint32_t written = 0;
        const uint32_t limit = std::min(inputCount, outputCapacity);
        for (uint32_t i = 0; i < limit; ++i)
            if (inputEvents[i].frameOffset < numFrames) outputEvents[written++] = inputEvents[i];
        return written;
    };
    auto finishProcess = [this]() {
        const uint8_t prior = processState_.exchange(0, std::memory_order_acq_rel);
        if (prior == 2) sem_post(&processDone_);
    };
    uint32_t midiOutputCount = 0;
    const size_t maxCopy = std::min(static_cast<size_t>(numFrames), kMaxLv2BufferFrames);
    auto passthrough = [&]() {
        if (!inputs || !outputs) return;
        for (uint32_t ch = 0; ch < 2; ++ch)
            if (inputs[ch] && outputs[ch]) std::memcpy(outputs[ch], inputs[ch], numFrames * sizeof(float));
    };
    if (uridFaulted_.load(std::memory_order_acquire) ||
        !isActive_.load(std::memory_order_acquire) || !instance_ ||
        maxCopy == 0 || numFrames > static_cast<uint32_t>(maxBlockLength_)) {
        if (numFrames > static_cast<uint32_t>(maxBlockLength_)) quantumViolations_.fetch_add(1, std::memory_order_relaxed);
        passthrough();
        finishProcess();
        return passthroughMidi();
    }

    // Control ports are owned by the DSP instance while it runs.  Apply the
    // atomically published UI values here, at the block boundary, so no UI
    // thread ever races a plugin read of a float port.
    for (size_t i = 0; i < controlPorts_.size() && i < pendingControlPorts_.size(); ++i) {
        if (controlPorts_[i] && pendingControlPorts_[i] &&
            i < controlPortInputs_.size() && controlPortInputs_[i])
            *controlPorts_[i] = pendingControlPorts_[i]->load(std::memory_order_acquire);
    }
    for (size_t i = 0; i < audioInputPorts_.size() && i < 2; ++i) {
        if (inputs[i] && audioInputPorts_[i])
            std::memcpy(audioInputPorts_[i], inputs[i], maxCopy * sizeof(float));
    }
    for (auto& ap : atomPorts_) {
        auto* seq = reinterpret_cast<LV2_Atom_Sequence*>(atomPortBuffers_[ap.bufferIdx].data());
        seq->atom.type = ap.isInput ? atom_Sequence_ : atom_Chunk_;
        seq->atom.size = ap.isInput ? sizeof(LV2_Atom_Sequence_Body)
                                    : static_cast<uint32_t>(ap.capacity - sizeof(LV2_Atom));
    }
    if (inputEvents && inputCount > 0) {
        for (auto& ap : atomPorts_) {
            if (!ap.isInput || !ap.supportsMidi) continue;
            auto* seq = reinterpret_cast<LV2_Atom_Sequence*>(atomPortBuffers_[ap.bufferIdx].data());
            uint32_t used = seq->atom.size;
            const size_t bodyCapacity =
                ap.capacity - sizeof(LV2_Atom) - sizeof(LV2_Atom_Sequence_Body);
            for (uint32_t i = 0; i < inputCount; ++i) {
                const MidiEvent& midi = inputEvents[i];
                if (midi.frameOffset >= maxCopy) continue;
                const uint32_t kMidiBytes = (midi.status & 0xE0u) == 0xC0u ? 2u : 3u;
                const uint32_t eventBytes = sizeof(LV2_Atom_Event) + kMidiBytes;
                const uint32_t padded = (eventBytes + 7u) & ~uint32_t(7u);
                if (used < sizeof(LV2_Atom_Sequence_Body) ||
                    padded > bodyCapacity - (used - sizeof(LV2_Atom_Sequence_Body))) break;
                auto* event = reinterpret_cast<LV2_Atom_Event*>(
                    reinterpret_cast<uint8_t*>(&seq->body) + used);
                event->time.frames = midi.frameOffset;
                event->body.type = midi_MidiEvent_;
                event->body.size = kMidiBytes;
                uint8_t* data = reinterpret_cast<uint8_t*>(event) + sizeof(LV2_Atom_Event);
                data[0] = midi.status; data[1] = midi.data1;
                if (kMidiBytes == 3) data[2] = midi.data2;
                if (padded > eventBytes) std::memset(data + kMidiBytes, 0, padded - eventBytes);
                used += padded;
            }
            seq->atom.size = used;
        }
    }
    for (size_t drain = 0; drain < 8; ++drain) {
        const bool consumed = pendingAtoms_.consume([&](const uint8_t* atomData, size_t atomSize) {
            if (atomSize < sizeof(LV2_Atom)) return;
            const auto* src = reinterpret_cast<const LV2_Atom*>(atomData);
            if (src->size > atomSize - sizeof(LV2_Atom)) return;
            const uint64_t padded =
                    (sizeof(LV2_Atom_Event) + static_cast<uint64_t>(src->size) + 7u) & ~uint64_t(7u);
            for (auto& ap : atomPorts_) {
                if (!ap.isInput) continue;
                auto* seq =
                        reinterpret_cast<LV2_Atom_Sequence*>(atomPortBuffers_[ap.bufferIdx].data());
                const size_t used = seq->atom.size - sizeof(LV2_Atom_Sequence_Body);
                const size_t capacity =
                        ap.capacity - sizeof(LV2_Atom) - sizeof(LV2_Atom_Sequence_Body);
                if (seq->atom.size < sizeof(LV2_Atom_Sequence_Body) || padded > capacity - used) {
                    break;
                }
                auto* evt = reinterpret_cast<LV2_Atom_Event*>(
                        reinterpret_cast<uint8_t*>(&seq->body) + seq->atom.size);
                evt->time.frames = 0;
                evt->body = *src;
                std::memcpy(reinterpret_cast<uint8_t*>(evt) + sizeof(LV2_Atom_Event),
                            atomData + sizeof(LV2_Atom), src->size);
                seq->atom.size += static_cast<uint32_t>(padded);
                break;
            }
        });
        if (!consumed) break;
    }
    pendingFilePaths_.consume([&](const FilePathMessage& fileMsg) {
        for (auto& ap : atomPorts_) {
            if (!ap.isInput) continue;
            auto* buf = atomPortBuffers_[ap.bufferIdx].data();
            lv2_atom_forge_set_buffer(&forge_, buf, ap.capacity);
            LV2_Atom_Forge_Frame sf, of;
            lv2_atom_forge_sequence_head(&forge_, &sf, 0);
            lv2_atom_forge_frame_time(&forge_, 0);
            lv2_atom_forge_object(&forge_, &of, 0, patch_Set_);
            lv2_atom_forge_key(&forge_, patch_property_);
            lv2_atom_forge_urid(&forge_, fileMsg.propertyUrid);
            lv2_atom_forge_key(&forge_, patch_value_);
            lv2_atom_forge_path(&forge_, fileMsg.path, fileMsg.pathSize);
            lv2_atom_forge_pop(&forge_, &of);
            lv2_atom_forge_pop(&forge_, &sf);
            break;
        }
    });
    for (auto& ap : atomPorts_) {
        if (!ap.isInput) continue;
        auto* buf = atomPortBuffers_[ap.bufferIdx].data();
        auto* seq = reinterpret_cast<LV2_Atom_Sequence*>(buf);
        const uint32_t oldSize = seq->atom.size;
        constexpr uint32_t kTimeEventReserve = 512;
        if (oldSize < sizeof(LV2_Atom_Sequence_Body) ||
            oldSize > ap.capacity - sizeof(LV2_Atom) ||
            ap.capacity - sizeof(LV2_Atom) - oldSize < kTimeEventReserve) {
            timeEventDrops_.fetch_add(1, std::memory_order_relaxed);
            break;
        }

        auto* append = reinterpret_cast<uint8_t*>(&seq->body) + oldSize;
        auto* event = reinterpret_cast<LV2_Atom_Event*>(append);
        const uint32_t remaining = static_cast<uint32_t>(ap.capacity - sizeof(LV2_Atom) - oldSize);
        const uint32_t eventBodyOffset = static_cast<uint32_t>(
            reinterpret_cast<const uint8_t*>(&event->body) - reinterpret_cast<const uint8_t*>(append));
        if (remaining <= eventBodyOffset) {
            seq->atom.size = oldSize;
            timeEventDrops_.fetch_add(1, std::memory_order_relaxed);
            break;
        }
        event->time.frames = 0;
        lv2_atom_forge_set_buffer(&forge_,
                                  append + eventBodyOffset,
                                  remaining - eventBodyOffset);
        LV2_Atom_Forge_Frame of;
        lv2_atom_forge_object(&forge_, &of, 0, time_Position_);
        lv2_atom_forge_key(&forge_, time_frame_); lv2_atom_forge_long(&forge_, static_cast<int64_t>(context.transportFrame));
        lv2_atom_forge_key(&forge_, time_speed_); lv2_atom_forge_float(&forge_, context.playing ? 1.0f : 0.0f);
        lv2_atom_forge_key(&forge_, time_beatsPerMinute_); lv2_atom_forge_float(&forge_, static_cast<float>(context.beatsPerMinute));
        lv2_atom_forge_key(&forge_, time_beatsPerBar_); lv2_atom_forge_float(&forge_, context.beatsPerBar);
        lv2_atom_forge_key(&forge_, time_beatUnit_); lv2_atom_forge_int(&forge_, context.beatUnit);
        lv2_atom_forge_key(&forge_, time_bar_); lv2_atom_forge_long(&forge_, context.bar);
        lv2_atom_forge_key(&forge_, time_barBeat_); lv2_atom_forge_float(&forge_, static_cast<float>(context.barBeat));
        lv2_atom_forge_pop(&forge_, &of);

        const uint32_t eventBytes = eventBodyOffset + static_cast<uint32_t>(forge_.offset);
        const uint32_t padded = (eventBytes + 7u) & ~uint32_t(7u);
        if (forge_.offset > remaining - eventBodyOffset || padded > remaining) {
            seq->atom.size = oldSize;
            timeEventDrops_.fetch_add(1, std::memory_order_relaxed);
        } else {
            if (padded > eventBytes) std::memset(append + eventBytes, 0, padded - eventBytes);
            seq->atom.size = oldSize + padded;
        }
        break;
    }
    // delivered during this run and must be drained before end_run().
    const LV2_Handle workerHandle = lilv_instance_get_handle(instance_);
    gLv2RealtimeMapMiss = false;
    gLv2RealtimeMapContext = true;
    lilv_instance_run(instance_, static_cast<uint32_t>(maxCopy));
    if (workerInterface_ && workerInterface_->work_response) {
        for (size_t drain = 0; drain < 8 && workResponses_.consume([&](const WorkerMessage& response) {
            workerInterface_->work_response(workerHandle, response.size, response.data);
        });) {}
    }
    if (workerInterface_ && workerInterface_->end_run) workerInterface_->end_run(workerHandle);
    gLv2RealtimeMapContext = false;
    if (gLv2RealtimeMapMiss) {
        uridFaulted_.store(true, std::memory_order_release);
        passthrough();
        finishProcess();
        return passthroughMidi();
    }
    if (latencyControlPosition_ >= 0 &&
        static_cast<size_t>(latencyControlPosition_) < controlPorts_.size() &&
        controlPorts_[static_cast<size_t>(latencyControlPosition_)]) {
        const float reported = *controlPorts_[static_cast<size_t>(latencyControlPosition_)];
        if (std::isfinite(reported) && reported >= 0.0f) {
            constexpr float kMaxPdcFrames = 65535.0f;
            const float bounded = std::min(reported, kMaxPdcFrames);
            latencyFrames_.store(static_cast<uint32_t>(bounded), std::memory_order_relaxed);
        } else {
            latencyFrames_.store(0, std::memory_order_relaxed);
        }
    } else {
        latencyFrames_.store(0, std::memory_order_relaxed);
    }
    // Publish LV2 output controls only after run(), through atomics consumed by
    // UI/state readers. They are never written back into the DSP input ports.
    for (size_t i = 0; i < controlPorts_.size() && i < pendingControlPorts_.size(); ++i) {
        if (controlPorts_[i] && pendingControlPorts_[i] &&
            i < controlPortInputs_.size() && !controlPortInputs_[i]) {
            pendingControlPorts_[i]->store(*controlPorts_[i], std::memory_order_release);
        }
    }
    for (auto& ap : atomPorts_) {
        if (ap.isInput) continue;
        const auto* base = atomPortBuffers_[ap.bufferIdx].data();
        const auto* atom = reinterpret_cast<const LV2_Atom*>(base);
        const uint32_t total = atom->size;
        if (atom->type != atom_Sequence_ || total < sizeof(LV2_Atom_Sequence_Body) ||
            total > ap.capacity - sizeof(LV2_Atom)) continue;
        const uint8_t* pos = base + sizeof(LV2_Atom) + sizeof(LV2_Atom_Sequence_Body);
        const uint8_t* end = base + sizeof(LV2_Atom) + total;
        bool validSequence = true;
        while (pos + sizeof(LV2_Atom_Event) <= end) {
            const auto* ev = reinterpret_cast<const LV2_Atom_Event*>(pos);
            const uint64_t eventBytes = sizeof(LV2_Atom_Event) + static_cast<uint64_t>(ev->body.size);
            const uint64_t padded = (eventBytes + 7u) & ~uint64_t(7u);
            if (padded < sizeof(LV2_Atom_Event) || padded > static_cast<uint64_t>(end - pos)) {
                validSequence = false;
                break;
            }
            if (ev->body.type == midi_MidiEvent_ && ev->body.size >= 2 && ev->body.size <= 3 &&
                outputEvents && midiOutputCount < outputCapacity) {
                const auto* data = reinterpret_cast<const uint8_t*>(&ev->body) + sizeof(LV2_Atom);
                if ((data[0] & 0x80u) != 0)
                    outputEvents[midiOutputCount++] = MidiEvent{
                        static_cast<uint32_t>(ev->time.frames), data[0], data[1],
                        static_cast<uint8_t>(ev->body.size == 3 ? data[2] : 0)};
            }
            pos += padded;
        }
        // Queue one complete validated sequence per output atom port. The UI
        // thread splits it into OutputAtomEvent records after draining.
        const size_t sequenceBytes = sizeof(LV2_Atom) + static_cast<size_t>(total);
        const size_t queuedSize = sizeof(uint64_t) + sequenceBytes;
        if (validSequence && queuedSize <= pendingOutputAtoms_.payloadSize()) {
            if (!pendingOutputAtoms_.tryEmplace(queuedSize, [&](uint8_t* dst, size_t) {
                std::memcpy(dst, &ap.portIndex, sizeof(uint32_t));
                std::memset(dst + sizeof(uint32_t), 0, sizeof(uint32_t));
                std::memcpy(dst + sizeof(uint64_t), base, sequenceBytes);
            })) outputAtomDrops_.fetch_add(1, std::memory_order_relaxed);
        } else {
            outputAtomDrops_.fetch_add(1, std::memory_order_relaxed);
        }
    }
    for (size_t i = 0; i < audioOutputPorts_.size() && i < 2; ++i) {
        if (outputs[i] && audioOutputPorts_[i]) std::memcpy(outputs[i], audioOutputPorts_[i], maxCopy * sizeof(float));
    }
    if (audioOutputPorts_.size() == 1 && outputs[0] && outputs[1])
        std::memcpy(outputs[1], audioOutputPorts_[0], maxCopy * sizeof(float));
    finishProcess();
    if (midiOutputCount == 0) midiOutputCount = passthroughMidi();
    return midiOutputCount;
}

PluginInfo LV2Plugin::getInfo() const {
    PluginInfo info;
    
    if (!plugin_) {
        return info;
    }
    
    const LilvNode* uri = lilv_plugin_get_uri(plugin_);
    const LilvNode* name = lilv_plugin_get_name(plugin_);
    
    if (uri) {
        info.id = lilv_node_as_string(uri);
    }
    if (name) {
        info.name = lilv_node_as_string(name);
    }
    info.format = "LV2";
    
    // Get port information
    uint32_t numPorts = lilv_plugin_get_num_ports(plugin_);
    info.ports.reserve(numPorts);
    
    LilvNode* audioClass = lilv_new_uri(world_, LILV_URI_AUDIO_PORT);
    LilvNode* controlClass = lilv_new_uri(world_, LILV_URI_CONTROL_PORT);
    LilvNode* inputClass = lilv_new_uri(world_, LILV_URI_INPUT_PORT);
    LilvNode* outputClass = lilv_new_uri(world_, LILV_URI_OUTPUT_PORT);
    LilvNode* toggledClass = lilv_new_uri(world_, LV2_CORE__toggled);
    info.realtimeClass = (instance_ && structuralValid_.load(std::memory_order_acquire))
        ? RealtimeClass::CertifiedInProcess : RealtimeClass::Unsupported;
    for (uint32_t i = 0; i < numPorts; ++i) {
        const LilvPort* port = lilv_plugin_get_port_by_index(plugin_, i);
        if (!port) continue;
        
        PortInfo portInfo;
        portInfo.index = i;
        
        const LilvNode* portName = lilv_port_get_name(plugin_, port);
        const LilvNode* portSymbol = lilv_port_get_symbol(plugin_, port);
        
        if (portName) {
            portInfo.name = lilv_node_as_string(portName);
        }
        if (portSymbol) {
            portInfo.symbol = lilv_node_as_string(portSymbol);
        }
        
        // Check port classes
        bool isAudio = lilv_port_is_a(plugin_, port, audioClass);
        bool isControl = lilv_port_is_a(plugin_, port, controlClass);
        bool isInput = lilv_port_is_a(plugin_, port, inputClass);
        bool isOutput = lilv_port_is_a(plugin_, port, outputClass);
        
        portInfo.isAudio = isAudio;
        portInfo.isControl = isControl;
        portInfo.isInput = isInput;
        portInfo.isToggle = isControl && lilv_port_has_property(plugin_, port, toggledClass);
        
        // Get default, min, max values for control ports
        if (isControl) {
            LilvNode* defNode = nullptr;
            LilvNode* minNode = nullptr;
            LilvNode* maxNode = nullptr;
            lilv_port_get_range(plugin_, port, &defNode, &minNode, &maxNode);
            if (defNode) {
                portInfo.defaultValue = static_cast<float>(lilv_node_as_float(defNode));
                lilv_node_free(defNode);
            }
            if (minNode) {
                portInfo.minValue = static_cast<float>(lilv_node_as_float(minNode));
                lilv_node_free(minNode);
            }
            if (maxNode) {
                portInfo.maxValue = static_cast<float>(lilv_node_as_float(maxNode));
                lilv_node_free(maxNode);
            }
            // Scale points (enumeration values) for dropdown UI
            LilvScalePoints* points = lilv_port_get_scale_points(plugin_, port);
            if (points) {
                LILV_FOREACH(scale_points, it, points) {
                    const LilvScalePoint* sp = lilv_scale_points_get(points, it);
                    if (sp) {
                        ScalePoint spOut;
                        const LilvNode* labelNode = lilv_scale_point_get_label(sp);
                        const LilvNode* valueNode = lilv_scale_point_get_value(sp);
                        if (labelNode) {
                            spOut.label = lilv_node_as_string(labelNode);
                        }
                        if (valueNode) {
                            spOut.value = static_cast<float>(lilv_node_as_float(valueNode));
                        }
                        portInfo.scalePoints.push_back(spOut);
                    }
                }
                lilv_scale_points_free(points);
            }
        }

        info.ports.push_back(portInfo);
    }
    
    lilv_node_free(audioClass);
    lilv_node_free(controlClass);
    lilv_node_free(inputClass);
    lilv_node_free(outputClass);
    lilv_node_free(toggledClass);
    
    // Discover modgui (modgui.ttl + iconTemplate)
    discoverModguiMetadata(plugin_, info);

    // Discover X11 UI
    LilvNode* x11UiClass = lilv_new_uri(world_, "http://lv2plug.in/ns/extensions/ui#X11UI");
    if (x11UiClass) {
        LilvUIs* uis = lilv_plugin_get_uis(plugin_);
        if (uis) {
            LILV_FOREACH(uis, u, uis) {
                const LilvUI* ui = lilv_uis_get(uis, u);
                if (!lilv_ui_is_a(ui, x11UiClass)) continue;
                std::string binaryPath = resolveX11UIBinaryPath(ui, plugin_, world_);
                if (!binaryPath.empty()) {
                    info.hasX11Ui = true;
                    info.x11UiBinaryPath = binaryPath;
                    info.x11UiUri = lilv_node_as_string(lilv_ui_get_uri(ui));
                    const LilvNode* x11BundleUri = lilv_ui_get_bundle_uri(ui);
                    if (!x11BundleUri) {
                        x11BundleUri = lilv_plugin_get_bundle_uri(plugin_);
                    }
                    if (x11BundleUri) {
                        char* parsedX11BundlePath = lilv_file_uri_parse(lilv_node_as_string(x11BundleUri), nullptr);
                        if (parsedX11BundlePath) {
                            info.x11UiBundlePath = parsedX11BundlePath;
                            if (!info.x11UiBundlePath.empty() &&
                                info.x11UiBundlePath.back() != '/') {
                                info.x11UiBundlePath.push_back('/');
                            }
                            lilv_free(parsedX11BundlePath);
                        }
                    }
                    break;
                }
            }
            lilv_uis_free(uis);
        }
        lilv_node_free(x11UiClass);
    }
    
    return info;
}
    
void LV2Plugin::setParameter(uint32_t portIndex, float value) {
    // Publish input controls through an atomic mailbox. The audio callback
    // copies the value into the connected LV2 float port at block boundaries.
    for (size_t k = 0; k < controlPortIndices_.size(); ++k) {
        if (controlPortIndices_[k] == portIndex &&
            k < pendingControlPorts_.size() && pendingControlPorts_[k] &&
            k < controlPortInputs_.size() && controlPortInputs_[k]) {
            pendingControlPorts_[k]->store(value, std::memory_order_release);
            return;
        }
    }
}

float LV2Plugin::getParameter(uint32_t portIndex) const {
    for (size_t k = 0; k < controlPortIndices_.size(); ++k) {
        if (controlPortIndices_[k] == portIndex &&
            k < pendingControlPorts_.size() && pendingControlPorts_[k]) {
            return pendingControlPorts_[k]->load(std::memory_order_acquire);
        }
    }
    return 0.0f;
}


uint32_t LV2Plugin::getNumInputPorts() const {
    return static_cast<uint32_t>(audioInputPorts_.size());
}

uint32_t LV2Plugin::getNumOutputPorts() const {
    return static_cast<uint32_t>(audioOutputPorts_.size());
}

void LV2Plugin::setFilePath(
        const std::string& propertyUri, const std::string& path) {
    const LV2_URID propertyUrid = getGlobalUridMap().map(propertyUri.c_str());
    const uint32_t propertySize = static_cast<uint32_t>(
        std::min(propertyUri.size(), sizeof(FilePathMessage::property) - 1));
    const uint32_t pathSize = static_cast<uint32_t>(
        std::min(path.size() + 1, sizeof(FilePathMessage::path)));
    if (!pendingFilePaths_.tryEmplace([&](FilePathMessage& msg) {
            msg.propertyUrid = propertyUrid; msg.propertySize = propertySize; msg.pathSize = pathSize;
            std::memcpy(msg.property, propertyUri.data(), propertySize); msg.property[propertySize] = '\0';
            std::memcpy(msg.path, path.data(), pathSize ? pathSize - 1 : 0);
            msg.path[pathSize ? pathSize - 1 : 0] = '\0';
        })) filePathDrops_.fetch_add(1, std::memory_order_relaxed);
}

void LV2Plugin::injectAtom(const void* data, uint32_t size) {
    if (!data || size == 0 || size > pendingAtoms_.payloadSize()) return;
    if (!pendingAtoms_.tryEmplace(size, [&](uint8_t* dst, size_t) { std::memcpy(dst, data, size); }))
        pendingAtomDrops_.fetch_add(1, std::memory_order_relaxed);
}

std::vector<OutputAtomEvent> LV2Plugin::drainOutputAtoms() {
    constexpr size_t kMaxEventsPerTick = 64;
    constexpr size_t kMaxRecordsPerTick = 64;
    std::vector<OutputAtomEvent> result;
    result.reserve(kMaxEventsPerTick);
    size_t consumedRecords = 0;

    while (result.size() < kMaxEventsPerTick) {
        if (outputDrainRecord_.empty()) {
            if (consumedRecords >= kMaxRecordsPerTick) break;
            const bool consumed = pendingOutputAtoms_.consume(
                [&](const uint8_t* source, size_t size) {
                    outputDrainRecord_.assign(source, source + size);
                });
            if (!consumed) break;
            ++consumedRecords;
            if (outputDrainRecord_.size() < sizeof(uint64_t) + sizeof(LV2_Atom)) {
                outputDrainRecord_.clear();
                continue;
            }
            std::memcpy(&outputDrainPort_, outputDrainRecord_.data(), sizeof(outputDrainPort_));
            const auto* atom = reinterpret_cast<const LV2_Atom*>(
                outputDrainRecord_.data() + sizeof(uint64_t));
            const size_t available = outputDrainRecord_.size() - sizeof(uint64_t);
            if (atom->type != atom_Sequence_ ||
                atom->size < sizeof(LV2_Atom_Sequence_Body) ||
                atom->size > available - sizeof(LV2_Atom)) {
                outputDrainRecord_.clear();
                continue;
            }
            outputDrainOffset_ = sizeof(uint64_t) + sizeof(LV2_Atom) +
                                 sizeof(LV2_Atom_Sequence_Body);
        }

        const auto* atom = reinterpret_cast<const LV2_Atom*>(
            outputDrainRecord_.data() + sizeof(uint64_t));
        const size_t endOffset = sizeof(uint64_t) + sizeof(LV2_Atom) + atom->size;
        if (outputDrainOffset_ + sizeof(LV2_Atom_Event) > endOffset) {
            outputDrainRecord_.clear();
            outputDrainOffset_ = 0;
            continue;
        }
        const auto* event = reinterpret_cast<const LV2_Atom_Event*>(
            outputDrainRecord_.data() + outputDrainOffset_);
        const uint64_t eventBytes =
            sizeof(LV2_Atom_Event) + static_cast<uint64_t>(event->body.size);
        const uint64_t padded = (eventBytes + 7u) & ~uint64_t(7u);
        if (padded < sizeof(LV2_Atom_Event) ||
            padded > static_cast<uint64_t>(endOffset - outputDrainOffset_)) {
            outputDrainRecord_.clear();
            outputDrainOffset_ = 0;
            continue;
        }
        const size_t atomBytes = sizeof(LV2_Atom) + event->body.size;
        std::vector<uint8_t> data(atomBytes);
        std::memcpy(data.data(), &event->body, atomBytes);
        result.push_back(OutputAtomEvent{outputDrainPort_, std::move(data)});
        outputDrainOffset_ += static_cast<size_t>(padded);
        if (outputDrainOffset_ >= endOffset) {
            outputDrainRecord_.clear();
            outputDrainOffset_ = 0;
        }
    }
    return result;
}

void LV2Plugin::connectPorts() {
    if (!instance_ || !plugin_) {
        return;
    }

    uint32_t controlIdx = 0;
    uint32_t audioInputIdx = 0;
    uint32_t audioOutputIdx = 0;

    uint32_t numPorts = lilv_plugin_get_num_ports(plugin_);
    LilvNode* audioClass = lilv_new_uri(world_, LILV_URI_AUDIO_PORT);
    LilvNode* controlClass = lilv_new_uri(world_, LILV_URI_CONTROL_PORT);
    LilvNode* inputClass = lilv_new_uri(world_, LILV_URI_INPUT_PORT);

    for (uint32_t i = 0; i < numPorts; ++i) {
        const LilvPort* port = lilv_plugin_get_port_by_index(plugin_, i);
        if (!port) continue;

        bool isAudio = lilv_port_is_a(plugin_, port, audioClass);
        bool isControl = lilv_port_is_a(plugin_, port, controlClass);
        bool isInput = lilv_port_is_a(plugin_, port, inputClass);

        // Check if this is an atom port we have a buffer for
        bool connectedAtom = false;
        for (auto& ap : atomPorts_) {
            if (ap.portIndex == i) {
                lilv_instance_connect_port(instance_, i,
                    atomPortBuffers_[ap.bufferIdx].data());
                connectedAtom = true;
                break;
            }
        }
        if (connectedAtom) continue;

        if (isControl && controlIdx < controlPorts_.size()) {
            lilv_instance_connect_port(instance_, i, controlPorts_[controlIdx].get());
            controlIdx++;
        } else if (isAudio && isInput && audioInputIdx < audioInputPorts_.size()) {
            lilv_instance_connect_port(instance_, i, audioInputPorts_[audioInputIdx]);
            audioInputIdx++;
        } else if (isAudio && !isInput && audioOutputIdx < audioOutputPorts_.size()) {
            lilv_instance_connect_port(instance_, i, audioOutputPorts_[audioOutputIdx]);
            audioOutputIdx++;
        } else {
            lilv_instance_connect_port(instance_, i, nullptr);
        }
    }

    lilv_node_free(audioClass);
    lilv_node_free(controlClass);
    lilv_node_free(inputClass);
}

bool LV2Plugin::initializePorts() {
    if (!plugin_ || !world_) return false;
    controlPorts_.clear(); pendingControlPorts_.clear(); controlPortInputs_.clear(); controlPortIndices_.clear();
    latencyControlPosition_ = -1; audioInputBuffers_.clear(); audioOutputBuffers_.clear();
    audioInputPorts_.clear(); audioOutputPorts_.clear(); atomPortBuffers_.clear(); atomPorts_.clear();
    outputDrainRecord_.clear();
    outputDrainOffset_ = 0;
    outputDrainPort_ = 0;
    const uint32_t numPorts = lilv_plugin_get_num_ports(plugin_);
    LilvNode *audioClass=lilv_new_uri(world_,LILV_URI_AUDIO_PORT), *controlClass=lilv_new_uri(world_,LILV_URI_CONTROL_PORT);
    LilvNode *latencyDesignation=lilv_new_uri(world_,LV2_CORE__latency), *atomClass=lilv_new_uri(world_,LILV_URI_ATOM_PORT);
    LilvNode *inputClass=lilv_new_uri(world_,LILV_URI_INPUT_PORT), *outputClass=lilv_new_uri(world_,LILV_URI_OUTPUT_PORT);
    LilvNode *optionalClass=lilv_new_uri(world_,LV2_CORE__connectionOptional), *atomSupports=lilv_new_uri(world_,LV2_ATOM__supports);
    LilvNode *midiEventNode=lilv_new_uri(world_,LV2_MIDI__MidiEvent), *minimumSizeNode=lilv_new_uri(world_,LV2_RESIZE_PORT__minimumSize);
    const LilvPort* designated=lilv_plugin_get_port_by_designation(plugin_,controlClass,latencyDesignation);
    size_t maxCapacity=kAtomBufferSize; bool valid = numPorts != 0;
    for (uint32_t i=0;i<numPorts;++i) {
        const LilvPort* port=lilv_plugin_get_port_by_index(plugin_,i); if(!port) continue;
        const bool audio=lilv_port_is_a(plugin_,port,audioClass), control=lilv_port_is_a(plugin_,port,controlClass);
        const bool atom=lilv_port_is_a(plugin_,port,atomClass), input=lilv_port_is_a(plugin_,port,inputClass);
        const bool output=lilv_port_is_a(plugin_,port,outputClass);
        const bool optional=lilv_port_has_property(plugin_,port,optionalClass);
        const unsigned typeCount = static_cast<unsigned>(audio) +
            static_cast<unsigned>(control) + static_cast<unsigned>(atom);
        if (typeCount != 1 || input == output) {
            if (optional) continue;
            valid = false;
            break;
        }
        if(control) {
            float value=0.0f; LilvNode *d=nullptr,*mn=nullptr,*mx=nullptr; lilv_port_get_range(plugin_,port,&d,&mn,&mx);
            if(d){value=static_cast<float>(lilv_node_as_float(d));lilv_node_free(d);} if(mn)lilv_node_free(mn);if(mx)lilv_node_free(mx);
            const size_t p=controlPorts_.size(); controlPorts_.push_back(std::unique_ptr<float>(new float(value)));
            pendingControlPorts_.push_back(std::unique_ptr<std::atomic<float>>(new std::atomic<float>(value)));
            controlPortInputs_.push_back(input); controlPortIndices_.push_back(i); if(!input&&port==designated)latencyControlPosition_=static_cast<int32_t>(p);
        } else if(audio) {
            if (input && audioInputPorts_.size() >= 2) { valid = false; break; }
            if (!input && audioOutputPorts_.size() >= 2) { valid = false; break; }
            if(input){audioInputBuffers_.emplace_back(kMaxLv2BufferFrames,0.0f);audioInputPorts_.push_back(audioInputBuffers_.back().data());}
            else{audioOutputBuffers_.emplace_back(kMaxLv2BufferFrames,0.0f);audioOutputPorts_.push_back(audioOutputBuffers_.back().data());}
        } else if(atom) {
            size_t cap = kAtomBufferSize;
            LilvNode* minimumSize = lilv_port_get(plugin_, port, minimumSizeNode);
            if (minimumSize && lilv_node_is_int(minimumSize)) {
                const long minimum = lilv_node_as_int(minimumSize);
                if (minimum > 0) cap = static_cast<size_t>(minimum);
            }
            if (minimumSize) lilv_node_free(minimumSize);
            cap = std::max(cap, kAtomBufferSize);
            if (cap > 1024u * 1024u) { valid = false; break; }
            bool midi=false; LilvNodes* supported=lilv_port_get_value(plugin_,port,atomSupports);
            if(supported){LILV_FOREACH(nodes,si,supported)if(lilv_node_equals(lilv_nodes_get(supported,si),midiEventNode))midi=true;lilv_nodes_free(supported);}
            const size_t idx=atomPortBuffers_.size(); atomPortBuffers_.emplace_back(cap,0); atomPorts_.push_back({i,input,midi,idx,cap}); maxCapacity=std::max(maxCapacity,cap);
        }
    }
    lilv_node_free(minimumSizeNode);lilv_node_free(latencyDesignation);lilv_node_free(optionalClass);lilv_node_free(atomSupports);lilv_node_free(midiEventNode);lilv_node_free(controlClass);lilv_node_free(atomClass);lilv_node_free(outputClass);lilv_node_free(inputClass);lilv_node_free(audioClass);
    if(!valid){atomPortBuffers_.clear();atomPorts_.clear();return false;}
    // Keep descriptor capacity independent of payload-byte storage.
    const size_t slots = std::max<size_t>(256, kUiQueueCapacity);
    pendingAtoms_.reset(slots, maxCapacity);
    pendingOutputAtoms_.reset(slots, maxCapacity + sizeof(uint64_t));
    LOGI("initializePorts: control=%zu audioIn=%zu audioOut=%zu atom=%zu",
         controlPorts_.size(), audioInputPorts_.size(), audioOutputPorts_.size(), atomPorts_.size());
    return true;
}

// ---------- State path mapping ----------
char* LV2Plugin::mapAbstractPathCallback(LV2_State_Map_Path_Handle handle,const char* absolutePath) {
    auto*self=static_cast<LV2Plugin*>(handle); std::string abs(absolutePath?absolutePath:""),result;
    if(!self->filesDir_.empty()&&abs.size()>self->filesDir_.size()+1&&abs.compare(0,self->filesDir_.size(),self->filesDir_)==0&&abs[self->filesDir_.size()]=='/') result=abs.substr(self->filesDir_.size()+1); else result=abs;
    char*ret=static_cast<char*>(malloc(result.size()+1));if(ret)memcpy(ret,result.c_str(),result.size()+1);return ret;
}

char* LV2Plugin::mapAbsolutePathCallback(LV2_State_Map_Path_Handle handle, const char* abstractPath) {
    auto* self = static_cast<LV2Plugin*>(handle);
    std::string abst(abstractPath ? abstractPath : "");
    std::string result;

    // If relative, prepend filesDir
    if (!abst.empty() && abst[0] != '/') {
        result = self->filesDir_ + "/" + abst;
    } else {
        result = abst;
    }

    char* ret = static_cast<char*>(malloc(result.size() + 1));
    if (ret) memcpy(ret, result.c_str(), result.size() + 1);
    return ret;
}

void LV2Plugin::freePathCallback(LV2_State_Free_Path_Handle /*handle*/, char* path) {
    free(path);
}

// ---------- State save/restore ----------

PluginState LV2Plugin::saveState() {
    PluginState state;
    if (!plugin_) return state;

    // Plugin URI
    const LilvNode* uri = lilv_plugin_get_uri(plugin_);
    if (uri) state.pluginUri = lilv_node_as_string(uri);

    // Control port values
    for (size_t k = 0; k < pendingControlPorts_.size(); ++k) {
        if (pendingControlPorts_[k] && k < controlPortIndices_.size()) {
            state.controlPortValues.emplace_back(
                controlPortIndices_[k],
                pendingControlPorts_[k]->load(std::memory_order_acquire));
        }
    }

    // State properties via state:interface
    if (stateInterface_ && stateInterface_->save && instance_) {
        struct SaveContext {
            std::vector<StateProperty>* props;
            UridMapImpl* uridMap;
        };
        SaveContext ctx{&state.properties, &getGlobalUridMap()};

        auto storeCallback = [](LV2_State_Handle handle, uint32_t key,
                                const void* value, size_t size,
                                uint32_t type, uint32_t flags) -> LV2_State_Status {
            auto* c = static_cast<SaveContext*>(handle);
            StateProperty prop;
            const char* keyStr = c->uridMap->unmap(key);
            const char* typeStr = c->uridMap->unmap(type);
            if (!keyStr) return LV2_STATE_ERR_UNKNOWN;
            prop.keyUri = keyStr;
            prop.typeUri = typeStr ? typeStr : "";
            prop.flags = flags;
            if (value && size > 0) {
                auto* bytes = static_cast<const uint8_t*>(value);
                prop.value.assign(bytes, bytes + size);
            }
            c->props->push_back(std::move(prop));
            return LV2_STATE_SUCCESS;
        };

        const LV2_Feature* stateFeatures[] = {
            &mapPathFeature_, &freePathFeature_, nullptr
        };

        LV2_Handle lv2Handle = lilv_instance_get_handle(instance_);
        LV2_State_Status status = stateInterface_->save(
            lv2Handle, storeCallback, &ctx,
            LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE, stateFeatures);

        LOGI("saveState: %zu properties, status=%d", state.properties.size(), status);
    }

    return state;
}

bool LV2Plugin::restoreState(const PluginState& state) {
    if (!instance_) return false;

    // Restore control port values
    for (const auto& [portIndex, value] : state.controlPortValues) {
        setParameter(portIndex, value);
    }

    // Restore state properties via state:interface
    if (stateInterface_ && stateInterface_->restore && !state.properties.empty()) {
        struct RestoreContext {
            const std::vector<StateProperty>* props;
            UridMapImpl* uridMap;
        };
        RestoreContext ctx{&state.properties, &getGlobalUridMap()};

        auto retrieveCallback = [](LV2_State_Handle handle, uint32_t key,
                                   size_t* size, uint32_t* type,
                                   uint32_t* flags) -> const void* {
            auto* c = static_cast<RestoreContext*>(handle);
            const char* keyStr = c->uridMap->unmap(key);
            if (!keyStr) return nullptr;

            for (const auto& prop : *c->props) {
                if (prop.keyUri == keyStr) {
                    if (size) *size = prop.value.size();
                    if (type) *type = c->uridMap->map(prop.typeUri.c_str());
                    if (flags) *flags = prop.flags;
                    return prop.value.empty() ? nullptr : prop.value.data();
                }
            }
            return nullptr;
        };

        const LV2_Feature* stateFeatures[] = {
            &mapPathFeature_, &freePathFeature_, nullptr
        };

        LV2_Handle lv2Handle = lilv_instance_get_handle(instance_);
        LV2_State_Status status = stateInterface_->restore(
            lv2Handle, retrieveCallback, &ctx, 0, stateFeatures);

        LOGI("restoreState: %zu properties, status=%d", state.properties.size(), status);
        return status == LV2_STATE_SUCCESS;
    }

    return true;
}

// ---------- Worker extension ----------

bool LV2Plugin::waitForProcessAcknowledgement() noexcept {
    if (!processSemInitialized_) return false;
    int result;
    do {
        result = sem_wait(&processDone_);
    } while (result != 0 && errno == EINTR);
    if (result != 0) {
        LOGE("LV2 process acknowledgement failed: %d", errno);
        return false;
    }
    return true;
}

bool LV2Plugin::startWorker() {
    if (!instance_) return false;
    const void* iface = lilv_instance_get_extension_data(instance_, LV2_WORKER__interface);
    workerInterface_ = static_cast<const LV2_Worker_Interface*>(iface);
    if (!workerInterface_) return !requiredWorker_;
    LOGI("Starting worker thread (work=%p work_response=%p end_run=%p)",
         (void*)workerInterface_->work, (void*)workerInterface_->work_response,
         (void*)workerInterface_->end_run);
    if (!workerInterface_->work || !workerInterface_->work_response ||
        !workerInterface_->end_run ||
        !workerSemInitialized_.load(std::memory_order_acquire)) {
        workerInterface_ = nullptr;
        return false;
    }
    workerRunning_.store(true, std::memory_order_release);
    workerThread_ = std::thread(&LV2Plugin::workerThreadFunc, this);
    return true;
}

void LV2Plugin::stopWorker() {
    if (!workerRunning_.load(std::memory_order_acquire)) return;
    workerRunning_.store(false, std::memory_order_release);
    if (workerSemInitialized_.load(std::memory_order_acquire)) sem_post(&workerWake_);
    if (workerThread_.joinable()) workerThread_.join();
    workRequests_.clear();
    workResponses_.clear();
    workerInterface_ = nullptr;
}

void LV2Plugin::workerThreadFunc() {
    auto processOne = [this]() {
        return workRequests_.consume([this](const WorkerMessage& msg) {
            if (msg.size > 0 && workerInterface_ && instance_)
                workerInterface_->work(lilv_instance_get_handle(instance_), respondCallback,
                                       this, msg.size, msg.data);
        });
    };
    while (workerRunning_.load(std::memory_order_acquire)) {
        if (processOne()) continue;
        int result;
        do {
            result = sem_wait(&workerWake_);
        } while (result != 0 && errno == EINTR);
        if (result != 0) break;
    }
    while (processOne()) {}
}

LV2_Worker_Status LV2Plugin::scheduleWorkCallback(
    LV2_Worker_Schedule_Handle handle, uint32_t size, const void* data) {
    auto* self = static_cast<LV2Plugin*>(handle);
    if (size > kWorkerPayloadSize || (!data && size != 0)) return LV2_WORKER_ERR_NO_SPACE;
    if (!self->workRequests_.tryEmplace([&](WorkerMessage& msg) {
        msg.size = size;
        if (size) std::memcpy(msg.data, data, size);
    })) {
        self->workRequestDrops_.fetch_add(1, std::memory_order_relaxed);
        return LV2_WORKER_ERR_NO_SPACE;
    }
    if (self->workerSemInitialized_.load(std::memory_order_acquire)) sem_post(&self->workerWake_);
    return LV2_WORKER_SUCCESS;
}

LV2_Worker_Status LV2Plugin::respondCallback(
    LV2_Worker_Respond_Handle handle, uint32_t size, const void* data) {
    auto* self = static_cast<LV2Plugin*>(handle);
    if (size > kWorkerPayloadSize || (!data && size != 0))
        return LV2_WORKER_ERR_NO_SPACE;
    if (!self->workResponses_.tryEmplace([&](WorkerMessage& msg) {
            msg.size = size;
            if (size) std::memcpy(msg.data, data, size);
        })) {
        self->workResponseDrops_.fetch_add(1, std::memory_order_relaxed);
        return LV2_WORKER_ERR_NO_SPACE;
    }
    return LV2_WORKER_SUCCESS;
}

#else // HAVE_LV2 not defined or == 0 - stub implementation

LV2Plugin::LV2Plugin(LilvPlugin_* plugin, LilvWorld_* world, float sampleRate, const std::string& /*filesDir*/)
    : plugin_(plugin)
    , world_(world)
    , instance_(nullptr)
    , sampleRate_(sampleRate)
    , isActive_(false)
{
    LOGE("LV2Plugin: LV2 libraries not available (stub mode)");
}

LV2Plugin::~LV2Plugin() {
    deactivate();
}

void LV2Plugin::activate(float sampleRate, uint32_t /*bufferSize*/) {
    latencyFrames_.store(0, std::memory_order_relaxed);
    sampleRate_ = sampleRate;
    isActive_ = true;
}

void LV2Plugin::deactivate() {
    latencyFrames_.store(0, std::memory_order_relaxed);
    isActive_ = false;
}

uint32_t LV2Plugin::process(const float* const* inputs, float* const* outputs, uint32_t numFrames,
                            const AudioProcessContext& /*context*/,
                            const MidiEvent* inputEvents, uint32_t inputCount,
                            MidiEvent* outputEvents, uint32_t outputCapacity) {
    if (inputs && outputs && numFrames > 0) {
        for (uint32_t ch = 0; ch < 2; ++ch) {
            if (inputs[ch] && outputs[ch])
                std::memcpy(outputs[ch], inputs[ch], numFrames * sizeof(float));
        }
    }
    if (!outputEvents || !inputEvents) return 0;
    uint32_t count = 0;
    for (uint32_t i = 0; i < inputCount && count < outputCapacity; ++i)
        if (inputEvents[i].frameOffset < numFrames) outputEvents[count++] = inputEvents[i];
    return count;
}
PluginInfo LV2Plugin::getInfo() const {
    PluginInfo info;
    info.format = "LV2";
    return info;
}

void LV2Plugin::setParameter(uint32_t portIndex, float value) {
    // Stub
}

float LV2Plugin::getParameter(uint32_t portIndex) const {
    return 0.0f;
}

uint32_t LV2Plugin::getNumInputPorts() const {
    return 0;
}

uint32_t LV2Plugin::getNumOutputPorts() const {
    return 0;
}

void LV2Plugin::setFilePath(const std::string& propertyUri, const std::string& path) {
    // Stub
}

void LV2Plugin::connectPorts() {
    // Stub
}

bool LV2Plugin::initializePorts() {
    return false;
}

#endif // HAVE_LV2 == 1

} // namespace guitarrackcraft
