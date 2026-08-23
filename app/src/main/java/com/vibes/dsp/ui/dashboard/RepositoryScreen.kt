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

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Add
import androidx.compose.material.icons.filled.Delete
import androidx.compose.material.icons.filled.Refresh
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.Divider
import androidx.compose.material3.Icon
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.SnackbarDuration
import androidx.compose.material3.SnackbarHost
import androidx.compose.material3.SnackbarHostState
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.foundation.text.KeyboardActions
import androidx.compose.foundation.text.KeyboardOptions
import com.vibes.dsp.ui.components.NnagaButton
import com.vibes.dsp.ui.components.NnagaIconButton
import com.vibes.dsp.ui.components.NnagaOutlinedButton
import com.vibes.dsp.ui.components.NnagaSwitch
import com.vibes.dsp.ui.components.NnagaTextButton
import com.vibes.dsp.ui.components.NnagaTonalButton
import com.vibes.dsp.ui.components.nnagaOutlinedTextFieldColors

private object RepositoryDimensions {
    val compactPadding = 8.dp
    val contentPadding = 12.dp
    val sectionSpacing = 16.dp
    val itemSpacing = 8.dp
    val inlineSpacing = 4.dp
    val divider = 1.dp
    val progressHeight = 2.dp
    val actionProgress = 16.dp
    val sourcePaneWidth = 320.dp
    val contentMaxWidth = 1080.dp
    val wideBreakpoint = 720.dp
}

@Composable
fun RepositoryScreen(
    viewModel: RepositoryViewModel,
    onInstallPackage: (RepositoryPackageItem) -> Unit = { viewModel.install(it.id) },
    onUpdatePackage: (RepositoryPackageItem) -> Unit = { viewModel.update(it.id) },
    modifier: Modifier = Modifier,
) {
    val snapshot by viewModel.snapshot.collectAsState()
    val actionState by viewModel.actionState.collectAsState()
    val snackbarHostState = remember { SnackbarHostState() }
    var sourceUrl by rememberSaveable { mutableStateOf("") }
    var addWasPending by remember { mutableStateOf(false) }
    val addPending = actionState.isPending(RepositoryViewModel.ADD_SOURCE)

    LaunchedEffect(actionState.errorMessage) {
        actionState.errorMessage?.let { message ->
            snackbarHostState.showSnackbar(message, duration = SnackbarDuration.Long)
            viewModel.clearActionError()
        }
    }
    LaunchedEffect(addPending) {
        if (addWasPending && !addPending && actionState.errorMessage == null) sourceUrl = ""
        addWasPending = addPending
    }

    Box(modifier = modifier.fillMaxSize()) {
        Column(modifier = Modifier.fillMaxSize()) {
            RepositoryToolbar(
                sourceUrl = sourceUrl,
                onSourceUrlChanged = { sourceUrl = it },
                onAddSource = { viewModel.addSource(sourceUrl) },
                onRefresh = viewModel::refreshAll,
                addingSource = addPending,
                refreshing = snapshot.isRefreshing ||
                    actionState.isPending(RepositoryViewModel.REFRESH_ALL),
            )
            snapshot.errorMessage
                ?.takeIf { snapshot.sources.isNotEmpty() || snapshot.packages.isNotEmpty() }
                ?.let { message ->
                    RepositoryErrorBanner(
                        message = message,
                        onRetry = viewModel::refreshAll,
                    )
                }
            Divider(
                thickness = RepositoryDimensions.divider,
                color = MaterialTheme.colorScheme.outlineVariant,
            )
            RepositoryContent(
                snapshot = snapshot,
                actionState = actionState,
                onRetry = viewModel::refreshAll,
                onSourceEnabledChanged = viewModel::setSourceEnabled,
                onRefreshSource = viewModel::refreshSource,
                onRemoveSource = viewModel::removeSource,
                onInstall = onInstallPackage,
                onUpdate = { id ->
                    snapshot.packages.firstOrNull { it.id == id }?.let(onUpdatePackage)
                },
                onRemovePackage = viewModel::remove,
                modifier = Modifier.weight(1f),
            )
        }
        SnackbarHost(
            hostState = snackbarHostState,
            modifier = Modifier
                .align(Alignment.BottomCenter)
                .padding(RepositoryDimensions.contentPadding),
        )
    }
}

