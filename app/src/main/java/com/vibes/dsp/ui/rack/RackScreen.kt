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

package com.vibes.dsp.ui.rack

import androidx.compose.foundation.ExperimentalFoundationApi
import androidx.compose.foundation.gestures.detectDragGestures
import androidx.compose.foundation.gestures.detectDragGesturesAfterLongPress
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.verticalScroll
import androidx.compose.foundation.combinedClickable
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.zIndex
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Add
import androidx.compose.material.icons.filled.ExpandLess
import androidx.compose.material.icons.filled.Extension
import androidx.compose.material.icons.filled.ExpandMore
import androidx.compose.material.icons.filled.Favorite
import androidx.compose.material.icons.filled.FavoriteBorder
import androidx.compose.material.icons.filled.FiberManualRecord
import androidx.compose.material.icons.filled.Folder
import androidx.compose.material.icons.filled.Close
import androidx.compose.material.icons.filled.Cloud
import androidx.compose.material.icons.filled.Dashboard
import androidx.compose.material.icons.filled.Mic
import androidx.compose.material.icons.filled.Delete
import androidx.compose.material.icons.filled.DesktopWindows
import androidx.compose.material.icons.filled.Fullscreen
import androidx.compose.material.icons.filled.Keyboard
import androidx.compose.material.icons.filled.MoreVert
import androidx.compose.material.icons.filled.Settings
import androidx.compose.material.icons.filled.MusicNote
import androidx.compose.material.icons.filled.Pause
import androidx.compose.material.icons.filled.Repeat
import androidx.compose.material.icons.filled.SkipPrevious
import androidx.compose.material.icons.filled.StopCircle
import androidx.compose.material.icons.filled.SwapHoriz
import androidx.compose.material.icons.filled.Tune
import androidx.compose.material.icons.filled.PlayArrow
import androidx.compose.material.icons.filled.Stop
import androidx.compose.material.icons.filled.VolumeUp
import androidx.compose.material3.*
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateListOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.produceState
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.platform.LocalLifecycleOwner
import androidx.compose.ui.platform.LocalView
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.LifecycleEventObserver
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.draw.clip
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.disabled
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.semantics.stateDescription
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import android.app.Activity
import android.content.pm.ActivityInfo
import android.provider.OpenableColumns
import android.util.Log
import androidx.activity.compose.BackHandler
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.ui.layout.boundsInWindow
import androidx.compose.ui.layout.onGloballyPositioned
import androidx.lifecycle.viewmodel.compose.viewModel
import com.vibes.dsp.engine.RackManager
import com.vibes.dsp.engine.X11Bridge
import com.vibes.dsp.engine.PluginInfo
import com.vibes.dsp.engine.UiType
import com.vibes.dsp.engine.DirectUsbSessionState
import com.vibes.dsp.engine.DirectUsbAudioManager
import com.vibes.dsp.engine.MASTER_PATH_ID
import com.vibes.dsp.engine.TrackLaunchQuantization
import com.vibes.dsp.ui.modgui.InlineModguiView
import com.vibes.dsp.ui.x11.PluginX11UiView
import com.vibes.dsp.ui.x11.X11DisplayManager
import com.vibes.dsp.BuildConfig
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import android.net.Uri
import androidx.compose.runtime.key
import androidx.compose.runtime.rememberCoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.File
import java.io.IOException
import androidx.compose.animation.core.animateFloatAsState
import androidx.compose.animation.core.tween
import androidx.compose.animation.core.RepeatMode
import androidx.compose.animation.core.animateFloat
import androidx.compose.animation.core.infiniteRepeatable
import androidx.compose.animation.core.rememberInfiniteTransition
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableStateMapOf
import androidx.compose.runtime.toMutableStateList
import androidx.compose.animation.core.Animatable
import androidx.compose.animation.core.AnimationVector1D
import androidx.compose.animation.core.spring
import androidx.compose.ui.input.nestedscroll.NestedScrollConnection
import androidx.compose.ui.input.nestedscroll.NestedScrollSource
import androidx.compose.ui.input.nestedscroll.nestedScroll
import kotlin.math.roundToInt

private val PunchArmedColor = Color(0xFFFFC107)

/** Item positions for drag-drop: (top in viewport px, height px) per plugin index. */
private class ScrollableDragDropState(
    private val itemPositions: () -> List<Pair<Int, Int>>,
    private val onMove: (Int, Int) -> Unit
) {
    var draggedIndex by mutableStateOf<Int?>(null)
        private set
    var draggedOffset by mutableFloatStateOf(0f)
        private set
    private var draggedInitialTop = 0
    private var originalIndex = -1

    fun onDragStart(viewportY: Float) {
        val positions = itemPositions()
        val y = viewportY.toInt()
        val idx = positions.indexOfFirst { (top, h) -> y in top until (top + h) }
        if (idx < 0) return
        draggedIndex = idx
        originalIndex = idx
        val (top, height) = positions[idx]
        draggedInitialTop = top + height / 2
        draggedOffset = 0f
    }

    fun onDrag(change: Float) {
        draggedOffset += change
        val dragged = draggedIndex ?: return
        val positions = itemPositions()
        if (dragged >= positions.size) return
        val (draggedTop, draggedHeight) = positions[dragged]
        val offsetPx = draggedOffset.toInt()
        // Use center-to-midpoint: swap when dragged center passes neighbor's midpoint.
        // This prevents oscillation at boundaries (edge-based triggers jitter).
        val draggedCenter = draggedTop + draggedHeight / 2 + offsetPx

        val below = (dragged + 1 until positions.size).firstOrNull()
        val above = (dragged - 1 downTo 0).firstOrNull()
        val targetIndex = when {
            below != null && draggedCenter > positions[below].first + positions[below].second / 2 -> below
            above != null && draggedCenter < positions[above].first + positions[above].second / 2 -> above
            else -> null
        }
        if (targetIndex != null && targetIndex != dragged) {
            onMove(dragged, targetIndex)
            val (targetTop, targetHeight) = positions[targetIndex]
            val shift = if (targetIndex > dragged) {
                (targetTop + targetHeight) - (draggedTop + draggedHeight)
            } else {
                targetTop - draggedTop
            }
            draggedInitialTop += shift
            draggedOffset -= shift
            draggedIndex = targetIndex
        }
    }

    fun onDragEnd(): Pair<Int, Int>? {
        val result = draggedIndex?.let { current ->
            if (originalIndex != current) Pair(originalIndex, current) else null
        }
        draggedIndex = null
        draggedOffset = 0f
        originalIndex = -1
        return result
    }

    fun onDragCancel() {
        draggedIndex = null
        draggedOffset = 0f
        originalIndex = -1
    }
}

@Composable
private fun rememberScrollableDragDropState(
    itemPositions: () -> List<Pair<Int, Int>>,
    onMove: (Int, Int) -> Unit
): ScrollableDragDropState {
    return remember(itemPositions, onMove) { ScrollableDragDropState(itemPositions, onMove) }
}

