/*
 * Hardware-gated direct USB duplex stress/diagnostic instrumentation.
 *
 * This source set is never included in ordinary unit-test suites. The test
 * only proceeds when UsbManager reports an app-authorized USB Audio device;
 * otherwise it logs an exact SKIP line and uses JUnit Assume to skip cleanly.
 */
package com.vibes.dsp.engine

import android.content.Context
import android.hardware.usb.UsbConstants
import android.hardware.usb.UsbDevice
import android.hardware.usb.UsbManager
import android.os.SystemClock
import android.util.Log
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import kotlinx.coroutines.runBlocking
import org.junit.Assert.assertTrue
import org.junit.Assume.assumeTrue
import org.junit.Test
import org.junit.runner.RunWith
import java.io.File
import kotlin.math.PI
import kotlin.math.roundToInt
import kotlin.math.sin

@RunWith(AndroidJUnit4::class)
class DirectUsbDeviceStressTest {
    private val tag = "DirectUsbDeviceStress"
    private val durationMs = 1_500L
    private val warmupMs = 250L
    private val maxXrunGrowth = 0L
    private val buffers = intArrayOf(16, 32, 64, 128, 256, 512, 1024)
    private val multipliers = intArrayOf(1, 2, 3)

    @Test
    fun duplexRateBufferLifecycleStress() {
        val context = InstrumentationRegistry.getInstrumentation().targetContext
        val usb = context.getSystemService(Context.USB_SERVICE) as UsbManager
        val audioDevices = usb.deviceList.values.filter(::isUsbAudio)
        val authorized = audioDevices.filter(usb::hasPermission)
        if (authorized.isEmpty()) {
            Log.i(tag, "SKIP reason=no-authorized-usb-audio-device discovered=${audioDevices.size}")
        }
        assumeTrue("SKIP reason=no-authorized-usb-audio-device", authorized.isNotEmpty())

        val option = DirectUsbAudioManager.getAudioDevices(context)
            .firstOrNull { candidate -> authorized.any { it.deviceId == candidate.id } }
        if (option == null) {
            Log.i(tag, "SKIP reason=authorized-device-not-exposed-by-manager")
        }
        assumeTrue("SKIP reason=authorized-device-not-exposed-by-manager", option != null)

        val engine = NativeEngine.getInstance()
        val originalRate = AudioSettingsManager.getDirectUsbRate(context)
        val originalBits = AudioSettingsManager.getDirectUsbBits(context)
        val originalSubslot = AudioSettingsManager.getDirectUsbSubslot(context)
        val originalChannels = AudioSettingsManager.getDirectUsbChannels(context)
        val originalInputChannel = AudioSettingsManager.getDirectUsbInputChannel(context)
        val originalOutputPair = AudioSettingsManager.getDirectUsbOutputPair(context)
        val originalBuffer = AudioSettingsManager.getBufferSize(context)
        val originalMultiplier = AudioSettingsManager.getDirectUsbPeriodMultiplier(context)
        var cases = 0
        AudioSettingsManager.setDirectUsbInputChannel(context, 0)
        AudioSettingsManager.setDirectUsbOutputPair(context, 0)
        var passed = true
        try {
            EngineInitHelper.preloadLilv(context.applicationInfo.nativeLibraryDir)
            val nativeInitialized = EngineInitHelper.initEngine(context)
            if (!nativeInitialized) {
                Log.e(tag, "FAIL phase=native-init reason=nativeInit-failed")
                assertTrue("Native engine initialization failed", false)
            }
            val probe = runBlocking {
                DirectUsbAudioManager.probeFormats(context, option!!)
            }
            if (probe.isFailure) {
                val message = probe.exceptionOrNull()?.message ?: "unknown-probe-failure"
                Log.e(tag, "FAIL phase=probe reason=probe-failed detail=$message")
                assertTrue("Direct USB probe failed: $message", false)
            }
            val matrix = probe.getOrThrow()
                .filter { format ->
                    format.sampleRate == 44_100 || format.sampleRate == 48_000
                }
                .filter { format ->
                    format.bits > 0 && format.subslotBytes > 0 &&
                        format.bits <= format.subslotBytes * 8 && format.channels >= 2 &&
                        format.channels % 2 == 0
                }
                .groupBy { it.sampleRate }
                .values
                .map { formats ->
                    formats.sortedWith(
                        compareByDescending<DirectUsbFormat> { it.bits == 24 }
                            .thenByDescending { it.subslotBytes }
                            .thenByDescending { it.channels }
                            .thenByDescending { it.bits }
                    ).first()
                }
                .sortedBy { it.sampleRate }
            if (matrix.isEmpty()) {
                Log.i(tag, "SKIP reason=no-supported-44100-or-48000-format")
            }
            assumeTrue("SKIP reason=no-supported-44100-or-48000-format", matrix.isNotEmpty())
            assertTrue(
                "USB interface exposes no capture channels",
                DirectUsbAudioManager.getInputChannelCount() > 0
            )

            for (cycle in 1..2) {
                for (multiplier in multipliers) {
                    AudioSettingsManager.setDirectUsbPeriodMultiplier(context, multiplier)
                    for (format in matrix) {
                        for (buffer in buffers) {
                            cases++
                            val result = runCase(
                                context = context,
                                engine = engine,
                                format = format,
                                buffer = buffer,
                                multiplier = multiplier,
                                cycle = cycle,
                            )
                            if (!result) {
                                assertTrue(
                                    "Direct USB case failed: cycle=$cycle rate=${format.sampleRate} " +
                                        "buffer=$buffer multiplier=$multiplier",
                                    result
                                )
                            }
                            passed = passed && result
                            val lifecycleStats = engine.getDirectUsbStats()
                            val stopped = lifecycleStats.state != DirectUsbSessionState.Running &&
                                lifecycleStats.state != DirectUsbSessionState.Starting
                            Log.i(
                                tag,
                                telemetry(
                                    "lifecycle-after-stop", cycle, format, buffer, multiplier,
                                    lifecycleStats, rawStats = engine.nativeGetDirectUsbStats(),
                                    predicate = { stopped }
                                )
                            )
                            assertTrue(
                                "Direct USB remained active after stop; inspect lifecycle telemetry",
                                stopped
                            )
                        }
                    }
                }
            }
            assertTrue("At least one direct USB case was executed", cases > 0)
            assertTrue("One or more direct USB cases failed; inspect FAIL telemetry", passed)
        } finally {
            DirectUsbAudioManager.disable(context)
            AudioSettingsManager.setDirectUsbFormat(
                context, originalRate, originalBits, originalSubslot, originalChannels
            )
            AudioSettingsManager.setBufferSize(context, originalBuffer)
            AudioSettingsManager.setDirectUsbInputChannel(context, originalInputChannel)
            AudioSettingsManager.setDirectUsbOutputPair(context, originalOutputPair)
            AudioSettingsManager.setDirectUsbPeriodMultiplier(context, originalMultiplier)
        }
        Log.i(tag, "SUMMARY cases=$cases result=${if (passed) "PASS" else "FAIL"}")
    }

