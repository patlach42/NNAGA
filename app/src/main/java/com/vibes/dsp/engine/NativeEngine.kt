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

package com.vibes.dsp.engine

import android.content.Context
import java.io.File

enum class DirectUsbSessionState {
    Stopped, Starting, Running, Failed, Stopping, Unknown;

    companion object {
        fun fromOrdinal(value: Long): DirectUsbSessionState =
            values().getOrNull(value.toInt()) ?: Unknown
    }
}

enum class DirectUsbFailure {
    Ok, NoDevice, NoMatchingAlt, ClaimInterfaceFailed, SetAltFailed,
    SetSampleRateFailed, IsoPumpAllocFailed, IsoPumpSubmitFailed,
    TransportStoppedUnexpectedly, Unknown;

    companion object {
        fun fromCode(value: Long): DirectUsbFailure =
            values().getOrNull(value.toInt()) ?: Unknown
    }
}

data class DirectUsbStats(
    val sequence: Long = 0,
    val captureOverruns: Long = 0,
    val captureUnderruns: Long = 0,
    val captureRingFrames: Long = 0,
    val playbackRingFrames: Long = 0,
    val captureTransferErrors: Long = 0,
    val playbackTransferErrors: Long = 0,
    val captureWaitPressure: Long = 0,
    val writeWaitPressure: Long = 0,
    val playbackXruns: Long = 0,
    val playbackBackpressure: Long = 0,
    val lifecycleFailures: Long = 0,
    val transportFailed: Boolean = false,
    val performanceHintActive: Boolean = false,
    val schemaVersion: Long = 0,
    val sessionId: Long = 0,
    val state: DirectUsbSessionState = DirectUsbSessionState.Unknown,
    val failure: DirectUsbFailure = DirectUsbFailure.Unknown,
    val sampleRateHz: Long = 0,
    val effectiveQuantum: Long = 0,
    val periodMultiplier: Long = 0,
    val startupPrime: Long = 0,
    val steadyTarget: Long = 0,
    val queuedOut: Long = 0,
    val captureTransferFrames: Long = 0,
    val lastDspNs: Long = 0,
    val peakDspNs: Long = 0,
    val knownHostLatencyFrames: Long = 0,
    val actualXruns: Long = 0,
    val lastCycleNs: Long = 0,
    val peakCycleNs: Long = 0,
    val deadlineBudgetNs: Long = 0,
    val deadlineMisses: Long = 0,
) {
    companion object {
        private const val SEQUENCE = 0
        private const val CAPTURE_OVERRUNS = 1
        private const val CAPTURE_UNDERRUNS = 2
        private const val CAPTURE_RING = 8
        private const val PLAYBACK_RING = 7
        private const val CAPTURE_TRANSFER_ERRORS = 5
        private const val PLAYBACK_TRANSFER_ERRORS = 6
        private const val PLAYBACK_XRUNS = 15
        private const val CAPTURE_WAIT_PRESSURE = 16
        private const val WRITE_WAIT_PRESSURE = 17
        private const val SCHEMA = 18
        private const val LIFECYCLE_FAILURES = 9
        private const val TRANSPORT_FAILED = 10
        private const val SESSION = 19
        private const val STATE = 20
        private const val FAILURE = 21
        private const val RATE = 22
        private const val QUANTUM = 23
        private const val MULTIPLIER = 24
        private const val PRIME = 25
        private const val TARGET = 26
        private const val QUEUED = 27
        private const val TRANSFER_FRAMES = 28
        private const val LAST_CYCLE = 33
        private const val PEAK_CYCLE = 34
        private const val DEADLINE_BUDGET = 35
        private const val DEADLINE_MISSES = 36
        private const val LAST_DSP = 29
        private const val PEAK_DSP = 30
        private const val HOST_LATENCY = 31
        private const val ACTUAL_XRUNS = 32
        private const val PLAYBACK_BACKPRESSURE = 37
        private const val PERFORMANCE_HINT_ACTIVE = 38

        fun fromRaw(raw: LongArray): DirectUsbStats {
            fun at(index: Int) = raw.getOrElse(index) { 0L }
            return DirectUsbStats(
                sequence = at(SEQUENCE),
                captureOverruns = at(CAPTURE_OVERRUNS),
                captureUnderruns = at(CAPTURE_UNDERRUNS),
                captureRingFrames = at(CAPTURE_RING),
                playbackRingFrames = at(PLAYBACK_RING),
                captureTransferErrors = at(CAPTURE_TRANSFER_ERRORS),
                playbackTransferErrors = at(PLAYBACK_TRANSFER_ERRORS),
                captureWaitPressure = at(CAPTURE_WAIT_PRESSURE),
                writeWaitPressure = at(WRITE_WAIT_PRESSURE),
                playbackXruns = at(PLAYBACK_XRUNS),
                playbackBackpressure = at(PLAYBACK_BACKPRESSURE),
                lifecycleFailures = at(LIFECYCLE_FAILURES),
                transportFailed = at(TRANSPORT_FAILED) != 0L,
                performanceHintActive = at(PERFORMANCE_HINT_ACTIVE) != 0L,
                schemaVersion = at(SCHEMA),
                sessionId = at(SESSION),
                state = DirectUsbSessionState.fromOrdinal(at(STATE)),
                failure = DirectUsbFailure.fromCode(at(FAILURE)),
                sampleRateHz = at(RATE),
                effectiveQuantum = at(QUANTUM),
                periodMultiplier = at(MULTIPLIER),
                startupPrime = at(PRIME),
                steadyTarget = at(TARGET),
                queuedOut = at(QUEUED),
                captureTransferFrames = at(TRANSFER_FRAMES),
                lastDspNs = at(LAST_DSP),
                peakDspNs = at(PEAK_DSP),
                knownHostLatencyFrames = at(HOST_LATENCY),
                actualXruns = at(ACTUAL_XRUNS),
                lastCycleNs = at(LAST_CYCLE),
                peakCycleNs = at(PEAK_CYCLE),
                deadlineBudgetNs = at(DEADLINE_BUDGET),
                deadlineMisses = at(DEADLINE_MISSES),
            )
        }
    }
}