@OptIn(ExperimentalMaterial3Api::class, ExperimentalFoundationApi::class)
@Composable
fun RackScreen(
    isVisible: Boolean = true,
    onNavigateToBrowser: (Long) -> Unit,
    onNavigateToSettings: () -> Unit = {},
    onNavigateToTone3000: (String?, String?, String?, Int, String?) -> Unit = { _, _, _, _, _ -> },
    onReplacePlugin: (Long, Int) -> Unit = { _, _ -> },
    viewModel: RackViewModel = viewModel()
) {
    val lifecycleOwner = LocalLifecycleOwner.current
    DisposableEffect(lifecycleOwner, isVisible) {
        val observer = LifecycleEventObserver { _, event ->
            if (event == Lifecycle.Event.ON_RESUME && isVisible) {
                viewModel.refreshRack()
            }
        }
        lifecycleOwner.lifecycle.addObserver(observer)
        onDispose { lifecycleOwner.lifecycle.removeObserver(observer) }
    }
    DisposableEffect(viewModel) {
        onDispose { viewModel.setRackVisible(false) }
    }

    val context = LocalContext.current

    // Refresh when navigating back to this screen (isVisible changes to true)
    LaunchedEffect(isVisible) {
        viewModel.setRackVisible(isVisible)
        if (isVisible) {
            viewModel.refreshRack()
        }
    }

    val isEngineRunning by viewModel.isEngineRunning.collectAsState()
    val tracks by viewModel.tracks.collectAsState()
    val selectedPathId by viewModel.selectedPathId.collectAsState()
    val rackPlugins by viewModel.selectedPathPlugins.collectAsState()
    val transport by viewModel.transport.collectAsState()
    val errorMessage by viewModel.errorMessage.collectAsState()
    val blockingOperation by viewModel.blockingOperation.collectAsState()
    val selectedTrack = tracks.firstOrNull { it.id == selectedPathId }
    val inputChannelCount = DirectUsbAudioManager.getInputChannelCount()
    var launchQuantizationOrdinal by rememberSaveable(selectedTrack?.id) { mutableIntStateOf(0) }
    val launchQuantization = TrackLaunchQuantization.entries[launchQuantizationOrdinal]
    var loopLengthsByTrack by rememberSaveable {
        mutableStateOf<Map<Long, Double>>(emptyMap())
    }
    var showTempoDialog by rememberSaveable { mutableStateOf(false) }
    var tempoInput by rememberSaveable { mutableStateOf("") }




    // Fullscreen state
    var fullscreenPluginIndex by rememberSaveable { mutableStateOf<Int?>(null) }
    var fullscreenPluginWidth by rememberSaveable { mutableIntStateOf(0) }
    var fullscreenPluginHeight by rememberSaveable { mutableIntStateOf(0) }
    val isFullscreenActive = fullscreenPluginIndex != null

    // Orientation handling for fullscreen
    val activity = context as? Activity
    LaunchedEffect(fullscreenPluginIndex) {
        if (fullscreenPluginIndex != null && fullscreenPluginWidth > 0 && fullscreenPluginHeight > 0) {
            if (fullscreenPluginWidth > fullscreenPluginHeight * 1.3) {
                activity?.requestedOrientation = ActivityInfo.SCREEN_ORIENTATION_USER_LANDSCAPE
            }
        } else if (fullscreenPluginIndex == null) {
            activity?.requestedOrientation = ActivityInfo.SCREEN_ORIENTATION_PORTRAIT
        }
    }
    DisposableEffect(Unit) {
        onDispose { activity?.requestedOrientation = ActivityInfo.SCREEN_ORIENTATION_PORTRAIT }
    }
    BackHandler(enabled = isFullscreenActive) {
        fullscreenPluginIndex = null
    }
    BackHandler(enabled = blockingOperation != null) { }

    val scope = rememberCoroutineScope()
    var pendingAudioTargetId by remember { mutableStateOf<Long?>(null) }
    val pendingTrackLaunches = remember { mutableStateMapOf<Long, Boolean>() }
    val audioFilePickerLauncher = rememberLauncherForActivityResult(
        contract = ActivityResultContracts.OpenDocument()
    ) { uri: Uri? ->
        val target = pendingAudioTargetId
        pendingAudioTargetId = null
        if (uri != null && target != null) {
            scope.launch {
                val name = queryDisplayName(context, uri) ?: uri.lastPathSegment ?: "Imported audio"
                viewModel.importTrackAudio(target, uri, name)
            }
        }
    }
    val snackbarHostState = remember { SnackbarHostState() }


    Box(modifier = Modifier.fillMaxSize()) {
        Scaffold(
            contentWindowInsets = WindowInsets(0, 0, 0, 0),
            snackbarHost = { SnackbarHost(snackbarHostState) },
            topBar = {
            // Keep high-frequency meter/status invalidations inside the top bar
            // restart scope instead of recomposing the complete rack screen.
            val meterState by viewModel.meterState.collectAsState()
            val inputLevel = meterState.inputLevel
            val outputLevel = meterState.outputLevel
            val inputClipping = meterState.inputClipping
            val outputClipping = meterState.outputClipping
            val cpuLoad by viewModel.cpuLoad.collectAsState()
            val xRunCount by viewModel.xRunCount.collectAsState()
            val directUsbState by viewModel.directUsbState.collectAsState()
            val directUsbStats by viewModel.directUsbStats.collectAsState()
            val statusText = when (directUsbState) {
                DirectUsbSessionState.Starting,
                DirectUsbSessionState.Stopping -> "Starting USB"
                DirectUsbSessionState.Failed -> "USB failed"
                DirectUsbSessionState.Running -> {
                    var s = "Running · DSP ~%.1f ms · CPU %.0f%%".format(
                        if (directUsbStats.sampleRateHz > 0)
                            directUsbStats.knownHostLatencyFrames.toDouble() /
                                directUsbStats.sampleRateHz * 1000.0 else 0.0,
                        cpuLoad * 100f
                    )
                    if (xRunCount > 0) s += " · XRuns $xRunCount"
                    if (inputClipping || outputClipping) s += " · Clip!"
                    s
                }
                else -> "Stopped"
            }
            val statusColor = if (directUsbState == DirectUsbSessionState.Failed ||
                inputClipping || outputClipping)
                MaterialTheme.colorScheme.error
            else
                MaterialTheme.colorScheme.onSurfaceVariant

            val cutoutPadding = WindowInsets.displayCutout.asPaddingValues()
            val cutoutTop = cutoutPadding.calculateTopPadding()

            // Get actual cutout bounds to center VU meters in each half
            val view = LocalView.current
            val density = LocalDensity.current
            val displayCutout = view.rootWindowInsets?.displayCutout
            val screenWidthPx = view.width.toFloat()
            // Find the cutout center zone (left edge to right edge) with generous padding
            val cutoutCenterStartDp: Dp
            val cutoutCenterEndDp: Dp
            if (displayCutout != null && displayCutout.boundingRects.isNotEmpty()) {
                val topCutout = displayCutout.boundingRects.firstOrNull { it.top == 0 }
                    ?: displayCutout.boundingRects[0]
                val extraPadding = with(density) { 4.dp.toPx() }
                cutoutCenterStartDp = with(density) { (topCutout.left - extraPadding).coerceAtLeast(0f).toDp() }
                cutoutCenterEndDp = with(density) { (screenWidthPx - topCutout.right - extraPadding).coerceAtLeast(0f).toDp() }
            } else {
                cutoutCenterStartDp = 0.dp
                cutoutCenterEndDp = 0.dp
            }

            Surface(
                color = MaterialTheme.colorScheme.background,
                modifier = Modifier.fillMaxWidth()
            ) {
                Column(modifier = Modifier.fillMaxWidth()) {
                    // VU meters row — sits inside the cutout/status bar area
                    if (isEngineRunning) {
                        BoxWithConstraints(
                            modifier = Modifier
                                .fillMaxWidth()
                                .height(cutoutTop)
                        ) {
                            val halfWidth = minOf(cutoutCenterStartDp, cutoutCenterEndDp)
                            val meterWidth = halfWidth * 0.8f

                            Row(
                                modifier = Modifier.fillMaxSize(),
                                horizontalArrangement = Arrangement.SpaceEvenly,
                                verticalAlignment = Alignment.CenterVertically
                            ) {
                                // In VU meter — centered in left half
                                Box(
                                    modifier = Modifier.width(halfWidth),
                                    contentAlignment = Alignment.Center
                                ) {
                                    VuMeter(
                                        modifier = Modifier.width(meterWidth),
                                        label = "In",
                                        level = inputLevel,
                                        clipping = inputClipping,
                                        onClippingTap = { viewModel.resetClipping() }
                                    )
                                }
                                // Out VU meter — centered in right half
                                Box(
                                    modifier = Modifier.width(halfWidth),
                                    contentAlignment = Alignment.Center
                                ) {
                                    VuMeter(
                                        modifier = Modifier.width(meterWidth),
                                        label = "Out",
                                        level = outputLevel,
                                        clipping = outputClipping,
                                        onClippingTap = { viewModel.resetClipping() }
                                    )
                                }
                            }
                        }
                    } else {
                        Spacer(modifier = Modifier.height(cutoutTop))
                    }

                    // Status row
                    Row(
                        modifier = Modifier
                            .fillMaxWidth()
                            .padding(horizontal = 8.dp, vertical = 2.dp),
                        verticalAlignment = Alignment.CenterVertically
                    ) {
                        Text(
                            text = statusText,
                            style = MaterialTheme.typography.bodySmall,
                            color = statusColor,
                            modifier = Modifier
                                .weight(1f)
                                .clickable(enabled = inputClipping || outputClipping) {
                                    viewModel.resetClipping()
                                }
                        )
                        Box {
                            var showOverflowMenu by remember { mutableStateOf(false) }
                            IconButton(
                                onClick = { showOverflowMenu = true },
                                modifier = Modifier.size(28.dp)
                            ) {
                                Icon(
                                    Icons.Default.MoreVert,
                                    contentDescription = "More options",
                                    modifier = Modifier.size(18.dp)
                                )
                            }
                            DropdownMenu(
                                expanded = showOverflowMenu,
                                onDismissRequest = { showOverflowMenu = false }
                            ) {
                                DropdownMenuItem(
                                    text = { Text("Add plugin") },
                                    onClick = {
                                        showOverflowMenu = false
                                        onNavigateToBrowser(selectedPathId)
                                    },
                                    leadingIcon = {
                                        Icon(Icons.Default.Add, contentDescription = null)
                                    },
                                    modifier = Modifier.testTag("rack_add_plugin")
                                )
                                DropdownMenuItem(
                                    text = { Text("Add track") },
                                    onClick = {
                                        showOverflowMenu = false
                                        viewModel.addTrack()
                                    },
                                    leadingIcon = {
                                        Icon(Icons.Default.Add, contentDescription = null)
                                    },
                                    modifier = Modifier.testTag("rack_add_track")
                                )
                                DropdownMenuItem(
                                    text = { Text("Master effects") },
                                    onClick = {
                                        showOverflowMenu = false
                                        viewModel.selectPath(MASTER_PATH_ID)
                                    },
                                    leadingIcon = {
                                        Icon(
                                            Icons.Default.Tune,
                                            contentDescription = null,
                                            modifier = Modifier.size(20.dp)
                                        )
                                    }
                                )
                                Divider()
                                DropdownMenuItem(
                                    text = { Text("Settings") },
                                    onClick = {
                                        showOverflowMenu = false
                                        onNavigateToSettings()
                                    },
                                    leadingIcon = {
                                        Icon(
                                            Icons.Default.Settings,
                                            contentDescription = null,
                                            modifier = Modifier.size(20.dp)
                                        )
                                    }
                                )
                            }
                        }
                    }
                }
            }
        }
        ) { padding ->

            Column(
                modifier = Modifier
                    .fillMaxSize()
                    .padding(if (isFullscreenActive) PaddingValues(0.dp) else padding)
            ) {
    Row(
        Modifier.fillMaxWidth().padding(horizontal = 8.dp, vertical = 4.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(4.dp)
    ) {
        IconButton(
            onClick = { if (transport.playing) viewModel.transportPause() else viewModel.transportPlay() },
            modifier = Modifier.size(40.dp)
        ) {
            Icon(
                if (transport.playing) Icons.Default.Pause else Icons.Default.PlayArrow,
                if (transport.playing) "Pause global transport" else "Start global transport"
            )
        }
        IconButton(
            onClick = { viewModel.transportRestart() },
            modifier = Modifier.size(40.dp)
        ) {
            Icon(Icons.Default.SkipPrevious, "Restart global transport")
        }
        Text(
            "${formatMusicalPosition(transport.positionSec, transport.beatsPerMinute)} · " +
                formatElapsedTime(transport.positionSec),
            modifier = Modifier.weight(1f),
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            maxLines = 1
        )
        TextButton(
            onClick = {
                tempoInput = transport.beatsPerMinute.toString()
                showTempoDialog = true
            },
            modifier = Modifier.heightIn(min = 40.dp),
            contentPadding = PaddingValues(horizontal = 6.dp, vertical = 0.dp)
        ) {
            Text(
                "${transport.beatsPerMinute.toInt()} BPM",
                style = MaterialTheme.typography.bodySmall
            )
        }
    }
    if (showTempoDialog) {
        val enteredTempo = tempoInput.toDoubleOrNull()
        val validTempo = enteredTempo?.takeIf { it in 20.0..400.0 }
        AlertDialog(
            onDismissRequest = { showTempoDialog = false },
            title = { Text("Global tempo") },
            text = {
                Column(verticalArrangement = Arrangement.spacedBy(12.dp)) {
                    OutlinedTextField(
                        value = tempoInput,
                        onValueChange = { tempoInput = it },
                        label = { Text("BPM") },
                        singleLine = true,
                        keyboardOptions = androidx.compose.foundation.text.KeyboardOptions(
                            keyboardType = androidx.compose.ui.text.input.KeyboardType.Decimal
                        ),
                        isError = tempoInput.isNotBlank() && validTempo == null,
                        supportingText = {
                            if (tempoInput.isNotBlank() && validTempo == null) {
                                Text("Enter a value from 20 to 400 BPM")
                            }
                        }
                    )
                    Row(
                        Modifier.fillMaxWidth(),
                        horizontalArrangement = Arrangement.spacedBy(8.dp, Alignment.CenterHorizontally),
                        verticalAlignment = Alignment.CenterVertically
                    ) {
                        TextButton(
                            onClick = {
                                tempoInput = ((enteredTempo ?: transport.beatsPerMinute) - 1.0)
                                    .coerceIn(20.0, 400.0).toString()
                            }
                        ) { Text("−") }
                        Text(
                            validTempo?.let { "${it.toInt()} BPM" } ?: "—",
                            style = MaterialTheme.typography.titleMedium
                        )
                        TextButton(
                            onClick = {
                                tempoInput = ((enteredTempo ?: transport.beatsPerMinute) + 1.0)
                                    .coerceIn(20.0, 400.0).toString()
                            }
                        ) { Text("+") }
                    }
                }
            },
            dismissButton = {
                TextButton(onClick = { showTempoDialog = false }) { Text("Cancel") }
            },
            confirmButton = {
                TextButton(
                    enabled = validTempo != null,
                    onClick = {
                        viewModel.setTransportBpm(validTempo ?: return@TextButton)
                        showTempoDialog = false
                    }
                ) { Text("Apply") }
            }
        )
    }
    if (selectedPathId != MASTER_PATH_ID) {
        Row(
            Modifier
                .fillMaxWidth()
                .padding(horizontal = 8.dp)
                .horizontalScroll(rememberScrollState()),
            horizontalArrangement = Arrangement.spacedBy(8.dp),
            verticalAlignment = Alignment.CenterVertically
        ) {
            tracks.forEachIndexed { index, track ->
                var deleteMenuExpanded by remember(track.id) { mutableStateOf(false) }
                Box(
                    modifier = Modifier
                        .heightIn(min = 44.dp)
                        .combinedClickable(
                            onClick = { viewModel.selectPath(track.id) },
                            onLongClick = { deleteMenuExpanded = true }
                        )
                ) {
                    Surface(
                        modifier = Modifier.heightIn(min = 44.dp),
                        shape = RoundedCornerShape(8.dp),
                        color = if (track.id == selectedPathId) {
                            MaterialTheme.colorScheme.secondaryContainer
                        } else {
                            MaterialTheme.colorScheme.surface
                        },
                        contentColor = if (track.id == selectedPathId) {
                            MaterialTheme.colorScheme.onSecondaryContainer
                        } else {
                            MaterialTheme.colorScheme.onSurface
                        },
                        border = BorderStroke(1.dp, MaterialTheme.colorScheme.outlineVariant)
                    ) {
                        Box(contentAlignment = Alignment.Center) {
                            Text("Track ${index + 1}", modifier = Modifier.padding(horizontal = 16.dp))
                        }
                    }
                    DropdownMenu(
                        expanded = deleteMenuExpanded,
                        onDismissRequest = { deleteMenuExpanded = false }
                    ) {
                        DropdownMenuItem(
                            text = { Text("Delete track") },
                            onClick = {
                                deleteMenuExpanded = false
                                pendingTrackLaunches.remove(track.id)
                                viewModel.removeTrack(track.id)
                            }
                        )
                    }
                }
            }
        }
        selectedTrack?.let { track ->
            var trackTransportMenuExpanded by remember(track.id) { mutableStateOf(false) }
            var launchQuantizeMenuExpanded by remember(track.id) { mutableStateOf(false) }
            var recordMenuExpanded by remember(track.id) { mutableStateOf(false) }
            var loopLengthMenuExpanded by remember(track.id) { mutableStateOf(false) }
            var inputMenuExpanded by remember(track.id) { mutableStateOf(false) }
            var inputChannelMenuExpanded by remember(track.id) { mutableStateOf(false) }
            val loopLengthBars = loopLengthsByTrack[track.id] ?: 1.0
            val loopLengthLabel = when (loopLengthBars) {
                0.25 -> "1/4"
                1.0 -> "1 bar"
                else -> "${loopLengthBars.toInt()} bars"
            }
            val launchPending = pendingTrackLaunches[track.id] == true &&
                !track.playing && track.wavLoaded
            LaunchedEffect(track.id, track.playing, track.wavLoaded) {
                if (track.playing || !track.wavLoaded) pendingTrackLaunches.remove(track.id)
            }
            val playIconAlpha = if (launchPending) {
                val transition = rememberInfiniteTransition(label = "Pending track launch")
                val alpha by transition.animateFloat(
                    initialValue = 1f,
                    targetValue = 0.45f,
                    animationSpec = infiniteRepeatable(
                        animation = tween(durationMillis = 650),
                        repeatMode = RepeatMode.Reverse
                    ),
                    label = "Pending track launch alpha"
                )
                alpha
            } else {
                1f
            }
            val recordIconAlpha = if (track.recordPending) {
                val transition = rememberInfiniteTransition(label = "Pending loop recording")
                val alpha by transition.animateFloat(
                    initialValue = 1f,
                    targetValue = 0.55f,
                    animationSpec = infiniteRepeatable(
                        animation = tween(durationMillis = 850),
                        repeatMode = RepeatMode.Reverse
                    ),
                    label = "Pending loop recording alpha"
                )
                alpha
            } else {
                1f
            }
            Row(
                Modifier.fillMaxWidth().padding(horizontal = 8.dp, vertical = 4.dp),
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(8.dp)
            ) {
                Text("Vol ${(track.volume * 100).roundToInt()}%", color = MaterialTheme.colorScheme.onSurfaceVariant)
                Slider(
                    value = track.volume,
                    onValueChange = { viewModel.setTrackVolume(track.id, it) },
                    modifier = Modifier.weight(1f)
                )
            }
            if (track.wavLoaded) {
                Row(
                    Modifier.fillMaxWidth().padding(horizontal = 8.dp),
                    verticalAlignment = Alignment.CenterVertically,
                    horizontalArrangement = Arrangement.spacedBy(8.dp)
                ) {
                    Text(track.wavDisplayName, Modifier.weight(1f), maxLines = 1)
                    Text("WAV → FX", color = MaterialTheme.colorScheme.onSurfaceVariant)
                }
            }
            Row(
                Modifier.fillMaxWidth().padding(horizontal = 8.dp, vertical = 4.dp),
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(4.dp)
            ) {
                Text(
                    "${formatMusicalPosition(track.positionSec, transport.beatsPerMinute)} · " +
                        formatElapsedTime(track.positionSec),
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    modifier = Modifier.weight(1f)
                )
                Box {
                    Box(
                        modifier = Modifier
                            .size(48.dp)
                            .combinedClickable(
                                onClick = {
                                    viewModel.setTrackInputArmed(track.id, !track.inputArmed)
                                },
                                onLongClick = {
                                    inputMenuExpanded = true
                                    inputChannelMenuExpanded = false
                                }
                            )
                            .semantics {
                                contentDescription = "Record input"
                                stateDescription = if (track.inputArmed) "Active" else "Inactive"
                            },
                        contentAlignment = Alignment.Center
                    ) {
                        Icon(
                            Icons.Default.Mic,
                            contentDescription = null,
                            tint = if (track.inputArmed) MaterialTheme.colorScheme.error
                                else MaterialTheme.colorScheme.onSurfaceVariant
                        )
                    }
                    DropdownMenu(
                        expanded = inputMenuExpanded,
                        onDismissRequest = {
                            inputMenuExpanded = false
                            inputChannelMenuExpanded = false
                        }
                    ) {
                        DropdownMenuItem(
                            text = { Text("Input channel") },
                            enabled = inputChannelCount > 0,
                            onClick = { inputChannelMenuExpanded = true }
                        )
                    }
                    DropdownMenu(
                        expanded = inputChannelMenuExpanded,
                        onDismissRequest = { inputChannelMenuExpanded = false }
                    ) {
                        repeat(inputChannelCount) { channel ->
                            DropdownMenuItem(
                                text = { Text("Input ${channel + 1}") },
                                trailingIcon = {
                                    RadioButton(
                                        selected = track.inputChannel == channel,
                                        onClick = null
                                    )
                                },
                                onClick = {
                                    viewModel.setTrackInputChannel(track.id, channel)
                                    inputChannelMenuExpanded = false
                                    inputMenuExpanded = false
                                }
                            )
                        }
                    }
                }
                Box {
                    val playEnabled = track.wavLoaded
                    Box(
                        modifier = Modifier
                            .size(48.dp)
                            .alpha(if (playEnabled) 1f else 0.38f)
                            .combinedClickable(
                                // Keep the gesture surface active for long-press quantization access,
                                // while guarding normal taps below when no audio is loaded.
                                onClick = {
                                    if (playEnabled) {
                                        when {
                                            track.playing || launchPending -> {
                                                pendingTrackLaunches.remove(track.id)
                                                viewModel.setTrackTransportPlaying(
                                                    track.id,
                                                    false,
                                                    launchQuantization
                                                )
                                            }
                                            else -> {
                                                pendingTrackLaunches[track.id] = true
                                                viewModel.launchTrackTransport(
                                                    track.id,
                                                    launchQuantization,
                                                    startGlobal = !transport.playing
                                                )
                                            }
                                        }
                                    }
                                },
                                onLongClick = {
                                    trackTransportMenuExpanded = true
                                    launchQuantizeMenuExpanded = false
                                }
                            )
                            .semantics {
                                if (!playEnabled) disabled()
                                contentDescription = when {
                                    !playEnabled -> "Track play unavailable until audio is loaded"
                                    track.playing -> "Pause selected track"
                                    launchPending -> "Cancel pending track launch"
                                    else -> "Start selected track"
                                }
                                if (launchPending) stateDescription = "Launch pending"
                            },
                        contentAlignment = Alignment.Center
                    ) {
                        Icon(
                            if (track.playing) Icons.Default.Pause else Icons.Default.PlayArrow,
                            contentDescription = null,
                            modifier = Modifier.alpha(playIconAlpha)
                        )
                    }
                    DropdownMenu(
                        expanded = trackTransportMenuExpanded,
                        onDismissRequest = {
                            trackTransportMenuExpanded = false
                            launchQuantizeMenuExpanded = false
                        }
                    ) {
                        DropdownMenuItem(
                            text = { Text("Launch Quantize") },
                            onClick = { launchQuantizeMenuExpanded = true }
                        )
                    }
                    DropdownMenu(
                        expanded = launchQuantizeMenuExpanded,
                        onDismissRequest = { launchQuantizeMenuExpanded = false }
                    ) {
                        TrackLaunchQuantization.entries.forEach { quantization ->
                            val label = when (quantization) {
                                TrackLaunchQuantization.Bar -> "Bar"
                                TrackLaunchQuantization.Quarter -> "1/4"
                                TrackLaunchQuantization.Eighth -> "1/8"
                                TrackLaunchQuantization.Sixteenth -> "1/16"
                                TrackLaunchQuantization.None -> "None (Immediate)"
                            }
                            DropdownMenuItem(
                                text = { Text(label) },
                                onClick = {
                                    launchQuantizationOrdinal = quantization.ordinal
                                    launchQuantizeMenuExpanded = false
                                    trackTransportMenuExpanded = false
                                }
                            )
                        }
                    }
                }
                IconButton(
                    onClick = { viewModel.setTrackTransportLooping(track.id, !track.looping) },
                    modifier = Modifier.semantics {
                        contentDescription = "Loop selected track"
                        stateDescription = if (track.looping) "Active" else "Inactive"
                    }
                ) {
                    Icon(
                        Icons.Default.Repeat,
                        contentDescription = null,
                        tint = if (track.looping) Color(0xFF2E7D32)
                        else MaterialTheme.colorScheme.onSurfaceVariant
                    )
                }
                when {
                    track.wavLoaded -> {
                        IconButton(onClick = { viewModel.unloadTrackWav(track.id) }) {
                            Icon(Icons.Default.Close, contentDescription = "Unload audio from selected track")
                        }
                    }
                    track.punchArmed || (track.inputArmed && track.looping) -> {
                        Box {
                            Box(
                                modifier = Modifier
                                    .size(48.dp)
                                    .combinedClickable(
                                        enabled = !track.recordPending && !track.recording,
                                        onClick = {
                                            if (!track.punchArmed) {
                                                viewModel.startTrackLoopRecording(
                                                    track.id,
                                                    loopLengthBars,
                                                    launchQuantization,
                                                    startGlobal = !transport.playing
                                                )
                                            }
                                        },
                                        onLongClick = {
                                            recordMenuExpanded = true
                                            loopLengthMenuExpanded = false
                                        }
                                    )
                                    .semantics {
                                        contentDescription = "Record $loopLengthLabel loop"
                                        stateDescription = when {
                                            track.punchArmed -> "Enter on punch armed"
                                            track.recording -> "Recording"
                                            track.recordPending -> "Recording pending"
                                            else -> "Ready"
                                        }
                                    },
                                contentAlignment = Alignment.Center
                            ) {
                                Icon(
                                    Icons.Default.FiberManualRecord,
                                    contentDescription = null,
                                    tint = when {
                                        track.punchArmed -> PunchArmedColor
                                        track.recordPending || track.recording ->
                                            MaterialTheme.colorScheme.error
                                        else -> MaterialTheme.colorScheme.onSurfaceVariant
                                    },
                                    modifier = Modifier.alpha(recordIconAlpha)
                                )
                            }
                            val enterOnPunchEnabled =
                                !transport.playing || launchQuantization == TrackLaunchQuantization.None
                            DropdownMenu(
                                expanded = recordMenuExpanded,
                                onDismissRequest = {
                                    recordMenuExpanded = false
                                    loopLengthMenuExpanded = false
                                }
                            ) {
                                DropdownMenuItem(
                                    text = { Text("Loop length") },
                                    onClick = { loopLengthMenuExpanded = true }
                                )
                                DropdownMenuItem(
                                    text = { Text("Enter on punch") },
                                    enabled = enterOnPunchEnabled,
                                    leadingIcon = {
                                        Checkbox(
                                            checked = track.punchArmed,
                                            onCheckedChange = null,
                                            enabled = enterOnPunchEnabled
                                        )
                                    },
                                    onClick = {
                                        viewModel.setEnterOnPunch(
                                            track.id,
                                            loopLengthBars,
                                            launchQuantization,
                                            armed = !track.punchArmed
                                        )
                                        recordMenuExpanded = false
                                        loopLengthMenuExpanded = false
                                    }
                                )
                            }
                            DropdownMenu(
                                expanded = loopLengthMenuExpanded,
                                onDismissRequest = { loopLengthMenuExpanded = false }
                            ) {
                                listOf(
                                    0.25 to "1/4",
                                    1.0 to "1 bar",
                                    2.0 to "2 bars",
                                    4.0 to "4 bars",
                                    8.0 to "8 bars",
                                    16.0 to "16 bars"
                                ).forEach { (bars, label) ->
                                    DropdownMenuItem(
                                        text = { Text(label) },
                                        onClick = {
                                            loopLengthsByTrack = loopLengthsByTrack + (track.id to bars)
                                            loopLengthMenuExpanded = false
                                            recordMenuExpanded = false
                                        }
                                    )
                                }
                            }
                        }
                    }
                    else -> {
                        IconButton(
                            onClick = {
                                pendingAudioTargetId = track.id
                                audioFilePickerLauncher.launch(
                                    arrayOf("audio/wav", "audio/x-wav", "audio/mpeg", "audio/ogg", "audio/mp4", "audio/x-m4a")
                                )
                            }
                        ) {
                            Icon(Icons.Default.Folder, contentDescription = "Load audio for selected track")
                        }
                    }
                }
            }
        }
    } else {
        Row(
            Modifier
                .fillMaxWidth()
                .padding(horizontal = 8.dp)
                .horizontalScroll(rememberScrollState()),
            horizontalArrangement = Arrangement.spacedBy(8.dp),
            verticalAlignment = Alignment.CenterVertically
        ) {
            tracks.forEachIndexed { index, track ->
                var deleteMenuExpanded by remember(track.id) { mutableStateOf(false) }
                Box(
                    modifier = Modifier
                        .heightIn(min = 44.dp)
                        .combinedClickable(
                            onClick = { viewModel.selectPath(track.id) },
                            onLongClick = { deleteMenuExpanded = true }
                        )
                ) {
                    Surface(
                        modifier = Modifier.heightIn(min = 44.dp),
                        shape = RoundedCornerShape(8.dp),
                        color = MaterialTheme.colorScheme.surface,
                        contentColor = MaterialTheme.colorScheme.onSurface,
                        border = BorderStroke(1.dp, MaterialTheme.colorScheme.outlineVariant)
                    ) {
                        Box(contentAlignment = Alignment.Center) {
                            Text("Track ${index + 1}", modifier = Modifier.padding(horizontal = 16.dp))
                        }
                    }
                    DropdownMenu(
                        expanded = deleteMenuExpanded,
                        onDismissRequest = { deleteMenuExpanded = false }
                    ) {
                        DropdownMenuItem(
                            text = { Text("Delete track") },
                            onClick = {
                                deleteMenuExpanded = false
                                pendingTrackLaunches.remove(track.id)
                                viewModel.removeTrack(track.id)
                            }
                        )
                    }
                }
            }
        }
        // Master path does not show track-level transport/record controls here.
        }
        // Drag-reorder state — use scrollable Column so all plugin cards stay in composition (no re-render when scrolling)
        val localPlugins = remember { mutableStateOf(rackPlugins.toMutableStateList()) }
        val scrollState = rememberScrollState()
        val viewportTopInRoot = remember { mutableStateOf(0f) }
        val itemPositions = remember { mutableStateListOf<Pair<Int, Int>>() }
        val swapOffsets = remember { mutableStateMapOf<Long, Float>() }
        val swapAnimTriggers = remember { mutableStateMapOf<Long, Int>() }
        val dragDropState = rememberScrollableDragDropState(
            itemPositions = { itemPositions.toList() },
            onMove = { from, to ->
                val list = localPlugins.value
                if (from in list.indices && to in list.indices) {
                    val displaced = list[to]
                    val positions = itemPositions.toList()
                    if (from < positions.size && to < positions.size) {
                        val displacement = (positions[to].first - positions[from].first).toFloat()
                        if (displacement != 0f) {
                            swapOffsets[displaced.instanceId] = displacement
                            swapAnimTriggers[displaced.instanceId] =
                                (swapAnimTriggers[displaced.instanceId] ?: 0) + 1
                        }
                    }
                    val item = list.removeAt(from)
                    list.add(to, item)
                }
            }
        )
        // Sync from viewmodel only when not dragging
        LaunchedEffect(rackPlugins, dragDropState.draggedIndex) {
            if (dragDropState.draggedIndex == null) {
                localPlugins.value = rackPlugins.toMutableStateList()
                swapOffsets.clear()
                swapAnimTriggers.clear()
            }
        }
        // Keep positions list size in sync with plugin count (so onGloballyPositioned can set itemPositions[index])
        val pluginCount = localPlugins.value.size
        if (itemPositions.size < pluginCount) {
            repeat(pluginCount - itemPositions.size) { itemPositions.add(0 to 0) }
        } else if (itemPositions.size > pluginCount) {
            repeat(itemPositions.size - pluginCount) { itemPositions.removeAt(itemPositions.size - 1) }
        }

        val isDragging = dragDropState.draggedIndex != null
        val listScale by animateFloatAsState(
            targetValue = if (isDragging) 0.7f else 1f,
            animationSpec = tween(durationMillis = 250),
            label = "rackZoom"
        )

        // Block scroll dispatching while a drag is active
        val dragScrollBlocker = remember {
            object : NestedScrollConnection {
                override fun onPreScroll(available: Offset, source: NestedScrollSource): Offset {
                    return if (dragDropState.draggedIndex != null) available else Offset.Zero
                }
            }
        }

        BoxWithConstraints(
            modifier = Modifier
                .fillMaxWidth()
                .weight(1f)
        ) {
            val viewportHeight = maxHeight
            val contentHeight = (viewportHeight / listScale).coerceAtLeast(viewportHeight)
            Column(
                modifier = Modifier
                    .fillMaxSize()
                    .then(if (isFullscreenActive) Modifier else Modifier.nestedScroll(dragScrollBlocker))
                    .then(if (isFullscreenActive) Modifier else Modifier.verticalScroll(scrollState))
                    .onGloballyPositioned { coords ->
                        viewportTopInRoot.value = coords.boundsInWindow().top
                    }
                    .then(if (isFullscreenActive) Modifier else Modifier.pointerInput(Unit) {
                    detectDragGesturesAfterLongPress(
                        onDragStart = { offset ->
                            dragDropState.onDragStart(offset.y)
                        },
                        onDrag = { change, dragAmount ->
                            change.consume()
                            dragDropState.onDrag(dragAmount.y)
                        },
                        onDragEnd = {
                            dragDropState.onDragEnd()?.let { (from, to) ->
                                viewModel.reorderPlugins(selectedPathId, from, to)
                            }
                        },
                        onDragCancel = {
                            dragDropState.onDragCancel()
                        }
                    )
                })
            ) {
                Box(
                    modifier = Modifier
                        .fillMaxWidth()
                        .then(if (isDragging) Modifier.height(contentHeight) else Modifier.wrapContentHeight())
                        .graphicsLayer {
                            scaleX = listScale
                            scaleY = listScale
                            transformOrigin = androidx.compose.ui.graphics.TransformOrigin(0.5f, 0f)
                        }
                ) {
                    Column(
                        modifier = Modifier
                            .fillMaxWidth()
                            .wrapContentHeight()
                    ) {
            if (!isFullscreenActive) {
            Spacer(modifier = Modifier.height(16.dp))
            if (!isEngineRunning) {
                Button(
                    onClick = { viewModel.startEngine() },
                    modifier = Modifier
                        .fillMaxWidth()
                        .heightIn(min = 44.dp)
                ) {
                    Icon(
                        imageVector = Icons.Filled.PlayArrow,
                        contentDescription = null,
                        modifier = Modifier.size(20.dp)
                    )
                    Spacer(modifier = Modifier.width(8.dp))
                    Text("Start engine")
                }
                Spacer(modifier = Modifier.height(8.dp))
            }
            errorMessage?.let { error ->
                Surface(
                    modifier = Modifier.fillMaxWidth(),
                    color = MaterialTheme.colorScheme.errorContainer,
                    shape = RoundedCornerShape(4.dp),
                    border = BorderStroke(1.dp, MaterialTheme.colorScheme.error.copy(alpha = 0.45f))
                ) {
                    Row(
                        modifier = Modifier.fillMaxWidth().padding(start = 16.dp, top = 4.dp, bottom = 4.dp, end = 4.dp),
                        verticalAlignment = Alignment.CenterVertically
                    ) {
                        Text(
                            text = error,
                            modifier = Modifier.weight(1f),
                            color = MaterialTheme.colorScheme.onErrorContainer
                        )
                        IconButton(onClick = { viewModel.clearError() }) {
                            Icon(
                                imageVector = Icons.Filled.Close,
                                contentDescription = "Dismiss",
                                tint = MaterialTheme.colorScheme.onErrorContainer
                            )
                        }
                    }
                }
                Spacer(modifier = Modifier.height(8.dp))
            }
            if (localPlugins.value.isEmpty()) {
                EmptyRackPlaceholder(
                    modifier = Modifier
                        .fillMaxWidth()
                        .padding(vertical = 32.dp)
                )
            }
            } // end !isFullscreenActive
            if (localPlugins.value.isNotEmpty()) {
                Column(
                    modifier = Modifier
                        .fillMaxWidth()
                        .then(if (isFullscreenActive) Modifier.fillMaxHeight() else Modifier),
                    verticalArrangement = if (isFullscreenActive) Arrangement.Top else Arrangement.spacedBy(8.dp)
                ) {
                    localPlugins.value.forEachIndexed { index, plugin ->
                        key(plugin.instanceId) {
                        val isDragged = dragDropState.draggedIndex == index
                        val nativeIndex = plugin.index
                        val isThisPluginFullscreen = fullscreenPluginIndex == nativeIndex
                        val hideThisPlugin = isFullscreenActive && !isThisPluginFullscreen

                        // Displacement animation for non-dragged items during reorder
                        val offsetAnim = remember { Animatable(0f) }
                        val swapTrigger = swapAnimTriggers[plugin.instanceId] ?: 0
                        LaunchedEffect(swapTrigger) {
                            if (swapTrigger > 0) {
                                val target = swapOffsets[plugin.instanceId] ?: 0f
                                if (target != 0f) {
                                    offsetAnim.snapTo(target)
                                    swapOffsets.remove(plugin.instanceId)
                                    offsetAnim.animateTo(
                                        0f,
                                        spring(dampingRatio = 0.8f, stiffness = 800f)
                                    )
                                }
                            }
                        }
                        val rawDisplacement = swapOffsets[plugin.instanceId] ?: 0f
                        val displacementOffset =
                            if (rawDisplacement != 0f) rawDisplacement else offsetAnim.value

                        Box(
                            modifier = Modifier
                                .fillMaxWidth()
                                .then(if (isThisPluginFullscreen) Modifier.fillMaxHeight() else Modifier)
                                .then(if (hideThisPlugin) Modifier.height(0.dp) else Modifier)
                                .onGloballyPositioned { coords ->
                                    if (index < itemPositions.size) {
                                        val b = coords.boundsInWindow()
                                        // Use content-space coordinates (add scrollState.value) to match
                                        // the pointerInput coordinate space which is inner to verticalScroll
                                        val viewportRelative = (b.top - viewportTopInRoot.value).toInt()
                                        itemPositions[index] = (viewportRelative + scrollState.value) to b.height.toInt()
                                    }
                                }
                        ) {
                            PluginCard(
                                plugin = plugin,
                                pluginIndex = nativeIndex,
                                pathId = selectedPathId,
                                viewModel = viewModel,
                                onRemove = { viewModel.removePlugin(selectedPathId, nativeIndex) },
                                onReplace = { onReplacePlugin(selectedPathId, nativeIndex) },
                                isFullscreen = isThisPluginFullscreen,
                                isAnyPluginFullscreen = isFullscreenActive,
                                isRackVisible = isVisible,
                                screenHeight = viewportHeight,
                                onOpenFullscreen = { pluginIdx, _, w, h ->
                                    fullscreenPluginWidth = w
                                    fullscreenPluginHeight = h
                                    fullscreenPluginIndex = pluginIdx
                                },
                                onExitFullscreen = {
                                    fullscreenPluginIndex = null
                                },
                                onNavigateToTone3000 = onNavigateToTone3000,
                                modifier = Modifier
                                    .then(if (isThisPluginFullscreen) Modifier.fillMaxSize() else Modifier.fillMaxWidth())
                                    .zIndex(if (isDragged) 1f else 0f)
                                    .graphicsLayer {
                                        translationY = if (isDragged) dragDropState.draggedOffset
                                            else displacementOffset
                                        scaleX = if (isDragged) 1.05f else 1f
                                        scaleY = if (isDragged) 1.05f else 1f
                                        shadowElevation = 0f
                                    }
                            )
                        }
                        }
                    }
                }
            }
            Spacer(modifier = Modifier.height(16.dp))
                    }
                }
            }
        }
            }
        }
        blockingOperation?.let { label ->
            BlockingOperationOverlay(label = label)
        }
    }
}


@Composable
private fun BlockingOperationOverlay(label: String) {
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
        contentAlignment = Alignment.Center
    ) {
        Surface(
            shape = RoundedCornerShape(4.dp),
            color = MaterialTheme.colorScheme.surface,
            border = BorderStroke(1.dp, MaterialTheme.colorScheme.outlineVariant)
        ) {
            Column(
                modifier = Modifier.padding(horizontal = 28.dp, vertical = 22.dp),
                horizontalAlignment = Alignment.CenterHorizontally,
                verticalArrangement = Arrangement.spacedBy(12.dp)
            ) {
                CircularProgressIndicator(strokeWidth = 3.dp)
                Text(
                    text = "$label...",
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurface
                )
            }
        }
    }
}

