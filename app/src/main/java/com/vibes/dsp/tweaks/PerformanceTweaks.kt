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

    private const val STORE = "performance_tweaks"
    private const val KEY_IRQ_AFFINITY = "original_irq_affinity"
    private const val KEY_MIN_FREQ = "original_scaling_min_freq"
    private const val KEY_SLACK = "original_timerslack_ns"

    data class Tweak(
        val id: String,
        val title: String,
        /** What it does and why, in the user's terms. */
        val summary: String,
        /** What could go wrong. Shown before anything is applied. */
        val caution: String,
        val requirement: Requirement,
        val risk: Risk,
        /** False when the system owns the state and the app can only ask. */
        val supportsRevert: Boolean = true,
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
                "level and applies only to this app's audio threads, but a " +
                "real-time thread competes with the whole device.",
            requirement = Requirement.Root,
            risk = Risk.SystemWide,
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
            id = "usb_irq_affinity",
            title = "Pin the USB interrupt to a big core",
            summary = "Moves the USB controller's interrupt onto a fast core, " +
                "so servicing it does not wait behind work on a small one. " +
                "The interrupt is found by name, not assumed: use Investigate " +
                "first to see which one it is and how busy.",
            caution = "The vendor's power service may move it back, which is " +
                "why the new value is read back and reported. Reverts on " +
                "reboot regardless.",
            requirement = Requirement.Root,
            risk = Risk.SystemWide,
        ),
        Tweak(
            id = "big_cluster_floor",
            title = "Raise the big cluster's minimum frequency",
            summary = "Stops the fast cores dropping to their lowest clock " +
                "between audio blocks, so a block does not begin on a core " +
                "that is still ramping up.",
            caution = "Runs warmer and uses more battery, and sustained heat " +
                "eventually causes throttling that costs more than it gains. " +
                "Raises the floor to a mid point, never to the maximum. " +
                "Reverts on reboot.",
            requirement = Requirement.Root,
            risk = Risk.SystemWide,
        ),
        Tweak(
            id = "battery_exemption",
            title = "Exempt from battery optimisation",
            summary = "Stops the system throttling the app in the background. " +
                "Opens the system dialog; the choice is yours.",
            caution = "None beyond slightly higher battery use.",
            requirement = Requirement.None,
            risk = Risk.Safe,
            supportsRevert = false,
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
                // Per thread, not per process: writing to the group leader
                // leaves the render and event threads untouched, which is
                // where it would have to matter. And 0 is not "no slack" - the
                // kernel reads it as "reset to the default" - so a tight value
                // is 1, and reverting restores what was saved.
                val pid = android.os.Process.myPid()
                val tids = PrivilegedShell.runAsRoot(
                    "for t in /proc/$pid/task/*; do " +
                        "n=\$(cat \$t/comm 2>/dev/null); " +
                        "case \"\$n\" in *Usb*|*udio*|*ender*) echo \${t##*/};; esac; done"
                ).stdout.lines().mapNotNull { it.trim().takeIf(String::isNotEmpty) }
                if (tids.isEmpty()) {
                    return Outcome(
                        State.Unavailable,
                        "no audio threads found; start audio first",
                    )
                }
                val prefs = context.getSharedPreferences(STORE, Context.MODE_PRIVATE)
                var failure: String? = null
                tids.forEach { tid ->
                    val path = "/proc/$pid/task/$tid/timerslack_ns"
                    if (enable && !prefs.contains(KEY_SLACK)) {
                        PrivilegedShell.readPrivileged(path)?.trim()
                            ?.let { prefs.edit().putString(KEY_SLACK, it).apply() }
                    }
                    val target = if (enable) {
                        "1"
                    } else {
                        prefs.getString(KEY_SLACK, null) ?: "50000"
                    }
                    val result = PrivilegedShell.writePrivilegedAndVerify(path, target)
                    if (!result.ok && failure == null) failure = result.output.trim()
                }
                failure?.let {
                    return Outcome(State.NotApplied, it.ifBlank { "could not set timer slack" })
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

            "usb_irq_affinity" -> {
                val irq = usbInterruptNumber()
                    ?: return Outcome(State.Unavailable, "no USB interrupt found")
                // Remember what the system had before touching it. Reverting to
                // a guessed "all CPUs" would discard the vendor's own choice
                // and is wrong outright on a machine with more than eight cores.
                val prefs = context.getSharedPreferences(STORE, Context.MODE_PRIVATE)
                if (enable && !prefs.contains(KEY_IRQ_AFFINITY)) {
                    PrivilegedShell.readPrivileged("/proc/irq/$irq/smp_affinity")
                        ?.trim()?.let { prefs.edit().putString(KEY_IRQ_AFFINITY, it).apply() }
                }
                // Highest CPU is the fastest on every big.LITTLE layout seen
                // here; the mask is a bitmask, so CPU n is 1 << n.
                val target = if (enable) {
                    bigCoreMask().ifEmpty {
                        return Outcome(State.Unavailable, "CPU capacities unreadable")
                    }
                } else {
                    prefs.getString(KEY_IRQ_AFFINITY, null)
                        ?: return Outcome(State.Unavailable, "no saved affinity to restore")
                }
                val result = PrivilegedShell.writePrivilegedAndVerify(
                    "/proc/irq/$irq/smp_affinity", target
                )
                if (!result.ok) {
                    // A vendor balancer that reverts the write is the expected
                    // failure here, and saying so is more useful than "failed".
                    return Outcome(
                        State.NotApplied,
                        result.output.trim().ifBlank { "the system did not keep the value" },
                    )
                }
            }

            "big_cluster_floor" -> {
                val policy = bigClusterPolicyPath()
                    ?: return Outcome(State.Unavailable, "no cpufreq policy found")
                val available = PrivilegedShell.readPrivileged(
                    "$policy/scaling_available_frequencies"
                )?.trim()?.split(Regex("\\s+"))?.mapNotNull { it.toLongOrNull() }?.sorted()
                val prefs = context.getSharedPreferences(STORE, Context.MODE_PRIVATE)
                if (enable && !prefs.contains(KEY_MIN_FREQ)) {
                    PrivilegedShell.readPrivileged("$policy/scaling_min_freq")
                        ?.trim()?.let { prefs.edit().putString(KEY_MIN_FREQ, it).apply() }
                }
                val target = if (!enable) {
                    // The value that was there, not the hardware floor: the
                    // vendor or the user may have set something deliberately.
                    prefs.getString(KEY_MIN_FREQ, null)
                        ?: return Outcome(State.Unavailable, "no saved minimum to restore")
                } else {
                    // Strictly between the lowest and the highest. With only
                    // two steps there is no middle, and taking the top would
                    // contradict what this tweak promises.
                    val middle = available?.takeIf { it.size >= 3 }
                        ?.get(available.size / 2)
                    middle?.toString()
                        ?: return Outcome(
                            State.Unavailable,
                            "no intermediate frequency between the extremes",
                        )
                }
                val result = PrivilegedShell.writePrivilegedAndVerify(
                    "$policy/scaling_min_freq", target
                )
                if (!result.ok) {
                    return Outcome(
                        State.NotApplied,
                        result.output.trim().ifBlank { "the system did not keep the value" },
                    )
                }
            }

            else -> return Outcome(State.Unknown, "unknown tweak")
        }
        return inspect(context, tweak)
    }

    /**
     * The interrupt number the USB controller uses, or null.
     *
     * Matched by name rather than assumed: the controller appears as xhci on
     * some devices, dwc3 on others, and under a vendor string on a few.
     */
    private fun usbInterruptNumber(): String? {
        val result = PrivilegedShell.runAsRoot(
            "grep -iE 'xhci|dwc3' /proc/interrupts | head -1 | cut -d: -f1"
        )
        return result.stdout.trim().takeIf { result.ok && it.isNotEmpty() }
    }

    /** Bitmask of the highest-capacity CPUs, as an affinity mask expects. */
    private fun bigCoreMask(): String {
        val caps = PrivilegedShell.runAsRoot(
            "for c in /sys/devices/system/cpu/cpu[0-9]*; do " +
                "printf \"%s %s\n\" \${c##*/cpu} \$(cat \$c/cpu_capacity 2>/dev/null || echo 0); done"
        )
        return bigCoreMaskOf(caps.stdout)
    }

    /**
     * Affinity mask covering every CPU at the highest reported capacity.
     *
     * Split out from the shell call so it can be tested: an affinity mask is
     * a bitmask in hex, and getting the shift or the base wrong silently pins
     * an interrupt to the wrong cluster - a mistake that would look like the
     * tweak working while making things worse.
     *
     * Falls back to every CPU when the topology cannot be read, which is the
     * kernel default and therefore harmless.
     */
    internal fun bigCoreMaskOf(topology: String): String {
        val entries = topology.lineSequence()
            .mapNotNull { line ->
                val parts = line.trim().split(Regex("\\s+"))
                val cpu = parts.getOrNull(0)?.toIntOrNull()
                val capacity = parts.getOrNull(1)?.toIntOrNull()
                if (cpu != null && capacity != null && capacity > 0) cpu to capacity else null
            }
            .toList()
        // No capacities means no way to tell clusters apart. Returning a
        // mask covering everything would claim every core is fast and pin an
        // interrupt on that basis, so refuse instead.
        if (entries.isEmpty()) return ""
        val best = entries.maxOf { it.second }
        if (best <= 0) return ""
        var mask = 0L
        entries.filter { it.second == best }.forEach { mask = mask or (1L shl it.first) }
        return java.lang.Long.toHexString(mask)
    }

    /**
     * Soft real-time priority limit from the text of /proc/pid/limits.
     *
     * Split out so it can be tested: the file is column-aligned with variable
     * spacing and the units column is absent for this row, so a naive split
     * picks up the wrong field and would report a limit that is not there.
     */
    internal fun realtimeLimitOf(limits: String): Int? {
        val line = limits.lineSequence()
            .firstOrNull { it.contains("realtime priority", ignoreCase = true) }
            ?: return null
        // "Max realtime priority        0          0"
        val tail = line.substringAfter("priority", "").trim()
        val soft = tail.split(Regex("\\s+")).firstOrNull() ?: return null
        return soft.toIntOrNull() ?: if (soft == "unlimited") Int.MAX_VALUE else null
    }

    /** cpufreq policy directory governing the highest-capacity cluster. */
    private fun bigClusterPolicyPath(): String? {
        val result = PrivilegedShell.runAsRoot(
            "for c in /sys/devices/system/cpu/cpu[0-9]*; do " +
                "cap=\$(cat \$c/cpu_capacity 2>/dev/null || echo 0); " +
                "printf \"%s %s\n\" \$cap \$c; done | sort -rn | head -1 | cut -d' ' -f2"
        )
        val cpu = result.stdout.trim().takeIf { result.ok && it.isNotEmpty() }
            ?: return null
        val related = PrivilegedShell.runAsRoot("readlink -f $cpu/cpufreq")
        return related.stdout.trim().takeIf { related.ok && it.isNotEmpty() }
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
            // Read this process's own limit. `ulimit` inside `su -c` reports
            // the root shell's limit, which is not the one that governs our
            // audio threads and would have shown success regardless.
            val pid = android.os.Process.myPid()
            val limits = PrivilegedShell.readPrivileged("/proc/$pid/limits")
            val soft = limits?.let { realtimeLimitOf(it) }
            when {
                limits == null -> Outcome(State.Unavailable, "needs root")
                soft == null -> Outcome(State.Unknown, "limit not reported")
                soft > 0 -> Outcome(State.Applied, "real-time priority up to $soft")
                else -> Outcome(State.NotApplied, "real-time priority not permitted")
            }
        }

        "timer_slack" -> {
            // Report an audio thread's slack, not the group leader's: they are
            // different values and only the former matters here.
            val pid = android.os.Process.myPid()
            val slack = PrivilegedShell.runAsRoot(
                "for t in /proc/$pid/task/*; do " +
                    "n=\$(cat \$t/comm 2>/dev/null); " +
                    "case \"\$n\" in *Usb*|*udio*|*ender*) " +
                    "cat \$t/timerslack_ns 2>/dev/null; break;; esac; done"
            )
            val value = slack.stdout.trim().toLongOrNull()
            when {
                !slack.ok -> Outcome(State.Unavailable, "needs root")
                value == null -> Outcome(State.Unknown, "no audio thread running")
                value <= 1L -> Outcome(State.Applied, "slack is $value ns")
                else -> Outcome(State.NotApplied, "slack is $value ns")
            }
        }

        "usb_irq_affinity" -> {
            val irq = usbInterruptNumber()
            val current = irq?.let {
                PrivilegedShell.readPrivileged("/proc/irq/$it/smp_affinity")
            }?.trim()
            when {
                irq == null -> Outcome(State.Unavailable, "no USB interrupt found")
                current == null -> Outcome(State.Unavailable, "needs root")
                current.trimStart('0').equals(bigCoreMask().trimStart('0'), true) ->
                    Outcome(State.Applied, "IRQ $irq on mask $current")
                else -> Outcome(State.NotApplied, "IRQ $irq on mask $current")
            }
        }

        "big_cluster_floor" -> {
            val policy = bigClusterPolicyPath()
            val min = policy?.let { PrivilegedShell.readPrivileged("$it/scaling_min_freq") }
            val hwMin = policy?.let { PrivilegedShell.readPrivileged("$it/cpuinfo_min_freq") }
            when {
                policy == null || min == null -> Outcome(State.Unavailable, "needs root")
                hwMin != null && min.trim() != hwMin.trim() ->
                    Outcome(State.Applied, "floor ${min.trim()} kHz")
                else -> Outcome(State.NotApplied, "floor at hardware minimum ${min.trim()}")
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
