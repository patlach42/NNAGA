package com.vibes.dsp.ui.live

import android.net.Uri
import android.os.Build
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.ExperimentalFoundationApi
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.combinedClickable
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.gestures.detectHorizontalDragGestures
import androidx.compose.foundation.gestures.detectVerticalDragGestures
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.requiredWidth
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.widthIn
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Add
import androidx.compose.material.icons.filled.FiberManualRecord
import androidx.compose.material.icons.filled.GraphicEq
import androidx.compose.material.icons.filled.KeyboardArrowDown
import androidx.compose.material.icons.filled.KeyboardArrowUp
import androidx.compose.material.icons.filled.Remove
import androidx.compose.material.icons.filled.Mic
import androidx.compose.material.icons.filled.Pause
import androidx.compose.material.icons.filled.PlayArrow
import androidx.compose.material.icons.filled.Settings
import androidx.compose.material.icons.filled.SkipPrevious
import androidx.compose.material.icons.filled.MoreVert
import androidx.compose.material.icons.filled.Tune
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Checkbox
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.RadioButton
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.key
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableLongStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.drawBehind
import androidx.compose.ui.draw.clipToBounds
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.graphics.RectangleShape
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.platform.LocalView
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.ProgressBarRangeInfo
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.disabled
import androidx.compose.ui.semantics.progressBarRangeInfo
import androidx.compose.ui.semantics.selected
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.semantics.stateDescription
import androidx.compose.ui.semantics.setProgress
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.vibes.dsp.engine.ClipSlotInfo
import com.vibes.dsp.engine.DirectUsbAudioManager
import com.vibes.dsp.engine.DirectUsbSessionState
import com.vibes.dsp.engine.MidiNoteInfo
import com.vibes.dsp.engine.RackPathId
import com.vibes.dsp.engine.RackTrackInfo
import com.vibes.dsp.engine.TrackLaunchQuantization
import com.vibes.dsp.ui.rack.PluginCard
import com.vibes.dsp.ui.rack.RackViewModel
import com.vibes.dsp.ui.theme.AppearancePreferences
import kotlin.math.roundToInt

private enum class InspectorTab { Clips, Track }

private data class TopCutoutBounds(
    val left: Int = 0,
    val right: Int = 0,
    val bottom: Int = 0,
) {
    val present: Boolean get() = right > left && bottom > 0
}

private fun topCutoutBounds(view: android.view.View): TopCutoutBounds {
    if (Build.VERSION.SDK_INT < Build.VERSION_CODES.P) return TopCutoutBounds()
    val cutout = view.rootWindowInsets?.displayCutout ?: return TopCutoutBounds()
    val centralTopBounds = cutout.boundingRects
        .filter { it.top <= 1 && it.width() > 0 }
    if (centralTopBounds.isEmpty()) return TopCutoutBounds()
    return TopCutoutBounds(
        left = centralTopBounds.minOf { it.left },
        right = centralTopBounds.maxOf { it.right },
        bottom = centralTopBounds.maxOf { it.bottom },
    )
}

private object LiveColors {
    val panel = Color.Black
    val raised = Color(0xFF0B0B0B)
    val card = Color(0xFF111111)
    val divider = Color(0xFF292929)
    val waveformLine = Color(0xFF343434)
    val text = Color(0xFFF2F2F2)
    val textMuted = Color(0xFFA3A3A3)
    val textDim = Color(0xFF707070)
    val audio = Color(0xFF64C7B4)
    val midi = Color(0xFFA98BEF)
    val record = Color(0xFFFF4D4D)
    val error = Color(0xFFFF6B6B)
}

private object LiveDimensions {
    val hitTarget = 44.dp
    val control = 32.dp
    val toolbar = 44.dp
    val transport = 44.dp
    val trackWidth = 120.dp
    val trackHeader = 44.dp
    val slotHeight = 48.dp
    val mixerChannel = 64.dp
    val pluginMinWidth = 216.dp
    val pluginMaxWidth = 288.dp
    val icon = 18.dp
    val gap = 8.dp
    val smallGap = 4.dp
    val hairline = 1.dp
}