@Composable
private fun EmptyRackPlaceholder(modifier: Modifier = Modifier) {
    Box(
        modifier = modifier,
        contentAlignment = Alignment.Center
    ) {
        Column(
            horizontalAlignment = Alignment.CenterHorizontally,
            verticalArrangement = Arrangement.spacedBy(8.dp)
        ) {
            Text(
                text = "No plugins in rack",
                style = MaterialTheme.typography.titleMedium
            )
            Text(
                text = "Tap + to add plugins",
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant
            )
        }
    }
}

private fun defaultScaleForAspectRatio(width: Int, height: Int): Float =
    if (width >= height) 1.0f else 0.4f

/** Fixed height for the X11 plugin viewport so the X server root window matches the plugin box (not full screen). */
private val x11ViewportHeight = 360.dp

@Composable
private fun ResizeHandle(
    currentScale: Float,
    onScaleChange: (Float) -> Unit,
    modifier: Modifier = Modifier
) {
    val scaleState = rememberUpdatedState(currentScale)
    val callbackState = rememberUpdatedState(onScaleChange)
    val outlineColor = MaterialTheme.colorScheme.outline.copy(alpha = 0.7f)
    Box(
        modifier = modifier
            .width(48.dp)
            .height(20.dp)
            .pointerInput(Unit) {
                detectDragGestures { change, dragAmount ->
                    change.consume()
                    val scaleDelta = dragAmount.y / (300.dp.toPx())
                    val newScale = (scaleState.value + scaleDelta).coerceIn(0.3f, 1f)
                    callbackState.value(newScale)
                }
            },
        contentAlignment = Alignment.Center
    ) {
        Canvas(modifier = Modifier.width(24.dp).height(10.dp)) {
            val strokeWidth = 1.5.dp.toPx()
            val color = outlineColor
            val gap = size.height / 4f
            for (i in 0..2) {
                val y = gap + i * gap
                drawLine(
                    color = color,
                    start = Offset(0f, y),
                    end = Offset(size.width, y),
                    strokeWidth = strokeWidth
                )
            }
        }
    }
}