    private fun runCase(
        context: Context,
        engine: NativeEngine,
        format: DirectUsbFormat,
        buffer: Int,
        multiplier: Int,
        cycle: Int,
    ): Boolean {
        AudioSettingsManager.setBufferSize(context, buffer)
        DirectUsbAudioManager.startSelected(context, format)
        var temporaryTrackId = 0L
        var wav: File? = null
        var wavAttached = false
        var originalLooping = false
        var failure: String? = null
        try {
            val started = runBlocking { DirectUsbAudioManager.startConfigured(context) }
            if (started.isFailure) {
                val detail = started.exceptionOrNull()?.message ?: "unknown-start-failure"
                val reason = "start-failed detail=$detail"
                logFailure(reason, cycle, format, buffer, multiplier, engine)
                return false
            }

            val runningDeadline = SystemClock.elapsedRealtime() + 1_000L
            var runningStats: DirectUsbStats? = null
            while (SystemClock.elapsedRealtime() < runningDeadline) {
                val stats = engine.getDirectUsbStats()
                if (stats.state == DirectUsbSessionState.Failed) {
                    failure = "session-failed code=${stats.failure}"
                    break
                }
                if (stats.state == DirectUsbSessionState.Running) {
                    runningStats = stats
                    break
                }
                SystemClock.sleep(10)
            }
            if (failure == null && runningStats == null) {
                failure = "session-not-running-within-1s"
            }
            if (failure == null && runningStats != null) {
                failure = validateConfiguration(runningStats, format, buffer, multiplier)
            }
            if (failure != null) {
                logFailure(failure, cycle, format, buffer, multiplier, engine)
                return false
            }
            originalLooping = engine.getWavTransportInfo().looping


            temporaryTrackId = engine.addTrack()
            if (temporaryTrackId <= 0L) {
                val reason = "temporary-track-create-failed"
                logFailure(reason, cycle, format, buffer, multiplier, engine)
                return false
            }
            wav = createStressWav(context.cacheDir, format.sampleRate)
            if (!engine.loadTrackWav(temporaryTrackId, wav.absolutePath, wav.name)) {
                val reason = "track-wav-load-failed"
                logFailure(reason, cycle, format, buffer, multiplier, engine)
                return false
            }
            wavAttached = true

            engine.setWavTransportLooping(true)
            if (!engine.restartWavTransport() || !engine.setWavTransportPlaying(true)) {
                val reason = "wav-transport-start-failed"
                logFailure(reason, cycle, format, buffer, multiplier, engine)
                return false
            }
            val transport = engine.getWavTransportInfo()
            if (!transport.playing || !transport.looping || transport.loadedTrackCount <= 0 ||
                transport.durationSec <= 0.0
            ) {
                val reason = "wav-transport-info-invalid"
                logFailure(reason, cycle, format, buffer, multiplier, engine)
                return false
            }

            val start = SystemClock.elapsedRealtime()
            val deadline = start + durationMs
            val warmupDeadline = start + warmupMs
            var warmupStats: DirectUsbStats? = null
            var previousSequence = engine.getDirectUsbStats().sequence
            var framesObserved = 0L
            var previousPosition = transport.positionSec
            var positionChanged = false
            while (SystemClock.elapsedRealtime() < deadline && failure == null) {
                val stats = engine.getDirectUsbStats()
                val rawStats = engine.nativeGetDirectUsbStats()
                failure = validateRunningStats(stats, rawStats, format, buffer, multiplier)
                if (failure == null && stats.sequence < previousSequence) {
                    failure = "capture-sequence-regressed"
                }
                if (failure == null && stats.sequence > previousSequence) {
                    framesObserved += stats.sequence - previousSequence
                }
                previousSequence = stats.sequence
                val currentTransport = engine.getWavTransportInfo()
                positionChanged = positionChanged || currentTransport.positionSec != previousPosition
                previousPosition = currentTransport.positionSec
                if (warmupStats == null && SystemClock.elapsedRealtime() >= warmupDeadline) {
                    warmupStats = stats
                    if (rawStats.getOrZero(EVENT_THREAD_URGENT_AUDIO) != 1L ||
                        rawStats.getOrZero(RENDER_THREAD_URGENT_AUDIO) != 1L
                    ) {
                        failure = "urgent-audio-thread-not-enabled"
                    }
                }
                SystemClock.sleep(10)
            }
            val finalStats = engine.getDirectUsbStats()
            val finalRawStats = engine.nativeGetDirectUsbStats()
            val finalTransport = engine.getWavTransportInfo()
            val baselineActualXruns = warmupStats?.actualXruns ?: finalStats.actualXruns
            val actualXrunGrowth = (finalStats.actualXruns - baselineActualXruns).coerceAtLeast(0L)
            if (failure == null && framesObserved == 0L) failure = "no-capture-sequence-progress"
            if (failure == null && !positionChanged) failure = "wav-position-did-not-advance"
            if (failure == null && actualXrunGrowth > maxXrunGrowth) {
                failure = "actual-xrun-growth-exceeded"
            }
            if (failure == null) {
                failure = validateRunningStats(finalStats, finalRawStats, format, buffer, multiplier)
            }
            Log.i(
                tag,
                telemetry(
                    failure ?: "pass", cycle, format, buffer, multiplier, finalStats,
                    finalRawStats, predicate = {
                        finalTransport.playing && finalTransport.looping &&
                            finalTransport.loadedTrackCount > 0
                    }
                ) + " wav_position_sec=${finalTransport.positionSec} " +
                    "wav_duration_sec=${finalTransport.durationSec} " +
                    "wav_loaded_tracks=${finalTransport.loadedTrackCount} " +
                    "actual_xrun_growth=$actualXrunGrowth"
            )
            return failure == null
        } finally {
            if (wavAttached) {
                runCatching { engine.setWavTransportPlaying(false) }
            }
            if (temporaryTrackId > 0L) {
                runCatching { engine.unloadTrackWav(temporaryTrackId) }
                runCatching { engine.removeTrack(temporaryTrackId) }
                runCatching { engine.setWavTransportLooping(originalLooping) }
            }
            wav?.delete()
            DirectUsbAudioManager.disable(context)
        }
    }