typealias RackPathId = Long
const val MASTER_PATH_ID: RackPathId = 0L

data class RackTrackInfo(
    val id: RackPathId,
    val volume: Float,
    val inputArmed: Boolean,
    val wavLoaded: Boolean,
    val wavDisplayName: String,
    val wavDurationSec: Double
)

data class TransportInfo(
    val playing: Boolean,
    val looping: Boolean,
    val positionSec: Double,
    val durationSec: Double,
    val loadedTrackCount: Int,
    val beatsPerMinute: Double = 120.0,
    val samplePosition: Long = 0L,
    val transportFrame: Long = 0L
)

data class RackPluginEntry(
    val index: Int,
    val instanceId: Long,
    val info: PluginInfo
)

/**
 * JNI bridge to the native audio engine.
 * Provides a Kotlin interface to the C++ audio processing engine.
 */
class NativeEngine private constructor() {
    
    companion object {
        @Volatile
        private var INSTANCE: NativeEngine? = null

        fun getInstance(): NativeEngine {
            return INSTANCE ?: synchronized(this) {
                INSTANCE ?: NativeEngine().also { INSTANCE = it }
            }
        }
    }

    init {
        // Preload libc++_shared.so so that dlopen() of LV2 plugin .so files
        // (which depend on it) can resolve it from the app's lib/ directory
        try {
            System.loadLibrary("c++_shared")
        } catch (_: UnsatisfiedLinkError) {}

        // Preload lilv shared library when using shared LV2 build (liblilv-0.so.0 in jniLibs).
        // With the default static LV2 build (build_all_lv2.sh), lilv is linked into libguitarrackcraft.so
        // and no .so is packaged — preload will fail; that is expected and not an error.
        try {
            System.loadLibrary("lilv-0")
        } catch (e: UnsatisfiedLinkError) {
            android.util.Log.d("NativeEngine", "Lilv preload skipped (static build or no liblilv-0.so): ${e.message}")
        }
        System.loadLibrary("guitarrackcraft")
    }

    /** Pins the calling UI thread away from CPUs reserved for Direct USB. */
    external fun nativeApplyCurrentThreadUiAffinity()

    /**
     * Set the path where LV2 bundles (e.g. Guitarix) are extracted.
     * Must be called before nativeInit() so plugins are discovered.
     */
    external fun nativeSetLv2Path(path: String)

    /**
     * Set the path to the app's native library directory (for symlink-based plugin loading).
     * Must be called before nativeInit().
     */
    external fun nativeSetNativeLibDir(path: String)

    /**
     * Set the path to the app's files directory. Call before nativeInit() if needed.
     */
    external fun nativeSetFilesDir(path: String)

