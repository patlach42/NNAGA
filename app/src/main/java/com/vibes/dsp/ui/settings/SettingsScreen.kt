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
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Divider
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import com.vibes.dsp.ui.components.NnagaTextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.role
import androidx.compose.ui.semantics.selected
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.unit.dp
import com.vibes.dsp.BuildConfig
import com.vibes.dsp.engine.AudioSettingsManager
import com.vibes.dsp.engine.AudioBackend
import com.vibes.dsp.engine.PluginRepositoryService
import com.vibes.dsp.engine.UsbAudioDriver
import com.vibes.dsp.ui.rack.RackViewModel
import com.vibes.dsp.ui.vst.VstManagerTab

internal object SettingsDimensions {
    val tabHeight = 44.dp
    val divider = 1.dp
    val touchTarget = 48.dp
    val swatch = 32.dp
    val spacing = 4.dp
    val contentPadding = 8.dp
    val selectedIndicator = 2.dp
}

enum class SettingsTab(val argument: String, val label: String) {
    Driver("driver", "Driver"),
    Vst("vst", "Manage VST"),
    Interface("interface", "Interface"),
    ClipLauncher("clip-launcher", "Clip Launcher");

    companion object {
        fun fromArgument(argument: String?): SettingsTab =
            entries.firstOrNull { it.argument == argument } ?: Driver
    }
}

/**
 * Settings content intended to be hosted by the dashboard shell.
 * Navigation and the top-level Back action belong to that shell.
 */
@OptIn(ExperimentalFoundationApi::class)
@Composable
fun SettingsScreen(
    viewModel: RackViewModel,
    repositoryService: PluginRepositoryService,
    pendingRepositoryPackageId: String? = null,
    initialTab: SettingsTab = SettingsTab.Driver,
    modifier: Modifier = Modifier,
    onFullscreenChanged: (Boolean) -> Unit = {},
) {
    val availableTabs = remember {
        buildList {
            add(SettingsTab.Driver)
            if (BuildConfig.HAS_VST_HOST) add(SettingsTab.Vst)
            add(SettingsTab.Interface)
            add(SettingsTab.ClipLauncher)
        }
    }
    var selectedTab by rememberSaveable(initialTab, availableTabs) {
        mutableStateOf(initialTab.takeIf { it in availableTabs } ?: SettingsTab.Driver)
    }
    var showDriverDialog by remember { mutableStateOf(false) }
    var showBackendDialog by remember { mutableStateOf(false) }
    var fullscreenContent by remember { mutableStateOf(false) }
    val context = androidx.compose.ui.platform.LocalContext.current
    if (showBackendDialog) {
        val current = AudioSettingsManager.getAudioBackend(context)
        AlertDialog(
            onDismissRequest = { showBackendDialog = false },
            title = { Text("Audio backend (${current.name})") },
            text = { Text("Select Direct USB or Android Oboe") },
            confirmButton = {
                Row {
                    NnagaTextButton(onClick = {
                        viewModel.setAudioBackend(AudioBackend.DirectUsb)
                        showBackendDialog = false
                        showDriverDialog = true
                    }) { Text("Direct USB") }
                    NnagaTextButton(onClick = {
                        viewModel.setAudioBackend(AudioBackend.AndroidOboe)
                        showBackendDialog = false
                    }) { Text("Android") }
                }
            },
        )
    }

    if (showDriverDialog) {
        val current = AudioSettingsManager.getUsbAudioDriver(context)
        AlertDialog(
            onDismissRequest = { showDriverDialog = false },
            title = { Text("USB driver (${current.name})") },
            text = { Text("Choose the direct USB transport driver") },
            confirmButton = {
                Row {
                    NnagaTextButton(
                        onClick = {
                            viewModel.setUsbAudioDriver(UsbAudioDriver.Uac)
                            showDriverDialog = false
                        },
                    ) { Text("UAC") }
                    NnagaTextButton(
                        onClick = {
                            viewModel.setUsbAudioDriver(UsbAudioDriver.Line6)
                            showDriverDialog = false
                        },
                    ) { Text("Line6") }
                }
            },
        )
    }

    Column(modifier = modifier.fillMaxSize()) {
        if (!fullscreenContent) {
            Column {
                Row(
                    modifier = Modifier
                        .fillMaxWidth()
                        .height(SettingsDimensions.tabHeight),
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
                                        { showBackendDialog = true }
                                    } else {
                                        null
                                    },
                                    onLongClickLabel = if (tab == SettingsTab.Driver) {
                                        "Choose audio backend"
                                    } else {
                                        null
                                    },
                                    role = Role.Tab,
                                ),
                            contentAlignment = Alignment.Center,
                        ) {
                            Text(
                                text = tab.label,
                                style = MaterialTheme.typography.labelMedium,
                                maxLines = 1,
                            )
                            if (isSelected) {
                                Box(
                                    modifier = Modifier
                                        .align(Alignment.BottomCenter)
                                        .fillMaxWidth()
                                        .height(SettingsDimensions.selectedIndicator)
                                        .background(MaterialTheme.colorScheme.primary),
                                )
                            }
                        }
                    }
                }
                Divider(
                    thickness = SettingsDimensions.divider,
                    color = MaterialTheme.colorScheme.outlineVariant,
                )
            }
        }

        when (selectedTab) {
            SettingsTab.Driver -> AudioSettingsScreen(
                viewModel = viewModel,
                onNavigateBack = {},
                embedded = true,
            )
            SettingsTab.Vst -> VstManagerTab(
                repositoryService = repositoryService,
                pendingRepositoryPackageId = pendingRepositoryPackageId,
                onWineSessionActiveChanged = { active ->
                    fullscreenContent = active
                    onFullscreenChanged(active)
                },
            )
            SettingsTab.Interface -> InterfaceSettingsScreen()
            SettingsTab.ClipLauncher -> ClipLauncherSettingsScreen()
        }
    }
}
