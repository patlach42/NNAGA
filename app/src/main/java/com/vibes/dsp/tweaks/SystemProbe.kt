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

/**
 * Read-only questions about the device, for finding out who is competing with
 * the audio thread.
 *
 * Nothing here changes anything. The campaign that motivated it spent an
 * afternoon attributing an audible fault to four different causes in turn, each
 * refuted by the next measurement, because the evidence was counts rather than
 * identities. These answer "which interrupt", "which CPU", "how often" - the
 * questions that would have settled it sooner.
 */
object SystemProbe {

    data class Report(val title: String, val body: String)

    /**
     * Which interrupt the USB controller actually uses, and how its count is
     * distributed across CPUs.
     *
     * The name is not known in advance: the controller may appear as xhci,
     * dwc3 or a vendor string, so this matches several and reports what it
     * found rather than assuming. Counts per CPU show which core is already
     * servicing USB, which is what an affinity change would have to respect.
     */
    fun usbInterrupts(): Report {
        val header = PrivilegedShell.runAsRoot("head -1 /proc/interrupts")
        val rows = PrivilegedShell.runAsRoot(
            "grep -iE 'xhci|dwc3|usb' /proc/interrupts"
        )
        val body = when {
            !rows.ok -> "Could not read /proc/interrupts: ${rows.output.trim()}"
            rows.stdout.isBlank() -> "No interrupt line matched xhci, dwc3 or usb."
            else -> header.stdout.trimEnd() + "\n" + rows.stdout.trimEnd()
        }
        return Report("USB interrupts", body)
    }

    /**
     * Two samples of the same interrupt counters a second apart, so the rate
     * is visible rather than the total. A total says which interrupt exists; a
     * rate says which one is busy right now.
     */
    fun interruptRate(): Report {
        val script = "grep -iE 'xhci|dwc3|usb' /proc/interrupts > /data/local/tmp/.irq_a; " +
            "sleep 1; " +
            "grep -iE 'xhci|dwc3|usb' /proc/interrupts > /data/local/tmp/.irq_b; " +
            "paste /data/local/tmp/.irq_a /data/local/tmp/.irq_b; " +
            "rm -f /data/local/tmp/.irq_a /data/local/tmp/.irq_b"
        val result = PrivilegedShell.runAsRoot(script)
        return Report(
            "USB interrupt rate (1 s apart)",
            if (result.ok && result.stdout.isNotBlank()) result.stdout.trimEnd()
            else "Unavailable: ${result.output.trim()}",
        )
    }

    /** CPU capacities, so "big core" is a fact rather than an assumption. */
    fun cpuTopology(): Report {
        val result = PrivilegedShell.runAsRoot(
            "for c in /sys/devices/system/cpu/cpu[0-9]*; do " +
                "n=\${c##*/}; " +
                "cap=\$(cat \$c/cpu_capacity 2>/dev/null || echo ?); " +
                "cur=\$(cat \$c/cpufreq/scaling_cur_freq 2>/dev/null || echo ?); " +
                "max=\$(cat \$c/cpufreq/scaling_max_freq 2>/dev/null || echo ?); " +
                "gov=\$(cat \$c/cpufreq/scaling_governor 2>/dev/null || echo ?); " +
                "echo \"\$n capacity=\$cap cur=\$cur max=\$max governor=\$gov\"; done"
        )
        return Report(
            "CPU topology",
            if (result.ok && result.stdout.isNotBlank()) result.stdout.trimEnd()
            else "Unavailable: ${result.output.trim()}",
        )
    }

    /**
     * Scheduling policy and priority of this process's threads, which answers
     * whether the real-time request actually took. The app asks for
     * SCHED_FIFO at thread start and falls back silently, so the only way to
     * know is to look.
     */
    fun audioThreadPolicies(): Report {
        val pid = android.os.Process.myPid()
        // /proc/pid/stat is read through its own fields rather than awk on
        // whitespace: the second field is the thread name in parentheses and
        // may itself contain spaces, which shifts every column after it.
        // Reading sched instead avoids the question entirely and names the
        // policy rather than leaving a number to be looked up.
        val result = PrivilegedShell.runAsRoot(
            "for t in /proc/$pid/task/*; do " +
                "n=\$(cat \$t/comm 2>/dev/null); " +
                "case \"\$n\" in *Usb*|*udio*|*ender*) " +
                "p=\$(grep -m1 policy \$t/sched 2>/dev/null | tr -s ' ' | cut -d' ' -f3); " +
                "pr=\$(grep -m1 prio \$t/sched 2>/dev/null | tr -s ' ' | cut -d' ' -f3); " +
                "echo \"\$n policy=\$p prio=\$pr\";; " +
                "esac; done"
        )
        return Report(
            "Audio thread scheduling",
            if (result.ok && result.stdout.isNotBlank()) {
                // policy 0 is the ordinary scheduler, 1 is SCHED_FIFO. The app
                // asks for FIFO at thread start and falls back silently, so
                // this is the only place the answer appears.
                result.stdout.trimEnd() + "\n(policy 0 = normal, 1 = FIFO)"
            } else {
                "Unavailable: ${result.output.trim()} — is audio running?"
            },
        )
    }