    /**
     * Set the path to extracted X11 SONAME libs (libX11.so.6 etc.) so plugin UI load can copy them into the bundle dir.
     * Call after extracting assets to e.g. context.filesDir/x11_libs.
     */
    external fun nativeSetX11LibsDir(path: String)

    /**
     * Set the path to extracted PAD plugin .so files (playstore flavor).
     * Must be called before nativeInit() so plugins are discovered.
     */
    external fun nativeSetPluginLibDir(path: String)

    /**
     * Initialize the native engine.
     * Must be called before any other methods.
     * Call nativeSetLv2Path() first with the extracted assets path to enable LV2 plugins.
     */
    external fun nativeInit(): Boolean

    /** Re-runs each PluginFactory's initialize() and rebuilds the registry's
     *  plugin cache. Used by the Manage VST UI (full flavor) so an imported
     *  VST appears in the browser without restarting the audio engine. */
    external fun nativeRefreshPluginRegistry(): Boolean

    external fun nativeGetRackPluginX11Display(pathId: Long, pluginIndex: Int): Int
    external fun nativeGetRackPluginEditorSize(pathId: Long, pluginIndex: Int): Long
    external fun nativeGetRackPluginInstanceId(pathId: Long, pluginIndex: Int): Long
    external fun nativeGetRackPlugins(pathId: Long): Array<RackPluginEntry>


    /** Opens an app-permitted USB device FD for the direct UAC playback prototype. */
    external fun nativeOpenDirectUsbOutput(fileDescriptor: Int): Boolean
    /** Starts direct USB with one selected zero-based mono capture channel. */
    external fun nativeStartDirectUsbSession(
        sampleRate: Int,
        bitsPerSample: Int,
        bytesPerSample: Int,
        channels: Int,
        inputChannel: Int,
        outputPair: Int,
        bufferFrames: Int,
        periodMultiplier: Int,
        watermarkFrames: Int
    ): Boolean


    /** Packed quadruples: sample rate Hz, valid bits, PCM subslot bytes, and channels. */
    external fun nativeGetDirectUsbOutputFormats(): IntArray

    /** Number of selectable mono capture channels on the opened USB interface. */
    external fun nativeGetDirectUsbInputChannelCount(): Int

    /** Stops direct USB playback without closing the Java UsbDeviceConnection. */
    external fun nativeStopDirectUsbOutput()

    external fun nativeIsDirectUsbOutputStreaming(): Boolean

    /**
     * Direct USB transport diagnostics. Indices are documented by the
     * instrumentation stress test and remain stable for field telemetry.
     */
    fun getDirectUsbStats(): DirectUsbStats = DirectUsbStats.fromRaw(nativeGetDirectUsbStats())
    external fun nativeGetDirectUsbStats(): LongArray
    external fun nativeGetDirectUsbErrorDetail(): String
    /** Flushes live PGO profile data; best effort and safe when unsupported. */
    external fun nativeFlushPgoProfile(): Boolean

    /**
     * Stop the audio engine.
     */
    external fun nativeStopEngine()

    /**
     * Check if the engine is currently running.
     */
    external fun nativeIsEngineRunning(): Boolean

    /**
     * Get current sample rate in Hz.
     */
    external fun nativeGetSampleRate(): Float

    /**
     * Get actual buffer frame count used by the audio callback.
     */
    external fun nativeGetBufferFrameCount(): Int


    /**
     * Get current audio latency in milliseconds.
     */
    external fun nativeGetLatencyMs(): Double

    /**
     * Get input peak level (0.0–1.0).
     */
    external fun nativeGetInputLevel(): Float

    /**
     * Get output peak level (0.0–1.0).
     */
    external fun nativeGetOutputLevel(): Float

    /**
     * Get CPU load (0.0–1.0).
     */
    external fun nativeGetCpuLoad(): Float

    /**
     * Get cumulative audio xrun (underrun/overrun) count.
     */
    external fun nativeGetXRunCount(): Int

    /**
     * True if input has clipped.
     */
    external fun nativeIsInputClipping(): Boolean

    /**
     * True if output has clipped.
     */
    external fun nativeIsOutputClipping(): Boolean

    /**
     * Clear clipping indicators.
     */
    external fun nativeResetClipping()

    /**
     * Get list of all available plugins.
     */
    external fun nativeGetAvailablePlugins(): Array<PluginInfo>