@Composable
private fun RepositoryToolbar(
    sourceUrl: String,
    onSourceUrlChanged: (String) -> Unit,
    onAddSource: () -> Unit,
    onRefresh: () -> Unit,
    addingSource: Boolean,
    refreshing: Boolean,
) {
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .padding(RepositoryDimensions.compactPadding),
        horizontalArrangement = Arrangement.spacedBy(RepositoryDimensions.compactPadding),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        OutlinedTextField(
            value = sourceUrl,
            onValueChange = onSourceUrlChanged,
            modifier = Modifier.weight(1f),
            placeholder = { Text("Repository URL") },
            singleLine = true,
            enabled = !addingSource,
            keyboardOptions = KeyboardOptions(
                keyboardType = KeyboardType.Uri,
                imeAction = ImeAction.Done,
            ),
            keyboardActions = KeyboardActions(
                onDone = { if (sourceUrl.isNotBlank() && !addingSource) onAddSource() },
            ),
            shape = MaterialTheme.shapes.small,
            colors = nnagaOutlinedTextFieldColors(),
        )
        NnagaButton(
            onClick = onAddSource,
            enabled = sourceUrl.isNotBlank() && !addingSource,
        ) {
            if (addingSource) {
                CircularProgressIndicator(
                    modifier = Modifier.size(RepositoryDimensions.actionProgress),
                    strokeWidth = RepositoryDimensions.progressHeight,
                )
            } else {
                Icon(Icons.Default.Add, contentDescription = null)
            }
            Spacer(modifier = Modifier.width(RepositoryDimensions.inlineSpacing))
            Text("Add")
        }
        NnagaIconButton(onClick = onRefresh, enabled = !refreshing) {
            if (refreshing) {
                CircularProgressIndicator(
                    modifier = Modifier.size(RepositoryDimensions.actionProgress),
                    strokeWidth = RepositoryDimensions.progressHeight,
                )
            } else {
                Icon(Icons.Default.Refresh, contentDescription = "Refresh all repositories")
            }
        }
    }
}

@Composable
private fun RepositoryContent(
    snapshot: RepositorySnapshot,
    actionState: RepositoryActionState,
    onRetry: () -> Unit,
    onSourceEnabledChanged: (String, Boolean) -> Unit,
    onRefreshSource: (String) -> Unit,
    onRemoveSource: (String) -> Unit,
    onInstall: (RepositoryPackageItem) -> Unit,
    onUpdate: (String) -> Unit,
    onRemovePackage: (String) -> Unit,
    modifier: Modifier = Modifier,
) {
    when {
        snapshot.isLoading && snapshot.sources.isEmpty() && snapshot.packages.isEmpty() -> {
            Box(modifier = modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
                CircularProgressIndicator(strokeWidth = RepositoryDimensions.progressHeight)
            }
        }
        snapshot.errorMessage != null && snapshot.sources.isEmpty() && snapshot.packages.isEmpty() -> {
            RepositoryFailure(
                message = snapshot.errorMessage,
                onRetry = onRetry,
                modifier = modifier,
            )
        }
        else -> BoxWithConstraints(modifier = modifier.fillMaxSize()) {
            val wide = maxWidth >= RepositoryDimensions.wideBreakpoint
            if (wide) {
                Row(
                    modifier = Modifier
                        .fillMaxSize()
                        .widthIn(max = RepositoryDimensions.contentMaxWidth)
                        .align(Alignment.TopCenter),
                ) {
                    SourceList(
                        sources = snapshot.sources,
                        actionState = actionState,
                        onEnabledChanged = onSourceEnabledChanged,
                        onRefresh = onRefreshSource,
                        onRemove = onRemoveSource,
                        modifier = Modifier
                            .width(RepositoryDimensions.sourcePaneWidth)
                            .fillMaxHeight(),
                    )
                    Divider(
                        modifier = Modifier
                            .width(RepositoryDimensions.divider)
                            .fillMaxHeight(),
                        thickness = RepositoryDimensions.divider,
                        color = MaterialTheme.colorScheme.outlineVariant,
                    )
                    PackageList(
                        packages = snapshot.packages,
                        actionState = actionState,
                        onInstall = onInstall,
                        onUpdate = onUpdate,
                        onRemove = onRemovePackage,
                        modifier = Modifier
                            .weight(1f)
                            .fillMaxHeight(),
                    )
                }
            } else {
                CompactRepositoryList(
                    snapshot = snapshot,
                    actionState = actionState,
                    onSourceEnabledChanged = onSourceEnabledChanged,
                    onRefreshSource = onRefreshSource,
                    onRemoveSource = onRemoveSource,
                    onInstall = onInstall,
                    onUpdate = onUpdate,
                    onRemovePackage = onRemovePackage,
                )
            }
        }
    }
}

