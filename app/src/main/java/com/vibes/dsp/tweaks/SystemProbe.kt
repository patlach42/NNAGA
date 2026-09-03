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
        val result = PrivilegedShell.runAsRoot(
            "for t in /proc/$pid/task/*; do " +
                "n=\$(cat \$t/comm 2>/dev/null); " +
                "case \"\$n\" in *Usb*|*udio*|*ender*) " +
                "echo \"\$n \$(cat \$t/stat 2>/dev/null | awk '{print \"policy_prio=\" \$18 \" rt_prio=\" \$40}')\";; " +
                "esac; done"
        )
        return Report(
            "Audio thread scheduling",
            if (result.ok && result.stdout.isNotBlank()) result.stdout.trimEnd()
            else "Unavailable: ${result.output.trim()}",
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

    fun all(): List<Report> = listOf(
        cpuTopology(),
        usbInterrupts(),
        interruptRate(),
        audioThreadPolicies(),
        thermal(),
    )
}
