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

#include "PluginChain.h"
#include <liblowlatencyaudio/ThreadUtils.h>
#include <android/log.h>
#include <algorithm>
#include <chrono>
#include <atomic>
#include <cstring>

#define LOG_TAG "PluginChain"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

namespace {
std::atomic<uint64_t> gNextPluginInstanceId{1};

uint64_t nextPluginInstanceId() {
    const uint64_t id = gNextPluginInstanceId.fetch_add(1, std::memory_order_relaxed);
    return id == 0 ? gNextPluginInstanceId.fetch_add(1, std::memory_order_relaxed) : id;
}

void copyInputChannels(const float* const* inputs, float* const* outputs,
                       uint32_t numFrames) noexcept {
    if (!outputs || numFrames == 0) {
        return;
    }
    for (uint32_t channel = 0; channel < 2; ++channel) {
        if (!outputs[channel]) {
            continue;
        }
        if (inputs && inputs[channel]) {
            if (outputs[channel] != inputs[channel]) {
                std::memcpy(
                    outputs[channel], inputs[channel],
                    numFrames * sizeof(float));
            }
        } else {
            std::memset(outputs[channel], 0, numFrames * sizeof(float));
        }
    }
}
} // namespace

namespace guitarrackcraft {

PluginChain::~PluginChain() {
    {
        std::lock_guard lock(teardownMutex_);
        teardownStop_ = true;
    }
    teardownCond_.notify_one();
    if (teardownThread_.joinable()) {
        teardownThread_.join();
    }
}

int PluginChain::addPlugin(std::unique_ptr<IPlugin> plugin, int position) {
    if (!plugin) {
        return -1;
    }

    bool pluginActivated = false;
    float activatedSampleRate = 0.0f;
    uint32_t activatedBufferSize = 0;

    while (true) {
        float targetSampleRate = 0.0f;
        uint32_t targetBufferSize = 0;
        {
            std::shared_lock lock(chainMutex_);
            targetSampleRate = sampleRate_;
            targetBufferSize = bufferSize_;
        }

        // VST activation may start Wine and wait for the guest process. Keep
        // that outside the exclusive chain lock so the live audio callback
        // keeps running the existing chain until this plugin is ready.
        if (targetSampleRate > 0.0f &&
            (!pluginActivated ||
             activatedSampleRate != targetSampleRate ||
             activatedBufferSize != targetBufferSize)) {
            if (pluginActivated) {
                plugin->deactivate();
            }
            plugin->activate(targetSampleRate, targetBufferSize);
            pluginActivated = true;
            activatedSampleRate = targetSampleRate;
            activatedBufferSize = targetBufferSize;
        }

        std::unique_lock lock(chainMutex_);
        if (sampleRate_ > 0.0f &&
            (!pluginActivated ||
             activatedSampleRate != sampleRate_ ||
             activatedBufferSize != bufferSize_)) {
            continue;
        }

        int index;
        if (position < 0 || position >= static_cast<int>(plugins_.size())) {
            plugins_.push_back({nextPluginInstanceId(), std::move(plugin)});
            index = static_cast<int>(plugins_.size() - 1);
        } else {
            plugins_.insert(
                plugins_.begin() + position,
                {nextPluginInstanceId(), std::move(plugin)});
            index = position;
        }

        pluginCount_.store(
            static_cast<uint32_t>(plugins_.size()),
            std::memory_order_release);
        LOGI("addPlugin: index=%d sampleRate=%.0f", index, sampleRate_);
        return index;
    }
}

bool PluginChain::removePlugin(int index) {
    std::unique_ptr<IPlugin> removedPlugin;
    {
        std::unique_lock lock(chainMutex_);

        if (index < 0 || index >= static_cast<int>(plugins_.size())) {
            return false;
        }

        removedPlugin = std::move(plugins_[index].plugin);
        plugins_.erase(plugins_.begin() + index);
        pluginCount_.store(
            static_cast<uint32_t>(plugins_.size()),
            std::memory_order_release);
    }

    // Wine VST teardown can block while the helper process exits. Detach first
    // so the rack/UI can continue immediately, then drain teardown in the
    // background while the plugin object is still owned by PluginChain.
    enqueueTeardown(std::move(removedPlugin));
    return true;
}

void PluginChain::enqueueTeardown(std::unique_ptr<IPlugin> plugin) {
    if (!plugin) {
        return;
    }

    {
        std::lock_guard lock(teardownMutex_);
        if (teardownStop_) {
            plugin->deactivate();
            return;
        }

        teardownQueue_.push_back(std::move(plugin));
        if (!teardownThread_.joinable()) {
            teardownThread_ = std::thread(&PluginChain::teardownLoop, this);
        }
    }
    teardownCond_.notify_one();
}

void PluginChain::teardownLoop() {
    LOGI("teardownLoop: started tid=%ld", getTid());
    while (true) {
        std::unique_ptr<IPlugin> plugin;
        {
            std::unique_lock lock(teardownMutex_);
            teardownCond_.wait(lock, [this] {
                return teardownStop_ || !teardownQueue_.empty();
            });

            if (teardownQueue_.empty()) {
                if (teardownStop_) {
                    break;
                }
                continue;
            }

            plugin = std::move(teardownQueue_.front());
            teardownQueue_.pop_front();
        }

        LOGI("teardownLoop: deactivating removed plugin tid=%ld", getTid());
        plugin->deactivate();
        LOGI("teardownLoop: removed plugin deactivated");
    }
    LOGI("teardownLoop: stopped tid=%ld", getTid());
}

bool PluginChain::reorderPlugins(int fromIndex, int toIndex) {
    std::unique_lock lock(chainMutex_);
    
    if (fromIndex < 0 || fromIndex >= static_cast<int>(plugins_.size()) ||
        toIndex < 0 || toIndex >= static_cast<int>(plugins_.size()) ||
        fromIndex == toIndex) {
        return false;
    }

    auto plugin = std::move(plugins_[fromIndex]);
    plugins_.erase(plugins_.begin() + fromIndex);
    
    plugins_.insert(plugins_.begin() + toIndex, std::move(plugin));
    
    return true;
}

void PluginChain::process(const float* const* inputs, float* const* outputs, uint32_t numFrames,
                          const AudioProcessContext& context) {
    // Structural control must never block the callback. On contention, keep
    // live audio flowing directly; do not touch buffers owned by the writer.
    std::shared_lock lock(chainMutex_, std::try_to_lock);
    if (!lock.owns_lock()) {
        copyInputChannels(inputs, outputs, numFrames);
        return;
    }

    // An oversized callback cannot be processed by lifecycle-sized buffers.
    // Preserve the allocation-free dry passthrough behavior.
    if (numFrames == 0) {
        return;
    }
    if (numFrames > renderBufferSize_) {
        copyInputChannels(inputs, outputs, numFrames);
        return;
    }
    if (!outputs || !outputs[0] || !outputs[1]) {
        clearOutputs(outputs, numFrames);
        return;
    }

    if (plugins_.empty()) {
        if (inputs && inputs[0] && inputs[1]) {
            if (outputs[0] != inputs[0]) {
                std::memcpy(
                    outputs[0], inputs[0], numFrames * sizeof(float));
            }
            if (outputs[1] != inputs[1]) {
                std::memcpy(
                    outputs[1], inputs[1], numFrames * sizeof(float));
            }
        } else {
            clearOutputs(outputs, numFrames);
        }
    } else {
        // Process through chain. Intermediate storage was allocated by
        // setSampleRate and is immutable for the lifetime of this render.
        if (!inputs || !inputs[0] || !inputs[1] ||
            intermediateBuffers_.size() < 2 ||
            intermediateBuffers_[0].size() < numFrames ||
            intermediateBuffers_[1].size() < numFrames) {
            clearOutputs(outputs, numFrames);
            return;
        }

        const float* currentInputs[2] = {inputs[0], inputs[1]};
        float* currentOutputs[2] = {nullptr, nullptr};
        for (size_t i = 0; i < plugins_.size(); ++i) {
            auto& plugin = plugins_[i].plugin;
            if (i == plugins_.size() - 1) {
                currentOutputs[0] = outputs[0];
                currentOutputs[1] = outputs[1];
            } else {
                currentOutputs[0] = intermediateBuffers_[0].data();
                currentOutputs[1] = intermediateBuffers_[1].data();
            }
            plugin->process(currentInputs, currentOutputs, numFrames, context);
            if (i < plugins_.size() - 1) {
                currentInputs[0] = intermediateBuffers_[0].data();
                currentInputs[1] = intermediateBuffers_[1].data();
            }
        }
    }

}

void PluginChain::setSampleRate(float sampleRate, uint32_t bufferSize) {
    std::unique_lock lock(chainMutex_);
    sampleRate_ = sampleRate;
    bufferSize_ = bufferSize;
    renderBufferSize_ = bufferSize_;
    ensureBuffers(renderBufferSize_, 2);
    for (auto& slot : plugins_) {
        slot.plugin->activate(sampleRate, bufferSize);
    }
}

void PluginChain::activate() {
    // No-op: plugins are activated individually in setSampleRate() and addPlugin().
}

void PluginChain::deactivate() {
    LOGI("deactivate() entered tid=%ld", getTid());
    std::unique_lock lock(chainMutex_);
    LOGI("deactivate() chainMutex_ acquired tid=%ld", getTid());
    for (auto& slot : plugins_) {
        slot.plugin->deactivate();
    }
    LOGI("deactivate() done tid=%ld", getTid());
}

size_t PluginChain::getSize() const {
    std::shared_lock lock(chainMutex_);
    return plugins_.size();
}

IPlugin* PluginChain::getPlugin(int index) {
    std::shared_lock lock(chainMutex_);
    if (index < 0 || index >= static_cast<int>(plugins_.size())) {
        return nullptr;
    }
    return plugins_[index].plugin.get();
}

uint64_t PluginChain::getPluginInstanceId(int index) const {
    std::shared_lock lock(chainMutex_);
    if (index < 0 || index >= static_cast<int>(plugins_.size())) {
        return 0;
    }
    return plugins_[index].instanceId;
}

void PluginChain::setParameter(int pluginIndex, uint32_t portIndex, float value) {
    std::shared_lock lock(chainMutex_);
    if (pluginIndex < 0 || pluginIndex >= static_cast<int>(plugins_.size())) {
        return;
    }
    plugins_[pluginIndex].plugin->setParameter(portIndex, value);
}

float PluginChain::getParameter(int pluginIndex, uint32_t portIndex) const {
    std::shared_lock lock(chainMutex_);
    if (pluginIndex < 0 || pluginIndex >= static_cast<int>(plugins_.size())) {
        return 0.0f;
    }
    return plugins_[pluginIndex].plugin->getParameter(portIndex);
}

void PluginChain::setPluginFilePath(int pluginIndex, const std::string& propertyUri, const std::string& path) {
    std::shared_lock lock(chainMutex_);
    if (pluginIndex < 0 || pluginIndex >= static_cast<int>(plugins_.size())) {
        return;
    }
    plugins_[pluginIndex].plugin->setFilePath(propertyUri, path);
}

void PluginChain::injectAtom(int pluginIndex, const void* data, uint32_t size) {
    std::shared_lock lock(chainMutex_);
    if (pluginIndex < 0 || pluginIndex >= static_cast<int>(plugins_.size())) {
        return;
    }
    plugins_[pluginIndex].plugin->injectAtom(data, size);
}

PluginChain::ChainState PluginChain::saveChainState() {
    std::shared_lock lock(chainMutex_);
    ChainState cs;
    cs.plugins.reserve(plugins_.size());
    for (auto& slot : plugins_) {
        auto ps = slot.plugin->saveState();
        // Retain the factory format for a complete plugin-state snapshot.
        // Plugins that do not override saveState() return an empty
        // PluginState, so identify them from the current factory metadata.
        if (ps.format.empty()) {
            ps.format = slot.plugin->getInfo().format;
        }
        if (ps.pluginUri.empty()) {
            // Strip any "FORMAT:" prefix from the factory identifier.
            auto id = slot.plugin->getInfo().id;
            auto colon = id.find(':');
            if (colon != std::string::npos && id.substr(0, colon) == ps.format) {
                ps.pluginUri = id.substr(colon + 1);
            } else {
                ps.pluginUri = id;
            }
        }
        cs.plugins.push_back(std::move(ps));
    }
    LOGI("saveChainState: %zu plugins", cs.plugins.size());
    return cs;
}

bool PluginChain::restorePluginState(int index, const PluginState& state) {
    std::unique_lock lock(chainMutex_);
    if (index < 0 || index >= static_cast<int>(plugins_.size())) {
        return false;
    }
    bool ok = plugins_[index].plugin->restoreState(state);
    LOGI("restorePluginState: index=%d ok=%d", index, ok);
    return ok;
}

void PluginChain::ensureBuffers(uint32_t numFrames, uint32_t numChannels) {
    if (intermediateBuffers_.size() < numChannels) {
        intermediateBuffers_.resize(numChannels);
    }
    for (auto& buffer : intermediateBuffers_) {
        if (buffer.size() < numFrames) {
            buffer.resize(numFrames);
        }
    }
}

void PluginChain::clearOutputs(float* const* outputs, uint32_t numFrames) const noexcept {
    if (!outputs || numFrames == 0) {
        return;
    }
    for (uint32_t channel = 0; channel < 2; ++channel) {
        if (outputs[channel]) {
            std::memset(outputs[channel], 0, numFrames * sizeof(float));
        }
    }
}

} // namespace guitarrackcraft
