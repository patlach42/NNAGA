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

package com.vibes.dsp.ui.browser

import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.defaultMinSize
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.offset
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.FilterList
import androidx.compose.material.icons.filled.Star
import androidx.compose.material.icons.outlined.Star
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.ModalBottomSheet
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.zIndex
import com.vibes.dsp.engine.PluginInfo
import com.vibes.dsp.ui.components.NnagaButton
import com.vibes.dsp.ui.components.NnagaFilterChip
import com.vibes.dsp.ui.components.NnagaIconButton
import com.vibes.dsp.ui.components.NnagaTextButton

private object PluginBrowserDimensions {
    val inlineSpacing = 4.dp
    val itemSpacing = 8.dp
    val contentPadding = 12.dp
    val sectionSpacing = 16.dp
    val sheetHeaderSpacing = 20.dp
    val sheetBottomPadding = 32.dp
    val badgeMinimum = 16.dp
}

@OptIn(ExperimentalLayoutApi::class, ExperimentalMaterial3Api::class)
@Composable
internal fun PluginBrowserList(
    viewModel: PluginBrowserViewModel,
    onPluginClick: (PluginInfo) -> Unit,
    pluginItemsEnabled: Boolean = true,
    modifier: Modifier = Modifier.fillMaxSize(),
) {
    val entries by viewModel.entries.collectAsState()
    val visibleEntries by viewModel.visibleEntries.collectAsState()
    val filters by viewModel.filters.collectAsState()
    val authorOptions by viewModel.authorOptions.collectAsState()
    val tagOptions by viewModel.tagOptions.collectAsState()
    val typeOptions by viewModel.typeOptions.collectAsState()
    val isLoading by viewModel.isLoading.collectAsState()
    val errorMessage by viewModel.errorMessage.collectAsState()
    val favorites by viewModel.favorites.collectAsState()
    var showFilterSheet by rememberSaveable { mutableStateOf(false) }
    val activeFilterCount =
        (if (filters.author != null) 1 else 0) + filters.tags.size + (if (filters.type != null) 1 else 0)

    Box(modifier = modifier, contentAlignment = Alignment.Center) {
        when {
            isLoading -> CircularProgressIndicator(
                color = MaterialTheme.colorScheme.primary,
                strokeWidth = 2.dp,
            )

            errorMessage != null -> PluginBrowserError(
                message = errorMessage ?: "Unknown error",
                onRetry = viewModel::refresh,
            )

            entries.isEmpty() -> PluginBrowserCatalogEmpty()

            else -> Column(modifier = Modifier.fillMaxSize()) {
                PluginBrowserToolbar(
                    resultCount = visibleEntries.size,
                    activeFilterCount = activeFilterCount,
                    onOpenFilters = { showFilterSheet = true },
                )
                if (visibleEntries.isEmpty()) {
                    Column(
                        modifier = Modifier
                            .fillMaxSize()
                            .padding(PluginBrowserDimensions.sectionSpacing),
                        horizontalAlignment = Alignment.CenterHorizontally,
                        verticalArrangement = Arrangement.Center,
                    ) {
                        Text("No plugins match these filters")
                        Spacer(modifier = Modifier.height(PluginBrowserDimensions.itemSpacing))
                        NnagaButton(onClick = viewModel::clearFilters) {
                            Text("Clear filters")
                        }
                    }
                } else {
                    LazyColumn(
                        modifier = Modifier.fillMaxSize(),
                        contentPadding = PaddingValues(PluginBrowserDimensions.contentPadding),
                        verticalArrangement = Arrangement.spacedBy(PluginBrowserDimensions.itemSpacing),
                    ) {
                        items(
                            items = visibleEntries,
                            key = { it.plugin.fullId },
                        ) { entry ->
                            PluginItem(
                                entry = entry,
                                enabled = pluginItemsEnabled,
                                isFavorite = entry.plugin.fullId in favorites,
                                onToggleFavorite = { viewModel.toggleFavorite(entry.plugin.fullId) },
                                onClick = { onPluginClick(entry.plugin) },
                            )
                        }
                    }
                }
            }
        }
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
                    .padding(horizontal = PluginBrowserDimensions.contentPadding)
                    .padding(bottom = PluginBrowserDimensions.sheetBottomPadding),
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
                    if (activeFilterCount > 0) {
                        NnagaTextButton(onClick = viewModel::clearFilters) {
                            Text("Clear All")
                        }
                    }
                }
                Spacer(modifier = Modifier.height(PluginBrowserDimensions.sheetHeaderSpacing))
                if (authorOptions.isNotEmpty()) {
                    PluginFilterSection(title = "Author") {
                        PluginSingleChoiceFilters(
                            facetName = "Author",
                            options = authorOptions,
                            selected = filters.author,
                            onSelected = viewModel::setAuthorFilter,
                        )
                    }
                }
                if (tagOptions.isNotEmpty()) {
                    PluginFilterSection(title = "Tags") {
                        FlowRow(
                            modifier = Modifier.fillMaxWidth(),
                            horizontalArrangement = Arrangement.spacedBy(PluginBrowserDimensions.itemSpacing),
                            verticalArrangement = Arrangement.spacedBy(PluginBrowserDimensions.itemSpacing),
                        ) {
                            tagOptions.forEach { tag ->
                                val selected = filters.tags.any { it.equals(tag, ignoreCase = true) }
                                NnagaFilterChip(
                                    text = tag,
                                    selected = selected,
                                    onClick = { viewModel.toggleTagFilter(tag) },
                                    modifier = Modifier.semantics {
                                        contentDescription = "Tag filter: $tag"
                                    },
                                )
                            }
                        }
                    }
                }
                if (typeOptions.isNotEmpty()) {
                    PluginFilterSection(title = "Type") {
                        PluginSingleChoiceFilters(
                            facetName = "Type",
                            options = typeOptions,
                            selected = filters.type,
                            onSelected = viewModel::setTypeFilter,
                        )
                    }
                }
            }
        }
    }
}

