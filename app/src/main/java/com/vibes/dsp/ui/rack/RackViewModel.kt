package com.vibes.dsp.ui.rack

import android.app.Application
import android.content.Context
import android.util.Log
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import com.vibes.dsp.engine.*
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.withContext
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.channels.Channel
import java.util.concurrent.ConcurrentHashMap
import kotlinx.coroutines.sync.withLock
import android.net.Uri
import java.io.File

import com.vibes.dsp.ui.live.ClipLauncherPreferences

internal fun parseClipSourceBpmFromFilename(filename: String): Double? {
    val match = Regex("""(?i)(?<![\d.,])(\d+(?:[.,]\d+)?)\s*bpm\b""").find(filename)
        ?: return null
    val bpm = match.groupValues[1].replace(',', '.').toDoubleOrNull() ?: return null
    return bpm.takeIf { it in 20.0..400.0 }
}

internal fun detectLoopTempoBpm(durationSeconds: Double, referenceBpm: Double): Double? {
    if (!durationSeconds.isFinite() || durationSeconds <= 0.0) return null
    val reference = referenceBpm.takeIf { it.isFinite() && it > 0.0 } ?: 120.0
    return listOf(2, 4, 8, 16)
        .asSequence()
        .map { bars ->
            val bpm = bars * 4.0 * 60.0 / durationSeconds
            bars to bpm
        }
        .filter { (_, bpm) -> bpm.isFinite() && bpm in 50.0..200.0 }
        .minWithOrNull(compareBy<Pair<Int, Double>> { kotlin.math.abs(it.second - reference) }.thenBy { it.first })
        ?.second
}
data class RackPlugin(val index: Int, val name: String, val pluginId: String, val instanceId: Long)

data class MeterState(
    val inputLevel: Float = 0f,
    val outputLevel: Float = 0f,
    val inputClipping: Boolean = false,
    val outputClipping: Boolean = false
)

class RackViewModel(application: Application) : AndroidViewModel(application) {
    private val native = NativeEngine.getInstance()
    private val _isEngineRunning = MutableStateFlow(false); val isEngineRunning = _isEngineRunning.asStateFlow()
    private val _latencyMs = MutableStateFlow(0.0); val latencyMs = _latencyMs.asStateFlow()
    private val _meterState = MutableStateFlow(MeterState()); val meterState = _meterState.asStateFlow()
    private val _cpuLoad = MutableStateFlow(0f); val cpuLoad = _cpuLoad.asStateFlow()
    private data class ParameterKey(val pathId: RackPathId, val pluginIndex: Int, val portIndex: Int)
    private val parameterChannels = ConcurrentHashMap<ParameterKey, Channel<Float>>()
    private val parameterJobs = ConcurrentHashMap<ParameterKey, Job>()
    private val _xRunCount = MutableStateFlow(0); val xRunCount = _xRunCount.asStateFlow()
    private val _tracks = MutableStateFlow<List<RackTrackInfo>>(emptyList()); val tracks: StateFlow<List<RackTrackInfo>> = _tracks.asStateFlow()
    private val _selectedPathId = MutableStateFlow<RackPathId>(1L); val selectedPathId = _selectedPathId.asStateFlow()
    private val _directUsbStats = MutableStateFlow(DirectUsbStats())
    val directUsbStats: StateFlow<DirectUsbStats> = _directUsbStats.asStateFlow()
    private val _directUsbState = MutableStateFlow(DirectUsbSessionState.Stopped)
    val directUsbState: StateFlow<DirectUsbSessionState> = _directUsbState.asStateFlow()
    private val _selectedPathPlugins = MutableStateFlow<List<RackPlugin>>(emptyList()); val selectedPathPlugins = _selectedPathPlugins.asStateFlow()
    private val _transport = MutableStateFlow(TransportInfo(false, 0.0, 120.0, 0L, 0L)); val transport = _transport.asStateFlow()
    private val _waveformPeaks = MutableStateFlow<Map<RackPathId, List<Float>>>(emptyMap())
    val waveformPeaks: StateFlow<Map<RackPathId, List<Float>>> = _waveformPeaks.asStateFlow()
    private val _clipSlots = MutableStateFlow<Map<RackPathId, List<ClipSlotInfo>>>(emptyMap())
    val clipSlots: StateFlow<Map<RackPathId, List<ClipSlotInfo>>> = _clipSlots.asStateFlow()
    private val _midiNotes = MutableStateFlow<Map<Pair<RackPathId, Int>, List<MidiNoteInfo>>>(emptyMap())
    val midiNotes: StateFlow<Map<Pair<RackPathId, Int>, List<MidiNoteInfo>>> = _midiNotes.asStateFlow()
    private val _errorMessage = MutableStateFlow<String?>(null); val errorMessage = _errorMessage.asStateFlow()
    private val _blockingOperation = MutableStateFlow<String?>(null); val blockingOperation = _blockingOperation.asStateFlow()
    private var lastUsbSignature: List<Long>? = null
    private var failedUsbSessionId: Long = 0
    private val lifecycleMutex = Mutex()
    private val rackControlMutex = Mutex()
    private var restartJob: Job? = null
    private var autoStartAttempted = false
    @Volatile private var nativeReady = false
    @Volatile private var rackVisible = false
    @Volatile private var usbDiagnosticsVisible = false

