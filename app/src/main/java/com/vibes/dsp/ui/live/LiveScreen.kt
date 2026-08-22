package com.vibes.dsp.ui.live

import android.app.Activity
import android.content.pm.ActivityInfo
import android.net.Uri
import android.os.Build
import android.widget.Toast
import androidx.activity.compose.BackHandler
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.ExperimentalFoundationApi
import androidx.compose.foundation.LocalOverscrollConfiguration
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
import androidx.compose.material.icons.filled.Lock
import androidx.compose.material.icons.filled.Mic
import androidx.compose.material.icons.filled.Pause
import androidx.compose.material.icons.filled.PlayArrow
import androidx.compose.material.icons.filled.Repeat
import androidx.compose.material.icons.filled.Settings
import androidx.compose.material.icons.filled.SkipPrevious
import androidx.compose.material.icons.filled.Stop
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
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.runtime.DisposableEffect
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
import androidx.compose.ui.unit.sp
import com.vibes.dsp.engine.ClipSlotInfo
import com.vibes.dsp.engine.ClipTempoMode
import com.vibes.dsp.engine.DirectUsbAudioManager
import com.vibes.dsp.engine.MidiNoteInfo
import com.vibes.dsp.engine.MASTER_PATH_ID
import com.vibes.dsp.engine.RackPathId
import com.vibes.dsp.engine.RackTrackInfo
import com.vibes.dsp.engine.TrackLaunchQuantization
import com.vibes.dsp.ui.rack.PluginCard
import com.vibes.dsp.ui.rack.RackViewModel
import com.vibes.dsp.ui.rack.RackPlugin
import com.vibes.dsp.ui.theme.AppearancePreferences
import kotlin.math.roundToInt


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
    val trackHeader = 40.dp
    val slotHeight = 48.dp
    val mixerChannel = 64.dp
    val pluginMinWidth = 216.dp
    val pluginMaxWidth = 288.dp
    val icon = 18.dp
    val indicatorIcon = 12.dp
    val gap = 8.dp
    val smallGap = 4.dp
    val hairline = 1.dp
}

private fun emptyClipSlot(track: RackTrackInfo, slot: Int) = ClipSlotInfo(
    trackId = track.id,
    slot = slot,
    wavLoaded = false,
    midiLoaded = false,
    displayName = "",
    durationSec = 0.0,
    active = false,
    playing = false,
    looping = false,
    positionSec = 0.0,
    transportFrame = 0L,
    loopLengthBars = track.defaultLoopLengthBars,
    enterOnPunch = false,
)

internal data class LiveFullscreenRequest(
    val instanceId: Long,
    val pathId: Long,
    val width: Int,
    val height: Int,
)

