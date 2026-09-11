/* Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * This file is part of NNAGA.
 * NNAGA is free software under the GNU General Public License version 3.
 */
#include "AudioPathDiagnostics.h"

#include <chrono>
#include <cstdio>
#include <mutex>
#include <thread>

#if defined(__ANDROID__)
#include <android/log.h>
#define DIAG_LOG(...) __android_log_print(ANDROID_LOG_INFO, "NNAGA_AUDIODIAG", __VA_ARGS__)
#else
#define DIAG_LOG(...) do { std::printf(__VA_ARGS__); std::printf("\n"); } while (0)
#endif

namespace guitarrackcraft::diag {
namespace {
std::once_flag g_started;

struct Snapshot {
    uint64_t values[10]{};
};

Snapshot read() {
    Counters& c = counters();
    Snapshot s;
    s.values[0] = c.jsfxNotActive.load(std::memory_order_relaxed);
    s.values[1] = c.jsfxFaulted.load(std::memory_order_relaxed);
    s.values[2] = c.jsfxOversized.load(std::memory_order_relaxed);
    s.values[3] = c.jsfxInitBypass.load(std::memory_order_relaxed);
    s.values[4] = c.jsfxBadPorts.load(std::memory_order_relaxed);
    s.values[5] = c.chainOversized.load(std::memory_order_relaxed);
    s.values[6] = c.chainNoPlan.load(std::memory_order_relaxed);
    s.values[7] = c.graphSilenced.load(std::memory_order_relaxed);
    s.values[8] = c.latencyResetNode.load(std::memory_order_relaxed);
    s.values[9] = c.latencyResetGlobal.load(std::memory_order_relaxed);
    return s;
}
} // namespace

Counters& counters() noexcept {
    static Counters instance;
    return instance;
}

void startLogging() noexcept {
    std::call_once(g_started, [] {
        std::thread([] {
            Snapshot previous = read();
            for (;;) {
                std::this_thread::sleep_for(std::chrono::seconds(2));
                const Snapshot now = read();
                bool changed = false;
                for (int i = 0; i < 10; ++i)
                    if (now.values[i] != previous.values[i]) changed = true;
                if (!changed) continue;
                DIAG_LOG("jsfx[inactive=%llu faulted=%llu oversized=%llu initBypass=%llu "
                         "badPorts=%llu] chain[oversized=%llu noPlan=%llu] "
                         "graph[silenced=%llu latencyResetNode=%llu latencyResetGlobal=%llu]",
                         (unsigned long long)(now.values[0] - previous.values[0]),
                         (unsigned long long)(now.values[1] - previous.values[1]),
                         (unsigned long long)(now.values[2] - previous.values[2]),
                         (unsigned long long)(now.values[3] - previous.values[3]),
                         (unsigned long long)(now.values[4] - previous.values[4]),
                         (unsigned long long)(now.values[5] - previous.values[5]),
                         (unsigned long long)(now.values[6] - previous.values[6]),
                         (unsigned long long)(now.values[7] - previous.values[7]),
                         (unsigned long long)(now.values[8] - previous.values[8]),
                         (unsigned long long)(now.values[9] - previous.values[9]));
                previous = now;
            }
        }).detach();
    });
}

} // namespace guitarrackcraft::diag
