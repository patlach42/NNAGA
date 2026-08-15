package com.vibes.dsp.ui.live

import android.net.Uri
import android.os.Build
import androidx.compose.foundation.ExperimentalFoundationApi
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.combinedClickable
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.RadioButton
import androidx.compose.material3.Checkbox
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.ui.text.input.KeyboardType
import com.vibes.dsp.engine.DirectUsbAudioManager
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.verticalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.requiredWidth
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.foundation.layout.navigationBars
import androidx.compose.foundation.layout.windowInsetsPadding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.ui.platform.LocalView
import androidx.compose.ui.draw.alpha
import androidx.compose.material.icons.filled.FiberManualRecord
import androidx.compose.material.icons.filled.Mic
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Add
import androidx.compose.material.icons.filled.Pause
import androidx.compose.material.icons.filled.PlayArrow
import androidx.compose.material.icons.filled.SkipPrevious
import androidx.compose.material.icons.filled.Stop
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Button
import androidx.compose.material3.NavigationBar
import androidx.compose.material3.NavigationBarItem
import androidx.compose.material3.Slider
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Surface
import androidx.compose.material3.Tab
import androidx.compose.material3.TabRow
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableLongStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.rotate
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.selected
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.vibes.dsp.engine.ClipSlotInfo
import com.vibes.dsp.engine.DirectUsbSessionState
import com.vibes.dsp.engine.MidiNoteInfo
import com.vibes.dsp.engine.RackPathId
import com.vibes.dsp.engine.RackTrackInfo
import com.vibes.dsp.engine.TrackLaunchQuantization
import com.vibes.dsp.ui.rack.RackViewModel
import com.vibes.dsp.ui.rack.PluginCard
import com.vibes.dsp.ui.settings.AudioSettingsScreen
import kotlin.math.roundToInt

private enum class InspectorTab { Clips, Track, Devices }

private data class CutoutHorizontalAvoidance(val start: Int = 0, val end: Int = 0)

private fun cutoutHorizontalAvoidance(view: android.view.View): CutoutHorizontalAvoidance {
    if (Build.VERSION.SDK_INT < Build.VERSION_CODES.P) return CutoutHorizontalAvoidance()
    val insets = view.rootWindowInsets ?: return CutoutHorizontalAvoidance()
    val bounds = insets.displayCutout?.boundingRects.orEmpty()
    if (bounds.isEmpty() || view.width <= 0) return CutoutHorizontalAvoidance()
    val topBounds = bounds.filter { it.top <= 1 && it.width() > 0 }
    if (topBounds.isEmpty()) return CutoutHorizontalAvoidance()
    return CutoutHorizontalAvoidance(
        start = topBounds.maxOf { it.left },
        end = topBounds.maxOf { view.width - it.right },
    )
}

private object LiveColors {
    val background = Color(0xFF15181D)
    val panel = Color(0xFF1B1F25)
    val raised = Color(0xFF20252C)
    val card = Color(0xFF292F37)
    val selected = Color(0xFF33404A)
    val playing = Color(0xFF2E6452)
    val selectedTrack = Color(0xFF2B4038)
    val divider = Color(0xFF35404D)
    val waveformLine = Color(0xFF46515F)
    val text = Color(0xFFE6E9ED)
    val textMuted = Color(0xFFAAB3BE)
    val textDim = Color(0xFF7F8A98)
    val live = Color(0xFF8ED7A4)
    val audio = Color(0xFF67C6B3)
    val midi = Color(0xFF9B7EEA)
    val playhead = Color(0xFFFFD166)
}

private object LiveDimensions {
    val action = 48.dp
    val transport = 56.dp
    val nav = 64.dp
    val trackWidth = 152.dp
    val slotHeight = 72.dp
    val mixerChannel = 72.dp
    val radius = 6.dp
    val gap = 8.dp
    val smallGap = 4.dp
}

