/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * This file is part of Guitar RackCraft.
 *
 * Guitar RackCraft is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Guitar RackCraft is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Guitar RackCraft. If not, see <https://www.gnu.org/licenses/>.
 */

package com.vibes.dsp.ui.navigation

import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.ui.Modifier
import androidx.navigation.NavHostController
import androidx.navigation.NavType
import androidx.navigation.compose.NavHost
import androidx.navigation.compose.composable
import androidx.navigation.compose.currentBackStackEntryAsState
import androidx.navigation.compose.rememberNavController
import androidx.navigation.navArgument
import androidx.lifecycle.viewmodel.compose.viewModel
import kotlinx.coroutines.launch
import com.vibes.dsp.ui.browser.PluginBrowserScreen
import com.vibes.dsp.ui.modgui.ModguiScreen
import com.vibes.dsp.ui.rack.RackScreen
import com.vibes.dsp.ui.vst.VST_MANAGER_ROUTE
import com.vibes.dsp.ui.vst.vstManagerRoute
import com.vibes.dsp.ui.rack.RackViewModel
import com.vibes.dsp.ui.recordings.RecordingsScreen
import com.vibes.dsp.ui.settings.AudioSettingsScreen
import com.vibes.dsp.ui.tone3000.Tone
import com.vibes.dsp.ui.tone3000.Tone3000Screen
import com.vibes.dsp.ui.tone3000.ToneDetailScreen

sealed class Screen(val route: String) {
    object Rack : Screen("rack")
    object Browser : Screen("browser?pathId={pathId}&replaceIndex={replaceIndex}") {
        fun route(pathId: Long, replaceIndex: Int = -1) = "browser?pathId=$pathId&replaceIndex=$replaceIndex"
    }
    object Modgui : Screen("modgui/{pluginIndex}?pathId={pathId}&w={w}&h={h}") {
        fun route(pathId: Long, pluginIndex: Int, w: Int = 0, h: Int = 0) = "modgui/$pluginIndex?pathId=$pathId&w=$w&h=$h"
    }
    object Settings : Screen("settings")
    object Recordings : Screen("recordings?targetPathId={targetPathId}") {
        fun route(targetPathId: Long) = "recordings?targetPathId=$targetPathId"
    }
    object Tone3000 : Screen("tone3000?tag={tag}&gear={gear}&platform={platform}&sourcePlugin={sourcePlugin}&sourceSlot={sourceSlot}") {
        fun route(tag: String? = null, gear: String? = null, platform: String? = null, sourcePluginIndex: Int = -1, sourceSlot: String? = null): String {
            val tagPart = tag?.let { "tag=$it" } ?: ""
            val gearPart = gear?.let { "gear=$it" } ?: ""
            val platformPart = platform?.let { "platform=$it" } ?: ""
            val sourcePart = if (sourcePluginIndex >= 0) "sourcePlugin=$sourcePluginIndex" else ""
            val slotPart = sourceSlot?.let { "sourceSlot=$it" } ?: ""
            val query = listOf(tagPart, gearPart, platformPart, sourcePart, slotPart).filter { it.isNotEmpty() }.joinToString("&")
            return if (query.isNotEmpty()) "tone3000?$query" else "tone3000"
        }
    }
    object ToneDetail : Screen("tone_detail/{toneId}?sourcePlugin={sourcePlugin}&sourceSlot={sourceSlot}&architecture={architecture}") {
        fun route(
            toneId: String,
            sourcePluginIndex: Int = -1,
            sourceSlot: String? = null,
            architecture: String? = null
        ): String {
            val parts = mutableListOf<String>()
            if (sourcePluginIndex >= 0) parts.add("sourcePlugin=$sourcePluginIndex")
            if (sourceSlot != null) parts.add("sourceSlot=$sourceSlot")
            if (architecture != null) parts.add("architecture=$architecture")
            val query = parts.joinToString("&")
            return if (query.isNotEmpty()) "tone_detail/$toneId?$query" else "tone_detail/$toneId"
        }
    }
}