@Composable
@OptIn(ExperimentalFoundationApi::class)
fun LiveScreen(
    viewModel: RackViewModel,
    onNavigateToBrowser: (Long, Int) -> Unit,
    onNavigateToSettings: () -> Unit = {},
    onNavigateToTone3000: (String?, String?, String?, Int, String?) -> Unit = { _, _, _, _, _ -> },
) {
    val context = LocalContext.current
    val tracks by viewModel.tracks.collectAsState()
    val transport by viewModel.transport.collectAsState()
    val selectedPlugins by viewModel.selectedPathPlugins.collectAsState()
    val selectedPath by viewModel.selectedPathId.collectAsState()
    val engineRunning by viewModel.isEngineRunning.collectAsState()
    val meterState by viewModel.meterState.collectAsState()
    val latencyMs by viewModel.latencyMs.collectAsState()
    val cpuLoad by viewModel.cpuLoad.collectAsState()
    val xRunCount by viewModel.xRunCount.collectAsState()
    val directUsbState by viewModel.directUsbState.collectAsState()
    val directUsbStats by viewModel.directUsbStats.collectAsState()
    val errorMessage by viewModel.errorMessage.collectAsState()
    val blockingOperation by viewModel.blockingOperation.collectAsState()
    val slotsByTrack by viewModel.clipSlots.collectAsState()
    val peaksByTrack by viewModel.waveformPeaks.collectAsState()
    val notesByClip by viewModel.midiNotes.collectAsState()
    val inputChannelCount = DirectUsbAudioManager.getInputChannelCount()

    var launchQuantizationOrdinal by rememberSaveable { mutableIntStateOf(0) }
    val launchQuantization = TrackLaunchQuantization.entries[launchQuantizationOrdinal]
    var loopLengthsByTrack by rememberSaveable { mutableStateOf<Map<Long, Double>>(emptyMap()) }
    var showTempoDialog by rememberSaveable { mutableStateOf(false) }
    var tempoInput by rememberSaveable { mutableStateOf("") }
    var selectedTrackId by rememberSaveable { mutableLongStateOf(0L) }
    var selectedSlot by rememberSaveable { mutableIntStateOf(0) }
    var tab by rememberSaveable { mutableIntStateOf(InspectorTab.Clips.ordinal) }
    var importTarget by remember { mutableStateOf<Pair<Long, Int>?>(null) }
    var inputMenuTrack by remember { mutableStateOf<RackTrackInfo?>(null) }
    var recordMenuTrack by remember { mutableStateOf<RackTrackInfo?>(null) }
    var trackColorOverrides by remember { mutableStateOf<Map<Long, Int>>(emptyMap()) }
    var showQuantizationMenu by remember { mutableStateOf(false) }
    var editTiles by rememberSaveable { mutableStateOf(false) }
    var tileOrder by rememberSaveable { mutableStateOf(LiveLayoutPreferences.getTileOrder(context)) }
    var visibleTiles by rememberSaveable { mutableStateOf(LiveLayoutPreferences.getVisibleTiles(context)) }
    val horizontalPlugins = remember { LiveLayoutPreferences.getHorizontalPlugins(context) }
    val fitTilesOnScreen = remember { LiveLayoutPreferences.getFitTilesOnScreen(context) }
    var tileHeights by remember {
        mutableStateOf(tileOrder.associateWith { id -> LiveLayoutPreferences.getTileHeight(context, id) })
    }
    val supportedLoopLengths = listOf(
        0.25 to "1/4", 1.0 to "1 bar", 2.0 to "2 bars", 4.0 to "4 bars",
        8.0 to "8 bars", 16.0 to "16 bars",
    )
    val launcherHorizontalScrollState = rememberScrollState()
    val launcherVerticalScrollState = rememberScrollState()
    val mixerScrollState = rememberScrollState()
    val contentScrollState = rememberScrollState()

    fun toggleTile(id: String) {
        visibleTiles = if (id in visibleTiles) visibleTiles - id else visibleTiles + id
        LiveLayoutPreferences.setVisibleTiles(context, visibleTiles)
    }

    fun moveTile(id: String, direction: Int) {
        val displayed = tileOrder.filter { it in visibleTiles }
        val displayedIndex = displayed.indexOf(id)
        val otherId = displayed.getOrNull(displayedIndex + direction) ?: return
        val from = tileOrder.indexOf(id)
        val to = tileOrder.indexOf(otherId)
        tileOrder = tileOrder.toMutableList().apply {
            this[from] = otherId
            this[to] = id
        }
        LiveLayoutPreferences.setTileOrder(context, tileOrder)
    }

    fun resizeTile(id: String, deltaDp: Float) {
        val currentHeight = tileHeights[id] ?: LiveLayoutPreferences.getTileHeight(context, id)
        val resizedHeight = LiveLayoutPreferences.clampTileHeight(id, currentHeight + deltaDp)
        if (resizedHeight == currentHeight) return
        tileHeights = tileHeights + (id to resizedHeight)
        LiveLayoutPreferences.setTileHeight(context, id, resizedHeight)
    }

    val picker = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()) { uri: Uri? ->
        importTarget?.let { (trackId, slot) ->
            if (uri != null) {
                viewModel.loadTrackClipMedia(trackId, slot, uri, uri.lastPathSegment ?: "Clip")
            }
        }
        importTarget = null
    }

    LaunchedEffect(Unit) {
        viewModel.setRackVisible(true)
        viewModel.refreshRack()
    }
    LaunchedEffect(tracks) {
        tracks.forEach { viewModel.refreshTrackClipSlots(it.id) }
    }

    val selectedTrack = tracks.firstOrNull { it.id == selectedTrackId } ?: tracks.firstOrNull()
    val selectedClip = selectedTrack?.let { track ->
        slotsByTrack[track.id]?.firstOrNull { it.slot == selectedSlot }
    }

    LaunchedEffect(selectedTrack?.id, selectedSlot, selectedClip?.midiLoaded, selectedClip?.wavLoaded) {
        selectedTrack ?: return@LaunchedEffect
        if (selectedClip?.midiLoaded == true) viewModel.loadTrackClipMidiNotes(selectedTrack.id, selectedSlot)
        if (selectedClip?.wavLoaded == true) viewModel.loadTrackWaveform(selectedTrack.id)
    }

    inputMenuTrack?.let { track ->
        AlertDialog(
            onDismissRequest = { inputMenuTrack = null },
            title = { Text("Input channel") },
            text = {
                if (inputChannelCount == 0) {
                    Text("No input channels available")
                } else {
                    Column {
                        (0 until inputChannelCount).forEach { channel ->
                            DropdownMenuItem(
                                text = { Text("Channel ${channel + 1}") },
                                onClick = {
                                    viewModel.setTrackInputChannel(track.id, channel)
                                    inputMenuTrack = null
                                },
                            )
                        }
                    }
                }
            },
            confirmButton = { TextButton(onClick = { inputMenuTrack = null }) { Text("Cancel") } },
        )
    }
    recordMenuTrack?.let { track ->
        val enterOnPunchEnabled = !transport.playing || launchQuantization == TrackLaunchQuantization.None
        AlertDialog(
            onDismissRequest = { recordMenuTrack = null },
            title = { Text("Recording options") },
            text = {
                Column {
                    Text("Loop length (bars)")
                    supportedLoopLengths.forEach { (bars, label) ->
                        Row(
                            Modifier.fillMaxWidth().clickable {
                                loopLengthsByTrack = loopLengthsByTrack + (track.id to bars)
                            },
                            verticalAlignment = Alignment.CenterVertically,
                        ) {
                            RadioButton(
                                selected = (loopLengthsByTrack[track.id] ?: 1.0) == bars,
                                onClick = { loopLengthsByTrack = loopLengthsByTrack + (track.id to bars) },
                            )
                            Text(label)
                        }
                    }
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        Checkbox(
                            checked = track.punchArmed,
                            enabled = enterOnPunchEnabled,
                            onCheckedChange = {
                                viewModel.setEnterOnPunch(
                                    track.id,
                                    loopLengthsByTrack[track.id] ?: 1.0,
                                    launchQuantization,
                                    armed = it,
                                )
                                recordMenuTrack = null
                            },
                        )
                        Text("Enter on punch")
                    }
                }
            },
            confirmButton = { TextButton(onClick = { recordMenuTrack = null }) { Text("Done") } },
        )
    }
    if (showQuantizationMenu) {
        AlertDialog(
            onDismissRequest = { showQuantizationMenu = false },
            title = { Text("Launch quantization") },
            text = {
                Column {
                    TrackLaunchQuantization.entries.forEachIndexed { index, quantization ->
                        Row(
                            Modifier.fillMaxWidth().clickable {
                                launchQuantizationOrdinal = index
                                showQuantizationMenu = false
                            },
                            verticalAlignment = Alignment.CenterVertically,
                        ) {
                            RadioButton(
                                selected = launchQuantization == quantization,
                                onClick = {
                                    launchQuantizationOrdinal = index
                                    showQuantizationMenu = false
                                },
                            )
                            Text(quantization.name)
                        }
                    }
                }
            },
            confirmButton = { TextButton(onClick = { showQuantizationMenu = false }) { Text("Cancel") } },
        )
    }
    if (showTempoDialog) {
        AlertDialog(
            onDismissRequest = { showTempoDialog = false },
            title = { Text("Tempo") },
            text = {
                OutlinedTextField(
                    value = tempoInput,
                    onValueChange = { tempoInput = it },
                    keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Decimal),
                    suffix = { Text(" BPM") },
                )
            },
            confirmButton = {
                TextButton(onClick = {
                    tempoInput.toDoubleOrNull()?.coerceIn(20.0, 400.0)?.let(viewModel::setTransportBpm)
                    showTempoDialog = false
                }) { Text("Apply") }
            },
            dismissButton = { TextButton(onClick = { showTempoDialog = false }) { Text("Cancel") } },
        )
    }

    Scaffold(
        modifier = Modifier.fillMaxSize(),
        containerColor = Color.Black,
        contentWindowInsets = WindowInsets(0, 0, 0, 0),
    ) { contentPadding ->
        Column(Modifier.fillMaxSize().padding(contentPadding)) {
            CameraToolbar(
                visibleTiles = visibleTiles,
                editMode = editTiles,
                onToggleTile = ::toggleTile,
                onSettings = onNavigateToSettings,
                onToggleEdit = { editTiles = !editTiles },
            )
            TransportBar(
                playing = transport.playing,
                bpm = transport.beatsPerMinute,
                onPlay = { if (transport.playing) viewModel.transportPause() else viewModel.transportPlay() },
                onRestart = viewModel::transportRestart,
                onBpm = {
                    tempoInput = transport.beatsPerMinute.toString()
                    showTempoDialog = true
                },
            )
            StatusStrip(
                engineRunning = engineRunning,
                meterState = meterState,
                latencyMs = latencyMs,
                cpuLoad = cpuLoad,
                xRunCount = xRunCount,
                usbState = directUsbState,
                usbStats = directUsbStats,
                errorMessage = errorMessage,
                blockingOperation = blockingOperation,
                onToggleEngine = { if (engineRunning) viewModel.stopEngine() else viewModel.startEngine() },
                onResetClipping = viewModel::resetClipping,
                onClearError = viewModel::clearError,
            )
            val displayedTiles = tileOrder.filter { it in visibleTiles }
            val tileStackModifier = Modifier.fillMaxWidth().weight(1f)
            Column(
                modifier = if (fitTilesOnScreen) {
                    tileStackModifier
                } else {
                    tileStackModifier.verticalScroll(contentScrollState)
                },
            ) {
                displayedTiles.forEachIndexed { displayedIndex, tileId ->
                    val currentHeight = tileHeights[tileId]
                        ?: LiveLayoutPreferences.defaultTileHeight(tileId)
                    val smallerHeight = LiveLayoutPreferences.clampTileHeight(tileId, currentHeight - 32f)
                    val largerHeight = LiveLayoutPreferences.clampTileHeight(tileId, currentHeight + 32f)
                    TileContainer(
                        id = tileId,
                        modifier = if (fitTilesOnScreen) {
                            Modifier.weight(currentHeight.coerceAtLeast(1f))
                        } else {
                            Modifier.height(currentHeight.dp)
                        },
                        editMode = editTiles,
                        currentHeight = currentHeight,
                        canShrink = smallerHeight < currentHeight,
                        canGrow = largerHeight > currentHeight,
                        canMoveUp = displayedIndex > 0,
                        canMoveDown = displayedIndex < displayedTiles.lastIndex,
                        onShrink = { resizeTile(tileId, -32f) },
                        onGrow = { resizeTile(tileId, 32f) },
                        onMoveUp = { moveTile(tileId, -1) },
                        onMoveDown = { moveTile(tileId, 1) },
                    ) {
                        when (tileId) {
                            "launcher" -> {
                                Launcher(
                                    tracks = tracks,
                                    slotsByTrack = slotsByTrack,
                                    selectedTrack = selectedTrack,
                                    selectedSlot = selectedSlot,
                                    trackColors = tracks.mapIndexed { index, track ->
                                        track.id to (
                                            trackColorOverrides[track.id]
                                                ?: LiveLayoutPreferences.getTrackColor(
                                                    context,
                                                    track.id,
                                                    AppearancePreferences.palettes[
                                                        index % AppearancePreferences.palettes.size
                                                    ].argb,
                                                )
                                            )
                                    }.toMap(),
                                    horizontalScrollState = launcherHorizontalScrollState,
                                    verticalScrollState = launcherVerticalScrollState,
                                    modifier = Modifier.fillMaxSize(),
                                    onSelectTrack = { track ->
                                        val slot = slotsByTrack[track.id]
                                            ?.firstOrNull { it.active }
                                            ?.slot
                                            ?: 0
                                        selectedTrackId = track.id
                                        selectedSlot = slot
                                        viewModel.selectPath(track.id)
                                        viewModel.selectTrackClipSlot(track.id, slot)
                                    },
                                    onSelect = { track, slot ->
                                        selectedTrackId = track.id
                                        selectedSlot = slot
                                        viewModel.selectPath(track.id)
                                        viewModel.selectTrackClipSlot(track.id, slot)
                                    },
                                    onLoad = { track, slot ->
                                        importTarget = track.id to slot
                                        picker.launch(arrayOf("audio/*", "audio/midi", "audio/x-midi", "application/x-midi"))
                                    },
                                    onLaunch = { track, slot ->
                                        selectedTrackId = track.id
                                        selectedSlot = slot
                                        viewModel.selectTrackClipSlot(track.id, slot)
                                        val active = track.playing &&
                                            slotsByTrack[track.id]?.firstOrNull { it.slot == slot }?.active == true
                                        if (active) {
                                            viewModel.setTrackTransportPlaying(track.id, false, launchQuantization)
                                        } else {
                                            viewModel.launchTrackTransport(track.id, launchQuantization, !transport.playing)
                                        }
                                    },
                                    onRecordClip = { track, slot ->
                                        selectedTrackId = track.id
                                        selectedSlot = slot
                                        viewModel.selectPath(track.id)
                                        viewModel.startTrackClipRecording(
                                            track.id,
                                            slot,
                                            loopLengthsByTrack[track.id] ?: 1.0,
                                            launchQuantization,
                                            startGlobal = !transport.playing,
                                        )
                                    },
                                    onTrackColor = { track, argb ->
                                        LiveLayoutPreferences.setTrackColor(context, track.id, argb)
                                        trackColorOverrides = trackColorOverrides + (track.id to argb)
                                    },
                                )
                            }
                            "inspector" -> ClipInspector(
                                track = selectedTrack,
                                clip = selectedClip,
                                peaks = peaksByTrack[selectedTrack?.id].orEmpty(),
                                notes = notesByClip[selectedTrack?.id to selectedSlot].orEmpty(),
                                tab = InspectorTab.entries.getOrElse(tab) { InspectorTab.Clips },
                                onTab = { tab = it.ordinal },
                                onTrackVolume = { track, volume -> viewModel.setTrackVolume(track.id, volume) },
                                onTrackInput = { inputMenuTrack = it },
                                onTrackArm = { track ->
                                    viewModel.setTrackInputArmed(track.id, !track.inputArmed)
                                },
                                onTrackDelete = { track -> viewModel.removeTrack(track.id) },
                                onTrackRecordMenu = { recordMenuTrack = it },
                                launchQuantization = launchQuantization,
                                onLaunchQuantizationClick = { showQuantizationMenu = true },
                                modifier = Modifier.fillMaxSize(),
                            )
                            "devices" -> DevicesTile(
                                devices = selectedPlugins,
                                pathId = selectedPath,
                                viewModel = viewModel,
                                horizontal = horizontalPlugins,
                                onBrowser = { path -> onNavigateToBrowser(path, -1) },
                                modifier = Modifier.fillMaxSize(),
                            )
                            "mixer" -> Mixer(
                                tracks = tracks,
                                selected = selectedTrack?.id,
                                scrollState = mixerScrollState,
                                onSelect = { track ->
                                    selectedTrackId = track.id
                                    selectedSlot = slotsByTrack[track.id]?.firstOrNull { it.active }?.slot ?: 0
                                    viewModel.selectPath(track.id)
                                    viewModel.selectTrackClipSlot(track.id, selectedSlot)
                                },
                                onVolume = { track, gain -> viewModel.setTrackVolume(track.id, gain) },
                                onAdd = viewModel::addTrack,
                                modifier = Modifier.fillMaxSize(),
                            )
                        }
                    }
                }
                if (displayedTiles.isEmpty()) {
                    Text(
                        "Use the controls above to show Clip Launcher, Devices, or Mixer.",
                        color = LiveColors.textMuted,
                        style = MaterialTheme.typography.bodySmall,
                        modifier = Modifier.padding(horizontal = LiveDimensions.gap, vertical = LiveDimensions.smallGap),
                    )
                }
            }
        }
    }
}

