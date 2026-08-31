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

package com.vibes.dsp.ui.dashboard

import androidx.activity.compose.BackHandler
import androidx.compose.foundation.ExperimentalFoundationApi
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.combinedClickable
import androidx.compose.foundation.clickable
import androidx.compose.foundation.text.KeyboardActions
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.WindowInsetsSides
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.only
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.safeDrawing
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.windowInsetsPadding
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.ArrowBack
import androidx.compose.material.icons.outlined.Info
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Divider
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.saveable.rememberSaveableStateHolder
import androidx.compose.ui.Alignment
import androidx.compose.ui.focus.FocusRequester
import androidx.compose.ui.focus.focusRequester
import androidx.compose.ui.Modifier
import androidx.compose.ui.ExperimentalComposeUiApi
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.ColorFilter
import androidx.compose.ui.platform.LocalSoftwareKeyboardController
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.platform.LocalUriHandler
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.role
import androidx.compose.ui.semantics.selected
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import com.vibes.dsp.BuildConfig
import com.vibes.dsp.R
import com.vibes.dsp.ui.components.NnagaIconButton
import com.vibes.dsp.ui.components.NnagaButton
import com.vibes.dsp.ui.components.NnagaOutlinedButton
import com.vibes.dsp.ui.components.NnagaTextButton
import com.vibes.dsp.ui.components.FontaudioGlyph
import com.vibes.dsp.ui.components.FontaudioIcon
import com.vibes.dsp.ui.components.nnagaOutlinedTextFieldColors
import com.vibes.dsp.engine.PluginRepositoryService
import com.vibes.dsp.ui.dashboard.RepositoryPackageItem
import com.vibes.dsp.ui.rack.RackViewModel
import com.vibes.dsp.ui.settings.SettingsScreen
import com.vibes.dsp.ui.settings.SettingsTab
import com.vibes.dsp.ui.tone3000.Tone
import com.vibes.dsp.ui.tone3000.Tone3000Screen
import kotlin.math.roundToInt

private object DashboardDimensions {
    val topBarHeight = 48.dp
    val innerTabHeight = 44.dp
    val touchTarget = 48.dp
    val icon = 18.dp
    val minimumTopTabWidth = 72.dp
    val divider = 1.dp
    val selectedIndicator = 2.dp
    val brandWidth = 200.dp
    val brandActionWidth = 200.dp
    val brandActionTopSpacing = 24.dp
    val brandActionSpacing = 8.dp
}

enum class DashboardSection(val argument: String) {
    Dashboard("dashboard"),
    Settings("settings");

    companion object {
        fun fromArgument(argument: String?): DashboardSection =
            entries.firstOrNull { it.argument == argument } ?: Dashboard
    }
}

enum class DashboardTab(val argument: String, val label: String) {
    Main("main", "Main"),
    Tone3000("tone3000", "TONE3000"),
    Repository("repository", "Repository");

    companion object {
        fun fromArgument(argument: String?): DashboardTab =
            entries.firstOrNull { it.argument == argument } ?: Main
    }
}

/**
 * Root dashboard surface. It owns Back and both navigation levels; child settings and TONE3000
 * screens are deliberately embedded so they do not render duplicate app bars.
 */