@Composable
private fun VuMeter(
    modifier: Modifier = Modifier,
    label: String,
    level: Float,
    clipping: Boolean,
    onClippingTap: () -> Unit
) {
    val levelColor = when {
        level >= 0.9f -> Color(0xFFF44336)
        level >= 0.7f -> Color(0xFFFFC107)
        else -> Color(0xFF4CAF50)
    }
    Row(
        modifier = modifier,
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(4.dp)
    ) {
        Text(label, style = MaterialTheme.typography.labelSmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
        Box(
            modifier = Modifier
                .weight(1f)
                .height(6.dp)
                .background(MaterialTheme.colorScheme.surfaceVariant)
        ) {
            Box(
                modifier = Modifier
                    .fillMaxHeight()
                    .fillMaxWidth(level.coerceIn(0f, 1f))
                    .background(levelColor)
            )
        }
        Box(
            modifier = Modifier
                .sizeIn(minWidth = 44.dp, minHeight = 44.dp)
                .clickable(onClick = onClippingTap),
            contentAlignment = Alignment.Center
        ) {
            Box(
                modifier = Modifier
                    .size(8.dp)
                    .clip(CircleShape)
                    .background(if (clipping) Color(0xFFF44336) else MaterialTheme.colorScheme.surfaceVariant)
            )
        }
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun PluginCard(
    plugin: RackPlugin,
    pluginIndex: Int,
    pathId: Long,
    viewModel: RackViewModel,
    onRemove: () -> Unit,
    onReplace: () -> Unit = {},
    isFullscreen: Boolean = false,
    isAnyPluginFullscreen: Boolean = false,
    isRackVisible: Boolean = true,
    screenHeight: Dp = 0.dp,
    onOpenFullscreen: (pluginIndex: Int, uiType: UiType, width: Int, height: Int) -> Unit = { _, _, _, _ -> },
    onExitFullscreen: () -> Unit = {},
    onNavigateToTone3000: (String?, String?, String?, Int, String?) -> Unit = { _, _, _, _, _ -> },
    modifier: Modifier = Modifier.fillMaxWidth()
) {
    val selectedPathId = pathId
    var expanded by rememberSaveable { mutableStateOf(true) }
    val uiInstanceId = remember { System.nanoTime() }
    val pluginInfoState = produceState<PluginInfo?>(
        initialValue = null,
        key1 = pathId,
        key2 = plugin.instanceId
    ) {
        value = withContext(Dispatchers.IO) {
            RackManager.getRackPluginInfo(pathId, pluginIndex)
        }
    }
    val pluginInfo = pluginInfoState.value

    Surface(
        modifier = modifier,
        color = MaterialTheme.colorScheme.surface,
        shape = RoundedCornerShape(4.dp),
        border = BorderStroke(1.dp, MaterialTheme.colorScheme.outlineVariant)
    ) {
        Column(
            modifier = Modifier.fillMaxWidth()
        ) {
            // Parameter controls / plugin UI — always in composition to avoid re-renders
            if (pluginInfo != null) {
                // Determine which UI modes are available
                val availableUiTypes = remember(pluginInfo) {
                    buildList {
                        if (pluginInfo.hasX11Ui) add(UiType.X11)
                        if (pluginInfo.hasModgui) add(UiType.MODGUI)
                        add(UiType.SLIDERS)
                    }
                }
                var currentUiMode by rememberSaveable(pluginInfo.fullId) {
                    mutableStateOf(viewModel.getPreferredUiTypeForPlugin(pluginInfo))
                }
                LaunchedEffect(pluginIndex, pluginInfo, currentUiMode) {
                    Log.i("GuitarRackCraft.UI", "Plugin[$pluginIndex] ${pluginInfo.name}: chosen UI mode=$currentUiMode (preferred=${pluginInfo.preferredUiType}, available=${pluginInfo.guiTypes})")
                }

                var x11UserScale by rememberSaveable { mutableStateOf(Float.NaN) }
                var modguiUserScale by rememberSaveable { mutableStateOf(Float.NaN) }

                // Hoisted dimension state — used by fullscreen button for orientation decision
                var x11PluginNaturalW by remember { mutableStateOf(0) }
                var x11PluginNaturalH by remember { mutableStateOf(0) }
                var x11UIScale by remember { mutableStateOf(1f) }
                var modguiContentWidth by remember { mutableStateOf(0) }
                var modguiContentHeight by remember { mutableStateOf(0) }

                // Top bar: context menu + plugin name + expand/remove — hidden in fullscreen
                if (!isFullscreen) Row(
                    modifier = Modifier
                        .fillMaxWidth()
                        .background(MaterialTheme.colorScheme.surface)
                        .padding(horizontal = 4.dp, vertical = 4.dp),
                    verticalAlignment = Alignment.CenterVertically
                ) {
                    // Context menu button
                    var showContextMenu by remember { mutableStateOf(false) }
                    Box {
                        IconButton(
                            onClick = { showContextMenu = true },
                            modifier = Modifier.size(44.dp)
                        ) {
                            Icon(Icons.Default.MoreVert, contentDescription = "Options", modifier = Modifier.size(20.dp))
                        }
                        DropdownMenu(
                            expanded = showContextMenu,
                            onDismissRequest = { showContextMenu = false }
                        ) {
                            // UI mode selection
                            if (availableUiTypes.size > 1) {
                                availableUiTypes.forEach { uiType ->
                                    val isSelected = currentUiMode == uiType
                                    val icon = when (uiType) {
                                        UiType.X11 -> Icons.Default.DesktopWindows
                                        UiType.MODGUI -> Icons.Default.Dashboard
                                        UiType.SLIDERS -> Icons.Default.Tune
                                    }
                                    DropdownMenuItem(
                                        text = {
                                            Text(
                                                uiType.displayName,
                                                fontWeight = if (isSelected) FontWeight.Bold else FontWeight.Normal
                                            )
                                        },
                                        onClick = {
                                            currentUiMode = uiType
                                            viewModel.setPreferredUiTypeForPlugin(pluginInfo.fullId, uiType)
                                            showContextMenu = false
                                        },
                                        leadingIcon = {
                                            Icon(icon, contentDescription = null, modifier = Modifier.size(20.dp))
                                        },
                                        modifier = if (uiType == UiType.X11) Modifier.testTag("plugin_native_ui_button") else Modifier
                                    )
                                }
                                Divider()
                            }
                            // Scale slider
                            val hasScale = (currentUiMode == UiType.X11 && !x11UserScale.isNaN()) ||
                                    (currentUiMode == UiType.MODGUI && !modguiUserScale.isNaN())
                            if (hasScale) {
                                val scaleVal = when (currentUiMode) {
                                    UiType.X11 -> if (x11UserScale.isNaN()) 1f else x11UserScale
                                    UiType.MODGUI -> if (modguiUserScale.isNaN()) 1f else modguiUserScale
                                    else -> 1f
                                }
                                Row(
                                    modifier = Modifier.padding(horizontal = 12.dp),
                                    verticalAlignment = Alignment.CenterVertically
                                ) {
                                    Slider(
                                        value = scaleVal,
                                        onValueChange = { v ->
                                            when (currentUiMode) {
                                                UiType.X11 -> x11UserScale = v
                                                UiType.MODGUI -> modguiUserScale = v
                                                else -> {}
                                            }
                                        },
                                        valueRange = 0.3f..1f,
                                        modifier = Modifier.width(150.dp).padding(horizontal = 4.dp)
                                    )
                                }
                                Divider()
                            }
                        }
                    }

                    Text(
                        text = plugin.name,
                        style = MaterialTheme.typography.titleSmall,
                        fontWeight = FontWeight.Bold,
                        modifier = Modifier.weight(1f)
                    )
                    // Collapse
                    IconButton(
                        onClick = { expanded = !expanded },
                        modifier = Modifier.size(44.dp).testTag("plugin_card_expand")
                    ) {
                        Icon(
                            if (expanded) Icons.Default.ExpandLess else Icons.Default.ExpandMore,
                            contentDescription = if (expanded) "Collapse" else "Expand",
                            modifier = Modifier.size(20.dp)
                        )
                    }
                    // Fullscreen button:
                    //   - LV2 X11 / Modgui: shown when those modes are active (uses LV2 dims)
                    //   - VST: always shown in LANDSCAPE so users can pop the wine editor
                    //     full-screen without first switching into X11 mode (wine editors
                    //     are typically wider than tall and benefit most from landscape).
                    val isLandscapeOrientation = androidx.compose.ui.platform.LocalConfiguration.current.orientation ==
                        android.content.res.Configuration.ORIENTATION_LANDSCAPE
                    val isVstFsCandidate = (pluginInfo.format == "VST2" || pluginInfo.format == "VST3") &&
                        com.vibes.dsp.BuildConfig.HAS_VST_HOST &&
                        isLandscapeOrientation
                    val showFullscreenButton = currentUiMode == UiType.X11 || currentUiMode == UiType.MODGUI ||
                        isVstFsCandidate
                    if (showFullscreenButton) {
                        IconButton(
                            onClick = {
                                if (isVstFsCandidate && currentUiMode != UiType.X11) {
                                    // Auto-switch into X11 so the fullscreen layout renders
                                    // VstInlineEditor (the sliders panel would be useless full-screen).
                                    currentUiMode = UiType.X11
                                    viewModel.setPreferredUiTypeForPlugin(pluginInfo.fullId, UiType.X11)
                                }
                                val (w, h) = when {
                                    pluginInfo.format == "VST2" || pluginInfo.format == "VST3" -> {
                                        // Use the wine editor's reported size; falls back to 0/0
                                        // if not yet known (orientation request is gated on
                                        // w > h*1.3 so 0/0 just skips the rotate, harmless).
                                        val encoded = runCatching {
                                            com.vibes.dsp.engine.NativeEngine.getInstance()
                                                .nativeGetRackPluginEditorSize(pathId, pluginIndex)
                                        }.getOrDefault(0L)
                                        Pair((encoded ushr 32).toInt(), (encoded and 0xffffffffL).toInt())
                                    }
                                    currentUiMode == UiType.X11 -> Pair(x11PluginNaturalW, x11PluginNaturalH)
                                    else -> Pair(modguiContentWidth, modguiContentHeight)
                                }
                                onOpenFullscreen(pluginIndex, UiType.X11, w, h)
                            },
                            modifier = Modifier.size(44.dp)
                        ) {
                            Icon(
                                Icons.Default.Fullscreen,
                                contentDescription = "Fullscreen",
                                modifier = Modifier.size(20.dp)
                            )
                        }
                    }
                    // Keyboard — VST plugins in X11 mode (wine editor is on-screen)
                    val isVstPluginChrome = pluginInfo.format == "VST2" || pluginInfo.format == "VST3"
                    if (isVstPluginChrome && com.vibes.dsp.BuildConfig.HAS_VST_HOST &&
                        currentUiMode == UiType.X11) {
                        IconButton(
                            onClick = {
                                com.vibes.dsp.ui.vst.VstKeyboardAction.showKeyboard(pathId, pluginIndex)
                            },
                            modifier = Modifier.size(44.dp)
                        ) {
                            Icon(
                                Icons.Default.Keyboard,
                                contentDescription = "Keyboard",
                                modifier = Modifier.size(20.dp)
                            )
                        }
                    }
                    // Replace
                    IconButton(
                        onClick = onReplace,
                        modifier = Modifier.size(44.dp)
                    ) {
                        Icon(
                            Icons.Default.SwapHoriz,
                            contentDescription = "Replace",
                            modifier = Modifier.size(20.dp)
                        )
                    }
                    // Remove
                    IconButton(
                        onClick = onRemove,
                        modifier = Modifier.size(44.dp)
                    ) {
                        Icon(
                            Icons.Default.Close,
                            contentDescription = "Remove",
                            modifier = Modifier.size(20.dp)
                        )
                    }
                }

                val contentModifier = when {
                    isFullscreen -> Modifier.fillMaxSize()
                    expanded -> Modifier
                    else -> Modifier.height(0.dp).alpha(0f)
                }
                Column(modifier = contentModifier) {

                Spacer(modifier = Modifier.height(8.dp))

                // Allocate X11 display number for this plugin — always kept alive while in rack.
                // Display is only released when plugin is removed from rack (onDispose).
                // VST plugins have their OWN X11 server inside vsthost_lib (different display
                // number range) — they don't borrow from :app's X11DisplayManager.
                val isVstPlugin = pluginInfo.format == "VST2" || pluginInfo.format == "VST3"
                var x11DisplayNumber by remember {
                    mutableStateOf(
                        if (pluginInfo.hasX11Ui && !isVstPlugin) X11DisplayManager.allocateDisplay() else -1
                    )
                }

                // Cleanup X11 display when plugin is removed from rack
                DisposableEffect(Unit) {
                    onDispose {
                        if (x11DisplayNumber >= 0) {
                            Log.i("GuitarRackCraft.UI", "Plugin[$pluginIndex]: Releasing X11 display $x11DisplayNumber (removed from rack)")
                            val dispNum = x11DisplayNumber
                            x11DisplayNumber = -1
                            X11Bridge.destroyPluginUI(pathId, plugin.instanceId, uiInstanceId)
                            X11Bridge.detachAndDestroyX11DisplayIfExists(dispNum)
                            X11DisplayManager.releaseDisplay(dispNum)
                            }
                        }
                    }
                }

                // --- X11 UI file picker (ui:requestValue) ---
                val context = LocalContext.current
                val scope = rememberCoroutineScope()
                var x11FileRequestPending by remember { mutableStateOf<Pair<Int, String>?>(null) }
                var showX11FilePicker by remember { mutableStateOf(false) }

                val x11FilePicker = rememberLauncherForActivityResult(
                    contract = ActivityResultContracts.OpenDocument()
                ) { uri ->
                    val req = x11FileRequestPending ?: return@rememberLauncherForActivityResult
                    if (uri != null) {
                        scope.launch(Dispatchers.IO) {
                            val pickerConfig = getX11FilePickerConfig(req.second)
                            val fileName = resolvePickedFileName(
                                context,
                                uri,
                                "model",
                                pickerConfig.extensions + "zip"
                            )
                            val storageDir = pickerConfig.storageDirs.first()
                            val destDir = java.io.File(context.filesDir, storageDir)
                            destDir.mkdirs()
                            val destFile = java.io.File(destDir, fileName)
                            context.contentResolver.openInputStream(uri)?.use { input ->
                                destFile.outputStream().use { output -> input.copyTo(output) }
                            }
                            withContext(Dispatchers.Main) {
                                viewModel.setPluginFilePath(pathId, req.first, req.second, destFile.absolutePath)
                                X11Bridge.deliverFileToPluginUI(pathId, req.first, req.second, destFile.absolutePath)
                            }
                        }
                    }
                    x11FileRequestPending = null
                }

                // Poll for X11 UI file requests when X11 UI is visible
                val x11UiActive = currentUiMode == UiType.X11
                val x11DisplayNumber = -1
                LaunchedEffect(x11UiActive, x11DisplayNumber) {
                    if (x11UiActive && x11DisplayNumber >= 0) {
                        while (isActive) {
                            val req = X11Bridge.pollFileRequest()
                            if (req != null && req.size == 2) {
                                x11FileRequestPending = Pair(req[0].toInt(), req[1])
                                showX11FilePicker = true
                            }
                            delay(100)
                        }
                    }
                }

                // X11 file picker dialog
                if (showX11FilePicker && x11FileRequestPending != null) {
                    val req = x11FileRequestPending!!
                    val pickerConfig = remember(req.second) { getX11FilePickerConfig(req.second) }
                    X11FilePickerDialog(
                        config = pickerConfig,
                        sourcePluginIndex = req.first,
                        sourcePropertyUri = req.second,
                        onFileSelected = { path ->
                            showX11FilePicker = false
                            viewModel.setPluginFilePath(pathId, req.first, req.second, path)
                            X11Bridge.deliverFileToPluginUI(pathId, req.first, req.second, path)
                            x11FileRequestPending = null
                        },
                        onBrowseFiles = {
                            showX11FilePicker = false
                            x11FilePicker.launch(arrayOf("*/*"))
                        },
                        onNavigateToTone3000 = onNavigateToTone3000,
                        onDismiss = {
                            showX11FilePicker = false
                            x11FileRequestPending = null
                        }
                    )
                }

                var vstWineFileRequestPending by remember { mutableStateOf<VstWineFilePickerRequest?>(null) }
                var showVstWineFilePicker by remember { mutableStateOf(false) }
                var vstWinePickerNavigatingToTone3000 by remember { mutableStateOf(false) }
                var vstWinePickerLeftRackForTone3000 by remember { mutableStateOf(false) }

                fun respondVstWineFilePicker(
                    request: VstWineFilePickerRequest,
                    cancelled: Boolean,
                    windowsPath: String = ""
                ) {
                    com.vibes.dsp.engine.NativeEngine.getInstance()
                        .nativeRespondVstFilePicker(
                            pathId,
                            request.pluginIndex,
                            request.sequence,
                            cancelled,
                            windowsPath
                        )
                }

                LaunchedEffect(pluginIndex) {
                    RackManager.vstTone3000FileSelectedEvents.collect { event ->
                        if (event.pathId != pathId || event.pluginIndex != pluginIndex) return@collect
                        val request = vstWineFileRequestPending ?: return@collect
                        if (request.pluginIndex != event.pluginIndex) return@collect

                        showVstWineFilePicker = false
                        val windowsPath = withContext(Dispatchers.IO) {
                            runCatching {
                                copyExistingFileForVstWinePicker(event.filePath, request)
                            }.getOrElse { error ->
                                Log.e("GuitarRackCraft.UI", "VST Tone3000 picker copy failed", error)
                                ""
                            }
                        }

                        respondVstWineFilePicker(
                            request,
                            cancelled = windowsPath.isEmpty(),
                            windowsPath = windowsPath
                        )
                        if (windowsPath.isNotEmpty()) {
                            RackManager.notifyModelLoaded(
                                pathId,
                                request.pluginIndex,
                                java.io.File(event.filePath).nameWithoutExtension
                            )
                        }
                        vstWineFileRequestPending = null
                        vstWinePickerNavigatingToTone3000 = false
                        vstWinePickerLeftRackForTone3000 = false
                    }
                }

                LaunchedEffect(isRackVisible, vstWinePickerNavigatingToTone3000, vstWineFileRequestPending) {
                    if (vstWineFileRequestPending == null) {
                        vstWinePickerNavigatingToTone3000 = false
                        vstWinePickerLeftRackForTone3000 = false
                        return@LaunchedEffect
                    }
                    if (vstWinePickerNavigatingToTone3000 && !isRackVisible) {
                        vstWinePickerLeftRackForTone3000 = true
                    }
                    if (isRackVisible && vstWinePickerNavigatingToTone3000 && vstWinePickerLeftRackForTone3000) {
                        showVstWineFilePicker = true
                        vstWinePickerNavigatingToTone3000 = false
                        vstWinePickerLeftRackForTone3000 = false
                    }
                }

                val vstWineFilePicker = rememberLauncherForActivityResult(
                    contract = ActivityResultContracts.OpenDocument()
                ) { uri ->
                    val request = vstWineFileRequestPending ?: return@rememberLauncherForActivityResult
                    if (uri == null) {
                        respondVstWineFilePicker(request, cancelled = true)
                        vstWineFileRequestPending = null
                        return@rememberLauncherForActivityResult
                    }

                    scope.launch(Dispatchers.IO) {
                        val windowsPath = runCatching {
                            copySafUriForVstWinePicker(context, uri, request)
                        }.getOrElse { error ->
                            Log.e("GuitarRackCraft.UI", "VST picker copy failed", error)
                            ""
                        }
                        withContext(Dispatchers.Main) {
                            respondVstWineFilePicker(
                                request,
                                cancelled = windowsPath.isEmpty(),
                                windowsPath = windowsPath
                            )
                            vstWineFileRequestPending = null
                        }
                    }
                }

                if (showVstWineFilePicker && vstWineFileRequestPending != null) {
                    val request = vstWineFileRequestPending!!
                    GenericFilePickerDialog(
                        title = request.config.title,
                        storageDirs = request.config.storageDirs,
                        extensions = request.config.extensions,
                        builtInItems = request.config.builtInItems,
                        onFileSelected = { path ->
                            showVstWineFilePicker = false
                            scope.launch(Dispatchers.IO) {
                                val windowsPath = runCatching {
                                    copyExistingFileForVstWinePicker(path, request)
                                }.getOrElse { error ->
                                    Log.e("GuitarRackCraft.UI", "VST picker existing-file copy failed", error)
                                    ""
                                }
                                withContext(Dispatchers.Main) {
                                    respondVstWineFilePicker(
                                        request,
                                        cancelled = windowsPath.isEmpty(),
                                        windowsPath = windowsPath
                                    )
                                    vstWineFileRequestPending = null
                                }
                            }
                        },
                        onBrowseFiles = {
                            showVstWineFilePicker = false
                            vstWineFilePicker.launch(mimeTypesForVstWinePicker(request))
                        },
                        onNavigateToTone3000 = {
                            showVstWineFilePicker = false
                            vstWinePickerNavigatingToTone3000 = true
                            vstWinePickerLeftRackForTone3000 = false
                            val initialPlatform = when (request.kind) {
                                VstWinePickerKind.IR -> "ir"
                                VstWinePickerKind.MODEL -> "nam"
                            }
                            val initialGear = if (request.kind == VstWinePickerKind.IR) "ir" else null
                            onNavigateToTone3000(null, initialGear, initialPlatform, request.pluginIndex, null)
                        },
                        dismissBeforeTone3000 = false,
                        onDismiss = {
                            showVstWineFilePicker = false
                            respondVstWineFilePicker(request, cancelled = true)
                            vstWineFileRequestPending = null
                            vstWinePickerNavigatingToTone3000 = false
                            vstWinePickerLeftRackForTone3000 = false
                        }
                    )
                }

                // CRITICAL: Always keep PluginX11UiView in composition to avoid TextureView destruction.
                // When switching away from X11 mode, we hide it instead of removing it.
                // This prevents the Android graphics driver from destroying shared mutexes.
                // VST plugins have their own X11 server (vsthost_lib) and use the separate
                // VstInlineEditor block further down — skip the LV2 viewport entirely.
                var x11UiReady by rememberSaveable { mutableStateOf(false) }
                var x11OnScreen by remember { mutableStateOf(true) }
                if (pluginInfo.format == "LV2") {
                Box(modifier = if (isFullscreen && currentUiMode == UiType.X11) Modifier.fillMaxSize() else Modifier.fillMaxWidth()) {
                    BoxWithConstraints(
                        modifier = if (isFullscreen && currentUiMode == UiType.X11) Modifier.fillMaxSize() else Modifier.fillMaxWidth(),
                        contentAlignment = Alignment.Center
                    ) {
                        val userScale = if (isFullscreen) 1f else if (x11UserScale.isNaN()) 1f else x11UserScale
                        val effectiveScale = if (isFullscreen) 1f else (x11UIScale * userScale).coerceIn(0.3f, 1f)
                        val x11Height = if (isFullscreen && x11PluginNaturalW > 0 && x11PluginNaturalH > 0) {
                            // Aspect-fit to screen
                            val aspect = x11PluginNaturalH.toFloat() / x11PluginNaturalW.toFloat()
                            val fitHeight = maxWidth * aspect
                            if (fitHeight > screenHeight) screenHeight else fitHeight
                        } else if (x11PluginNaturalW > 0 && x11PluginNaturalH > 0) {
                            maxWidth * effectiveScale * (x11PluginNaturalH.toFloat() / x11PluginNaturalW.toFloat())
                        } else {
                            if (isFullscreen) screenHeight else x11ViewportHeight
                        }
                        val x11Visible = currentUiMode == UiType.X11
                        val x11ShowContent = x11Visible && x11UiReady
                        Box(
                            modifier = Modifier
                                .then(if (isFullscreen) Modifier.fillMaxWidth() else Modifier.fillMaxWidth(fraction = effectiveScale))
                                .height(if (x11Visible) x11Height else 0.dp)
                                .alpha(if (x11ShowContent) 1f else 0f)
                                .testTag("x11_plugin_viewport")
                                .clip(RoundedCornerShape(4.dp))
                                .onGloballyPositioned { coords ->
                                    val bounds = coords.boundsInWindow()
                                    x11OnScreen = !bounds.isEmpty
                                }
                        ) {
                            if (x11DisplayNumber >= 0) {
                                PluginX11UiView(
                                    pathId = pathId,
                                    pluginInstanceId = plugin.instanceId,
                                    uiInstanceId = uiInstanceId,
                                    pluginIndex = pluginIndex,
                                    displayNumber = x11DisplayNumber,
                                    isVisible = expanded && currentUiMode == UiType.X11 && (!isAnyPluginFullscreen || isFullscreen),
                                    shouldDestroyOnDispose = true,
                                    modifier = Modifier.fillMaxSize(),
                                    onPluginSizeKnown = { w, h, scale ->
                                        x11PluginNaturalW = w
                                        x11PluginNaturalH = h
                                        x11UIScale = scale
                                        if (x11UserScale.isNaN()) {
                                            x11UserScale = defaultScaleForAspectRatio(w, h)
                                        }
                                        x11UiReady = true
                                    }
                                )
                            }
                        }
                    }
                    // Fullscreen back button — overlays on top of viewport
                    if (isFullscreen && currentUiMode == UiType.X11) {
                        IconButton(
                            onClick = onExitFullscreen,
                            modifier = Modifier
                                .align(Alignment.TopStart)
                                .padding(16.dp)
                                .background(MaterialTheme.colorScheme.scrim.copy(alpha = 0.5f), CircleShape)
                        ) {
                            Icon(
                                Icons.Default.Close,
                                contentDescription = "Exit fullscreen",
                                tint = MaterialTheme.colorScheme.onSurface
                            )
                        }
                    }
                }
                // Resize handle — below X11 viewport, hidden in fullscreen
                if (!isFullscreen && currentUiMode == UiType.X11 && x11UiReady) {
                    ResizeHandle(
                        currentScale = if (x11UserScale.isNaN()) 1f else x11UserScale,
                        onScaleChange = { x11UserScale = it },
                        modifier = Modifier.align(Alignment.CenterHorizontally)
                    )
                }
                }  // end if (pluginInfo.format == "LV2")

                // VST X11 (wine editor) — uses vsthost_lib's separate X11 server.
                // Composed only when X11 mode is active. PluginSurface uses
                // destroyOnDispose=false so the wine subprocess + X11 server
                // stay alive on mode switch; only the SurfaceView is recreated.
                // attachSurface drains any prior render thread on re-attach.
                val isVstPlugin = pluginInfo.format == "VST2" || pluginInfo.format == "VST3"
                if (isVstPlugin && com.vibes.dsp.BuildConfig.HAS_VST_HOST &&
                    currentUiMode == UiType.X11) {
                    var vstUiReady by remember { mutableStateOf(false) }
                    Box(
                        modifier = if (isFullscreen) Modifier.fillMaxSize() else Modifier.fillMaxWidth(),
                        contentAlignment = Alignment.Center
                    ) {
                        // Mirror the LV2 X11 scale machinery: auto-scale tall/stompbox editors
                        // down (defaultScaleForAspectRatio) and honor the user scale slider /
                        // resize bar. Fullscreen ignores the scale (it fits the screen instead).
                        val userScale = if (isFullscreen) 1f else if (x11UserScale.isNaN()) 1f else x11UserScale
                        val effectiveScale = if (isFullscreen) 1f else userScale.coerceIn(0.3f, 1f)
                        Box(modifier = if (isFullscreen) Modifier.fillMaxSize() else Modifier.fillMaxWidth(fraction = effectiveScale)) {
                            com.vibes.dsp.ui.vst.VstInlineEditor(
                                pathId,
                                pluginIndex,
                                isFullscreen = isFullscreen,
                                onPluginSizeKnown = { w, h ->
                                    x11PluginNaturalW = w
                                    x11PluginNaturalH = h
                                    if (x11UserScale.isNaN()) {
                                        x11UserScale = defaultScaleForAspectRatio(w, h)
                                    }
                                    vstUiReady = true
                                },
                                onFilePickerRequested = { sequence, title, filterPatterns, initialDir, copyDirLinux, copyDirWindows ->
                                    val existing = vstWineFileRequestPending
                                    if (existing != null) {
                                        true
                                    } else {
                                        val classified = getVstWineFilePickerConfig(title, filterPatterns)
                                        if (classified == null) {
                                            false
                                        } else {
                                            val (kind, config) = classified
                                            vstWineFileRequestPending = VstWineFilePickerRequest(
                                                pluginIndex = pluginIndex,
                                                sequence = sequence,
                                                title = title,
                                                filterPatterns = filterPatterns,
                                                initialDir = initialDir,
                                                copyDirLinux = copyDirLinux,
                                                copyDirWindows = copyDirWindows,
                                                kind = kind,
                                                config = config
                                            )
                                            showVstWineFilePicker = true
                                            true
                                        }
                                    }
                                }
                            )
                        }
                        if (isFullscreen) {
                            Column(
                                modifier = Modifier
                                    .align(Alignment.TopStart)
                                    .padding(16.dp),
                                verticalArrangement = Arrangement.spacedBy(8.dp)
                            ) {
                                IconButton(
                                    onClick = onExitFullscreen,
                                    modifier = Modifier.background(MaterialTheme.colorScheme.scrim.copy(alpha = 0.5f), CircleShape)
                                ) {
                                    Icon(
                                        Icons.Default.Close,
                                        contentDescription = "Exit fullscreen",
                                        tint = MaterialTheme.colorScheme.onSurface
                                    )
                                }
                                // Soft-keyboard toggle — mirrors the exe-installer fullscreen
                                // control; raises the IME against the wine editor surface for
                                // plugins with text fields (license keys, in-editor search).
                                IconButton(
                                    onClick = { com.vibes.dsp.ui.vst.VstKeyboardAction.showKeyboard(pathId, pluginIndex) },
                                    modifier = Modifier.background(MaterialTheme.colorScheme.scrim.copy(alpha = 0.5f), CircleShape)
                                ) {
                                    Icon(
                                        Icons.Default.Keyboard,
                                        contentDescription = "Toggle keyboard",
                                        tint = MaterialTheme.colorScheme.onSurface
                                    )
                                }
                            }
                        }
                    }
                    // Resize handle — the scale "bar" below the VST editor, hidden in fullscreen.
                    if (!isFullscreen && vstUiReady) {
                        ResizeHandle(
                            currentScale = if (x11UserScale.isNaN()) 1f else x11UserScale,
                            onScaleChange = { x11UserScale = it },
                            modifier = Modifier.align(Alignment.CenterHorizontally)
                        )
                    }
                }

                // Model picker shared state — declared before modgui block so it can be referenced there
                val modelConfig = remember(pluginInfo.id) { getModelPluginConfig(pluginInfo.id) }
                var modelFilePickerTrigger by remember { mutableStateOf(0) }
                var modelActiveModelName by rememberSaveable { mutableStateOf<String?>(null) }
                var modelFiles by remember { mutableStateOf<List<java.io.File>>(emptyList()) }

                // Observe model loaded events from external sources (e.g. Tone3000 download)
                LaunchedEffect(pluginIndex) {
                    RackManager.modelLoadedEvents.collect { event ->
                        if (event.pluginIndex == pluginIndex) {
                            modelActiveModelName = event.modelName
                        }
                    }
                }

                // Keep modgui always in composition (like X11) so it doesn't re-render on each switch.
                if (pluginInfo.hasModgui) {
                    var modguiReady by rememberSaveable { mutableStateOf(false) }
                    var modguiOnScreen by remember { mutableStateOf(true) }
                    @Suppress("NAME_SHADOWING") val density = LocalDensity.current
                    val modguiModeActive = currentUiMode == UiType.MODGUI
                    val modguiVisible = modguiModeActive && modguiReady
                    val modguiFullscreen = isFullscreen && modguiModeActive
                    Box(modifier = if (modguiFullscreen) Modifier.fillMaxSize() else Modifier.fillMaxWidth()) {
                        BoxWithConstraints(
                            modifier = Modifier
                                .then(if (modguiFullscreen) Modifier.fillMaxSize() else Modifier.fillMaxWidth())
                                .then(if (!modguiModeActive) Modifier.height(0.dp) else Modifier)
                                .alpha(if (modguiVisible) 1f else 0f)
                                .onGloballyPositioned { coords ->
                                    val bounds = coords.boundsInWindow()
                                    modguiOnScreen = !bounds.isEmpty
                                },
                            contentAlignment = Alignment.Center
                        ) {
                            val effectiveScale = if (modguiFullscreen) 1f else (if (modguiUserScale.isNaN()) 1f else modguiUserScale).coerceIn(0.3f, 1f)
                            val heightDp = if (modguiFullscreen && modguiContentWidth > 0 && modguiContentHeight > 0) {
                                // Aspect-fit to screen
                                val aspect = modguiContentHeight.toFloat() / modguiContentWidth.toFloat()
                                val fitHeight = maxWidth * aspect
                                if (fitHeight > screenHeight) screenHeight else fitHeight
                            } else if (modguiContentWidth > 0 && modguiContentHeight > 0) {
                                val availableWidthPx = with(density) { maxWidth.toPx() } * effectiveScale
                                val scale = availableWidthPx / modguiContentWidth.toFloat()
                                with(density) { (modguiContentHeight * scale).toDp() }
                            } else {
                                if (modguiFullscreen) screenHeight else 200.dp
                            }
                            InlineModguiView(
                                pathId = pathId,
                                pluginIndex = pluginIndex,
                                pluginInfo = pluginInfo,
                                isVisible = expanded && modguiOnScreen && (!isAnyPluginFullscreen || isFullscreen),
                                modifier = Modifier
                                    .then(if (modguiFullscreen) Modifier.fillMaxWidth() else Modifier.fillMaxWidth(fraction = effectiveScale))
                                    .height(heightDp),
                                onContentSize = { w, h ->
                                    modguiContentWidth = w
                                    modguiContentHeight = h
                                    if (modguiUserScale.isNaN()) {
                                        modguiUserScale = defaultScaleForAspectRatio(w, h)
                                    }
                                    modguiReady = true
                                },
                                onFilePickerRequested = { modelFilePickerTrigger++ },
                                modelDisplayName = modelActiveModelName,
                                modelFiles = modelFiles,
                                onModelSelected = { path ->
                                    modelActiveModelName = java.io.File(path).nameWithoutExtension
                                    modelConfig?.let { cfg ->
                                        viewModel.setPluginFilePath(selectedPathId, pluginIndex, cfg.propertyUri, path)
                                    }
                                }
                            )
                        }
                        // Fullscreen back button — overlays on top of modgui viewport
                        if (modguiFullscreen) {
                            IconButton(
                                onClick = onExitFullscreen,
                                modifier = Modifier
                                    .align(Alignment.TopStart)
                                    .padding(16.dp)
                                    .background(MaterialTheme.colorScheme.scrim.copy(alpha = 0.5f), CircleShape)
                            ) {
                                Icon(
                                    Icons.Default.Close,
                                    contentDescription = "Exit fullscreen",
                                    tint = MaterialTheme.colorScheme.onSurface
                                )
                            }
                        }
                    }
                    // Resize handle — below Modgui viewport, hidden in fullscreen
                    if (!modguiFullscreen && modguiModeActive && modguiReady) {
                        ResizeHandle(
                            currentScale = if (modguiUserScale.isNaN()) 1f else modguiUserScale,
                            onScaleChange = { modguiUserScale = it },
                            modifier = Modifier.align(Alignment.CenterHorizontally)
                        )
                    }
                }

                // ModelPicker: headless, kept in composition for the file picker launcher
                if (modelConfig != null) {
                    val initialPlatform = when {
                        pluginInfo.id.contains("neural-amp-modeler") || pluginInfo.id.contains("neuralrack", ignoreCase = true) -> "nam"
                        pluginInfo.id.contains("aidadsp") -> "aida-x"
                        else -> null
                    }
                    val initialTag = if (initialPlatform == "aida-x") "aida-x" else null
                    ModelPicker(
                        pluginIndex = pluginIndex,
                        pathId = pathId,
                        viewModel = viewModel,
                        config = modelConfig,
                        externalPickerTrigger = modelFilePickerTrigger,
                        onActiveModelChanged = { modelActiveModelName = it },
                        onModelFilesChanged = { modelFiles = it },
                        onNavigateToTone3000 = {
                            val sourceSlot = modelConfig.propertyUri.substringAfterLast('#', "").ifEmpty { null }
                            onNavigateToTone3000(initialTag, null, initialPlatform, pluginIndex, sourceSlot)
                        }
                    )
                }

                if (!isFullscreen) when (currentUiMode) {
                    UiType.X11 -> {
                        // X11 view is always present above, just show it
                        LaunchedEffect(pluginIndex, pluginInfo) {
                            Log.i("GuitarRackCraft.UI", "Plugin[$pluginIndex] ${pluginInfo.name}: showing X11 UI (portrait)")
                        }
                    }
                    UiType.MODGUI -> {
                        // Modgui is always present above, nothing extra needed
                    }
                    UiType.SLIDERS -> {
                      Column(modifier = Modifier.padding(horizontal = 12.dp)) {
                        if (modelConfig != null) {
                            var modelDropdownExpanded by remember { mutableStateOf(false) }
                            ExposedDropdownMenuBox(
                                expanded = modelDropdownExpanded,
                                onExpandedChange = {
                                    if (modelFiles.size > 1) {
                                        modelDropdownExpanded = it
                                    } else {
                                        modelFilePickerTrigger++
                                    }
                                }
                            ) {
                                OutlinedTextField(
                                    value = modelActiveModelName ?: modelConfig.placeholder,
                                    onValueChange = {},
                                    readOnly = true,
                                    modifier = Modifier
                                        .fillMaxWidth()
                                        .menuAnchor(),
                                    trailingIcon = { ExposedDropdownMenuDefaults.TrailingIcon(expanded = modelDropdownExpanded) }
                                )
                                ExposedDropdownMenu(
                                    expanded = modelDropdownExpanded,
                                    onDismissRequest = { modelDropdownExpanded = false }
                                ) {
                                    DropdownMenuItem(
                                        text = { Text("\u2026", fontWeight = androidx.compose.ui.text.font.FontWeight.Bold) },
                                        onClick = {
                                            modelDropdownExpanded = false
                                            modelFilePickerTrigger++
                                        }
                                    )
                                    modelFiles.forEach { modelFile ->
                                        DropdownMenuItem(
                                            text = { Text(modelFile.nameWithoutExtension) },
                                            onClick = {
                                                modelDropdownExpanded = false
                                                modelActiveModelName = modelFile.nameWithoutExtension
                                                viewModel.setPluginFilePath(selectedPathId, pluginIndex, modelConfig.propertyUri, modelFile.absolutePath)
                                            }
                                        )
                                    }
                                }
                            }
                            Spacer(modifier = Modifier.height(8.dp))
                        }
                        val controlPorts = pluginInfo.controlPorts
                        val isVst = pluginInfo.format == "VST2" || pluginInfo.format == "VST3"
                        if (controlPorts.isNotEmpty()) {
                            Divider()
                            Spacer(modifier = Modifier.height(8.dp))
                            Column(
                                verticalArrangement = Arrangement.spacedBy(12.dp)
                            ) {
                                controlPorts.forEach { port ->
                                    ParameterControl(
                                        port = port,
                                        pluginIndex = pluginIndex,
                                        pathId = pathId,
                                        viewModel = viewModel
                                    )
                                }
                            }
                        } else if (modelConfig == null && !isVst) {
                            Text(
                                text = "No control parameters available",
                                style = MaterialTheme.typography.bodySmall,
                                color = MaterialTheme.colorScheme.onSurfaceVariant
                            )
                        }
                      }
                    }
                }
                } // end contentModifier Column
            }
        }
    }

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun ParameterControl(
    port: com.vibes.dsp.engine.PortInfo,
    pluginIndex: Int,
    pathId: Long,
    viewModel: RackViewModel
) {
    val selectedPathId = pathId
    val currentValue = remember(pathId, pluginIndex, port.index) {
        mutableFloatStateOf(port.defaultValue)
    }
    var isUserInteracting by remember { mutableStateOf(false) }

    // Poll native parameter value periodically so that changes made via the X11 UI
    // (or other external sources) are reflected in the Android slider controls.
    LaunchedEffect(selectedPathId, pluginIndex, port.index) {
        while (true) {
            delay(200)
            if (!isUserInteracting) {
                val nativeValue = viewModel.getParameter(selectedPathId, pluginIndex, port.index)
                if (kotlin.math.abs(nativeValue - currentValue.value) > 1e-5f) {
                    currentValue.value = nativeValue
                }
            }
        }
    }

    Column {
        Row(
            modifier = Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.SpaceBetween,
            verticalAlignment = Alignment.CenterVertically
        ) {
            Text(
                text = port.name.ifEmpty { port.symbol },
                style = MaterialTheme.typography.bodyMedium
            )
            Text(
                text = if (port.scalePoints.isNotEmpty()) {
                    port.scalePoints.find { kotlin.math.abs(it.value - currentValue.value) < 1e-6f }
                        ?.label ?: "%.2f".format(currentValue.value)
                } else {
                    "%.2f".format(currentValue.value)
                },
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant
            )
        }

        if (port.isToggle) {
            // Toggle port: switch
            Switch(
                checked = currentValue.value > 0.5f,
                onCheckedChange = { checked ->
                    val newValue = if (checked) port.maxValue else port.minValue
                    currentValue.value = newValue
                    viewModel.setParameter(selectedPathId, pluginIndex, port.index, newValue)
                }
            )
        } else if (port.scalePoints.isNotEmpty()) {
            // Enumeration port: dropdown
            var expanded by remember { mutableStateOf(false) }
            ExposedDropdownMenuBox(
                expanded = expanded,
                onExpandedChange = { expanded = it }
            ) {
                OutlinedTextField(
                    value = port.scalePoints.find { kotlin.math.abs(it.value - currentValue.value) < 1e-6f }
                        ?.label ?: "%.2f".format(currentValue.value),
                    onValueChange = {},
                    readOnly = true,
                    modifier = Modifier
                        .fillMaxWidth()
                        .menuAnchor(),
                    trailingIcon = { ExposedDropdownMenuDefaults.TrailingIcon(expanded = expanded) }
                )
                ExposedDropdownMenu(
                    expanded = expanded,
                    onDismissRequest = { expanded = false }
                ) {
                    port.scalePoints.forEach { sp ->
                        DropdownMenuItem(
                            text = { Text(sp.label) },
                            onClick = {
                                currentValue.value = sp.value
                                viewModel.setParameter(selectedPathId, pluginIndex, port.index, sp.value)
                                expanded = false
                            }
                        )
                    }
                }
            }
        } else {
            // Continuous port: slider
            Slider(
                value = currentValue.value,
                onValueChange = { newValue ->
                    isUserInteracting = true
                    currentValue.value = newValue
                    viewModel.setParameter(selectedPathId, pluginIndex, port.index, newValue)
                },
                onValueChangeFinished = {
                    isUserInteracting = false
                },
                valueRange = port.minValue..port.maxValue,
                modifier = Modifier.fillMaxWidth()
            )
        }
    }
}

private data class ModelPluginConfig(
    val propertyUri: String,
    val extensions: Set<String>,
    val storageDirs: List<String>,
    val placeholder: String,
    val builtInItems: List<Pair<String, String>> = emptyList()
)

private fun getModelPluginConfig(pluginId: String): ModelPluginConfig? = when {
    pluginId.contains("neural-amp-modeler") || pluginId.contains("neuralrack", ignoreCase = true) -> ModelPluginConfig(
        propertyUri = if (pluginId.contains("neural-amp-modeler")) "http://github.com/mikeoliphant/neural-amp-modeler-lv2#model" else "urn:brummer:neuralrack#Neural_Model",
        extensions = setOf("nam", "nammodel", "json"),
        storageDirs = listOf("neural_models", "aidax_models"),
        placeholder = "No model loaded",
        builtInItems = if (pluginId.contains("neural-amp-modeler")) listOf("Default: NAM Profile" to "default") else emptyList()
    )
    pluginId.contains("aidadsp") -> ModelPluginConfig(
        propertyUri = "http://aidadsp.cc/plugins/aidadsp-bundle/rt-neural-generic#json",
        extensions = setOf("json", "aidax", "aidadspmodel"),
        storageDirs = listOf("aidax_models"),
        placeholder = "No model loaded",
        builtInItems = listOf("Default: California Clean" to "default")
    )
    pluginId.contains("ImpulseLoader") -> ModelPluginConfig(
        propertyUri = "urn:brummer:ImpulseLoader#irfile",
        extensions = setOf("wav"),
        storageDirs = listOf("ir_models"),
        placeholder = "No IR loaded"
    )
    else -> null
}

private data class X11FilePickerConfig(
    val title: String,
    val storageDirs: List<String>,
    val extensions: Set<String>,
    val builtInItems: List<Pair<String, String>> // displayName to deliveryValue
)

private enum class VstWinePickerKind {
    MODEL,
    IR
}

private data class VstWineFilePickerRequest(
    val pluginIndex: Int,
    val sequence: Int,
    val title: String,
    val filterPatterns: String,
    val initialDir: String,
    val copyDirLinux: String,
    val copyDirWindows: String,
    val kind: VstWinePickerKind,
    val config: X11FilePickerConfig
)

private fun getX11FilePickerConfig(propertyUri: String): X11FilePickerConfig = when {
    propertyUri.endsWith("#json") || propertyUri.contains("rt-neural-generic#json") -> X11FilePickerConfig(
        title = "Select AIDA-X Model",
        storageDirs = listOf("aidax_models"),
        extensions = setOf("json", "aidax", "aidadspmodel"),
        builtInItems = listOf("Default: California Clean" to "default")
    )
    propertyUri.endsWith("#cabinet") || propertyUri.contains("irfile") -> X11FilePickerConfig(
        title = "Select Impulse Response",
        storageDirs = listOf("ir_models"),
        extensions = setOf("wav"),
        builtInItems = if (propertyUri.endsWith("#cabinet")) listOf("Default: V30 Audix i5" to "default") else emptyList()
    )
    propertyUri.contains("neural-amp-modeler") || propertyUri.contains("neuralrack#Neural_Model") -> X11FilePickerConfig(
        title = "Select Neural Model",
        storageDirs = listOf("neural_models", "aidax_models"),
        extensions = setOf("nam", "nammodel", "json"),
        builtInItems = if (propertyUri.contains("neural-amp-modeler")) listOf("Default: NAM Profile" to "default") else emptyList()
    )
    else -> X11FilePickerConfig(
        title = "Select File",
        storageDirs = listOf("aidax_models"),
        extensions = emptySet(),
        builtInItems = emptyList()
    )
}

private fun getVstWineFilePickerConfig(
    title: String,
    filterPatterns: String
): Pair<VstWinePickerKind, X11FilePickerConfig>? {
    val haystack = "$title\n$filterPatterns".lowercase()
    val isIr = haystack.contains(".wav") ||
        haystack.contains("*.wav") ||
        haystack.contains("impulse") ||
        Regex("""\bir\b""").containsMatchIn(haystack)
    val isModel = haystack.contains(".nam") ||
        haystack.contains("*.nam") ||
        haystack.contains("nammodel") ||
        haystack.contains("profile") ||
        haystack.contains("model")

    return when {
        isIr -> VstWinePickerKind.IR to X11FilePickerConfig(
            title = "Select Impulse Response",
            storageDirs = listOf("ir_models"),
            extensions = setOf("wav"),
            builtInItems = emptyList()
        )
        isModel -> VstWinePickerKind.MODEL to X11FilePickerConfig(
            title = "Select Neural Model",
            storageDirs = listOf("neural_models", "aidax_models"),
            extensions = setOf("nam", "nammodel", "json"),
            builtInItems = emptyList()
        )
        else -> null
    }
}

private fun mimeTypesForVstWinePicker(request: VstWineFilePickerRequest): Array<String> =
    when (request.kind) {
        VstWinePickerKind.IR -> arrayOf("audio/*", "*/*")
        VstWinePickerKind.MODEL -> arrayOf("*/*")
    }

@Composable
private fun GenericFilePickerDialog(
    title: String,
    storageDirs: List<String>,
    extensions: Set<String>,
    builtInItems: List<Pair<String, String>>,
    onFileSelected: (String) -> Unit,
    onBrowseFiles: () -> Unit,
    onNavigateToTone3000: () -> Unit,
    showTone3000: Boolean = true,
    dismissBeforeTone3000: Boolean = true,
    onDismiss: () -> Unit
) {
    val context = LocalContext.current
    var existingFiles by remember { mutableStateOf<List<java.io.File>>(emptyList()) }
    var favorites by remember { mutableStateOf(com.vibes.dsp.engine.ModelFavoritesManager.getFavorites(context)) }
    var expandedGroups by remember { mutableStateOf(setOf<String>()) }

    fun refreshFiles() {
        val allFiles = mutableListOf<java.io.File>()
        storageDirs.forEach { dirName ->
            val dir = java.io.File(context.filesDir, dirName)
            if (dir.exists()) {
                allFiles.addAll(
                    dir.walk()
                        .filter { it.isFile }
                        .filter { extensions.isEmpty() || it.extension.lowercase() in extensions }
                        .toList()
                )
            }
        }
        existingFiles = allFiles.sortedBy { it.name.lowercase() }
    }

    LaunchedEffect(storageDirs) {
        withContext(Dispatchers.IO) {
            refreshFiles()
        }
    }

    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text(title) },
        text = {
            LazyColumn(modifier = Modifier.fillMaxWidth().heightIn(max = 400.dp)) {
                item {
                    Column {
                        Row(
                            modifier = Modifier
                                .fillMaxWidth()
                                .clickable { onBrowseFiles() }
                                .padding(vertical = 12.dp),
                            verticalAlignment = Alignment.CenterVertically,
                            horizontalArrangement = Arrangement.spacedBy(12.dp)
                        ) {
                            Icon(Icons.Default.Folder, contentDescription = null)
                            Text("Browse files\u2026", fontWeight = FontWeight.Bold)
                        }
                        if (showTone3000) {
                            Row(
                                modifier = Modifier
                                    .fillMaxWidth()
                                    .clickable {
                                        if (dismissBeforeTone3000) {
                                            onDismiss()
                                        }
                                        onNavigateToTone3000()
                                    }
                                    .padding(vertical = 12.dp),
                                verticalAlignment = Alignment.CenterVertically,
                                horizontalArrangement = Arrangement.spacedBy(12.dp)
                            ) {
                                Icon(Icons.Default.Cloud, contentDescription = null, tint = MaterialTheme.colorScheme.primary)
                                Text("Browse TONE3000", fontWeight = FontWeight.Bold, color = MaterialTheme.colorScheme.primary)
                            }
                        }
                    }
                }

                val favoriteFiles = existingFiles.filter { favorites.contains(it.absolutePath) }
                val nonFavoriteFiles = existingFiles.filter { !favorites.contains(it.absolutePath) }

                if (favoriteFiles.isNotEmpty()) {
                    item { Divider() }
                    item {
                        Text(
                            "Favorites",
                            style = MaterialTheme.typography.labelSmall,
                            color = MaterialTheme.colorScheme.primary,
                            modifier = Modifier.padding(top = 8.dp, bottom = 4.dp)
                        )
                    }
                    items(favoriteFiles) { file ->
                        FileItemRow(
                            name = file.nameWithoutExtension,
                            path = file.absolutePath,
                            isFavorite = true,
                            onSelect = { onFileSelected(it) },
                            onToggleFavorite = {
                                com.vibes.dsp.engine.ModelFavoritesManager.toggleFavorite(context, it)
                                favorites = com.vibes.dsp.engine.ModelFavoritesManager.getFavorites(context)
                            },
                            onDelete = {
                                java.io.File(it).delete()
                                refreshFiles()
                            }
                        )
                    }
                }

                if (builtInItems.isNotEmpty() || nonFavoriteFiles.isNotEmpty()) {
                    item { Divider() }
                }

                if (builtInItems.isNotEmpty()) {
                    item {
                        Text(
                            "Built-in",
                            style = MaterialTheme.typography.labelSmall,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                            modifier = Modifier.padding(top = 8.dp, bottom = 4.dp)
                        )
                    }
                    items(builtInItems) { (name, value) ->
                        FileItemRow(
                            name = name,
                            path = value,
                            isFavorite = favorites.contains(value),
                            canDelete = false,
                            onSelect = { onFileSelected(it) },
                            onToggleFavorite = {
                                com.vibes.dsp.engine.ModelFavoritesManager.toggleFavorite(context, it)
                                favorites = com.vibes.dsp.engine.ModelFavoritesManager.getFavorites(context)
                            }
                        )
                    }
                }

                if (nonFavoriteFiles.isNotEmpty()) {
                    item {
                        Text(
                            "On device",
                            style = MaterialTheme.typography.labelSmall,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                            modifier = Modifier.padding(top = 8.dp, bottom = 4.dp)
                        )
                    }

                    val groups = nonFavoriteFiles.groupBy { file ->
                        val parent = file.parentFile ?: return@groupBy null
                        val isAtRoot = storageDirs.any { dirName -> 
                            parent.absolutePath == java.io.File(context.filesDir, dirName).absolutePath 
                        }
                        if (isAtRoot) null else parent.name
                    }

                    groups.forEach { (groupName, files) ->
                        if (groupName == null) {
                            items(files) { file ->
                                FileItemRow(
                                    name = file.nameWithoutExtension,
                                    path = file.absolutePath,
                                    isFavorite = false,
                                    onSelect = { onFileSelected(it) },
                                    onToggleFavorite = {
                                        com.vibes.dsp.engine.ModelFavoritesManager.toggleFavorite(context, it)
                                        favorites = com.vibes.dsp.engine.ModelFavoritesManager.getFavorites(context)
                                    },
                                    onDelete = {
                                        java.io.File(it).delete()
                                        refreshFiles()
                                    }
                                )
                            }
                        } else {
                            val isExpanded = expandedGroups.contains(groupName)
                            item(key = groupName) {
                                GroupRow(
                                    name = groupName.replace("_", " "),
                                    isExpanded = isExpanded,
                                    onClick = {
                                        expandedGroups = if (isExpanded) expandedGroups - groupName else expandedGroups + groupName
                                    }
                                )
                            }
                            if (isExpanded) {
                                items(files) { file ->
                                    FileItemRow(
                                        name = file.nameWithoutExtension,
                                        path = file.absolutePath,
                                        isFavorite = false,
                                        onSelect = { onFileSelected(it) },
                                        onToggleFavorite = {
                                            com.vibes.dsp.engine.ModelFavoritesManager.toggleFavorite(context, it)
                                            favorites = com.vibes.dsp.engine.ModelFavoritesManager.getFavorites(context)
                                        },
                                        onDelete = {
                                            java.io.File(it).delete()
                                            refreshFiles()
                                        },
                                        modifier = Modifier.padding(start = 24.dp)
                                    )
                                }
                            }
                        }
                    }
                }
            }
        },
        confirmButton = {},
        dismissButton = {
            TextButton(onClick = onDismiss) { Text("Cancel") }
        }
    )
}

@Composable
private fun GroupRow(
    name: String,
    isExpanded: Boolean,
    onClick: () -> Unit
) {
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .clickable { onClick() }
            .padding(vertical = 4.dp),
        verticalAlignment = Alignment.CenterVertically
    ) {
        Icon(
            Icons.Default.Folder,
            contentDescription = null,
            modifier = Modifier.size(24.dp).padding(4.dp),
            tint = MaterialTheme.colorScheme.primary.copy(alpha = 0.7f)
        )
        Text(
            text = name,
            modifier = Modifier.weight(1f).padding(horizontal = 8.dp),
            style = MaterialTheme.typography.bodyMedium,
            fontWeight = FontWeight.Bold
        )
        Icon(
            if (isExpanded) Icons.Default.ExpandLess else Icons.Default.ExpandMore,
            contentDescription = null,
            modifier = Modifier.size(24.dp).padding(4.dp),
            tint = MaterialTheme.colorScheme.onSurfaceVariant
        )
    }
}

