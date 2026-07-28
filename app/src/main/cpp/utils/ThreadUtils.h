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

#include <unistd.h>
#include <sys/syscall.h>
#include <pthread.h>
#include <sys/resource.h>

namespace guitarrackcraft {

inline long getTid() {
#if defined(__ANDROID__) && defined(__linux__)
    return static_cast<long>(syscall(SYS_gettid));
#else
    return static_cast<long>(pthread_self());
#endif
}

// Best-effort Android/Linux realtime scheduling for app-owned audio threads.
// Prefer SCHED_FIFO when permitted, then Android's urgent-audio nice level.
// Call only once at thread startup; failure is exposed through diagnostics.
inline bool setCurrentThreadUrgentAudio(const char* name) noexcept {
    if (name != nullptr) {
        (void)pthread_setname_np(pthread_self(), name);
    }
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
