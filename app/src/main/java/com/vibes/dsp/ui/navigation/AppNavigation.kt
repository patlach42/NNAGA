/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
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

package com.vibes.dsp.ui.navigation
import android.content.ContentResolver
import android.content.Intent
import android.net.Uri

import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.ui.Modifier
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.lifecycle.viewmodel.compose.viewModel
import androidx.navigation.NavHostController
import androidx.navigation.NavType
import androidx.navigation.compose.NavHost
import androidx.navigation.compose.composable
import androidx.navigation.compose.currentBackStackEntryAsState
import androidx.navigation.compose.rememberNavController
import androidx.navigation.navArgument
import com.vibes.dsp.ui.live.LiveScreen
import com.vibes.dsp.ui.modgui.ModguiScreen
import com.vibes.dsp.ui.rack.RackViewModel
import com.vibes.dsp.ui.dashboard.DashboardScreen
import com.vibes.dsp.ui.dashboard.DashboardSection
import com.vibes.dsp.ui.dashboard.DashboardTab
import com.vibes.dsp.ui.settings.SettingsTab
import com.vibes.dsp.ui.tone3000.Tone
import androidx.compose.runtime.remember
import androidx.compose.ui.platform.LocalContext
import com.vibes.dsp.engine.NativeEngine
import com.vibes.dsp.engine.PluginRepositoryService
import com.vibes.dsp.engine.WineInstallOwnership
import com.vibes.dsp.ui.vst.RepositoryVstAdapter
import com.vibes.dsp.ui.dashboard.RepositoryViewModel
import com.vibes.dsp.ui.tone3000.ToneDetailScreen
private const val PENDING_REPOSITORY_PACKAGE_ID = "pendingRepositoryPackageId"

