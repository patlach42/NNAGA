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
import kotlinx.coroutines.withContext
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import java.io.File

data class RackPlugin(val index: Int, val name: String, val pluginId: String, val instanceId: Long)

class RackViewModel(application: Application) : AndroidViewModel(application) {
    private val native = NativeEngine.getInstance()
    private val _isEngineRunning = MutableStateFlow(false); val isEngineRunning = _isEngineRunning.asStateFlow()
    private val _latencyMs = MutableStateFlow(0.0); val latencyMs = _latencyMs.asStateFlow()
    private val _inputLevel = MutableStateFlow(0f); val inputLevel = _inputLevel.asStateFlow()
    private val _outputLevel = MutableStateFlow(0f); val outputLevel = _outputLevel.asStateFlow()
    private val _cpuLoad = MutableStateFlow(0f); val cpuLoad = _cpuLoad.asStateFlow()
    private val _xRunCount = MutableStateFlow(0); val xRunCount = _xRunCount.asStateFlow()
    private val _inputClipping = MutableStateFlow(false); val inputClipping = _inputClipping.asStateFlow()
    private val _outputClipping = MutableStateFlow(false); val outputClipping = _outputClipping.asStateFlow()
    private val _tracks = MutableStateFlow<List<RackTrackInfo>>(emptyList()); val tracks: StateFlow<List<RackTrackInfo>> = _tracks.asStateFlow()
    private val _selectedPathId = MutableStateFlow<RackPathId>(1L); val selectedPathId = _selectedPathId.asStateFlow()
    private val _directUsbStats = MutableStateFlow(DirectUsbStats())
    val directUsbStats: StateFlow<DirectUsbStats> = _directUsbStats.asStateFlow()
    private val _directUsbState = MutableStateFlow(DirectUsbSessionState.Stopped)
    val directUsbState: StateFlow<DirectUsbSessionState> = _directUsbState.asStateFlow()
    private val _selectedPathPlugins = MutableStateFlow<List<RackPlugin>>(emptyList()); val selectedPathPlugins = _selectedPathPlugins.asStateFlow()
    private val _wavTransport = MutableStateFlow(WavTransportInfo(false, false, 0.0, 0.0, 0)); val wavTransport = _wavTransport.asStateFlow()
    private val _errorMessage = MutableStateFlow<String?>(null); val errorMessage = _errorMessage.asStateFlow()
    private val _blockingOperation = MutableStateFlow<String?>(null); val blockingOperation = _blockingOperation.asStateFlow()
    private val _presetList = MutableStateFlow<List<String>>(emptyList()); val presetList = _presetList.asStateFlow()
    private val _recentPresets = MutableStateFlow<List<String>>(emptyList()); val recentPresets = _recentPresets.asStateFlow()
    private val _presetMessage = MutableStateFlow<String?>(null); val presetMessage = _presetMessage.asStateFlow()
    private val _isRecording = MutableStateFlow(false); val isRecording = _isRecording.asStateFlow()
    private val _recordingDurationSec = MutableStateFlow(0.0); val recordingDurationSec = _recordingDurationSec.asStateFlow()
    private val presetManager = PresetManager(native)
    private var recentPresetsManager: RecentPresetsManager? = null
    private var lastUsbSignature: List<Long>? = null
    private var failedUsbSessionId: Long = 0
    private val lifecycleMutex = Mutex()
    private val recordingMutex = Mutex()
    private val rackControlMutex = Mutex()
    private var restartJob: Job? = null
    @Volatile private var nativeReady = false
    @Volatile private var rackVisible = false