    init {
        // Native meters are sampled once and published as one conflated immutable state.
        viewModelScope.launch(Dispatchers.Default) {
            while (true) {
                if (nativeReady && rackVisible && _isEngineRunning.value) {
                    runCatching {
                        _meterState.value = MeterState(
                            AudioEngine.getInputLevel(),
                            AudioEngine.getOutputLevel(),
                            AudioEngine.isInputClipping(),
                            AudioEngine.isOutputClipping()
                        )
                    }
                } else if (_meterState.value != MeterState()) {
                    _meterState.value = MeterState()
                }
                delay(17)
            }
        }
        // Control/status JNI may allocate or wait behind control locks. Keep it
        // away from Compose Main and at a lower cadence than the meters.
        viewModelScope.launch(Dispatchers.IO) {
            while (true) {
                if (nativeReady && (rackVisible || usbDiagnosticsVisible)) {
                    runCatching { pollDirectUsbStats() }
                    if (_isEngineRunning.value && rackVisible) {
                        runCatching {
                            _cpuLoad.value = AudioEngine.getCpuLoad()
                            _latencyMs.value = AudioEngine.getLatencyMs()
                            _xRunCount.value = maxOf(
                                AudioEngine.getXRunCount(),
                                _directUsbStats.value.actualXruns.toInt()
                            )
                        }
                    }
                    if (rackVisible) {
                        refreshTransport()
                        refreshTrackTransport()
                        _tracks.value.forEach { track ->
                            refreshTrackClipSlotsNow(track.id)
                        }
                    }
                }
                delay(200)
            }
        }
    }

    private suspend fun pollDirectUsbStats() {
        val stats = native.getDirectUsbStats()
        _directUsbStats.value = stats
        if (AudioSettingsManager.getAudioBackend(getApplication()) == AudioBackend.AndroidOboe) {
            val error = native.nativeIsEngineError()
            val running = native.nativeIsEngineRunning()
            _directUsbState.value = when {
                error -> DirectUsbSessionState.Failed
                running -> DirectUsbSessionState.Running
                else -> DirectUsbSessionState.Stopped
            }
            _isEngineRunning.value = running
            if (error) _errorMessage.value = "Android audio route failed"
            return
        }
        _directUsbState.value = stats.state
        _isEngineRunning.value = stats.state == DirectUsbSessionState.Running
        if (stats.state == DirectUsbSessionState.Failed && stats.sessionId != 0L &&
            stats.sessionId != failedUsbSessionId) {
            failedUsbSessionId = stats.sessionId
            _errorMessage.value = "USB failed: ${stats.failure}"
        }
        // Occupancy, queued frames and host-latency estimates fluctuate every
        // poll. Logging them as signature fields caused continuous logcat I/O.
        val signature = listOf(
            stats.playbackXruns, stats.captureOverruns, stats.captureUnderruns,
            stats.captureTransferErrors, stats.playbackTransferErrors,
            stats.captureWaitPressure, stats.writeWaitPressure,
            stats.effectiveQuantum, stats.periodMultiplier, stats.startupPrime,
            stats.steadyTarget, stats.deadlineMisses,
        )
        if (signature != lastUsbSignature) {
            lastUsbSignature = signature
            Log.i("DirectUsbDiag", "playbackXruns=${stats.playbackXruns} " +
                "captureOver=${stats.captureOverruns} captureUnder=${stats.captureUnderruns} " +
                "transferErrors(capture=${stats.captureTransferErrors},playback=${stats.playbackTransferErrors}) " +
                "waitPressure(capture=${stats.captureWaitPressure},write=${stats.writeWaitPressure}) " +
                "ring(capture=${stats.captureRingFrames},playback=${stats.playbackRingFrames}) " +
                "queued/prime/target=${stats.queuedOut}/${stats.startupPrime}/${stats.steadyTarget} " +
                "Q=${stats.effectiveQuantum} multiplier=${stats.periodMultiplier} " +
                "estimatedHostQueue=${stats.knownHostLatencyFrames}f " +
                "DSP(last=${stats.lastDspNs},peak=${stats.peakDspNs}) " +
                "cycle(last=${stats.lastCycleNs},peak=${stats.peakCycleNs},budget=${stats.deadlineBudgetNs},misses=${stats.deadlineMisses})")
        }
    }