    external fun nativeAddPluginToRack(pathId: Long, pluginId: String, position: Int = -1): Int
    external fun nativeRemovePluginFromRack(pathId: Long, position: Int): Boolean
    external fun nativeReorderRack(pathId: Long, fromPos: Int, toPos: Int): Boolean
    external fun nativeSetPluginFilePath(pathId: Long, pluginIndex: Int, propertyUri: String, filePath: String)
    external fun nativeSetParameter(pathId: Long, pluginIndex: Int, portIndex: Int, value: Float)
    external fun nativeGetParameter(pathId: Long, pluginIndex: Int, portIndex: Int): Float
    external fun nativeGetRackSize(pathId: Long): Int
    external fun nativeGetRackPluginInfo(pathId: Long, index: Int): PluginInfo?

    // --- X11 UI management (EGL + ANativeWindow, native X server) ---

    /**
     * Attach an Android Surface to the given X11 display number.
     * Starts the native X server and EGL rendering. Returns root window ID for createPluginUI.
     */
    external fun nativeAttachSurfaceToDisplay(displayNumber: Int, surface: android.view.Surface, width: Int, height: Int): Long

    /** Detach surface and stop X server for the display. */
    /** Signal X11 threads to exit without blocking (call from view destroy callback). */
    /** @return true if detach was deferred (createPluginUI still running); false if connection was closed now */
    external fun nativeSignalDetachSurfaceFromDisplay(displayNumber: Int): Boolean
    external fun nativeStopX11RenderThreadOnly(displayNumber: Int)
    external fun nativeDetachSurfaceFromDisplay(displayNumber: Int)

    /** Release display resources (call after detach when releasing display number). */
    external fun nativeDestroyX11Display(displayNumber: Int)
    /** Detach and destroy display only if it exists (for delayed cleanup from view destroy). */
    external fun nativeDetachAndDestroyX11DisplayIfExists(displayNumber: Int)

    /**
     * Hide the X11 display without destroying it.
     * Stops the render thread, but keeps the X server and surface alive.
     * This allows the display to be reused when switching back to X11 UI mode.
     * Use this when switching to another UI mode (MODGUI/Sliders) to avoid driver mutex crashes.
     */
    external fun nativeHideX11Display(displayNumber: Int)

    /**
     * Resume the X11 display after hiding.
     * Restarts the render thread to resume rendering.
     * Use this when switching back to X11 UI mode.
     */
    external fun nativeResumeX11Display(displayNumber: Int)

    /** Update surface size (e.g. on surfaceChanged). */
    external fun nativeSetSurfaceSize(displayNumber: Int, width: Int, height: Int)

    /** Inject touch: action 0=down, 1=up, 2=move; x, y in view coordinates. */
    external fun nativeInjectTouch(displayNumber: Int, action: Int, x: Int, y: Int)

    /** Hit-test: returns true if (x, y) in surface coords hits an X11 widget (knob, slider, etc.). */
    external fun nativeIsWidgetAtPoint(displayNumber: Int, x: Int, y: Int): Boolean

    /** Request a frame (swap buffers). */
    external fun nativeRequestX11Frame(displayNumber: Int)

    /** Get plugin natural window size [width, height]. Returns [0, 0] if not yet known. */
    external fun nativeGetX11PluginSize(displayNumber: Int): IntArray

    /** Get the UI scale factor for the given display (1.0 = no scaling). */
    external fun nativeGetX11UIScale(displayNumber: Int): Float

    external fun nativeBeginCreatePluginUI(pathId: Long, pluginIndex: Int, pluginInstanceId: Long, uiInstanceId: Long, displayNumber: Int)
    external fun nativeCreatePluginUI(pathId: Long, pluginIndex: Int, pluginInstanceId: Long, uiInstanceId: Long, displayNumber: Int, parentWindowId: Long): Boolean
    external fun nativeDestroyPluginUI(pathId: Long, pluginInstanceId: Long, uiInstanceId: Long)
    external fun nativeIdlePluginUIs(): Boolean
    external fun nativePollFileRequest(): Array<String>?
    external fun nativeDeliverFileToPluginUI(pathId: Long, pluginIndex: Int, propertyUri: String, filePath: String)
    external fun nativePollVstFilePickerRequest(pathId: Long, pluginIndex: Int): Array<String>?
    external fun nativeRespondVstFilePicker(pathId: Long, pluginIndex: Int, sequence: Int, cancelled: Boolean, windowsPath: String)
    external fun nativeSaveRackState(): String?
    external fun nativeRestorePluginState(pathId: Long, pluginIndex: Int, portValues: FloatArray, portIndices: IntArray, propertyKeys: Array<String>, propertyTypes: Array<String>, propertyValues: Array<ByteArray>, propertyFlags: IntArray): Boolean