    private fun validateConfiguration(
        stats: DirectUsbStats,
        format: DirectUsbFormat,
        buffer: Int,
        multiplier: Int,
    ): String? {
        if (stats.schemaVersion < 2L) return "unsupported-stats-schema-${stats.schemaVersion}"
        if (stats.periodMultiplier != multiplier.toLong()) {
            return "period-multiplier-mismatch-${stats.periodMultiplier}"
        }
        if (stats.effectiveQuantum != buffer.toLong()) {
            return "effective-quantum-mismatch-${stats.effectiveQuantum}"
        }
        val configuredTarget = minOf(1024L, buffer.toLong() * multiplier)
        if (stats.steadyTarget < configuredTarget) {
            return "steady-target-below-configured-${stats.steadyTarget}-expected-at-least-$configuredTarget"
        }
        if (stats.steadyTarget < stats.startupPrime) {
            return "steady-target-below-startup-prime-${stats.steadyTarget}-${stats.startupPrime}"
        }
        if (stats.steadyTarget + stats.effectiveQuantum > ringFrameLimit(format)) {
            return "steady-target-exceeds-ring-capacity-${stats.steadyTarget}"
        }
        if (stats.startupPrime < configuredTarget) return "startup-prime-too-small-${stats.startupPrime}"
        if (stats.knownHostLatencyFrames <= 0L) return "host-latency-unavailable"
        if (stats.sampleRateHz != format.sampleRate.toLong()) return "sample-rate-mismatch-${stats.sampleRateHz}"
        return null
    }