@Composable
fun DashboardScreen(
    viewModel: RackViewModel,
    repositoryViewModel: RepositoryViewModel,
    repositoryService: PluginRepositoryService,
    onRepositoryInstall: (RepositoryPackageItem) -> Unit,
    onSaveProject: (String) -> Unit,
    onOpenProject: () -> Unit,
    pendingRepositoryPackageId: String? = null,
    onRepositoryHandoff: () -> Unit = {},
    onNavigateToToneDetail: (Tone, String?) -> Unit,
    initialSection: DashboardSection = DashboardSection.Dashboard,
    onNavigateBack: () -> Unit,
    initialDashboardTab: DashboardTab = DashboardTab.Main,
    initialSettingsTab: SettingsTab = SettingsTab.Driver,
    initialTag: String? = null,
    initialGear: String? = null,
    initialPlatform: String? = null,
    sourcePathId: Long = -1L,
    sourcePluginIndex: Int = -1,
    sourceSlot: String? = null,
) {
    var selectedSection by rememberSaveable(initialSection) { mutableStateOf(initialSection) }
    var selectedDashboardTab by rememberSaveable(initialDashboardTab) {
        mutableStateOf(initialDashboardTab)
    }
    var fullscreenContent by remember { mutableStateOf(false) }
    val sectionStateHolder = rememberSaveableStateHolder()
    val cutoutBounds = rememberTopCutoutBounds()

    BackHandler(enabled = !fullscreenContent, onBack = onNavigateBack)

    Scaffold(
        contentWindowInsets = WindowInsets(0, 0, 0, 0),
        topBar = {
            if (!fullscreenContent) {
                DashboardTopBar(
                    selectedSection = selectedSection,
                    cutoutBounds = cutoutBounds,
                    onNavigateBack = onNavigateBack,
                    onSectionSelected = { selectedSection = it },
                )
            }
        },
    ) { contentPadding ->
        Box(
            modifier = Modifier
                .fillMaxSize()
                .padding(contentPadding),
        ) {
            sectionStateHolder.SaveableStateProvider(selectedSection.argument) {
                when (selectedSection) {
                    DashboardSection.Dashboard -> DashboardContent(
                        repositoryViewModel = repositoryViewModel,
                        onRepositoryInstall = onRepositoryInstall,
                        onSaveProject = onSaveProject,
                        onOpenProject = onOpenProject,
                        onTabSelected = { selectedDashboardTab = it },
                        selectedTab = selectedDashboardTab,
                        onNavigateBack = onNavigateBack,
                        onNavigateToToneDetail = onNavigateToToneDetail,
                        initialTag = initialTag,
                        initialGear = initialGear,
                        initialPlatform = initialPlatform,
                        sourcePathId = sourcePathId,
                        sourcePluginIndex = sourcePluginIndex,
                        sourceSlot = sourceSlot,
                    )
                    DashboardSection.Settings -> SettingsScreen(
                        viewModel = viewModel,
                        repositoryService = repositoryService,
                        pendingRepositoryPackageId = pendingRepositoryPackageId,
                        onRepositoryHandoff = onRepositoryHandoff,
                        initialTab = initialSettingsTab,
                        onFullscreenChanged = { active -> fullscreenContent = active },
                    )
                }
            }
        }
    }
}

