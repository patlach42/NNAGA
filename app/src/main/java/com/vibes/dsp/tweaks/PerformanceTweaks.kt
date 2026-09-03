/*
 * Copyright (C) 2026 patlach42
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

package com.vibes.dsp.tweaks

import android.content.Context
import android.os.PowerManager

/**
 * What the app can do to help itself, and what it can only report.
 *
 * The measurements behind these are in CLAUDE_DRIVER_RECAP.md. Two of them
 * shaped the whole list: steady-state DSP uses six tenths of a percent of the
 * quantum budget, so nothing here is about making audio computation faster;
 * and the audible faults that remained after every software fix came from
 * outside the app, so most of the value is in removing interference rather
 * than in tuning the engine.
 */
object PerformanceTweaks {

    enum class Requirement { None, Root }

    enum class Risk {
        /** Reversible, scoped to this app, cannot affect anything else. */
        Safe,

        /** Affects the device beyond this app, but reverts on reboot. */
        SystemWide,
    }

    enum class State { Unknown, Applied, NotApplied, Unavailable }

    data class Tweak(
        val id: String,
        val title: String,
        /** What it does and why, in the user's terms. */
        val summary: String,
        /** What could go wrong. Shown before anything is applied. */
        val caution: String,
        val requirement: Requirement,
        val risk: Risk,
    )

    data class Outcome(val state: State, val detail: String)

    val catalogue: List<Tweak> = listOf(
        Tweak(
            id = "wifi_off",
            title = "Turn Wi-Fi off while playing",
            summary = "Background Wi-Fi activity was measured on this hardware " +
                "to interrupt USB audio about every thirty seconds. Turning " +
                "the radio off removed it completely.",
            caution = "You lose network until you turn it back on. Nothing else " +
                "changes, and it is restored by turning Wi-Fi on again.",
            requirement = Requirement.Root,
            risk = Risk.SystemWide,
        ),
        Tweak(
            id = "rt_priority",
            title = "Real-time scheduling for the audio thread",
            summary = "Lets the render thread run at a real-time priority the " +
                "system normally reserves for itself, so ordinary work cannot " +
                "push it aside. Without this it runs at a favourable but " +
                "ordinary priority.",
            caution = "A real-time thread that misbehaves can starve the rest " +
                "of the system. The priority used here is the lowest real-time " +
                "level and applies only to this app's audio threads.",
            requirement = Requirement.Root,
            risk = Risk.Safe,
        ),
        Tweak(
            id = "timer_slack",
            title = "Tighten timer slack",
            summary = "Android lets the kernel batch wakeups by delaying them " +
                "slightly, which saves power and costs precision. Expect " +
                "little from it here: this engine is woken by eventfd, and " +
                "those wakeups do not wait for timer slack. Offered for the " +
                "timeout paths and for measurement, not as a fix.",
            caution = "Slightly higher battery use while the app runs. Scoped " +
                "to this process and reset when it exits.",
            requirement = Requirement.Root,
            risk = Risk.Safe,
        ),
        Tweak(
            id = "battery_exemption",
            title = "Exempt from battery optimisation",
            summary = "Stops the system throttling the app in the background. " +
                "Opens the system dialog; the choice is yours.",
            caution = "None beyond slightly higher battery use.",
            requirement = Requirement.None,
            risk = Risk.Safe,
        ),
    )

    /**
     * Applies or reverts a tweak and reports what the device says afterwards,
     * never what was intended. Returns the fresh inspection so a caller cannot
     * show success for a write that did not take.
     */
    fun apply(context: Context, tweak: Tweak, enable: Boolean): Outcome {
        when (tweak.id) {
            "wifi_off" -> {
                val result = PrivilegedShell.runAsRoot(
                    if (enable) "svc wifi disable" else "svc wifi enable"
                )
                if (!result.ok) {
                    return Outcome(State.Unavailable, result.output.trim().ifBlank {
                        "could not change Wi-Fi state"
                    })
                }
            }

            "rt_priority" -> {
                // Raising the limit does not promote threads that already
                // exist: each asks for its policy once, when it starts. So
                // this only takes effect for an audio session started after
                // it, and saying so is more useful than appearing to work.
                val pid = android.os.Process.myPid()
                val result = PrivilegedShell.runAsRoot(
                    if (enable) "prlimit --rtprio=1:1 --pid $pid"
                    else "prlimit --rtprio=0:0 --pid $pid"
                )
                if (!result.ok) {
                    return Outcome(State.Unavailable, result.output.trim().ifBlank {
                        "prlimit unavailable on this device"
                    })
                }
            }

            "timer_slack" -> {
                val pid = android.os.Process.myPid()
                val target = if (enable) "0" else "50000"
                val result = PrivilegedShell.writePrivilegedAndVerify(
                    "/proc/$pid/timerslack_ns", target
                )
                if (!result.ok) {
                    return Outcome(State.Unavailable, result.output.trim().ifBlank {
                        "could not set timer slack"
                    })
                }
            }

            "battery_exemption" -> {
                // The system decides this one; all the app can do is ask.
                val shown = com.vibes.dsp.engine.AudioInterferenceAdvisor
                    .requestBatteryOptimisationExemption(context)
                if (!shown) {
                    return inspect(context, tweak)
                }
                return Outcome(State.Unknown, "system dialog opened")
            }

            else -> return Outcome(State.Unknown, "unknown tweak")
        }
        return inspect(context, tweak)
    }

    /** Everything the device currently reports about a tweak's state. */
    fun inspect(context: Context, tweak: Tweak): Outcome = when (tweak.id) {
        "wifi_off" -> {
            val wifi = context.applicationContext
                .getSystemService(android.net.wifi.WifiManager::class.java)
            when (wifi?.isWifiEnabled) {
                true -> Outcome(State.NotApplied, "Wi-Fi is on")
                false -> Outcome(State.Applied, "Wi-Fi is off")
                null -> Outcome(State.Unknown, "Wi-Fi state unavailable")
            }
        }

        "rt_priority" -> {
            val limit = PrivilegedShell.runAsRoot("ulimit -r 2>/dev/null")
            val value = limit.stdout.trim().toIntOrNull()
            when {
                !limit.ok -> Outcome(State.Unavailable, "needs root")
                value == null -> Outcome(State.Unknown, "limit not reported")
                value > 0 -> Outcome(State.Applied, "real-time priority up to $value")
                else -> Outcome(State.NotApplied, "real-time priority not permitted")
            }
        }

        "timer_slack" -> {
            val slack = PrivilegedShell.readPrivileged("/proc/self/timerslack_ns")
            when {
                slack == null -> Outcome(State.Unavailable, "needs root")
                slack.toLongOrNull() == 0L -> Outcome(State.Applied, "slack is 0 ns")
                else -> Outcome(State.NotApplied, "slack is $slack ns")
            }
        }

        "battery_exemption" -> {
            val power = context.getSystemService(PowerManager::class.java)
            val exempt = power?.isIgnoringBatteryOptimizations(context.packageName)
            when (exempt) {
                true -> Outcome(State.Applied, "exempt")
                false -> Outcome(State.NotApplied, "optimised")
                null -> Outcome(State.Unknown, "state unavailable")
            }
        }

        else -> Outcome(State.Unknown, "unknown tweak")
    }
}