@Composable
private fun PluginBrowserToolbar(
    resultCount: Int,
    activeFilterCount: Int,
    onOpenFilters: () -> Unit,
) {
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .padding(horizontal = PluginBrowserDimensions.contentPadding, vertical = PluginBrowserDimensions.inlineSpacing),
        horizontalArrangement = Arrangement.SpaceBetween,
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Text(
            text = if (resultCount == 1) "1 plugin" else "$resultCount plugins",
            style = MaterialTheme.typography.labelMedium,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
        Box {
            NnagaIconButton(onClick = onOpenFilters) {
                Icon(
                    imageVector = Icons.Default.FilterList,
                    contentDescription = if (activeFilterCount == 0) {
                        "Open plugin filters"
                    } else {
                        "Open plugin filters, $activeFilterCount selected"
                    },
                    tint = if (activeFilterCount > 0) {
                        MaterialTheme.colorScheme.primary
                    } else {
                        MaterialTheme.colorScheme.onSurfaceVariant
                    },
                )
            }
            if (activeFilterCount > 0) {
                Surface(
                    modifier = Modifier
                        .align(Alignment.TopEnd)
                        .offset(
                            x = -PluginBrowserDimensions.inlineSpacing,
                            y = PluginBrowserDimensions.inlineSpacing,
                        )
                        .defaultMinSize(
                            minWidth = PluginBrowserDimensions.badgeMinimum,
                            minHeight = PluginBrowserDimensions.badgeMinimum,
                        ),
                    shape = MaterialTheme.shapes.extraSmall,
                    color = MaterialTheme.colorScheme.error,
                ) {
                    Text(
                        text = activeFilterCount.toString(),
                        modifier = Modifier.padding(horizontal = PluginBrowserDimensions.inlineSpacing),
                        style = MaterialTheme.typography.labelSmall,
                        color = MaterialTheme.colorScheme.onError,
                    )
                }
            }
        }
    }
}

@Composable
private fun PluginBrowserError(message: String, onRetry: () -> Unit) {
    Column(
        modifier = Modifier
            .fillMaxSize()
            .padding(PluginBrowserDimensions.sectionSpacing),
        horizontalAlignment = Alignment.CenterHorizontally,
        verticalArrangement = Arrangement.Center,
    ) {
        Text(text = message, color = MaterialTheme.colorScheme.error)
        Spacer(modifier = Modifier.height(PluginBrowserDimensions.sectionSpacing))
        NnagaButton(onClick = onRetry) { Text("Retry") }
    }
}

@Composable
private fun PluginBrowserCatalogEmpty() {
    Column(
        modifier = Modifier
            .fillMaxSize()
            .padding(PluginBrowserDimensions.sectionSpacing),
        horizontalAlignment = Alignment.CenterHorizontally,
        verticalArrangement = Arrangement.Center,
    ) {
        Text("No plugins available")
        Spacer(modifier = Modifier.height(PluginBrowserDimensions.itemSpacing))
        Text(
            text = "LV2 plugins are loaded from the app's extracted assets (assets/lv2). This app ships with GxPlugins in assets—if you see nothing here, the native build may be using the LV2 stub (no lilv/serd/sord). Build the LV2 libraries and place them in app/src/main/cpp/libs/lv2/, then rebuild the app. See LV2_INTEGRATION.md.",
            style = MaterialTheme.typography.bodySmall,
        )
    }
}

