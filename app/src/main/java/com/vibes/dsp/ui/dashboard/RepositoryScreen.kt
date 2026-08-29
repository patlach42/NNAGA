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

import androidx.compose.ui.ExperimentalComposeUiApi
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.defaultMinSize
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.offset
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.foundation.text.KeyboardActions
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Add
import androidx.compose.material.icons.filled.Close
import androidx.compose.material.icons.filled.Delete
import androidx.compose.material.icons.filled.Refresh
import androidx.compose.material.icons.filled.FilterList
import androidx.compose.material.icons.filled.Search
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.Divider
import androidx.compose.material3.Icon
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.ModalBottomSheet
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
import androidx.compose.ui.platform.LocalSoftwareKeyboardController
import androidx.compose.ui.platform.LocalUriHandler
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.vibes.dsp.ui.components.NnagaButton
import com.vibes.dsp.ui.components.NnagaFilterChip
import com.vibes.dsp.ui.components.NnagaIconButton
import com.vibes.dsp.ui.components.NnagaOutlinedButton
import com.vibes.dsp.ui.components.NnagaSwitch
import com.vibes.dsp.ui.components.NnagaTextButton
import com.vibes.dsp.ui.components.NnagaTonalButton
import com.vibes.dsp.ui.components.nnagaOutlinedTextFieldColors
import java.util.Locale
import kotlin.math.min

internal const val REPOSITORY_PAGE_SIZE = 25

private val REPOSITORY_FORMAT_GROUPS = listOf("LV2", "Wine", "JSFX")

private object RepositoryDimensions {
    val compactPadding = 8.dp
    val contentPadding = 12.dp
    val sectionSpacing = 16.dp
    val itemSpacing = 8.dp
    val inlineSpacing = 4.dp
    val divider = 1.dp
    val progressHeight = 2.dp
    val actionProgress = 16.dp
    val badgeMinimum = 16.dp
    val sheetBottomPadding = 32.dp
    val sheetHeaderSpacing = 20.dp
}