    fun onNativeEngineReady() {
        nativeReady = true
        refreshRack(forceNewInstanceIds = true)
        if (autoStartAttempted) return
        autoStartAttempted = true
        val context = getApplication<Application>()
        if (!AudioSettingsManager.getEngineRunAtStart(context) ||
            AudioSettingsManager.getDirectUsbDeviceName(context).isBlank()) return
        viewModelScope.launch(Dispatchers.IO) {
            val configuredVendorId = AudioSettingsManager.getDirectUsbVendorId(context)
            val configuredProductId = AudioSettingsManager.getDirectUsbProductId(context)
            if (DirectUsbAudioManager.getAudioDevices(context).any {
                    it.vendorId == configuredVendorId && it.productId == configuredProductId
                }) {
                startEngine()
            }
        }
    }
    fun setRackVisible(visible: Boolean) {
        rackVisible = visible
    }
    fun setUsbDiagnosticsVisible(visible: Boolean) {
        usbDiagnosticsVisible = visible
    }
    fun selectPath(pathId: RackPathId) { if (pathId == MASTER_PATH_ID || _tracks.value.any { it.id == pathId }) { _selectedPathId.value = pathId; refreshSelectedPath() } }
    fun refreshRack(forceNewInstanceIds: Boolean = false) {
        if (!nativeReady) return
        viewModelScope.launch(Dispatchers.IO) { refreshRackNow(forceNewInstanceIds) }
    }
    private suspend fun refreshRackNow(force: Boolean = false) {
        if (!nativeReady) return
        runCatching {
            val all = RackManager.getTracks().toList(); _tracks.value = all
            if (_selectedPathId.value != MASTER_PATH_ID && all.none { it.id == _selectedPathId.value }) _selectedPathId.value = all.firstOrNull()?.id ?: MASTER_PATH_ID
            refreshSelectedPath(force)
            refreshTransport()
            if (_directUsbState.value != DirectUsbSessionState.Failed) _errorMessage.value = null
        }.onFailure {
            android.util.Log.e("RackViewModel", "Failed to refresh rack", it)
            _errorMessage.value = "Failed to refresh rack: ${it.message}"
        }
    }
    fun refreshSelectedPath(forceNewInstanceIds: Boolean = false) {
        if (!nativeReady) return
        val path = _selectedPathId.value
        viewModelScope.launch(Dispatchers.IO) {
            runCatching {
                val entries = RackManager.getRackPlugins(path).toList()
                val old = _selectedPathPlugins.value
                val ids = if (forceNewInstanceIds) emptyMap() else old.associateBy { it.instanceId }
                _selectedPathPlugins.value = entries.map { e ->
                    val previous = ids[e.instanceId]
                    RackPlugin(e.index, e.info.name.ifEmpty { e.info.id }, e.info.fullId, previous?.instanceId ?: e.instanceId)
                }
            }.onFailure { _errorMessage.value = "Failed to get rack plugins: ${it.message}" }
        }
    }
    fun addTrack() { viewModelScope.launch(Dispatchers.IO) { val id = RackManager.addTrack(); if (id != 0L) { refreshRackNow(); selectPath(id) } else _errorMessage.value = "Failed to add track" } }
    fun removeTrack(trackId: RackPathId) { viewModelScope.launch(Dispatchers.IO) { if (RackManager.removeTrack(trackId)) refreshRackNow() else _errorMessage.value = "Failed to remove track" } }
    fun setTrackVolume(trackId: RackPathId, volume: Float) {
        val clamped = volume.coerceIn(0f, 1f)
        _tracks.value = _tracks.value.map { track ->
            if (track.id == trackId) track.copy(volume = clamped) else track
        }
        viewModelScope.launch {
            rackControlMutex.withLock {
                val ok = withContext(Dispatchers.IO) {
                    RackManager.setTrackVolume(trackId, clamped)
                }
                if (!ok) {
                    _errorMessage.value = "Failed to set track volume"
                    refreshRackNow()
                }
            }
        }
    }
    fun setTrackInputArmed(trackId: RackPathId, armed: Boolean) {
        _tracks.value = _tracks.value.map { track ->
            if (track.id == trackId) track.copy(inputArmed = armed) else track
        }
        viewModelScope.launch {
            rackControlMutex.withLock {
                val ok = withContext(Dispatchers.IO) {
                    RackManager.setTrackInputArmed(trackId, armed)
                }
                if (!ok) {
                    _errorMessage.value = "Failed to arm track"
                    refreshRackNow()
                }
            }
        }
    }
    fun setTrackInputArmLocked(trackId: RackPathId, locked: Boolean) {
        _tracks.value = _tracks.value.map { track ->
            if (track.id == trackId) track.copy(inputArmLocked = locked) else track
        }
        viewModelScope.launch {
            rackControlMutex.withLock {
                val ok = withContext(Dispatchers.IO) {
                    RackManager.setTrackInputArmLocked(trackId, locked)
                }
                if (!ok) {
                    _errorMessage.value = "Failed to ${if (locked) "lock" else "unlock"} track arm"
                    refreshRackNow()
                }
            }
        }
    }
    fun armTrackExclusively(trackId: RackPathId) {
        _tracks.value = _tracks.value.map { track ->
            track.copy(inputArmed = track.id == trackId)
        }
        viewModelScope.launch {
            rackControlMutex.withLock {
                val ok = withContext(Dispatchers.IO) {
                    RackManager.armTrackExclusively(trackId)
                }
                if (!ok) {
                    _errorMessage.value = "Failed to arm selected track exclusively; please try again"
                    refreshRackNow()
                }
            }
        }
    }
    fun setTrackInputHardwarePair(trackId: RackPathId, firstChannel: Int) {
        _tracks.value = _tracks.value.map { t ->
            if (t.id == trackId) t.copy(inputSourceKind = 0, inputSourceFirstChannel = firstChannel) else t
        }
        viewModelScope.launch(Dispatchers.IO) {
            rackControlMutex.withLock {
                if (!RackManager.setTrackInputHardwarePair(trackId, firstChannel)) {
                    _errorMessage.value = "Failed to set hardware input pair"
                }
                refreshRackNow()
            }
        }
    }
    fun setTrackInputHardwareMono(trackId: RackPathId, channel: Int) {
        _tracks.value = _tracks.value.map { t ->
            if (t.id == trackId) t.copy(inputSourceKind = 2, inputSourceFirstChannel = channel) else t
        }
        viewModelScope.launch(Dispatchers.IO) {
            rackControlMutex.withLock {
                if (!RackManager.setTrackInputHardwareMono(trackId, channel)) {
                    _errorMessage.value = "Failed to set mono hardware input"
                }
                refreshRackNow()
            }
        }
    }
    fun setTrackInputTrack(trackId: RackPathId, sourceTrackId: RackPathId, tap: Int) {
        _tracks.value = _tracks.value.map { t ->
            if (t.id == trackId) t.copy(inputSourceKind = 1, inputSourceTrackId = sourceTrackId, inputTap = tap) else t
        }
        viewModelScope.launch(Dispatchers.IO) {
            rackControlMutex.withLock {
                if (!RackManager.setTrackInputTrack(trackId, sourceTrackId, tap)) {
                    _errorMessage.value = "Invalid track input route (cycle or missing track)"
                }
                refreshRackNow()
            }
        }
    }