@Composable
private fun DashboardTopBar(
    selectedSection: DashboardSection,
    cutoutBounds: TopCutoutBounds,
    onNavigateBack: () -> Unit,
    onSectionSelected: (DashboardSection) -> Unit,
) {
    Surface(color = MaterialTheme.colorScheme.background) {
        BoxWithConstraints(modifier = Modifier.fillMaxWidth()) {
            val density = LocalDensity.current
            val screenWidthPx = with(density) { maxWidth.toPx().roundToInt() }
            val leftWidth = with(density) { cutoutBounds.left.toDp() }
            val rightWidth = maxWidth - with(density) { cutoutBounds.right.toDp() }
            val cutoutHeight = with(density) { cutoutBounds.bottom.toDp() }
            val usesCutoutZones = cutoutBounds.isValidFor(screenWidthPx) &&
                leftWidth >= DashboardDimensions.touchTarget + DashboardDimensions.minimumTopTabWidth &&
                rightWidth >= DashboardDimensions.minimumTopTabWidth

            if (usesCutoutZones) {
                Box(
                    modifier = Modifier
                        .fillMaxWidth()
                        .height(maxOf(DashboardDimensions.topBarHeight, cutoutHeight)),
                ) {
                    Row(
                        modifier = Modifier
                            .align(Alignment.TopStart)
                            .width(leftWidth)
                            .height(DashboardDimensions.topBarHeight),
                        verticalAlignment = Alignment.CenterVertically,
                    ) {
                        BackButton(onNavigateBack)
                        TopLevelTab(
                            text = "Dashboard",
                            selected = selectedSection == DashboardSection.Dashboard,
                            onClick = { onSectionSelected(DashboardSection.Dashboard) },
                            modifier = Modifier.weight(1f),
                        )
                    }
                    TopLevelTab(
                        text = "Settings",
                        selected = selectedSection == DashboardSection.Settings,
                        onClick = { onSectionSelected(DashboardSection.Settings) },
                        modifier = Modifier
                            .align(Alignment.TopEnd)
                            .width(rightWidth),
                    )
                    Divider(
                        modifier = Modifier.align(Alignment.BottomCenter),
                        thickness = DashboardDimensions.divider,
                        color = MaterialTheme.colorScheme.outlineVariant,
                    )
                }
            } else {
                Column(
                    modifier = Modifier.windowInsetsPadding(
                        WindowInsets.safeDrawing.only(WindowInsetsSides.Top + WindowInsetsSides.Horizontal),
                    ),
                ) {
                    Row(
                        modifier = Modifier
                            .fillMaxWidth()
                            .height(DashboardDimensions.topBarHeight),
                        verticalAlignment = Alignment.CenterVertically,
                    ) {
                        BackButton(onNavigateBack)
                        TopLevelTab(
                            text = "Dashboard",
                            selected = selectedSection == DashboardSection.Dashboard,
                            onClick = { onSectionSelected(DashboardSection.Dashboard) },
                            modifier = Modifier.weight(1f),
                        )
                        TopLevelTab(
                            text = "Settings",
                            selected = selectedSection == DashboardSection.Settings,
                            onClick = { onSectionSelected(DashboardSection.Settings) },
                            modifier = Modifier.weight(1f),
                        )
                    }
                    Divider(
                        thickness = DashboardDimensions.divider,
                        color = MaterialTheme.colorScheme.outlineVariant,
                    )
                }
            }
        }
    }
}

@Composable
private fun BackButton(onNavigateBack: () -> Unit) {
    NnagaIconButton(
        onClick = onNavigateBack,
        modifier = Modifier.size(DashboardDimensions.touchTarget),
    ) {
        Icon(
            imageVector = Icons.Default.ArrowBack,
            contentDescription = "Back",
            modifier = Modifier.size(DashboardDimensions.icon),
        )
    }
}

@Composable
private fun TopLevelTab(
    text: String,
    selected: Boolean,
    onClick: () -> Unit,
    modifier: Modifier = Modifier,
) {
    Box(
        modifier = modifier
            .height(DashboardDimensions.topBarHeight)
            .semantics {
                this.selected = selected
                role = Role.Tab
            }
            .clickable(role = Role.Tab, onClick = onClick),
        contentAlignment = Alignment.Center,
    ) {
        Text(
            text = text,
            style = MaterialTheme.typography.labelLarge,
            color = if (selected) {
                MaterialTheme.colorScheme.primary
            } else {
                MaterialTheme.colorScheme.onSurfaceVariant
            },
            fontWeight = if (selected) FontWeight.SemiBold else FontWeight.Normal,
            maxLines = 1,
        )
        if (selected) {
            Box(
                modifier = Modifier
                    .align(Alignment.BottomCenter)
                    .fillMaxWidth()
                    .height(DashboardDimensions.selectedIndicator)
                    .background(MaterialTheme.colorScheme.primary),
            )
        }
    }
}