@Composable
@OptIn(ExperimentalFoundationApi::class)
private fun CameraToolbar(
    visibleTiles: Set<String>,
    editMode: Boolean,
    onToggleTile: (String) -> Unit,
    onSettings: () -> Unit,
    onToggleEdit: () -> Unit,
) {
    val view = LocalView.current
    val density = LocalDensity.current
    val cutout = topCutoutBounds(view)
    BoxWithConstraints(
        modifier = Modifier.fillMaxWidth().background(LiveColors.panel),
    ) {
        val leftWidth = with(density) { cutout.left.toDp() }
        val rightWidth = maxWidth - with(density) { cutout.right.toDp() }
        val cutoutBottom = with(density) { cutout.bottom.toDp() }
        val canUseSideZones = cutout.present &&
            leftWidth >= LiveDimensions.hitTarget * 2 &&
            rightWidth >= LiveDimensions.hitTarget * 3 + LiveDimensions.smallGap * 2

        if (canUseSideZones) {
            Box(
                modifier = Modifier.fillMaxWidth().height(maxOf(LiveDimensions.toolbar, cutoutBottom)),
            ) {
                Row(
                    modifier = Modifier.align(Alignment.TopStart).width(leftWidth),
                    horizontalArrangement = Arrangement.SpaceEvenly,
                ) {
                    CameraToolbarButton("mixer", "Mixer tile", visibleTiles, onToggleTile)
                    CameraToolbarButton("devices", "Devices tile", visibleTiles, onToggleTile)
                }
                Row(
                    modifier = Modifier.align(Alignment.TopEnd).width(rightWidth),
                    horizontalArrangement = Arrangement.SpaceEvenly,
                ) {
                    CameraToolbarButton("launcher", "Clip launcher tile", visibleTiles, onToggleTile)
                    CameraToolbarButton("inspector", "Clip inspector tile", visibleTiles, onToggleTile)
                    NnagaToolbarButton(editMode, onSettings, onToggleEdit)
                }
            }
        } else {
            Column {
                if (cutout.present) Spacer(Modifier.height(cutoutBottom))
                Row(
                    modifier = Modifier.fillMaxWidth().height(LiveDimensions.toolbar),
                    horizontalArrangement = Arrangement.SpaceEvenly,
                ) {
                    CameraToolbarButton("mixer", "Mixer tile", visibleTiles, onToggleTile)
                    CameraToolbarButton("devices", "Devices tile", visibleTiles, onToggleTile)
                    CameraToolbarButton("launcher", "Clip launcher tile", visibleTiles, onToggleTile)
                    CameraToolbarButton("inspector", "Clip inspector tile", visibleTiles, onToggleTile)
                    NnagaToolbarButton(editMode, onSettings, onToggleEdit)
                }
            }
        }
    }
}

