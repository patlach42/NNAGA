/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * This file is part of NNAGA.
 */
package com.vibes.dsp.ui.settings

import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.verticalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.defaultMinSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.material3.Divider
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.semantics.clearAndSetSemantics
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.role
import androidx.compose.ui.semantics.selected
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.semantics.stateDescription
import com.vibes.dsp.ui.live.LiveLayoutPreferences
import com.vibes.dsp.ui.theme.AppearancePreferences

@Composable
fun InterfaceSettingsScreen() {
    val context = LocalContext.current
    var horizontalPlugins by remember { mutableStateOf(LiveLayoutPreferences.getHorizontalPlugins(context)) }
    var fitTilesOnScreen by remember { mutableStateOf(LiveLayoutPreferences.getFitTilesOnScreen(context)) }
    var armExclusiveOnTrackSelection by remember {
        mutableStateOf(LiveLayoutPreferences.getArmExclusiveOnTrackSelection(context))
    }
    var selectedPaletteId by remember { mutableStateOf(AppearancePreferences.selectedPaletteId(context)) }

    Column(
        modifier = Modifier
            .padding(
                horizontal = SettingsDimensions.contentPadding,
                vertical = SettingsDimensions.spacing
            )
            .verticalScroll(rememberScrollState()),
        verticalArrangement = Arrangement.spacedBy(SettingsDimensions.spacing)
    ) {
        Text(text = "Accent color", style = MaterialTheme.typography.titleMedium)
        Text(
            text = "Used for selected, active, and performance controls.",
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant
        )
        AppearancePreferences.palettes.chunked(5).forEach { row ->
            Row(
                horizontalArrangement = Arrangement.spacedBy(SettingsDimensions.spacing),
                verticalAlignment = Alignment.CenterVertically
            ) {
                row.forEach { palette ->
                    val isSelected = palette.id == selectedPaletteId
                    val swatchColor = Color(palette.argb)
                    Box(
                        modifier = Modifier
                            .size(SettingsDimensions.touchTarget)
                            .clickable(role = Role.RadioButton) {
                                selectedPaletteId = palette.id
                                AppearancePreferences.setPalette(context, palette.id)
                            }
                            .semantics {
                                contentDescription = palette.label
                                role = Role.RadioButton
                                selected = isSelected
                                stateDescription = if (isSelected) "Selected" else "Not selected"
                            },
                        contentAlignment = Alignment.Center
                    ) {
                        Box(
                            modifier = Modifier
                                .size(SettingsDimensions.swatch)
                                .clip(CircleShape)
                                .background(swatchColor)
                                .border(
                                    width = if (isSelected) {
                                        SettingsDimensions.selectedIndicator
                                    } else {
                                        SettingsDimensions.divider
                                    },
                                    color = if (isSelected) {
                                        MaterialTheme.colorScheme.onSurface
                                    } else {
                                        MaterialTheme.colorScheme.outlineVariant
                                    },
                                    shape = CircleShape
                                ),
                            contentAlignment = Alignment.Center
                        ) {
                            if (isSelected) {
                                Text(
                                    text = "✓",
                                    color = Color(AppearancePreferences.contentArgbForAccent(palette.argb)),
                                    style = MaterialTheme.typography.labelLarge,
                                    modifier = Modifier.clearAndSetSemantics { }
                                )
                            }
                        }
                    }
                }
            }
        }
        Divider(
            thickness = SettingsDimensions.divider,
            color = MaterialTheme.colorScheme.outlineVariant
        )
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .defaultMinSize(minHeight = SettingsDimensions.touchTarget),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.SpaceBetween
        ) {
            Column(modifier = Modifier.weight(1f)) {
                Text(text = "Plugins in a horizontal row", style = MaterialTheme.typography.bodyMedium)
                Text(
                    text = "Off = vertical plugin stack; on = horizontal scrolling row in Devices tile.",
                    style = MaterialTheme.typography.labelSmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant
                )
            }
            Switch(
                checked = horizontalPlugins,
                onCheckedChange = { enabled ->
                    horizontalPlugins = enabled
                    LiveLayoutPreferences.setHorizontalPlugins(context, enabled)
                },
                modifier = Modifier.semantics { contentDescription = "Plugins in a horizontal row" }
            )
        }
        Divider(
            thickness = SettingsDimensions.divider,
            color = MaterialTheme.colorScheme.outlineVariant
        )
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .defaultMinSize(minHeight = SettingsDimensions.touchTarget),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.SpaceBetween
        ) {
            Column(modifier = Modifier.weight(1f)) {
                Text(text = "Fit all tiles on screen", style = MaterialTheme.typography.bodyMedium)
                Text(
                    text = "All visible tiles share available height; vertical page scrolling disabled.",
                    style = MaterialTheme.typography.labelSmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant
                )
            }
            Switch(
                checked = fitTilesOnScreen,
                onCheckedChange = { enabled ->
                    fitTilesOnScreen = enabled
                    LiveLayoutPreferences.setFitTilesOnScreen(context, enabled)
                },
                modifier = Modifier.semantics { contentDescription = "Fit all tiles on screen" }
            )
        }
        Divider(
            thickness = SettingsDimensions.divider,
            color = MaterialTheme.colorScheme.outlineVariant
        )
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .defaultMinSize(minHeight = SettingsDimensions.touchTarget),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.SpaceBetween
        ) {
            Column(modifier = Modifier.weight(1f)) {
                Text(
                    text = "Arm exclusive on track selection",
                    style = MaterialTheme.typography.bodyMedium
                )
                Text(
                    text = "Selecting a track arms it and disarms every other track.",
                    style = MaterialTheme.typography.labelSmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant
                )
            }
            Switch(
                checked = armExclusiveOnTrackSelection,
                onCheckedChange = { enabled ->
                    armExclusiveOnTrackSelection = enabled
                    LiveLayoutPreferences.setArmExclusiveOnTrackSelection(context, enabled)
                },
                modifier = Modifier.semantics {
                    contentDescription = "Arm exclusive on track selection"
                }
            )
        }
    }
}