    suspend fun loadTrackWav(trackId: RackPathId, path: String, displayName: String): Boolean = withBlockingOperation("Loading WAV") { withContext(Dispatchers.IO) { RackManager.loadTrackWav(trackId, path, displayName) } }.also { if (!it) _errorMessage.value = "Failed to load WAV"; refreshRack() }
    fun loadTrackWavAsync(trackId: RackPathId, path: String, displayName: String) { viewModelScope.launch { loadTrackWav(trackId, path, displayName) } }
    fun unloadTrackWav(trackId: RackPathId) { viewModelScope.launch { val ok = withBlockingOperation("Unloading WAV") { withContext(Dispatchers.IO) { RackManager.unloadTrackWav(trackId) } }; if (!ok) _errorMessage.value = "Failed to unload WAV"; refreshRack() } }
    fun loadTrackWaveform(trackId: RackPathId) {
        viewModelScope.launch(Dispatchers.IO) {
            val peaks = runCatching {
                RackManager.getTrackWaveformPeaks(trackId).toList()
            }.getOrDefault(emptyList())
            _waveformPeaks.update { current ->
                current.toMutableMap().apply {
                    if (peaks.isEmpty()) remove(trackId) else put(trackId, peaks)
                }
            }
        }
    }
    fun refreshTrackClipSlots(trackId: RackPathId) {
        viewModelScope.launch(Dispatchers.IO) {
            val slots = runCatching { RackManager.getTrackClipSlots(trackId).toList() }
                .getOrDefault(emptyList())
            _clipSlots.update { current -> current + (trackId to slots) }
        }
    }
    private suspend fun refreshTrackClipSlotsNow(trackId: RackPathId) {
        val slots = runCatching { RackManager.getTrackClipSlots(trackId).toList() }
            .getOrDefault(emptyList())
        _clipSlots.update { current -> current + (trackId to slots) }
    }

