/* Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * This file is part of NNAGA.
 * NNAGA is free software under the GNU General Public License version 3.
 */
#pragma once
#include <atomic>
#include <cstdint>

namespace guitarrackcraft::diag {

// Every place the audio path can emit something other than processed audio
// while still meeting its deadline. x-runs stay at zero in all of these, which
// is why they are invisible to the existing realtime stats -- the callback was
// on time, the content was wrong.
//
// Counters are bumped with relaxed atomics from the audio thread (a few ns) and
// printed from a separate logger thread. Read them with:
//     adb logcat -s NNAGA_AUDIODIAG
struct Counters {
    std::atomic<uint64_t> jsfxNotActive{0};      // plugin inactive/not ready
    std::atomic<uint64_t> jsfxFaulted{0};        // ysfx threw once; passthrough forever
    std::atomic<uint64_t> jsfxOversized{0};      // frames > activated quantum
    std::atomic<uint64_t> jsfxInitBypass{0};     // waiting for @init on the worker
    std::atomic<uint64_t> jsfxBadPorts{0};       // null in/out buffers
    std::atomic<uint64_t> chainOversized{0};     // PluginChain cleared the block
    std::atomic<uint64_t> chainNoPlan{0};        // no published plan
    std::atomic<uint64_t> graphSilenced{0};      // RackGraph silenced the rack
    std::atomic<uint64_t> latencyResetNode{0};   // one track's history discarded
    std::atomic<uint64_t> latencyResetGlobal{0}; // every track's history discarded
};

Counters& counters() noexcept;

// Starts the logger thread once. Safe to call repeatedly.
void startLogging() noexcept;

} // namespace guitarrackcraft::diag