@Composable
private fun CameraToolbarButton(
    id: String,
    contentDescription: String,
    visibleTiles: Set<String>,
    onToggleTile: (String) -> Unit,
) {
    val active = id in visibleTiles
    val accent = MaterialTheme.colorScheme.primary
    Box(
        modifier = Modifier.size(LiveDimensions.hitTarget)
            .semantics { selected = active }
            .clickable(role = Role.Button) { onToggleTile(id) },
        contentAlignment = Alignment.Center,
    ) {
        Surface(
            color = if (active) accent.copy(alpha = 0.16f) else Color.Transparent,
            contentColor = if (active) accent else LiveColors.textMuted,
            shape = RoundedCornerShape(2.dp),
            modifier = Modifier.size(LiveDimensions.control),
        ) {
            Box(contentAlignment = Alignment.Center) {
                when (id) {
                    "mixer" -> Icon(Icons.Default.Tune, contentDescription, modifier = Modifier.size(LiveDimensions.icon))
                    "devices" -> Icon(Icons.Default.Settings, contentDescription, modifier = Modifier.size(LiveDimensions.icon))
                    "inspector" -> Icon(Icons.Default.GraphicEq, contentDescription, modifier = Modifier.size(LiveDimensions.icon))
                    else -> Icon(Icons.Default.PlayArrow, contentDescription, modifier = Modifier.size(LiveDimensions.icon))
                }
            }
        }
    }
}

@Composable
@OptIn(ExperimentalFoundationApi::class)
private fun NnagaToolbarButton(
    editMode: Boolean,
    onSettings: () -> Unit,
    onToggleEdit: () -> Unit,
) {
    val accent = MaterialTheme.colorScheme.primary
    Box(
        modifier = Modifier.height(LiveDimensions.hitTarget).widthIn(min = LiveDimensions.hitTarget)
            .combinedClickable(
                role = Role.Button,
                onClickLabel = "Open settings",
                onLongClickLabel = "Edit tile order",
                onClick = onSettings,
                onLongClick = onToggleEdit,
            ),
        contentAlignment = Alignment.Center,
    ) {
        Surface(
            color = if (editMode) accent.copy(alpha = 0.16f) else Color.Transparent,
            contentColor = if (editMode) accent else LiveColors.text,
            shape = RoundedCornerShape(2.dp),
            modifier = Modifier.height(LiveDimensions.control),
        ) {
            Box(Modifier.padding(horizontal = LiveDimensions.gap), contentAlignment = Alignment.Center) {
                Text("NNAGA", style = MaterialTheme.typography.labelSmall, fontWeight = FontWeight.Black)
            }
        }
    }
}

@Composable
private fun TileContainer(
    id: String,
    modifier: Modifier,
    editMode: Boolean,
    currentHeight: Float,
    canShrink: Boolean,
    canGrow: Boolean,
    canMoveUp: Boolean,
    canMoveDown: Boolean,
    onShrink: () -> Unit,
    onGrow: () -> Unit,
    onMoveUp: () -> Unit,
    onMoveDown: () -> Unit,
    content: @Composable Function0<Unit>,
) {
    val title = when (id) {
        "launcher" -> "CLIP LAUNCHER"
        "inspector" -> "CLIP INSPECTOR"
        "devices" -> "DEVICES"
        "mixer" -> "MIXER"
        else -> id.uppercase()
    }
    Column(
        modifier = modifier.fillMaxWidth().clipToBounds().background(LiveColors.panel),
    ) {
        Box(Modifier.fillMaxWidth().height(LiveDimensions.hairline).background(LiveColors.divider))
        if (editMode) {
            Row(
                modifier = Modifier.fillMaxWidth().height(LiveDimensions.hitTarget)
                    .padding(start = LiveDimensions.gap),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Text(
                    title,
                    color = LiveColors.textMuted,
                    style = MaterialTheme.typography.labelMedium,
                    fontWeight = FontWeight.SemiBold,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                    modifier = Modifier.weight(1f),
                )
                Text(
                    "${currentHeight.roundToInt()} dp",
                    color = LiveColors.textDim,
                    style = MaterialTheme.typography.labelSmall,
                    maxLines = 1,
                )
                IconButton(
                    onClick = onShrink,
                    enabled = canShrink,
                    modifier = Modifier.size(LiveDimensions.control),
                ) {
                    Icon(
                        Icons.Default.Remove,
                        "Decrease $title height by 32 dp",
                        modifier = Modifier.size(LiveDimensions.icon),
                    )
                }
                IconButton(
                    onClick = onGrow,
                    enabled = canGrow,
                    modifier = Modifier.size(LiveDimensions.control),
                ) {
                    Icon(
                        Icons.Default.Add,
                        "Increase $title height by 32 dp",
                        modifier = Modifier.size(LiveDimensions.icon),
                    )
                }
                IconButton(
                    onClick = onMoveUp,
                    enabled = canMoveUp,
                    modifier = Modifier.size(LiveDimensions.control),
                ) {
                    Icon(
                        Icons.Default.KeyboardArrowUp,
                        "Move $title up",
                        modifier = Modifier.size(LiveDimensions.icon),
                    )
                }
                IconButton(
                    onClick = onMoveDown,
                    enabled = canMoveDown,
                    modifier = Modifier.size(LiveDimensions.control),
                ) {
                    Icon(
                        Icons.Default.KeyboardArrowDown,
                        "Move $title down",
                        modifier = Modifier.size(LiveDimensions.icon),
                    )
                }
            }
        }
        Box(Modifier.fillMaxWidth().weight(1f).clipToBounds()) {
            content()
        }
    }
}

@Composable
private fun DevicesTile(
    devices: List<com.vibes.dsp.ui.rack.RackPlugin>,
    pathId: Long,
    viewModel: RackViewModel,
    horizontal: Boolean,
    onBrowser: (Long) -> Unit,
    modifier: Modifier,
) {
    val horizontalScrollState = rememberScrollState()
    val verticalScrollState = rememberScrollState()
    Column(modifier = modifier) {
        Row(
            modifier = Modifier.fillMaxWidth().height(LiveDimensions.hitTarget)
                .padding(start = LiveDimensions.gap, end = LiveDimensions.smallGap),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Text(
                "DEVICES",
                color = LiveColors.textMuted,
                style = MaterialTheme.typography.labelMedium,
                fontWeight = FontWeight.SemiBold,
                modifier = Modifier.weight(1f),
            )
            TextButton(
                onClick = { onBrowser(pathId) },
                modifier = Modifier.height(LiveDimensions.hitTarget),
            ) {
                Icon(
                    Icons.Default.Add,
                    contentDescription = null,
                    modifier = Modifier.size(LiveDimensions.icon),
                )
                Text("ADD", modifier = Modifier.padding(start = LiveDimensions.smallGap))
            }
        }
        Box(Modifier.fillMaxWidth().weight(1f).clipToBounds()) {
            when {
                devices.isEmpty() -> {
                    Text(
                        "No devices on this track",
                        color = LiveColors.textMuted,
                        style = MaterialTheme.typography.bodySmall,
                        modifier = Modifier.padding(
                            start = LiveDimensions.gap,
                            end = LiveDimensions.gap,
                            bottom = LiveDimensions.smallGap,
                        ),
                    )
                }
                horizontal -> {
                    Box(
                        modifier = Modifier.fillMaxSize().verticalScroll(verticalScrollState),
                    ) {
                        Row(
                            modifier = Modifier.fillMaxWidth().horizontalScroll(horizontalScrollState)
                                .padding(horizontal = LiveDimensions.smallGap),
                            horizontalArrangement = Arrangement.spacedBy(LiveDimensions.smallGap),
                        ) {
                            devices.forEach { plugin ->
                                key(plugin.instanceId) {
                                    var expanded by rememberSaveable(plugin.instanceId) { mutableStateOf(true) }
                                    PluginCard(
                                        plugin = plugin,
                                        pluginIndex = plugin.index,
                                        pathId = pathId,
                                        viewModel = viewModel,
                                        onRemove = { viewModel.removePlugin(pathId, plugin.index) },
                                        onReplace = { onBrowser(pathId) },
                                        expanded = expanded,
                                        onExpandedChange = { expanded = it },
                                        compact = true,
                                        modifier = Modifier.widthIn(
                                            min = LiveDimensions.pluginMinWidth,
                                            max = LiveDimensions.pluginMaxWidth,
                                        ),
                                    )
                                }
                            }
                        }
                    }
                }
                else -> {
                    Column(
                        modifier = Modifier.fillMaxSize().verticalScroll(verticalScrollState)
                            .padding(horizontal = LiveDimensions.smallGap),
                        verticalArrangement = Arrangement.spacedBy(LiveDimensions.smallGap),
                    ) {
                        devices.forEach { plugin ->
                            key(plugin.instanceId) {
                                var expanded by rememberSaveable(plugin.instanceId) { mutableStateOf(true) }
                                PluginCard(
                                    plugin = plugin,
                                    pluginIndex = plugin.index,
                                    pathId = pathId,
                                    viewModel = viewModel,
                                    onRemove = { viewModel.removePlugin(pathId, plugin.index) },
                                    onReplace = { onBrowser(pathId) },
                                    expanded = expanded,
                                    onExpandedChange = { expanded = it },
                                    compact = true,
                                    modifier = Modifier.fillMaxWidth(),
                                )
                            }
                        }
                    }
                }
            }
        }
    }
}