    fun loadTrackClipMedia(trackId: RackPathId, slot: Int, uri: Uri) {
        viewModelScope.launch {
            val resolver = getApplication<Application>().contentResolver
            val sourceName = runCatching {
                resolver.query(uri, arrayOf(android.provider.OpenableColumns.DISPLAY_NAME), null, null, null)
                    ?.use { cursor ->
                        if (cursor.moveToFirst()) cursor.getString(0) else null
                    }
            }.getOrNull()
                ?: uri.path?.substringBefore('?')?.substringAfterLast('/')
                ?: "Clip"
            val displayName = sourceName.substringBeforeLast('.', sourceName).ifBlank { "Clip" }
            val mimeType = resolver.getType(uri)?.lowercase()
            val midi = uri.lastPathSegment?.substringBefore('?')?.lowercase()?.let {
                it.endsWith(".mid") || it.endsWith(".midi")
            } == true || mimeType in setOf("audio/midi", "audio/x-midi", "application/x-midi")
            val isWav = sourceName.substringAfterLast('.', "").equals("wav", ignoreCase = true) ||
                mimeType in setOf("audio/wav", "audio/x-wav", "audio/wave")
            val detectedBpm = if (!midi && isWav &&
                ClipLauncherPreferences.getAutoDetectBpmFromFilename(getApplication())) {
                parseClipSourceBpmFromFilename(sourceName)
            } else null
            var modeSetFailed = false
            val loaded = withBlockingOperation(if (midi) "Importing MIDI clip" else "Importing audio clip") {
                withContext(Dispatchers.IO) {
                    val extension = if (midi) ".mid" else ".wav"
                    val source = File.createTempFile("clip_import_", extension, getApplication<Application>().cacheDir)
                    try {
                        resolver.openInputStream(uri)?.use { input ->
                            source.outputStream().use(input::copyTo)
                        } ?: return@withContext false
                        if (midi) RackManager.loadTrackClipMidi(trackId, slot, source.absolutePath, displayName)
                        else {
                            val wav = File.createTempFile("clip_import_decoded_", ".wav", getApplication<Application>().cacheDir)
                            try {
                                AudioImportDecoder.copyOrDecode(getApplication(), uri, wav)
                                val durationBpm = if (detectedBpm == null &&
                                    ClipLauncherPreferences.getAutoDetectLoopTempo(getApplication())) {
                                    AudioImportDecoder.readWavDurationSeconds(wav)
                                        ?.let { detectLoopTempoBpm(it, _transport.value.beatsPerMinute) }
                                } else null
                                val selectedBpm = detectedBpm ?: durationBpm
                                val wavLoaded = RackManager.loadTrackClipWav(
                                    trackId,
                                    slot,
                                    wav.absolutePath,
                                    displayName,
                                    selectedBpm ?: _transport.value.beatsPerMinute,
                                )
                                if (wavLoaded && selectedBpm != null) {
                                    modeSetFailed = !RackManager.setClipTempoMode(
                                        trackId,
                                        slot,
                                        ClipTempoMode.Stretch,
                                    )
                                }
                                wavLoaded
                            } finally { wav.delete() }
                        }
                    } finally { source.delete() }
                }
            }
            if (!loaded) _errorMessage.value = "Unable to load clip media"
            else if (modeSetFailed) _errorMessage.value = "Failed to set clip tempo mode"
            refreshTrackClipSlots(trackId)
            if (!midi) loadTrackWaveform(trackId)
        }
    }
    fun unloadTrackClipMedia(trackId: RackPathId, slot: Int, wavLoaded: Boolean, midiLoaded: Boolean) {
        viewModelScope.launch(Dispatchers.IO) {
            rackControlMutex.withLock {
                var ok = true
                if (wavLoaded) ok = RackManager.unloadTrackClipWav(trackId, slot) && ok
                if (midiLoaded) ok = RackManager.unloadTrackClipMidi(trackId, slot) && ok
                if (!ok) _errorMessage.value = "Unable to unload clip media"
                refreshTrackClipSlotsNow(trackId)
                refreshRackNow()
                refreshTransport()
                refreshTrackTransport()
            }
        }
    }
    fun renameTrackClip(trackId: RackPathId, slot: Int, displayName: String) {
        val trimmed = displayName.trim()
        if (trimmed.isBlank()) {
            _errorMessage.value = "Clip name cannot be blank"
            return
        }
        viewModelScope.launch {
            val renamed = withContext(Dispatchers.IO) {
                RackManager.renameTrackClip(trackId, slot, trimmed)
            }
            if (!renamed) _errorMessage.value = "Unable to rename clip"
            refreshTrackClipSlots(trackId)
        }
    }
    fun selectTrackClipSlot(trackId: RackPathId, slot: Int) {
        viewModelScope.launch(Dispatchers.IO) {
            if (!RackManager.selectTrackClipSlot(trackId, slot)) _errorMessage.value = "Unable to select clip"
            refreshTrackClipSlots(trackId)
            refreshTrackTransport()
        }
    }
    fun setTrackDefaultLoopLength(trackId: RackPathId, bars: Double) {
        viewModelScope.launch(Dispatchers.IO) {
            rackControlMutex.withLock {
                val ok = RackManager.setTrackDefaultLoopLength(trackId, bars)
                if (!ok) _errorMessage.value = "Failed to set track loop length"
                refreshTrackClipSlotsNow(trackId)
                refreshRackNow()
                refreshTransport()
                refreshTrackTransport()
            }
        }
    }

    fun setSlotDefaultLoopLength(trackId: RackPathId, slot: Int, bars: Double) {
        viewModelScope.launch(Dispatchers.IO) {
            rackControlMutex.withLock {
                val ok = RackManager.setSlotDefaultLoopLength(trackId, slot, bars)
                if (!ok) _errorMessage.value = "Failed to set slot default loop length"
                refreshTrackClipSlotsNow(trackId)
                refreshRackNow()
                refreshTransport()
                refreshTrackTransport()
            }
        }
    }

    fun setClipTempoMode(trackId: RackPathId, slot: Int, mode: ClipTempoMode) {
        viewModelScope.launch(Dispatchers.IO) {
            rackControlMutex.withLock {
                val ok = RackManager.setClipTempoMode(trackId, slot, mode)
                if (!ok) _errorMessage.value = "Failed to set clip tempo mode"
                refreshTrackClipSlotsNow(trackId)
                refreshRackNow()
                refreshTransport()
                refreshTrackTransport()
            }
        }
    }
    fun setClipSourceBpm(trackId: RackPathId, slot: Int, sourceBpm: Double) {
        viewModelScope.launch(Dispatchers.IO) {
            rackControlMutex.withLock {
                val ok = RackManager.setClipSourceBpm(trackId, slot, sourceBpm)
                if (!ok) _errorMessage.value = "Failed to set clip Base BPM"
                refreshTrackClipSlotsNow(trackId)
                refreshRackNow()
                refreshTransport()
                refreshTrackTransport()
            }
        }
    }

    fun setClipLoopLength(trackId: RackPathId, slot: Int, bars: Double) {
        viewModelScope.launch(Dispatchers.IO) {
            rackControlMutex.withLock {
                val ok = RackManager.setClipLoopLength(trackId, slot, bars)
                if (!ok) _errorMessage.value = "Failed to set clip loop length"
                refreshTrackClipSlotsNow(trackId)
                refreshRackNow()
                refreshTransport()
                refreshTrackTransport()
            }
        }
    }
    fun setClipLoopStartQuarterNotes(trackId: RackPathId, slot: Int, value: Double, result: (Boolean) -> Unit) {
        viewModelScope.launch(Dispatchers.IO) {
            rackControlMutex.withLock {
                val ok = RackManager.setClipLoopStartQuarterNotes(trackId, slot, value)
                if (!ok) _errorMessage.value = "Failed to set clip loop start"
                refreshTrackClipSlotsNow(trackId); refreshRackNow(); refreshTransport(); refreshTrackTransport()
                withContext(Dispatchers.Main) { result(ok) }
            }
        }
    }