@Composable
private fun FileItemRow(
    name: String,
    path: String,
    isFavorite: Boolean,
    canDelete: Boolean = true,
    onSelect: (String) -> Unit,
    onToggleFavorite: (String) -> Unit,
    onDelete: ((String) -> Unit)? = null,
    modifier: Modifier = Modifier
) {
    Row(
        modifier = modifier
            .fillMaxWidth()
            .clickable { onSelect(path) }
            .padding(vertical = 4.dp),
        verticalAlignment = Alignment.CenterVertically
    ) {
        IconButton(
            onClick = { onToggleFavorite(path) },
            modifier = Modifier.size(32.dp)
        ) {
            Icon(
                if (isFavorite) Icons.Default.Favorite else Icons.Default.FavoriteBorder,
                contentDescription = "Favorite",
                tint = if (isFavorite) MaterialTheme.colorScheme.primary else MaterialTheme.colorScheme.onSurfaceVariant,
                modifier = Modifier.size(18.dp)
            )
        }
        Text(
            text = name,
            modifier = Modifier.weight(1f).padding(horizontal = 8.dp),
            style = MaterialTheme.typography.bodyMedium
        )
        if (canDelete && onDelete != null) {
            IconButton(
                onClick = { onDelete(path) },
                modifier = Modifier.size(32.dp)
            ) {
                Icon(
                    Icons.Default.Delete,
                    contentDescription = "Delete",
                    tint = MaterialTheme.colorScheme.error.copy(alpha = 0.7f),
                    modifier = Modifier.size(18.dp)
                )
            }
        }
    }
}