@Composable
@OptIn(ExperimentalFoundationApi::class)
fun LiveScreen(
    viewModel: RackViewModel,
    onNavigateToBrowser: (Long, Int) -> Unit,
    onNavigateToSettings: () -> Unit = {},
    onNavigateToTone3000: (String?, String?, String?, Int, String?) -> Unit = { _, _, _, _, _ -> }
) {
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
    val inputChannelCount = DirectUsbAudioManager.getInputChannelCount()
    var launchQuantizationOrdinal by rememberSaveable { mutableIntStateOf(0) }
    val launchQuantization = TrackLaunchQuantization.entries[launchQuantizationOrdinal]
    var loopLengthsByTrack by rememberSaveable { mutableStateOf<Map<Long, Double>>(emptyMap()) }
    var showTempoDialog by rememberSaveable { mutableStateOf(false) }
    var tempoInput by rememberSaveable { mutableStateOf("") }
    val errorMessage by viewModel.errorMessage.collectAsState()
    val blockingOperation by viewModel.blockingOperation.collectAsState()
    val slotsByTrack by viewModel.clipSlots.collectAsState()
    val peaksByTrack by viewModel.waveformPeaks.collectAsState()
    val notesByClip by viewModel.midiNotes.collectAsState()
    var selectedTrackId by rememberSaveable { mutableLongStateOf(0L) }
    var selectedSlot by rememberSaveable { mutableIntStateOf(0) }
    var tab by rememberSaveable { mutableIntStateOf(InspectorTab.Clips.ordinal) }
    var liveMode by rememberSaveable { mutableStateOf("Session") }
    var importTarget by remember { mutableStateOf<Pair<Long, Int>?>(null) }
    var inputMenuTrack by remember { mutableStateOf<RackTrackInfo?>(null) }
    var recordMenuTrack by remember { mutableStateOf<RackTrackInfo?>(null) }
    var showQuantizationMenu by remember { mutableStateOf(false) }
    val supportedLoopLengths = listOf(0.25 to "1/4", 1.0 to "1 bar", 2.0 to "2 bars", 4.0 to "4 bars", 8.0 to "8 bars", 16.0 to "16 bars")
    val launcherHorizontalScrollState = rememberScrollState()
    val launcherVerticalScrollState = rememberScrollState()
    val mixerScrollState = rememberScrollState()

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
                                }
                            )
                        }
                    }
                }
            },
            confirmButton = { TextButton(onClick = { inputMenuTrack = null }) { Text("Cancel") } }
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
                            verticalAlignment = Alignment.CenterVertically
                        ) {
                            RadioButton(
                                selected = (loopLengthsByTrack[track.id] ?: 1.0) == bars,
                                onClick = {
                                    loopLengthsByTrack = loopLengthsByTrack + (track.id to bars)
                                }
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
                                    armed = it
                                )
                                recordMenuTrack = null
                            }
                        )
                        Text("Enter on punch")
                    }
                }
            },
            confirmButton = { TextButton(onClick = { recordMenuTrack = null }) { Text("Done") } }
        )
    }
    if (showQuantizationMenu) AlertDialog(onDismissRequest = { showQuantizationMenu = false }, title = { Text("Launch quantization") },
        text = { Column { TrackLaunchQuantization.entries.forEachIndexed { i, q -> Row(Modifier.fillMaxWidth().clickable { launchQuantizationOrdinal = i; showQuantizationMenu = false }, verticalAlignment = Alignment.CenterVertically) { RadioButton(launchQuantization == q, { launchQuantizationOrdinal = i; showQuantizationMenu = false }); Text(q.name) } } } },
        confirmButton = { TextButton(onClick = { showQuantizationMenu = false }) { Text("Cancel") } })
    if (showTempoDialog) AlertDialog(onDismissRequest = { showTempoDialog = false }, title = { Text("Tempo") },
        text = { OutlinedTextField(value = tempoInput, onValueChange = { tempoInput = it }, keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Decimal), suffix = { Text(" BPM") }) },
        confirmButton = { TextButton(onClick = { tempoInput.toDoubleOrNull()?.coerceIn(20.0, 400.0)?.let(viewModel::setTransportBpm); showTempoDialog = false }) { Text("Apply") } },
        dismissButton = { TextButton(onClick = { showTempoDialog = false }) { Text("Cancel") } })

    Scaffold(
        modifier = Modifier.fillMaxSize(),
        containerColor = LiveColors.background,
        contentWindowInsets = WindowInsets(0, 0, 0, 0),
        bottomBar = {
            LiveNavigation(
                mode = liveMode,
                onDevices = { liveMode = "Devices" },
                onSession = { liveMode = "Session" },
            )
        },
    ) { contentPadding ->
        if (liveMode == "Devices") {
            DevicesSurface(
                devices = selectedPlugins,
                pathId = selectedPath,
                viewModel = viewModel,
                onBrowser = { path -> onNavigateToBrowser(path, -1) },
                onSettings = onNavigateToSettings,
            )
        } else {
            BoxWithConstraints(
                modifier = Modifier
                    .fillMaxSize()
                    .padding(contentPadding)
            ) {
                val inspectorHeight = if (maxHeight < 720.dp) 144.dp else 164.dp
                val mixerHeight = if (maxHeight < 720.dp) 176.dp else 200.dp
                Column(modifier = Modifier.fillMaxSize()) {
                    TransportBar(
                        playing = transport.playing,
                        bpm = transport.beatsPerMinute,
                        onPlay = { if (transport.playing) viewModel.transportPause() else viewModel.transportPlay() },
                        onRestart = viewModel::transportRestart,
                        onSettings = onNavigateToSettings,
                        onBrowser = { path -> onNavigateToBrowser(path, -1) },
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
                    Launcher(
                        tracks = tracks,
                        slotsByTrack = slotsByTrack,
                        selectedTrack = selectedTrack,
                        selectedSlot = selectedSlot,
                        horizontalScrollState = launcherHorizontalScrollState,
                        verticalScrollState = launcherVerticalScrollState,
                        modifier = Modifier.weight(1f),
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
                            val active = track.playing && slotsByTrack[track.id]?.firstOrNull { it.slot == slot }?.active == true
                            if (active) viewModel.setTrackTransportPlaying(track.id, false, launchQuantization)
                            else viewModel.launchTrackTransport(track.id, launchQuantization, !transport.playing)
                        },
                        onArm = { track -> viewModel.setTrackInputArmed(track.id, !track.inputArmed) },
                        onRecord = { track ->
                            val bars = loopLengthsByTrack[track.id] ?: 1.0
                            viewModel.setTrackTransportLooping(track.id, true)
                            viewModel.startTrackLoopRecording(
                                track.id,
                                bars,
                                launchQuantization,
                                startGlobal = !transport.playing
                            )
                        },
                        onDelete = viewModel::removeTrack,
                        onInputMenu = { inputMenuTrack = it },
                        onQuantizeMenu = { showQuantizationMenu = true },
                        onRecordMenu = { recordMenuTrack = it },
                    )
                    ClipInspector(
                        track = selectedTrack,
                        clip = selectedClip,
                        peaks = peaksByTrack[selectedTrack?.id].orEmpty(),
                        notes = notesByClip[selectedTrack?.id to selectedSlot].orEmpty(),
                        tab = InspectorTab.entries[tab],
                        onTab = { tab = it.ordinal },
                        onSettings = onNavigateToSettings,
                        onBrowser = { path -> onNavigateToBrowser(path, -1) },
                        devices = selectedPlugins,
                        viewModel = viewModel,
                        selectedPath = selectedPath,
                        engineRunning = engineRunning,
                        errorMessage = errorMessage,
                        blockingOperation = blockingOperation,
                        modifier = Modifier.height(inspectorHeight),
                    )
                    Mixer(
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
                        modifier = Modifier.height(mixerHeight),
                    )
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
    onSettings: () -> Unit,
    onBrowser: (Long) -> Unit,
    onBpm: () -> Unit,
) {
    val view = LocalView.current
    val density = LocalDensity.current
    val avoidance = cutoutHorizontalAvoidance(view)
    val startAvoidance = with(density) { avoidance.start.toDp() }
    val endAvoidance = with(density) { avoidance.end.toDp() }

    Surface(color = LiveColors.panel) {
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .height(LiveDimensions.transport)
                .padding(
                    start = LiveDimensions.gap + startAvoidance,
                    end = LiveDimensions.gap + endAvoidance,
                ),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            IconButton(onClick = onPlay, modifier = Modifier.size(LiveDimensions.action)) {
                Icon(
                    imageVector = if (playing) Icons.Default.Pause else Icons.Default.PlayArrow,
                    contentDescription = if (playing) "Pause transport" else "Play transport",
                    tint = LiveColors.live,
                )
            }
            IconButton(onClick = onRestart, modifier = Modifier.size(LiveDimensions.action)) {
                Icon(Icons.Default.SkipPrevious, contentDescription = "Restart transport")
            }
            Text(
                text = "LIVE",
                color = LiveColors.text,
                fontWeight = FontWeight.Black,
                letterSpacing = 2.sp,
                modifier = Modifier.padding(start = LiveDimensions.gap),
            )
            TextButton(
                onClick = onBpm,
                modifier = Modifier.height(LiveDimensions.action),
            ) {
                Text("${bpm.roundToInt()} BPM")
            }
            TextButton(onClick = onSettings, modifier = Modifier.height(LiveDimensions.action)) {
                Text("SETTINGS")
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
    horizontalScrollState: androidx.compose.foundation.ScrollState,
    verticalScrollState: androidx.compose.foundation.ScrollState,
    modifier: Modifier,
    onSelect: (RackTrackInfo, Int) -> Unit,
    onLoad: (RackTrackInfo, Int) -> Unit,
    onLaunch: (RackTrackInfo, Int) -> Unit,
    onArm: (RackTrackInfo) -> Unit,
    onRecord: (RackTrackInfo) -> Unit,
    onDelete: (Long) -> Unit,
    onInputMenu: (RackTrackInfo) -> Unit,
    onQuantizeMenu: () -> Unit,
    onRecordMenu: (RackTrackInfo) -> Unit,
) {
    Column(modifier = modifier.fillMaxWidth()) {
        Text("CLIP LAUNCHER", color = LiveColors.textMuted, style = MaterialTheme.typography.labelMedium,
            modifier = Modifier.padding(horizontal = LiveDimensions.gap, vertical = LiveDimensions.smallGap))
        Column(
            modifier = Modifier
                .weight(1f)
                .fillMaxWidth()
                .verticalScroll(verticalScrollState)
        ) {
            Row(modifier = Modifier.horizontalScroll(horizontalScrollState)) {
                tracks.forEachIndexed { index, track ->
                    TrackSlots(track, index, slotsByTrack[track.id].orEmpty(), track.id == selectedTrack?.id,
                        selectedSlot, { onSelect(track, it) }, { onLoad(track, it) }, { onLaunch(track, it) },
                        { onArm(track) }, { onRecord(track) }, { onDelete(track.id) }, { onInputMenu(track) },
                        onQuantizeMenu, { onRecordMenu(track) })
                }
            }
        }
    }
}

@Composable
private fun TrackSlots(
    track: RackTrackInfo, index: Int, slots: List<ClipSlotInfo>, selected: Boolean, selectedSlot: Int,
    onSelect: (Int) -> Unit, onLoad: (Int) -> Unit, onLaunch: (Int) -> Unit,
    onArm: () -> Unit, onRecord: () -> Unit, onDelete: () -> Unit,
    onInputMenu: () -> Unit, onQuantizeMenu: () -> Unit, onRecordMenu: () -> Unit,
) {
    val visibleCount = maxOf(8, (slots.maxOfOrNull { it.slot + 1 } ?: 0) + 4)
    val bySlot = slots.associateBy { it.slot }
    Column(Modifier.requiredWidth(LiveDimensions.trackWidth).padding(end = LiveDimensions.gap)) {
        TrackHeader(index, track, selected, track.playing, onArm, onRecord, onDelete, onInputMenu, onRecordMenu)
        repeat(visibleCount) { slotIndex ->
            val slot = bySlot[slotIndex] ?: ClipSlotInfo(track.id, slotIndex, false, false, "", 0.0, false)
            ClipCard(track, slot, selected && slotIndex == selectedSlot, onSelect, onLoad, onLaunch, onQuantizeMenu)
        }
    }
}

@OptIn(ExperimentalFoundationApi::class)
@Composable
private fun TrackHeader(
    index: Int, track: RackTrackInfo, selected: Boolean, playing: Boolean,
    onArm: () -> Unit, onRecord: () -> Unit, onDelete: () -> Unit,
    onInputMenu: () -> Unit, onRecordMenu: () -> Unit,
) {
    var menu by remember { mutableStateOf(false) }
    val stateText = when { playing -> "PLAYING"; selected -> "SELECTED"; else -> null }
    Row(
        Modifier.fillMaxWidth().height(48.dp).combinedClickable(onClick = {}, onLongClick = { menu = true }).padding(horizontal = LiveDimensions.gap),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Text("TRACK ${index + 1}", color = if (selected) LiveColors.text else LiveColors.textMuted,
            fontWeight = if (selected) FontWeight.Bold else FontWeight.Medium, style = MaterialTheme.typography.labelMedium,
            modifier = Modifier.weight(1f))
        if (stateText != null) Text(stateText, color = if (playing) LiveColors.live else LiveColors.audio, style = MaterialTheme.typography.labelSmall)
        IconButton(onClick = onArm, modifier = Modifier.size(48.dp)) {
            Icon(Icons.Default.Mic, if (track.inputArmed) "Disarm track input" else "Arm track input", tint = if (track.inputArmed) LiveColors.live else LiveColors.textMuted)
        }
        IconButton(onClick = onRecord, enabled = track.inputArmed && !track.recordPending && !track.recording, modifier = Modifier.size(48.dp)) {
            Icon(Icons.Default.FiberManualRecord, "Record", tint = if (track.recording || track.recordPending) LiveColors.live else LiveColors.textMuted)
        }
    }
    DropdownMenu(expanded = menu, onDismissRequest = { menu = false }) {
        DropdownMenuItem(text = { Text("Delete track") }, onClick = { menu = false; onDelete() })
        DropdownMenuItem(text = { Text("Input channel") }, onClick = { menu = false; onInputMenu() })
        DropdownMenuItem(text = { Text("Loop length / enter on punch") }, onClick = { menu = false; onRecordMenu() })
    }
}

@OptIn(ExperimentalFoundationApi::class)
@Composable
private fun ClipCard(
    track: RackTrackInfo, slot: ClipSlotInfo, selected: Boolean,
    onSelect: (Int) -> Unit, onLoad: (Int) -> Unit, onLaunch: (Int) -> Unit,
    onQuantizeMenu: () -> Unit,
) {
    val filled = slot.wavLoaded || slot.midiLoaded
    val playing = filled && track.playing && slot.active
    val background = when { playing -> LiveColors.playing; selected -> LiveColors.selected; else -> LiveColors.card }
    val type = when { slot.midiLoaded -> "MIDI"; slot.wavLoaded -> "AUDIO"; else -> "EMPTY SLOT" }
    val accent = if (slot.midiLoaded) LiveColors.midi else LiveColors.audio
    Card(
        modifier = Modifier.fillMaxWidth().height(LiveDimensions.slotHeight).padding(vertical = LiveDimensions.smallGap)
            .semantics { this.selected = selected }
            .combinedClickable(role = Role.Button, onClick = { onSelect(slot.slot) },
                onLongClick = if (filled) onQuantizeMenu else null),
        colors = CardDefaults.cardColors(containerColor = background), shape = RoundedCornerShape(LiveDimensions.radius),
    ) {
        Row(Modifier.fillMaxSize().padding(start = LiveDimensions.gap), verticalAlignment = Alignment.CenterVertically) {
            Box(Modifier.width(LiveDimensions.smallGap).height(36.dp).background(if (filled) accent else LiveColors.divider))
            Column(Modifier.padding(start = LiveDimensions.gap).weight(1f)) {
                Text(if (filled) slot.displayName else "Empty clip", color = LiveColors.text, maxLines = 1, overflow = TextOverflow.Ellipsis, style = MaterialTheme.typography.bodyMedium)
                Text(if (playing) "$type · PLAYING" else type, color = if (playing) LiveColors.live else LiveColors.textMuted, style = MaterialTheme.typography.labelSmall)
            }
            IconButton(onClick = { if (filled) onLaunch(slot.slot) else onLoad(slot.slot) }, modifier = Modifier.size(LiveDimensions.action)) {
                Icon(if (!filled) Icons.Default.Add else if (playing) Icons.Default.Pause else Icons.Default.PlayArrow,
                    if (!filled) "Load clip into slot ${slot.slot + 1}" else if (playing) "Stop ${slot.displayName}" else "Launch ${slot.displayName}",
                    tint = if (filled) LiveColors.text else LiveColors.textMuted)
            }
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
    onSettings: () -> Unit,
    onBrowser: (Long) -> Unit,
    devices: List<com.vibes.dsp.ui.rack.RackPlugin>,
    viewModel: RackViewModel,
    selectedPath: Long,
    engineRunning: Boolean,
    errorMessage: String?,
    blockingOperation: String?,
    modifier: Modifier,
) {
    Surface(color = LiveColors.panel, modifier = modifier.fillMaxWidth()) {
        Column {
            Row(
                modifier = Modifier.fillMaxWidth().padding(horizontal = LiveDimensions.gap),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Text(
                    text = clip?.takeIf { it.wavLoaded || it.midiLoaded }?.displayName ?: "CLIP INSPECTOR",
                    color = LiveColors.text,
                    fontWeight = FontWeight.SemiBold,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                    modifier = Modifier.weight(1f),
                )
                Text(
                    text = track?.let { "${(it.volume * 100).roundToInt()}%" } ?: "NO TRACK",
                    color = LiveColors.textMuted,
                    style = MaterialTheme.typography.labelSmall,
                )
            }
            TabRow(selectedTabIndex = tab.ordinal, containerColor = LiveColors.raised) {
                InspectorTab.entries.forEach { item ->
                    Tab(
                        selected = tab == item,
                        onClick = { onTab(item) },
                        text = { Text(item.name.uppercase()) },
                    )
                }
            }
            when (tab) {
                InspectorTab.Clips -> when {
                    clip?.midiLoaded == true -> PianoRoll(clip, notes)
                    else -> Waveform(track, clip, peaks)
                }
                InspectorTab.Track -> InspectorMessage(
                    track?.let { "Input ${it.inputChannel + 1} · Volume ${(it.volume * 100).roundToInt()}%" }
                        ?: "Select a track to inspect its input and level",
                )
                InspectorTab.Devices -> Column(modifier = Modifier.fillMaxSize().padding(LiveDimensions.gap)) {
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        Text("DEVICES · ${track?.let { "TRACK ${it.id}" } ?: "MASTER"}", color = LiveColors.text, modifier = Modifier.weight(1f))
                        TextButton(onClick = { onBrowser(track?.id ?: -1L) }) { Text("ADD") }
                    }
                    androidx.compose.foundation.lazy.LazyColumn(modifier = Modifier.weight(1f)) {
                        items(devices.size) { index ->
                            val plugin = devices[index]
                            PluginCard(plugin = plugin, pluginIndex = index, pathId = selectedPath, viewModel = viewModel,
                                onRemove = { viewModel.removePlugin(selectedPath, index) },
                                onReplace = { onBrowser(selectedPath) }, modifier = Modifier.padding(vertical = 2.dp))
                        }
                    }
                    TextButton(onClick = onSettings) { Text("USB AUDIO CONFIGURATION") }
                    if (engineRunning) Text("Engine running", color = LiveColors.live)
                    errorMessage?.let { Text(it, color = MaterialTheme.colorScheme.error) }
                    blockingOperation?.let { Text(it, color = LiveColors.playhead) }
                }
            }
        }
    }
}

@Composable
private fun InspectorMessage(message: String) {
    Box(modifier = Modifier.fillMaxSize().padding(LiveDimensions.gap), contentAlignment = Alignment.CenterStart) {
        Text(message, color = LiveColors.textMuted)
    }
}

@Composable
private fun Waveform(track: RackTrackInfo?, clip: ClipSlotInfo?, peaks: List<Float>) {
    if (track == null || clip?.wavLoaded != true) {
        InspectorMessage("Select an audio or MIDI clip to inspect it")
        return
    }
    Canvas(modifier = Modifier.fillMaxSize().padding(LiveDimensions.gap)) {
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
            drawLine(LiveColors.playhead, Offset(x, 0f), Offset(x, size.height), 2f)
        }
    }
}

@Composable
private fun PianoRoll(clip: ClipSlotInfo, notes: List<MidiNoteInfo>) {
    Canvas(modifier = Modifier.fillMaxSize().padding(LiveDimensions.gap)) {
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
    Surface(color = LiveColors.panel, modifier = Modifier.fillMaxWidth()) {
        Column(Modifier.padding(horizontal = LiveDimensions.gap, vertical = LiveDimensions.smallGap)) {
            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(LiveDimensions.gap)) {
                Button(onClick = onToggleEngine) { Text(if (engineRunning) "STOP" else "START") }
                Text("IN ${(meterState.inputLevel * 100).roundToInt()}%", color = if (meterState.inputClipping) Color.Red else LiveColors.audio)
                Text("OUT ${(meterState.outputLevel * 100).roundToInt()}%", color = if (meterState.outputClipping) Color.Red else LiveColors.audio)
                TextButton(onClick = onResetClipping) { Text("RESET CLIP") }
                Text("${latencyMs.roundToInt()}ms CPU ${(cpuLoad * 100).roundToInt()}% XR $xRunCount", color = LiveColors.textMuted)
            }
            Text("USB: ${usbState.name}  ${usbStats.sampleRateHz}Hz", color = LiveColors.textDim, style = MaterialTheme.typography.labelSmall)
            blockingOperation?.let { Text("BLOCKED: $it", color = LiveColors.playhead, style = MaterialTheme.typography.labelSmall) }
            errorMessage?.let {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Text(it, color = Color(0xFFFF8A80), modifier = Modifier.weight(1f), maxLines = 1, overflow = TextOverflow.Ellipsis)
                    TextButton(onClick = onClearError) { Text("CLEAR") }
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
            modifier = Modifier.fillMaxSize().padding(LiveDimensions.gap).horizontalScroll(scrollState),
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
            IconButton(onClick = onAdd, modifier = Modifier.size(LiveDimensions.action)) {
                Icon(Icons.Default.Add, contentDescription = "Add track")
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
    Column(
        modifier = Modifier.width(LiveDimensions.mixerChannel).fillMaxHeight()
            .clip(RoundedCornerShape(LiveDimensions.radius))
            .background(if (selected) LiveColors.selectedTrack else Color.Transparent)
            .semantics { this.selected = selected }
            .clickable(role = Role.Button, onClick = onSelect)
            .padding(vertical = LiveDimensions.smallGap),
        horizontalAlignment = Alignment.CenterHorizontally,
    ) {
        Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(LiveDimensions.smallGap)) {
            Box(
                modifier = Modifier.width(LiveDimensions.smallGap).height(16.dp)
                    .background(if (track.playing) LiveColors.live else LiveColors.divider),
            )
            Text(
                text = "T${index + 1}",
                color = if (selected) LiveColors.live else LiveColors.text,
                fontWeight = if (selected) FontWeight.Bold else FontWeight.Medium,
            )
        }
        Box(modifier = Modifier.weight(1f), contentAlignment = Alignment.Center) {
            Slider(
                value = track.volume,
                onValueChange = onVolume,
                modifier = Modifier.width(112.dp).rotate(-90f),
            )
        }
    }
}

@Composable
private fun MasterChannel() {
    Column(
        modifier = Modifier.width(LiveDimensions.mixerChannel).fillMaxHeight().padding(vertical = LiveDimensions.smallGap),
        horizontalAlignment = Alignment.CenterHorizontally,
    ) {
        Text("MASTER", color = LiveColors.playhead, style = MaterialTheme.typography.labelSmall)
        Spacer(modifier = Modifier.weight(1f))
        Box(modifier = Modifier.width(LiveDimensions.smallGap).height(56.dp).background(LiveColors.divider))
        Text("OUT", color = LiveColors.textMuted, style = MaterialTheme.typography.labelSmall)
    }
}

@Composable
private fun LiveNavigation(
    mode: String,
    onDevices: () -> Unit,
    onSession: () -> Unit,
    modifier: Modifier = Modifier,
) {
    NavigationBar(
        containerColor = LiveColors.panel,
        modifier = modifier.height(LiveDimensions.nav),
    ) {
        NavigationBarItem(
            selected = mode == "Session",
            onClick = onSession,
            icon = { Icon(Icons.Default.PlayArrow, contentDescription = "Session view") },
            label = { Text("Session") },
        )
        NavigationBarItem(
            selected = mode == "Devices",
            onClick = onDevices,
            icon = { Icon(Icons.Default.Stop, contentDescription = "Devices view") },
            label = { Text("Devices") },
        )
    }
}


@Composable
private fun DevicesSurface(devices: List<com.vibes.dsp.ui.rack.RackPlugin>, pathId: Long, viewModel: RackViewModel, onBrowser: (Long) -> Unit, onSettings: () -> Unit) {
    Column(Modifier.fillMaxSize().padding(LiveDimensions.gap)) {
        Row(verticalAlignment = Alignment.CenterVertically) { Text("DEVICES", color = LiveColors.text, modifier = Modifier.weight(1f)); TextButton(onClick = onSettings) { Text("USB AUDIO") } }
        androidx.compose.foundation.lazy.LazyColumn(Modifier.weight(1f)) { items(devices.size) { i -> val p = devices[i]; PluginCard(plugin=p, pluginIndex=i, pathId=pathId, viewModel=viewModel, onRemove={viewModel.removePlugin(pathId,i)}, onReplace={onBrowser(pathId)}) } }
        Button(onClick = { onBrowser(pathId) }, modifier = Modifier.fillMaxWidth()) { Text("ADD DEVICE") }
    }
}