@Composable
private fun TransportBar(
    playing: Boolean,
    bpm: Double,
    onPlay: () -> Unit,
    onRestart: () -> Unit,
    onBpm: () -> Unit,
) {
    val accent = MaterialTheme.colorScheme.primary
    Surface(color = LiveColors.raised) {
        Row(
            modifier = Modifier.fillMaxWidth().height(LiveDimensions.transport)
                .padding(horizontal = LiveDimensions.smallGap),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Box(
                modifier = Modifier.size(LiveDimensions.hitTarget).clickable(role = Role.Button, onClick = onPlay),
                contentAlignment = Alignment.Center,
            ) {
                Surface(
                    color = if (playing) accent.copy(alpha = 0.16f) else Color.Transparent,
                    contentColor = accent,
                    shape = RoundedCornerShape(2.dp),
                    modifier = Modifier.size(LiveDimensions.control),
                ) {
                    Box(contentAlignment = Alignment.Center) {
                        Icon(
                            imageVector = if (playing) Icons.Default.Pause else Icons.Default.PlayArrow,
                            contentDescription = if (playing) "Pause transport" else "Play transport",
                            modifier = Modifier.size(LiveDimensions.icon),
                        )
                    }
                }
            }
            IconButton(onClick = onRestart, modifier = Modifier.size(LiveDimensions.hitTarget)) {
                Icon(
                    Icons.Default.SkipPrevious,
                    contentDescription = "Restart transport",
                    tint = LiveColors.textMuted,
                    modifier = Modifier.size(LiveDimensions.icon),
                )
            }
            Text(
                text = if (playing) "PLAYING" else "STOPPED",
                color = if (playing) accent else LiveColors.textMuted,
                style = MaterialTheme.typography.labelSmall,
                fontWeight = FontWeight.Bold,
                modifier = Modifier.padding(start = LiveDimensions.smallGap).weight(1f),
            )
            TextButton(onClick = onBpm, modifier = Modifier.height(LiveDimensions.hitTarget)) {
                Text(
                    "${bpm.roundToInt()} BPM",
                    color = LiveColors.text,
                    style = MaterialTheme.typography.labelMedium,
                    fontWeight = FontWeight.SemiBold,
                )
            }
        }
    }
}

@Composable
private fun Launcher(
    tracks: List<RackTrackInfo>,
    slotsByTrack: Map<Long, List<ClipSlotInfo>>,
    selectedTrack: RackTrackInfo?,
    selectedSlot: Int,
    trackColors: Map<Long, Int>,
    horizontalScrollState: androidx.compose.foundation.ScrollState,
    verticalScrollState: androidx.compose.foundation.ScrollState,
    modifier: Modifier,
    onSelectTrack: (RackTrackInfo) -> Unit,
    onSelect: (RackTrackInfo, Int) -> Unit,
    onLoad: (RackTrackInfo, Int) -> Unit,
    onLaunch: (RackTrackInfo, Int) -> Unit,
    onRecordClip: (RackTrackInfo, Int) -> Unit,
    onTrackColor: (RackTrackInfo, Int) -> Unit,
) {
    Column(modifier = modifier.fillMaxWidth()) {
        Column(
            modifier = Modifier
                .weight(1f)
                .fillMaxWidth()
                .verticalScroll(verticalScrollState)
        ) {
            Row(modifier = Modifier.horizontalScroll(horizontalScrollState)) {
                tracks.forEachIndexed { index, track ->
                    TrackSlots(
                        track = track,
                        index = index,
                        slots = slotsByTrack[track.id].orEmpty(),
                        selected = track.id == selectedTrack?.id,
                        selectedSlot = selectedSlot,
                        trackColor = trackColors[track.id]
                            ?: AppearancePreferences.palettes[
                                index % AppearancePreferences.palettes.size
                            ].argb,
                        onSelectTrack = { onSelectTrack(track) },
                        onSelect = { onSelect(track, it) },
                        onLoad = { onLoad(track, it) },
                        onLaunch = { onLaunch(track, it) },
                        onRecordClip = { onRecordClip(track, it) },
                        onTrackColor = { onTrackColor(track, it) },
                    )
                }
            }
        }
    }
}

@Composable
private fun TrackSlots(
    track: RackTrackInfo,
    index: Int,
    slots: List<ClipSlotInfo>,
    selected: Boolean,
    selectedSlot: Int,
    trackColor: Int,
    onSelectTrack: () -> Unit,
    onSelect: (Int) -> Unit,
    onLoad: (Int) -> Unit,
    onLaunch: (Int) -> Unit,
    onRecordClip: (Int) -> Unit,
    onTrackColor: (Int) -> Unit,
) {
    val visibleCount = maxOf(8, (slots.maxOfOrNull { it.slot + 1 } ?: 0) + 4)
    val bySlot = slots.associateBy { it.slot }
    Box(
        Modifier.requiredWidth(LiveDimensions.trackWidth).background(LiveColors.divider)
            .padding(end = LiveDimensions.hairline),
    ) {
        Column(Modifier.fillMaxWidth().background(LiveColors.panel)) {
            TrackHeader(index, selected, trackColor, onSelectTrack, onTrackColor)
            repeat(visibleCount) { slotIndex ->
                val slot = bySlot[slotIndex] ?: ClipSlotInfo(track.id, slotIndex, false, false, "", 0.0, false)
                ClipCard(
                    track = track,
                    slot = slot,
                    selected = selected && slotIndex == selectedSlot,
                    onSelect = onSelect,
                    onLoad = onLoad,
                    onLaunch = onLaunch,
                    onRecord = onRecordClip,
                )
            }
        }
    }
}

@OptIn(ExperimentalFoundationApi::class)
@Composable
private fun TrackHeader(
    index: Int,
    selected: Boolean,
    trackColor: Int,
    onSelect: () -> Unit,
    onColorSelected: (Int) -> Unit,
) {
    var showPalette by remember { mutableStateOf(false) }
    val color = Color(trackColor)
    Row(
        Modifier.fillMaxWidth().height(LiveDimensions.trackHeader)
            .background(if (selected) color.copy(alpha = 0.16f) else LiveColors.raised)
            .combinedClickable(
                role = Role.Button,
                onClick = onSelect,
                onLongClick = { showPalette = true },
            ),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Box(Modifier.width(3.dp).fillMaxHeight().background(color))
        Text(
            "TRACK ${index + 1}",
            color = if (selected) color else LiveColors.textMuted,
            fontWeight = if (selected) FontWeight.Bold else FontWeight.Medium,
            style = MaterialTheme.typography.labelMedium,
            modifier = Modifier.padding(horizontal = LiveDimensions.gap).weight(1f),
        )
    }
    if (showPalette) {
        AlertDialog(
            onDismissRequest = { showPalette = false },
            title = { Text("Track ${index + 1} color") },
            text = {
                Column(verticalArrangement = Arrangement.spacedBy(LiveDimensions.smallGap)) {
                    AppearancePreferences.palettes.chunked(5).forEach { palettes ->
                        Row(horizontalArrangement = Arrangement.spacedBy(LiveDimensions.smallGap)) {
                            palettes.forEach { palette ->
                                val isSelected = palette.argb == trackColor
                                Box(
                                    modifier = Modifier
                                        .size(LiveDimensions.hitTarget)
                                        .clickable(role = Role.RadioButton) {
                                            onColorSelected(palette.argb)
                                            showPalette = false
                                        }
                                        .semantics {
                                            contentDescription = palette.label
                                            this.selected = isSelected
                                            stateDescription = if (isSelected) "Selected" else "Not selected"
                                        },
                                    contentAlignment = Alignment.Center,
                                ) {
                                    Box(
                                        Modifier
                                            .size(LiveDimensions.control)
                                            .background(Color(palette.argb), CircleShape)
                                            .border(
                                                LiveDimensions.hairline,
                                                if (isSelected) LiveColors.text else LiveColors.divider,
                                                CircleShape,
                                            ),
                                    )
                                }
                            }
                        }
                    }
                }
            },
            confirmButton = {
                TextButton(onClick = { showPalette = false }) { Text("Cancel") }
            },
        )
    }
}