    fun setClipLoopLengthQuarterNotes(trackId: RackPathId, slot: Int, value: Double, result: (Boolean) -> Unit) {
        viewModelScope.launch(Dispatchers.IO) {
            rackControlMutex.withLock {
                val ok = RackManager.setClipLoopLengthQuarterNotes(trackId, slot, value)
                if (!ok) _errorMessage.value = "Failed to set clip loop length"
                refreshTrackClipSlotsNow(trackId); refreshRackNow(); refreshTransport(); refreshTrackTransport()
                withContext(Dispatchers.Main) { result(ok) }
            }
        }
    }

    fun setClipLooping(trackId: RackPathId, slot: Int, looping: Boolean) {
        viewModelScope.launch(Dispatchers.IO) {
            rackControlMutex.withLock {
                val ok = RackManager.setClipLooping(trackId, slot, looping)
                if (!ok) _errorMessage.value = "Failed to set clip looping"
                refreshTrackClipSlotsNow(trackId)
                refreshRackNow()
                refreshTransport()
                refreshTrackTransport()
            }
        }
    }

    fun setSlotEnterOnPunch(
        trackId: RackPathId,
        slot: Int,
        armed: Boolean,
        quantization: TrackLaunchQuantization
    ) {
        viewModelScope.launch(Dispatchers.IO) {
            rackControlMutex.withLock {
                val target = _clipSlots.value[trackId]?.firstOrNull { it.slot == slot }
                if (armed && (
                        _tracks.value.firstOrNull { it.id == trackId }?.inputArmed != true ||
                            target?.wavLoaded == true ||
                            target?.midiLoaded == true
                        )) {
                    _errorMessage.value = "Failed to arm enter on punch"
                    return@withLock
                }
                val ok = if (armed) {
                    val configured = RackManager.setSlotEnterOnPunch(trackId, slot, true, quantization)
                    if (!configured) {
                        false
                    } else {
                        val started = RackManager.startTrackClipRecording(trackId, slot, quantization)
                        if (!started) {
                            RackManager.setSlotEnterOnPunch(trackId, slot, false, quantization)
                        }
                        started
                    }
                } else {
                    RackManager.cancelTrackLoopRecording(trackId)
                    RackManager.setSlotEnterOnPunch(trackId, slot, false, quantization)
                }
                if (!ok) _errorMessage.value = if (armed) {
                    "Failed to arm enter on punch"
                } else {
                    "Failed to cancel enter on punch"
                }
                refreshTrackClipSlotsNow(trackId)
                refreshRackNow()
                refreshTransport()
                refreshTrackTransport()
            }
        }
    }

    fun setClipTransportPlaying(
        trackId: RackPathId,
        slot: Int,
        playing: Boolean,
        quantization: TrackLaunchQuantization
    ) {
        viewModelScope.launch(Dispatchers.IO) {
            rackControlMutex.withLock {
                val ok = RackManager.setClipTransportPlaying(trackId, slot, playing, quantization)
                if (!ok) _errorMessage.value = "Failed to set clip transport"
                refreshTrackClipSlotsNow(trackId)
                refreshRackNow()
                refreshTransport()
                refreshTrackTransport()
            }
        }
    }

    fun stopTrackClipTransport(
        trackId: RackPathId,
        quantization: TrackLaunchQuantization
    ) {
        viewModelScope.launch(Dispatchers.IO) {
            rackControlMutex.withLock {
                var ok = true
                _clipSlots.value[trackId].orEmpty().forEach { clip ->
                    if ((clip.playing || clip.launchPending) &&
                        !RackManager.setClipTransportPlaying(trackId, clip.slot, false, quantization)
                    ) {
                        ok = false
                    }
                }
                if (!ok) _errorMessage.value = "Failed to stop track clip transport"
                refreshTrackClipSlotsNow(trackId)
                refreshRackNow()
                refreshTransport()
                refreshTrackTransport()
            }
        }
    }

    fun launchClipTransport(
        trackId: RackPathId,
        slot: Int,
        quantization: TrackLaunchQuantization,
        startGlobal: Boolean
    ) {
        viewModelScope.launch(Dispatchers.IO) {
            rackControlMutex.withLock {
                if (startGlobal) RackManager.setTransportPlaying(true)
                val ok = RackManager.setClipTransportPlaying(trackId, slot, true, quantization)
                if (!ok) _errorMessage.value = "Failed to launch clip transport"
                refreshTrackClipSlotsNow(trackId)
                refreshRackNow()
                refreshTransport()
                refreshTrackTransport()
            }
        }
    }

    fun startTrackClipRecording(
        trackId: RackPathId,
        slot: Int,
        quantization: TrackLaunchQuantization
    ) {
        viewModelScope.launch(Dispatchers.IO) {
            rackControlMutex.withLock {
                val recorded = RackManager.startTrackClipRecording(trackId, slot, quantization)
                if (!recorded) _errorMessage.value = "Failed to start clip recording"
                refreshTrackClipSlotsNow(trackId)
                refreshRackNow()
                refreshTransport()
                refreshTrackTransport()
            }
        }
    }