    /** Thermal state, since throttling changes what any other number means. */
    fun thermal(): Report {
        val result = PrivilegedShell.runAsRoot(
            "for z in /sys/class/thermal/thermal_zone*; do " +
                "t=\$(cat \$z/temp 2>/dev/null); " +
                "n=\$(cat \$z/type 2>/dev/null); " +
                "case \"\$n\" in *cpu*|*CPU*|*soc*|*SOC*) echo \"\$n \$t\";; esac; " +
                "done | head -12"
        )
        return Report(
            "Thermal zones",
            if (result.ok && result.stdout.isNotBlank()) result.stdout.trimEnd()
            else "Unavailable: ${result.output.trim()}",
        )
    }

    /**
     * What in the Wi-Fi stack repeats on a period.
     *
     * The dropouts that cost this campaign an afternoon arrived every thirty
     * seconds and vanished when the radio was switched off, but background
     * scanning was already disabled, so the mechanism is still unnamed.
     * Connectivity validation and health monitoring are the remaining
     * candidates with a period of that order; this reports their settings
     * rather than guessing between them.
     */
    fun wifiPeriodics(): Report {
        val settings = listOf(
            "wifi_scan_always_enabled",
            "captive_portal_mode",
            "wifi_watchdog_poor_network_test_enabled",
            "network_recommendations_enabled",
            "wifi_networks_available_notification_on",
        )
        val body = settings.joinToString("\n") { key ->
            val value = PrivilegedShell.runAsRoot("settings get global $key")
            "$key = ${value.stdout.trim().ifBlank { "?" }}"
        }
        // The framework's own thirty-second candidate. If this reads 30000 it
        // is at least a timer of the right period; anything else rules the
        // framework out and leaves vendor firmware housekeeping, which has no
        // public name and no setting to read.
        val rssiPoll = PrivilegedShell.runAsRoot(
            "cmd wifi get-poll-rssi-interval-msecs 2>/dev/null"
        )
        val power = PrivilegedShell.runAsRoot(
            "dumpsys wifi 2>/dev/null | grep -iE 'power save|screen off|dtim' | head -4"
        )
        return Report(
            "Wi-Fi periodic behaviour",
            body +
                "\nrssi poll interval = " +
                rssiPoll.stdout.trim().ifBlank { "?" } +
                "\n" + power.stdout.trimEnd().ifBlank { "(no power-save detail)" },
        )
    }

    /**
     * What the app already does for itself, and whether it took.
     *
     * These need no permission and no root: they are the app's own doing. They
     * belong next to the tweaks because a user looking at a list of knobs
     * should see what is already on before turning anything, and because each
     * of them can fail silently on a device that declines it.
     */
    fun appSideMeasures(context: android.content.Context): Report {
        val lines = mutableListOf<String>()

        val power = context.getSystemService(android.os.PowerManager::class.java)
        lines += "sustained performance supported: " +
            (power?.isSustainedPerformanceModeSupported?.toString() ?: "?")
        lines += "battery optimisation exempt: " +
            (power?.isIgnoringBatteryOptimizations(context.packageName)?.toString() ?: "?")

        // A foreground service is what keeps the process out of the cached
        // state, so its absence explains far more than any tuning knob.
        val services = runCatching {
            val manager = context.getSystemService(android.app.ActivityManager::class.java)
            @Suppress("DEPRECATION")
            manager?.getRunningServices(64)
                ?.count { it.service.className.contains("AudioSessionService") } ?: 0
        }.getOrDefault(-1)
        lines += "audio foreground service running: " + when (services) {
            -1 -> "unknown"
            0 -> "no"
            else -> "yes"
        }

        val stats = runCatching {
            com.vibes.dsp.engine.NativeEngine.getInstance().getDirectUsbStats()
        }.getOrNull()
        lines += "ADPF hint session active: " +
            (stats?.performanceHintActive?.toString() ?: "unknown (audio not running)")

        return Report("App-side measures", lines.joinToString("\n"))
    }

    fun all(): List<Report> = listOf(
        cpuTopology(),
        usbInterrupts(),
        interruptRate(),
        audioThreadPolicies(),
        thermal(),
        wifiPeriodics(),
    )
}
