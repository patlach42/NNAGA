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

import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.ArrowBack
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.TabRow
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Tab
import androidx.compose.material3.Text
import androidx.compose.material3.TopAppBar
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import com.vibes.dsp.BuildConfig
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

@OptIn(ExperimentalMaterial3Api::class)
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
        androidx.compose.foundation.layout.Column(
            modifier = Modifier
                .fillMaxSize()
                .padding(padding)
        ) {
            TabRow(selectedTabIndex = availableTabs.indexOf(selectedTab)) {
                availableTabs.forEach { tab ->
                    Tab(
                        selected = selectedTab == tab,
                        onClick = { selectedTab = tab },
                        text = { Text(tab.label) }
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
                SettingsTab.Vst -> VstManagerTab()
            }
        }
    }
}