    fun cancelTrackLoopRecording(trackId: RackPathId) {
        viewModelScope.launch(Dispatchers.IO) {
            rackControlMutex.withLock {
                val reservationExpected = _tracks.value
                    .firstOrNull { it.id == trackId }
                    ?.recordingSlot
                    ?.let { it >= 0 } == true
                val cancelled = RackManager.cancelTrackLoopRecording(trackId)
                refreshTrackClipSlotsNow(trackId)
                refreshRackNow()
                refreshTransport()
                refreshTrackTransport()
                val reservationStillActive = _tracks.value
                    .firstOrNull { it.id == trackId }
                    ?.recordingSlot
                    ?.let { it >= 0 } == true
                if (reservationExpected && !cancelled && reservationStillActive) {
                    _errorMessage.value = "Failed to cancel clip recording"
                }
            }
        }
    }

    fun loadTrackClipMidiNotes(trackId: RackPathId, slot: Int) {
        viewModelScope.launch(Dispatchers.IO) {
            val notes = runCatching { RackManager.getTrackClipMidiNotes(trackId, slot).toList() }
                .getOrDefault(emptyList())
            _midiNotes.update { current -> current + ((trackId to slot) to notes) }
        }
    }
    fun unloadTrackMidi(trackId: RackPathId) {
        viewModelScope.launch {
            val ok = withBlockingOperation("Unloading MIDI") {
                withContext(Dispatchers.IO) { RackManager.unloadTrackMidi(trackId) }
            }
            if (!ok) _errorMessage.value = "Failed to unload MIDI"
            refreshRack()
        }
    }
    suspend fun importTrackAudio(trackId: RackPathId, uri: Uri, displayName: String): Boolean {
        val midi = uri.lastPathSegment?.substringBefore('?')?.lowercase()?.let { it.endsWith(".mid") || it.endsWith(".midi") } == true ||
            getApplication<Application>().contentResolver.getType(uri)?.lowercase() in setOf("audio/midi", "audio/x-midi", "application/x-midi")
        val result = runCatching {
            withBlockingOperation(if (midi) "Importing MIDI" else "Importing audio") {
                withContext(Dispatchers.IO) {
                    val ext = if (midi) ".mid" else ".wav"
                    val cacheFile = File.createTempFile("track_import_", ext, getApplication<Application>().cacheDir)
                    try {
                        getApplication<Application>().contentResolver.openInputStream(uri)?.use { input ->
                            cacheFile.outputStream().use { output -> input.copyTo(output) }
                        } ?: error("Unable to open selected media")
                        if (midi) RackManager.loadTrackMidi(trackId, cacheFile.absolutePath, displayName)
                        else {
                            val wav = File.createTempFile("track_import_decoded_", ".wav", getApplication<Application>().cacheDir)
                            try { AudioImportDecoder.copyOrDecode(getApplication(), uri, wav); RackManager.loadTrackWav(trackId, wav.absolutePath, displayName) }
                            finally { wav.delete() }
                        }
                    } finally { cacheFile.delete() }
                }
            }
        }.getOrElse { error ->
            _errorMessage.value = error.message?.takeIf { it.isNotBlank() } ?: "Unable to import selected media"
            false
        }
        if (!result) _errorMessage.value = _errorMessage.value ?: "Unable to load imported media"
        refreshRack()
        return result
    }
    fun transportPlay() { setTransportPlaying(true) }
    fun transportPause() { setTransportPlaying(false) }
    fun transportRestart() {
        viewModelScope.launch(Dispatchers.IO) {
            RackManager.restartTransport()
            refreshTransport()
        }
    }
    fun transportStop() {
        viewModelScope.launch(Dispatchers.IO) {
            val stopped = RackManager.stopTransport()
            if (!stopped) _errorMessage.value = "Failed to stop transport"
            refreshTransport()
            refreshTrackTransport()
        }
    }
    fun setTransportBpm(bpm: Double) { viewModelScope.launch(Dispatchers.IO) { RackManager.setTransportBpm(bpm); refreshTransport() } }
    private fun refreshTransport() { if (nativeReady) runCatching { _transport.value = RackManager.getTransportInfo() } }
    private fun refreshTrackTransport() {
        if (nativeReady) runCatching { _tracks.value = RackManager.getTracks().toList() }
    }
    private fun setTransportPlaying(value: Boolean) { viewModelScope.launch(Dispatchers.IO) { RackManager.setTransportPlaying(value); refreshTransport(); refreshTrackTransport() } }