@Composable
private fun SourceList(
    sources: List<RepositorySourceItem>,
    actionState: RepositoryActionState,
    onEnabledChanged: (String, Boolean) -> Unit,
    onRefresh: (String) -> Unit,
    onRemove: (String) -> Unit,
    modifier: Modifier = Modifier,
) {
    LazyColumn(
        modifier = modifier,
        contentPadding = PaddingValues(RepositoryDimensions.contentPadding),
        verticalArrangement = Arrangement.spacedBy(RepositoryDimensions.itemSpacing),
    ) {
        item { SectionHeading("Sources", "Enable repositories that contribute packages.") }
        if (sources.isEmpty()) {
            item {
                EmptyMessage("Add a repository URL above to discover plugin packages.")
            }
        } else {
            items(sources, key = { it.id }) { source ->
                SourceRow(
                    source = source,
                    pending = actionState.isPending(RepositoryViewModel.sourceKey(source.id)),
                    onEnabledChanged = { onEnabledChanged(source.id, it) },
                    onRefresh = { onRefresh(source.id) },
                    onRemove = { onRemove(source.id) },
                )
            }
        }
    }
}

@Composable
private fun PackageList(
    packages: List<RepositoryPackageItem>,
    actionState: RepositoryActionState,
    onInstall: (RepositoryPackageItem) -> Unit,
    onUpdate: (String) -> Unit,
    onRemove: (String) -> Unit,
    modifier: Modifier = Modifier,
) {
    LazyColumn(
        modifier = modifier,
        contentPadding = PaddingValues(RepositoryDimensions.contentPadding),
        verticalArrangement = Arrangement.spacedBy(RepositoryDimensions.itemSpacing),
    ) {
        item { SectionHeading("Packages", "Install and maintain plugins from enabled sources.") }
        if (packages.isEmpty()) {
            item {
                EmptyMessage("Enable a source and refresh repositories to discover packages.")
            }
        } else {
            items(packages, key = { it.id }) { repositoryPackage ->
                PackageRow(
                    repositoryPackage = repositoryPackage,
                    pending = actionState.isPending(
                        RepositoryViewModel.packageKey(repositoryPackage.id),
                    ),
                    onInstall = { onInstall(repositoryPackage) },
                    onUpdate = { onUpdate(repositoryPackage.id) },
                    onRemove = { onRemove(repositoryPackage.id) },
                )
            }
        }
    }
}

@Composable
private fun CompactRepositoryList(
    snapshot: RepositorySnapshot,
    actionState: RepositoryActionState,
    onSourceEnabledChanged: (String, Boolean) -> Unit,
    onRefreshSource: (String) -> Unit,
    onRemoveSource: (String) -> Unit,
    onInstall: (RepositoryPackageItem) -> Unit,
    onUpdate: (String) -> Unit,
    onRemovePackage: (String) -> Unit,
) {
    LazyColumn(
        modifier = Modifier.fillMaxSize(),
        contentPadding = PaddingValues(RepositoryDimensions.contentPadding),
        verticalArrangement = Arrangement.spacedBy(RepositoryDimensions.itemSpacing),
    ) {
        item { SectionHeading("Sources", "Enable repositories that contribute packages.") }
        if (snapshot.sources.isEmpty()) {
            item { EmptyMessage("Add a repository URL above to discover plugin packages.") }
        } else {
            items(snapshot.sources, key = { "source:${it.id}" }) { source ->
                SourceRow(
                    source = source,
                    pending = actionState.isPending(RepositoryViewModel.sourceKey(source.id)),
                    onEnabledChanged = { onSourceEnabledChanged(source.id, it) },
                    onRefresh = { onRefreshSource(source.id) },
                    onRemove = { onRemoveSource(source.id) },
                )
            }
        }
        item {
            Spacer(modifier = Modifier.height(RepositoryDimensions.compactPadding))
            SectionHeading("Packages", "Install and maintain plugins from enabled sources.")
        }
        if (snapshot.packages.isEmpty()) {
            item { EmptyMessage("Enable a source and refresh repositories to discover packages.") }
        } else {
            items(snapshot.packages, key = { "package:${it.id}" }) { repositoryPackage ->
                PackageRow(
                    repositoryPackage = repositoryPackage,
                    pending = actionState.isPending(
                        RepositoryViewModel.packageKey(repositoryPackage.id),
                    ),
                    onInstall = { onInstall(repositoryPackage) },
                    onUpdate = { onUpdate(repositoryPackage.id) },
                    onRemove = { onRemovePackage(repositoryPackage.id) },
                )
            }
        }
    }
}

