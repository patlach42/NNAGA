package com.vibes.dsp.ui.browser

import android.net.Uri
import androidx.activity.compose.BackHandler
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Close
import androidx.compose.material.icons.filled.ExpandLess
import androidx.compose.material.icons.filled.ExpandMore
import androidx.compose.material.icons.filled.Folder
import androidx.compose.material.icons.filled.Refresh
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.CircularProgressIndicator
import com.vibes.dsp.ui.components.FontaudioGlyph
import com.vibes.dsp.ui.components.FontaudioIcon
import com.vibes.dsp.ui.components.NnagaIconButton
import com.vibes.dsp.ui.components.NnagaTextButton

import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.SnackbarHost
import androidx.compose.material3.SnackbarHostState
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.role
import androidx.compose.ui.semantics.selected
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.lifecycle.viewmodel.compose.viewModel
import com.vibes.dsp.engine.PluginInfo
import com.vibes.dsp.engine.RackPathId
import kotlinx.coroutines.launch

internal enum class MediaBrowserTab { Clips, Plugins }

@Composable
internal fun MediaBrowserDrawerContent(
    selectedTrackId: RackPathId?,
    selectedTrackLabel: String?,
    selectedSlot: Int,
    requestedTab: MediaBrowserTab = MediaBrowserTab.Clips,
    pluginTargetPathId: RackPathId? = null,
    pluginTargetLabel: String? = null,
    replaceIndex: Int? = null,
    onClose: () -> Unit,
    onLoadClip: (RackPathId, Int, Uri) -> Unit,
    onPluginAdded: (RackPathId) -> Unit,
) {
    var selectedTab by rememberSaveable { mutableIntStateOf(requestedTab.ordinal) }
    val snackbarHostState = remember { SnackbarHostState() }
    LaunchedEffect(requestedTab) { selectedTab = requestedTab.ordinal }

    Box(modifier = Modifier.fillMaxSize()) {
        Column(modifier = Modifier.fillMaxSize()) {
            Row(
                modifier = Modifier
                    .fillMaxWidth()
                    .height(48.dp)
                    .padding(horizontal = 4.dp),
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.SpaceBetween,
            ) {
                Text("Browser", style = MaterialTheme.typography.titleMedium)
                Row {
                    if (selectedTab == MediaBrowserTab.Clips.ordinal) ClipsRefreshButton()
                    else PluginsRefreshButton()
                    NnagaIconButton(onClick = onClose) {
                        Icon(Icons.Default.Close, contentDescription = "Close")
                    }
                }
            }
            Row(modifier = Modifier.fillMaxWidth()) {
                BrowserTab(
                    label = "Clips",
                    selected = selectedTab == MediaBrowserTab.Clips.ordinal,
                    onClick = { selectedTab = MediaBrowserTab.Clips.ordinal },
                    modifier = Modifier.weight(1f),
                )
                BrowserTab(
                    label = "Plugins",
                    selected = selectedTab == MediaBrowserTab.Plugins.ordinal,
                    onClick = { selectedTab = MediaBrowserTab.Plugins.ordinal },
                    modifier = Modifier.weight(1f),
                )
            }
            if (selectedTab == MediaBrowserTab.Clips.ordinal) {
                ClipsBrowserTab(
                    selectedTrackId = selectedTrackId,
                    selectedTrackLabel = selectedTrackLabel,
                    selectedSlot = selectedSlot,
                    onLoadClip = onLoadClip,
                    modifier = Modifier.weight(1f),
                )
            } else {
                PluginsBrowserTab(
                    selectedTrackId = pluginTargetPathId ?: selectedTrackId,
                    selectedTrackLabel = pluginTargetLabel ?: selectedTrackLabel,
                    replaceIndex = replaceIndex,
                    onPluginAdded = onPluginAdded,
                    snackbarHostState = snackbarHostState,
                    modifier = Modifier.weight(1f),
                )
            }
        }
        SnackbarHost(
            hostState = snackbarHostState,
            modifier = Modifier.align(Alignment.BottomCenter),
        )
    }
}