    private fun validateRunningStats(
        stats: DirectUsbStats,
        rawStats: LongArray,
        format: DirectUsbFormat,
        buffer: Int,
        multiplier: Int,
    ): String? {
        if (stats.state == DirectUsbSessionState.Failed) return "session-failed code=${stats.failure}"
        if (stats.state != DirectUsbSessionState.Running) return "session-state-${stats.state}"
        validateConfiguration(stats, format, buffer, multiplier)?.let { return it }
        if (stats.queuedOut <= 0L) return "queued-output-empty"
        if (stats.captureTransferErrors != 0L ||
            stats.playbackTransferErrors != 0L ||
            rawStats.getOrZero(CAPTURE_TRANSFER_ERRORS) != 0L ||
            rawStats.getOrZero(PLAYBACK_TRANSFER_ERRORS) != 0L ||
            rawStats.getOrZero(LIFECYCLE_FAILURES) != 0L ||
            rawStats.getOrZero(TRANSPORT_FAILED) != 0L
        ) {
            return "transfer-lifecycle-transport-failure"
        }
        if (rawStats.getOrZero(CAPTURE_RING_FRAMES) > ringFrameLimit(format) ||
            rawStats.getOrZero(PLAYBACK_RING_FRAMES) > ringFrameLimit(format) ||
            rawStats.getOrZero(IMPLICIT_FIFO_DEPTH) > MAX_IMPLICIT_FIFO
        ) {
            return "usb-queue-depth-exceeded"
        }
        return null
    }

    private fun logFailure(
        reason: String,
        cycle: Int,
        format: DirectUsbFormat,
        buffer: Int,
        multiplier: Int,
        engine: NativeEngine,
    ) {
        val stats = engine.getDirectUsbStats()
        val nativeDetail = runCatching {
            engine.nativeGetDirectUsbErrorDetail()
                .replace("\r", " ")
                .replace("\n", " ")
        }.getOrDefault("")
        Log.e(
            tag,
            telemetry(reason, cycle, format, buffer, multiplier, stats, engine.nativeGetDirectUsbStats()) +
                " native_detail=$nativeDetail"
        )
    }