@Composable
private fun X11FilePickerDialog(
    config: X11FilePickerConfig,
    sourcePluginIndex: Int,
    sourcePropertyUri: String,
    onFileSelected: (String) -> Unit,
    onBrowseFiles: () -> Unit,
    onNavigateToTone3000: (String?, String?, String?, Int, String?) -> Unit,
    onDismiss: () -> Unit
) {
    GenericFilePickerDialog(
        title = config.title,
        storageDirs = config.storageDirs,
        extensions = config.extensions,
        builtInItems = config.builtInItems,
        onFileSelected = onFileSelected,
        onBrowseFiles = onBrowseFiles,
        onNavigateToTone3000 = {
            val initialPlatform = when {
                config.title.contains("Neural Model") -> "nam"
                config.title.contains("AIDA-X") -> "aida-x"
                config.title.contains("Impulse Response") -> "ir"
                else -> null
            }
            val initialTag = if (initialPlatform == "aida-x") "aida-x" else null
            val initialGear = if (config.title.contains("Impulse Response")) "ir" else null
            val sourceSlot = sourcePropertyUri.substringAfterLast('#', "").ifEmpty { null }
            onNavigateToTone3000(initialTag, initialGear, initialPlatform, sourcePluginIndex, sourceSlot)
        },
        onDismiss = onDismiss
    )
}