sealed class Screen(val route: String) {
    object Live : Screen("live")
    object Modgui : Screen("modgui/{pluginIndex}?pathId={pathId}&w={w}&h={h}") {
        fun route(pathId: Long, pluginIndex: Int, w: Int = 0, h: Int = 0) = "modgui/$pluginIndex?pathId=$pathId&w=$w&h=$h"
    }
    object Dashboard : Screen(
        "dashboard?section={section}&dashboardTab={dashboardTab}&settingsTab={settingsTab}" +
            "&tag={tag}&gear={gear}&platform={platform}&sourcePlugin={sourcePlugin}&sourceSlot={sourceSlot}"
    ) {
        fun route(
            section: DashboardSection = DashboardSection.Dashboard,
            dashboardTab: DashboardTab = DashboardTab.Main,
            settingsTab: SettingsTab = SettingsTab.Driver,
            tag: String? = null,
            gear: String? = null,
            platform: String? = null,
            sourcePluginIndex: Int = -1,
            sourceSlot: String? = null,
        ): String {
            val parts = mutableListOf(
                "section=${section.argument}",
                "dashboardTab=${dashboardTab.argument}",
                "settingsTab=${settingsTab.argument}",
            )
            tag?.let { parts.add("tag=$it") }
            gear?.let { parts.add("gear=$it") }
            platform?.let { parts.add("platform=$it") }
            if (sourcePluginIndex >= 0) parts.add("sourcePlugin=$sourcePluginIndex")
            sourceSlot?.let { parts.add("sourceSlot=$it") }
            return "dashboard?${parts.joinToString("&")}"
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
    val context = LocalContext.current.applicationContext
    val repositoryService = remember(context) {
        PluginRepositoryService(
            context = context,
            nativeRefresh = {
                runCatching { NativeEngine.getInstance().nativeRefreshPluginRegistry() }.getOrDefault(false)
            },
            removeWineOwnership = { ownership ->
                RepositoryVstAdapter.removeOwnership(context, ownership)
            },
        )
    }
    val repositoryViewModel: RepositoryViewModel = viewModel(
        factory = RepositoryViewModel.factory(repositoryService),
    )
    val rackViewModel: RackViewModel = viewModel()
    var pendingProjectName by rememberSaveable { mutableStateOf<String?>(null) }
    val saveProjectLauncher = rememberLauncherForActivityResult(
        ActivityResultContracts.OpenDocumentTree(),
    ) { uri ->
        val projectName = pendingProjectName
        pendingProjectName = null
        if (uri != null && projectName != null) {
            context.contentResolver.takeProjectPermission(uri)
            rackViewModel.saveProject(uri, projectName)
        }
    }
    val openProjectLauncher = rememberLauncherForActivityResult(
        ActivityResultContracts.OpenDocumentTree(),
    ) { uri ->
        if (uri != null) {
            context.contentResolver.takeProjectPermission(uri)
            rackViewModel.openProject(uri)
        }
    }
    LaunchedEffect(engineReady) {
        if (engineReady) rackViewModel.onNativeEngineReady()
    }

    NavHost(
        navController = navController,
        startDestination = Screen.Live.route,
        modifier = Modifier.fillMaxSize(),
    ) {
        composable(Screen.Live.route) {
            LiveScreen(
                viewModel = rackViewModel,
                onNavigateToDashboard = {
                    navController.navigate(Screen.Dashboard.route())
                },
                onNavigateToTone3000 = { tag, gear, platform, sourcePluginIndex, sourceSlot ->
                    navController.navigate(
                        Screen.Dashboard.route(
                            dashboardTab = DashboardTab.Tone3000,
                            tag = tag,
                            gear = gear,
                            platform = platform,
                            sourcePluginIndex = sourcePluginIndex,
                            sourceSlot = sourceSlot,
                        )
                    )
                }
            )
        }
        composable(
            route = Screen.Dashboard.route,
            arguments = listOf(
                navArgument("section") {
                    type = NavType.StringType
                    defaultValue = DashboardSection.Dashboard.argument
                },
                navArgument("dashboardTab") {
                    type = NavType.StringType
                    defaultValue = DashboardTab.Main.argument
                },
                navArgument("settingsTab") {
                    type = NavType.StringType
                    defaultValue = SettingsTab.Driver.argument
                },
                navArgument("tag") { type = NavType.StringType; nullable = true; defaultValue = null },
                navArgument("gear") { type = NavType.StringType; nullable = true; defaultValue = null },
                navArgument("platform") { type = NavType.StringType; nullable = true; defaultValue = null },
                navArgument("sourcePlugin") { type = NavType.IntType; defaultValue = -1 },
                navArgument("sourceSlot") { type = NavType.StringType; nullable = true; defaultValue = null },
            ),
        ) { entry ->
            val sourcePluginIndex = entry.arguments?.getInt("sourcePlugin") ?: -1
            val sourceSlot = entry.arguments?.getString("sourceSlot")
            val pendingRepositoryPackageId =
                entry.savedStateHandle.get<String>(PENDING_REPOSITORY_PACKAGE_ID)
                    ?: navController.previousBackStackEntry?.savedStateHandle?.get(
                        PENDING_REPOSITORY_PACKAGE_ID,
                    )
            DashboardScreen(
                viewModel = rackViewModel,
                repositoryViewModel = repositoryViewModel,
                repositoryService = repositoryService,
                onSaveProject = { projectName ->
                    pendingProjectName = projectName
                    saveProjectLauncher.launch(null)
                },
                onOpenProject = { openProjectLauncher.launch(null) },
                onRepositoryInstall = { item ->
                    if (item.format == "wine_installer" ||
                        item.format == "wine_archive" ||
                        item.format == "wine_directory"
                    ) {
                        navController.currentBackStackEntry?.savedStateHandle?.set(
                            PENDING_REPOSITORY_PACKAGE_ID,
                            item.id,
                        )
                        navController.navigate(
                            Screen.Dashboard.route(
                                section = DashboardSection.Settings,
                                settingsTab = SettingsTab.Vst,
                            ),
                        )
                    } else if (item.status == com.vibes.dsp.ui.dashboard.RepositoryPackageStatus.Update) {
                        repositoryViewModel.update(item.id)
                    } else {
                        repositoryViewModel.install(item.id)
                    }
                },
                onNavigateToToneDetail = { tone, architecture ->
                    navController.currentBackStackEntry?.savedStateHandle?.set("selected_tone", tone)
                    navController.navigate(
                        Screen.ToneDetail.route(
                            tone.id,
                            sourcePluginIndex,
                            sourceSlot,
                            architecture,
                        )
                    )
                },
                onNavigateBack = { navController.popBackStack() },
                initialSection = DashboardSection.fromArgument(entry.arguments?.getString("section")),
                pendingRepositoryPackageId = pendingRepositoryPackageId,
                onRepositoryHandoff = {
                    entry.savedStateHandle.remove<String>(PENDING_REPOSITORY_PACKAGE_ID)
                    navController.previousBackStackEntry?.savedStateHandle?.remove<String>(
                        PENDING_REPOSITORY_PACKAGE_ID,
                    )
                },
                initialDashboardTab = DashboardTab.fromArgument(entry.arguments?.getString("dashboardTab")),
                initialSettingsTab = SettingsTab.fromArgument(entry.arguments?.getString("settingsTab")),
                initialTag = entry.arguments?.getString("tag"),
                initialGear = entry.arguments?.getString("gear"),
                initialPlatform = entry.arguments?.getString("platform"),
                sourcePluginIndex = sourcePluginIndex,
                sourceSlot = sourceSlot,
            )
        }
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

private fun ContentResolver.takeProjectPermission(uri: Uri) {
    val flags = Intent.FLAG_GRANT_READ_URI_PERMISSION or Intent.FLAG_GRANT_WRITE_URI_PERMISSION
    try {
        takePersistableUriPermission(uri, flags)
    } catch (_: SecurityException) {
        try {
            takePersistableUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION)
        } catch (_: SecurityException) {
            // The provider may not support persistable grants; the immediate grant remains usable.
        }
    }
}