@OptIn(ExperimentalLayoutApi::class)
@Composable
private fun PluginSingleChoiceFilters(
    facetName: String,
    options: List<String>,
    selected: String?,
    onSelected: (String?) -> Unit,
) {
    FlowRow(
        modifier = Modifier.fillMaxWidth(),
        horizontalArrangement = Arrangement.spacedBy(PluginBrowserDimensions.itemSpacing),
        verticalArrangement = Arrangement.spacedBy(PluginBrowserDimensions.itemSpacing),
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
private fun PluginFilterSection(
    title: String,
    content: @Composable () -> Unit,
) {
    Column(modifier = Modifier.padding(bottom = PluginBrowserDimensions.sectionSpacing)) {
        Text(
            text = title,
            style = MaterialTheme.typography.labelLarge,
            color = MaterialTheme.colorScheme.primary,
            modifier = Modifier.padding(bottom = PluginBrowserDimensions.itemSpacing),
        )
        content()
    }
}

@Composable
internal fun PluginItem(
    entry: PluginBrowserEntry,
    isFavorite: Boolean = false,
    enabled: Boolean = true,
    onToggleFavorite: () -> Unit = {},
    onClick: () -> Unit,
) {
    val plugin = entry.plugin
    Card(
        modifier = Modifier
            .fillMaxWidth()
            .heightIn(min = 48.dp)
            .clickable(enabled = enabled, onClick = onClick)
            .testTag("browser_plugin_item"),
        shape = MaterialTheme.shapes.small,
        colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surfaceVariant),
        elevation = CardDefaults.cardElevation(defaultElevation = 0.dp),
        border = CardDefaults.outlinedCardBorder(),
    ) {
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .padding(PluginBrowserDimensions.itemSpacing),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(PluginBrowserDimensions.itemSpacing),
        ) {
            Column(modifier = Modifier.weight(1f)) {
                Text(
                    text = plugin.name.ifEmpty { plugin.id },
                    style = MaterialTheme.typography.titleMedium,
                    fontWeight = FontWeight.Bold,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                )
                if (entry.author.isNotBlank()) {
                    Text(
                        text = entry.author,
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                        maxLines = 1,
                        overflow = TextOverflow.Ellipsis,
                    )
                }
                Spacer(modifier = Modifier.height(PluginBrowserDimensions.inlineSpacing))
                Row(
                    verticalAlignment = Alignment.CenterVertically,
                    horizontalArrangement = Arrangement.spacedBy(6.dp),
                ) {
                    PluginBadge(text = plugin.format, kind = PluginBadgeKind.Format)
                    if (plugin.arch.isNotEmpty()) {
                        PluginBadge(text = plugin.arch, kind = PluginBadgeKind.Architecture)
                    }
                    Text(
                        text = plugin.guiTypes.joinToString(", ") { it.displayName },
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant.copy(alpha = 0.7f),
                        maxLines = 1,
                        overflow = TextOverflow.Ellipsis,
                    )
                }
            }
            NnagaIconButton(onClick = onToggleFavorite) {
                Icon(
                    imageVector = if (isFavorite) Icons.Filled.Star else Icons.Outlined.Star,
                    contentDescription = if (isFavorite) "Remove from favorites" else "Add to favorites",
                    tint = if (isFavorite) MaterialTheme.colorScheme.primary else MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
        }
    }
}

private enum class PluginBadgeKind { Format, Architecture }

@Composable
private fun PluginBadge(text: String, kind: PluginBadgeKind) {
    val (background, foreground) = when (kind) {
        PluginBadgeKind.Format -> when (text) {
            "VST3" -> Color(0xFF1976D2) to Color.White
            "VST2" -> Color(0xFFE64A19) to Color.White
            "LV2" -> Color(0xFF388E3C) to Color.White
            else -> MaterialTheme.colorScheme.secondaryContainer to MaterialTheme.colorScheme.onSecondaryContainer
        }
        PluginBadgeKind.Architecture -> when (text) {
            "x64" -> Color(0xFF7B1FA2) to Color.White
            "x86" -> Color(0xFF00838F) to Color.White
            "native" -> Color(0xFF546E7A) to Color.White
            else -> MaterialTheme.colorScheme.secondaryContainer to MaterialTheme.colorScheme.onSecondaryContainer
        }
    }
    Surface(shape = MaterialTheme.shapes.extraSmall, color = background) {
        Text(
            text = text,
            style = MaterialTheme.typography.labelSmall,
            color = foreground,
            modifier = Modifier.padding(horizontal = 6.dp, vertical = 2.dp),
        )
    }
}

@Composable
internal fun BrowserBlockingOperationOverlay(label: String) {
    Box(
        modifier = Modifier
            .fillMaxSize()
            .zIndex(100f)
            .background(MaterialTheme.colorScheme.scrim.copy(alpha = 0.45f))
            .pointerInput(Unit) {
                awaitPointerEventScope {
                    while (true) {
                        val event = awaitPointerEvent()
                        event.changes.forEach { it.consume() }
                    }
                }
            },
        contentAlignment = Alignment.Center,
    ) {
        Surface(
            tonalElevation = 0.dp,
            shadowElevation = 0.dp,
            shape = MaterialTheme.shapes.small,
            color = MaterialTheme.colorScheme.surface,
        ) {
            Column(
                modifier = Modifier.padding(horizontal = 28.dp, vertical = 22.dp),
                horizontalAlignment = Alignment.CenterHorizontally,
                verticalArrangement = Arrangement.spacedBy(PluginBrowserDimensions.contentPadding),
            ) {
                CircularProgressIndicator(
                    color = MaterialTheme.colorScheme.primary,
                    strokeWidth = 2.dp,
                )
                Text(
                    text = "$label...",
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurface,
                )
            }
        }
    }
}