@Composable
private fun BrowserTab(
    label: String,
    selected: Boolean,
    onClick: () -> Unit,
    modifier: Modifier,
) {
    Box(
        modifier = modifier
            .height(44.dp)
            .semantics {
                role = Role.Tab
                this.selected = selected
            },
        contentAlignment = Alignment.Center,
    ) {
        TextButton(onClick = onClick, modifier = Modifier.fillMaxSize()) {
            Text(
                text = label,
                color = if (selected) MaterialTheme.colorScheme.primary
                else MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
    }
}

@Composable
private fun ClipsRefreshButton(viewModel: ClipBrowserViewModel = viewModel()) {
    NnagaIconButton(onClick = viewModel::refresh) {
        Icon(Icons.Default.Refresh, contentDescription = "Refresh")
    }
}

@Composable
private fun PluginsRefreshButton(viewModel: PluginBrowserViewModel = viewModel()) {
    NnagaIconButton(onClick = viewModel::refresh) {
        Icon(Icons.Default.Refresh, contentDescription = "Refresh")
    }
}

@Composable
private fun TargetBanner(trackLabel: String?, slot: Int) {
    Text(
        text = trackLabel?.let { "$it · Slot ${slot + 1}" } ?: "Select a track first",
        modifier = Modifier.padding(horizontal = 12.dp, vertical = 8.dp),
        style = MaterialTheme.typography.labelLarge,
    )
}

@Composable
private fun EmptyBrowserMessage(message: String) {
    Text(
        text = message,
        modifier = Modifier.padding(16.dp),
        style = MaterialTheme.typography.bodyMedium,
        color = MaterialTheme.colorScheme.onSurfaceVariant,
    )
}

@Composable
private fun ClipsBrowserTab(
    selectedTrackId: RackPathId?,
    selectedTrackLabel: String?,
    selectedSlot: Int,
    onLoadClip: (RackPathId, Int, Uri) -> Unit,
    modifier: Modifier,
    viewModel: ClipBrowserViewModel = viewModel(),
) {
    val state by viewModel.uiState.collectAsState()
    var pendingClip by remember { mutableStateOf<PendingClipRequest?>(null) }
    LaunchedEffect(Unit) { viewModel.ensureLoaded() }
    Column(modifier = modifier.fillMaxSize()) {
        TargetBanner(selectedTrackLabel, selectedSlot)
        state.errorMessage?.let { message ->
            Row(
                modifier = Modifier.fillMaxWidth().padding(horizontal = 12.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Text(
                    text = message,
                    modifier = Modifier.weight(1f),
                    color = MaterialTheme.colorScheme.error,
                    style = MaterialTheme.typography.bodySmall,
                )
                NnagaTextButton(onClick = viewModel::refresh) { Text("Retry") }
            }
        }
        when {
            state.rootUri == null && !state.isLoading -> EmptyBrowserMessage(
                "Choose a clip folder in Settings > Clip Launcher",
            )
            state.rows.isEmpty() && !state.isLoading && state.errorMessage == null -> {
                EmptyBrowserMessage("No supported clips in this folder")
            }
            else -> LazyColumn(
                modifier = Modifier.fillMaxSize(),
                contentPadding = PaddingValues(bottom = 16.dp),
            ) {
                items(state.rows, key = { it.uri.toString() }) { row ->
                    ClipTreeRow(
                        row = row,
                        selectedTrackId = selectedTrackId,
                        onToggleDirectory = viewModel::toggleDirectory,
                        onMediaClick = { uri, fileName ->
                            selectedTrackId?.let { trackId ->
                                pendingClip = PendingClipRequest(
                                    uri = uri,
                                    trackId = trackId,
                                    slot = selectedSlot,
                                    trackLabel = selectedTrackLabel ?: "Track",
                                    fileName = fileName,
                                )
                            }
                        },
                    )
                }
            }
        }
    }

    pendingClip?.let { request ->
        AlertDialog(
            onDismissRequest = { pendingClip = null },
            title = { Text("Load clip?") },
            text = { Text("Load ${request.fileName} to ${request.trackLabel} · Slot ${request.slot + 1}?") },
            confirmButton = {
                NnagaTextButton(onClick = {
                    val captured = pendingClip ?: return@NnagaTextButton
                    pendingClip = null
                    onLoadClip(captured.trackId, captured.slot, captured.uri)
                }) { Text("Load") }
            },
            dismissButton = {
                NnagaTextButton(onClick = { pendingClip = null }) { Text("Cancel") }
            },
        )
    }
}

private data class PendingClipRequest(
    val uri: Uri,
    val trackId: RackPathId,
    val slot: Int,
    val trackLabel: String,
    val fileName: String,
)

@Composable
private fun ClipTreeRow(
    row: ClipBrowserRow,
    selectedTrackId: RackPathId?,
    onToggleDirectory: (Uri) -> Unit,
    onMediaClick: (Uri, String) -> Unit,
) {
    val isEnabled = row.isDirectory || selectedTrackId != null
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .padding(start = (12 + row.depth * 20).dp, end = 8.dp)
            .heightIn(min = 48.dp)
            .clickable(
                enabled = isEnabled,
                onClick = {
                    if (row.isDirectory) onToggleDirectory(row.uri)
                    else onMediaClick(row.uri, row.name)
                },
            )
            .padding(vertical = 8.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        if (row.isDirectory) {
            Icon(imageVector = Icons.Default.Folder, contentDescription = null, modifier = Modifier.size(22.dp))
        } else {
            FontaudioIcon(
                glyph = when {
                    row.mimeType.contains("midi", ignoreCase = true) -> FontaudioGlyph.MIDI_PLUG
                    row.name.endsWith(".mid", ignoreCase = true) ||
                        row.name.endsWith(".midi", ignoreCase = true) -> FontaudioGlyph.KEYBOARD
                    else -> FontaudioGlyph.WAVEFORM
                },
                contentDescription = null,
                modifier = Modifier,
                size = 22.dp,
            )
        }
        Spacer(modifier = Modifier.width(8.dp))
        Text(
            text = row.name,
            modifier = Modifier.weight(1f),
            maxLines = 1,
            overflow = TextOverflow.Ellipsis,
        )
        if (row.isDirectory) {
            Icon(
                imageVector = if (row.isExpanded) Icons.Default.ExpandLess else Icons.Default.ExpandMore,
                contentDescription = if (row.isExpanded) "Collapse" else "Expand",
            )
        }
        if (row.isLoading) {
            CircularProgressIndicator(modifier = Modifier.size(16.dp), strokeWidth = 2.dp)
        }
    }
}
private data class PendingPlugin(
    val plugin: PluginInfo,
    val pathId: RackPathId,
    val trackLabel: String,
    val replaceIndex: Int?,
)

@Composable
private fun PluginsBrowserTab(
    selectedTrackId: RackPathId?,
    selectedTrackLabel: String?,
    replaceIndex: Int?,
    onPluginAdded: (RackPathId) -> Unit,
    snackbarHostState: SnackbarHostState,
    modifier: Modifier,
    viewModel: PluginBrowserViewModel = viewModel(),
) {
    val blockingOperation by viewModel.blockingOperation.collectAsState()
    val addFailureMessage by viewModel.addFailureMessage.collectAsState()
    val scope = rememberCoroutineScope()
    var pendingPlugin by remember { mutableStateOf<PendingPlugin?>(null) }

    LaunchedEffect(addFailureMessage) {
        addFailureMessage?.let { message ->
            snackbarHostState.showSnackbar(message)
            viewModel.clearAddFailureMessage()
        }
    }
    BackHandler(enabled = blockingOperation != null) { }

    Box(modifier = modifier.fillMaxSize()) {
        Column(modifier = Modifier.fillMaxSize()) {
            TargetBanner(selectedTrackLabel, slot = 0)
            PluginBrowserList(
                viewModel = viewModel,
                onPluginClick = { plugin ->
                    selectedTrackId?.let { trackId ->
                        pendingPlugin = PendingPlugin(
                            plugin = plugin,
                            pathId = trackId,
                            trackLabel = selectedTrackLabel ?: "Track",
                            replaceIndex = replaceIndex,
                        )
                    }
                },
                pluginItemsEnabled = selectedTrackId != null,
                modifier = Modifier.weight(1f),
            )
        }
        blockingOperation?.let { BrowserBlockingOperationOverlay(it) }
    }

    pendingPlugin?.let { request ->
        AlertDialog(
            onDismissRequest = { if (blockingOperation == null) pendingPlugin = null },
            title = { Text(if (request.replaceIndex == null) "Add plugin?" else "Replace plugin?") },
            text = {
                Text(
                    if (request.replaceIndex == null) {
                        "Add ${request.plugin.name} to ${request.trackLabel}?"
                    } else {
                        "Replace plugin in ${request.trackLabel}?"
                    },
                )
            },
            confirmButton = {
                NnagaTextButton(
                    enabled = blockingOperation == null,
                    onClick = {
                        pendingPlugin = null
                        scope.launch {
                            val success = request.replaceIndex?.let {
                                viewModel.replacePluginInRack(request.pathId, it, request.plugin)
                            } ?: viewModel.addPluginToRack(request.pathId, request.plugin)
                            if (success) onPluginAdded(request.pathId)
                        }
                    },
                ) { Text(if (request.replaceIndex == null) "Add" else "Replace") }
            },
            dismissButton = {
                NnagaTextButton(
                    enabled = blockingOperation == null,
                    onClick = { pendingPlugin = null },
                ) { Text("Cancel") }
            },
        )
    }
}