    init {
        // Meter values are lock-free native atomics. Sample them off Main at
        // display cadence; the 200 ms control poll made the VU bars visibly step.
        viewModelScope.launch(Dispatchers.Default) {
            while (true) {
                if (nativeReady && rackVisible && _isEngineRunning.value) {
                    runCatching {
                        _inputLevel.value = AudioEngine.getInputLevel()
                        _outputLevel.value = AudioEngine.getOutputLevel()
                        _inputClipping.value = AudioEngine.isInputClipping()
                        _outputClipping.value = AudioEngine.isOutputClipping()
                    }
                } else if (_inputLevel.value != 0f || _outputLevel.value != 0f) {
                    _inputLevel.value = 0f
                    _outputLevel.value = 0f
                }
                delay(16)
            }
        }
        // Control/status JNI may allocate or wait behind control locks. Keep it
        // away from Compose Main and at a lower cadence than the meters.
        viewModelScope.launch(Dispatchers.IO) {
            while (true) {
                if (nativeReady && rackVisible) {
                    runCatching { pollDirectUsbStats() }
                    if (_isEngineRunning.value) {
                        runCatching {
                            _cpuLoad.value = AudioEngine.getCpuLoad()
                            _latencyMs.value = AudioEngine.getLatencyMs()
                            _xRunCount.value = maxOf(
                                AudioEngine.getXRunCount(),
                                _directUsbStats.value.actualXruns.toInt()
                            )
                            if (_isRecording.value) {
                                _recordingDurationSec.value =
                                    RecordingManager.getRecordingDurationSec()
                            }
                        }
                    }
                    refreshWavTransport()
                }
                delay(200)
            }
        }
    }