@OptIn(ExperimentalFoundationApi::class)
@Composable
private fun ClipCard(
    track: RackTrackInfo,
    slot: ClipSlotInfo,
    selected: Boolean,
    onSelect: (Int) -> Unit,
    onLoad: (Int) -> Unit,
    onLaunch: (Int) -> Unit,
    onRecord: (Int) -> Unit,
) {
    val filled = slot.wavLoaded || slot.midiLoaded
    val playing = filled && track.playing && slot.active
    val recordingSlot = slot.active || (!filled && selected)
    val recordingPending = recordingSlot && track.recordPending
    val activelyRecording = recordingSlot && track.recording
    val recording = recordingPending || activelyRecording
    val recordAction = !filled && track.inputArmed
    val recordEnabled = recordAction && !track.recordPending && !track.recording
    val clipState = when {
        activelyRecording -> "Recording"
        recordingPending -> "Recording pending"
        playing -> "Playing"
        filled -> "Loaded"
        recordAction -> "Empty, armed for recording"
        else -> "Empty"
    }
    val accent = MaterialTheme.colorScheme.primary
    val typeColor = if (slot.midiLoaded) LiveColors.midi else LiveColors.audio
    val background = when {
        recording -> LiveColors.record.copy(alpha = 0.18f)
        playing -> accent.copy(alpha = 0.18f)
        else -> LiveColors.card
    }
    Surface(
        color = background,
        shape = RectangleShape,
        modifier = Modifier.fillMaxWidth().height(LiveDimensions.slotHeight)
            .semantics {
                this.selected = selected
                stateDescription = clipState
            }
            .clickable(role = Role.Button) { onSelect(slot.slot) },
    ) {
        Box {
            Row(
                Modifier.fillMaxSize().padding(start = LiveDimensions.smallGap),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Box(
                    Modifier.width(2.dp).height(28.dp).background(
                        when {
                            recording -> LiveColors.record
                            playing -> accent
                            filled -> typeColor
                            recordAction -> LiveColors.record.copy(alpha = 0.6f)
                            selected -> accent.copy(alpha = 0.55f)
                            else -> LiveColors.divider
                        },
                    ),
                )
                Text(
                    text = if (filled) slot.displayName else "${slot.slot + 1}",
                    color = when {
                        recording || recordAction -> LiveColors.record
                        playing || selected -> accent
                        filled -> LiveColors.text
                        else -> LiveColors.textDim
                    },
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                    style = MaterialTheme.typography.labelMedium,
                    fontWeight = if (playing || selected || recording) FontWeight.SemiBold else FontWeight.Normal,
                    modifier = Modifier.padding(start = LiveDimensions.smallGap).weight(1f),
                )
                IconButton(
                    onClick = {
                        when {
                            filled -> onLaunch(slot.slot)
                            recordAction -> onRecord(slot.slot)
                            else -> onLoad(slot.slot)
                        }
                    },
                    enabled = !recordAction || recordEnabled,
                    modifier = Modifier.size(LiveDimensions.hitTarget),
                ) {
                    Icon(
                        imageVector = when {
                            filled && playing -> Icons.Default.Pause
                            filled -> Icons.Default.PlayArrow
                            recordAction -> Icons.Default.FiberManualRecord
                            else -> Icons.Default.Add
                        },
                        contentDescription = when {
                            filled && playing -> "Stop ${slot.displayName}"
                            filled -> "Launch ${slot.displayName}"
                            activelyRecording -> "Recording into slot ${slot.slot + 1}"
                            recordingPending -> "Recording pending in slot ${slot.slot + 1}"
                            recordAction -> "Record into slot ${slot.slot + 1}"
                            else -> "Load clip into slot ${slot.slot + 1}"
                        },
                        tint = when {
                            recordAction || recording -> LiveColors.record
                            playing -> accent
                            filled -> LiveColors.textMuted
                            else -> LiveColors.textDim
                        },
                        modifier = Modifier.size(16.dp),
                    )
                }
            }
            Box(
                Modifier.align(Alignment.BottomCenter).fillMaxWidth().height(LiveDimensions.hairline)
                    .background(LiveColors.divider),
            )
        }
    }
}

@Composable
private fun ClipInspector(
    track: RackTrackInfo?,
    clip: ClipSlotInfo?,
    peaks: List<Float>,
    notes: List<MidiNoteInfo>,
    tab: InspectorTab,
    onTab: (InspectorTab) -> Unit,
    onTrackVolume: (RackTrackInfo, Float) -> Unit,
    onTrackInput: (RackTrackInfo) -> Unit,
    onTrackArm: (RackTrackInfo) -> Unit,
    onTrackDelete: (RackTrackInfo) -> Unit,
    onTrackRecordMenu: (RackTrackInfo) -> Unit,
    launchQuantization: TrackLaunchQuantization,
    onLaunchQuantizationClick: () -> Unit,
    modifier: Modifier,
) {
    Surface(color = LiveColors.panel, modifier = modifier.fillMaxWidth()) {
        Column {
            Row(Modifier.fillMaxWidth().height(36.dp).background(LiveColors.raised)) {
                InspectorTab.entries.forEach { item ->
                    val selected = tab == item
                    Box(
                        modifier = Modifier.weight(1f).fillMaxHeight()
                            .semantics { this.selected = selected }
                            .clickable(role = Role.Tab) { onTab(item) },
                        contentAlignment = Alignment.Center,
                    ) {
                        Text(
                            item.name.uppercase(),
                            color = if (selected) MaterialTheme.colorScheme.primary else LiveColors.textMuted,
                            style = MaterialTheme.typography.labelSmall,
                            fontWeight = if (selected) FontWeight.SemiBold else FontWeight.Normal,
                        )
                        if (selected) {
                            Box(
                                Modifier.align(Alignment.BottomCenter).fillMaxWidth()
                                    .height(2.dp).background(MaterialTheme.colorScheme.primary),
                            )
                        }
                    }
                }
            }
            Box(Modifier.fillMaxWidth().weight(1f)) {
                when (tab) {
                    InspectorTab.Clips -> when {
                        clip?.midiLoaded == true -> PianoRoll(clip, notes)
                        else -> Waveform(track, clip, peaks)
                    }
                    InspectorTab.Track -> TrackInspectorControls(
                        track = track,
                        onVolumeChange = { value -> track?.let { onTrackVolume(it, value) } },
                        onInputClick = { track?.let(onTrackInput) },
                        onArmClick = { track?.let(onTrackArm) },
                        onDeleteClick = { track?.let(onTrackDelete) },
                        onRecordOptionsClick = { track?.let(onTrackRecordMenu) },
                        launchQuantization = launchQuantization,
                        onLaunchQuantizationClick = onLaunchQuantizationClick,
                    )
                }
            }
        }
    }
}