internal fun resolveLiveFullscreenPlugin(
    request: LiveFullscreenRequest,
    selectedPathId: Long,
    plugins: List<RackPlugin>,
): RackPlugin? = if (request.pathId == selectedPathId) {
    plugins.firstOrNull { it.instanceId == request.instanceId }
} else {
    null
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
    val latencyMs by viewModel.latencyMs.collectAsState()
    val cpuLoad by viewModel.cpuLoad.collectAsState()
    val xRunCount by viewModel.xRunCount.collectAsState()
    val errorMessage by viewModel.errorMessage.collectAsState()
    val blockingOperation by viewModel.blockingOperation.collectAsState()
    val slotsByTrack by viewModel.clipSlots.collectAsState()
    val peaksByTrack by viewModel.waveformPeaks.collectAsState()
    val notesByClip by viewModel.midiNotes.collectAsState()
    val inputChannelCount = DirectUsbAudioManager.getInputChannelCount()

    var launchQuantizationOrdinal by rememberSaveable { mutableIntStateOf(0) }
    val launchQuantization = TrackLaunchQuantization.entries[launchQuantizationOrdinal]
    var recordMenuTarget by remember { mutableStateOf<Pair<Long, Int>?>(null) }
    var showTempoDialog by rememberSaveable { mutableStateOf(false) }
    var tempoInput by rememberSaveable { mutableStateOf("") }
    var selectedTrackId by rememberSaveable { mutableLongStateOf(0L) }
    var selectedSlot by rememberSaveable { mutableIntStateOf(0) }
    var importTarget by remember { mutableStateOf<Pair<Long, Int>?>(null) }
    var inputMenuTrack by remember { mutableStateOf<RackTrackInfo?>(null) }
    var trackColorOverrides by remember { mutableStateOf<Map<Long, Int>>(emptyMap()) }
    var showQuantizationMenu by remember { mutableStateOf(false) }
    var editTiles by rememberSaveable { mutableStateOf(false) }
    var tileOrder by rememberSaveable { mutableStateOf(LiveLayoutPreferences.getTileOrder(context)) }
    var visibleTiles by rememberSaveable { mutableStateOf(LiveLayoutPreferences.getVisibleTiles(context)) }
    val horizontalPlugins = remember { LiveLayoutPreferences.getHorizontalPlugins(context) }
    val fitTilesOnScreen = remember { LiveLayoutPreferences.getFitTilesOnScreen(context) }
    val hideTransportWithoutLauncher = remember {
        LiveLayoutPreferences.getHideTransportWithoutLauncher(context)
    }
    var tileHeights by remember {
        mutableStateOf(tileOrder.associateWith { id -> LiveLayoutPreferences.getTileHeight(context, id) })
    }
    var fullscreenPluginInstanceId by rememberSaveable { mutableStateOf<Long?>(null) }
    var fullscreenPluginPathId by rememberSaveable { mutableStateOf<Long?>(null) }
    var fullscreenPluginWidth by rememberSaveable { mutableStateOf<Int?>(null) }
    var fullscreenPluginHeight by rememberSaveable { mutableStateOf<Int?>(null) }
    val fullscreenRequest = fullscreenPluginInstanceId?.let { instanceId ->
        fullscreenPluginPathId?.let { pathId ->
            fullscreenPluginWidth?.let { width ->
                fullscreenPluginHeight?.let { height ->
                    LiveFullscreenRequest(instanceId, pathId, width, height)
                }
            }
        }
    }
    fun exitFullscreen() {
        fullscreenPluginInstanceId = null
        fullscreenPluginPathId = null
        fullscreenPluginWidth = null
        fullscreenPluginHeight = null
    }
    val supportedLoopLengths = listOf(
        0.25 to "1/4", 1.0 to "1 bar", 2.0 to "2 bars", 4.0 to "4 bars",
        8.0 to "8 bars", 16.0 to "16 bars",
    )
    val launcherHorizontalScrollState = rememberScrollState()
    val launcherVerticalScrollState = rememberScrollState()
    val mixerScrollState = rememberScrollState()
    val contentScrollState = rememberScrollState()
    fun armTrackExclusivelyIfEnabled(trackId: Long) {
        if (LiveLayoutPreferences.getArmExclusiveOnTrackSelection(context)) {
            viewModel.armTrackExclusively(trackId)
        }
    }


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
                viewModel.loadTrackClipMedia(trackId, slot, uri)
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
    LaunchedEffect(errorMessage) {
        val message = errorMessage ?: return@LaunchedEffect
        Toast.makeText(context, message, Toast.LENGTH_LONG).show()
        viewModel.clearError()
    }
    LaunchedEffect(blockingOperation) {
        val operation = blockingOperation ?: return@LaunchedEffect
        Toast.makeText(context, operation, Toast.LENGTH_SHORT).show()
    }

    val fullscreenPlugin = fullscreenRequest?.let { request ->
        resolveLiveFullscreenPlugin(request, selectedPath, selectedPlugins)
    }
    val activity = context as? Activity
    LaunchedEffect(fullscreenRequest) {
        if (fullscreenRequest != null &&
            fullscreenRequest.width > 0 &&
            fullscreenRequest.height > 0
        ) {
            if (fullscreenRequest.width > fullscreenRequest.height * 1.3) {
                activity?.requestedOrientation = ActivityInfo.SCREEN_ORIENTATION_USER_LANDSCAPE
            }
        } else if (fullscreenRequest == null) {
            activity?.requestedOrientation = ActivityInfo.SCREEN_ORIENTATION_PORTRAIT
        }
    }
    DisposableEffect(Unit) {
        onDispose { activity?.requestedOrientation = ActivityInfo.SCREEN_ORIENTATION_PORTRAIT }
    }
    BackHandler(enabled = fullscreenRequest != null) {
        exitFullscreen()
    }
    LaunchedEffect(fullscreenRequest, fullscreenPlugin) {
        if (fullscreenRequest != null && fullscreenPlugin == null) {
            exitFullscreen()
        }
    }

    if (fullscreenRequest != null) {
        BoxWithConstraints(
            modifier = Modifier.fillMaxSize().background(Color.Black),
        ) {
            fullscreenPlugin?.let { plugin ->
                key(plugin.instanceId) {
                    PluginCard(
                        plugin = plugin,
                        pluginIndex = plugin.index,
                        pathId = fullscreenRequest.pathId,
                        viewModel = viewModel,
                        onRemove = {
                            viewModel.removePlugin(fullscreenRequest.pathId, plugin.index)
                        },
                        onReplace = {
                            onNavigateToBrowser(fullscreenRequest.pathId, plugin.index)
                        },
                        isFullscreen = true,
                        isAnyPluginFullscreen = true,
                        isRackVisible = true,
                        screenHeight = maxHeight,
                        onExitFullscreen = ::exitFullscreen,
                        onNavigateToTone3000 = onNavigateToTone3000,
                        compact = true,
                        modifier = Modifier.fillMaxSize(),
                    )
                }
            }
        }
        return
    }

    val selectedTrack = tracks.firstOrNull { it.id == selectedTrackId } ?: tracks.firstOrNull()
    val selectedClip = selectedTrack?.let { track ->
        slotsByTrack[track.id]?.firstOrNull { it.slot == selectedSlot }
            ?: emptyClipSlot(track, selectedSlot)
    }
    LaunchedEffect(selectedTrack?.id) {
        if (selectedTrack != null && selectedTrack.id != selectedTrackId) {
            selectedTrackId = selectedTrack.id
            selectedSlot = selectedTrack.selectedSlot
        }
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
    recordMenuTarget?.let { (trackId, slotIndex) ->
        val track = tracks.firstOrNull { it.id == trackId } ?: return@let
        val clip = slotsByTrack[trackId]?.firstOrNull { it.slot == slotIndex }
            ?: emptyClipSlot(track, slotIndex)
        AlertDialog(
            onDismissRequest = { recordMenuTarget = null },
            title = { Text("Clip options") },
            text = {
                Column(Modifier.verticalScroll(rememberScrollState())) {
                    Text("Default loop length for new slots")
                    supportedLoopLengths.forEach { (bars, label) ->
                        Row(
                            Modifier.fillMaxWidth().clickable {
                                viewModel.setTrackDefaultLoopLength(track.id, bars)
                            },
                            verticalAlignment = Alignment.CenterVertically,
                        ) {
                            RadioButton(
                                selected = track.defaultLoopLengthBars == bars,
                                onClick = { viewModel.setTrackDefaultLoopLength(track.id, bars) },
                            )
                            Text(label)
                        }
                    }
                    Text("Selected slot loop length")
                    supportedLoopLengths.forEach { (bars, label) ->
                        Row(
                            Modifier.fillMaxWidth().clickable {
                                viewModel.setClipLoopLength(track.id, slotIndex, bars)
                            },
                            verticalAlignment = Alignment.CenterVertically,
                        ) {
                            RadioButton(
                                selected = clip.loopLengthBars == bars,
                                onClick = { viewModel.setClipLoopLength(track.id, slotIndex, bars) },
                            )
                            Text(label)
                        }
                    }
                    if (clip.wavLoaded) {
                        Text("WAV tempo mode")
                        val currentMode = ClipTempoMode.entries.getOrElse(clip.tempoMode) { ClipTempoMode.Original }
                        ClipTempoMode.entries.forEach { mode ->
                            Row(
                                Modifier.fillMaxWidth().clickable {
                                    viewModel.setClipTempoMode(track.id, slotIndex, mode)
                                },
                                verticalAlignment = Alignment.CenterVertically,
                            ) {
                                RadioButton(
                                    selected = currentMode == mode,
                                    onClick = {
                                        viewModel.setClipTempoMode(track.id, slotIndex, mode)
                                    },
                                )
                                Text(mode.name)
                            }
                        }
                    }
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        Checkbox(
                            checked = clip.enterOnPunch,
                            enabled = clip.enterOnPunch ||
                                !transport.playing ||
                                launchQuantization == TrackLaunchQuantization.None,
                            onCheckedChange = {
                                viewModel.setClipEnterOnPunch(
                                    track.id,
                                    slotIndex,
                                    it,
                                    launchQuantization,
                                )
                            },
                        )
                        Text("Enter on punch")
                    }
                }
            },
            confirmButton = { TextButton(onClick = { recordMenuTarget = null }) { Text("Done") } },
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
        floatingActionButtonPosition = androidx.compose.material3.FabPosition.Center,
        floatingActionButton = {
            if (!engineRunning) {
                EngineControlButton(onClick = viewModel::startEngine)
            }
        },
    ) { contentPadding ->
        Column(Modifier.fillMaxSize().padding(contentPadding)) {
            CameraToolbar(
                visibleTiles = visibleTiles,
                editMode = editTiles,
                onToggleTile = ::toggleTile,
                onSettings = onNavigateToSettings,
                onToggleEdit = { editTiles = !editTiles },
            )
            if (!hideTransportWithoutLauncher || "launcher" in visibleTiles) {
                TransportBar(
                    playing = transport.playing,
                    positionSec = transport.positionSec,
                    bpm = transport.beatsPerMinute,
                    onPlay = {
                        if (transport.playing) viewModel.transportPause() else viewModel.transportPlay()
                    },
                    onStop = viewModel::transportStop,
                    onRestart = viewModel::transportRestart,
                    onBpm = {
                        tempoInput = transport.beatsPerMinute.toString()
                        showTempoDialog = true
                    },
                )
            }
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
                    val inspectorCollapsed =
                        tileId == "inspector" && selectedTrack == null && !editTiles
                    TileContainer(
                        id = tileId,
                        modifier = when {
                            inspectorCollapsed -> Modifier.height(
                                LiveDimensions.hitTarget + LiveDimensions.gap + LiveDimensions.hairline,
                            )
                            fitTilesOnScreen -> Modifier.weight(currentHeight.coerceAtLeast(1f))
                            else -> Modifier.height(currentHeight.dp)
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
                                        val slot = track.selectedSlot
                                        selectedTrackId = track.id
                                        armTrackExclusivelyIfEnabled(track.id)
                                        selectedSlot = slot
                                        viewModel.selectPath(track.id)
                                        viewModel.selectTrackClipSlot(track.id, slot)
                                    },
                                    onSelect = { track, slot ->
                                        selectedTrackId = track.id
                                        armTrackExclusivelyIfEnabled(track.id)
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
                                        armTrackExclusivelyIfEnabled(track.id)
                                        selectedSlot = slot
                                        viewModel.selectTrackClipSlot(track.id, slot)
                                        val clip = slotsByTrack[track.id]
                                            ?.firstOrNull { it.slot == slot }
                                            ?: emptyClipSlot(track, slot)
                                        if (clip.playing) {
                                            viewModel.setClipTransportPlaying(
                                                track.id,
                                                slot,
                                                false,
                                                launchQuantization,
                                            )
                                        } else {
                                            viewModel.launchClipTransport(
                                                track.id,
                                                slot,
                                                launchQuantization,
                                                startGlobal = !transport.playing,
                                            )
                                        }
                                    },
                                    onRecordClip = { track, slot ->
                                        selectedTrackId = track.id
                                        armTrackExclusivelyIfEnabled(track.id)
                                        selectedSlot = slot
                                        viewModel.selectPath(track.id)
                                        viewModel.startTrackClipRecording(
                                            track.id,
                                            slot,
                                            launchQuantization,
                                            startGlobal = !transport.playing,
                                        )
                                    },
                                    onRename = { track, slot, label ->
                                        viewModel.renameTrackClip(track.id, slot, label)
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
                                bpm = transport.beatsPerMinute,
                                onClipSourceBpm = { track, clip, value ->
                                    viewModel.setClipSourceBpm(track.id, clip.slot, value)
                                },
                                onTrackVolume = { track, volume -> viewModel.setTrackVolume(track.id, volume) },
                                onTrackInput = { inputMenuTrack = it },
                                onTrackArm = { track ->
                                    viewModel.setTrackInputArmed(track.id, !track.inputArmed)
                                },
                                onTrackArmLock = { track ->
                                    viewModel.setTrackInputArmLocked(track.id, !track.inputArmLocked)
                                },
                                onTrackDelete = { track -> viewModel.removeTrack(track.id) },
                                onTrackRecordMenu = { track, clip ->
                                    recordMenuTarget = track.id to clip.slot
                                },
                                onClipLoop = { track, clip ->
                                    viewModel.setClipLooping(track.id, clip.slot, !clip.looping)
                                },
                                launchQuantization = launchQuantization,
                                onLaunchQuantizationClick = { showQuantizationMenu = true },
                                modifier = Modifier.fillMaxSize(),
                            )
                            "devices" -> DevicesTile(
                                devices = selectedPlugins,
                                pathId = selectedPath,
                                viewModel = viewModel,
                                horizontal = horizontalPlugins,
                                latencyMs = latencyMs,
                                cpuLoad = cpuLoad,
                                xRunCount = xRunCount,
                                onBrowser = { path -> onNavigateToBrowser(path, -1) },
                                onOpenFullscreen = { plugin, width, height ->
                                    fullscreenPluginInstanceId = plugin.instanceId
                                    fullscreenPluginPathId = selectedPath
                                    fullscreenPluginWidth = width
                                    fullscreenPluginHeight = height
                                },
                                onNavigateToTone3000 = onNavigateToTone3000,
                                modifier = Modifier.fillMaxSize(),
                            )
                            "mixer" -> Mixer(
                                tracks = tracks,
                                selected = selectedPath,
                                scrollState = mixerScrollState,
                                onSelectMaster = { viewModel.selectPath(MASTER_PATH_ID) },
                                onSelect = { track ->
                                    selectedTrackId = track.id
                                    armTrackExclusivelyIfEnabled(track.id)
                                    selectedSlot = track.selectedSlot
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
    latencyMs: Double,
    cpuLoad: Float,
    xRunCount: Int,
    onBrowser: (Long) -> Unit,
    onOpenFullscreen: (RackPlugin, Int, Int) -> Unit,
    modifier: Modifier,
    onNavigateToTone3000: (String?, String?, String?, Int, String?) -> Unit,
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
            Text(
                "${latencyMs.roundToInt()}ms · CPU ${(cpuLoad * 100).roundToInt()}% · XR $xRunCount",
                color = LiveColors.textMuted,
                style = MaterialTheme.typography.labelSmall.copy(fontSize = 9.sp),
                modifier = Modifier.padding(horizontal = LiveDimensions.smallGap),
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
                                        onOpenFullscreen = { _, _, width, height ->
                                            onOpenFullscreen(plugin, width, height)
                                        },
                                        onNavigateToTone3000 = onNavigateToTone3000,
                                        compact = true,
                                        modifier = Modifier
                                            .widthIn(
                                                min = LiveDimensions.pluginMinWidth,
                                                max = LiveDimensions.pluginMaxWidth,
                                            )
                                            .then(
                                                if (expanded) Modifier else Modifier
                                                    .height(LiveDimensions.hitTarget)
                                                    .clipToBounds()
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
                                    onOpenFullscreen = { _, _, width, height ->
                                        onOpenFullscreen(plugin, width, height)
                                    },
                                    onNavigateToTone3000 = onNavigateToTone3000,
                                    compact = true,
                                    modifier = Modifier
                                        .fillMaxWidth()
                                        .then(
                                            if (expanded) Modifier else Modifier
                                                .height(LiveDimensions.hitTarget)
                                                .clipToBounds()
                                        ),
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
    positionSec: Double,
    bpm: Double,
    onPlay: () -> Unit,
    onStop: () -> Unit,
    onRestart: () -> Unit,
    onBpm: () -> Unit,
) {
    val accent = MaterialTheme.colorScheme.primary
    val stopped = !playing && positionSec <= 0.001
    Surface(color = LiveColors.raised) {
        Row(
            modifier = Modifier.fillMaxWidth().height(LiveDimensions.transport)
                .padding(horizontal = LiveDimensions.smallGap),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Box(
                modifier = Modifier.size(LiveDimensions.hitTarget)
                    .clickable(role = Role.Button, onClick = onPlay),
                contentAlignment = Alignment.Center,
            ) {
                Surface(
                    color = if (playing) accent.copy(alpha = 0.16f) else Color.Transparent,
                    contentColor = if (playing) accent else LiveColors.textMuted,
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
            Box(
                modifier = Modifier.size(LiveDimensions.hitTarget)
                    .clickable(role = Role.Button, onClick = onStop),
                contentAlignment = Alignment.Center,
            ) {
                Surface(
                    color = if (stopped) accent.copy(alpha = 0.16f) else Color.Transparent,
                    contentColor = if (stopped) accent else LiveColors.textMuted,
                    shape = RoundedCornerShape(2.dp),
                    modifier = Modifier.size(LiveDimensions.control),
                ) {
                    Box(contentAlignment = Alignment.Center) {
                        Icon(
                            imageVector = Icons.Default.Stop,
                            contentDescription = "Stop transport",
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
                text = "${formatMusicalPosition(positionSec, bpm)} · ${formatElapsedTime(positionSec)}",
                color = LiveColors.textMuted,
                style = MaterialTheme.typography.labelSmall,
                fontWeight = FontWeight.Medium,
                maxLines = 1,
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
private fun EngineControlButton(onClick: () -> Unit) {
    val accent = MaterialTheme.colorScheme.primary
    Box(
        modifier = Modifier
            .width(116.dp)
            .height(LiveDimensions.hitTarget)
            .semantics {
                stateDescription = "Audio engine inactive"
            }
            .clickable(role = Role.Button, onClick = onClick),
        contentAlignment = Alignment.Center,
    ) {
        Surface(
            modifier = Modifier.fillMaxWidth().height(32.dp),
            color = LiveColors.raised,
            contentColor = accent,
            shape = RoundedCornerShape(16.dp),
            shadowElevation = 4.dp,
        ) {
            Row(
                modifier = Modifier.fillMaxSize().padding(horizontal = 10.dp),
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.Center,
            ) {
                Box(
                    modifier = Modifier.size(6.dp).background(accent, CircleShape),
                )
                Text(
                    text = "ACTIVATE AUDIO",
                    modifier = Modifier.padding(start = 6.dp),
                    style = MaterialTheme.typography.labelSmall,
                    fontWeight = FontWeight.Bold,
                )
            }
        }
    }
}

@OptIn(ExperimentalFoundationApi::class)
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
    onRename: (RackTrackInfo, Int, String) -> Unit,
    onTrackColor: (RackTrackInfo, Int) -> Unit,
) {
    CompositionLocalProvider(LocalOverscrollConfiguration provides null) {
    Column(modifier = modifier.fillMaxWidth()) {
        Row(modifier = Modifier.fillMaxWidth().horizontalScroll(horizontalScrollState)) {
            tracks.forEachIndexed { index, track ->
                Box(
                    Modifier.requiredWidth(LiveDimensions.trackWidth)
                        .background(LiveColors.divider)
                        .padding(end = LiveDimensions.hairline),
                ) {
                    TrackHeader(
                        index = index,
                        selected = track.id == selectedTrack?.id,
                        trackColor = trackColors[track.id]
                            ?: AppearancePreferences.palettes[
                                index % AppearancePreferences.palettes.size
                            ].argb,
                        onSelect = { onSelectTrack(track) },
                        onColorSelected = { onTrackColor(track, it) },
                    )
                }
            }
        }
        Row(
            modifier = Modifier
                .weight(1f)
                .fillMaxWidth()
                .verticalScroll(verticalScrollState)
                .horizontalScroll(horizontalScrollState),
        ) {
            tracks.forEach { track ->
                TrackSlots(
                    track = track,
                    slots = slotsByTrack[track.id].orEmpty(),
                    selected = track.id == selectedTrack?.id,
                    selectedSlot = selectedSlot,
                    onSelect = { onSelect(track, it) },
                    onLoad = { onLoad(track, it) },
                    onLaunch = { onLaunch(track, it) },
                    onRecordClip = { onRecordClip(track, it) },
                    onRename = { slotIndex, label -> onRename(track, slotIndex, label) },
                )
            }
        }
    }
    }
}

@Composable
private fun TrackSlots(
    track: RackTrackInfo,
    slots: List<ClipSlotInfo>,
    selected: Boolean,
    selectedSlot: Int,
    onSelect: (Int) -> Unit,
    onLoad: (Int) -> Unit,
    onLaunch: (Int) -> Unit,
    onRecordClip: (Int) -> Unit,
    onRename: (Int, String) -> Unit,
) {
    val visibleCount = maxOf(8, (slots.maxOfOrNull { it.slot + 1 } ?: 0) + 4)
    val bySlot = slots.associateBy { it.slot }
    Box(
        Modifier.requiredWidth(LiveDimensions.trackWidth).background(LiveColors.divider)
            .padding(end = LiveDimensions.hairline),
    ) {
        Column(Modifier.fillMaxWidth().background(if (selected) LiveColors.panel else Color.Black)) {
            repeat(visibleCount) { slotIndex ->
                val slot = bySlot[slotIndex] ?: emptyClipSlot(track, slotIndex)
                ClipCard(
                    track = track,
                    slot = slot,
                    selected = selected && slotIndex == selectedSlot,
                    columnActive = selected,
                    onSelect = onSelect,
                    onLoad = onLoad,
                    onLaunch = onLaunch,
                    onRecord = onRecordClip,
                    onRename = onRename,
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
    columnActive: Boolean,
    onSelect: (Int) -> Unit,
    onLoad: (Int) -> Unit,
    onLaunch: (Int) -> Unit,
    onRecord: (Int) -> Unit,
    onRename: (Int, String) -> Unit,
) {
    val filled = slot.wavLoaded || slot.midiLoaded
    var showRename by remember { mutableStateOf(false) }
    var renameText by remember(slot.displayName) { mutableStateOf(slot.displayName) }
    val playing = filled && slot.playing
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
    val background = when {
        !columnActive -> Color.Black
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
            .combinedClickable(
                role = Role.Button,
                onClick = { onSelect(slot.slot) },
                onLongClick = { if (filled) showRename = true },
            ),
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
                            recordAction && selected -> LiveColors.record.copy(alpha = 0.6f)
                            selected -> accent.copy(alpha = 0.55f)
                            else -> Color.Transparent
                        },
                    ),
                )
                Text(
                    text = if (filled) slot.displayName else "",
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
    if (showRename) {
        AlertDialog(
            onDismissRequest = { showRename = false },
            title = { Text("Rename clip") },
            text = {
                OutlinedTextField(
                    value = renameText,
                    onValueChange = { renameText = it },
                    singleLine = true,
                    label = { Text("Label") },
                    keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Text),
                )
            },
            confirmButton = {
                TextButton(
                    onClick = {
                        onRename(slot.slot, renameText)
                        showRename = false
                    },
                    enabled = renameText.trim().isNotBlank(),
                ) { Text("Rename") }
            },
            dismissButton = { TextButton(onClick = { showRename = false }) { Text("Cancel") } },
        )
    }
}

@Composable
private fun ClipInspector(
    track: RackTrackInfo?,
    clip: ClipSlotInfo?,
    peaks: List<Float>,
    notes: List<MidiNoteInfo>,
    onTrackVolume: (RackTrackInfo, Float) -> Unit,
    onTrackInput: (RackTrackInfo) -> Unit,
    onTrackArm: (RackTrackInfo) -> Unit,
    onTrackArmLock: (RackTrackInfo) -> Unit,
    onTrackDelete: (RackTrackInfo) -> Unit,
    onTrackRecordMenu: (RackTrackInfo, ClipSlotInfo) -> Unit,
    onClipLoop: (RackTrackInfo, ClipSlotInfo) -> Unit,
    onClipSourceBpm: (RackTrackInfo, ClipSlotInfo, Double) -> Unit,
    bpm: Double,
    launchQuantization: TrackLaunchQuantization,
    onLaunchQuantizationClick: () -> Unit,
    modifier: Modifier,
) {
    Surface(color = LiveColors.panel, modifier = modifier.fillMaxWidth()) {
        Column {
            Box(
                modifier = Modifier
                    .fillMaxWidth()
                    .height(LiveDimensions.hitTarget + LiveDimensions.gap),
            ) {
                TrackInspectorControls(
                    track = track,
                    clip = clip,
                    onVolumeChange = { value -> track?.let { onTrackVolume(it, value) } },
                    onInputClick = { track?.let(onTrackInput) },
                    onArmClick = { track?.let(onTrackArm) },
                    onArmLockClick = { track?.let(onTrackArmLock) },
                    onDeleteClick = { track?.let(onTrackDelete) },
                    onRecordOptionsClick = {
                        if (track != null && clip != null) onTrackRecordMenu(track, clip)
                    },
                    onLoopClick = {
                        if (track != null && clip != null) onClipLoop(track, clip)
                    },
                    launchQuantization = launchQuantization,
                    onLaunchQuantizationClick = onLaunchQuantizationClick,
                )
            }
            Box(Modifier.fillMaxWidth().weight(1f)) {
                if (clip?.midiLoaded == true) {
                    PianoRoll(clip, notes, bpm)
                } else {
                    Waveform(clip, peaks, bpm, track, onClipSourceBpm)
                }
            }
        }
    }
}

@Composable
@OptIn(ExperimentalFoundationApi::class)
private fun TrackInspectorControls(
    track: RackTrackInfo?,
    clip: ClipSlotInfo?,
    onVolumeChange: (Float) -> Unit,
    onInputClick: () -> Unit,
    onArmClick: () -> Unit,
    onArmLockClick: () -> Unit,
    onDeleteClick: () -> Unit,
    onRecordOptionsClick: () -> Unit,
    onLoopClick: () -> Unit,
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
    var showArmLockMenu by remember { mutableStateOf(false) }
    var showMore by remember { mutableStateOf(false) }
    Row(
        modifier = Modifier.fillMaxSize().padding(horizontal = LiveDimensions.smallGap),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(LiveDimensions.smallGap),
    ) {
        Box {
            Box(
                modifier = Modifier
                    .size(LiveDimensions.hitTarget)
                    .combinedClickable(
                        role = Role.Button,
                        onClick = onArmClick,
                        onLongClickLabel = if (track.inputArmLocked) "Unlock arm" else "Lock arm",
                        onLongClick = { showArmLockMenu = true },
                    )
                    .semantics {
                        contentDescription =
                            if (track.inputArmed) "Disarm track input" else "Arm track input"
                        stateDescription = when {
                            track.inputArmed && track.inputArmLocked -> "Armed, arm locked"
                            track.inputArmed -> "Armed, arm unlocked"
                            track.inputArmLocked -> "Disarmed, arm locked"
                            else -> "Disarmed, arm unlocked"
                        }
                    },
                contentAlignment = Alignment.Center,
            ) {
                Icon(
                    Icons.Default.Mic,
                    contentDescription = null,
                    tint = if (track.inputArmed) LiveColors.record else LiveColors.textDim,
                    modifier = Modifier.size(LiveDimensions.icon),
                )
                if (track.inputArmLocked) {
                    Icon(
                        Icons.Default.Lock,
                        contentDescription = null,
                        tint = LiveColors.text,
                        modifier = Modifier
                            .align(Alignment.BottomEnd)
                            .padding(LiveDimensions.smallGap)
                            .size(LiveDimensions.indicatorIcon),
                    )
                }
            }
            DropdownMenu(
                expanded = showArmLockMenu,
                onDismissRequest = { showArmLockMenu = false },
            ) {
                DropdownMenuItem(
                    text = { Text(if (track.inputArmLocked) "Unlock arm" else "Lock arm") },
                    onClick = {
                        showArmLockMenu = false
                        onArmLockClick()
                    },
                )
            }
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
        IconButton(
            onClick = onLoopClick,
            modifier = Modifier
                .size(LiveDimensions.hitTarget)
                .background(
                    if (clip?.looping == true) MaterialTheme.colorScheme.primary.copy(alpha = 0.16f)
                    else Color.Transparent,
                    RoundedCornerShape(2.dp),
                )
                .semantics {
                    contentDescription = "Loop selected clip slot"
                    stateDescription = if (clip?.looping == true) "Active" else "Inactive"
                },
        ) {
            Icon(
                Icons.Default.Repeat,
                contentDescription = null,
                tint = if (clip?.looping == true) MaterialTheme.colorScheme.primary else LiveColors.textDim,
                modifier = Modifier.size(LiveDimensions.icon),
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

private fun clipTimelineDuration(clip: ClipSlotInfo, bpm: Double): Double {
    val loopDuration = if (
        bpm.isFinite() && bpm > 0.0 &&
        clip.loopLengthBars.isFinite() && clip.loopLengthBars > 0.0
    ) {
        clip.loopLengthBars * 4.0 * 60.0 / bpm
    } else {
        0.0
    }
    return when {
        clip.looping && loopDuration.isFinite() && loopDuration > 0.0 -> loopDuration
        clip.durationSec.isFinite() && clip.durationSec > 0.0 -> clip.durationSec
        else -> 0.0
    }
}

private fun formatLoopLengthBars(bars: Double): String = when {
    !bars.isFinite() || bars <= 0.0 -> "—"
    bars == 0.25 -> "¼ bar"
    bars == 1.0 -> "1 bar"
    bars == bars.roundToInt().toDouble() -> "${bars.roundToInt()} bars"
    else -> "$bars bars"
}

@Composable
private fun ClipPositionOverlay(
    clip: ClipSlotInfo,
    bpm: Double,
    track: RackTrackInfo?,
    onClipSourceBpm: (RackTrackInfo, ClipSlotInfo, Double) -> Unit,
    modifier: Modifier = Modifier,
) {
    var showBpmDialog by remember { mutableStateOf(false) }
    var bpmInput by remember(clip.sourceBpm) { mutableStateOf(clip.sourceBpm.toString()) }
    val positionSec = clip.positionSec.takeIf { it.isFinite() && it >= 0.0 } ?: 0.0
    val durationSec = clip.durationSec.takeIf { it.isFinite() && it >= 0.0 } ?: 0.0
    val mediaType = if (clip.midiLoaded) "MIDI" else "WAV"
    val details = buildString {
        append(clip.displayName.ifBlank { "Untitled clip" })
        append(" · Slot ${clip.slot + 1} · $mediaType")
        append(" · ${formatMusicalPosition(positionSec, bpm)} · ${formatElapsedTime(positionSec)}")
        append(" · Duration ${formatElapsedTime(durationSec)}")
        append(" · Loop ${if (clip.looping) "on" else "off"}")
        append(" (${formatLoopLengthBars(clip.loopLengthBars)})")
        append(" · Punch ${if (clip.enterOnPunch) "on" else "off"}")
        append(" · ${if (clip.playing) "Playing" else "Stopped"}")
        append(" · ${if (clip.active) "Selected" else "Not selected"}")
        if (clip.wavLoaded) {
            append(" · Base BPM ${"%.2f".format(clip.sourceBpm)}")
            append(" · Tempo ${ClipTempoMode.entries.getOrElse(clip.tempoMode) { ClipTempoMode.Original }.name}")
        }
    }
    Box(
        modifier = modifier
            .fillMaxWidth()
            .background(Color.Black.copy(alpha = 0.8f))
            .semantics {
                contentDescription = if (clip.wavLoaded) {
                    "Clip information for ${clip.displayName.ifBlank { "Untitled clip" }}. Double tap to edit Base BPM."
                } else {
                    "MIDI clip information for ${clip.displayName.ifBlank { "Untitled clip" }}"
                }
            }
            .then(
                if (clip.wavLoaded && track != null) {
                    Modifier.clickable { showBpmDialog = true }
                } else {
                    Modifier
                }
            ),
    ) {
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .horizontalScroll(rememberScrollState())
                .padding(
                    horizontal = LiveDimensions.gap,
                    vertical = LiveDimensions.smallGap,
                ),
        ) {
            Text(
                text = details,
                color = LiveColors.text,
                style = MaterialTheme.typography.labelSmall,
                maxLines = 1,
            )
        }

    }
    if (showBpmDialog && clip.wavLoaded && track != null) {
        val value = bpmInput.toDoubleOrNull()
        AlertDialog(
            onDismissRequest = { showBpmDialog = false },
            title = { Text("Edit Base BPM") },
            text = {
                OutlinedTextField(
                    value = bpmInput,
                    onValueChange = { bpmInput = it },
                    label = { Text("Base BPM") },
                    singleLine = true,
                    keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Decimal),
                )
            },
            confirmButton = {
                TextButton(
                    onClick = {
                        if (value != null && value.isFinite() && value in 20.0..400.0) {
                            onClipSourceBpm(track, clip, value)
                            showBpmDialog = false
                        }
                    },
                    enabled = value != null && value.isFinite() && value in 20.0..400.0,
                ) { Text("Save") }
            },
            dismissButton = { TextButton(onClick = { showBpmDialog = false }) { Text("Cancel") } },
        )
    }
}
@Composable
private fun Waveform(
    clip: ClipSlotInfo?,
    peaks: List<Float>,
    bpm: Double,
    track: RackTrackInfo?,
    onClipSourceBpm: (RackTrackInfo, ClipSlotInfo, Double) -> Unit,
) {
    if (clip?.wavLoaded != true) return
    val accent = MaterialTheme.colorScheme.primary
    Box(Modifier.fillMaxSize()) {
        Canvas(
            modifier = Modifier
                .fillMaxSize()
                .padding(horizontal = LiveDimensions.smallGap),
        ) {
            val durationSec = clipTimelineDuration(clip, bpm)
            val mid = size.height / 2
            drawLine(LiveColors.waveformLine, Offset(0f, mid), Offset(size.width, mid), 1f)
            if (durationSec > 0.0 && bpm.isFinite() && bpm > 0.0) {
                val secondsPerBeat = 60.0 / bpm
                val beatWidth = size.width * (secondsPerBeat / durationSec).toFloat()
                if (beatWidth.isFinite() && beatWidth >= 1f) {
                    val beatCount = (durationSec / secondsPerBeat).toInt()
                        .coerceAtMost(size.width.toInt().coerceAtLeast(0))
                    repeat(beatCount + 1) { beat ->
                        val x = beat * beatWidth
                        drawLine(LiveColors.divider, Offset(x, 0f), Offset(x, size.height), 1f)
                    }
                }
            }
            val audioDurationSec = clip.durationSec.takeIf { it.isFinite() && it > 0.0 } ?: 0.0
            if (peaks.isNotEmpty() && durationSec > 0.0 && audioDurationSec > 0.0) {
                val audioWidth = size.width *
                    (audioDurationSec / durationSec).toFloat().coerceIn(0f, 1f)
                val step = audioWidth / peaks.size
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
            val positionSec = clip.positionSec.takeIf { it.isFinite() && it >= 0.0 } ?: 0.0
            if (clip.playing && durationSec > 0.0) {
                val x = size.width *
                    (positionSec / durationSec).toFloat().coerceIn(0f, 1f)
                drawLine(accent, Offset(x, 0f), Offset(x, size.height), 2f)
            }
        }
        ClipPositionOverlay(
            clip = clip,
            bpm = bpm,
            track = track,
            onClipSourceBpm = onClipSourceBpm,
            modifier = Modifier.align(Alignment.BottomStart),
        )
    }
}

@Composable
private fun PianoRoll(clip: ClipSlotInfo, notes: List<MidiNoteInfo>, bpm: Double) {
    val accent = MaterialTheme.colorScheme.primary
    Box(Modifier.fillMaxSize()) {
        Canvas(
            modifier = Modifier
                .fillMaxSize()
                .padding(horizontal = LiveDimensions.smallGap),
        ) {
            val rows = 12
            repeat(rows + 1) { row ->
                val y = size.height * row / rows
                drawLine(LiveColors.divider, Offset(0f, y), Offset(size.width, y), 1f)
            }
            val durationSec = clipTimelineDuration(clip, bpm)
            if (durationSec > 0.0 && bpm.isFinite() && bpm > 0.0) {
                val secondsPerBeat = 60.0 / bpm
                val beatWidth = size.width * (secondsPerBeat / durationSec).toFloat()
                if (beatWidth.isFinite() && beatWidth >= 1f) {
                    val beatCount = (durationSec / secondsPerBeat).toInt()
                        .coerceAtMost(size.width.toInt().coerceAtLeast(0))
                    repeat(beatCount + 1) { beat ->
                        val x = beat * beatWidth
                        drawLine(LiveColors.divider, Offset(x, 0f), Offset(x, size.height), 1f)
                    }
                }
            }
            val durationFrames = (durationSec * 48_000).coerceAtLeast(1.0)
            notes.forEach { note ->
                val x = (note.startFrame / durationFrames).toFloat().coerceIn(0f, 1f) * size.width
                val width = (
                    (note.durationFrames.coerceAtLeast(24_000) / durationFrames).toFloat() * size.width
                ).coerceAtLeast(4f)
                val y = size.height - ((note.pitch.coerceIn(48, 83) - 47) / 36f) * size.height
                drawRect(
                    LiveColors.midi,
                    Offset(x, y - size.height / rows + 2f),
                    Size(width, size.height / rows - 4f),
                )
            }
            val positionSec = clip.positionSec.takeIf { it.isFinite() && it >= 0.0 } ?: 0.0
            if (clip.playing && durationSec > 0.0) {
                val x = size.width *
                    (positionSec / durationSec).toFloat().coerceIn(0f, 1f)
                drawLine(accent, Offset(x, 0f), Offset(x, size.height), 2f)
            }
        }
        ClipPositionOverlay(
            clip = clip,
            bpm = bpm,
            track = null,
            onClipSourceBpm = { _, _, _ -> },
            modifier = Modifier.align(Alignment.BottomStart),
        )
    }
}


@Composable
private fun Mixer(
    tracks: List<RackTrackInfo>,
    selected: RackPathId?,
    onSelect: (RackTrackInfo) -> Unit,
    onSelectMaster: () -> Unit,
    onVolume: (RackTrackInfo, Float) -> Unit,
    onAdd: () -> Unit,
    modifier: Modifier,
    scrollState: androidx.compose.foundation.ScrollState,
) {
    Surface(color = LiveColors.raised, modifier = modifier.fillMaxWidth()) {
        Row(modifier = Modifier.fillMaxSize()) {
            Row(
                modifier = Modifier.weight(1f).fillMaxHeight().horizontalScroll(scrollState),
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
                IconButton(onClick = onAdd, modifier = Modifier.size(LiveDimensions.hitTarget)) {
                    Icon(
                        Icons.Default.Add,
                        contentDescription = "Add track",
                        tint = LiveColors.textMuted,
                        modifier = Modifier.size(LiveDimensions.icon),
                    )
                }
            }
            MasterChannel(
                selected = selected == MASTER_PATH_ID,
                onSelect = onSelectMaster,
            )
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
                    .background(if (track.activeSlot >= 0) accent else LiveColors.divider),
            )
            Text(
                text = "T${index + 1}",
                color = if (selected || track.activeSlot >= 0) accent else LiveColors.text,
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
private fun MasterChannel(
    selected: Boolean,
    onSelect: () -> Unit,
) {
    val accent = MaterialTheme.colorScheme.primary
    Column(
        modifier = Modifier.width(LiveDimensions.mixerChannel).fillMaxHeight()
            .background(if (selected) accent.copy(alpha = 0.12f) else LiveColors.panel)
            .drawBehind {
                drawLine(
                    color = LiveColors.divider,
                    start = Offset(0f, 0f),
                    end = Offset(0f, size.height),
                    strokeWidth = 1.dp.toPx(),
                )
            }
            .semantics { this.selected = selected }
            .clickable(role = Role.Button, onClick = onSelect)
            .padding(vertical = LiveDimensions.smallGap),
        horizontalAlignment = Alignment.CenterHorizontally,
    ) {
        Text(
            "MASTER",
            color = if (selected) accent else LiveColors.text,
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
