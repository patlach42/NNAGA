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

#include "PluginUIManager.h"
#include "PluginChain.h"
#include "PluginUIGuard.h"
#include "../x11/X11Worker.h"
#include "../x11/X11NativeDisplay.h"
#include <android/log.h>
#include <chrono>

#define LOG_TAG "PluginUIManager"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

namespace guitarrackcraft {

PluginUIManager::~PluginUIManager() {
    // Clear every display callback and perform the same graceful X11 cleanup
    // used for an explicit destroy before this path manager is discarded.
    for (size_t i = 0; i < uiEntries_.size(); ++i) {
        destroyPluginUI(static_cast<int>(i));
    }
}
void PluginUIManager::rebindChain(PluginChain* chain) {
    // Invalidate callbacks before destroying their UIs. The caller retains
    // the old chain while this synchronous teardown is in progress.
    {
        std::lock_guard uiLock(uiMutex_);
        chain_ = nullptr;
        ++chainGeneration_;
    }

    size_t entryCount = 0;
    {
        std::lock_guard uiLock(uiMutex_);
        entryCount = uiEntries_.size();
    }
    for (size_t i = 0; i < entryCount; ++i) {
        destroyPluginUI(static_cast<int>(i));
    }

    {
        std::lock_guard uiLock(uiMutex_);
        chain_ = chain;
        ++chainGeneration_;
    }
}

 

bool PluginUIManager::createPluginUI(int pluginIndex, int displayNumber,
                                     unsigned long parentWindowId,
                                     const std::string& nativeLibDir,
                                     const std::string& x11LibsDir)
{
    LOGI("createPluginUI entry: pluginIndex=%d displayNumber=%d parentWindowId=0x%lx",
         pluginIndex, displayNumber, parentWindowId);

    // Tear down any prior UI before taking the chain snapshot. This prevents
    // a concurrent rebind from making an old snapshot destroy a new UI.
    destroyPluginUI(pluginIndex);

    PluginChain* chainSnapshot = nullptr;
    uint64_t generationSnapshot = 0;
    {
        std::lock_guard uiLock(uiMutex_);
        chainSnapshot = chain_;
        generationSnapshot = chainGeneration_;
    }
    if (!chainSnapshot) return false;

    IPlugin* plugin = nullptr;
    PluginInfo info;
    chainSnapshot->visitPlugin(static_cast<size_t>(pluginIndex), [&](IPlugin& candidate) {
        plugin = &candidate;
        info = candidate.getInfo();
    });
    if (!plugin) {
        LOGI("createPluginUI: invalid index %d", pluginIndex);
        return false;
    }

    if (!info.hasX11Ui || info.x11UiBinaryPath.empty()) {
        LOGI("createPluginUI: plugin %d has no X11 UI", pluginIndex);
        return false;
    }

    LOGI("createPluginUI: path=%s uiUri=%s", info.x11UiBinaryPath.c_str(), info.x11UiUri.c_str());

    auto ui = std::make_unique<LV2PluginUI>();
    auto indexPtr = std::make_shared<std::atomic<int>>(pluginIndex);
    auto detachedPtr = std::make_shared<std::atomic<bool>>(false);
    auto instanceId = chainSnapshot->getPluginInstanceId(pluginIndex);
    auto paramCb = [this, instanceId, detachedPtr](uint32_t portIndex, float value) {
        if (detachedPtr->load(std::memory_order_acquire)) return;
        PluginChain* chain = nullptr;
        uint64_t generation = 0;
        {
            std::lock_guard uiLock(uiMutex_);
            chain = chain_;
            generation = chainGeneration_;
            bool found = false;
            for (const auto& entry : uiEntries_) {
                if (entry.detached == detachedPtr) { found = true; break; }
            }
            if (!found) chain = nullptr;
        }
        if (chain && generation == chainGeneration_ &&
            !detachedPtr->load(std::memory_order_acquire))
            chain->submitParameter(instanceId, portIndex, value);
    };

    auto atomCb = [this, instanceId, detachedPtr](uint32_t portIndex, uint32_t size, const void* data) {
        if (detachedPtr->load(std::memory_order_acquire)) return;
        PluginChain* chain = nullptr;
        uint64_t generation = 0;
        {
            std::lock_guard uiLock(uiMutex_);
            chain = chain_;
            generation = chainGeneration_;
            bool found = false;
            for (const auto& entry : uiEntries_) {
                if (entry.detached == detachedPtr) { found = true; break; }
            }
            if (!found) chain = nullptr;
        }
        if (chain && generation == chainGeneration_ &&
            !detachedPtr->load(std::memory_order_acquire))
            chain->injectAtom(instanceId, portIndex, data, size);
    };

    ui->setAtomCallback(atomCb);

    X11NativeDisplay* display = getX11Display(displayNumber);
    if (!display) {
        LOGI("createPluginUI: display %d not found", displayNumber);
        return false;
    }

    LOGI("createPluginUI: posting instantiate to display's pluginUI thread");

    LV2PluginUI* uiPtr = ui.get();
    std::string binaryPath = info.x11UiBinaryPath;
    std::string bundlePath = info.x11UiBundlePath;
    std::string uiUri = info.x11UiUri;
    std::string pluginId = info.id;
    std::string nativeLib = nativeLibDir;
    std::string x11Lib = x11LibsDir;

    bool ok = false;
    display->postTaskAndWait([
        &ok, uiPtr, &binaryPath, &bundlePath, &uiUri, &pluginId, displayNumber, parentWindowId,
        plugin, &paramCb, &nativeLib, &x11Lib]() {
        LOGI("createPluginUI: running instantiate on pluginUI thread");
        ok = uiPtr->instantiate(
            binaryPath, bundlePath, uiUri, pluginId, displayNumber, parentWindowId,
            plugin, paramCb, nativeLib, x11Lib
        );
        LOGI("createPluginUI: instantiate returned ok=%d on pluginUI thread", ok ? 1 : 0);
        if (ok) {
            uiPtr->triggerResize();
            LOGI("createPluginUI: triggerResize called on pluginUI thread");
        }
    });
    LOGI("createPluginUI: instantiate completed with ok=%d", ok ? 1 : 0);

    if (!ok) {
        LOGI("createPluginUI: instantiate failed");
        return false;
    }

    bool stale = false;
    {
        std::lock_guard uiLock(uiMutex_);
        stale = chain_ != chainSnapshot || chainGeneration_ != generationSnapshot;
        if (!stale) {
            if (static_cast<int>(uiEntries_.size()) <= pluginIndex) {
                uiEntries_.resize(pluginIndex + 1);
            }
            uiEntries_[pluginIndex].ui = std::move(ui);
            uiEntries_[pluginIndex].displayNumber = displayNumber;
            uiEntries_[pluginIndex].plugin = plugin;
            uiEntries_[pluginIndex].pluginIndex = indexPtr;
            uiEntries_[pluginIndex].detached = detachedPtr;
        }
    }
    if (stale) {
        detachedPtr->store(true, std::memory_order_release);
        auto uiShared = std::make_shared<std::unique_ptr<LV2PluginUI>>(std::move(ui));
        bool posted = withDisplayPostTaskAndWait(displayNumber, [uiShared, displayNumber]() {
            if (*uiShared) {
                (*uiShared)->gracefulShutdown(displayNumber, 1000);
                (*uiShared)->cleanup();
                uiShared->reset();
            }
        });
        if (!posted) {
            getX11Worker().postAndWait([uiShared, displayNumber]() {
                if (*uiShared) {
                    (*uiShared)->gracefulShutdown(displayNumber, 1000);
                    (*uiShared)->cleanup();
                    uiShared->reset();
                }
            });
        }
        return false;
    }

    LOGI("createPluginUI: requesting initial frame for display=%d", displayNumber);
    if (display) {
        display->setIdleCallback([this, plugin, detachedPtr]() {
            if (paused_.load(std::memory_order_acquire) ||
                detachedPtr->load(std::memory_order_acquire)) return;
            LV2PluginUI* uiSnapshot = nullptr;
            uint64_t generationSnapshot = 0;
            {
                std::lock_guard uiLock(uiMutex_);
                generationSnapshot = chainGeneration_;
                for (const auto& entry : uiEntries_) {
                    if (entry.plugin == plugin && entry.detached == detachedPtr &&
                        entry.ui && entry.ui->isValid()) {
                        uiSnapshot = entry.ui.get();
                        break;
                    }
                }
            }
            if (!uiSnapshot || detachedPtr->load(std::memory_order_acquire)) return;
            if (generationSnapshot != chainGeneration_) return;
            uiSnapshot->idle();
            uiSnapshot->syncOutputPorts(nullptr);
        });
        LOGI("createPluginUI: idle callback set on display=%d", displayNumber);
    }

    guitarrackcraft::withDisplayRequestFrame(displayNumber);

    return true;
}

void PluginUIManager::destroyPluginUI(int pluginIndex) {
    LOGI("destroyPluginUI ENTER pluginIndex=%d", pluginIndex);

    std::unique_ptr<LV2PluginUI> uiToDestroy;
    int displayNumber = -1;

    {
        std::lock_guard uiLock(uiMutex_);
        if (pluginIndex >= 0 && pluginIndex < static_cast<int>(uiEntries_.size())) {
            auto& entry = uiEntries_[pluginIndex];
            if (entry.detached) entry.detached->store(true, std::memory_order_release);
            entry.plugin = nullptr;
            uiToDestroy = std::move(entry.ui);
            displayNumber = entry.displayNumber;
            entry.displayNumber = -1;
            entry.pluginIndex.reset();
        }
    }

    if (displayNumber >= 0) {
        withDisplaySetIdleCallback(displayNumber, nullptr);
        LOGI("destroyPluginUI: cleared idle callback on display=%d", displayNumber);
    }

    if (uiToDestroy) {
        LOGI("destroyPluginUI: graceful shutdown for pluginIndex=%d display=%d (SYNCHRONOUS)", pluginIndex, displayNumber);
        bool posted = false;
        if (displayNumber >= 0) {
            auto uiShared = std::make_shared<std::unique_ptr<LV2PluginUI>>(std::move(uiToDestroy));
            posted = withDisplayPostTaskAndWait(displayNumber, [uiShared, displayNumber]() {
                LOGI("destroyPluginUI: Phase 1 - stopping plugin event loop on pluginUI thread");
                if (uiShared && *uiShared) {
                    (*uiShared)->gracefulShutdown(displayNumber, 1000);
                    LOGI("destroyPluginUI: Phase 2 - running final cleanup on pluginUI thread");
                    (*uiShared)->cleanup();
                    uiShared->reset();
                }
                LOGI("destroyPluginUI: graceful shutdown completed on pluginUI thread");
            });
            if (!posted) {
                // Display gone between our check and post — reclaim ownership for fallback
                uiToDestroy = std::move(*uiShared);
            }
        }
        if (!posted) {
            LOGI("destroyPluginUI: display gone, falling back to X11Worker");
            auto uiShared = std::make_shared<std::unique_ptr<LV2PluginUI>>(std::move(uiToDestroy));
            getX11Worker().postAndWait([uiShared, displayNumber]() {
                if (uiShared && *uiShared) {
                    (*uiShared)->gracefulShutdown(displayNumber, 1000);
                    (*uiShared)->cleanup();
                    uiShared->reset();
                }
            });
        }
        LOGI("destroyPluginUI EXIT pluginIndex=%d (graceful shutdown completed)", pluginIndex);
    } else {
        LOGI("destroyPluginUI EXIT pluginIndex=%d (no-op, out of range or no UI)", pluginIndex);
    }
}

bool PluginUIManager::idleAllUIs() {
    return false;
}

void PluginUIManager::pauseAllUIs() {
    paused_.store(true, std::memory_order_release);
    LOGI("pauseAllUIs: UI idle callbacks paused");
}

void PluginUIManager::resumeAllUIs() {
    paused_.store(false, std::memory_order_release);
    LOGI("resumeAllUIs: UI idle callbacks resumed");
}

void PluginUIManager::reorderUIs(int fromIndex, int toIndex) {
    std::lock_guard lock(uiMutex_);

    int maxIdx = std::max(fromIndex, toIndex);
    if (maxIdx >= static_cast<int>(uiEntries_.size())) {
        uiEntries_.resize(maxIdx + 1);
    }

    // Mirror the same move that PluginChain::reorderPlugins does
    auto entry = std::move(uiEntries_[fromIndex]);
    uiEntries_.erase(uiEntries_.begin() + fromIndex);
    uiEntries_.insert(uiEntries_.begin() + toIndex, std::move(entry));

    // Update every live atomic index to reflect the new positions
    for (int i = 0; i < static_cast<int>(uiEntries_.size()); ++i) {
        if (uiEntries_[i].pluginIndex) {
            uiEntries_[i].pluginIndex->store(i, std::memory_order_release);
        }
    }

    LOGI("reorderUIs: moved %d -> %d, updated %zu entries",
         fromIndex, toIndex, uiEntries_.size());
}

void PluginUIManager::detachPlugin(int pluginIndex) {
    std::lock_guard lock(uiMutex_);
    if (pluginIndex >= 0 && pluginIndex < static_cast<int>(uiEntries_.size())) {
        auto& entry = uiEntries_[pluginIndex];
        if (entry.detached) {
            entry.detached->store(true, std::memory_order_release);
        }
        if (entry.ui) {
            entry.ui->clearPlugin();
            LOGI("detachPlugin: cleared plugin pointer for index %d", pluginIndex);
        }
    }
}

void PluginUIManager::detachAndShiftForRemoval(int chainIndex) {
    std::lock_guard lock(uiMutex_);

    // Find the UI entry whose captured index matches the chain index being removed
    for (auto& entry : uiEntries_) {
        if (entry.pluginIndex &&
            entry.pluginIndex->load(std::memory_order_acquire) == chainIndex) {
            if (entry.detached) {
                entry.detached->store(true, std::memory_order_release);
            }
            if (entry.ui) {
                entry.ui->clearPlugin();
                LOGI("detachAndShiftForRemoval: detached UI for chainIndex %d", chainIndex);
            }
            // Invalidate so no callback uses this index
            entry.pluginIndex->store(-1, std::memory_order_release);
            break;
        }
    }

    // Decrement all captured indices above chainIndex to match the chain's erase
    for (auto& entry : uiEntries_) {
        if (entry.pluginIndex) {
            int idx = entry.pluginIndex->load(std::memory_order_acquire);
            if (idx > chainIndex) {
                entry.pluginIndex->store(idx - 1, std::memory_order_release);
            }
        }
    }

    LOGI("detachAndShiftForRemoval: shifted indices above %d", chainIndex);
}

void PluginUIManager::notifyUIParameterChange(int pluginIndex, uint32_t portIndex, float value) {
    int displayNumber = -1;
    IPlugin* plugin = nullptr;
    std::shared_ptr<std::atomic<bool>> token;
    {
        std::lock_guard uiLock(uiMutex_);
        if (pluginIndex >= 0 && pluginIndex < static_cast<int>(uiEntries_.size())) {
            auto& entry = uiEntries_[pluginIndex];
            if (entry.ui && entry.ui->isValid() && entry.displayNumber >= 0) {
                displayNumber = entry.displayNumber;
                plugin = entry.plugin;
                token = entry.detached;
            }
        }
    }
    if (displayNumber >= 0 && plugin && token) {
        withDisplayPostTask(displayNumber, [this, plugin, token, portIndex, value]() {
            if (token->load(std::memory_order_acquire)) return;
            std::lock_guard uiLk(uiMutex_);
            for (auto& entry : uiEntries_) {
                if (entry.plugin == plugin && entry.detached == token &&
                    !token->load(std::memory_order_acquire) &&
                    entry.ui && entry.ui->isValid()) {
                    entry.ui->portEvent(portIndex, value);
                    return;
                }
            }
        });
    }
}

bool PluginUIManager::pollFileRequest(FileRequest& out) {
    std::lock_guard uiLock(uiMutex_);
    for (int i = 0; i < static_cast<int>(uiEntries_.size()); ++i) {
        auto& entry = uiEntries_[i];
        if (entry.ui && entry.ui->isValid()) {
            std::string uri = entry.ui->getPendingFileRequest();
            if (!uri.empty()) {
                out.pathId = pathId_;
                out.pluginIndex = i;
                out.propertyUri = std::move(uri);
                return true;
            }
        }
    }
    return false;
}

void PluginUIManager::deliverFileToUI(int pluginIndex, const std::string& propertyUri, const std::string& filePath) {
    int displayNumber = -1;
    IPlugin* plugin = nullptr;
    std::shared_ptr<std::atomic<bool>> token;
    {
        std::lock_guard uiLock(uiMutex_);
        if (pluginIndex >= 0 && pluginIndex < static_cast<int>(uiEntries_.size())) {
            auto& entry = uiEntries_[pluginIndex];
            if (entry.ui && entry.ui->isValid() && entry.displayNumber >= 0) {
                displayNumber = entry.displayNumber;
                plugin = entry.plugin;
                token = entry.detached;
            }
        }
    }
    if (displayNumber < 0 || !plugin || !token) {
        LOGI("deliverFileToUI: no active UI for plugin %d", pluginIndex);
        return;
    }

    withDisplayPostTask(displayNumber, [this, plugin, token, propertyUri, filePath]() {
        if (token->load(std::memory_order_acquire)) return;
        std::lock_guard uiLk(uiMutex_);
        for (auto& entry : uiEntries_) {
            if (entry.plugin == plugin && entry.detached == token &&
                !token->load(std::memory_order_acquire) &&
                entry.ui && entry.ui->isValid()) {
                entry.ui->deliverFilePath(propertyUri, filePath);
                return;
            }
        }
    });

    LOGI("deliverFileToUI: posted delivery for plugin %d prop=%s path=%s",
         pluginIndex, propertyUri.c_str(), filePath.c_str());
}

} // namespace guitarrackcraft
