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

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>

#include <unistd.h>
#include <sys/syscall.h>
#include <pthread.h>
#include <sys/resource.h>
#if defined(__linux__)
#include <sched.h>
#endif
#if defined(__ANDROID__)
#include <dlfcn.h>
#endif
namespace guitarrackcraft {

inline long getTid() {
#if defined(__ANDROID__) && defined(__linux__)
    return static_cast<long>(syscall(SYS_gettid));
#else
    return static_cast<long>(pthread_self());
#endif
}

#if defined(__linux__)
// Select the two highest-ranked allowed CPUs without allocating or consulting
// system state. Direct USB render and event threads need separate performance
// cores; leaving the fastest core to UI work caused live-session starvation.
inline cpu_set_t deriveAudioCpuMask(const cpu_set_t& allowed) noexcept {
    cpu_set_t audio;
    CPU_ZERO(&audio);

    int allowedCount = 0;
    for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu)
        allowedCount += CPU_ISSET(cpu, &allowed) ? 1 : 0;
    if (allowedCount == 0)
        return audio;

    const int firstRank = std::max(0, allowedCount - 2);
    const int lastRank = allowedCount - 1;
    int rank = 0;
    for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
        if (!CPU_ISSET(cpu, &allowed))
            continue;
        if (rank >= firstRank && rank <= lastRank)
            CPU_SET(cpu, &audio);
        ++rank;
    }
    return audio;
}

// Keep UI descendants away from the CPUs reserved for Direct USB audio. On
// constrained one/two-CPU cpusets preserve at least one runnable UI CPU.
inline cpu_set_t deriveUiCpuMask(const cpu_set_t& allowed) noexcept {
    cpu_set_t ui = allowed;
    const int allowedCount = CPU_COUNT(&allowed);
    if (allowedCount <= 0)
        return ui;
    if (allowedCount <= 2) {
        CPU_ZERO(&ui);
        for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
            if (CPU_ISSET(cpu, &allowed)) {
                CPU_SET(cpu, &ui);
                break;
            }
        }
        return ui;
    }
    const cpu_set_t audio = deriveAudioCpuMask(allowed);
    CPU_XOR(&ui, &allowed, &audio);
    return ui;
}

inline void applyCurrentThreadUiAffinity() noexcept {
    cpu_set_t allowed;
    if (syscall(SYS_sched_getaffinity, 0, sizeof(allowed), &allowed) != 0)
        return;
    const cpu_set_t ui = deriveUiCpuMask(allowed);
    if (CPU_COUNT(&ui) == 0)
        return;
    (void)syscall(SYS_sched_setaffinity, 0, sizeof(ui), &ui);
}

inline void applyCurrentThreadAudioAffinity() noexcept {
    cpu_set_t allowed;
    if (syscall(SYS_sched_getaffinity, 0, sizeof(allowed), &allowed) != 0)
        return;
    const cpu_set_t audio = deriveAudioCpuMask(allowed);
    if (CPU_COUNT(&audio) == 0)
        return;
    (void)syscall(SYS_sched_setaffinity, 0, sizeof(audio), &audio);
}
#endif

// Android Dynamic Performance Framework session, resolved lazily so the
// min-SDK 26 binary remains loadable on pre-API-31 devices.
class PerformanceHintSession {
public:
    explicit PerformanceHintSession(
            int64_t targetDurationNs,
            const int32_t* threadIds = nullptr,
            size_t threadCount = 0) noexcept {
#if defined(__ANDROID__)
        if (targetDurationNs <= 0) return;
        library_ = dlopen("libandroid.so", RTLD_NOW | RTLD_LOCAL);
        if (!library_) return;
        getManager_ = reinterpret_cast<GetManagerFn>(
            dlsym(library_, "APerformanceHint_getManager"));
        createSession_ = reinterpret_cast<CreateSessionFn>(
            dlsym(library_, "APerformanceHint_createSession"));
        reportActual_ = reinterpret_cast<ReportActualFn>(
            dlsym(library_, "APerformanceHint_reportActualWorkDuration"));
        closeSession_ = reinterpret_cast<CloseSessionFn>(
            dlsym(library_, "APerformanceHint_closeSession"));
        setThreads_ = reinterpret_cast<SetThreadsFn>(
            dlsym(library_, "APerformanceHint_setThreads"));
        if (!getManager_ || !createSession_ || !reportActual_ || !closeSession_) {
            dlclose(library_);
            library_ = nullptr;
            return;
        }
        void* manager = getManager_();
        const int32_t currentTid = static_cast<int32_t>(getTid());
        const int32_t* initialThreadIds =
            threadIds && threadCount > 0 ? threadIds : &currentTid;
        const size_t initialThreadCount =
            threadIds && threadCount > 0 ? threadCount : 1U;
        if (manager) {
            session_ = createSession_(
                manager, initialThreadIds, initialThreadCount,
                targetDurationNs);
        }
#else
        (void)targetDurationNs;
        (void)threadIds;
        (void)threadCount;
#endif
    }

