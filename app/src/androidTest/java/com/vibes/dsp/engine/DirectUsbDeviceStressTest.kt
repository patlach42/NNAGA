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
    private val buffers = intArrayOf(512, 1024)

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
        var cases = 0
        AudioSettingsManager.setDirectUsbInputChannel(context, 0)
        AudioSettingsManager.setDirectUsbOutputPair(context, 0)
        var passed = true
        try {
            val probe = runBlocking {
                DirectUsbAudioManager.probeFormats(context, option!!)
            }
            if (probe.isFailure) {
                val message = probe.exceptionOrNull()?.message ?: "unknown-probe-failure"
                Log.e(tag, "FAIL phase=probe reason=probe-failed detail=$message")
                assertTrue("Direct USB probe failed: $message", false)
            }
            val matrix = probe.getOrThrow()
                .filter { it.sampleRate == 44_100 || it.sampleRate == 48_000 }
                .distinctBy { listOf(it.sampleRate, it.bits, it.subslotBytes, it.channels) }
            if (matrix.isEmpty()) {
                Log.i(tag, "SKIP reason=no-supported-44100-or-48000-format")
            }
            assumeTrue("SKIP reason=no-supported-44100-or-48000-format", matrix.isNotEmpty())
            assertTrue(
                "USB interface exposes no capture channels",
                DirectUsbAudioManager.getInputChannelCount() > 0
            )

            for (cycle in 1..2) {
                for (format in matrix) {
                    for (buffer in buffers) {
                        cases++
                        val result = runCase(context, engine, format, buffer, cycle)
                        passed = passed && result
                        DirectUsbAudioManager.disable(context)
                        val lifecycleStats = engine.nativeGetDirectUsbStats()
                        val stopped = !engine.nativeIsDirectUsbOutputStreaming()
                        Log.i(
                            tag,
                            telemetry(
                                "lifecycle-after-stop", cycle, format, buffer,
                                lifecycleStats, engine.getXRunCount().toLong()
                            ) { stopped }
                        )
                        assertTrue(
                            "Direct USB remained streaming after stop; inspect lifecycle telemetry",
                            stopped
                        )
                    }
                }
            }
            assertTrue("At least one direct USB case was executed", cases > 0)
            assertTrue("One or more direct USB cases failed; inspect FAIL telemetry", passed)
        } finally {
            DirectUsbAudioManager.disable(context)
            engine.setChainBypass(false)
            engine.setWavBypassChain(false)
            AudioSettingsManager.setDirectUsbFormat(
                context, originalRate, originalBits, originalSubslot, originalChannels
            )
            AudioSettingsManager.setBufferSize(context, originalBuffer)
            AudioSettingsManager.setDirectUsbInputChannel(context, originalInputChannel)
            AudioSettingsManager.setDirectUsbOutputPair(context, originalOutputPair)
        }
        Log.i(tag, "SUMMARY cases=$cases result=${if (passed) "PASS" else "FAIL"}")
    }

    private fun runCase(
        context: Context,
        engine: NativeEngine,
        format: DirectUsbFormat,
        buffer: Int,
        cycle: Int,
    ): Boolean {
        AudioSettingsManager.setBufferSize(context, buffer)
        DirectUsbAudioManager.startSelected(context, format)
        engine.setChainBypass(true)
        val started = runBlocking { DirectUsbAudioManager.startConfigured(context) }
        if (started.isFailure) {
            val detail = started.exceptionOrNull()?.message ?: "unknown-start-failure"
            Log.e(
                tag,
                "FAIL phase=start cycle=$cycle rate=${format.sampleRate} bits=${format.bits} " +
                    "bytes=${format.subslotBytes} channels=${format.channels} buffer=$buffer " +
                    "reason=start-failed detail=$detail"
            )
            return false
        }

        val startWaitDeadline = SystemClock.elapsedRealtime() + 1_000L
        while (!engine.nativeIsDirectUsbOutputStreaming() &&
            SystemClock.elapsedRealtime() < startWaitDeadline
        ) {
            SystemClock.sleep(10)
        }
        if (!engine.nativeIsDirectUsbOutputStreaming()) {
            val stats = engine.nativeGetDirectUsbStats()
            Log.e(tag, telemetry("not-streaming", cycle, format, buffer, stats, engine.getXRunCount().toLong()))
            return false
        }
        val initialStats = engine.nativeGetDirectUsbStats()
        if (initialStats.size < REQUIRED_STATS_SIZE) {
            Log.e(
                tag,
                "FAIL phase=diagnostics reason=diagnostics-seam-unavailable " +
                    "required_stats=$REQUIRED_STATS_SIZE actual_stats=${initialStats.size}"
            )
            return false
        }

        val start = SystemClock.elapsedRealtime()
        val deadline = start + durationMs
        val warmupDeadline = start + warmupMs
        var warmupStats: LongArray? = if (warmupMs == 0L) engine.nativeGetDirectUsbStats() else null
        var warmupXruns = if (warmupMs == 0L) engine.getXRunCount().toLong() else 0L
        var previousSequence = engine.nativeGetDirectUsbStats().getOrZero(CAPTURE_SEQUENCE)
        var framesObserved = 0L
        var failure: String? = null
        while (SystemClock.elapsedRealtime() < deadline && failure == null) {
            val stats = engine.nativeGetDirectUsbStats()
            val sequence = stats.getOrZero(CAPTURE_SEQUENCE)
            if (stats.size < REQUIRED_STATS_SIZE) {
                failure = "diagnostics-seam-unavailable"
            } else if (sequence < previousSequence) {
                failure = "capture-sequence-regressed"
            } else if (stats.getOrZero(CAPTURE_RING_FRAMES) > ringFrameLimit(format)) {
                failure = "capture-ring-depth-exceeded"
            } else if (stats.getOrZero(PLAYBACK_RING_FRAMES) > ringFrameLimit(format)) {
                failure = "playback-ring-depth-exceeded"
            } else if (stats.getOrZero(IMPLICIT_FIFO_DEPTH) > MAX_IMPLICIT_FIFO) {
                failure = "implicit-fifo-depth-exceeded"
            } else if (stats.getOrZero(CAPTURE_TRANSFER_ERRORS) != 0L ||
                stats.getOrZero(PLAYBACK_TRANSFER_ERRORS) != 0L
            ) {
                failure = "transfer-errors"
            } else if (stats.getOrZero(LIFECYCLE_FAILURES) != 0L) {
                failure = "lifecycle-failure"
            } else if (stats.getOrZero(TRANSPORT_FAILED) != 0L) {
                failure = "transport-failed"
            }
            previousSequence = sequence
            if (sequence > 0L) framesObserved = sequence
            if (warmupStats == null && SystemClock.elapsedRealtime() >= warmupDeadline) {
                warmupStats = stats
                warmupXruns = engine.getXRunCount().toLong()
                if (stats.getOrZero(EVENT_THREAD_URGENT_AUDIO) != 1L ||
                    stats.getOrZero(RENDER_THREAD_URGENT_AUDIO) != 1L
                ) {
                    failure = "urgent-audio-thread-not-enabled"
                }
            }
            SystemClock.sleep(10)
        }
        val captureStats = engine.nativeGetDirectUsbStats()
        if (warmupStats == null) {
            warmupStats = captureStats
            warmupXruns = engine.getXRunCount().toLong()
        }
        val captureXruns = engine.getXRunCount().toLong()
        val captureXrunGrowth = (captureXruns - warmupXruns).coerceAtLeast(0L)
        if (failure == null && framesObserved == 0L) failure = "no-capture-sequence-progress"
        if (failure == null && captureXrunGrowth > maxXrunGrowth) failure = "xrun-growth-exceeded"
        if (failure == null) {
            failure = runWavPhase(context, engine, format, buffer, cycle)
        }

        val finalStats = engine.nativeGetDirectUsbStats()
        val finalXruns = engine.getXRunCount().toLong()
        DirectUsbAudioManager.disable(context)
        if (failure != null || engine.nativeIsDirectUsbOutputStreaming()) {
            val reason = failure ?: "streaming-after-stop"
            Log.e(tag, telemetry(reason, cycle, format, buffer, finalStats, finalXruns))
            return false
        }
        Log.i(tag, telemetry("pass", cycle, format, buffer, finalStats, finalXruns))
        return true
    }

    private fun runWavPhase(
        context: Context,
        engine: NativeEngine,
        format: DirectUsbFormat,
        buffer: Int,
        cycle: Int,
    ): String? {
        val wav = createStressWav(context.cacheDir, format.sampleRate)
        var failure: String? = null
        try {
            if (!engine.loadWav(wav)) return "wav-load-failed"
            engine.setWavBypassChain(true)
            val before = engine.nativeGetDirectUsbStats()
            val beforeWritten = before.getOrZero(WRITTEN_FRAMES)
            val beforePlayed = before.getOrZero(PLAYED_FRAMES)
            var previousWritten = beforeWritten
            var previousPlayed = beforePlayed
            var previousPosition = engine.getWavPositionSec()
            var outputSeen = false
            val start = SystemClock.elapsedRealtime()
            val warmupDeadline = start + warmupMs
            val deadline = start + durationMs.coerceAtMost(1_000L)
            var warmupXruns: Long? = null
            engine.wavPlay()
            while (SystemClock.elapsedRealtime() < deadline && failure == null) {
                val stats = engine.nativeGetDirectUsbStats()
                val position = engine.getWavPositionSec()
                val written = stats.getOrZero(WRITTEN_FRAMES)
                val played = stats.getOrZero(PLAYED_FRAMES)
                if (position < previousPosition) {
                    failure = "wav-position-regressed"
                } else if (written < previousWritten || played < previousPlayed) {
                    failure = "wav-frame-counter-regressed"
                } else if (stats.getOrZero(CAPTURE_RING_FRAMES) > ringFrameLimit(format)) {
                    failure = "wav-capture-ring-depth-exceeded"
                } else if (stats.getOrZero(CAPTURE_TRANSFER_ERRORS) != 0L ||
                    stats.getOrZero(PLAYBACK_TRANSFER_ERRORS) != 0L ||
                    stats.getOrZero(LIFECYCLE_FAILURES) != 0L ||
                    stats.getOrZero(TRANSPORT_FAILED) != 0L
                ) {
                    failure = "wav-transport-or-lifecycle-failure"
                }
                previousPosition = position
                previousWritten = written
                previousPlayed = played
                outputSeen = outputSeen || engine.getOutputLevel() > 0.0001f
                if (warmupXruns == null && SystemClock.elapsedRealtime() >= warmupDeadline) {
                    warmupXruns = engine.getXRunCount().toLong()
                }
                SystemClock.sleep(10)
            }
            val finalStats = engine.nativeGetDirectUsbStats()
            val finalPosition = engine.getWavPositionSec()
            val finalXruns = engine.getXRunCount().toLong()
            val xrunGrowth = warmupXruns?.let { (finalXruns - it).coerceAtLeast(0L) } ?: 0L
            if (failure == null && finalPosition <= 0.0) failure = "wav-position-did-not-advance"
            if (failure == null && finalStats.getOrZero(WRITTEN_FRAMES) <= beforeWritten) {
                failure = "wav-written-frames-did-not-advance"
            }
            if (failure == null && finalStats.getOrZero(PLAYED_FRAMES) <= beforePlayed) {
                failure = "wav-played-frames-did-not-advance"
            }
            if (failure == null && !outputSeen) failure = "wav-output-level-remained-zero"
            if (failure == null && xrunGrowth > maxXrunGrowth) failure = "wav-xrun-growth-exceeded"
            Log.i(
                tag,
                "WAV_TELEMETRY reason=${failure ?: "pass"} cycle=$cycle rate=${format.sampleRate} " +
                    "bits=${format.bits} bytes=${format.subslotBytes} channels=${format.channels} " +
                    "buffer=$buffer position_sec=$finalPosition duration_sec=${engine.getWavDurationSec()} " +
                    "output_level=${engine.getOutputLevel()} written_frames=${finalStats.getOrZero(WRITTEN_FRAMES)} " +
                    "played_frames=${finalStats.getOrZero(PLAYED_FRAMES)} capture_ring_frames=${finalStats.getOrZero(CAPTURE_RING_FRAMES)} " +
                    "xrun_count=$finalXruns"
            )
        } finally {
            engine.wavPause()
            engine.unloadWav()
            wav.delete()
        }
        return failure
    }

    private fun telemetry(
        reason: String,
        cycle: Int,
        format: DirectUsbFormat,
        buffer: Int,
        stats: LongArray,
        xruns: Long,
        predicate: () -> Boolean = { true },
    ): String {
        return "TELEMETRY reason=$reason cycle=$cycle rate=${format.sampleRate} " +
            "bits=${format.bits} bytes=${format.subslotBytes} channels=${format.channels} " +
            "buffer=$buffer sequence=${stats.getOrZero(CAPTURE_SEQUENCE)} " +
            "capture_overruns=${stats.getOrZero(CAPTURE_OVERRUNS)} " +
            "capture_underruns=${stats.getOrZero(CAPTURE_UNDERRUNS)} " +
            "fifo_depth=${stats.getOrZero(IMPLICIT_FIFO_DEPTH)} " +
            "fallback_packets=${stats.getOrZero(IMPLICIT_FALLBACK_PACKETS)} " +
            "capture_transfer_errors=${stats.getOrZero(CAPTURE_TRANSFER_ERRORS)} " +
            "playback_transfer_errors=${stats.getOrZero(PLAYBACK_TRANSFER_ERRORS)} " +
            "playback_ring_frames=${stats.getOrZero(PLAYBACK_RING_FRAMES)} " +
            "capture_ring_frames=${stats.getOrZero(CAPTURE_RING_FRAMES)} " +
            "lifecycle_failures=${stats.getOrZero(LIFECYCLE_FAILURES)} " +
            "transport_failed=${stats.getOrZero(TRANSPORT_FAILED)} " +
            "event_thread_urgent_audio=${stats.getOrZero(EVENT_THREAD_URGENT_AUDIO)} " +
            "render_thread_urgent_audio=${stats.getOrZero(RENDER_THREAD_URGENT_AUDIO)} " +
            "written_frames=${stats.getOrZero(WRITTEN_FRAMES)} " +
            "played_frames=${stats.getOrZero(PLAYED_FRAMES)} " +
            "xrun_count=$xruns predicate=${predicate()}"
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
        val file = File(directory, "direct-usb-stress-${sampleRate}.wav")
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
        const val REQUIRED_STATS_SIZE = 15
        const val CAPTURE_SEQUENCE = 0
        const val CAPTURE_OVERRUNS = 1
        const val CAPTURE_UNDERRUNS = 2
        const val IMPLICIT_FIFO_DEPTH = 3
        const val IMPLICIT_FALLBACK_PACKETS = 4
        const val CAPTURE_TRANSFER_ERRORS = 5
        const val PLAYBACK_TRANSFER_ERRORS = 6
        const val PLAYBACK_RING_FRAMES = 7
        const val CAPTURE_RING_FRAMES = 8
        const val LIFECYCLE_FAILURES = 9
        const val TRANSPORT_FAILED = 10
        const val EVENT_THREAD_URGENT_AUDIO = 11
        const val RENDER_THREAD_URGENT_AUDIO = 12
        const val WRITTEN_FRAMES = 13
        const val PLAYED_FRAMES = 14
    }
}
