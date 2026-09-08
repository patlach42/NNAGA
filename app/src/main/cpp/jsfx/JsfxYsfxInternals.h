/* Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * This file is part of NNAGA.
 * NNAGA is free software under the GNU General Public License version 3.
 */
#pragma once
#include <cstdint>

struct ysfx_s;
using ysfx_t = ysfx_s;

namespace guitarrackcraft {

// EEL allocates script RAM lazily, one 512 KiB calloc at a time, from whichever
// thread first touches a block -- inside @sample on the audio thread, and now
// also from @gfx, with ysfx's NSEEL_HOSTSTUB mutex left empty. Claim a bounded
// budget up front from a control thread so neither happens. Blocks past the
// budget still fall back to EEL's lazy path.
// Must be called off the audio thread.
void jsfxPreallocRam(ysfx_t* fx, uint32_t items);

// Runs the script's @slider section and clears ysfx's pending-slider flag, so
// the audio thread does not run it. @slider is script-defined and unbounded --
// a reverb or synth rebuilding coefficient tables was measured at 10.6 ms,
// against a 0.667 ms budget for a 32-frame block -- so it must not execute
// inside ysfx_process_float. Returns false if the script has no @slider.
// Must be called off the audio thread.
bool jsfxRunSliderCode(ysfx_t* fx);

// Takes ysfx's pending-@init flag, clearing it so ysfx_process_float will not
// run @init inline on the audio thread. ysfx sets it on every transport
// stopped->playing edge. @init is unbounded and ysfx_init() additionally
// destroys the script's open file objects, so unlike @slider it must never run
// concurrently with @sample -- the caller is expected to bypass the plugin
// until jsfxRunInit() has completed.
// Safe to call from the audio thread; it only reads and clears a flag.
bool jsfxTakePendingInit(ysfx_t* fx);

// Runs @init. Must be called off the audio thread, and only while the audio
// thread is bypassing this plugin.
void jsfxRunInit(ysfx_t* fx);

// 2 MiB of script RAM: four EEL blocks, enough for the delay lines of a typical
// reverb, without reserving the 64 MiB that ysfx's default maxmem would allow.
inline constexpr uint32_t kJsfxPreallocItems = 256 * 1024;

} // namespace guitarrackcraft
