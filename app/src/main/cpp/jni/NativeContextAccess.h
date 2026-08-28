/* Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * This file is part of NNAGA.
 * NNAGA is free software under the GNU General Public License version 3.
 */
#pragma once
#include <cstdint>
#include <mutex>
#include <string>
namespace guitarrackcraft { class PluginRegistry; class RackGraph; class PluginChain; }
struct NativeContext;
namespace guitarrackcraft {
NativeContext* nativeContext() noexcept;
PluginRegistry* nativePluginRegistry() noexcept;
RackGraph* nativeRackGraph() noexcept;
std::mutex* nativeRackMutex() noexcept;
std::mutex& nativeRackControlMutex() noexcept;
void nativeRebindPluginUIManager(int64_t pathId, PluginChain* chain) noexcept;
bool nativeEngineRunning() noexcept;
const std::string& nativeJsfxRoot() noexcept;
namespace jni {
using ::guitarrackcraft::nativePluginRegistry;
using ::guitarrackcraft::nativeRackGraph;
using ::guitarrackcraft::nativeRackControlMutex;
using ::guitarrackcraft::nativeEngineRunning;
}
}
