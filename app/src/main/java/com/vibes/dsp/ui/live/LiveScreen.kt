package com.vibes.dsp.ui.live
import com.vibes.dsp.ui.formatMusicalPosition
import com.vibes.dsp.ui.components.MusicalPositionControl
import com.vibes.dsp.ui.interpolatedMusicalQuarterNotes
import com.vibes.dsp.ui.interpolatedElapsedSeconds
import com.vibes.dsp.ui.rememberFrameClockNanos

import android.graphics.Paint
import android.app.Activity
import android.content.pm.ActivityInfo
import android.net.Uri
import android.widget.Toast
import androidx.activity.compose.BackHandler
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.animation.core.RepeatMode
import androidx.compose.foundation.Image
import androidx.compose.animation.core.animateFloat
import androidx.compose.animation.core.infiniteRepeatable
import androidx.compose.animation.core.rememberInfiniteTransition
import androidx.compose.animation.core.tween
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.ExperimentalFoundationApi
import androidx.compose.foundation.LocalOverscrollConfiguration
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.combinedClickable
import androidx.compose.foundation.gestures.detectTapGestures
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
import androidx.compose.material.icons.filled.Replay
import androidx.compose.material.icons.filled.Repeat
import androidx.compose.material.icons.filled.Settings
import androidx.compose.material.icons.filled.SkipPrevious
import androidx.compose.material.icons.filled.Stop
import androidx.compose.material.icons.filled.MoreVert
import androidx.compose.material.icons.filled.Tune
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.ui.res.painterResource
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
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.draw.clipToBounds
import androidx.compose.ui.graphics.nativeCanvas
import androidx.compose.ui.graphics.drawscope.DrawScope
import androidx.compose.ui.graphics.drawscope.drawIntoCanvas
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.toArgb
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.graphics.RectangleShape
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalDensity
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
import com.vibes.dsp.R
import com.vibes.dsp.engine.ClipSlotInfo
import com.vibes.dsp.engine.ClipTempoMode
import com.vibes.dsp.engine.DirectUsbAudioManager
import com.vibes.dsp.engine.MidiNoteInfo
import com.vibes.dsp.engine.MASTER_PATH_ID
import com.vibes.dsp.engine.RackPathId
import com.vibes.dsp.engine.RackTrackInfo
import com.vibes.dsp.engine.TrackLaunchQuantization
import com.vibes.dsp.ui.components.CompactHorizontalFader
import com.vibes.dsp.ui.components.NnagaChoiceRow
import com.vibes.dsp.ui.components.NnagaSelectorMenuItem
import com.vibes.dsp.ui.components.NnagaCheckbox
import com.vibes.dsp.ui.components.NnagaIconButton
import com.vibes.dsp.ui.components.NnagaTextButton
import com.vibes.dsp.ui.components.nnagaOutlinedTextFieldColors
import com.vibes.dsp.ui.dashboard.rememberTopCutoutBounds
import com.vibes.dsp.ui.rack.PluginCard
import com.vibes.dsp.ui.rack.RackViewModel
import com.vibes.dsp.ui.rack.RackPlugin
import com.vibes.dsp.ui.theme.AppearancePreferences
import kotlin.math.ceil
import kotlin.math.floor
import kotlin.math.roundToInt


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
    val hitTarget = 48.dp
    val control = 32.dp
    val toolbar = 48.dp
    val transport = 48.dp
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
    defaultLoopLengthBars = track.defaultLoopLengthBars,
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
    onNavigateToDashboard: () -> Unit = {},
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
    val deviceChainGuidance by viewModel.deviceChainGuidance.collectAsState()
    val slotsByTrack by viewModel.clipSlots.collectAsState()
    val peaksByTrack by viewModel.waveformPeaks.collectAsState()
    val notesByClip by viewModel.midiNotes.collectAsState()
    val inputChannelCount = DirectUsbAudioManager.getInputChannelCount()

    var launchQuantizationOrdinal by rememberSaveable { mutableIntStateOf(0) }
    val launchQuantization = TrackLaunchQuantization.entries[launchQuantizationOrdinal]
    var clipSettingsTarget by remember { mutableStateOf<Pair<Long, Int>?>(null) }
    var slotSettingsTarget by remember { mutableStateOf<Pair<Long, Int>?>(null) }
    var clipBpmInput by rememberSaveable { mutableStateOf("") }
    var clipNameInput by rememberSaveable { mutableStateOf("") }
    var showTempoDialog by rememberSaveable { mutableStateOf(false) }
    var tempoInput by rememberSaveable { mutableStateOf("") }
    var selectedTrackId by rememberSaveable { mutableLongStateOf(0L) }
    var selectedSlot by rememberSaveable { mutableIntStateOf(0) }
    var importTarget by remember { mutableStateOf<Pair<Long, Int>?>(null) }
    var deviceChainPathId by remember { mutableStateOf<RackPathId?>(null) }
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
    fun openClipSettings(trackId: Long, clip: ClipSlotInfo) {
        clipNameInput = clip.displayName
        clipBpmInput = if (clip.wavLoaded) clip.sourceBpm.toString() else ""
        clipSettingsTarget = trackId to clip.slot
    }

    fun closeClipSettings() {
        clipSettingsTarget = null
        clipNameInput = ""
        clipBpmInput = ""
    }
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
    val saveDeviceChainLauncher = rememberLauncherForActivityResult(
        ActivityResultContracts.CreateDocument("application/octet-stream"),
    ) { uri ->
        val pathId = deviceChainPathId
        deviceChainPathId = null
        if (uri != null && pathId != null) viewModel.saveDeviceChain(pathId, uri)
    }
    val loadDeviceChainLauncher = rememberLauncherForActivityResult(
        ActivityResultContracts.OpenDocument(),
    ) { uri ->
        val pathId = deviceChainPathId
        deviceChainPathId = null
        if (uri != null && pathId != null) viewModel.loadDeviceChain(pathId, uri)
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
    val selectedClipRecording = selectedTrack?.recordingSlot == selectedSlot &&
        (selectedTrack.punchArmed || selectedTrack.recordPending || selectedTrack.recording)
    LaunchedEffect(selectedTrack?.id) {
        if (selectedTrack != null && selectedTrack.id != selectedTrackId) {
            selectedTrackId = selectedTrack.id
            selectedSlot = selectedTrack.selectedSlot
        }
    }
    LaunchedEffect(selectedPath, tracks) {
        if (selectedPath != MASTER_PATH_ID) {
            tracks.firstOrNull { it.id == selectedPath }?.let { selectedTrackId = it.id }
        }
    }

    LaunchedEffect(
        selectedTrack?.id,
        selectedSlot,
        selectedClip?.midiLoaded,
        selectedClip?.wavLoaded,
        selectedClipRecording,
    ) {
        selectedTrack ?: return@LaunchedEffect
        if (selectedClip?.midiLoaded == true) viewModel.loadTrackClipMidiNotes(selectedTrack.id, selectedSlot)
        if (selectedClip?.wavLoaded == true && !selectedClipRecording) {
            viewModel.loadTrackWaveform(selectedTrack.id)
        }
    }

    inputMenuTrack?.let { menuTrack ->
        val track = tracks.firstOrNull { it.id == menuTrack.id } ?: menuTrack
        AlertDialog(
            onDismissRequest = { inputMenuTrack = null },
            title = { Text("Input source") },
            text = {
                Column(Modifier.verticalScroll(rememberScrollState())) {
                    (0 until inputChannelCount).forEach { channel ->
                        NnagaSelectorMenuItem(
                            text = "Hardware ${channel + 1} (mono)",
                            selected = track.inputSourceKind == 2 &&
                                track.inputSourceFirstChannel == channel,
                            onClick = {
                                viewModel.setTrackInputHardwareMono(track.id, channel)
                                inputMenuTrack = null
                            },
                            modifier = Modifier.fillMaxWidth(),
                        )
                    }
                    (0 until (inputChannelCount - 1) step 2).forEach { firstChannel ->
                        NnagaSelectorMenuItem(
                            text = "Hardware ${firstChannel + 1}/${firstChannel + 2} (stereo)",
                            selected = track.inputSourceKind == 0 &&
                                track.inputSourceFirstChannel == firstChannel,
                            onClick = {
                                viewModel.setTrackInputHardwarePair(track.id, firstChannel)
                                inputMenuTrack = null
                            },
                            modifier = Modifier.fillMaxWidth(),
                        )
                    }
                    tracks.filter { it.id != track.id }.forEach { source ->
                        listOf(0 to "Pre", 1 to "Post").forEach { (tap, label) ->
                            NnagaSelectorMenuItem(
                                text = "${source.id} $label-fader",
                                selected = track.inputSourceKind == 1 &&
                                    track.inputSourceTrackId == source.id && track.inputTap == tap,
                                onClick = {
                                    viewModel.setTrackInputTrack(track.id, source.id, tap)
                                    inputMenuTrack = null
                                },
                                modifier = Modifier.fillMaxWidth(),
                            )
                        }
                    }
                    if (inputChannelCount == 0 && tracks.none { it.id != track.id }) {
                        Text("Add another track to route its pre- or post-fader signal.")
                    }
                }
            },
            confirmButton = { NnagaTextButton(onClick = { inputMenuTrack = null }) { Text("Cancel") } },
        )
    }
    clipSettingsTarget?.let { (trackId, slotIndex) ->
        val track = tracks.firstOrNull { it.id == trackId } ?: return@let
        val clip = slotsByTrack[trackId]?.firstOrNull { it.slot == slotIndex }
            ?: return@let
        val hasMedia = clip.wavLoaded || clip.midiLoaded
        val normalizedClipName = clipNameInput.trim()
        val validClipName = normalizedClipName.isNotBlank()
        val parsedBpm = clipBpmInput.toDoubleOrNull()
        val validBpm = parsedBpm != null && parsedBpm.isFinite() && parsedBpm in 20.0..400.0
        AlertDialog(
            onDismissRequest = ::closeClipSettings,
            title = { Text("Clip settings") },
            text = {
                Column(Modifier.verticalScroll(rememberScrollState())) {
                    OutlinedTextField(
                        value = clipNameInput,
                        onValueChange = { clipNameInput = it },
                        label = { Text("Clip name") },
                        supportingText = {
                            if (!validClipName) Text("Enter a clip name")
                        },
                        isError = !validClipName,
                        singleLine = true,
                        keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Text),
                        modifier = Modifier.fillMaxWidth(),
                        shape = MaterialTheme.shapes.small,
                        colors = nnagaOutlinedTextFieldColors(),
                    )
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        NnagaCheckbox(
                            checked = clip.looping,
                            enabled = hasMedia,
                            onCheckedChange = {
                                viewModel.setClipLooping(track.id, slotIndex, it)
                            },
                        )
                        Text("Loop clip")
                    }
                    MusicalPositionControl(
                        label = "Loop start",
                        quarterNotes = clip.loopStartQuarterNotes,
                        maxQuarterNotes = (clip.durationSec * (if (clip.sourceBpm > 0.0) clip.sourceBpm else 120.0) / 60.0),
                        enabled = hasMedia,
                        onCommit = { value, result -> viewModel.setClipLoopStartQuarterNotes(track.id, slotIndex, value, result) },
                        modifier = Modifier.fillMaxWidth(),
                    )
                    MusicalPositionControl(
                        label = "Loop length",
                        quarterNotes = clip.loopLengthQuarterNotes,
                        maxQuarterNotes = ((clip.durationSec * (if (clip.sourceBpm > 0.0) clip.sourceBpm else 120.0) / 60.0) - clip.loopStartQuarterNotes).coerceAtLeast(0.0),
                        enabled = hasMedia,
                        onCommit = { value, result -> viewModel.setClipLoopLengthQuarterNotes(track.id, slotIndex, value, result) },
                        modifier = Modifier.fillMaxWidth(),
                    )
                    if (clip.wavLoaded) {
                        OutlinedTextField(
                            value = clipBpmInput,
                            onValueChange = { clipBpmInput = it },
                            label = { Text("WAV Base BPM") },
                            supportingText = {
                                if (!validBpm) Text("Enter a value from 20 to 400 BPM")
                            },
                            isError = !validBpm,
                            singleLine = true,
                            keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Decimal),
                            modifier = Modifier.fillMaxWidth(),
                            shape = MaterialTheme.shapes.small,
                            colors = nnagaOutlinedTextFieldColors(),
                        )
                        Text("Playback tempo mode")
                        val currentMode = ClipTempoMode.entries.getOrElse(clip.tempoMode) {
                            ClipTempoMode.Original
                        }
                        ClipTempoMode.entries.forEach { mode ->
                            NnagaChoiceRow(
                                text = mode.name,
                                selected = currentMode == mode,
                                onClick = {
                                    viewModel.setClipTempoMode(track.id, slotIndex, mode)
                                },
                                modifier = Modifier.fillMaxWidth(),
                            )
                        }
                    }
                }
            },
            confirmButton = {
                NnagaTextButton(
                    onClick = {
                        if (normalizedClipName != clip.displayName) {
                            viewModel.renameTrackClip(track.id, slotIndex, normalizedClipName)
                        }
                        if (clip.wavLoaded) {
                            viewModel.setClipSourceBpm(track.id, slotIndex, parsedBpm!!)
                        }
                        closeClipSettings()
                    },
                    enabled = validClipName && (!clip.wavLoaded || validBpm),
                ) { Text("Done") }
            },
            dismissButton = {
                NnagaTextButton(
                    onClick = {
                        viewModel.unloadTrackClipMedia(
                            track.id,
                            slotIndex,
                            clip.wavLoaded,
                            clip.midiLoaded,
                        )
                        closeClipSettings()
                    },
                ) {
                    Text("Clear slot", color = LiveColors.error)
                }
            },
        )
    }
    slotSettingsTarget?.let { (trackId, slotIndex) ->
        val track = tracks.firstOrNull { it.id == trackId } ?: return@let
        val slot = slotsByTrack[trackId]?.firstOrNull { it.slot == slotIndex }
            ?: emptyClipSlot(track, slotIndex)
        val slotHasMedia = slot.wavLoaded || slot.midiLoaded
        AlertDialog(
            onDismissRequest = { slotSettingsTarget = null },
            title = { Text("Slot settings") },
            text = {
                Column(Modifier.verticalScroll(rememberScrollState())) {
                    Text(
                        "Slot ${slotIndex + 1}",
                        style = MaterialTheme.typography.titleSmall,
                    )
                    Text("Default recording loop length")
                    supportedLoopLengths.forEach { (bars, label) ->
                        NnagaChoiceRow(
                            text = label,
                            selected = slot.defaultLoopLengthBars == bars,
                            onClick = {
                                viewModel.setSlotDefaultLoopLength(track.id, slotIndex, bars)
                            },
                            modifier = Modifier.fillMaxWidth(),
                        )
                    }
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        NnagaCheckbox(
                            checked = slot.enterOnPunch,
                            enabled = slot.enterOnPunch || !slotHasMedia,
                            onCheckedChange = {
                                viewModel.setSlotEnterOnPunch(
                                    track.id,
                                    slotIndex,
                                    it,
                                    launchQuantization,
                                )
                            },
                        )
                        Text("Enter on punch")
                    }
                    Text("Track default for future slots")
                    supportedLoopLengths.forEach { (bars, label) ->
                        NnagaChoiceRow(
                            text = label,
                            selected = track.defaultLoopLengthBars == bars,
                            onClick = {
                                viewModel.setTrackDefaultLoopLength(track.id, bars)
                            },
                            modifier = Modifier.fillMaxWidth(),
                        )
                    }
                }
            },
            confirmButton = {
                NnagaTextButton(onClick = { slotSettingsTarget = null }) { Text("Done") }
            },
        )
    }
    if (showQuantizationMenu) {
        AlertDialog(
            onDismissRequest = { showQuantizationMenu = false },
            title = { Text("Launch quantization") },
            text = {
                Column {
                    TrackLaunchQuantization.entries.forEachIndexed { index, quantization ->
                        NnagaChoiceRow(
                            text = quantization.name,
                            selected = launchQuantization == quantization,
                            onClick = {
                                launchQuantizationOrdinal = index
                                showQuantizationMenu = false
                            },
                            modifier = Modifier.fillMaxWidth(),
                        )
                    }
                }
            },
            confirmButton = { NnagaTextButton(onClick = { showQuantizationMenu = false }) { Text("Cancel") } },
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
                    shape = MaterialTheme.shapes.small,
                    colors = nnagaOutlinedTextFieldColors(),
                )
            },
            confirmButton = {
                NnagaTextButton(onClick = {
                    tempoInput.toDoubleOrNull()?.coerceIn(20.0, 400.0)?.let(viewModel::setTransportBpm)
                    showTempoDialog = false
                }) { Text("Apply") }
            },
            dismissButton = { NnagaTextButton(onClick = { showTempoDialog = false }) { Text("Cancel") } },
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
                onDashboard = onNavigateToDashboard,
                onToggleEdit = { editTiles = !editTiles },
            )
            if (!hideTransportWithoutLauncher || "launcher" in visibleTiles) {
                TransportBar(
                    playing = transport.playing,
                    positionSec = transport.positionSec,
                    musicalQuarterNotes = transport.musicalQuarterNotes,
                    bpm = transport.beatsPerMinute,
                    capturedAtMonotonicNanos = transport.capturedAtMonotonicNanos,
                    onPlay = {
                        if (transport.playing) viewModel.transportPause() else viewModel.transportPlay()
                    },
                    onStop = viewModel::transportStop,
                    onRestart = viewModel::transportRestart,
                    onBpm = {
                        tempoInput = transport.beatsPerMinute.toString()
                        showTempoDialog = true
                    },
                    launchQuantization = launchQuantization,
                    onLaunchQuantizationClick = { showQuantizationMenu = true },
                    onDeleteSelectedTrack = selectedTrack?.let { track ->
                        { viewModel.removeTrack(track.id) }
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
                                    bpm = transport.beatsPerMinute,
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
                                        viewModel.launchClipTransport(
                                            track.id,
                                            slot,
                                            launchQuantization,
                                            startGlobal = !transport.playing,
                                        )
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
                                        )
                                    },
                                    onCancelRecording = { track ->
                                        viewModel.cancelTrackLoopRecording(track.id)
                                    },
                                    onOpenClipSettings = { track, clip ->
                                        openClipSettings(track.id, clip)
                                    },
                                    onOpenSlotSettings = { track, clip ->
                                        slotSettingsTarget = track.id to clip.slot
                                    },
                                    onStop = { track ->
                                        viewModel.stopTrackClipTransport(track.id, launchQuantization)
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
                                peaks = if (selectedClipRecording) {
                                    emptyList()
                                } else {
                                    peaksByTrack[selectedTrack?.id].orEmpty()
                                },
                                notes = notesByClip[selectedTrack?.id to selectedSlot].orEmpty(),
                                bpm = transport.beatsPerMinute,
                                onTrackInput = { inputMenuTrack = it },
                                onTrackArm = { track ->
                                    viewModel.setTrackInputArmed(track.id, !track.inputArmed)
                                },
                                onTrackArmLock = { track ->
                                    viewModel.setTrackInputArmLocked(track.id, !track.inputArmLocked)
                                },
                                onOpenClipSettings = { track, clip -> openClipSettings(track.id, clip) },
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
                                canCreateParallelReturn = selectedTrack != null &&
                                    selectedPath != MASTER_PATH_ID,
                                guidance = deviceChainGuidance,
                                onCreateParallelReturn = viewModel::createParallelWetReturn,
                                onBrowser = { path -> onNavigateToBrowser(path, -1) },
                                onSaveChain = { path ->
                                    deviceChainPathId = path
                                    saveDeviceChainLauncher.launch("device-chain-$path.nnchain")
                                },
                                onLoadChain = { path ->
                                    deviceChainPathId = path
                                    loadDeviceChainLauncher.launch(arrayOf("application/octet-stream"))
                                },
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
    onDashboard: () -> Unit,
    onToggleEdit: () -> Unit,
) {
    val density = LocalDensity.current
    val cutout = rememberTopCutoutBounds()
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
                    NnagaToolbarButton(editMode, onDashboard, onToggleEdit)
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
                    NnagaToolbarButton(editMode, onDashboard, onToggleEdit)
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
    onDashboard: () -> Unit,
    onToggleEdit: () -> Unit,
) {
    val accent = MaterialTheme.colorScheme.primary
    Box(
        modifier = Modifier.height(LiveDimensions.hitTarget).widthIn(min = LiveDimensions.hitTarget)
            .combinedClickable(
                role = Role.Button,
                onClickLabel = "Open dashboard",
                onLongClickLabel = "Edit tile order",
                onClick = onDashboard,
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
                Image(
                    painter = painterResource(R.drawable.nnaga_brand_mark),
                    contentDescription = "NNAGA",
                    modifier = Modifier.size(32.dp),
                )
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
                NnagaIconButton(
                    onClick = onShrink,
                    enabled = canShrink,
                ) {
                    Icon(
                        Icons.Default.Remove,
                        "Decrease $title height by 32 dp",
                        modifier = Modifier.size(LiveDimensions.icon),
                    )
                }
                NnagaIconButton(
                    onClick = onGrow,
                    enabled = canGrow,
                ) {
                    Icon(
                        Icons.Default.Add,
                        "Increase $title height by 32 dp",
                        modifier = Modifier.size(LiveDimensions.icon),
                    )
                }
                NnagaIconButton(
                    onClick = onMoveUp,
                    enabled = canMoveUp,
                ) {
                    Icon(
                        Icons.Default.KeyboardArrowUp,
                        "Move $title up",
                        modifier = Modifier.size(LiveDimensions.icon),
                    )
                }
                NnagaIconButton(
                    onClick = onMoveDown,
                    enabled = canMoveDown,
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
    canCreateParallelReturn: Boolean,
    guidance: String?,
    onCreateParallelReturn: (Long) -> Unit,
    onBrowser: (Long) -> Unit,
    onSaveChain: (Long) -> Unit,
    onLoadChain: (Long) -> Unit,
    onOpenFullscreen: (RackPlugin, Int, Int) -> Unit,
    modifier: Modifier,
    onNavigateToTone3000: (String?, String?, String?, Int, String?) -> Unit,
) {
    var showDeviceChainMenu by remember { mutableStateOf(false) }
    val horizontalScrollState = rememberScrollState()
    val verticalScrollState = rememberScrollState()
    Column(modifier = modifier) {
        Row(
            modifier = Modifier.fillMaxWidth().height(LiveDimensions.hitTarget)
                .padding(start = LiveDimensions.gap, end = LiveDimensions.smallGap),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Box {
                NnagaIconButton(
                    onClick = { showDeviceChainMenu = true },
                    modifier = Modifier.size(LiveDimensions.hitTarget),
                ) {
                    Icon(
                        Icons.Default.MoreVert,
                        contentDescription = "Device chain options",
                        tint = LiveColors.textMuted,
                        modifier = Modifier.size(LiveDimensions.icon),
                    )
                }
                DropdownMenu(
                    expanded = showDeviceChainMenu,
                    onDismissRequest = { showDeviceChainMenu = false },
                ) {
                    DropdownMenuItem(
                        text = { Text("Save device chain") },
                        onClick = {
                            showDeviceChainMenu = false
                            onSaveChain(pathId)
                        },
                    )
                    DropdownMenuItem(
                        text = { Text("Load device chain") },
                        onClick = {
                            showDeviceChainMenu = false
                            onLoadChain(pathId)
                        },
                    )
                    DropdownMenuItem(
                        text = { Text("Create parallel dry/wet return") },
                        enabled = canCreateParallelReturn,
                        onClick = {
                            showDeviceChainMenu = false
                            onCreateParallelReturn(pathId)
                        },
                    )
                }
            }
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
            NnagaTextButton(
                onClick = { onBrowser(pathId) },
            ) {
                Icon(
                    Icons.Default.Add,
                    contentDescription = null,
                    modifier = Modifier.size(LiveDimensions.icon),
                )
                Text("ADD", modifier = Modifier.padding(start = LiveDimensions.smallGap))
            }
        }
        guidance?.let {
            Text(
                text = it,
                color = LiveColors.textMuted,
                style = MaterialTheme.typography.bodySmall,
                modifier = Modifier.padding(
                    start = LiveDimensions.gap,
                    end = LiveDimensions.gap,
                    bottom = LiveDimensions.smallGap,
                ),
            )
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
    musicalQuarterNotes: Double,
    bpm: Double,
    capturedAtMonotonicNanos: Long,
    onPlay: () -> Unit,
    onStop: () -> Unit,
    onRestart: () -> Unit,
    onBpm: () -> Unit,
    launchQuantization: TrackLaunchQuantization,
    onLaunchQuantizationClick: () -> Unit,
    onDeleteSelectedTrack: (() -> Unit)?,
) {
    val accent = MaterialTheme.colorScheme.primary
    val nowMonotonicNanos = rememberFrameClockNanos(playing)
    val stopped = !playing && positionSec <= 0.001
    val launchQuantizationLabel = when (launchQuantization) {
        TrackLaunchQuantization.Bar -> "1 BAR"
        TrackLaunchQuantization.Quarter -> "1/4"
        TrackLaunchQuantization.Eighth -> "1/8"
        TrackLaunchQuantization.Sixteenth -> "1/16"
        TrackLaunchQuantization.None -> "OFF"
    }
    var showMenu by remember { mutableStateOf(false) }
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
            NnagaIconButton(onClick = onRestart, modifier = Modifier.size(LiveDimensions.hitTarget)) {
                Icon(
                    Icons.Default.SkipPrevious,
                    contentDescription = "Restart transport",
                    tint = LiveColors.textMuted,
                    modifier = Modifier.size(LiveDimensions.icon),
                )
            }
            Text(
                text = "${formatMusicalPosition(interpolatedMusicalQuarterNotes(musicalQuarterNotes, bpm, playing, capturedAtMonotonicNanos, nowMonotonicNanos))} · " +
                    formatElapsedTime(interpolatedElapsedSeconds(positionSec, playing, capturedAtMonotonicNanos, nowMonotonicNanos)),
                color = LiveColors.textMuted,
                style = MaterialTheme.typography.labelSmall,
                fontWeight = FontWeight.Medium,
                maxLines = 1,
                modifier = Modifier.padding(start = LiveDimensions.smallGap).weight(1f),
            )
            NnagaTextButton(onClick = onBpm, modifier = Modifier.height(LiveDimensions.hitTarget)) {
                Text(
                    "${bpm.roundToInt()} BPM",
                    color = LiveColors.text,
                    style = MaterialTheme.typography.labelMedium,
                    fontWeight = FontWeight.SemiBold,
                )
            }
            Box {
                NnagaIconButton(
                    onClick = { showMenu = true },
                    modifier = Modifier.size(LiveDimensions.hitTarget),
                ) {
                    Icon(
                        Icons.Default.MoreVert,
                        contentDescription = "Transport options",
                        tint = LiveColors.textMuted,
                        modifier = Modifier.size(LiveDimensions.icon),
                    )
                }
                DropdownMenu(expanded = showMenu, onDismissRequest = { showMenu = false }) {
                    DropdownMenuItem(
                        text = { Text("Launch quantize · $launchQuantizationLabel") },
                        onClick = {
                            showMenu = false
                            onLaunchQuantizationClick()
                        },
                    )
                    onDeleteSelectedTrack?.let { onDelete ->
                        DropdownMenuItem(
                            text = { Text("Delete track") },
                            onClick = {
                                showMenu = false
                                onDelete()
                            },
                        )
                    }
                }
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
    bpm: Double,
    horizontalScrollState: androidx.compose.foundation.ScrollState,
    verticalScrollState: androidx.compose.foundation.ScrollState,
    modifier: Modifier,
    onSelectTrack: (RackTrackInfo) -> Unit,
    onSelect: (RackTrackInfo, Int) -> Unit,
    onLoad: (RackTrackInfo, Int) -> Unit,
    onLaunch: (RackTrackInfo, Int) -> Unit,
    onRecordClip: (RackTrackInfo, Int) -> Unit,
    onCancelRecording: (RackTrackInfo) -> Unit,
    onOpenClipSettings: (RackTrackInfo, ClipSlotInfo) -> Unit,
    onOpenSlotSettings: (RackTrackInfo, ClipSlotInfo) -> Unit,
    onTrackColor: (RackTrackInfo, Int) -> Unit,
    onStop: (RackTrackInfo) -> Unit,
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
                    onCancelRecording = { onCancelRecording(track) },
                    onOpenClipSettings = { clip -> onOpenClipSettings(track, clip) },
                    onOpenSlotSettings = { clip -> onOpenSlotSettings(track, clip) },
                )
            }
        }
        val activeClips = tracks.associateWith { track ->
            val slots = slotsByTrack[track.id].orEmpty()
            slots.firstOrNull { it.slot == track.activeSlot && it.playing }
                ?: slots.firstOrNull { it.playing || it.launchPending }
        }
        if (activeClips.values.any { it != null }) {
            Row(modifier = Modifier.fillMaxWidth().horizontalScroll(horizontalScrollState)) {
                tracks.forEach { track ->
                    TrackClipFooter(
                        clip = activeClips[track],
                        bpm = bpm,
                        onStop = { onStop(track) },
                    )
                }
            }
        }
    }
    }
}

@Composable
private fun TrackClipFooter(
    clip: ClipSlotInfo?,
    bpm: Double,
    onStop: (ClipSlotInfo) -> Unit,
) {
    val accent = MaterialTheme.colorScheme.primary
    val nowMonotonicNanos = rememberFrameClockNanos(clip?.playing == true)
    val timelineLength = if (clip?.looping == true) {
        clip.loopLengthQuarterNotes.takeIf { it.isFinite() && it > 0.0 } ?: 0.0
    } else if (clip != null) {
        clipSourceLengthQuarterNotes(clip)
    } else {
        0.0
    }
    val timelinePosition = if (clip != null && timelineLength > 0.0) {
        val displayPosition = clipDisplayPositionQuarterNotes(clip, bpm, nowMonotonicNanos)
        if (clip.looping) {
            (displayPosition - clip.loopStartQuarterNotes.coerceAtLeast(0.0))
                .coerceIn(0.0, timelineLength)
        } else {
            displayPosition.coerceIn(0.0, timelineLength)
        }
    } else {
        0.0
    }
    val progress = if (timelineLength > 0.0) {
        (timelinePosition / timelineLength).toFloat()
    } else {
        0f
    }
    val barNumber = if (timelineLength > 0.0) {
        (floor(timelinePosition / 4.0).toInt() + 1)
            .coerceAtMost(ceil(timelineLength / 4.0).toInt().coerceAtLeast(1))
    } else {
        1
    }

    Surface(
        color = LiveColors.raised,
        modifier = Modifier
            .requiredWidth(LiveDimensions.trackWidth)
            .height(LiveDimensions.hitTarget)
            .padding(end = LiveDimensions.hairline),
    ) {
        if (clip != null) {
            Row(
                modifier = Modifier.fillMaxSize().padding(horizontal = LiveDimensions.smallGap),
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.SpaceBetween,
            ) {
                NnagaIconButton(
                    onClick = { onStop(clip) },
                    modifier = Modifier
                        .size(LiveDimensions.hitTarget)
                        .semantics {
                            contentDescription = "Stop ${clip.displayName}"
                            stateDescription = when {
                                clip.playing -> "Playing"
                                clip.launchPending -> "Stop launch pending"
                                else -> "Stopped"
                            }
                        },
                ) {
                    Icon(
                        imageVector = Icons.Default.Stop,
                        contentDescription = null,
                        tint = accent,
                        modifier = Modifier.size(LiveDimensions.icon),
                    )
                }
                Box(
                    modifier = Modifier
                        .size(30.dp)
                        .semantics {
                            contentDescription = "Clip loop progress"
                            progressBarRangeInfo = ProgressBarRangeInfo(progress, 0f..1f)
                            stateDescription = "Bar $barNumber"
                        },
                    contentAlignment = Alignment.Center,
                ) {
                    Canvas(Modifier.fillMaxSize()) {
                        val strokeWidth = 2.dp.toPx()
                        drawCircle(
                            color = LiveColors.divider,
                            style = Stroke(width = strokeWidth),
                        )
                        drawArc(
                            color = accent,
                            startAngle = -90f,
                            sweepAngle = 360f * progress.coerceIn(0f, 1f),
                            useCenter = false,
                            style = Stroke(width = strokeWidth, cap = StrokeCap.Round),
                        )
                    }
                    Text(
                        text = barNumber.toString(),
                        color = accent,
                        fontSize = 10.sp,
                        fontWeight = FontWeight.Bold,
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
    onCancelRecording: () -> Unit,
    onOpenClipSettings: (ClipSlotInfo) -> Unit,
    onOpenSlotSettings: (ClipSlotInfo) -> Unit,
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
                    onCancelRecording = onCancelRecording,
                    onOpenClipSettings = onOpenClipSettings,
                    onOpenSlotSettings = onOpenSlotSettings,
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
                NnagaTextButton(onClick = { showPalette = false }) { Text("Cancel") }
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
    onCancelRecording: () -> Unit,
    onOpenClipSettings: (ClipSlotInfo) -> Unit,
    onOpenSlotSettings: (ClipSlotInfo) -> Unit,
) {
    val filled = slot.wavLoaded || slot.midiLoaded
    val ownsRecordingReservation = track.recordingSlot == slot.slot
    val waitingForPunch = ownsRecordingReservation && track.punchArmed
    val recordingPending = ownsRecordingReservation && track.recordPending
    val activelyRecording = ownsRecordingReservation && track.recording
    val recordingReservation = ownsRecordingReservation
    val playing = filled && !recordingReservation && slot.playing
    val recordAction = !filled && track.inputArmed
    val recordEnabled = recordAction &&
        track.recordingSlot < 0 &&
        !track.punchArmed &&
        !track.recordPending &&
        !track.recording
    val playIconAlpha = if (slot.launchPending && !recordingReservation) {
        val transition = rememberInfiniteTransition(label = "Pending clip launch")
        val alpha by transition.animateFloat(
            initialValue = 1f,
            targetValue = 0.35f,
            animationSpec = infiniteRepeatable(
                animation = tween(durationMillis = 650),
                repeatMode = RepeatMode.Reverse,
            ),
            label = "Pending clip launch alpha",
        )
        alpha
    } else {
        1f
    }
    val actionDescription = when {
        recordingReservation -> "Cancel recording"
        slot.launchPending && playing -> "Restart pending for ${slot.displayName}"
        slot.launchPending -> "Launch pending for ${slot.displayName}"
        playing -> "Restart ${slot.displayName}"
        filled -> "Launch ${slot.displayName}"
        recordAction && !recordEnabled -> "Recording unavailable"
        recordAction -> "Record into slot ${slot.slot + 1}"
        else -> "Load clip into slot ${slot.slot + 1}"
    }
    val clipState = when {
        waitingForPunch -> "Waiting for punch"
        recordingPending -> "Recording pending"
        activelyRecording -> "Recording"
        recordingReservation -> "Recording pending"
        slot.launchPending && playing -> "Restart pending"
        slot.launchPending -> "Launch pending"
        playing -> "Playing"
        filled -> "Loaded"
        recordAction && !recordEnabled -> "Recording unavailable"
        recordAction -> "Ready"
        else -> "Empty"
    }
    val accent = MaterialTheme.colorScheme.primary
    val background = when {
        !columnActive -> Color.Black
        recordingReservation -> LiveColors.record.copy(alpha = 0.18f)
        playing -> accent.copy(alpha = 0.18f)
        slot.launchPending -> accent.copy(alpha = 0.10f)
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
                onLongClickLabel = when {
                    recordingReservation -> null
                    filled -> "Open clip settings"
                    else -> "Open slot settings"
                },
                onLongClick = if (recordingReservation) {
                    null
                } else {
                    {
                        if (filled) onOpenClipSettings(slot) else onOpenSlotSettings(slot)
                    }
                },
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
                            recordingReservation -> LiveColors.record
                            playing -> accent
                            recordAction && selected -> LiveColors.record.copy(alpha = 0.6f)
                            slot.launchPending -> accent.copy(alpha = 0.75f)
                            selected -> accent.copy(alpha = 0.55f)
                            else -> Color.Transparent
                        },
                    ),
                )
                Text(
                    text = when {
                        waitingForPunch -> "Waiting for punch"
                        recordingPending -> "Recording pending"
                        activelyRecording -> "Recording"
                        recordingReservation -> "Recording pending"
                        filled -> slot.displayName
                        recordAction -> "Ready"
                        else -> ""
                    },
                    color = when {
                        recordingReservation || recordAction -> LiveColors.record
                        playing || slot.launchPending || selected -> accent
                        filled -> LiveColors.text
                        else -> LiveColors.textDim
                    },
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                    style = MaterialTheme.typography.labelMedium,
                    fontWeight = if (
                        playing || slot.launchPending || selected || recordingReservation
                    ) {
                        FontWeight.SemiBold
                    } else {
                        FontWeight.Normal
                    },
                    modifier = Modifier.padding(start = LiveDimensions.smallGap).weight(1f),
                )
                NnagaIconButton(
                    onClick = {
                        when {
                            recordingReservation -> onCancelRecording()
                            filled -> onLaunch(slot.slot)
                            recordAction -> onRecord(slot.slot)
                            else -> onLoad(slot.slot)
                        }
                    },
                    enabled = recordingReservation || !recordAction || recordEnabled,
                    modifier = Modifier
                        .size(LiveDimensions.hitTarget)
                        .semantics {
                            contentDescription = actionDescription
                            stateDescription = clipState
                        },
                ) {
                    Icon(
                        imageVector = when {
                            recordingReservation -> Icons.Default.Stop
                            filled && playing -> Icons.Default.Replay
                            filled -> Icons.Default.PlayArrow
                            recordAction -> Icons.Default.FiberManualRecord
                            else -> Icons.Default.Add
                        },
                        contentDescription = null,
                        tint = when {
                            recordingReservation || recordAction -> LiveColors.record
                            playing -> accent
                            filled -> LiveColors.textMuted
                            else -> LiveColors.textDim
                        },
                        modifier = Modifier.size(16.dp).alpha(playIconAlpha),
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
    onTrackInput: (RackTrackInfo) -> Unit,
    onTrackArm: (RackTrackInfo) -> Unit,
    onTrackArmLock: (RackTrackInfo) -> Unit,
    onOpenClipSettings: (RackTrackInfo, ClipSlotInfo) -> Unit,
    bpm: Double,
    modifier: Modifier,
) {
    val nowMonotonicNanos = rememberFrameClockNanos(clip?.playing == true)
    Surface(color = LiveColors.panel, modifier = modifier.fillMaxWidth()) {
        Column {
            Box(
                modifier = Modifier
                    .fillMaxWidth()
                    .height(LiveDimensions.hitTarget + LiveDimensions.gap),
            ) {
                TrackInspectorControls(
                    track = track,
                    onInputClick = { track?.let(onTrackInput) },
                    onArmClick = { track?.let(onTrackArm) },
                    onArmLockClick = { track?.let(onTrackArmLock) },
                )
            }
            Box(Modifier.fillMaxWidth().weight(1f)) {
                if (clip?.midiLoaded == true) {
                    PianoRoll(clip, notes, bpm, nowMonotonicNanos, track, onOpenClipSettings)
                } else {
                    Waveform(clip, peaks, bpm, nowMonotonicNanos, track, onOpenClipSettings)
                }
            }
        }
    }
}

@Composable
@OptIn(ExperimentalFoundationApi::class)
private fun TrackInspectorControls(
    track: RackTrackInfo?,
    onInputClick: () -> Unit,
    onArmClick: () -> Unit,
    onArmLockClick: () -> Unit,
) {
    if (track == null) {
        InspectorMessage("Select a track")
        return
    }
    var showArmLockMenu by remember { mutableStateOf(false) }
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
        NnagaTextButton(
            onClick = onInputClick,
        ) {
            Text(
                when (track.inputSourceKind) {
                    1 -> "TRK ${track.inputSourceTrackId} ${if (track.inputTap == 0) "PRE" else "POST"}"
                    2 -> "Hardware ${track.inputSourceFirstChannel + 1} (mono)"
                    else -> "Hardware ${track.inputSourceFirstChannel + 1}/${track.inputSourceFirstChannel + 2} (stereo)"
                },
                style = MaterialTheme.typography.labelSmall,
            )
        }
    }
}


@Composable
private fun InspectorMessage(message: String) {
    Box(
        modifier = Modifier.fillMaxSize().padding(horizontal = LiveDimensions.smallGap),
        contentAlignment = Alignment.CenterStart,
    ) {
        Text(message, color = LiveColors.textMuted, style = MaterialTheme.typography.bodySmall)
    }
}
private fun clipSourceBeatsPerMinute(clip: ClipSlotInfo): Double =
    clip.sourceBpm.takeIf { it.isFinite() && it > 0.0 } ?: 120.0

private fun clipSourceLengthQuarterNotes(clip: ClipSlotInfo): Double {
    val durationSeconds = clip.durationSec.takeIf { it.isFinite() && it > 0.0 } ?: return 0.0
    return durationSeconds * clipSourceBeatsPerMinute(clip) / 60.0
}

private fun clipTimelineLengthQuarterNotes(clip: ClipSlotInfo): Double {
    val loopEnd = (clip.loopStartQuarterNotes + clip.loopLengthQuarterNotes)
        .takeIf { it.isFinite() && it > 0.0 } ?: 0.0
    return clipSourceLengthQuarterNotes(clip).coerceAtLeast(loopEnd)
}

private fun clipDisplayPositionQuarterNotes(
    clip: ClipSlotInfo,
    bpm: Double,
    nowMonotonicNanos: Long,
): Double {
    val playbackQuarterNotes = interpolatedMusicalQuarterNotes(
        clip.musicalQuarterNotes,
        bpm,
        clip.playing,
        clip.capturedAtMonotonicNanos,
        nowMonotonicNanos,
    )
    val sourcePosition = when (ClipTempoMode.entries.getOrElse(clip.tempoMode) { ClipTempoMode.Original }) {
        ClipTempoMode.Original -> {
            if (bpm.isFinite() && bpm > 0.0) {
                playbackQuarterNotes * clipSourceBeatsPerMinute(clip) / bpm
            } else {
                playbackQuarterNotes
            }
        }
        ClipTempoMode.Stretch, ClipTempoMode.Repitch -> playbackQuarterNotes
    }
    return if (clip.looping && clip.loopLengthQuarterNotes.isFinite() && clip.loopLengthQuarterNotes > 0.0) {
        clip.loopStartQuarterNotes.coerceAtLeast(0.0) +
            sourcePosition.coerceAtLeast(0.0) % clip.loopLengthQuarterNotes
    } else {
        sourcePosition.coerceAtLeast(0.0)
    }
}

private fun DrawScope.drawMusicalGrid(durationQuarterNotes: Double, labelPaint: Paint) {
    if (durationQuarterNotes <= 0.0 || !durationQuarterNotes.isFinite()) return
    val quarterWidth = size.width / durationQuarterNotes.toFloat()
    if (!quarterWidth.isFinite() || quarterWidth <= 0f) return
    val beatCount = ceil(durationQuarterNotes).toInt().coerceAtLeast(0)
    val maxLines = size.width.toInt().coerceAtLeast(1)
    val beatStride = ceil((beatCount + 1).toDouble() / maxLines).toInt().coerceAtLeast(1)
    repeat(beatCount + 1) { beat ->
        val isBar = beat % 4 == 0
        if (!isBar && beat % beatStride != 0) return@repeat
        val x = beat * quarterWidth
        drawLine(
            color = if (isBar) LiveColors.text.copy(alpha = 0.8f) else LiveColors.divider,
            start = Offset(x, 0f),
            end = Offset(x, size.height),
            strokeWidth = if (isBar) 3f else 1.75f,
        )
        if (isBar) {
            drawIntoCanvas { canvas ->
                canvas.nativeCanvas.drawText(
                    (beat / 4 + 1).toString(),
                    x + 2f,
                    labelPaint.textSize + 1f,
                    labelPaint,
                )
            }
        }
    }
}

@Composable
private fun ClipPositionOverlay(
    clip: ClipSlotInfo,
    bpm: Double,
    nowMonotonicNanos: Long,
    onOpenSettings: () -> Unit,
    modifier: Modifier = Modifier,
) {
    val positionSec = interpolatedElapsedSeconds(
        clip.positionSec.takeIf { it.isFinite() && it >= 0.0 } ?: 0.0,
        clip.playing,
        clip.capturedAtMonotonicNanos,
        nowMonotonicNanos,
    )
    val positionQuarterNotes = clipDisplayPositionQuarterNotes(clip, bpm, nowMonotonicNanos)
    val durationQuarterNotes = clipTimelineLengthQuarterNotes(clip)
    val mediaType = if (clip.midiLoaded) "MIDI" else "WAV"
    val loopEnd = clip.loopStartQuarterNotes + clip.loopLengthQuarterNotes
    val details = buildString {
        append(clip.displayName.ifBlank { "Untitled clip" })
        append(" · Slot ${clip.slot + 1} · $mediaType")
        append(" · ${formatMusicalPosition(positionQuarterNotes)} · ${formatElapsedTime(positionSec)}")
        append(" · Length ${formatMusicalPosition(durationQuarterNotes)}")
        append(" · Loop ${if (clip.looping) "on" else "off"} ${formatMusicalPosition(clip.loopStartQuarterNotes)}–${formatMusicalPosition(loopEnd)}")
        append(" · ${if (clip.playing) "Playing" else "Stopped"}")
        append(" · ${if (clip.active) "Active" else "Inactive"}")
        if (clip.wavLoaded) {
            append(" · Base BPM ${"%.2f".format(clip.sourceBpm)}")
            append(" · Tempo ${ClipTempoMode.entries.getOrElse(clip.tempoMode) { ClipTempoMode.Original }.name}")
        }
    }
    Box(
        modifier = modifier
            .fillMaxWidth()
            .background(LiveColors.panel.copy(alpha = 0.82f))
            .clickable(role = Role.Button, onClick = onOpenSettings)
            .semantics {
                contentDescription =
                    "Clip information for ${clip.displayName.ifBlank { "Untitled clip" }}. Open clip settings."
            },
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
}
@Composable
private fun Waveform(
    clip: ClipSlotInfo?,
    peaks: List<Float>,
    bpm: Double,
    nowMonotonicNanos: Long,
    track: RackTrackInfo?,
    onOpenClipSettings: (RackTrackInfo, ClipSlotInfo) -> Unit,
) {
    if (clip?.wavLoaded != true) return
    val accent = MaterialTheme.colorScheme.primary
    val density = LocalDensity.current
    val gridLabelPaint = remember(density) {
        Paint(Paint.ANTI_ALIAS_FLAG).apply {
            color = LiveColors.textMuted.toArgb()
            textSize = with(density) { 9.dp.toPx() }
            alpha = 210
        }
    }
    Box(Modifier.fillMaxSize()) {
        Canvas(
            modifier = Modifier
                .fillMaxSize()
                .padding(horizontal = LiveDimensions.smallGap),
        ) {
            val timelineQuarterNotes = clipTimelineLengthQuarterNotes(clip)
            val contentQuarterNotes = clipSourceLengthQuarterNotes(clip)
            val mid = size.height / 2
            drawLine(LiveColors.waveformLine, Offset(0f, mid), Offset(size.width, mid), 1f)
            if (timelineQuarterNotes > 0.0) {
                val left = size.width *
                    (clip.loopStartQuarterNotes / timelineQuarterNotes).toFloat().coerceIn(0f, 1f)
                val loopEnd = clip.loopStartQuarterNotes + clip.loopLengthQuarterNotes
                val right = size.width * (loopEnd / timelineQuarterNotes).toFloat().coerceIn(0f, 1f)
                drawRect(
                    accent.copy(alpha = 0.12f),
                    Offset(left, 0f),
                    Size((right - left).coerceAtLeast(0f), size.height),
                )
                drawLine(accent.copy(alpha = 0.75f), Offset(left, 0f), Offset(left, size.height), 1.5f)
                drawLine(accent.copy(alpha = 0.75f), Offset(right, 0f), Offset(right, size.height), 1.5f)
            }
            drawMusicalGrid(timelineQuarterNotes, gridLabelPaint)
            if (peaks.isNotEmpty() && timelineQuarterNotes > 0.0 && contentQuarterNotes > 0.0) {
                val audioWidth = (
                    size.width * (contentQuarterNotes / timelineQuarterNotes).toFloat()
                ).coerceIn(0f, size.width)
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
            if (timelineQuarterNotes > 0.0) {
                val displayQuarterNotes = clipDisplayPositionQuarterNotes(clip, bpm, nowMonotonicNanos)
                val x = size.width *
                    (displayQuarterNotes / timelineQuarterNotes).toFloat().coerceIn(0f, 1f)
                drawLine(accent, Offset(x, 0f), Offset(x, size.height), 2f)
            }
        }
        ClipPositionOverlay(
            clip = clip,
            bpm = bpm,
            nowMonotonicNanos = nowMonotonicNanos,
            onOpenSettings = {
                if (track != null) onOpenClipSettings(track, clip)
            },
            modifier = Modifier.align(Alignment.BottomStart),
        )
    }
}

@Composable
private fun PianoRoll(
    clip: ClipSlotInfo,
    notes: List<MidiNoteInfo>,
    bpm: Double,
    nowMonotonicNanos: Long,
    track: RackTrackInfo?,
    onOpenClipSettings: (RackTrackInfo, ClipSlotInfo) -> Unit,
) {
    val accent = MaterialTheme.colorScheme.primary
    val density = LocalDensity.current
    val gridLabelPaint = remember(density) {
        Paint(Paint.ANTI_ALIAS_FLAG).apply {
            color = LiveColors.textMuted.toArgb()
            textSize = with(density) { 9.dp.toPx() }
            alpha = 210
        }
    }
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
            val timelineQuarterNotes = clipTimelineLengthQuarterNotes(clip)
            val loopLeft = size.width *
                (clip.loopStartQuarterNotes / timelineQuarterNotes.coerceAtLeast(1e-9)).toFloat()
                    .coerceIn(0f, 1f)
            val loopEnd = clip.loopStartQuarterNotes + clip.loopLengthQuarterNotes
            val loopRight = size.width *
                (loopEnd / timelineQuarterNotes.coerceAtLeast(1e-9)).toFloat().coerceIn(0f, 1f)
            drawRect(
                accent.copy(alpha = 0.12f),
                Offset(loopLeft, 0f),
                Size((loopRight - loopLeft).coerceAtLeast(0f), size.height),
            )
            drawLine(accent.copy(alpha = 0.75f), Offset(loopLeft, 0f), Offset(loopLeft, size.height), 1.5f)
            drawLine(accent.copy(alpha = 0.75f), Offset(loopRight, 0f), Offset(loopRight, size.height), 1.5f)
            drawMusicalGrid(timelineQuarterNotes, gridLabelPaint)
            val sourceBpm = clipSourceBeatsPerMinute(clip)
            notes.forEach { note ->
                val noteStartQuarterNotes = note.startMicroseconds * sourceBpm / 60_000_000.0
                val noteLengthQuarterNotes =
                    note.durationMicroseconds.coerceAtLeast(1L) * sourceBpm / 60_000_000.0
                val x = (
                    noteStartQuarterNotes / timelineQuarterNotes.coerceAtLeast(1e-9)
                ).toFloat().coerceIn(0f, 1f) * size.width
                val width = (
                    noteLengthQuarterNotes / timelineQuarterNotes.coerceAtLeast(1e-9) * size.width
                ).toFloat().coerceAtLeast(4f)
                val y = size.height - ((note.pitch.coerceIn(48, 83) - 47) / 36f) * size.height
                drawRect(
                    LiveColors.midi,
                    Offset(x, y - size.height / rows + 2f),
                    Size(width, size.height / rows - 4f),
                )
            }
            if (timelineQuarterNotes > 0.0) {
                val displayQuarterNotes = clipDisplayPositionQuarterNotes(clip, bpm, nowMonotonicNanos)
                val x = size.width *
                    (displayQuarterNotes / timelineQuarterNotes).toFloat().coerceIn(0f, 1f)
                drawLine(accent, Offset(x, 0f), Offset(x, size.height), 2f)
            }
        }
        ClipPositionOverlay(
            clip = clip,
            bpm = bpm,
            nowMonotonicNanos = nowMonotonicNanos,
            onOpenSettings = {
                if (track != null) onOpenClipSettings(track, clip)
            },
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
                NnagaIconButton(onClick = onAdd, modifier = Modifier.size(LiveDimensions.hitTarget)) {
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