private fun extractModelsFromZip(
    context: android.content.Context,
    uri: android.net.Uri,
    zipFileName: String,
    config: ModelPluginConfig
): List<java.io.File> {
    val baseName = zipFileName.substringBeforeLast('.')
    val storageDir = config.storageDirs.first()
    val destDir = java.io.File(context.filesDir, "$storageDir/$baseName")
    destDir.mkdirs()

    val extractedFiles = mutableListOf<java.io.File>()
    context.contentResolver.openInputStream(uri)?.use { inputStream ->
        java.util.zip.ZipInputStream(inputStream).use { zis ->
            var entry = zis.nextEntry
            while (entry != null) {
                if (!entry.isDirectory) {
                    val entryName = entry.name.substringAfterLast('/')
                    val ext = entryName.substringAfterLast('.', "").lowercase()
                    if (ext in config.extensions) {
                        val outFile = java.io.File(destDir, entryName)
                        outFile.outputStream().use { out -> zis.copyTo(out) }
                        extractedFiles.add(outFile)
                    }
                }
                zis.closeEntry()
                entry = zis.nextEntry
            }
        }
    }
    return extractedFiles.sortedBy { it.name.lowercase() }
}

private fun resolvePickedFileName(
    context: android.content.Context,
    uri: android.net.Uri,
    fallbackName: String,
    allowedExtensions: Set<String>
): String {
    val displayName = context.contentResolver.query(
        uri,
        arrayOf(OpenableColumns.DISPLAY_NAME),
        null,
        null,
        null
    )?.use { cursor ->
        val nameIndex = cursor.getColumnIndex(OpenableColumns.DISPLAY_NAME)
        if (nameIndex >= 0 && cursor.moveToFirst()) cursor.getString(nameIndex) else null
    }

    val rawName = displayName
        ?: uri.lastPathSegment?.substringAfterLast('/')
        ?: fallbackName

    val sanitized = rawName
        .substringAfterLast('/')
        .replace(Regex("[\\\\/:*?\"<>|]"), "_")
        .trim()
        .ifEmpty { fallbackName }

    val extension = sanitized.substringAfterLast('.', "").lowercase()
    if (extension.isNotEmpty() || allowedExtensions.isEmpty()) {
        return sanitized
    }

    val fallbackExtension = allowedExtensions.firstOrNull { it != "zip" } ?: return sanitized
    return "$sanitized.$fallbackExtension"
}

