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

#ifndef GUITARRACKCRAFT_PLUGIN_CHAIN_H
#define GUITARRACKCRAFT_PLUGIN_CHAIN_H

#include <array>
#include <atomic>
#include <cstdint>
#include <condition_variable>
#include <deque>
#include <type_traits>
#include <utility>
#include <vector>
#include <memory>
#include <shared_mutex>
#include <mutex>
#include <thread>
#include "IPlugin.h"

namespace guitarrackcraft {

struct PluginSlot {
    uint64_t instanceId;
    std::unique_ptr<IPlugin> plugin;
};

class PluginChain {
public:
    PluginChain() = default;
    ~PluginChain();

    int addPlugin(std::unique_ptr<IPlugin> plugin, int position = -1);
    bool removePlugin(int index);
    bool reorderPlugins(int fromIndex, int toIndex);

    uint32_t process(const float* const* inputs, float* const* outputs, uint32_t numFrames,
                     const AudioProcessContext& context,
                     const MidiEvent* inputEvents, uint32_t inputCount,
                     MidiEvent* outputEvents, uint32_t outputCapacity);
    bool isEmptyForAudio() const noexcept {
        return pluginCount_.load(std::memory_order_acquire) == 0;
    }

    void setSampleRate(float sampleRate, uint32_t bufferSize = 0);
    void activate();
    void deactivate();

    size_t getSize() const;
    IPlugin* getPlugin(int index);
    uint32_t getLatencyFrames() const noexcept {
        return latencyFrames_.load(std::memory_order_acquire);
    }

    // Latency is published by the render thread and is intentionally independent
    // of chainMutex_, so graph alignment can observe it without blocking audio.

    uint64_t getPluginInstanceId(int index) const;
    void setParameter(int pluginIndex, uint32_t portIndex, float value);
    float getParameter(int pluginIndex, uint32_t portIndex) const;

    template <typename Callback>
    decltype(auto) visitPlugin(size_t index, Callback&& callback) {
        std::shared_lock lock(chainMutex_);
        if (index >= plugins_.size()) {
            using Result = std::invoke_result_t<Callback, IPlugin&>;
            if constexpr (std::is_void_v<Result>) return;
            else return Result{};
        }
        return std::forward<Callback>(callback)(*plugins_[index].plugin);
    }
    template <typename Callback>
    decltype(auto) visitPlugin(size_t index, Callback&& callback) const {
        std::shared_lock lock(chainMutex_);
        if (index >= plugins_.size()) {
            using Result = std::invoke_result_t<Callback, const IPlugin&>;
            if constexpr (std::is_void_v<Result>) return;
            else return Result{};
        }
        return std::forward<Callback>(callback)(*plugins_[index].plugin);
    }

    template <typename Callback>
    bool visitPluginInstance(uint64_t instanceId, Callback&& callback) {
        std::shared_lock lock(chainMutex_);
        for (auto& slot : plugins_) {
            if (slot.instanceId == instanceId) {
                std::forward<Callback>(callback)(*slot.plugin);
                return true;
            }
        }
        return false;
    }

    void setPluginFilePath(int pluginIndex, const std::string& propertyUri, const std::string& path);

    /** Inject an atom message into a plugin (thread-safe, holds shared_lock). */
    void injectAtom(int pluginIndex, const void* data, uint32_t size);

    /** Save state of all plugins, including every control port value. */
    struct ChainState { std::vector<PluginState> plugins; };
    ChainState saveChainState();
    bool restorePluginState(int index, const PluginState& state);
    /** Expose chain mutex so UI code can take a shared_lock during port reads. */
    std::shared_mutex* getChainMutex() { return &chainMutex_; }

private:
    std::vector<PluginSlot> plugins_;
    mutable std::shared_mutex chainMutex_;
    std::atomic<uint32_t> pluginCount_{0};
    std::atomic<uint32_t> latencyFrames_{0};

    float sampleRate_ = 0.0f;
    uint32_t bufferSize_ = 0;

    // Render-owned scratch buffers are sized only during lifecycle setup. Structural
    static constexpr uint32_t kMaxMidiEvents = 128;
    std::array<MidiEvent, kMaxMidiEvents> midiScratchA_{};
    std::array<MidiEvent, kMaxMidiEvents> midiScratchB_{};
    // control mutations never resize or otherwise touch these buffers.
    std::vector<std::vector<float>> intermediateBuffers_;
    uint32_t renderBufferSize_ = 0;

    void ensureBuffers(uint32_t numFrames, uint32_t numChannels);
    void clearOutputs(float* const* outputs, uint32_t numFrames) const noexcept;

    std::deque<std::unique_ptr<IPlugin>> teardownQueue_;
    std::mutex teardownMutex_;
    std::condition_variable teardownCond_;
    std::thread teardownThread_;
    bool teardownStop_ = false;

    void enqueueTeardown(std::unique_ptr<IPlugin> plugin);
    void teardownLoop();
};

} // namespace guitarrackcraft

#endif // GUITARRACKCRAFT_PLUGIN_CHAIN_H
