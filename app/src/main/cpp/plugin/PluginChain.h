/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * This file is part of Guitar RackCraft.
 *
 * Guitar RackCraft is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Guitar RackCraft is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Guitar RackCraft. If not, see <https://www.gnu.org/licenses/>.
 */

#ifndef GUITARRACKCRAFT_PLUGIN_CHAIN_H
#define GUITARRACKCRAFT_PLUGIN_CHAIN_H

#include <atomic>
#include <cstdint>
#include <condition_variable>
#include <deque>
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

    void process(const float* const* inputs, float* const* outputs, uint32_t numFrames,
                 const AudioProcessContext& context);
    bool isEmptyForAudio() const noexcept {
        return pluginCount_.load(std::memory_order_acquire) == 0;
    }

    void setSampleRate(float sampleRate, uint32_t bufferSize = 0);
    void activate();
    void deactivate();

    size_t getSize() const;
    IPlugin* getPlugin(int index);
    uint64_t getPluginInstanceId(int index) const;
    void setParameter(int pluginIndex, uint32_t portIndex, float value);
    float getParameter(int pluginIndex, uint32_t portIndex) const;

    void setPluginFilePath(int pluginIndex, const std::string& propertyUri, const std::string& path);

    /** Inject an atom message into a plugin (thread-safe, holds shared_lock). */
    void injectAtom(int pluginIndex, const void* data, uint32_t size);

    /** Save state of all plugins in the chain. */
    struct ChainState { std::vector<PluginState> plugins; };
    ChainState saveChainState();

    /** Restore state for a single plugin by index. Takes exclusive lock. */
    bool restorePluginState(int index, const PluginState& state);

    /** Expose chain mutex so UI code can take a shared_lock during port reads. */
    std::shared_mutex* getChainMutex() { return &chainMutex_; }

private:
    std::vector<PluginSlot> plugins_;
    mutable std::shared_mutex chainMutex_;
    std::atomic<uint32_t> pluginCount_{0};

    float sampleRate_ = 0.0f;
    uint32_t bufferSize_ = 0;

    // Render-owned scratch buffers are sized only during lifecycle setup. Structural
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
