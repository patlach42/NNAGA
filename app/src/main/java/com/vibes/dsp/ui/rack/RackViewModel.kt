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
import java.util.concurrent.atomic.AtomicLong

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
    private var restartJob: Job? = null

    init {
        viewModelScope.launch {
            while (true) {
                if (nativeReady) {
                    runCatching { pollDirectUsbStats() }
                    if (_isEngineRunning.value) {
                        runCatching {
                            _inputLevel.value = AudioEngine.getInputLevel()
                            _outputLevel.value = AudioEngine.getOutputLevel()
                            _inputClipping.value = AudioEngine.isInputClipping()
                            _outputClipping.value = AudioEngine.isOutputClipping()
                            _cpuLoad.value = AudioEngine.getCpuLoad()
                            _latencyMs.value = AudioEngine.getLatencyMs()
                            _xRunCount.value = maxOf(AudioEngine.getXRunCount(), _directUsbStats.value.actualXruns.toInt())
                            if (_isRecording.value) _recordingDurationSec.value = RecordingManager.getRecordingDurationSec()
                        }
                    }
                    refreshWavTransport()
                }
                delay(200)
            }
        }
    }

    private suspend fun pollDirectUsbStats() {
        val stats = withContext(Dispatchers.IO) { native.getDirectUsbStats() }
        _directUsbStats.value = stats
        _directUsbState.value = stats.state
        _isEngineRunning.value = stats.state == DirectUsbSessionState.Running
        if (stats.state == DirectUsbSessionState.Failed && stats.sessionId != 0L &&
            stats.sessionId != failedUsbSessionId) {
            failedUsbSessionId = stats.sessionId
            _errorMessage.value = "USB failed: ${stats.failure}"
        }
        val signature = listOf(
            stats.playbackXruns, stats.captureOverruns, stats.captureUnderruns,
            stats.captureTransferErrors, stats.playbackTransferErrors,
            stats.captureWaitPressure, stats.writeWaitPressure,
            stats.effectiveQuantum, stats.periodMultiplier, stats.startupPrime,
            stats.steadyTarget, stats.captureRingFrames, stats.playbackRingFrames,
            stats.knownHostLatencyFrames, stats.queuedOut,
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


    private var nativeReady = false
    fun onNativeEngineReady() {
        nativeReady = true
        refreshRack(forceNewInstanceIds = true)
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
    fun setTrackVolume(trackId: RackPathId, volume: Float) { if (!RackManager.setTrackVolume(trackId, volume)) _errorMessage.value = "Failed to set track volume"; refreshRack() }
    fun setTrackInputArmed(trackId: RackPathId, armed: Boolean) { if (!RackManager.setTrackInputArmed(trackId, armed)) _errorMessage.value = "Failed to arm track"; refreshRack() }

    suspend fun loadTrackWav(trackId: RackPathId, path: String, displayName: String): Boolean = withBlockingOperation("Loading WAV") { withContext(Dispatchers.IO) { RackManager.loadTrackWav(trackId, path, displayName) } }.also { if (!it) _errorMessage.value = "Failed to load WAV"; refreshRack() }
    fun loadTrackWavAsync(trackId: RackPathId, path: String, displayName: String) { viewModelScope.launch { loadTrackWav(trackId, path, displayName) } }
    fun unloadTrackWav(trackId: RackPathId) { viewModelScope.launch { val ok = withBlockingOperation("Unloading WAV") { withContext(Dispatchers.IO) { RackManager.unloadTrackWav(trackId) } }; if (!ok) _errorMessage.value = "Failed to unload WAV"; refreshRack() } }
    fun clearTrackWavs() { viewModelScope.launch { RackManager.clearTrackWavs(); refreshRack() } }
    fun wavTransportPlay() { transportPlaying(true) }
    fun wavTransportPause() { transportPlaying(false) }
    fun wavTransportRestart() { viewModelScope.launch { if (!RackManager.restartWavTransport()) _errorMessage.value = if (!_isEngineRunning.value) "Start engine to play WAV" else "No WAV clips loaded"; refreshWavTransport() } }
    fun wavTransportToggleLoop() { viewModelScope.launch { RackManager.setWavTransportLooping(!_wavTransport.value.looping); refreshWavTransport() } }
    fun refreshWavTransport() { if (nativeReady) runCatching { _wavTransport.value = RackManager.getWavTransportInfo() } }
    private fun transportPlaying(value: Boolean) { viewModelScope.launch { if (!RackManager.setWavTransportPlaying(value) && value) _errorMessage.value = if (!_isEngineRunning.value) "Start engine to play WAV" else "No WAV clips loaded"; refreshWavTransport() } }

    fun addPlugin(pathId: RackPathId, pluginId: String, position: Int = -1) { viewModelScope.launch(Dispatchers.IO) { if (RackManager.addPlugin(pathId, pluginId, position) < 0) _errorMessage.value = "Failed to add plugin"; refreshSelectedPath() } }
    fun removePlugin(pathId: RackPathId, position: Int) { viewModelScope.launch(Dispatchers.IO) { if (!RackManager.removePlugin(pathId, position)) _errorMessage.value = "Failed to remove plugin"; refreshSelectedPath() } }
    fun reorderPlugins(pathId: RackPathId, fromPos: Int, toPos: Int) { viewModelScope.launch(Dispatchers.IO) { if (!RackManager.reorder(pathId, fromPos, toPos)) _errorMessage.value = "Failed to reorder plugins"; refreshSelectedPath() } }
    fun setPluginFilePath(pathId: RackPathId, pluginIndex: Int, propertyUri: String, filePath: String) { viewModelScope.launch { runCatching { RackManager.setPluginFilePath(pathId, pluginIndex, propertyUri, filePath) }.onFailure { _errorMessage.value = "Failed to set file path: ${it.message}" } } }
    fun setParameter(pathId: RackPathId, pluginIndex: Int, portIndex: Int, value: Float) { viewModelScope.launch { runCatching { RackManager.setParameter(pathId, pluginIndex, portIndex, value) }.onFailure { _errorMessage.value = "Failed to set parameter: ${it.message}" } } }
    fun getParameter(pathId: RackPathId, pluginIndex: Int, portIndex: Int): Float = runCatching { RackManager.getParameter(pathId, pluginIndex, portIndex) }.getOrDefault(0f)
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
        stopRecording()
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
    fun resetClipping() { AudioEngine.resetClipping(); _inputClipping.value = false; _outputClipping.value = false }
    fun toggleRecording(context: Context) { if (_isRecording.value) stopRecording() else if (!_isEngineRunning.value) _errorMessage.value = "Start the engine first to record" else { _isRecording.value = RecordingManager.startRecording(context); if (!_isRecording.value) _errorMessage.value = "Failed to start recording" } }
    fun stopRecording() { if (_isRecording.value) { RecordingManager.stopRecording(); _isRecording.value = false; _recordingDurationSec.value = 0.0 } }

    private suspend fun <T> withBlockingOperation(label: String, block: suspend () -> T): T { _blockingOperation.value = label; return try { block() } finally { _blockingOperation.value = null } }
    private var recentManager: RecentPresetsManager? get() = recentPresetsManager; set(v) { recentPresetsManager = v }
    fun refreshPresets(ctx: Context) { _presetList.value = presetManager.listPresets(ctx); _recentPresets.value = (recentManager ?: RecentPresetsManager(ctx).also { recentManager = it }).getRecents() }
    fun savePreset(ctx: Context, name: String) { viewModelScope.launch { val ok = withBlockingOperation("Saving preset") { withContext(Dispatchers.IO) { presetManager.savePreset(ctx, name) } }; if (ok) { (recentManager ?: RecentPresetsManager(ctx).also { recentManager = it }).addRecent(name); refreshPresets(ctx); _presetMessage.value = "Preset '$name' saved" } else _presetMessage.value = "Failed to save preset" } }
    fun loadPreset(ctx: Context, name: String) { viewModelScope.launch { val ok = withBlockingOperation("Loading preset") { withContext(Dispatchers.IO) { native.setRackBypass(true); try { presetManager.loadPreset(ctx, name) } finally { native.setRackBypass(false) } } }; if (ok) { refreshRackNow(true); _presetMessage.value = "Preset '$name' loaded" } else _presetMessage.value = "Failed to load preset" } }
    fun loadRecordingPreset(json: String) { viewModelScope.launch { val ok = withBlockingOperation("Loading preset") { withContext(Dispatchers.IO) { native.setRackBypass(true); try { presetManager.loadPresetFromJson(json) } finally { native.setRackBypass(false) } } }; if (ok) refreshRackNow(true); _presetMessage.value = if (ok) "Recording preset loaded" else "Failed to load recording preset" } }
    fun deletePreset(ctx: Context, name: String) { viewModelScope.launch { presetManager.deletePreset(ctx, name); recentManager?.removeRecent(name); refreshPresets(ctx); _presetMessage.value = "Preset '$name' deleted" } }
    fun getPresetJson(ctx: Context, name: String): String? = presetManager.getPresetJson(ctx, name)
    fun clearRecentPresets(ctx: Context) { recentManager?.clearRecents(); _recentPresets.value = emptyList() }
    fun clearError() { _errorMessage.value = null }
    fun clearPresetMessage() { _presetMessage.value = null }
}