@Composable
private fun DashboardContent(
    repositoryViewModel: RepositoryViewModel,
    onRepositoryInstall: (RepositoryPackageItem) -> Unit,
    onSaveProject: (String) -> Unit,
    onOpenProject: () -> Unit,
    selectedTab: DashboardTab,
    onTabSelected: (DashboardTab) -> Unit,
    onNavigateBack: () -> Unit,
    onNavigateToToneDetail: (Tone, String?) -> Unit,
    initialTag: String?,
    initialGear: String?,
    initialPlatform: String?,
    sourcePathId: Long,
    sourcePluginIndex: Int,
    sourceSlot: String?,
) {
    val tabStateHolder = rememberSaveableStateHolder()
    var manageRepositorySources by rememberSaveable { mutableStateOf(false) }
    Column(modifier = Modifier.fillMaxSize()) {
        InnerTabs(
            selectedTab = selectedTab,
            onTabSelected = { tab ->
                manageRepositorySources = false
                onTabSelected(tab)
            },
            onManageRepositorySources = {
                manageRepositorySources = true
                onTabSelected(DashboardTab.Repository)
            },
        )
        tabStateHolder.SaveableStateProvider(selectedTab.argument) {
            when (selectedTab) {
                DashboardTab.Repository -> RepositoryScreen(
                    viewModel = repositoryViewModel,
                    onInstallPackage = onRepositoryInstall,
                    onUpdatePackage = onRepositoryInstall,
                    manageSources = manageRepositorySources,
                    onCloseSourceManagement = { manageRepositorySources = false },
                    modifier = Modifier.weight(1f),
                )
                DashboardTab.Main -> BrandHome(
                    onSaveProject = onSaveProject,
                    onOpenProject = onOpenProject,
                    modifier = Modifier.weight(1f),
                )
                DashboardTab.Tone3000 -> Tone3000Screen(
                    onNavigateBack = onNavigateBack,
                    onNavigateToDetail = onNavigateToToneDetail,
                    initialTag = initialTag,
                    initialGear = initialGear,
                    initialPlatform = initialPlatform,
                    sourcePathId = sourcePathId,
                    sourcePluginIndex = sourcePluginIndex,
                    sourceSlot = sourceSlot,
                    embedded = true,
                )
            }
        }
    }
}

@OptIn(ExperimentalFoundationApi::class)
@Composable
private fun InnerTabs(
    selectedTab: DashboardTab,
    onTabSelected: (DashboardTab) -> Unit,
    onManageRepositorySources: () -> Unit,
) {
    Column {
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .height(DashboardDimensions.innerTabHeight),
        ) {
            DashboardTab.entries.forEach { tab ->
                val selected = tab == selectedTab
                Box(
                    modifier = Modifier
                        .weight(1f)
                        .fillMaxHeight()
                        .semantics {
                            this.selected = selected
                            role = Role.Tab
                        }
                        .combinedClickable(
                            role = Role.Tab,
                            onLongClickLabel = if (tab == DashboardTab.Repository) {
                                "Manage repository sources"
                            } else {
                                null
                            },
                            onLongClick = if (tab == DashboardTab.Repository) {
                                onManageRepositorySources
                            } else {
                                null
                            },
                            onClick = { onTabSelected(tab) },
                        ),
                    contentAlignment = Alignment.Center,
                ) {
                    Text(
                        text = tab.label,
                        style = MaterialTheme.typography.labelMedium,
                        color = if (selected) {
                            MaterialTheme.colorScheme.onSurface
                        } else {
                            MaterialTheme.colorScheme.onSurfaceVariant
                        },
                        maxLines = 1,
                    )
                    if (selected) {
                        Box(
                            modifier = Modifier
                                .align(Alignment.BottomCenter)
                                .fillMaxWidth()
                                .height(DashboardDimensions.selectedIndicator)
                                .background(MaterialTheme.colorScheme.primary),
                        )
                    }
                }
            }
        }
        Divider(
            thickness = DashboardDimensions.divider,
            color = MaterialTheme.colorScheme.outlineVariant,
        )
    }
}