@Composable
private fun TrackInspectorControls(
    track: RackTrackInfo?,
    onVolumeChange: (Float) -> Unit,
    onInputClick: () -> Unit,
    onArmClick: () -> Unit,
    onDeleteClick: () -> Unit,
    onRecordOptionsClick: () -> Unit,
    launchQuantization: TrackLaunchQuantization,
    onLaunchQuantizationClick: () -> Unit,
) {
    if (track == null) {
        InspectorMessage("Select a track")
        return
    }
    val launchQuantizationLabel = when (launchQuantization) {
        TrackLaunchQuantization.Bar -> "1 BAR"
        TrackLaunchQuantization.Quarter -> "1/4"
        TrackLaunchQuantization.Eighth -> "1/8"
        TrackLaunchQuantization.Sixteenth -> "1/16"
        TrackLaunchQuantization.None -> "OFF"
    }
    var showMore by remember { mutableStateOf(false) }
    Row(
        modifier = Modifier.fillMaxSize().padding(horizontal = LiveDimensions.smallGap),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(LiveDimensions.smallGap),
    ) {
        TextButton(
            onClick = onArmClick,
            modifier = Modifier.height(40.dp).semantics {
                stateDescription = if (track.inputArmed) "Armed" else "Disarmed"
            },
        ) {
            Icon(
                Icons.Default.Mic,
                contentDescription = if (track.inputArmed) "Disarm track input" else "Arm track input",
                tint = if (track.inputArmed) LiveColors.record else LiveColors.textDim,
                modifier = Modifier.size(LiveDimensions.icon),
            )
            Text(
                if (track.inputArmed) "ARMED" else "ARM",
                color = if (track.inputArmed) LiveColors.record else LiveColors.textMuted,
                style = MaterialTheme.typography.labelSmall,
                modifier = Modifier.padding(start = LiveDimensions.smallGap),
            )
        }
        TextButton(
            onClick = onInputClick,
            modifier = Modifier.height(40.dp),
        ) {
            Text("IN ${track.inputChannel + 1}", style = MaterialTheme.typography.labelSmall)
        }
        Column(modifier = Modifier.weight(1f)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text(
                    "VOLUME",
                    color = LiveColors.textDim,
                    style = MaterialTheme.typography.labelSmall,
                    modifier = Modifier.weight(1f),
                )
                Text(
                    "${(track.volume * 100).roundToInt()}%",
                    color = MaterialTheme.colorScheme.primary,
                    style = MaterialTheme.typography.labelSmall,
                )
            }
            TrackVolumeFader(
                value = track.volume,
                onValueChange = onVolumeChange,
                modifier = Modifier.fillMaxWidth().height(32.dp),
            )
        }
        Box {
            IconButton(
                onClick = { showMore = true },
                modifier = Modifier.size(LiveDimensions.hitTarget),
            ) {
                Icon(
                    Icons.Default.MoreVert,
                    contentDescription = "More track options",
                    tint = LiveColors.textMuted,
                    modifier = Modifier.size(LiveDimensions.icon),
                )
            }
            DropdownMenu(expanded = showMore, onDismissRequest = { showMore = false }) {
                DropdownMenuItem(
                    text = { Text("Launch quantize · $launchQuantizationLabel") },
                    onClick = {
                        showMore = false
                        onLaunchQuantizationClick()
                    },
                )
                DropdownMenuItem(
                    text = { Text("Loop length / enter on punch") },
                    onClick = {
                        showMore = false
                        onRecordOptionsClick()
                    },
                )
                DropdownMenuItem(
                    text = { Text("Delete track") },
                    onClick = {
                        showMore = false
                        onDeleteClick()
                    },
                )
            }
        }
    }
}

@Composable
private fun TrackVolumeFader(
    value: Float,
    onValueChange: (Float) -> Unit,
    modifier: Modifier = Modifier,
) {
    val accent = MaterialTheme.colorScheme.primary
    val currentOnValueChange by rememberUpdatedState(onValueChange)
    Canvas(
        modifier = modifier
            .semantics {
                contentDescription = "Track volume"
                progressBarRangeInfo = ProgressBarRangeInfo(value.coerceIn(0f, 1f), 0f..1f)
                setProgress { target ->
                    currentOnValueChange(target.coerceIn(0f, 1f))
                    true
                }
            }
            .pointerInput(Unit) {
                detectTapGestures { offset ->
                    val inset = 8.dp.toPx()
                    currentOnValueChange(horizontalFaderValue(offset.x, inset, size.width - inset))
                }
            }
            .pointerInput(Unit) {
                detectHorizontalDragGestures(
                    onDragStart = { offset ->
                        val inset = 8.dp.toPx()
                        currentOnValueChange(horizontalFaderValue(offset.x, inset, size.width - inset))
                    },
                    onHorizontalDrag = { change, _ ->
                        val inset = 8.dp.toPx()
                        currentOnValueChange(horizontalFaderValue(change.position.x, inset, size.width - inset))
                    },
                )
            },
    ) {
        val inset = 8.dp.toPx()
        val start = inset
        val end = size.width - inset
        val y = size.height / 2f
        val x = start + (end - start) * value.coerceIn(0f, 1f)
        drawLine(LiveColors.divider, Offset(start, y), Offset(end, y), 2.dp.toPx(), StrokeCap.Round)
        drawLine(accent, Offset(start, y), Offset(x, y), 2.dp.toPx(), StrokeCap.Round)
        drawCircle(accent, radius = 5.dp.toPx(), center = Offset(x, y))
    }
}

private fun horizontalFaderValue(x: Float, start: Float, end: Float): Float =
    if (end <= start) 0f else ((x - start) / (end - start)).coerceIn(0f, 1f)

@Composable
private fun InspectorMessage(message: String) {
    Box(
        modifier = Modifier.fillMaxSize().padding(horizontal = LiveDimensions.smallGap),
        contentAlignment = Alignment.CenterStart,
    ) {
        Text(message, color = LiveColors.textMuted, style = MaterialTheme.typography.bodySmall)
    }
}

@Composable
private fun Waveform(track: RackTrackInfo?, clip: ClipSlotInfo?, peaks: List<Float>) {
    if (track == null || clip?.wavLoaded != true) {
        InspectorMessage("Select an audio or MIDI clip to inspect it")
        return
    }
    val accent = MaterialTheme.colorScheme.primary
    Canvas(modifier = Modifier.fillMaxSize().padding(horizontal = LiveDimensions.smallGap)) {
        val mid = size.height / 2
        drawLine(LiveColors.waveformLine, Offset(0f, mid), Offset(size.width, mid), 1f)
        if (peaks.isNotEmpty()) {
            val step = size.width / peaks.size
            peaks.forEachIndexed { index, peak ->
                val half = (peak * mid).coerceAtLeast(1f)
                val x = index * step + step / 2
                drawLine(
                    LiveColors.audio,
                    Offset(x, mid - half),
                    Offset(x, mid + half),
                    step.coerceAtLeast(1f),
                    StrokeCap.Butt,
                )
            }
        }
        if (track.playing) {
            val x = size.width * (track.positionSec / clip.durationSec).toFloat().coerceIn(0f, 1f)
            drawLine(accent, Offset(x, 0f), Offset(x, size.height), 2f)
        }
    }
}

@Composable
private fun PianoRoll(clip: ClipSlotInfo, notes: List<MidiNoteInfo>) {
    Canvas(modifier = Modifier.fillMaxSize().padding(horizontal = LiveDimensions.smallGap)) {
        val rows = 12
        repeat(rows + 1) { row ->
            val y = size.height * row / rows
            drawLine(LiveColors.divider, Offset(0f, y), Offset(size.width, y), 1f)
        }
        val duration = (clip.durationSec * 48_000).coerceAtLeast(1.0)
        notes.forEach { note ->
            val x = (note.startFrame / duration).toFloat().coerceIn(0f, 1f) * size.width
            val width = ((note.durationFrames.coerceAtLeast(24_000) / duration).toFloat() * size.width).coerceAtLeast(4f)
            val y = size.height - ((note.pitch.coerceIn(48, 83) - 47) / 36f) * size.height
            drawRect(
                LiveColors.midi,
                Offset(x, y - size.height / rows + 2f),
                Size(width, size.height / rows - 4f),
            )
        }
    }
}

@Composable
private fun StatusStrip(
    engineRunning: Boolean,
    meterState: com.vibes.dsp.ui.rack.MeterState,
    latencyMs: Double,
    cpuLoad: Float,
    xRunCount: Int,
    usbState: DirectUsbSessionState,
    usbStats: com.vibes.dsp.engine.DirectUsbStats,
    errorMessage: String?,
    blockingOperation: String?,
    onToggleEngine: () -> Unit,
    onResetClipping: () -> Unit,
    onClearError: () -> Unit,
) {
    val accent = MaterialTheme.colorScheme.primary
    Surface(color = LiveColors.panel, modifier = Modifier.fillMaxWidth()) {
        Column(Modifier.padding(horizontal = LiveDimensions.smallGap)) {
            Row(
                modifier = Modifier.fillMaxWidth().height(LiveDimensions.hitTarget)
                    .horizontalScroll(rememberScrollState()),
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(LiveDimensions.smallGap),
            ) {
                Box(
                    modifier = Modifier.height(LiveDimensions.hitTarget).widthIn(min = 68.dp)
                        .clickable(role = Role.Button, onClick = onToggleEngine),
                    contentAlignment = Alignment.Center,
                ) {
                    Surface(
                        color = if (engineRunning) accent.copy(alpha = 0.16f) else LiveColors.card,
                        contentColor = if (engineRunning) accent else LiveColors.textMuted,
                        shape = RoundedCornerShape(2.dp),
                        modifier = Modifier.height(LiveDimensions.control).fillMaxWidth(),
                    ) {
                        Box(contentAlignment = Alignment.Center) {
                            Text(
                                if (engineRunning) "ENGINE ON" else "ENGINE OFF",
                                style = MaterialTheme.typography.labelSmall,
                                fontWeight = FontWeight.Bold,
                            )
                        }
                    }
                }
                Text(
                    "IN ${(meterState.inputLevel * 100).roundToInt()}",
                    color = if (meterState.inputClipping) LiveColors.record else accent,
                    style = MaterialTheme.typography.labelSmall,
                )
                Text(
                    "OUT ${(meterState.outputLevel * 100).roundToInt()}",
                    color = if (meterState.outputClipping) LiveColors.record else accent,
                    style = MaterialTheme.typography.labelSmall,
                )
                TextButton(onClick = onResetClipping, modifier = Modifier.height(LiveDimensions.hitTarget)) {
                    Text("RESET", style = MaterialTheme.typography.labelSmall)
                }
                Text(
                    "${latencyMs.roundToInt()}ms · CPU ${(cpuLoad * 100).roundToInt()}% · XR $xRunCount",
                    color = LiveColors.textMuted,
                    style = MaterialTheme.typography.labelSmall,
                )
                Text(
                    "USB ${usbState.name} · ${usbStats.sampleRateHz}Hz",
                    color = LiveColors.textDim,
                    style = MaterialTheme.typography.labelSmall,
                )
            }
            blockingOperation?.let {
                Text(
                    "BUSY · $it",
                    color = accent,
                    style = MaterialTheme.typography.labelSmall,
                    modifier = Modifier.padding(horizontal = LiveDimensions.smallGap, vertical = 2.dp),
                )
            }
            errorMessage?.let {
                Row(
                    modifier = Modifier.fillMaxWidth().height(LiveDimensions.hitTarget),
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    Text(
                        it,
                        color = LiveColors.error,
                        style = MaterialTheme.typography.bodySmall,
                        modifier = Modifier.weight(1f),
                        maxLines = 1,
                        overflow = TextOverflow.Ellipsis,
                    )
                    TextButton(onClick = onClearError, modifier = Modifier.height(LiveDimensions.hitTarget)) {
                        Text("CLEAR", color = LiveColors.error, style = MaterialTheme.typography.labelSmall)
                    }
                }
            }
        }
    }
}