@Composable
private fun SourceRow(
    source: RepositorySourceItem,
    pending: Boolean,
    onEnabledChanged: (Boolean) -> Unit,
    onRefresh: () -> Unit,
    onRemove: () -> Unit,
) {
    Surface(
        color = MaterialTheme.colorScheme.surfaceVariant,
        shape = MaterialTheme.shapes.small,
    ) {
        Column(modifier = Modifier.padding(RepositoryDimensions.contentPadding)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Column(modifier = Modifier.weight(1f)) {
                    Text(
                        text = source.name,
                        style = MaterialTheme.typography.titleSmall,
                        fontWeight = FontWeight.SemiBold,
                        maxLines = 1,
                        overflow = TextOverflow.Ellipsis,
                    )
                    Text(
                        text = source.url,
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                        maxLines = 2,
                        overflow = TextOverflow.Ellipsis,
                    )
                }
                NnagaSwitch(
                    checked = source.enabled,
                    onCheckedChange = onEnabledChanged,
                    enabled = !pending,
                )
            }
            source.errorMessage?.let { message ->
                Text(
                    text = message,
                    modifier = Modifier.padding(top = RepositoryDimensions.compactPadding),
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.error,
                )
            }
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.End,
                verticalAlignment = Alignment.CenterVertically,
            ) {
                if (pending) {
                    CircularProgressIndicator(
                        modifier = Modifier.size(RepositoryDimensions.actionProgress),
                        strokeWidth = RepositoryDimensions.progressHeight,
                    )
                    Spacer(modifier = Modifier.width(RepositoryDimensions.compactPadding))
                }
                NnagaTextButton(onClick = onRefresh, enabled = source.enabled && !pending) {
                    Icon(Icons.Default.Refresh, contentDescription = null)
                    Spacer(modifier = Modifier.width(RepositoryDimensions.inlineSpacing))
                    Text("Refresh")
                }
                if (source.isCustom) {
                    NnagaIconButton(onClick = onRemove, enabled = !pending) {
                        Icon(
                            Icons.Default.Delete,
                            contentDescription = "Remove ${source.name}",
                            tint = MaterialTheme.colorScheme.error,
                        )
                    }
                }
            }
        }
    }
}

