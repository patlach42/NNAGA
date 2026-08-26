/* Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * This file is part of NNAGA.
 * NNAGA is free software under the GNU General Public License version 3.
 */
#include "NativeContextAccess.h"
extern NativeContext* nnagaNativeContext() noexcept;
extern guitarrackcraft::PluginRegistry* nnagaNativePluginRegistry() noexcept;
extern guitarrackcraft::RackGraph* nnagaNativeRackGraph() noexcept;
extern std::mutex* nnagaNativeRackMutex() noexcept;
extern bool nnagaNativeEngineRunning() noexcept;
extern const std::string& nnagaNativeJsfxRoot() noexcept;
namespace guitarrackcraft {
NativeContext* nativeContext() noexcept { return nnagaNativeContext(); }
PluginRegistry* nativePluginRegistry() noexcept { return nnagaNativePluginRegistry(); }
RackGraph* nativeRackGraph() noexcept { return nnagaNativeRackGraph(); }
std::mutex* nativeRackMutex() noexcept { return nnagaNativeRackMutex(); }
std::mutex& nativeRackControlMutex() noexcept { return *nnagaNativeRackMutex(); }
bool nativeEngineRunning() noexcept { return nnagaNativeEngineRunning(); }
const std::string& nativeJsfxRoot() noexcept { return nnagaNativeJsfxRoot(); }
}
