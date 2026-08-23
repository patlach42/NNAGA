/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * This file is part of NNAGA.
 */
package com.vibes.dsp.ui.settings

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.defaultMinSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.MaterialTheme
import com.vibes.dsp.ui.components.NnagaSwitch
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import com.vibes.dsp.ui.live.ClipLauncherPreferences

@Composable
fun ClipLauncherSettingsScreen() {
    val context = LocalContext.current
    var autoDetectBpm by remember {
        mutableStateOf(ClipLauncherPreferences.getAutoDetectBpmFromFilename(context))
    }
    var autoDetectLoopTempo by remember {
        mutableStateOf(ClipLauncherPreferences.getAutoDetectLoopTempo(context))
    }

    Column(
        modifier = Modifier
            .padding(
                horizontal = SettingsDimensions.contentPadding,
                vertical = SettingsDimensions.spacing
            )
            .verticalScroll(rememberScrollState()),
        verticalArrangement = Arrangement.spacedBy(SettingsDimensions.spacing)
    ) {
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .defaultMinSize(minHeight = SettingsDimensions.touchTarget),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.SpaceBetween
        ) {
            Column(modifier = Modifier.weight(1f)) {
                Text(
                    text = "Detect BPM from WAV filename",
                    style = MaterialTheme.typography.bodyMedium
                )
                Text(
                    text = "Recognized BPM becomes the clip's Base BPM and enables Stretch mode.",
                    style = MaterialTheme.typography.labelSmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant
                )
            }
            NnagaSwitch(
                checked = autoDetectBpm,
                onCheckedChange = { enabled ->
                    autoDetectBpm = enabled
                    ClipLauncherPreferences.setAutoDetectBpmFromFilename(context, enabled)
                },
                modifier = Modifier.semantics {
                    contentDescription = "Detect BPM from WAV filename"
                }
            )
        }
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .defaultMinSize(minHeight = SettingsDimensions.touchTarget),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.SpaceBetween
        ) {
            Column(modifier = Modifier.weight(1f)) {
                Text(
                    text = "Autodetect loop tempo",
                    style = MaterialTheme.typography.bodyMedium
                )
                Text(
                    text = "Estimate loop tempo from 2, 4, 8, or 16 bars within 50–200 BPM.",
                    style = MaterialTheme.typography.labelSmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant
                )
            }
            NnagaSwitch(
                checked = autoDetectLoopTempo,
                onCheckedChange = { enabled ->
                    autoDetectLoopTempo = enabled
                    ClipLauncherPreferences.setAutoDetectLoopTempo(context, enabled)
                },
                modifier = Modifier.semantics {
                    contentDescription = "Autodetect loop tempo"
                }
            )
        }
    }
}