    ~PerformanceHintSession() {
#if defined(__ANDROID__)
        if (session_ && closeSession_) closeSession_(session_);
        if (library_) dlclose(library_);
#endif
    }

    PerformanceHintSession(const PerformanceHintSession&) = delete;
    PerformanceHintSession& operator=(const PerformanceHintSession&) = delete;

    bool active() const noexcept {
#if defined(__ANDROID__)
        return session_ != nullptr;
#else
        return false;
#endif
    }

    bool setThreads(const int32_t* tids, size_t count) noexcept {
#if defined(__ANDROID__)
        return session_ && setThreads_ && tids && count > 0 &&
               setThreads_(session_, tids, count) == 0;
#else
        (void)tids;
        (void)count;
        return false;
#endif
    }

    void reportActualWorkDuration(uint64_t durationNs) noexcept {
#if defined(__ANDROID__)
        if (session_ && durationNs > 0) {
            (void)reportActual_(session_, static_cast<int64_t>(durationNs));
        }
#else
        (void)durationNs;
#endif
    }

private:
#if defined(__ANDROID__)
    using GetManagerFn = void* (*)();
    using CreateSessionFn = void* (*)(void*, const int32_t*, size_t, int64_t);
    using ReportActualFn = int (*)(void*, int64_t);
    using SetThreadsFn = int (*)(void*, const int32_t*, size_t);
    using CloseSessionFn = void (*)(void*);

    void* library_ = nullptr;
    void* session_ = nullptr;
    GetManagerFn getManager_ = nullptr;
    CreateSessionFn createSession_ = nullptr;
    ReportActualFn reportActual_ = nullptr;
    SetThreadsFn setThreads_ = nullptr;
    CloseSessionFn closeSession_ = nullptr;
#endif
};

class ThermalHeadroomMonitor {
public:
    ThermalHeadroomMonitor() noexcept {
#if defined(__ANDROID__)
        library_ = dlopen("libandroid.so", RTLD_NOW | RTLD_LOCAL);
        if (!library_) return;
        acquire_ = reinterpret_cast<AcquireFn>(
            dlsym(library_, "AThermal_acquireManager"));
        release_ = reinterpret_cast<ReleaseFn>(
            dlsym(library_, "AThermal_releaseManager"));
        getHeadroom_ = reinterpret_cast<GetHeadroomFn>(
            dlsym(library_, "AThermal_getThermalHeadroom"));
        if (acquire_ && release_ && getHeadroom_)
            manager_ = acquire_();
        if (!manager_) {
            dlclose(library_);
            library_ = nullptr;
        }
#endif
    }

    ~ThermalHeadroomMonitor() {
#if defined(__ANDROID__)
        if (manager_ && release_) release_(manager_);
        if (library_) dlclose(library_);
#endif
    }

    ThermalHeadroomMonitor(const ThermalHeadroomMonitor&) = delete;
    ThermalHeadroomMonitor& operator=(const ThermalHeadroomMonitor&) = delete;

    float sample(int forecastSeconds) const noexcept {
#if defined(__ANDROID__)
        return manager_ && getHeadroom_
            ? getHeadroom_(manager_, forecastSeconds)
            : -1.0f;
#else
        (void)forecastSeconds;
        return -1.0f;
#endif
    }

private:
#if defined(__ANDROID__)
    using AcquireFn = void* (*)();
    using ReleaseFn = void (*)(void*);
    using GetHeadroomFn = float (*)(void*, int);
    void* library_ = nullptr;
    void* manager_ = nullptr;
    AcquireFn acquire_ = nullptr;
    ReleaseFn release_ = nullptr;
    GetHeadroomFn getHeadroom_ = nullptr;
#endif
};

// Best-effort Android/Linux realtime scheduling for app-owned audio threads.
// Prefer SCHED_FIFO when permitted, then Android's urgent-audio nice level.
// Call only once at thread startup; failure is exposed through diagnostics.
inline bool setCurrentThreadUrgentAudio(const char* name) noexcept {
    if (name != nullptr) {
        (void)pthread_setname_np(pthread_self(), name);
    }
#if defined(__linux__)
    applyCurrentThreadAudioAffinity();
#endif
#if defined(__ANDROID__) && defined(__linux__)
    sched_param realtime{};
    realtime.sched_priority = 1;
    if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &realtime) == 0)
        return true;
    constexpr int kUrgentAudioNice = -19;
    return setpriority(PRIO_PROCESS, static_cast<id_t>(getTid()),
                       kUrgentAudioNice) == 0;
#else
    return false;
#endif
}

} // namespace guitarrackcraft
