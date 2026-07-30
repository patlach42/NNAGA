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
import android.net.Uri
import java.io.File

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
    private val _xRunCount = MutableStateFlow(0); val xRunCount = _xRunCount.asStateFlow()
    private val _tracks = MutableStateFlow<List<RackTrackInfo>>(emptyList()); val tracks: StateFlow<List<RackTrackInfo>> = _tracks.asStateFlow()
    private val _selectedPathId = MutableStateFlow<RackPathId>(1L); val selectedPathId = _selectedPathId.asStateFlow()
    private val _directUsbStats = MutableStateFlow(DirectUsbStats())
    val directUsbStats: StateFlow<DirectUsbStats> = _directUsbStats.asStateFlow()
    private val _directUsbState = MutableStateFlow(DirectUsbSessionState.Stopped)
    val directUsbState: StateFlow<DirectUsbSessionState> = _directUsbState.asStateFlow()
    private val _selectedPathPlugins = MutableStateFlow<List<RackPlugin>>(emptyList()); val selectedPathPlugins = _selectedPathPlugins.asStateFlow()
    private val _transport = MutableStateFlow(TransportInfo(false, 0.0, 120.0, 0L, 0L)); val transport = _transport.asStateFlow()
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
                        }
                    }
                    refreshTransport()
                    refreshTrackTransport()
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
    fun setTrackInputChannel(trackId: RackPathId, inputChannel: Int) {
        _tracks.value = _tracks.value.map { track ->
            if (track.id == trackId) track.copy(inputChannel = inputChannel) else track
        }
        viewModelScope.launch {
            rackControlMutex.withLock {
                val ok = withContext(Dispatchers.IO) {
                    RackManager.setTrackInputChannel(trackId, inputChannel)
                }
                if (!ok) {
                    _errorMessage.value = "Failed to set track input channel"
                    refreshRackNow()
                }
            }
        }
    }


    suspend fun loadTrackWav(trackId: RackPathId, path: String, displayName: String): Boolean = withBlockingOperation("Loading WAV") { withContext(Dispatchers.IO) { RackManager.loadTrackWav(trackId, path, displayName) } }.also { if (!it) _errorMessage.value = "Failed to load WAV"; refreshRack() }
    fun loadTrackWavAsync(trackId: RackPathId, path: String, displayName: String) { viewModelScope.launch { loadTrackWav(trackId, path, displayName) } }
    fun unloadTrackWav(trackId: RackPathId) { viewModelScope.launch { val ok = withBlockingOperation("Unloading WAV") { withContext(Dispatchers.IO) { RackManager.unloadTrackWav(trackId) } }; if (!ok) _errorMessage.value = "Failed to unload WAV"; refreshRack() } }
    fun clearTrackWavs() { viewModelScope.launch(Dispatchers.IO) { RackManager.clearTrackWavs(); refreshRackNow() } }
    fun transportPlay() { setTransportPlaying(true) }
    fun transportPause() { setTransportPlaying(false) }
    fun transportRestart() { viewModelScope.launch(Dispatchers.IO) { RackManager.restartTransport(); refreshTransport() } }
    fun setTrackTransportPlaying(
        trackId: RackPathId,
        playing: Boolean,
        quantization: TrackLaunchQuantization
    ) {
        viewModelScope.launch(Dispatchers.IO) {
            RackManager.setTrackTransportPlaying(trackId, playing, quantization)
            refreshTrackTransport()
        }
    }
    fun launchTrackTransport(
        trackId: RackPathId,
        quantization: TrackLaunchQuantization,
        startGlobal: Boolean
    ) {
        viewModelScope.launch(Dispatchers.IO) {
            if (startGlobal) RackManager.setTransportPlaying(true)
            RackManager.setTrackTransportPlaying(trackId, true, quantization)
            refreshTransport()
            refreshTrackTransport()
        }
    }
    fun setTrackTransportLooping(trackId: RackPathId, looping: Boolean) {
        viewModelScope.launch(Dispatchers.IO) {
            RackManager.setTrackTransportLooping(trackId, looping)
            refreshTrackTransport()
        }
    }
    fun startTrackLoopRecording(
        trackId: RackPathId,
        bars: Double,
        quantization: TrackLaunchQuantization,
        startGlobal: Boolean
    ) {
        viewModelScope.launch(Dispatchers.IO) {
            if (startGlobal) RackManager.setTransportPlaying(true)
            val ok = RackManager.startTrackLoopRecording(trackId, bars, quantization, false)
            if (!ok) _errorMessage.value = "Failed to start loop recording"
            refreshTransport()
            refreshTrackTransport()
        }
    }
    fun setEnterOnPunch(
        trackId: RackPathId,
        bars: Double,
        quantization: TrackLaunchQuantization,
        armed: Boolean
    ) {
        viewModelScope.launch(Dispatchers.IO) {
            val ok = if (armed) {
                RackManager.startTrackLoopRecording(trackId, bars, quantization, true)
            } else {
                RackManager.cancelTrackLoopRecording(trackId)
            }
            if (!ok) {
                _errorMessage.value = if (armed) {
                    "Failed to arm enter on punch"
                } else {
                    "Failed to cancel enter on punch"
                }
            }
            refreshTransport()
            refreshTrackTransport()
        }
    }
    suspend fun importTrackAudio(trackId: RackPathId, uri: Uri, displayName: String): Boolean {
        val result = runCatching {
            withBlockingOperation("Importing audio") {
                withContext(Dispatchers.IO) {
                    val cacheFile = File.createTempFile("track_import_", ".wav", getApplication<Application>().cacheDir)
                    try {
                        AudioImportDecoder.copyOrDecode(getApplication(), uri, cacheFile)
                        RackManager.loadTrackWav(trackId, cacheFile.absolutePath, displayName)
                    } finally {
                        cacheFile.delete()
                    }
                }
            }
        }.getOrElse { error ->
            _errorMessage.value = error.message?.takeIf { it.isNotBlank() }
                ?: "Unable to import audio; choose a supported audio file"
            false
        }
        if (!result) _errorMessage.value = _errorMessage.value ?: "Unable to load imported audio"
        refreshRack()
        return result
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
    fun resetClipping() {
        AudioEngine.resetClipping()
        _meterState.value = _meterState.value.copy(inputClipping = false, outputClipping = false)
    }

    private suspend fun <T> withBlockingOperation(label: String, block: suspend () -> T): T { _blockingOperation.value = label; return try { block() } finally { _blockingOperation.value = null } }
    fun clearError() { _errorMessage.value = null }
}