@Composable
private fun Mixer(
    tracks: List<RackTrackInfo>,
    selected: RackPathId?,
    onSelect: (RackTrackInfo) -> Unit,
    onVolume: (RackTrackInfo, Float) -> Unit,
    onAdd: () -> Unit,
    modifier: Modifier,
    scrollState: androidx.compose.foundation.ScrollState,
) {
    Surface(color = LiveColors.raised, modifier = modifier.fillMaxWidth()) {
        Row(
            modifier = Modifier.fillMaxSize().horizontalScroll(scrollState),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            tracks.forEachIndexed { index, track ->
                MixerChannel(
                    track = track,
                    index = index,
                    selected = track.id == selected,
                    onSelect = { onSelect(track) },
                    onVolume = { onVolume(track, it) },
                )
            }
            MasterChannel()
            IconButton(onClick = onAdd, modifier = Modifier.size(LiveDimensions.hitTarget)) {
                Icon(
                    Icons.Default.Add,
                    contentDescription = "Add track",
                    tint = LiveColors.textMuted,
                    modifier = Modifier.size(LiveDimensions.icon),
                )
            }
        }
    }
}

@Composable
private fun MixerChannel(
    track: RackTrackInfo,
    index: Int,
    selected: Boolean,
    onSelect: () -> Unit,
    onVolume: (Float) -> Unit,
) {
    val accent = MaterialTheme.colorScheme.primary
    Column(
        modifier = Modifier.width(LiveDimensions.mixerChannel).fillMaxHeight()
            .background(if (selected) accent.copy(alpha = 0.12f) else LiveColors.panel)
            .drawBehind {
                drawLine(
                    color = LiveColors.divider,
                    start = Offset(size.width - 1.dp.toPx(), 0f),
                    end = Offset(size.width - 1.dp.toPx(), size.height),
                    strokeWidth = 1.dp.toPx(),
                )
            }
            .semantics { this.selected = selected }
            .clickable(role = Role.Button, onClick = onSelect)
            .padding(vertical = LiveDimensions.smallGap),
        horizontalAlignment = Alignment.CenterHorizontally,
    ) {
        Row(
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(LiveDimensions.smallGap),
        ) {
            Box(
                modifier = Modifier.width(2.dp).height(16.dp)
                    .background(if (track.playing) accent else LiveColors.divider),
            )
            Text(
                text = "T${index + 1}",
                color = if (selected || track.playing) accent else LiveColors.text,
                fontWeight = if (selected) FontWeight.Bold else FontWeight.Medium,
                style = MaterialTheme.typography.labelMedium,
            )
        }
        VerticalFader(
            value = track.volume,
            onValueChange = onVolume,
            label = "Track ${index + 1} volume",
            modifier = Modifier.fillMaxWidth().weight(1f),
        )
        Text(
            "${(track.volume.coerceIn(0f, 1f) * 100).roundToInt()}%",
            color = if (selected) accent else LiveColors.textMuted,
            style = MaterialTheme.typography.labelSmall,
        )
    }
}

@Composable
private fun MasterChannel() {
    Column(
        modifier = Modifier.width(LiveDimensions.mixerChannel).fillMaxHeight()
            .background(LiveColors.panel)
            .drawBehind {
                drawLine(
                    color = LiveColors.divider,
                    start = Offset(size.width - 1.dp.toPx(), 0f),
                    end = Offset(size.width - 1.dp.toPx(), size.height),
                    strokeWidth = 1.dp.toPx(),
                )
            }
            .padding(vertical = LiveDimensions.smallGap),
        horizontalAlignment = Alignment.CenterHorizontally,
    ) {
        Text(
            "MASTER",
            color = LiveColors.text,
            style = MaterialTheme.typography.labelSmall,
            fontWeight = FontWeight.Bold,
        )
        VerticalFader(
            value = 1f,
            onValueChange = {},
            label = "Master volume",
            enabled = false,
            modifier = Modifier.fillMaxWidth().weight(1f),
        )
        Text("UNITY", color = LiveColors.textMuted, style = MaterialTheme.typography.labelSmall)
    }
}

private fun verticalFaderValue(y: Float, railTop: Float, railBottom: Float): Float {
    if (railBottom <= railTop) return if (y <= railTop) 1f else 0f
    return ((railBottom - y) / (railBottom - railTop)).coerceIn(0f, 1f)
}

@Composable
private fun VerticalFader(
    value: Float,
    onValueChange: (Float) -> Unit,
    label: String,
    modifier: Modifier = Modifier,
    enabled: Boolean = true,
) {
    val level = value.coerceIn(0f, 1f)
    val accent = MaterialTheme.colorScheme.primary
    val currentOnValueChange by rememberUpdatedState(onValueChange)
    Canvas(
        modifier = modifier
            .semantics {
                contentDescription = label
                if (!enabled) disabled()
                progressBarRangeInfo = ProgressBarRangeInfo(level, 0f..1f)
                setProgress { target ->
                    if (!enabled) {
                        false
                    } else {
                        currentOnValueChange(target.coerceIn(0f, 1f))
                        true
                    }
                }
            }
            .pointerInput(enabled) {
                if (enabled) {
                    detectTapGestures { position ->
                        val railInset = 8.dp.toPx()
                        currentOnValueChange(
                            verticalFaderValue(
                                y = position.y,
                                railTop = railInset,
                                railBottom = size.height - railInset,
                            ),
                        )
                    }
                }
            }
            .pointerInput(enabled) {
                if (enabled) {
                    detectVerticalDragGestures { change, _ ->
                        change.consume()
                        val railInset = 8.dp.toPx()
                        currentOnValueChange(
                            verticalFaderValue(
                                y = change.position.y,
                                railTop = railInset,
                                railBottom = size.height - railInset,
                            ),
                        )
                    }
                }
            },
    ) {
        val railTop = 8.dp.toPx()
        val railBottom = size.height - 8.dp.toPx()
        val railX = size.width / 2f
        val thumbY = railBottom - (railBottom - railTop) * level
        drawLine(
            color = LiveColors.divider,
            start = Offset(railX, railTop),
            end = Offset(railX, railBottom),
            strokeWidth = 2.dp.toPx(),
            cap = StrokeCap.Round,
        )
        drawLine(
            color = if (enabled) accent else LiveColors.textDim,
            start = Offset(railX, thumbY),
            end = Offset(railX, railBottom),
            strokeWidth = 2.dp.toPx(),
            cap = StrokeCap.Round,
        )
        drawRect(
            color = if (enabled) accent else LiveColors.textMuted,
            topLeft = Offset(railX - 9.dp.toPx(), thumbY - 2.dp.toPx()),
            size = Size(18.dp.toPx(), 4.dp.toPx()),
        )
    }
}