    private fun telemetry(
        reason: String,
        cycle: Int,
        format: DirectUsbFormat,
        buffer: Int,
        multiplier: Int,
        stats: DirectUsbStats,
        rawStats: LongArray,
        predicate: () -> Boolean = { true },
    ): String {
        return "TELEMETRY reason=$reason cycle=$cycle rate=${format.sampleRate} " +
            "bits=${format.bits} bytes=${format.subslotBytes} channels=${format.channels} " +
            "buffer=$buffer multiplier=$multiplier schema=${stats.schemaVersion} " +
            "state=${stats.state} failure=${stats.failure} period_multiplier=${stats.periodMultiplier} " +
            "effective_quantum=${stats.effectiveQuantum} steady_target_frames=${stats.steadyTarget} " +
            "startup_prime_frames=${stats.startupPrime} queued_out_frames=${stats.queuedOut} " +
            "known_host_latency_frames=${stats.knownHostLatencyFrames} sequence=${stats.sequence} " +
            "capture_overruns=${rawStats.getOrZero(CAPTURE_OVERRUNS)} " +
            "capture_underruns=${rawStats.getOrZero(CAPTURE_UNDERRUNS)} " +
            "capture_transfer_errors=${stats.captureTransferErrors} playback_transfer_errors=${stats.playbackTransferErrors} " +
            "capture_wait_pressure=${stats.captureWaitPressure} write_wait_pressure=${stats.writeWaitPressure} " +
            "playback_xruns=${stats.playbackXruns} lifecycle_failures=${rawStats.getOrZero(LIFECYCLE_FAILURES)} " +
            "transport_failed=${rawStats.getOrZero(TRANSPORT_FAILED)} capture_ring_frames=${rawStats.getOrZero(CAPTURE_RING_FRAMES)} " +
            "playback_ring_frames=${rawStats.getOrZero(PLAYBACK_RING_FRAMES)} implicit_fifo_depth=${rawStats.getOrZero(IMPLICIT_FIFO_DEPTH)} " +
            "last_dsp_ns=${stats.lastDspNs} peak_dsp_ns=${stats.peakDspNs} actual_xruns=${stats.actualXruns} " +
            "capture_transfer_frames=${stats.captureTransferFrames} predicate=${predicate()}"
    }

    private fun createStressWav(directory: File, sampleRate: Int): File {
        val frames = sampleRate * 2
        val dataBytes = frames * 2
        val bytes = ByteArray(44 + dataBytes)
        fun ascii(offset: Int, value: String) {
            value.toByteArray(Charsets.US_ASCII).copyInto(bytes, offset)
        }
        fun le16(offset: Int, value: Int) {
            bytes[offset] = (value and 0xff).toByte()
            bytes[offset + 1] = ((value ushr 8) and 0xff).toByte()
        }
        fun le32(offset: Int, value: Int) {
            le16(offset, value)
            le16(offset + 2, value ushr 16)
        }
        ascii(0, "RIFF")
        le32(4, 36 + dataBytes)
        ascii(8, "WAVE")
        ascii(12, "fmt ")
        le32(16, 16)
        le16(20, 1)
        le16(22, 1)
        le32(24, sampleRate)
        le32(28, sampleRate * 2)
        le16(32, 2)
        le16(34, 16)
        ascii(36, "data")
        le32(40, dataBytes)
        for (frame in 0 until frames) {
            val sample = (sin(2.0 * PI * 440.0 * frame / sampleRate) * 12_000.0).roundToInt()
            le16(44 + frame * 2, sample)
        }
        val file = File.createTempFile("direct-usb-stress-$sampleRate-", ".wav", directory)
        file.writeBytes(bytes)
        return file
    }

    private fun ringFrameLimit(format: DirectUsbFormat): Long =
        65_536L / (format.channels.toLong() * format.subslotBytes.coerceAtLeast(1))

    private fun isUsbAudio(device: UsbDevice): Boolean =
        device.deviceClass == UsbConstants.USB_CLASS_AUDIO ||
            (0 until device.interfaceCount).any { device.getInterface(it).interfaceClass == UsbConstants.USB_CLASS_AUDIO }

    private fun LongArray.getOrZero(index: Int): Long = getOrNull(index) ?: 0L

    private companion object {
        const val MAX_IMPLICIT_FIFO = 256L
        const val CAPTURE_OVERRUNS = 1
        const val CAPTURE_UNDERRUNS = 2
        const val IMPLICIT_FIFO_DEPTH = 3
        const val CAPTURE_TRANSFER_ERRORS = 5
        const val PLAYBACK_TRANSFER_ERRORS = 6
        const val PLAYBACK_RING_FRAMES = 7
        const val CAPTURE_RING_FRAMES = 8
        const val LIFECYCLE_FAILURES = 9
        const val TRANSPORT_FAILED = 10
        const val EVENT_THREAD_URGENT_AUDIO = 11
        const val RENDER_THREAD_URGENT_AUDIO = 12
    }
}