    // Kotlin wrappers
    fun attachSurfaceToDisplay(displayNumber: Int, surface: android.view.Surface, width: Int, height: Int): Long =
        nativeAttachSurfaceToDisplay(displayNumber, surface, width, height)

    fun signalDetachSurfaceFromDisplay(displayNumber: Int): Boolean = nativeSignalDetachSurfaceFromDisplay(displayNumber)
    fun stopX11RenderThreadOnly(displayNumber: Int) = nativeStopX11RenderThreadOnly(displayNumber)
    fun detachSurfaceFromDisplay(displayNumber: Int) = nativeDetachSurfaceFromDisplay(displayNumber)
    fun destroyX11Display(displayNumber: Int) = nativeDestroyX11Display(displayNumber)
    fun detachAndDestroyX11DisplayIfExists(displayNumber: Int) = nativeDetachAndDestroyX11DisplayIfExists(displayNumber)
    fun hideX11Display(displayNumber: Int) = nativeHideX11Display(displayNumber)
    fun resumeX11Display(displayNumber: Int) = nativeResumeX11Display(displayNumber)
    fun setSurfaceSize(displayNumber: Int, width: Int, height: Int) = nativeSetSurfaceSize(displayNumber, width, height)
    fun injectTouch(displayNumber: Int, action: Int, x: Int, y: Int) = nativeInjectTouch(displayNumber, action, x, y)
    fun isWidgetAtPoint(displayNumber: Int, x: Int, y: Int): Boolean = nativeIsWidgetAtPoint(displayNumber, x, y)
    fun requestX11Frame(displayNumber: Int) = nativeRequestX11Frame(displayNumber)
    fun getX11PluginSize(displayNumber: Int): IntArray = nativeGetX11PluginSize(displayNumber)
    fun getX11UIScale(displayNumber: Int): Float = nativeGetX11UIScale(displayNumber)

    /**
     * Ensure the X11 scratch directory exists and set on native side.
     * X11 SONAME libs (libX11.so.6 etc.) are loaded from nativeLibDir at runtime.
     * This directory is used as a writable scratch dir for plugin UI .so copies.
     */
    fun ensureX11LibsDir(context: Context): File {
        val dir = File(context.filesDir, "x11_libs/arm64-v8a")
        dir.mkdirs()
        nativeSetX11LibsDir(dir.absolutePath)
        return dir
    }

    fun createPluginUI(pathId: Long, pluginIndex: Int, pluginInstanceId: Long, uiInstanceId: Long, displayNumber: Int, parentWindowId: Long): Boolean =
        nativeCreatePluginUI(pathId, pluginIndex, pluginInstanceId, uiInstanceId, displayNumber, parentWindowId)
    fun destroyPluginUI(pathId: Long, pluginInstanceId: Long, uiInstanceId: Long) =
        nativeDestroyPluginUI(pathId, pluginInstanceId, uiInstanceId)

    fun idlePluginUIs(): Boolean = nativeIdlePluginUIs()

    // --- Real-time recording ---

    external fun nativeStartRecording(rawPath: String, processedPath: String): Boolean
    external fun nativeStopRecording()
    external fun nativeIsRecording(): Boolean
    external fun nativeGetRecordingDurationSec(): Double

    external fun nativeAddTrack(): Long
    external fun nativeRemoveTrack(trackId: Long): Boolean
    external fun nativeGetTracks(): Array<RackTrackInfo>
    external fun nativeSetTrackVolume(trackId: Long, volume: Float): Boolean
    external fun nativeSetTrackInputArmed(trackId: Long, armed: Boolean): Boolean
    external fun nativeLoadTrackWav(trackId: Long, path: String, displayName: String): Boolean
    external fun nativeUnloadTrackWav(trackId: Long): Boolean
    external fun nativeClearTrackWavs(): Boolean
    external fun nativeSetTransportPlaying(playing: Boolean): Boolean
    external fun nativeSetTransportBpm(bpm: Double): Boolean
    external fun nativeRestartTransport(): Boolean
    external fun nativeSetTransportLooping(looping: Boolean)
    external fun nativeGetTransportInfo(): TransportInfo
    external fun nativeSetRackBypass(bypass: Boolean)
    fun setRackBypass(bypass: Boolean) = nativeSetRackBypass(bypass)


    fun stopEngine() {
        nativeStopEngine()
    }

