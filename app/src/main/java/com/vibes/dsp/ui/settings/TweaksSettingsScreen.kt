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

    suspend fun refresh() {
        withContext(Dispatchers.IO) {
            access = PrivilegedShell.access()
            states = PerformanceTweaks.catalogue.associate {
                it.id to PerformanceTweaks.inspect(context, it)
            }
            advice = AudioInterferenceAdvisor.inspect(context)
        }
    }

    LaunchedEffect(Unit) { refresh() }

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
                        AudioInterferenceAdvisor
                            .requestBatteryOptimisationExemption(context)
                    },
                ) { Text("Battery exemption") }
            }
        }
    }

    LaunchedEffect(busy) {
        if (busy) {
            refresh()
            busy = false
        }
    }
}
