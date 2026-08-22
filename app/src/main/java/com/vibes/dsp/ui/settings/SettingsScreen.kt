/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * This file is part of NNAGA.
 *
 * NNAGA is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

package com.vibes.dsp.ui.settings

import androidx.compose.foundation.ExperimentalFoundationApi
import androidx.compose.foundation.background
import androidx.compose.foundation.combinedClickable
import androidx.compose.foundation.layout.*
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.role
import androidx.compose.ui.semantics.selected
import androidx.compose.ui.semantics.semantics
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.ArrowBack
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.Divider
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import com.vibes.dsp.BuildConfig
import com.vibes.dsp.engine.AudioSettingsManager
import com.vibes.dsp.engine.UsbAudioDriver
import com.vibes.dsp.ui.rack.RackViewModel
import com.vibes.dsp.ui.tone3000.Tone
import com.vibes.dsp.ui.tone3000.Tone3000Screen
import com.vibes.dsp.ui.vst.VstManagerTab

internal object SettingsDimensions {
    val appBarHeight = 48.dp
    val tabHeight = 44.dp
    val touchTarget = 48.dp
    val icon = 18.dp
    val swatch = 32.dp
    val spacing = 4.dp
    val contentPadding = 8.dp
    val divider = 1.dp
    val selectedIndicator = 2.dp
}

@Composable
private fun CompactSettingsTopBar(onNavigateBack: () -> Unit) {
    Surface(color = MaterialTheme.colorScheme.background) {
        Column(
            modifier = Modifier.windowInsetsPadding(
                WindowInsets.safeDrawing.only(WindowInsetsSides.Top + WindowInsetsSides.Horizontal)
            )
        ) {
            Row(
                modifier = Modifier
                    .fillMaxWidth()
                    .height(SettingsDimensions.appBarHeight),
                verticalAlignment = Alignment.CenterVertically
            ) {
                IconButton(
                    onClick = onNavigateBack,
                    modifier = Modifier.size(SettingsDimensions.touchTarget)
                ) {
                    Icon(
                        Icons.Default.ArrowBack,
                        contentDescription = "Back",
                        modifier = Modifier.size(SettingsDimensions.icon)
                    )
                }
                Text(
                    text = "Settings",
                    style = MaterialTheme.typography.titleMedium
                )
            }
            Divider(
                thickness = SettingsDimensions.divider,
                color = MaterialTheme.colorScheme.outlineVariant
            )
        }
    }
}
enum class SettingsTab(val argument: String, val label: String) {
    Driver("driver", "Driver"),
    Tone3000("tone3000", "TONE3000"),
    Vst("vst", "Manage VST"),
    Interface("interface", "Interface"),
    ClipLauncher("clip-launcher", "Clip Launcher");

    companion object {
        fun fromArgument(argument: String?): SettingsTab =
            entries.firstOrNull { it.argument == argument } ?: Driver
    }
}

@OptIn(ExperimentalMaterial3Api::class, ExperimentalFoundationApi::class)
@Composable
fun SettingsScreen(
    viewModel: RackViewModel,
    initialTab: SettingsTab,
    onNavigateBack: () -> Unit,
    onNavigateToToneDetail: (Tone, String?) -> Unit,
    initialTag: String? = null,
    initialGear: String? = null,
    initialPlatform: String? = null,
    sourcePluginIndex: Int = -1,
    sourceSlot: String? = null
) {
    val availableTabs = remember {
        buildList {
            add(SettingsTab.Driver)
            add(SettingsTab.Tone3000)
            if (BuildConfig.HAS_VST_HOST) add(SettingsTab.Vst)
            add(SettingsTab.Interface)
            add(SettingsTab.ClipLauncher)
        }
    }
    var selectedTab by remember(initialTab, availableTabs) {
        mutableStateOf(initialTab.takeIf { it in availableTabs } ?: SettingsTab.Driver)
    }
    var showDriverDialog by remember { mutableStateOf(false) }
    var wineSessionActive by remember { mutableStateOf(false) }
    val context = androidx.compose.ui.platform.LocalContext.current
    if (showDriverDialog) {
        val current = AudioSettingsManager.getUsbAudioDriver(context)
        AlertDialog(
            onDismissRequest = { showDriverDialog = false },
            title = { Text("USB driver (${current.name})") },
            text = { Text("Choose the direct USB transport driver") },
            confirmButton = {
                androidx.compose.foundation.layout.Row {
                    TextButton(onClick = { viewModel.setUsbAudioDriver(UsbAudioDriver.Uac); showDriverDialog = false }) { Text("UAC") }
                    TextButton(onClick = { viewModel.setUsbAudioDriver(UsbAudioDriver.Line6); showDriverDialog = false }) { Text("Line6") }
                }
            }
        )
    }

    Scaffold(
        topBar = {
            if (!wineSessionActive) CompactSettingsTopBar(onNavigateBack)
        }
    ) { padding ->
        Column(
            modifier = Modifier
                .fillMaxSize()
                .padding(padding)
        ) {
            if (!wineSessionActive) {
                Column {
                    Row(
                        modifier = Modifier
                            .fillMaxWidth()
                            .height(SettingsDimensions.tabHeight)
                    ) {
                        availableTabs.forEach { tab ->
                            val isSelected = selectedTab == tab
                            Box(
                                modifier = Modifier
                                    .weight(1f)
                                    .fillMaxHeight()
                                    .semantics {
                                        selected = isSelected
                                        role = Role.Tab
                                    }
                                    .combinedClickable(
                                        onClick = { selectedTab = tab },
                                        onLongClick = if (tab == SettingsTab.Driver) {
                                            { showDriverDialog = true }
                                        } else null,
                                        onLongClickLabel = if (tab == SettingsTab.Driver) "Choose USB driver" else null,
                                        role = Role.Tab
                                    ),
                                contentAlignment = Alignment.Center
                            ) {
                                Text(
                                    text = tab.label,
                                    style = MaterialTheme.typography.labelMedium,
                                    maxLines = 1
                                )
                                if (isSelected) {
                                    Box(
                                        modifier = Modifier
                                            .align(Alignment.BottomCenter)
                                            .fillMaxWidth()
                                            .height(SettingsDimensions.selectedIndicator)
                                            .background(MaterialTheme.colorScheme.primary)
                                    )
                                }
                            }
                        }
                    }
                    Divider(
                        thickness = SettingsDimensions.divider,
                        color = MaterialTheme.colorScheme.outlineVariant
                    )
                }
            }
            when (selectedTab) {
                SettingsTab.Driver -> AudioSettingsScreen(
                    viewModel = viewModel,
                    onNavigateBack = onNavigateBack,
                    embedded = true
                )
                SettingsTab.Tone3000 -> Tone3000Screen(
                    onNavigateBack = onNavigateBack,
                    onNavigateToDetail = onNavigateToToneDetail,
                    initialTag = initialTag,
                    initialGear = initialGear,
                    initialPlatform = initialPlatform,
                    sourcePluginIndex = sourcePluginIndex,
                    sourceSlot = sourceSlot,
                    embedded = true
                )
                SettingsTab.Vst -> VstManagerTab(
                    onWineSessionActiveChanged = { wineSessionActive = it }
                )
                SettingsTab.Interface -> InterfaceSettingsScreen()
                SettingsTab.ClipLauncher -> ClipLauncherSettingsScreen()
            }
        }
    }
}