private fun copyExistingFileForVstWinePicker(
    selectedPath: String,
    request: VstWineFilePickerRequest
): String {
    val source = java.io.File(selectedPath)
    if (!source.isFile) throw IOException("Selected file does not exist: $selectedPath")
    return copyLocalFileIntoVstWinePicker(source, request)
}

private fun copySafUriForVstWinePicker(
    context: android.content.Context,
    uri: android.net.Uri,
    request: VstWineFilePickerRequest
): String {
    val fileName = resolvePickedFileName(
        context,
        uri,
        if (request.kind == VstWinePickerKind.IR) "ir" else "model",
        request.config.extensions + if (request.kind == VstWinePickerKind.MODEL) setOf("zip") else emptySet()
    )

    val storageDir = request.config.storageDirs.first()
    val storedFile = if (fileName.lowercase().endsWith(".zip") && request.kind == VstWinePickerKind.MODEL) {
        val models = extractModelsFromZip(
            context,
            uri,
            fileName,
            ModelPluginConfig(
                propertyUri = "",
                extensions = request.config.extensions,
                storageDirs = request.config.storageDirs,
                placeholder = ""
            )
        )
        models.firstOrNull() ?: throw IOException("No supported model files found in $fileName")
    } else {
        val destDir = java.io.File(context.filesDir, storageDir)
        destDir.mkdirs()
        val destFile = java.io.File(destDir, fileName)
        context.contentResolver.openInputStream(uri)?.use { input ->
            destFile.outputStream().use { output -> input.copyTo(output) }
        } ?: throw IOException("Could not open picked URI")
        destFile
    }

    return copyLocalFileIntoVstWinePicker(storedFile, request)
}

private fun copyLocalFileIntoVstWinePicker(
    source: java.io.File,
    request: VstWineFilePickerRequest
): String {
    val destDir = java.io.File(request.copyDirLinux)
    if (!destDir.exists() && !destDir.mkdirs()) {
        throw IOException("Could not create ${destDir.absolutePath}")
    }

    val safeName = sanitizeVstWineFileName(source.name)
    val destFile = uniqueVstWineDestinationFile(destDir, safeName)
    source.inputStream().use { input ->
        destFile.outputStream().use { output -> input.copyTo(output) }
    }

    val windowsDir = request.copyDirWindows.ifBlank { "C:\\vstpoc_picker" }.trimEnd('\\')
    return "$windowsDir\\${destFile.name}"
}

private fun sanitizeVstWineFileName(name: String): String {
    val sanitized = name
        .substringAfterLast('/')
        .replace(Regex("[\\\\/:*?\"<>|]"), "_")
        .trim()
        .trim('.')
    return sanitized.ifEmpty { "picked_file" }.take(180)
}

private fun uniqueVstWineDestinationFile(dir: java.io.File, fileName: String): java.io.File {
    val first = java.io.File(dir, fileName)
    if (!first.exists()) return first

    val dot = fileName.lastIndexOf('.')
    val base = if (dot > 0) fileName.substring(0, dot) else fileName
    val ext = if (dot > 0) fileName.substring(dot) else ""
    for (i in 1..999) {
        val candidate = java.io.File(dir, "${base}_$i$ext")
        if (!candidate.exists()) return candidate
    }
    return java.io.File(dir, "${base}_${System.currentTimeMillis()}$ext")
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun ModelPicker(
    pluginIndex: Int,
    pathId: Long,
    viewModel: RackViewModel,
    config: ModelPluginConfig,
    externalPickerTrigger: Int = 0,
    onActiveModelChanged: ((String?) -> Unit)? = null,
    onModelFilesChanged: ((List<java.io.File>) -> Unit)? = null,
    onNavigateToTone3000: () -> Unit = {}
) {
    val selectedPathId = pathId
    val context = LocalContext.current
    val scope = rememberCoroutineScope()
    var modelFiles by remember { mutableStateOf<List<java.io.File>>(emptyList()) }
    var selectedModelIndex by rememberSaveable { mutableStateOf(-1) }
    var activeModelName by rememberSaveable { mutableStateOf<String?>(null) }
    var isExtracting by remember { mutableStateOf(false) }
    var showDialog by remember { mutableStateOf(false) }

    // Report active model name changes to parent
    LaunchedEffect(activeModelName) {
        onActiveModelChanged?.invoke(activeModelName)
    }

    // Report model files changes to parent
    LaunchedEffect(modelFiles) {
        onModelFilesChanged?.invoke(modelFiles)
    }

    val launcher = rememberLauncherForActivityResult(
        contract = ActivityResultContracts.OpenDocument()
    ) { uri ->
        if (uri == null) return@rememberLauncherForActivityResult

        val fileName = resolvePickedFileName(
            context,
            uri,
            "model",
            config.extensions + "zip"
        )

        if (fileName.lowercase().endsWith(".zip")) {
            isExtracting = true
            scope.launch(Dispatchers.IO) {
                try {
                    val models = extractModelsFromZip(context, uri, fileName, config)
                    withContext(Dispatchers.Main) {
                        isExtracting = false
                        if (models.isNotEmpty()) {
                            modelFiles = models
                            selectedModelIndex = 0
                            activeModelName = models[0].nameWithoutExtension
                            viewModel.setPluginFilePath(selectedPathId, pluginIndex, config.propertyUri, models[0].absolutePath)
                        } else {
                            Log.e("ModelPicker", "No models found in $fileName")
                        }
                    }
                } catch (e: Exception) {
                    Log.e("ModelPicker", "Failed to extract zip: ${e.message}", e)
                    withContext(Dispatchers.Main) { isExtracting = false }
                }
            }
        } else {
            try {
                val storageDir = config.storageDirs.first()
                val modelsDir = java.io.File(context.filesDir, storageDir)
                modelsDir.mkdirs()
                val destFile = java.io.File(modelsDir, fileName)

                context.contentResolver.openInputStream(uri)?.use { input ->
                    destFile.outputStream().use { output -> input.copyTo(output) }
                }

                modelFiles = listOf(destFile)
                selectedModelIndex = 0
                activeModelName = destFile.nameWithoutExtension
                viewModel.setPluginFilePath(selectedPathId, pluginIndex, config.propertyUri, destFile.absolutePath)
            } catch (e: Exception) {
                Log.e("ModelPicker", "Failed to load model: ${e.message}", e)
            }
        }
    }

    // External trigger to open file picker (e.g. from modgui click)
    LaunchedEffect(externalPickerTrigger) {
        if (externalPickerTrigger > 0) {
            showDialog = true
        }
    }

    if (showDialog) {
        GenericFilePickerDialog(
            title = "Select Model",
            storageDirs = config.storageDirs,
            extensions = config.extensions,
            builtInItems = config.builtInItems,
            onFileSelected = { path ->
                showDialog = false
                val name = if (path == "default") {
                    config.builtInItems.find { it.second == "default" }?.first ?: "Default"
                } else {
                    java.io.File(path).nameWithoutExtension
                }
                activeModelName = name
                viewModel.setPluginFilePath(selectedPathId, pluginIndex, config.propertyUri, path)
            },
            onBrowseFiles = {
                showDialog = false
                launcher.launch(arrayOf("*/*"))
            },
            onNavigateToTone3000 = onNavigateToTone3000,
            onDismiss = {
                showDialog = false
            }
        )
    }
}





private fun formatElapsedTime(sec: Double): String {
    val totalSec = sec.coerceAtLeast(0.0).toLong()
    val hours = totalSec / 3_600
    val minutes = totalSec % 3_600 / 60
    val seconds = totalSec % 60
    return if (hours > 0) "%d:%02d:%02d".format(hours, minutes, seconds)
    else "%d:%02d".format(minutes, seconds)
}

private fun formatMusicalPosition(positionSec: Double, beatsPerMinute: Double): String {
    val totalSixteenths = (
        positionSec.coerceAtLeast(0.0) * beatsPerMinute.coerceAtLeast(1.0) / 60.0 * 4.0
    ).toLong()
    val bar = totalSixteenths / 16 + 1
    val beat = totalSixteenths % 16 / 4 + 1
    val sixteenth = totalSixteenths % 4 + 1
    return "$bar:$beat:$sixteenth"
}
private fun queryDisplayName(context: android.content.Context, uri: Uri): String? {
    return context.contentResolver.query(uri, arrayOf(OpenableColumns.DISPLAY_NAME), null, null, null)
        ?.use { cursor -> if (cursor.moveToFirst()) cursor.getString(0) else null }
}