    fun isEngineRunning(): Boolean {
        return nativeIsEngineRunning()
    }

    fun getSampleRate(): Float = nativeGetSampleRate()
    fun getBufferFrameCount(): Int = nativeGetBufferFrameCount()

    fun getLatencyMs(): Double {
        return nativeGetLatencyMs()
    }

    fun getInputLevel(): Float = nativeGetInputLevel()
    fun getOutputLevel(): Float = nativeGetOutputLevel()
    fun getCpuLoad(): Float = nativeGetCpuLoad()
    fun getXRunCount(): Int = nativeGetXRunCount()
    fun isInputClipping(): Boolean = nativeIsInputClipping()
    fun isOutputClipping(): Boolean = nativeIsOutputClipping()
    fun resetClipping() = nativeResetClipping()

    fun getAvailablePlugins(): List<PluginInfo> {
        return nativeGetAvailablePlugins().toList()
    }

    fun addPluginToRack(pathId: Long, pluginId: String, position: Int = -1): Int =
        nativeAddPluginToRack(pathId, pluginId, position)
    fun removePluginFromRack(pathId: Long, position: Int): Boolean = nativeRemovePluginFromRack(pathId, position)
    fun reorderRack(pathId: Long, fromPos: Int, toPos: Int): Boolean = nativeReorderRack(pathId, fromPos, toPos)
    fun setPluginFilePath(pathId: Long, pluginIndex: Int, propertyUri: String, filePath: String) =
        nativeSetPluginFilePath(pathId, pluginIndex, propertyUri, filePath)
    fun setParameter(pathId: Long, pluginIndex: Int, portIndex: Int, value: Float) =
        nativeSetParameter(pathId, pluginIndex, portIndex, value)
    fun getParameter(pathId: Long, pluginIndex: Int, portIndex: Int): Float =
        nativeGetParameter(pathId, pluginIndex, portIndex)
    fun getRackSize(pathId: Long): Int = nativeGetRackSize(pathId)
    fun getRackPluginInfo(pathId: Long, index: Int): PluginInfo? = nativeGetRackPluginInfo(pathId, index)
    fun getRackPluginInstanceId(pathId: Long, index: Int): Long = nativeGetRackPluginInstanceId(pathId, index)
    fun getRackPlugins(pathId: Long): Array<RackPluginEntry> = nativeGetRackPlugins(pathId)

    fun addTrack(): Long = nativeAddTrack()
    fun removeTrack(trackId: Long): Boolean = nativeRemoveTrack(trackId)
    fun getTracks(): Array<RackTrackInfo> = nativeGetTracks()
    fun setTrackVolume(trackId: Long, volume: Float): Boolean = nativeSetTrackVolume(trackId, volume.coerceIn(0f, 1f))
    fun setTrackInputArmed(trackId: Long, armed: Boolean): Boolean = nativeSetTrackInputArmed(trackId, armed)
    fun loadTrackWav(trackId: Long, path: String, displayName: String): Boolean = nativeLoadTrackWav(trackId, path, displayName)
    fun unloadTrackWav(trackId: Long): Boolean = nativeUnloadTrackWav(trackId)
    fun clearTrackWavs(): Boolean = nativeClearTrackWavs()
    fun setTransportBpm(bpm: Double): Boolean = nativeSetTransportBpm(bpm.coerceIn(20.0, 400.0))
    fun setTransportPlaying(playing: Boolean): Boolean = nativeSetTransportPlaying(playing)
    fun restartTransport(): Boolean = nativeRestartTransport()
    fun setTransportLooping(looping: Boolean) = nativeSetTransportLooping(looping)
    fun getTransportInfo(): TransportInfo = nativeGetTransportInfo()

    fun startRecording(rawPath: String, processedPath: String): Boolean = nativeStartRecording(rawPath, processedPath)
    fun stopRecording() = nativeStopRecording()
    fun isRecording(): Boolean = nativeIsRecording()
    fun getRecordingDurationSec(): Double = nativeGetRecordingDurationSec()
    fun saveRackState(): String? = nativeSaveRackState()
    fun restorePluginState(
        pathId: Long, pluginIndex: Int, portValues: FloatArray, portIndices: IntArray,
        propertyKeys: Array<String>, propertyTypes: Array<String>,
        propertyValues: Array<ByteArray>, propertyFlags: IntArray
    ): Boolean = nativeRestorePluginState(pathId, pluginIndex, portValues, portIndices, propertyKeys, propertyTypes, propertyValues, propertyFlags)
}