@Composable
fun RepositoryScreen(
    viewModel: RepositoryViewModel,
    onInstallPackage: (RepositoryPackageItem) -> Unit = { viewModel.install(it.id) },
    onUpdatePackage: (RepositoryPackageItem) -> Unit = { viewModel.update(it.id) },
    manageSources: Boolean = false,
    onCloseSourceManagement: () -> Unit = {},
    modifier: Modifier = Modifier,
) {
    val snapshot by viewModel.snapshot.collectAsState()
    val actionState by viewModel.actionState.collectAsState()
    val snackbarHostState = remember { SnackbarHostState() }
    var sourceUrl by rememberSaveable { mutableStateOf("") }
    var query by rememberSaveable { mutableStateOf("") }
    var selectedFormat by rememberSaveable { mutableStateOf<String?>(null) }
    var selectedManufacturer by rememberSaveable { mutableStateOf<String?>(null) }
    var selectedTags by rememberSaveable { mutableStateOf(arrayListOf<String>()) }
    var visiblePackageCount by rememberSaveable(
        snapshot.packages,
        query,
        selectedFormat,
        selectedManufacturer,
        selectedTags,
    ) {
        mutableStateOf(REPOSITORY_PAGE_SIZE)
    }
    var addWasPending by remember { mutableStateOf(false) }
    val addPending = actionState.isPending(RepositoryViewModel.ADD_SOURCE)
    val refreshing = snapshot.isRefreshing ||
        actionState.isPending(RepositoryViewModel.REFRESH_ALL)
    val manufacturerOptions = remember(snapshot.packages) {
        snapshot.packages
            .map { it.manufacturer.trim() }
            .filter { it.isNotEmpty() }
            .distinctBy { it.lowercase(Locale.ROOT) }
            .sortedWith(String.CASE_INSENSITIVE_ORDER)
    }
    val tagOptions = remember(snapshot.packages) {
        snapshot.packages
            .flatMap { it.tags }
            .map(String::trim)
            .filter(String::isNotEmpty)
            .distinctBy { it.lowercase(Locale.ROOT) }
            .sortedWith(String.CASE_INSENSITIVE_ORDER)
    }
    val selectedTagSet = remember(selectedTags) { selectedTags.toSet() }
    val filteredPackages = remember(
        snapshot.packages,
        query,
        selectedFormat,
        selectedManufacturer,
        selectedTagSet,
    ) {
        filterRepositoryPackages(
            packages = snapshot.packages,
            query = query,
            manufacturer = selectedManufacturer,
            tags = selectedTagSet,
            formatGroup = selectedFormat,
        )
    }
    val visiblePackages = remember(filteredPackages, visiblePackageCount) {
        paginateRepositoryPackages(filteredPackages, visiblePackageCount)
    }
    val activeFacetCount =
        listOfNotNull(selectedFormat, selectedManufacturer).size + selectedTags.size
    val filtersActive = query.isNotBlank() || activeFacetCount > 0

    LaunchedEffect(snapshot.isLoading, manufacturerOptions, tagOptions) {
        if (!snapshot.isLoading) {
            if (manufacturerOptions.none { it.equals(selectedManufacturer, ignoreCase = true) }) {
                selectedManufacturer = null
            }
            val availableTags = tagOptions.mapTo(mutableSetOf()) { it.lowercase(Locale.ROOT) }
            val retainedTags = selectedTags
                .filterTo(arrayListOf()) { it.lowercase(Locale.ROOT) in availableTags }
            if (retainedTags != selectedTags) selectedTags = retainedTags
        }
    }
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
            if (manageSources) {
                RepositorySourceToolbar(
                    sourceUrl = sourceUrl,
                    onSourceUrlChanged = { sourceUrl = it },
                    onAddSource = { viewModel.addSource(sourceUrl) },
                    onRefresh = viewModel::refreshAll,
                    onClose = onCloseSourceManagement,
                    addingSource = addPending,
                    refreshing = refreshing,
                )
            } else {
                RepositoryCatalogToolbar(
                    query = query,
                    onQueryChanged = { query = it },
                    formats = REPOSITORY_FORMAT_GROUPS,
                    selectedFormat = selectedFormat,
                    onFormatSelected = { selectedFormat = it },
                    manufacturers = manufacturerOptions,
                    selectedManufacturer = selectedManufacturer,
                    onManufacturerSelected = { selectedManufacturer = it },
                    tags = tagOptions,
                    selectedTags = selectedTagSet,
                    onTagSelected = { tag ->
                        selectedTags = ArrayList(selectedTags).apply {
                            val selectedIndex = indexOfFirst { it.equals(tag, ignoreCase = true) }
                            if (selectedIndex >= 0) removeAt(selectedIndex) else add(tag)
                        }
                    },
                    onClear = {
                        query = ""
                        selectedFormat = null
                        selectedManufacturer = null
                        selectedTags = arrayListOf()
                    },
                    filtersActive = filtersActive,
                    activeFacetCount = activeFacetCount,
                    onRefresh = viewModel::refreshAll,
                    refreshing = refreshing,
                )
            }
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
                catalogPackages = visiblePackages,
                matchedPackageCount = filteredPackages.size,
                filtersActive = filtersActive,
                actionState = actionState,
                manageSources = manageSources,
                onRetry = viewModel::refreshAll,
                onSourceEnabledChanged = viewModel::setSourceEnabled,
                onRefreshSource = viewModel::refreshSource,
                onRemoveSource = viewModel::removeSource,
                onInstall = onInstallPackage,
                onUpdate = { id ->
                    snapshot.packages.firstOrNull { it.id == id }?.let(onUpdatePackage)
                },
                onRemovePackage = viewModel::remove,
                onShowMore = {
                    visiblePackageCount = min(
                        filteredPackages.size,
                        visiblePackageCount + REPOSITORY_PAGE_SIZE,
                    )
                },
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
private fun RepositorySourceToolbar(
    sourceUrl: String,
    onSourceUrlChanged: (String) -> Unit,
    onAddSource: () -> Unit,
    onRefresh: () -> Unit,
    onClose: () -> Unit,
    addingSource: Boolean,
    refreshing: Boolean,
) {
    Column(
        modifier = Modifier
            .fillMaxWidth()
            .padding(RepositoryDimensions.compactPadding),
        verticalArrangement = Arrangement.spacedBy(RepositoryDimensions.compactPadding),
    ) {
        OutlinedTextField(
            value = sourceUrl,
            onValueChange = onSourceUrlChanged,
            modifier = Modifier.fillMaxWidth(),
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
        Row(
            modifier = Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.spacedBy(RepositoryDimensions.compactPadding),
            verticalAlignment = Alignment.CenterVertically,
        ) {
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
            RepositoryRefreshButton(onRefresh = onRefresh, refreshing = refreshing)
            Spacer(modifier = Modifier.weight(1f))
            NnagaTextButton(onClick = onClose) {
                Text("Close")
            }
        }
    }
}

@OptIn(
    ExperimentalComposeUiApi::class,
    ExperimentalLayoutApi::class,
    ExperimentalMaterial3Api::class,
)
@Composable
private fun RepositoryCatalogToolbar(
    query: String,
    onQueryChanged: (String) -> Unit,
    formats: List<String>,
    selectedFormat: String?,
    onFormatSelected: (String?) -> Unit,
    manufacturers: List<String>,
    selectedManufacturer: String?,
    onManufacturerSelected: (String?) -> Unit,
    tags: List<String>,
    selectedTags: Set<String>,
    onTagSelected: (String) -> Unit,
    onClear: () -> Unit,
    filtersActive: Boolean,
    activeFacetCount: Int,
    onRefresh: () -> Unit,
    refreshing: Boolean,
) {
    val keyboardController = LocalSoftwareKeyboardController.current
    var showFilterSheet by rememberSaveable { mutableStateOf(false) }

    Row(
        modifier = Modifier
            .fillMaxWidth()
            .padding(RepositoryDimensions.compactPadding),
        horizontalArrangement = Arrangement.spacedBy(RepositoryDimensions.inlineSpacing),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        OutlinedTextField(
            value = query,
            onValueChange = onQueryChanged,
            modifier = Modifier.weight(1f),
            placeholder = { Text("Search plugins...") },
            leadingIcon = { Icon(Icons.Default.Search, contentDescription = null) },
            trailingIcon = query.takeIf(String::isNotEmpty)?.let {
                {
                    NnagaIconButton(onClick = { onQueryChanged("") }) {
                        Icon(Icons.Default.Close, contentDescription = "Clear plugin search")
                    }
                }
            },
            singleLine = true,
            keyboardOptions = KeyboardOptions(
                keyboardType = KeyboardType.Text,
                imeAction = ImeAction.Search,
            ),
            keyboardActions = KeyboardActions(
                onSearch = { keyboardController?.hide() },
            ),
            shape = MaterialTheme.shapes.small,
            colors = nnagaOutlinedTextFieldColors(),
        )
        Box {
            NnagaIconButton(onClick = { showFilterSheet = true }) {
                Icon(
                    imageVector = Icons.Default.FilterList,
                    contentDescription = if (activeFacetCount == 0) {
                        "Open repository filters"
                    } else {
                        "Open repository filters, $activeFacetCount selected"
                    },
                    tint = if (activeFacetCount > 0) {
                        MaterialTheme.colorScheme.primary
                    } else {
                        MaterialTheme.colorScheme.onSurfaceVariant
                    },
                )
            }
            if (activeFacetCount > 0) {
                Surface(
                    modifier = Modifier
                        .align(Alignment.TopEnd)
                        .offset(
                            x = -RepositoryDimensions.inlineSpacing,
                            y = RepositoryDimensions.inlineSpacing,
                        )
                        .defaultMinSize(
                            minWidth = RepositoryDimensions.badgeMinimum,
                            minHeight = RepositoryDimensions.badgeMinimum,
                        ),
                    shape = MaterialTheme.shapes.extraSmall,
                    color = MaterialTheme.colorScheme.error,
                ) {
                    Text(
                        text = activeFacetCount.toString(),
                        modifier = Modifier.padding(horizontal = RepositoryDimensions.inlineSpacing),
                        style = MaterialTheme.typography.labelSmall,
                        color = MaterialTheme.colorScheme.onError,
                    )
                }
            }
        }
        RepositoryRefreshButton(onRefresh = onRefresh, refreshing = refreshing)
    }

    if (showFilterSheet) {
        ModalBottomSheet(
            onDismissRequest = { showFilterSheet = false },
            containerColor = MaterialTheme.colorScheme.surface,
            shape = MaterialTheme.shapes.small,
        ) {
            Column(
                modifier = Modifier
                    .fillMaxWidth()
                    .verticalScroll(rememberScrollState())
                    .padding(horizontal = RepositoryDimensions.contentPadding)
                    .padding(bottom = RepositoryDimensions.sheetBottomPadding),
            ) {
                Row(
                    modifier = Modifier.fillMaxWidth(),
                    horizontalArrangement = Arrangement.SpaceBetween,
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    Text(
                        text = "Filters",
                        style = MaterialTheme.typography.titleLarge,
                        fontWeight = FontWeight.Bold,
                    )
                    if (filtersActive) {
                        NnagaTextButton(onClick = onClear) {
                            Text("Clear All")
                        }
                    }
                }
                Spacer(modifier = Modifier.height(RepositoryDimensions.sheetHeaderSpacing))
                RepositoryFilterSection(title = "Format") {
                    RepositorySingleChoiceFlow(
                        facetName = "Format",
                        options = formats,
                        selected = selectedFormat,
                        onSelected = onFormatSelected,
                    )
                }
                RepositoryFilterSection(title = "Manufacturer") {
                    RepositorySingleChoiceFlow(
                        facetName = "Manufacturer",
                        options = manufacturers,
                        selected = selectedManufacturer,
                        onSelected = onManufacturerSelected,
                    )
                }
                RepositoryFilterSection(title = "Tags") {
                    FlowRow(
                        modifier = Modifier.fillMaxWidth(),
                        horizontalArrangement = Arrangement.spacedBy(RepositoryDimensions.itemSpacing),
                        verticalArrangement = Arrangement.spacedBy(RepositoryDimensions.itemSpacing),
                    ) {
                        tags.forEach { tag ->
                            val selected = selectedTags.any { it.equals(tag, ignoreCase = true) }
                            NnagaFilterChip(
                                text = tag,
                                selected = selected,
                                onClick = { onTagSelected(tag) },
                                modifier = Modifier.semantics {
                                    contentDescription = "Tag filter: $tag"
                                },
                            )
                        }
                    }
                }
            }
        }
    }
}

@OptIn(ExperimentalLayoutApi::class)
@Composable
private fun RepositorySingleChoiceFlow(
    facetName: String,
    options: List<String>,
    selected: String?,
    onSelected: (String?) -> Unit,
) {
    FlowRow(
        modifier = Modifier.fillMaxWidth(),
        horizontalArrangement = Arrangement.spacedBy(RepositoryDimensions.itemSpacing),
        verticalArrangement = Arrangement.spacedBy(RepositoryDimensions.itemSpacing),
    ) {
        options.forEach { option ->
            val isSelected = option.equals(selected, ignoreCase = true)
            NnagaFilterChip(
                text = option,
                selected = isSelected,
                onClick = { onSelected(option.takeUnless { isSelected }) },
                modifier = Modifier.semantics {
                    contentDescription = "$facetName filter: $option"
                },
            )
        }
    }
}

@Composable
private fun RepositoryFilterSection(
    title: String,
    content: @Composable () -> Unit,
) {
    Column(modifier = Modifier.padding(bottom = RepositoryDimensions.sectionSpacing)) {
        Text(
            text = title,
            style = MaterialTheme.typography.labelLarge,
            color = MaterialTheme.colorScheme.primary,
            modifier = Modifier.padding(bottom = RepositoryDimensions.itemSpacing),
        )
        content()
    }
}

@Composable
private fun RepositoryRefreshButton(
    onRefresh: () -> Unit,
    refreshing: Boolean,
) {
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

@Composable
private fun RepositoryContent(
    snapshot: RepositorySnapshot,
    catalogPackages: List<RepositoryPackageItem>,
    matchedPackageCount: Int,
    filtersActive: Boolean,
    actionState: RepositoryActionState,
    manageSources: Boolean,
    onRetry: () -> Unit,
    onSourceEnabledChanged: (String, Boolean) -> Unit,
    onRefreshSource: (String) -> Unit,
    onRemoveSource: (String) -> Unit,
    onInstall: (RepositoryPackageItem) -> Unit,
    onUpdate: (String) -> Unit,
    onRemovePackage: (String) -> Unit,
    onShowMore: () -> Unit,
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
        manageSources -> {
            SourceList(
                sources = snapshot.sources,
                actionState = actionState,
                onEnabledChanged = onSourceEnabledChanged,
                onRefresh = onRefreshSource,
                onRemove = onRemoveSource,
                modifier = modifier.fillMaxSize(),
            )
        }
        else -> {
            PackageList(
                packages = catalogPackages,
                matchedPackageCount = matchedPackageCount,
                totalPackageCount = snapshot.packages.size,
                filtersActive = filtersActive,
                actionState = actionState,
                onInstall = onInstall,
                onUpdate = onUpdate,
                onRemove = onRemovePackage,
                onShowMore = onShowMore,
                modifier = modifier.fillMaxSize(),
            )
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
    matchedPackageCount: Int,
    totalPackageCount: Int,
    filtersActive: Boolean,
    actionState: RepositoryActionState,
    onInstall: (RepositoryPackageItem) -> Unit,
    onUpdate: (String) -> Unit,
    onRemove: (String) -> Unit,
    onShowMore: () -> Unit,
    modifier: Modifier = Modifier,
) {
    LazyColumn(
        modifier = modifier,
        contentPadding = PaddingValues(RepositoryDimensions.contentPadding),
        verticalArrangement = Arrangement.spacedBy(RepositoryDimensions.itemSpacing),
    ) {
        item(key = "package-heading") {
            SectionHeading(
                title = "Plugins",
                description =
                    "${packages.size} shown · $matchedPackageCount matched · " +
                        "$totalPackageCount total.",
            )
        }
        if (packages.isEmpty()) {
            item(key = "package-empty") {
                EmptyMessage(
                    when {
                        totalPackageCount == 0 ->
                            "Enable a source and refresh repositories to discover plugins."
                        filtersActive ->
                            "No matching plugins. Clear filters or try another search."
                        else ->
                            "Refresh repositories to discover plugins."
                    },
                )
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
            if (packages.size < matchedPackageCount) {
                item(key = "package-show-more") {
                    NnagaOutlinedButton(
                        onClick = onShowMore,
                        modifier = Modifier.fillMaxWidth(),
                    ) {
                        Text(
                            "Show more (${matchedPackageCount - packages.size} remaining)",
                        )
                    }
                }
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
                        text = buildList {
                            add(repositoryPackage.format)
                            repositoryPackage.manufacturer
                                .takeIf(String::isNotBlank)
                                ?.let(::add)
                            add(repositoryPackage.sourceName)
                        }.joinToString(" · "),
                        style = MaterialTheme.typography.labelSmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                        maxLines = 1,
                        overflow = TextOverflow.Ellipsis,
                    )
                }
                PackageStatusBadge(repositoryPackage.status)
            }
            PackageTagBadges(repositoryPackage.tags)
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
            repositoryPackage.source?.takeIf(String::isNotBlank)?.let { source ->
                val uriHandler = LocalUriHandler.current
                Row(
                    modifier = Modifier
                        .fillMaxWidth()
                        .padding(top = RepositoryDimensions.compactPadding),
                    horizontalArrangement = Arrangement.Start,
                ) {
                    NnagaTextButton(onClick = { uriHandler.openUri(source) }) {
                        Text("Source")
                    }
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
private fun PackageTagBadges(tags: List<String>) {
    val normalizedTags = remember(tags) {
        tags.asSequence()
            .map(String::trim)
            .filter(String::isNotEmpty)
            .distinctBy { it.lowercase(Locale.ROOT) }
            .toList()
    }
    val visibleTags = normalizedTags.take(3)
    if (visibleTags.isEmpty()) return
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .padding(top = RepositoryDimensions.inlineSpacing),
        horizontalArrangement = Arrangement.spacedBy(RepositoryDimensions.inlineSpacing),
    ) {
        visibleTags.forEach { tag ->
            Surface(
                modifier = Modifier.weight(1f, fill = false),
                color = MaterialTheme.colorScheme.primaryContainer,
                contentColor = MaterialTheme.colorScheme.onPrimaryContainer,
                shape = MaterialTheme.shapes.extraSmall,
            ) {
                Text(
                    text = tag,
                    modifier = Modifier.padding(
                        horizontal = RepositoryDimensions.compactPadding,
                        vertical = RepositoryDimensions.inlineSpacing,
                    ),
                    style = MaterialTheme.typography.labelSmall,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                )
            }
        }
        if (normalizedTags.size > visibleTags.size) {
            Text(
                text = "+${normalizedTags.size - visibleTags.size}",
                modifier = Modifier.align(Alignment.CenterVertically),
                style = MaterialTheme.typography.labelSmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
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

internal fun filterRepositoryPackages(
    packages: List<RepositoryPackageItem>,
    query: String,
    manufacturer: String?,
    tags: Set<String>,
    formatGroup: String? = null,
): List<RepositoryPackageItem> {
    val normalizedQuery = query.trim()
    val normalizedManufacturer = manufacturer?.trim().orEmpty()
    val normalizedFormatGroup = formatGroup?.trim()?.lowercase(Locale.ROOT).orEmpty()
    val normalizedTags = tags.asSequence()
        .map(String::trim)
        .filter(String::isNotEmpty)
        .toList()
    if (
        normalizedQuery.isEmpty() &&
        normalizedManufacturer.isEmpty() &&
        normalizedFormatGroup.isEmpty() &&
        normalizedTags.isEmpty()
    ) {
        return packages
    }
    return packages.filter { repositoryPackage ->
        val matchesQuery = normalizedQuery.isEmpty() ||
            repositoryPackage.name.contains(normalizedQuery, ignoreCase = true) ||
            repositoryPackage.description?.contains(normalizedQuery, ignoreCase = true) == true ||
            repositoryPackage.manufacturer.contains(normalizedQuery, ignoreCase = true) ||
            repositoryPackage.tags.any { it.contains(normalizedQuery, ignoreCase = true) }
        val matchesManufacturer = normalizedManufacturer.isEmpty() ||
            repositoryPackage.manufacturer.trim()
                .equals(normalizedManufacturer, ignoreCase = true)
        val matchesTags = normalizedTags.all { selectedTag ->
            repositoryPackage.tags.any { tag ->
                tag.trim().equals(selectedTag, ignoreCase = true)
            }
        }
        val packageFormat = repositoryPackage.format.trim()
        val matchesFormat = when (normalizedFormatGroup) {
            "" -> true
            "lv2" -> packageFormat.equals("lv2", ignoreCase = true)
            "wine" -> packageFormat.startsWith("wine_", ignoreCase = true)
            "jsfx" -> packageFormat.equals("jsfx", ignoreCase = true)
            else -> false
        }
        matchesQuery && matchesManufacturer && matchesTags && matchesFormat
    }
}

internal fun paginateRepositoryPackages(
    packages: List<RepositoryPackageItem>,
    visibleCount: Int,
): List<RepositoryPackageItem> = packages.take(visibleCount.coerceIn(0, packages.size))
