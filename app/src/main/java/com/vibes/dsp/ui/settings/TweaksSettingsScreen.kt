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

package com.vibes.dsp.ui.settings

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import com.vibes.dsp.engine.AudioInterferenceAdvisor
import com.vibes.dsp.tweaks.PerformanceTweaks
import com.vibes.dsp.tweaks.PrivilegedShell
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext

/**
 * Performance controls and the measurements behind them.
 *
 * Everything here states what it does, what it costs and whether it actually
 * took effect. A tweak that silently fails is worse than one that is absent,
 * because it invites the user to believe a problem is solved: this whole
 * campaign lost an afternoon to an instrument that hid its own breakage.
 */
@Composable
fun TweaksSettingsScreen() {
    val context = LocalContext.current
    var access by remember { mutableStateOf<PrivilegedShell.Access?>(null) }
    var states by remember {
        mutableStateOf<Map<String, PerformanceTweaks.Outcome>>(emptyMap())
    }
    var advice by remember {
        mutableStateOf<List<AudioInterferenceAdvisor.Advice>>(emptyList())
    }
    var busy by remember { mutableStateOf(false) }
    var pending by remember {
        mutableStateOf<Pair<PerformanceTweaks.Tweak, Boolean>?>(null)
    }
    var lastResult by remember { mutableStateOf<String?>(null) }
    var probes by remember {
        mutableStateOf<List<com.vibes.dsp.tweaks.SystemProbe.Report>>(emptyList())
    }
    var probing by remember { mutableStateOf(false) }

    // Compute off the main thread, assign on it. Mutating Compose state from
    // a background dispatcher is a data race even when it appears to work.
    suspend fun refresh() {
        data class Snapshot(
            val access: PrivilegedShell.Access,
            val states: Map<String, PerformanceTweaks.Outcome>,
            val advice: List<AudioInterferenceAdvisor.Advice>,
        )
        val snapshot = withContext(Dispatchers.IO) {
            Snapshot(
                access = PrivilegedShell.access(),
                states = PerformanceTweaks.catalogue.associate {
                    it.id to PerformanceTweaks.inspect(context, it)
                },
                advice = AudioInterferenceAdvisor.inspect(context),
            )
        }
        access = snapshot.access
        states = snapshot.states
        advice = snapshot.advice
    }

    LaunchedEffect(Unit) { refresh() }

    // The battery exemption is answered in a system dialog, so the state read
    // straight after opening it is always the old one. Re-read when the screen
    // comes back, which is when the user has finished with that dialog.
    val lifecycleOwner = androidx.compose.ui.platform.LocalLifecycleOwner.current
    androidx.compose.runtime.DisposableEffect(lifecycleOwner) {
        val observer = androidx.lifecycle.LifecycleEventObserver { _, event ->
            if (event == androidx.lifecycle.Lifecycle.Event.ON_RESUME) busy = true
        }
        lifecycleOwner.lifecycle.addObserver(observer)
        onDispose { lifecycleOwner.lifecycle.removeObserver(observer) }
    }

    LazyColumn(
        modifier = Modifier.fillMaxWidth().padding(16.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        item {
            Column {
                Text("Tweaks", style = MaterialTheme.typography.titleLarge)
                Text(
                    when (access) {
                        PrivilegedShell.Access.Root ->
                            "Root available. Tweaks that need it are enabled."
                        PrivilegedShell.Access.None ->
                            "No root. Only tweaks that work without it are shown as available."
                        null -> "Checking for root…"
                    },
                    style = MaterialTheme.typography.bodySmall,
                )
            }
        }

        item {
            val drift = remember(busy) { PerformanceTweaks.profileDrift(context) }
            Card(modifier = Modifier.fillMaxWidth()) {
                Column(Modifier.padding(12.dp)) {
                    Text(
                        "Measured configuration",
                        style = MaterialTheme.typography.titleSmall,
                    )
                    Text(
                        "Quantum 64, multiplier 3, automatic geometry. A grid " +
                            "of three ninety-second runs per point found this " +
                            "the only one passing all three with no break in " +
                            "the software path, at a 9.00 ms host queue. The " +
                            "narrower geometry claims less latency and passed " +
                            "none of six.",
                        style = MaterialTheme.typography.bodySmall,
                    )
                    if (drift.isEmpty()) {
                        Text(
                            "Current settings match it.",
                            style = MaterialTheme.typography.labelSmall,
                        )
                    } else {
                        Text(
                            "Differs: " + drift.joinToString("; "),
                            style = MaterialTheme.typography.labelSmall,
                        )
                        OutlinedButton(
                            enabled = !busy,
                            onClick = {
                                PerformanceTweaks.restoreMeasuredProfile(context)
                                lastResult = "Restored the measured configuration. " +
                                    "It applies when audio next starts."
                                busy = true
                            },
                        ) { Text("Restore") }
                    }
                }
            }
        }

        if (advice.isNotEmpty()) {
            item {
                Card(modifier = Modifier.fillMaxWidth()) {
                    Column(Modifier.padding(12.dp)) {
                        Text(
                            "Working against low latency right now",
                            style = MaterialTheme.typography.titleSmall,
                        )
                        advice.forEach {
                            Text("• ${it.message}", style = MaterialTheme.typography.bodySmall)
                        }
                    }
                }
            }
        }

        items(PerformanceTweaks.catalogue, key = { it.id }) { tweak ->
            val outcome = states[tweak.id]
            val available = tweak.requirement == PerformanceTweaks.Requirement.None ||
                access == PrivilegedShell.Access.Root
            Card(modifier = Modifier.fillMaxWidth()) {
                Column(Modifier.padding(12.dp)) {
                    Row(
                        Modifier.fillMaxWidth(),
                        horizontalArrangement = Arrangement.SpaceBetween,
                        verticalAlignment = Alignment.CenterVertically,
                    ) {
                        Text(tweak.title, style = MaterialTheme.typography.titleSmall)
                        Text(
                            outcome?.state?.name ?: "…",
                            style = MaterialTheme.typography.labelSmall,
                        )
                    }
                    Text(tweak.summary, style = MaterialTheme.typography.bodySmall)
                    // The cost is stated next to the control, not hidden behind
                    // a confirmation nobody reads.
                    Text(
                        "Cost: ${tweak.caution}",
                        style = MaterialTheme.typography.bodySmall,
                    )
                    outcome?.detail?.let {
                        Text("Device reports: $it", style = MaterialTheme.typography.labelSmall)
                    }
                    if (!available) {
                        Text(
                            "Needs root, which is not available.",
                            style = MaterialTheme.typography.labelSmall,
                        )
                    } else {
                        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                            Button(
                                enabled = !busy && pending == null &&
                                    outcome?.state != PerformanceTweaks.State.Applied,
                                onClick = {
                                    busy = true
                                    pending = tweak to true
                                },
                            ) { Text("Apply") }
                            OutlinedButton(
                                enabled = !busy && pending == null &&
                                    tweak.supportsRevert &&
                                    outcome?.state == PerformanceTweaks.State.Applied,
                                onClick = {
                                    busy = true
                                    pending = tweak to false
                                },
                            ) { Text("Revert") }
                        }
                    }
                }
            }
        }

        item {
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                OutlinedButton(
                    enabled = !busy,
                    onClick = {
                        busy = true
                        PrivilegedShell.invalidate()
                    },
                ) { Text("Re-check") }
                Button(
                    enabled = !busy,
                    onClick = {
                        val shown = AudioInterferenceAdvisor
                            .requestBatteryOptimisationExemption(context)
                        lastResult = if (shown) {
                            "Battery exemption: system dialog opened"
                        } else {
                            "Battery exemption: already exempt, or the dialog " +
                                "could not be opened — grant it in Settings " +
                                "under Battery."
                        }
                    },
                ) { Text("Battery exemption") }
                OutlinedButton(
                    enabled = !probing,
                    onClick = { probing = true },
                ) { Text(if (probing) "Probing…" else "Investigate") }
            }
        }

        if (probes.isNotEmpty()) {
            item {
                OutlinedButton(onClick = {
                    // Findings that cannot leave the device are findings
                    // nobody else can act on.
                    val text = probes.joinToString("\n\n") { "== ${it.title}\n${it.body}" }
                    runCatching {
                        val clipboard = context.getSystemService(
                            android.content.ClipboardManager::class.java
                        )
                        clipboard?.setPrimaryClip(
                            android.content.ClipData.newPlainText("NNAGA probe", text)
                        )
                        lastResult = "Report copied to the clipboard"
                    }.onFailure { lastResult = "Could not copy: ${it.message}" }
                }) { Text("Copy report") }
            }
        }

        items(probes, key = { it.title }) { report ->
            Card(modifier = Modifier.fillMaxWidth()) {
                Column(Modifier.padding(12.dp)) {
                    Text(report.title, style = MaterialTheme.typography.titleSmall)
                    Text(report.body, style = MaterialTheme.typography.bodySmall)
                }
            }
        }

        lastResult?.let { message ->
            item {
                Card(modifier = Modifier.fillMaxWidth()) {
                    Text(
                        message,
                        modifier = Modifier.padding(12.dp),
                        style = MaterialTheme.typography.bodySmall,
                    )
                }
            }
        }
    }

    LaunchedEffect(busy) {
        if (busy) {
            refresh()
            busy = false
        }
    }

    // Applying reports what the device says afterwards, not what was asked
    // for: a sysfs write can succeed and be reverted by a vendor daemon, and
    // a tweak that only looks applied is worse than one that is absent.
    // Read-only, so it needs no confirmation and changes nothing if it fails.
    LaunchedEffect(probing) {
        if (!probing) return@LaunchedEffect
        val gathered = withContext(Dispatchers.IO) {
            // The app-side report needs no root, so it is worth showing even
            // when the privileged probes come back empty.
            listOf(com.vibes.dsp.tweaks.SystemProbe.appSideMeasures(context)) +
                if (access == PrivilegedShell.Access.Root) {
                    com.vibes.dsp.tweaks.SystemProbe.all()
                } else {
                    emptyList()
                }
        }
        probes = gathered
        probing = false
    }

    LaunchedEffect(pending) {
        val request = pending ?: return@LaunchedEffect
        val (tweak, enable) = request
        // A second tap while this ran used to change the key and cancel the
        // effect, but the blocking su call kept changing the device and its
        // result was lost. Both buttons now gate on busy and on a pending
        // request, so only one operation is ever in flight.
        val outcome = withContext(Dispatchers.IO) {
            PerformanceTweaks.apply(context, tweak, enable)
        }
        val freshAdvice = withContext(Dispatchers.IO) {
            AudioInterferenceAdvisor.inspect(context)
        }
        states = states + (tweak.id to outcome)
        advice = freshAdvice
        lastResult = "${tweak.title}: ${outcome.state} — ${outcome.detail}"
        pending = null
        busy = false
    }
}
