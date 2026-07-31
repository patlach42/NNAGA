/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * This file is part of Guitar RackCraft.
 *
 * Guitar RackCraft is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

package com.vibes.dsp.ui.settings

import androidx.compose.foundation.ExperimentalFoundationApi
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
import androidx.compose.material3.Scaffold
import androidx.compose.material3.TabRow
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.TopAppBar
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

enum class SettingsTab(val argument: String, val label: String) {
    Driver("driver", "Driver"),
    Tone3000("tone3000", "TONE3000"),
    Vst("vst", "Manage VST");

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
        }
    }
    var selectedTab by remember(initialTab, availableTabs) {
        mutableStateOf(initialTab.takeIf { it in availableTabs } ?: SettingsTab.Driver)
    }
    var showDriverDialog by remember { mutableStateOf(false) }
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
            TopAppBar(
                title = { Text("Settings") },
                navigationIcon = {
                    IconButton(onClick = onNavigateBack) {
                        Icon(Icons.Default.ArrowBack, contentDescription = "Back")
                    }
                }
            )
        }
    ) { padding ->
        Column(
            modifier = Modifier
                .fillMaxSize()
                .padding(padding)
        ) {
            TabRow(selectedTabIndex = availableTabs.indexOf(selectedTab)) {
                availableTabs.forEach { tab ->
                    Box(
                        modifier = Modifier
                            .weight(1f)
                            .height(48.dp)
                            .semantics {
                                selected = selectedTab == tab
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
                    ) { Text(tab.label) }
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
                SettingsTab.Vst -> VstManagerTab()
            }
        }
    }
}