@Composable
private fun PackageRow(
    repositoryPackage: RepositoryPackageItem,
    pending: Boolean,
    onInstall: () -> Unit,
    onUpdate: () -> Unit,
    onRemove: () -> Unit,
) {
    val operating = pending || repositoryPackage.operation != null
    Surface(
        color = MaterialTheme.colorScheme.surfaceVariant,
        shape = MaterialTheme.shapes.small,
    ) {
        Column(modifier = Modifier.padding(RepositoryDimensions.contentPadding)) {
            Row(
                horizontalArrangement = Arrangement.spacedBy(RepositoryDimensions.compactPadding),
                verticalAlignment = Alignment.Top,
            ) {
                Column(modifier = Modifier.weight(1f)) {
                    Text(
                        text = repositoryPackage.name,
                        style = MaterialTheme.typography.titleSmall,
                        fontWeight = FontWeight.SemiBold,
                    )
                    Text(
                        text = "${repositoryPackage.format} · ${repositoryPackage.sourceName}",
                        style = MaterialTheme.typography.labelSmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
                PackageStatusBadge(repositoryPackage.status)
            }
            repositoryPackage.description?.takeIf { it.isNotBlank() }?.let { description ->
                Text(
                    text = description,
                    modifier = Modifier.padding(top = RepositoryDimensions.compactPadding),
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    maxLines = 3,
                    overflow = TextOverflow.Ellipsis,
                )
            }
            VersionText(repositoryPackage)
            repositoryPackage.errorMessage?.let { message ->
                Text(
                    text = message,
                    modifier = Modifier.padding(top = RepositoryDimensions.compactPadding),
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.error,
                )
            }
            if (operating) {
                Text(
                    text = repositoryPackage.operation?.let(::operationLabel) ?: "Working…",
                    modifier = Modifier.padding(top = RepositoryDimensions.compactPadding),
                    style = MaterialTheme.typography.labelMedium,
                    color = MaterialTheme.colorScheme.primary,
                )
                val progress = repositoryPackage.progress
                if (progress == null) {
                    LinearProgressIndicator(
                        modifier = Modifier
                            .fillMaxWidth()
                            .padding(top = RepositoryDimensions.inlineSpacing)
                            .height(RepositoryDimensions.progressHeight),
                    )
                } else {
                    LinearProgressIndicator(
                        progress = progress.coerceIn(0f, 1f),
                        modifier = Modifier
                            .fillMaxWidth()
                            .padding(top = RepositoryDimensions.inlineSpacing)
                            .height(RepositoryDimensions.progressHeight),
                    )
                }
            }
            PackageActions(
                repositoryPackage = repositoryPackage,
                enabled = !operating &&
                    (com.vibes.dsp.BuildConfig.HAS_VST_HOST ||
                        !repositoryPackage.format.startsWith("wine_")),
                onInstall = onInstall,
                onUpdate = onUpdate,
                onRemove = onRemove,
            )
        }
    }
}

@Composable
private fun VersionText(repositoryPackage: RepositoryPackageItem) {
    val versionText = when {
        repositoryPackage.installedVersion != null && repositoryPackage.availableVersion != null &&
            repositoryPackage.installedVersion != repositoryPackage.availableVersion ->
            "${repositoryPackage.installedVersion} installed · ${repositoryPackage.availableVersion} available"
        repositoryPackage.installedVersion != null ->
            "Version ${repositoryPackage.installedVersion} installed"
        repositoryPackage.availableVersion != null ->
            "Version ${repositoryPackage.availableVersion} available"
        else -> null
    }
    versionText?.let {
        Text(
            text = it,
            modifier = Modifier.padding(top = RepositoryDimensions.compactPadding),
            style = MaterialTheme.typography.labelSmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
    }
}

@Composable
private fun PackageActions(
    repositoryPackage: RepositoryPackageItem,
    enabled: Boolean,
    onInstall: () -> Unit,
    onUpdate: () -> Unit,
    onRemove: () -> Unit,
) {
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .padding(top = RepositoryDimensions.compactPadding),
        horizontalArrangement = Arrangement.End,
        verticalAlignment = Alignment.CenterVertically,
    ) {
        when (repositoryPackage.status) {
            RepositoryPackageStatus.Incompatible ->
                NnagaTonalButton(onClick = {}, enabled = false) { Text("Incompatible") }
            RepositoryPackageStatus.Available ->
                NnagaButton(onClick = onInstall, enabled = enabled) { Text("Install") }
            RepositoryPackageStatus.Installed ->
                NnagaOutlinedButton(onClick = onRemove, enabled = enabled) { Text("Remove") }
            RepositoryPackageStatus.Update -> {
                NnagaTextButton(onClick = onRemove, enabled = enabled) { Text("Remove") }
                NnagaButton(onClick = onUpdate, enabled = enabled) { Text("Update") }
            }
            RepositoryPackageStatus.Error -> {
                if (repositoryPackage.installedVersion == null) {
                    NnagaTonalButton(onClick = onInstall, enabled = enabled) { Text("Retry install") }
                } else {
                    NnagaTextButton(onClick = onRemove, enabled = enabled) { Text("Remove") }
                    if (
                        repositoryPackage.availableVersion != null &&
                        repositoryPackage.availableVersion != repositoryPackage.installedVersion
                    ) {
                        NnagaTonalButton(onClick = onUpdate, enabled = enabled) {
                            Text("Retry update")
                        }
                    }
                }
            }
        }
    }
}

@Composable
private fun PackageStatusBadge(status: RepositoryPackageStatus) {
    val (containerColor, contentColor) = when (status) {
        RepositoryPackageStatus.Available ->
            MaterialTheme.colorScheme.tertiaryContainer to MaterialTheme.colorScheme.onTertiaryContainer
        RepositoryPackageStatus.Installed ->
            MaterialTheme.colorScheme.secondaryContainer to MaterialTheme.colorScheme.onSecondaryContainer
        RepositoryPackageStatus.Update ->
            MaterialTheme.colorScheme.primaryContainer to MaterialTheme.colorScheme.onPrimaryContainer
        RepositoryPackageStatus.Incompatible ->
            MaterialTheme.colorScheme.surfaceVariant to MaterialTheme.colorScheme.onSurfaceVariant
        RepositoryPackageStatus.Error ->
            MaterialTheme.colorScheme.errorContainer to MaterialTheme.colorScheme.onErrorContainer
    }
    Surface(
        color = containerColor,
        contentColor = contentColor,
        shape = MaterialTheme.shapes.extraSmall,
    ) {
        Text(
            text = when (status) {
                RepositoryPackageStatus.Available -> "Available"
                RepositoryPackageStatus.Installed -> "Installed"
                RepositoryPackageStatus.Update -> "Update"
                RepositoryPackageStatus.Incompatible -> "Incompatible"
                RepositoryPackageStatus.Error -> "Error"
            },
            modifier = Modifier.padding(
                horizontal = RepositoryDimensions.compactPadding,
                vertical = RepositoryDimensions.inlineSpacing,
            ),
            style = MaterialTheme.typography.labelSmall,
            fontWeight = FontWeight.SemiBold,
        )
    }
}

@Composable
private fun SectionHeading(title: String, description: String) {
    Column(modifier = Modifier.padding(bottom = RepositoryDimensions.compactPadding)) {
        Text(
            text = title,
            style = MaterialTheme.typography.titleMedium,
            fontWeight = FontWeight.SemiBold,
        )
        Text(
            text = description,
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
    }
}

@Composable
private fun EmptyMessage(message: String) {
    Text(
        text = message,
        modifier = Modifier.padding(vertical = RepositoryDimensions.sectionSpacing),
        style = MaterialTheme.typography.bodyMedium,
        color = MaterialTheme.colorScheme.onSurfaceVariant,
    )
}

@Composable
private fun RepositoryErrorBanner(
    message: String,
    onRetry: () -> Unit,
) {
    Surface(
        color = MaterialTheme.colorScheme.errorContainer,
        contentColor = MaterialTheme.colorScheme.onErrorContainer,
    ) {
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .padding(horizontal = RepositoryDimensions.contentPadding),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Text(
                text = message,
                modifier = Modifier.weight(1f),
                style = MaterialTheme.typography.bodySmall,
            )
            NnagaTextButton(onClick = onRetry) { Text("Retry") }
        }
    }
}

@Composable
private fun RepositoryFailure(
    message: String,
    onRetry: () -> Unit,
    modifier: Modifier = Modifier,
) {
    Box(modifier = modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
        Column(
            modifier = Modifier.padding(RepositoryDimensions.sectionSpacing),
            horizontalAlignment = Alignment.CenterHorizontally,
            verticalArrangement = Arrangement.spacedBy(RepositoryDimensions.contentPadding),
        ) {
            Text(
                text = message,
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.error,
            )
            NnagaTonalButton(onClick = onRetry) { Text("Retry") }
        }
    }
}

private fun operationLabel(operation: RepositoryPackageOperation): String = when (operation) {
    RepositoryPackageOperation.Installing -> "Installing…"
    RepositoryPackageOperation.Updating -> "Updating…"
    RepositoryPackageOperation.Removing -> "Removing…"
}