@Composable
private fun BrandHome(
    onSaveProject: (String) -> Unit,
    onOpenProject: () -> Unit,
    modifier: Modifier = Modifier,
) {
    var showAbout by rememberSaveable { mutableStateOf(false) }
    var showProjectNameDialog by rememberSaveable { mutableStateOf(false) }
    var projectName by rememberSaveable { mutableStateOf(DEFAULT_PROJECT_NAME) }
    var projectNameTouched by rememberSaveable { mutableStateOf(false) }
    val normalizedProjectName = normalizeProjectName(projectName)
    val projectNameError = projectNameError(projectName)
    val uriHandler = LocalUriHandler.current

    Box(
        modifier = modifier.fillMaxWidth(),
        contentAlignment = Alignment.Center,
    ) {
        Column(
            horizontalAlignment = Alignment.CenterHorizontally,
        ) {
            Image(
                painter = painterResource(R.drawable.nnaga_brand_mark),
                contentDescription = "NNAGA",
                modifier = Modifier.width(DashboardDimensions.brandWidth),
                contentScale = ContentScale.FillWidth,
                colorFilter = ColorFilter.tint(MaterialTheme.colorScheme.primary),
            )
            Spacer(modifier = Modifier.height(DashboardDimensions.brandActionTopSpacing))
            NnagaButton(
                onClick = {
                    projectName = DEFAULT_PROJECT_NAME
                    projectNameTouched = false
                    showProjectNameDialog = true
                },
                modifier = Modifier.width(DashboardDimensions.brandActionWidth),
            ) {
                Row(
                    horizontalArrangement = Arrangement.spacedBy(8.dp),
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    FontaudioIcon(
                        glyph = FontaudioGlyph.SAVE,
                        contentDescription = null,
                        size = 18.dp,
                    )
                    Text("Save Project")
                }
            }
            Spacer(modifier = Modifier.height(DashboardDimensions.brandActionSpacing))
            NnagaOutlinedButton(
                onClick = onOpenProject,
                modifier = Modifier.width(DashboardDimensions.brandActionWidth),
            ) {
                Row(
                    horizontalArrangement = Arrangement.spacedBy(8.dp),
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    FontaudioIcon(
                        glyph = FontaudioGlyph.OPEN,
                        contentDescription = null,
                        size = 18.dp,
                    )
                    Text("Open Project Folder")
                }
            }
            Text(
                text = "Select the .nnaga project directory.",
                modifier = Modifier
                    .padding(top = DashboardDimensions.brandActionSpacing)
                    .width(DashboardDimensions.brandActionWidth),
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
        Text(
            text = "NNAGA v${BuildConfig.VERSION_DISPLAY_NAME}",
            modifier = Modifier
                .align(Alignment.TopStart)
                .padding(12.dp),
            color = Color.White,
            style = MaterialTheme.typography.labelMedium,
        )
        NnagaIconButton(
            onClick = { showAbout = true },
            modifier = Modifier
                .align(Alignment.TopEnd)
                .padding(12.dp),
        ) {
            Icon(
                imageVector = Icons.Outlined.Info,
                contentDescription = "About NNAGA",
                tint = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
    }

    if (showProjectNameDialog) {
        ProjectNameDialog(
            projectName = projectName,
            error = if (projectNameTouched) projectNameError else null,
            confirmEnabled = projectNameError == null,
            normalizedProjectName = normalizedProjectName,
            onProjectNameChange = {
                projectName = it
                projectNameTouched = true
            },
            onDismiss = { showProjectNameDialog = false },
            onConfirm = {
                normalizedProjectName?.let { name ->
                    showProjectNameDialog = false
                    onSaveProject(name)
                }
            },
        )
    }

    if (showAbout) {
        AlertDialog(
            onDismissRequest = { showAbout = false },
            title = { Text("About NNAGA") },
            text = {
                Column {
                    Text("NNAGA is a fork of Guitar RackCraft.")
                    Text(
                        "Thank you to Varcain, the original author, and to every upstream contributor.",
                        modifier = Modifier.padding(top = 12.dp),
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                    NnagaTextButton(
                        onClick = {
                            uriHandler.openUri("https://github.com/Varcain/GuitarRackCraft")
                        },
                        modifier = Modifier.padding(top = 8.dp),
                    ) {
                        Text("Original repository")
                    }
                    NnagaTextButton(
                        onClick = {
                            uriHandler.openUri("https://github.com/patlach42/NNAGA")
                        },
                    ) {
                        Text("NNAGA fork on GitHub")
                    }
                }
            },
            confirmButton = {
                NnagaTextButton(onClick = { showAbout = false }) {
                    Text("Close")
                }
            },
        )
    }
}

private const val PROJECT_SUFFIX = ".nnaga"
private const val DEFAULT_PROJECT_NAME = "Project"
private val SIMPLE_PROJECT_NAME = Regex("[\\p{L}\\p{N}][\\p{L}\\p{N} _-]*")

private fun normalizeProjectName(input: String): String? {
    val trimmed = input.trim()
    val baseName = if (trimmed.endsWith(PROJECT_SUFFIX, ignoreCase = true)) {
        trimmed.dropLast(PROJECT_SUFFIX.length).trimEnd()
    } else {
        trimmed
    }
    return if (baseName.matches(SIMPLE_PROJECT_NAME)) "$baseName$PROJECT_SUFFIX" else null
}

private fun projectNameError(input: String): String? {
    val trimmed = input.trim()
    if (trimmed.isEmpty()) return "Enter a project name."
    return if (normalizeProjectName(trimmed) == null) {
        "Use letters, numbers, spaces, hyphens, or underscores."
    } else {
        null
    }
}

@OptIn(ExperimentalComposeUiApi::class)
@Composable
private fun ProjectNameDialog(
    projectName: String,
    error: String?,
    confirmEnabled: Boolean,
    normalizedProjectName: String?,
    onProjectNameChange: (String) -> Unit,
    onDismiss: () -> Unit,
    onConfirm: () -> Unit,
) {
    val focusRequester = remember { FocusRequester() }
    val keyboardController = LocalSoftwareKeyboardController.current

    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text("Save Project") },
        text = {
            Column(verticalArrangement = Arrangement.spacedBy(DashboardDimensions.brandActionSpacing)) {
                Text(
                    text = "Name the project, then select its parent folder. NNAGA will create a new .nnaga directory there.",
                    style = MaterialTheme.typography.bodyMedium,
                )
                OutlinedTextField(
                    value = projectName,
                    onValueChange = onProjectNameChange,
                    modifier = Modifier
                        .fillMaxWidth()
                        .focusRequester(focusRequester),
                    label = { Text("Project name") },
                    supportingText = {
                        Text(
                            text = error ?: normalizedProjectName.orEmpty(),
                            color = if (error != null) {
                                MaterialTheme.colorScheme.error
                            } else {
                                MaterialTheme.colorScheme.onSurfaceVariant
                            },
                        )
                    },
                    isError = error != null,
                    singleLine = true,
                    keyboardOptions = KeyboardOptions(imeAction = ImeAction.Done),
                    keyboardActions = KeyboardActions(
                        onDone = {
                            if (confirmEnabled) {
                                keyboardController?.hide()
                                onConfirm()
                            }
                        },
                    ),
                    colors = nnagaOutlinedTextFieldColors(),
                )
            }
        },
        confirmButton = {
            NnagaButton(
                onClick = onConfirm,
                enabled = confirmEnabled,
            ) {
                Text("Choose Parent Folder")
            }
        },
        dismissButton = {
            NnagaTextButton(onClick = onDismiss) {
                Text("Cancel")
            }
        },
    )

    androidx.compose.runtime.LaunchedEffect(Unit) {
        focusRequester.requestFocus()
    }
}