@Composable
fun AppNavigation(
    engineReady: Boolean,
    navController: NavHostController = rememberNavController()
) {
    val backStackEntry by navController.currentBackStackEntryAsState()
    val rackViewModel: RackViewModel = viewModel()
    LaunchedEffect(engineReady) {
        if (engineReady) rackViewModel.onNativeEngineReady()
    }
    val currentRoute = backStackEntry?.destination?.route
    val recordingScope = rememberCoroutineScope()

    Box(modifier = Modifier.fillMaxSize()) {
        // RackScreen is ALWAYS composed — kept alive so X11/modgui UIs don't re-render.
        val isRackVisible = currentRoute == null || currentRoute == Screen.Rack.route
        RackScreen(
            isVisible = isRackVisible,
            onNavigateToBrowser = { pathId -> navController.navigate(Screen.Browser.route(pathId)) },
            onNavigateToSettings = { navController.navigate(Screen.Settings.route) },
            onNavigateToRecordings = { pathId -> navController.navigate(Screen.Recordings.route(pathId)) },
            onNavigateToTone3000 = { tag, gear, platform, sourcePluginIndex, sourceSlot ->
                navController.navigate(Screen.Tone3000.route(tag, gear, platform, sourcePluginIndex, sourceSlot))
            },
            onNavigateToVstManager = { navController.navigate(VST_MANAGER_ROUTE) },
            onReplacePlugin = { pathId, replaceIndex ->
                navController.navigate(Screen.Browser.route(pathId, replaceIndex))
            },
            viewModel = rackViewModel
        )

        // NavHost is always composed (navController needs its graph).
        // The rack route is an empty placeholder; the real RackScreen lives above.
        // Other screens overlay on top of the rack when navigated to.
        NavHost(
            navController = navController,
            startDestination = Screen.Rack.route,
            modifier = Modifier.fillMaxSize()
        ) {
            composable(Screen.Rack.route) {
                // Empty — RackScreen is always composed above
            }
            // VST manager (full flavor only — playstore stub is a no-op).
            vstManagerRoute(navController)
            composable(
                route = Screen.Modgui.route,
                arguments = listOf(
                    navArgument("pluginIndex") { type = NavType.IntType },
                    navArgument("pathId") { type = NavType.LongType },
                    navArgument("w") { type = NavType.IntType; defaultValue = 0 },
                    navArgument("h") { type = NavType.IntType; defaultValue = 0 }
                )
            ) { entry ->
                val pathId = entry.arguments?.getLong("pathId") ?: 0L
                val pluginIndex = entry.arguments?.getInt("pluginIndex") ?: 0
                val contentWidth = entry.arguments?.getInt("w") ?: 0
                val contentHeight = entry.arguments?.getInt("h") ?: 0
                ModguiScreen(
                    pathId = pathId,
                    pluginIndex = pluginIndex,
                    contentWidth = contentWidth,
                    contentHeight = contentHeight,
                    onNavigateBack = { navController.popBackStack() }
                )
            }
            composable(
                route = Screen.Browser.route,
                arguments = listOf(
                    navArgument("pathId") { type = NavType.LongType },
                    navArgument("replaceIndex") {
                        type = NavType.IntType
                        defaultValue = -1
                    }
                )
            ) { entry ->
                val pathId = entry.arguments?.getLong("pathId") ?: 0L
                val replaceIndex = entry.arguments?.getInt("replaceIndex") ?: -1
                PluginBrowserScreen(
                    pathId = pathId,
                    replaceIndex = replaceIndex,
                    onNavigateBack = { navController.popBackStack() }
                )
            }
            composable(Screen.Settings.route) {
                AudioSettingsScreen(
                    viewModel = rackViewModel,
                    onNavigateBack = { navController.popBackStack() }
                )
            }
            composable(
                route = Screen.Recordings.route,
                arguments = listOf(navArgument("targetPathId") { type = NavType.LongType })
            ) { entry ->
                val targetPathId = entry.arguments?.getLong("targetPathId") ?: -1L
                RecordingsScreen(
                    targetPathId = targetPathId,
                    onNavigateBack = { navController.popBackStack() },
                    onPlayRecording = { path ->
                        if (targetPathId > 0L) {
                            val fileName = java.io.File(path).name
                            recordingScope.launch {
                                if (rackViewModel.loadTrackWav(targetPathId, path, fileName)) {
                                    navController.popBackStack()
                                }
                            }
                        }
                    },
                    onLoadRecordingPreset = { json ->
                        rackViewModel.loadRecordingPreset(json)
                        navController.popBackStack()
                    }
                )
            }
            composable(
                route = Screen.Tone3000.route,
                arguments = listOf(
                    navArgument("tag") { type = NavType.StringType; nullable = true; defaultValue = null },
                    navArgument("gear") { type = NavType.StringType; nullable = true; defaultValue = null },
                    navArgument("platform") { type = NavType.StringType; nullable = true; defaultValue = null },
                    navArgument("sourcePlugin") { type = NavType.IntType; defaultValue = -1 },
                    navArgument("sourceSlot") { type = NavType.StringType; nullable = true; defaultValue = null }
                )
            ) { entry ->
                val tag = entry.arguments?.getString("tag")
                val gear = entry.arguments?.getString("gear")
                val platform = entry.arguments?.getString("platform")
                val sourcePluginIndex = entry.arguments?.getInt("sourcePlugin") ?: -1
                val sourceSlot = entry.arguments?.getString("sourceSlot")
                Tone3000Screen(
                    onNavigateBack = { navController.popBackStack() },
                    onNavigateToDetail = { tone, architecture ->
                        // Store the selected tone in the back stack entry's saved state
                        navController.currentBackStackEntry?.savedStateHandle?.set("selected_tone", tone)
                        navController.navigate(Screen.ToneDetail.route(tone.id, sourcePluginIndex, sourceSlot, architecture))
                    },
                    initialTag = tag,
                    initialGear = gear,
                    initialPlatform = platform,
                    sourcePluginIndex = sourcePluginIndex,
                    sourceSlot = sourceSlot
                )
            }
            composable(
                route = Screen.ToneDetail.route,
                arguments = listOf(
                    navArgument("toneId") { type = NavType.StringType },
                    navArgument("sourcePlugin") { type = NavType.IntType; defaultValue = -1 },
                    navArgument("sourceSlot") { type = NavType.StringType; nullable = true; defaultValue = null },
                    navArgument("architecture") { type = NavType.StringType; nullable = true; defaultValue = null }
                )
            ) { entry ->
                val toneId = entry.arguments?.getString("toneId") ?: ""
                val sourcePluginIndex = entry.arguments?.getInt("sourcePlugin") ?: -1
                val sourceSlot = entry.arguments?.getString("sourceSlot")
                val architecture = entry.arguments?.getString("architecture")
                val selectedTone = navController.previousBackStackEntry?.savedStateHandle?.get<Tone>("selected_tone")

                ToneDetailScreen(
                    toneId = toneId,
                    initialTone = selectedTone,
                    onNavigateBack = { navController.popBackStack() },
                    sourcePluginIndex = sourcePluginIndex,
                    sourceSlot = sourceSlot,
                    architecture = architecture
                )
            }
        }
    }
}