    fun addPlugin(pathId: RackPathId, pluginId: String, position: Int = -1) { viewModelScope.launch(Dispatchers.IO) { if (RackManager.addPlugin(pathId, pluginId, position) < 0) _errorMessage.value = "Failed to add plugin"; refreshSelectedPath() } }
    fun removePlugin(pathId: RackPathId, position: Int) { viewModelScope.launch(Dispatchers.IO) { if (!RackManager.removePlugin(pathId, position)) _errorMessage.value = "Failed to remove plugin"; refreshSelectedPath() } }
    fun reorderPlugins(pathId: RackPathId, fromPos: Int, toPos: Int) { viewModelScope.launch(Dispatchers.IO) { if (!RackManager.reorder(pathId, fromPos, toPos)) _errorMessage.value = "Failed to reorder plugins"; refreshSelectedPath() } }
    fun setPluginFilePath(
        pathId: RackPathId,
        pluginIndex: Int,
        propertyUri: String,
        filePath: String
    ) {
        viewModelScope.launch {
            rackControlMutex.withLock {
                withContext(Dispatchers.IO) {
                    runCatching {
                        RackManager.setPluginFilePath(pathId, pluginIndex, propertyUri, filePath)
                    }.onFailure {
                        _errorMessage.value = "Failed to set file path: ${it.message}"
                    }
                }
            }
        }
    }
    fun setParameter(
        pathId: RackPathId,
        pluginIndex: Int,
        portIndex: Int,
        value: Float
    ) {
        val key = ParameterKey(pathId, pluginIndex, portIndex)
        val channel = parameterChannels.computeIfAbsent(key) { Channel(Channel.CONFLATED) }
        parameterJobs.computeIfAbsent(key) {
            viewModelScope.launch {
                for (pending in channel) {
                    rackControlMutex.withLock {
                        withContext(Dispatchers.IO) {
                            runCatching { RackManager.setParameter(pathId, pluginIndex, portIndex, pending) }
                                .onFailure { _errorMessage.value = "Failed to set parameter: ${it.message}" }
                        }
                    }
                }
            }
        }
        channel.trySend(value)
    }
    suspend fun getParameter(pathId: RackPathId, pluginIndex: Int, portIndex: Int): Float = withContext(Dispatchers.IO) { runCatching { RackManager.getParameter(pathId, pluginIndex, portIndex) }.getOrDefault(0f) }
    fun getPreferredUiTypeForPlugin(info: PluginInfo): UiType { val stored = PluginUiPreferenceManager.getStoredUiType(getApplication(), info.fullId); return if (stored != null && info.guiTypes.contains(stored)) stored else info.preferredUiType }
    fun setPreferredUiTypeForPlugin(id: String, type: UiType) = PluginUiPreferenceManager.setStoredUiType(getApplication(), id, type)

    fun startEngine() {
        _directUsbState.value = DirectUsbSessionState.Starting
        viewModelScope.launch {
            lifecycleMutex.withLock {
                withContext(Dispatchers.IO) {
                    val result = DirectUsbAudioManager.startConfigured(getApplication<Application>())
                    if (!result.isSuccess) _errorMessage.value =
                        "USB audio session unavailable: ${result.exceptionOrNull()?.message}"
                }
            }
        }
    }
    fun stopEngine() {
        _directUsbState.value = DirectUsbSessionState.Stopping
        _isEngineRunning.value = false
        viewModelScope.launch {
            lifecycleMutex.withLock {
                withContext(Dispatchers.IO) { DirectUsbAudioManager.disable(getApplication()) }
            }
        }
    }
    fun restartEngine() {
        restartJob?.cancel()
        _directUsbState.value = DirectUsbSessionState.Starting
        restartJob = viewModelScope.launch {
            lifecycleMutex.withLock {
                withContext(Dispatchers.IO) { DirectUsbAudioManager.disable(getApplication()) }
                delay(100)
                withContext(Dispatchers.IO) {
                    val result = DirectUsbAudioManager.startConfigured(getApplication<Application>())
                    if (!result.isSuccess) _errorMessage.value =
                        "USB audio session unavailable: ${result.exceptionOrNull()?.message}"
                }
            }
        }
    }
    fun setAudioBackend(backend: AudioBackend, onResult: (AudioBackend) -> Unit = {}) {
        viewModelScope.launch(Dispatchers.IO) {
            val actual = lifecycleMutex.withLock {
                if (native.nativeIsEngineRunning()) {
                    _errorMessage.value = "Stop the audio engine before changing backend"
                    return@withLock AudioSettingsManager.getAudioBackend(getApplication())
                }
                AudioSettingsManager.setAudioBackend(getApplication(), backend)
                val persisted = AudioSettingsManager.getAudioBackend(getApplication())
                if (persisted == backend) {
                    _directUsbState.value = DirectUsbSessionState.Stopped
                    _errorMessage.value = null
                }
                persisted
            }
            withContext(Dispatchers.Main) { onResult(actual) }
        }
    }
    fun setUsbAudioDriver(driver: UsbAudioDriver) {
        viewModelScope.launch(Dispatchers.IO) {
            lifecycleMutex.withLock {
                val stats = native.getDirectUsbStats()
                val localState = _directUsbState.value
                val blocked = native.nativeIsEngineRunning() ||
                    localState == DirectUsbSessionState.Starting ||
                    localState == DirectUsbSessionState.Running ||
                    localState == DirectUsbSessionState.Stopping ||
                    stats.state == DirectUsbSessionState.Starting ||
                    stats.state == DirectUsbSessionState.Running ||
                    stats.state == DirectUsbSessionState.Stopping
                if (blocked) {
                    _errorMessage.value = "Stop the audio engine before changing USB driver"
                    return@withLock
                }
                DirectUsbAudioManager.switchDriver(getApplication(), driver)
                _directUsbState.value = DirectUsbSessionState.Stopped
                _errorMessage.value = null
            }
        }
    }
    fun resetClipping() {
        AudioEngine.resetClipping()
        _meterState.value = _meterState.value.copy(inputClipping = false, outputClipping = false)
    }

    private suspend fun <T> withBlockingOperation(label: String, block: suspend () -> T): T { _blockingOperation.value = label; return try { block() } finally { _blockingOperation.value = null } }
    fun clearError() { _errorMessage.value = null }
}