    private suspend fun pollDirectUsbStats() {
        val stats = native.getDirectUsbStats()
        _directUsbStats.value = stats
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
            stats.steadyTarget,
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
                "DSP(last=${stats.lastDspNs},peak=${stats.peakDspNs})")
        }
    }


    fun onNativeEngineReady() {
        nativeReady = true
        refreshRack(forceNewInstanceIds = true)
    }
    fun setRackVisible(visible: Boolean) {
        rackVisible = visible
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
            refreshWavTransport()
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

    suspend fun loadTrackWav(trackId: RackPathId, path: String, displayName: String): Boolean = withBlockingOperation("Loading WAV") { withContext(Dispatchers.IO) { RackManager.loadTrackWav(trackId, path, displayName) } }.also { if (!it) _errorMessage.value = "Failed to load WAV"; refreshRack() }
    fun loadTrackWavAsync(trackId: RackPathId, path: String, displayName: String) { viewModelScope.launch { loadTrackWav(trackId, path, displayName) } }
    fun unloadTrackWav(trackId: RackPathId) { viewModelScope.launch { val ok = withBlockingOperation("Unloading WAV") { withContext(Dispatchers.IO) { RackManager.unloadTrackWav(trackId) } }; if (!ok) _errorMessage.value = "Failed to unload WAV"; refreshRack() } }
    fun clearTrackWavs() { viewModelScope.launch(Dispatchers.IO) { RackManager.clearTrackWavs(); refreshRackNow() } }
    fun wavTransportPlay() { transportPlaying(true) }
    fun wavTransportPause() { transportPlaying(false) }
    fun wavTransportRestart() { viewModelScope.launch(Dispatchers.IO) { if (!RackManager.restartWavTransport()) _errorMessage.value = if (!_isEngineRunning.value) "Start engine to play WAV" else "No WAV clips loaded"; refreshWavTransport() } }
    fun wavTransportToggleLoop() { viewModelScope.launch(Dispatchers.IO) { RackManager.setWavTransportLooping(!_wavTransport.value.looping); refreshWavTransport() } }
    private fun refreshWavTransport() { if (nativeReady) runCatching { _wavTransport.value = RackManager.getWavTransportInfo() } }
    private fun transportPlaying(value: Boolean) { viewModelScope.launch(Dispatchers.IO) { if (!RackManager.setWavTransportPlaying(value) && value) _errorMessage.value = if (!_isEngineRunning.value) "Start engine to play WAV" else "No WAV clips loaded"; refreshWavTransport() } }

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
        viewModelScope.launch {
            rackControlMutex.withLock {
                withContext(Dispatchers.IO) {
                    runCatching {
                        RackManager.setParameter(pathId, pluginIndex, portIndex, value)
                    }.onFailure {
                        _errorMessage.value = "Failed to set parameter: ${it.message}"
                    }
                }
            }
        }
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
                stopRecordingNow()
                withContext(Dispatchers.IO) { DirectUsbAudioManager.disable(getApplication()) }
            }
        }
    }
    fun restartEngine() {
        restartJob?.cancel()
        _directUsbState.value = DirectUsbSessionState.Starting
        restartJob = viewModelScope.launch {
            lifecycleMutex.withLock {
                stopRecordingNow()
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
    fun resetClipping() { AudioEngine.resetClipping(); _inputClipping.value = false; _outputClipping.value = false }
    fun toggleRecording(context: Context) {
        if (_isRecording.value) {
            stopRecording()
        } else if (!_isEngineRunning.value) {
            _errorMessage.value = "Start the engine first to record"
        } else {
            viewModelScope.launch {
                recordingMutex.withLock {
                    if (_isRecording.value || !_isEngineRunning.value) return@withLock
                    val started = withContext(Dispatchers.IO) {
                        RecordingManager.startRecording(context)
                    }
                    _isRecording.value = started
                    if (!started) _errorMessage.value = "Failed to start recording"
                }
            }
        }
    }
    fun stopRecording() {
        viewModelScope.launch { stopRecordingNow() }
    }
    private suspend fun stopRecordingNow() {
        recordingMutex.withLock {
            if (!_isRecording.value) return
            _isRecording.value = false
            withContext(Dispatchers.IO) { RecordingManager.stopRecording() }
            _recordingDurationSec.value = 0.0
        }
    }

    private suspend fun <T> withBlockingOperation(label: String, block: suspend () -> T): T { _blockingOperation.value = label; return try { block() } finally { _blockingOperation.value = null } }
    private var recentManager: RecentPresetsManager? get() = recentPresetsManager; set(v) { recentPresetsManager = v }
    fun refreshPresets(ctx: Context) {
        viewModelScope.launch(Dispatchers.IO) { refreshPresetsNow(ctx) }
    }
    private fun refreshPresetsNow(ctx: Context) {
        _presetList.value = presetManager.listPresets(ctx)
        _recentPresets.value =
            (recentManager ?: RecentPresetsManager(ctx).also { recentManager = it }).getRecents()
    }
    fun savePreset(ctx: Context, name: String) { viewModelScope.launch { val ok = withBlockingOperation("Saving preset") { withContext(Dispatchers.IO) { presetManager.savePreset(ctx, name) } }; if (ok) { withContext(Dispatchers.IO) { (recentManager ?: RecentPresetsManager(ctx).also { recentManager = it }).addRecent(name); refreshPresetsNow(ctx) }; _presetMessage.value = "Preset '$name' saved" } else _presetMessage.value = "Failed to save preset" } }
    fun loadPreset(ctx: Context, name: String) { viewModelScope.launch { val ok = withBlockingOperation("Loading preset") { withContext(Dispatchers.IO) { native.setRackBypass(true); try { presetManager.loadPreset(ctx, name) } finally { native.setRackBypass(false) } } }; if (ok) { refreshRackNow(true); _presetMessage.value = "Preset '$name' loaded" } else _presetMessage.value = "Failed to load preset" } }
    fun loadRecordingPreset(json: String) { viewModelScope.launch { val ok = withBlockingOperation("Loading preset") { withContext(Dispatchers.IO) { native.setRackBypass(true); try { presetManager.loadPresetFromJson(json) } finally { native.setRackBypass(false) } } }; if (ok) refreshRackNow(true); _presetMessage.value = if (ok) "Recording preset loaded" else "Failed to load recording preset" } }
    fun deletePreset(ctx: Context, name: String) { viewModelScope.launch(Dispatchers.IO) { presetManager.deletePreset(ctx, name); recentManager?.removeRecent(name); refreshPresetsNow(ctx); _presetMessage.value = "Preset '$name' deleted" } }
    suspend fun getPresetJson(ctx: Context, name: String): String? = withContext(Dispatchers.IO) { presetManager.getPresetJson(ctx, name) }
    fun clearRecentPresets() { viewModelScope.launch(Dispatchers.IO) { recentManager?.clearRecents(); _recentPresets.value = emptyList() } }
    fun clearError() { _errorMessage.value = null }
    fun clearPresetMessage() { _presetMessage.value = null }
}
