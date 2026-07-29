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
import android.os.Bundle
import android.os.SystemClock
import android.util.Log
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import kotlinx.coroutines.runBlocking
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertThrows
import org.junit.Assert.assertTrue
import org.junit.Assume.assumeTrue
import org.junit.Test
import org.junit.runner.RunWith
import java.io.File
import kotlin.math.PI
import kotlin.math.abs
import kotlin.math.ceil
import kotlin.math.roundToInt
import kotlin.math.sin

@RunWith(AndroidJUnit4::class)
class DirectUsbDeviceStressTest {
    private val tag = "DirectUsbDeviceStress"
    private val defaultRates = intArrayOf(44_100, 48_000)
    private val defaultBuffers = intArrayOf(16, 32, 64, 128, 256, 512, 1024)
    private val defaultMultipliers = intArrayOf(1, 2, 3)

    @Test
    fun duplexRateBufferLifecycleStress() {
        val args = InstrumentationRegistry.getArguments()
        val selectedRates = argumentCsv(args, "direct_usb_rates", defaultRates)
        val selectedBuffers = argumentCsv(args, "direct_usb_buffers", defaultBuffers)
        val selectedMultipliers = argumentCsv(args, "direct_usb_multipliers", defaultMultipliers)
        val cycles = argumentInt(args, "direct_usb_cycles", "cycles", 2, 2, 8)
        val durationMs = argumentLong(args, "direct_usb_duration_ms", "duration_ms", 5_000L, 5_000L, 30_000L)
        val warmupMs = minOf(1_000L, (durationMs / 3L).coerceAtLeast(250L))
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
        var originalTransport: TransportInfo? = null
        val results = linkedMapOf<CaseKey, MutableList<CaseResult>>()
        var cases = 0
        AudioSettingsManager.setDirectUsbInputChannel(context, 0)
        AudioSettingsManager.setDirectUsbOutputPair(context, 0)
        try {
            EngineInitHelper.preloadLilv(context.applicationInfo.nativeLibraryDir)
            assertTrue("Native engine initialization failed", EngineInitHelper.initEngine(context))
            originalTransport = runCatching { engine.getTransportInfo() }.getOrNull()
            val probe = runBlocking { DirectUsbAudioManager.probeFormats(context, option!!) }
            assertTrue("Direct USB probe failed: ${probe.exceptionOrNull()?.message}", probe.isSuccess)
            // probeFormats may return manager fallbacks when native descriptors are empty.
            // Only native descriptor tuples are verified negotiated formats.
            val verifiedFormats = runCatching {
                engine.nativeGetDirectUsbOutputFormats()
                    .asSequence()
                    .chunked(4)
                    .filter { it.size == 4 && it.all { value -> value > 0 } }
                    .map { DirectUsbFormat(it[0], it[1], it[2], it[3]) }
                    .toList()
            }.getOrDefault(emptyList())
            if (verifiedFormats.isEmpty()) {
                Log.i(tag, "SKIP reason=no-verified-native-usb-format-descriptors")
            }
            assumeTrue("SKIP reason=no-verified-native-usb-format-descriptors", verifiedFormats.isNotEmpty())
            // Exercise every distinct verified negotiated tuple; no supported descriptor
            // is selected away per rate, bit depth, subslot size, or channel count.
            val matrix = verifiedFormats
                .filter { it.sampleRate in selectedRates }
                .filter {
                    it.bits <= it.subslotBytes * 8 && it.channels >= 2 && it.channels % 2 == 0
                }
                .distinctBy { FormatKey(it.sampleRate, it.bits, it.subslotBytes, it.channels) }
                .sortedWith(
                    compareBy<DirectUsbFormat> { it.sampleRate }
                        .thenBy { it.bits }
                        .thenBy { it.subslotBytes }
                        .thenBy { it.channels }
                )
            if (matrix.isEmpty()) {
                Log.i(tag, "SKIP reason=no-supported-44100-or-48000-format")
            }
            assumeTrue("SKIP reason=no-supported-44100-or-48000-format", matrix.isNotEmpty())
            assertTrue("USB interface exposes no capture channels", DirectUsbAudioManager.getInputChannelCount() > 0)

            for (multiplier in selectedMultipliers) {
                AudioSettingsManager.setDirectUsbPeriodMultiplier(context, multiplier)
                for (format in matrix) {
                    for (buffer in selectedBuffers) {
                        val key = CaseKey(format, multiplier, buffer)
                        val bucket = results.getOrPut(key) { mutableListOf() }
                        for (cycle in 1..cycles) {
                            cases++
                            bucket += runCase(context, engine, format, buffer, multiplier, cycle, durationMs, warmupMs)
                        }
                    }
                }
            }

            var auditPassed = cases > 0
            for ((key, bucket) in results) {
                val passedCycles = bucket.count { it.passed }
                val stable = passedCycles == cycles
                val tested = results.keys.filter { it.sameFormatAndMultiplier(key) }.map { it.buffer }.sorted()
                val stableBuffers = tested.filter { candidate ->
                    results[CaseKey(key.format, key.multiplier, candidate)]?.count { it.passed } == cycles
                }
                val threshold = stableBuffers.firstOrNull()
                val monotonic = threshold != null && tested.filter { it >= threshold }.all { it in stableBuffers }
                // Failures below the first stable buffer are expected audit data.
                // Every tested buffer at or above that threshold must be stable.
                val keyAuditPassed = threshold != null && (key.buffer < threshold || (stable && monotonic))
                if (!keyAuditPassed) auditPassed = false
                Log.i(
                    tag,
                    "AUDIT_SUMMARY rate=${key.format.sampleRate} bits=${key.format.bits} " +
                        "bytes=${key.format.subslotBytes} channels=${key.format.channels} " +
                        "multiplier=${key.multiplier} buffer=${key.buffer} required_cycles=$cycles " +
                        "passed_cycles=$passedCycles min_stable_buffer=${threshold ?: 0} " +
                        "tested_buffers=${tested.joinToString(",")} stable_buffers=${stableBuffers.joinToString(",")} " +
                        "monotonic=${if (monotonic) 1 else 0} result=${if (keyAuditPassed) "PASS" else "FAIL"}"
                )
            }
            Log.i(
                tag,
                "AUDIT_SUMMARY overall=1 cases=$cases cycles=$cycles duration_ms=$durationMs " +
                    "result=${if (auditPassed) "PASS" else "FAIL"}"
            )
            assertTrue("Direct USB buffer audit failed; inspect AUDIT_SUMMARY/TELEMETRY", auditPassed)
        } finally {
            runCatching { DirectUsbAudioManager.disable(context) }
            AudioSettingsManager.setDirectUsbFormat(context, originalRate, originalBits, originalSubslot, originalChannels)
            AudioSettingsManager.setBufferSize(context, originalBuffer)
            AudioSettingsManager.setDirectUsbInputChannel(context, originalInputChannel)
            AudioSettingsManager.setDirectUsbOutputPair(context, originalOutputPair)
            AudioSettingsManager.setDirectUsbPeriodMultiplier(context, originalMultiplier)
            // Restore transport controls last. The exact frame cannot be restored
            // because no public API exposes a frame setter.
            originalTransport?.let {
                runCatching { engine.setTransportBpm(it.beatsPerMinute) }
                runCatching { engine.setTransportLooping(it.looping) }
                runCatching { engine.setTransportPlaying(it.playing) }
            }
        }
    }
    @Test
    fun directUsbCsvArgumentsSelectTargetedCases() {
        val args = Bundle().apply {
            putString("direct_usb_rates", "48000")
            putString("direct_usb_buffers", "512,16")
            putString("direct_usb_multipliers", "2")
        }

        assertArrayEquals(intArrayOf(48_000), argumentCsv(args, "direct_usb_rates", defaultRates))
        assertArrayEquals(
            intArrayOf(16, 512),
            argumentCsv(args, "direct_usb_buffers", defaultBuffers)
        )
        assertArrayEquals(intArrayOf(2), argumentCsv(args, "direct_usb_multipliers", defaultMultipliers))
        assertThrows(IllegalArgumentException::class.java) {
            argumentCsv(Bundle().apply { putString("direct_usb_buffers", "17") }, "direct_usb_buffers", defaultBuffers)
        }
    }


    private fun runCase(
        context: Context,
        engine: NativeEngine,
        format: DirectUsbFormat,
        buffer: Int,
        multiplier: Int,
        cycle: Int,
        durationMs: Long,
        warmupMs: Long,
    ): CaseResult {
        val requestedBpm = 120.0
        var reason: String? = null
        var temporaryTrackId = 0L
        var wav: File? = null
        var warmupStats: DirectUsbStats? = null
        var finalStats = runCatching { engine.getDirectUsbStats() }.getOrDefault(DirectUsbStats())
        var finalRaw = LongArray(0)
        var finalTransport: TransportInfo? = null
        try {
            AudioSettingsManager.setBufferSize(context, buffer)
            DirectUsbAudioManager.startSelected(context, format)
            val started = runBlocking { DirectUsbAudioManager.startConfigured(context) }
            if (started.isFailure) {
                reason = "start-failed detail=${started.exceptionOrNull()?.message ?: "unknown"}"
            }
            var runningStats: DirectUsbStats? = null
            if (reason == null) {
                val runningDeadline = SystemClock.elapsedRealtime() + 1_000L
                while (SystemClock.elapsedRealtime() < runningDeadline) {
                    val stats = engine.getDirectUsbStats()
                    if (stats.state == DirectUsbSessionState.Failed) {
                        reason = "session-failed code=${stats.failure}"
                        break
                    }
                    if (stats.state == DirectUsbSessionState.Running) {
                        runningStats = stats
                        break
                    }
                    SystemClock.sleep(10)
                }
                if (reason == null && runningStats == null) reason = "session-not-running-within-1s"
                if (reason == null) reason = runningStats?.let { validateConfiguration(it, format, buffer, multiplier) }
            }
            if (reason == null) {
                temporaryTrackId = engine.addTrack()
                if (temporaryTrackId <= 0L) reason = "temporary-track-create-failed"
            }
            if (reason == null) {
                wav = createStressWav(context.cacheDir, format.sampleRate)
                if (!engine.loadTrackWav(temporaryTrackId, wav.absolutePath, wav.name)) {
                    reason = "track-wav-load-failed"
                }
            }
            if (reason == null) {
                engine.setTransportLooping(true)
                if (!engine.setTransportBpm(requestedBpm)) {
                    reason = "transport-bpm-set-failed"
                } else if (!engine.restartTransport() || !engine.setTransportPlaying(true)) {
                    reason = "transport-start-failed"
                }
            }
            var transport: TransportInfo? = null
            if (reason == null) {
                val readyDeadline = SystemClock.elapsedRealtime() + 1_000L
                var ready = false
                while (SystemClock.elapsedRealtime() < readyDeadline) {
                    val candidate = engine.getTransportInfo()
                    transport = candidate
                    val candidateDurationFrames = ceil(candidate.durationSec * format.sampleRate.toDouble()).toLong()
                    ready = candidate.playing && candidate.looping &&
                        candidate.loadedTrackCount > 0 && candidate.durationSec > 0.0 &&
                        candidateDurationFrames > 0L &&
                        candidate.transportFrame < candidateDurationFrames &&
                        abs(candidate.beatsPerMinute - requestedBpm) <= 0.01
                    if (ready) break
                    SystemClock.sleep(10)
                }
                if (!ready) reason = "transport-state-not-applied"
            }

            if (reason == null && transport != null) {
                val start = SystemClock.elapsedRealtime()
                val deadline = start + durationMs
                val warmupDeadline = start + warmupMs
                var previousSequence = engine.getDirectUsbStats().sequence
                var previousSamplePosition = transport.samplePosition
                var previousTransportFrame = transport.transportFrame
                var samplePositionProgressed = false
                var transportFrameProgressed = false
                while (SystemClock.elapsedRealtime() < deadline && reason == null) {
                    val stats = engine.getDirectUsbStats()
                    val raw = engine.nativeGetDirectUsbStats()
                    reason = validateRunningStats(stats, raw, format, buffer, multiplier)
                    if (reason == null && stats.sequence < previousSequence) reason = "capture-sequence-regressed"
                    previousSequence = stats.sequence
                    val current = engine.getTransportInfo()
                    val durationFrames = ceil(current.durationSec * format.sampleRate.toDouble()).toLong().coerceAtLeast(1L)
                    val sampleDelta = current.samplePosition - previousSamplePosition
                    val expectedTransportFrame = (previousTransportFrame + sampleDelta) % durationFrames
                    val frameError = abs(current.transportFrame - expectedTransportFrame)
                    if (reason == null && current.samplePosition < previousSamplePosition) reason = "sample-position-regressed"
                    if (reason == null && current.transportFrame >= durationFrames) reason = "transport-frame-out-of-range"
                    if (reason == null && (!current.playing || !current.looping)) reason = "transport-state-changed"
                    if (reason == null && frameError > TRANSPORT_FRAME_ROUNDING_TOLERANCE) {
                        reason = "transport-frame-delta-incoherent-expected=$expectedTransportFrame-actual=${current.transportFrame}"
                    }
                    samplePositionProgressed = samplePositionProgressed || current.samplePosition > previousSamplePosition
                    transportFrameProgressed = transportFrameProgressed || current.transportFrame > previousTransportFrame
                    if (reason == null && abs(current.beatsPerMinute - requestedBpm) > 0.01) reason = "transport-bpm-incoherent"
                    previousSamplePosition = current.samplePosition
                    previousTransportFrame = current.transportFrame
                    transport = current
                    if (warmupStats == null && SystemClock.elapsedRealtime() >= warmupDeadline) {
                        warmupStats = stats
                        if (raw.getOrZero(EVENT_THREAD_URGENT_AUDIO) != 1L || raw.getOrZero(RENDER_THREAD_URGENT_AUDIO) != 1L) {
                            reason = "urgent-audio-thread-not-enabled"
                        }
                    }
                    SystemClock.sleep(10)
                }
                finalStats = engine.getDirectUsbStats()
                finalRaw = engine.nativeGetDirectUsbStats()
                finalTransport = engine.getTransportInfo()
                val baseline = warmupStats ?: finalStats
                val actualXrunGrowth = (finalStats.actualXruns - baseline.actualXruns).coerceAtLeast(0L)
                val deadlineMissGrowth = (finalStats.deadlineMisses - baseline.deadlineMisses).coerceAtLeast(0L)
                if (reason == null && !samplePositionProgressed) reason = "sample-position-did-not-advance"
                if (reason == null && !transportFrameProgressed) reason = "transport-frame-did-not-advance"
                if (reason == null && actualXrunGrowth > 0L) reason = "actual-xrun-growth-exceeded"
                if (reason == null && deadlineMissGrowth > 0L) reason = "deadline-miss-growth-exceeded"
                if (reason == null) reason = validateRunningStats(finalStats, finalRaw, format, buffer, multiplier)
            }
        } catch (t: Throwable) {
            reason = "exception-${t.message?.replace(Regex("[\\r\\n]"), " ") ?: t.javaClass.simpleName}"
        } finally {
            runCatching { engine.setTransportPlaying(false) }
            if (temporaryTrackId > 0L) {
                runCatching { engine.unloadTrackWav(temporaryTrackId) }
                runCatching { engine.removeTrack(temporaryTrackId) }
            }
            wav?.delete()
            runCatching { DirectUsbAudioManager.disable(context) }
        }
        val lifecycleStats = runCatching { engine.getDirectUsbStats() }.getOrDefault(finalStats)
        val lifecycleRaw = runCatching { engine.nativeGetDirectUsbStats() }.getOrDefault(finalRaw)
        val lifecycleOk = lifecycleStats.state == DirectUsbSessionState.Stopped &&
            lifecycleStats.failure == DirectUsbFailure.Ok
        if (!lifecycleOk && reason == null) reason = "lifecycle-after-stop-invalid-${lifecycleStats.state}-${lifecycleStats.failure}"
        val transport = finalTransport ?: runCatching { engine.getTransportInfo() }.getOrNull()
        Log.i(
            tag,
            telemetry(reason ?: "pass", cycle, format, buffer, multiplier, finalStats, finalRaw, lifecycleOk, transport, warmupStats)
        )
        Log.i(
            tag,
            telemetry(
                if (lifecycleOk) "lifecycle-after-stop" else "lifecycle-after-stop-failed",
                cycle,
                format,
                buffer,
                multiplier,
                lifecycleStats,
                lifecycleRaw,
                lifecycleOk,
                transport,
                warmupStats
            )
        )
        return CaseResult(reason == null, reason)
    }

    private fun validateConfiguration(stats: DirectUsbStats, format: DirectUsbFormat, buffer: Int, multiplier: Int): String? {
        if (stats.schemaVersion < 3L) return "unsupported-stats-schema-${stats.schemaVersion}"
        if (stats.periodMultiplier != multiplier.toLong()) return "period-multiplier-mismatch-${stats.periodMultiplier}"
        if (stats.effectiveQuantum != buffer.toLong()) return "effective-quantum-mismatch-${stats.effectiveQuantum}"
        val configuredTarget = minOf(1024L, buffer.toLong() * multiplier)
        if (stats.steadyTarget < configuredTarget) return "steady-target-below-configured-${stats.steadyTarget}"
        if (stats.startupPrime < stats.steadyTarget) return "startup-prime-below-steady-target-${stats.startupPrime}-${stats.steadyTarget}"
        if (stats.steadyTarget + stats.effectiveQuantum > ringFrameLimit(format)) return "steady-target-exceeds-ring-capacity-${stats.steadyTarget}"
        if (stats.startupPrime < configuredTarget) return "startup-prime-too-small-${stats.startupPrime}"
        if (stats.knownHostLatencyFrames <= 0L) return "host-latency-unavailable"
        if (stats.sampleRateHz != format.sampleRate.toLong()) return "sample-rate-mismatch-${stats.sampleRateHz}"
        val queueLatencyMs = stats.knownHostLatencyFrames * 1_000.0 / stats.sampleRateHz
        if (!queueLatencyMs.isFinite() || queueLatencyMs <= 0.0) return "invalid-host-queue-latency"
        return null
    }

    private fun validateRunningStats(stats: DirectUsbStats, rawStats: LongArray, format: DirectUsbFormat, buffer: Int, multiplier: Int): String? {
        if (stats.state == DirectUsbSessionState.Failed) return "session-failed code=${stats.failure}"
        if (stats.state != DirectUsbSessionState.Running) return "session-state-${stats.state}"
        validateConfiguration(stats, format, buffer, multiplier)?.let { return it }
        if (stats.queuedOut <= 0L) return "queued-output-empty"
        if (stats.deadlineBudgetNs <= 0L || stats.lastCycleNs <= 0L || stats.peakCycleNs < stats.lastCycleNs) return "invalid-cycle-timing"
        if (stats.captureTransferErrors != 0L || stats.playbackTransferErrors != 0L ||
            rawStats.getOrZero(CAPTURE_TRANSFER_ERRORS) != 0L || rawStats.getOrZero(PLAYBACK_TRANSFER_ERRORS) != 0L ||
            rawStats.getOrZero(LIFECYCLE_FAILURES) != 0L || rawStats.getOrZero(TRANSPORT_FAILED) != 0L) {
            return "transfer-lifecycle-transport-failure"
        }
        if (rawStats.getOrZero(CAPTURE_RING_FRAMES) > ringFrameLimit(format) || rawStats.getOrZero(PLAYBACK_RING_FRAMES) > ringFrameLimit(format) || rawStats.getOrZero(IMPLICIT_FIFO_DEPTH) > MAX_IMPLICIT_FIFO) return "usb-queue-depth-exceeded"
        return null
    }

    private fun telemetry(reason: String, cycle: Int, format: DirectUsbFormat, buffer: Int, multiplier: Int, stats: DirectUsbStats, rawStats: LongArray, lifecycleOk: Boolean, transport: TransportInfo?, warmup: DirectUsbStats?): String {
        val queueLatencyMs = if (stats.sampleRateHz > 0L) stats.knownHostLatencyFrames * 1_000.0 / stats.sampleRateHz else 0.0
        val actualXrunGrowth = (stats.actualXruns - (warmup?.actualXruns ?: stats.actualXruns)).coerceAtLeast(0L)
        val deadlineMissGrowth = (stats.deadlineMisses - (warmup?.deadlineMisses ?: stats.deadlineMisses)).coerceAtLeast(0L)
        return "TELEMETRY reason=$reason cycle=$cycle rate=${format.sampleRate} bits=${format.bits} bytes=${format.subslotBytes} channels=${format.channels} " +
            "buffer=$buffer multiplier=$multiplier schema=${stats.schemaVersion} state=${stats.state} failure=${stats.failure} period_multiplier=${stats.periodMultiplier} " +
            "effective_quantum=${stats.effectiveQuantum} steady_target_frames=${stats.steadyTarget} startup_prime_frames=${stats.startupPrime} queued_out_frames=${stats.queuedOut} " +
            "known_host_latency_frames=${stats.knownHostLatencyFrames} estimated_host_queue_latency_ms=$queueLatencyMs sequence=${stats.sequence} " +
            "capture_overruns=${rawStats.getOrZero(CAPTURE_OVERRUNS)} capture_underruns=${rawStats.getOrZero(CAPTURE_UNDERRUNS)} " +
            "capture_transfer_errors=${stats.captureTransferErrors} playback_transfer_errors=${stats.playbackTransferErrors} capture_wait_pressure=${stats.captureWaitPressure} " +
            "write_wait_pressure=${stats.writeWaitPressure} playback_xruns=${stats.playbackXruns} lifecycle_failures=${rawStats.getOrZero(LIFECYCLE_FAILURES)} " +
            "transport_failed=${rawStats.getOrZero(TRANSPORT_FAILED)} capture_ring_frames=${rawStats.getOrZero(CAPTURE_RING_FRAMES)} playback_ring_frames=${rawStats.getOrZero(PLAYBACK_RING_FRAMES)} " +
            "implicit_fifo_depth=${rawStats.getOrZero(IMPLICIT_FIFO_DEPTH)} last_dsp_ns=${stats.lastDspNs} peak_dsp_ns=${stats.peakDspNs} " +

            "last_cycle_ns=${stats.lastCycleNs} peak_cycle_ns=${stats.peakCycleNs} deadline_budget_ns=${stats.deadlineBudgetNs} deadline_misses=${stats.deadlineMisses} " +
            "actual_xruns=${stats.actualXruns} actual_xrun_growth=$actualXrunGrowth deadline_miss_growth=$deadlineMissGrowth " +
            "transport_playing=${transport?.playing == true} transport_looping=${transport?.looping == true} transport_bpm=${transport?.beatsPerMinute ?: 0.0} " +
            "sample_position=${transport?.samplePosition ?: 0L} transport_frame=${transport?.transportFrame ?: 0L} lifecycle_after_stop=${if (lifecycleOk) 1 else 0}"
    }

    private fun createStressWav(directory: File, sampleRate: Int): File {
        val frames = sampleRate * 2
        val dataBytes = frames * 2
        val bytes = ByteArray(44 + dataBytes)
        fun ascii(offset: Int, value: String) = value.toByteArray(Charsets.US_ASCII).copyInto(bytes, offset)
        fun le16(offset: Int, value: Int) { bytes[offset] = (value and 0xff).toByte(); bytes[offset + 1] = ((value ushr 8) and 0xff).toByte() }
        fun le32(offset: Int, value: Int) { le16(offset, value); le16(offset + 2, value ushr 16) }
        ascii(0, "RIFF"); le32(4, 36 + dataBytes); ascii(8, "WAVE"); ascii(12, "fmt "); le32(16, 16); le16(20, 1); le16(22, 1)
        le32(24, sampleRate); le32(28, sampleRate * 2); le16(32, 2); le16(34, 16); ascii(36, "data"); le32(40, dataBytes)
        for (frame in 0 until frames) le16(44 + frame * 2, (sin(2.0 * PI * 440.0 * frame / sampleRate) * 12_000.0).roundToInt())
        return File.createTempFile("direct-usb-stress-$sampleRate-", ".wav", directory).also { it.writeBytes(bytes) }
    }

    private fun argumentCsv(args: Bundle, key: String, allowed: IntArray): IntArray {
        val raw = args.getString(key)?.trim()
        if (raw.isNullOrEmpty()) return allowed.copyOf()

        val requested = raw.split(',').mapIndexed { index, token ->
            token.trim().toIntOrNull()
                ?: throw IllegalArgumentException("$key[$index] must be an integer: '$token'")
        }
        require(requested.isNotEmpty()) { "$key must contain at least one value" }
        require(requested.distinct().size == requested.size) { "$key must not contain duplicates" }
        val invalid = requested.filterNot { it in allowed }
        require(invalid.isEmpty()) {
            "$key contains unsupported values ${invalid.joinToString(",")}; allowed=${allowed.joinToString(",")}"
        }
        return allowed.filter { it in requested }.toIntArray()
    }

    private fun argumentInt(args: Bundle, primary: String, secondary: String, default: Int, min: Int, max: Int): Int =
        (args.getString(primary) ?: args.getString(secondary))?.toIntOrNull()?.coerceIn(min, max) ?: default

    private fun argumentLong(args: Bundle, primary: String, secondary: String, default: Long, min: Long, max: Long): Long =
        (args.getString(primary) ?: args.getString(secondary))?.toLongOrNull()?.coerceIn(min, max) ?: default

    private fun ringFrameLimit(format: DirectUsbFormat): Long = 65_536L / (format.channels.toLong() * format.subslotBytes.coerceAtLeast(1))
    private fun isUsbAudio(device: UsbDevice): Boolean = device.deviceClass == UsbConstants.USB_CLASS_AUDIO || (0 until device.interfaceCount).any { device.getInterface(it).interfaceClass == UsbConstants.USB_CLASS_AUDIO }
    private fun LongArray.getOrZero(index: Int): Long = getOrNull(index) ?: 0L

    private data class FormatKey(val sampleRate: Int, val bits: Int, val subslotBytes: Int, val channels: Int)
    private data class CaseKey(val format: DirectUsbFormat, val multiplier: Int, val buffer: Int) {
        fun sameFormatAndMultiplier(other: CaseKey): Boolean = format == other.format && multiplier == other.multiplier
    }
    private data class CaseResult(val passed: Boolean, val reason: String?)

    private companion object {
        const val MAX_IMPLICIT_FIFO = 256L
        const val TRANSPORT_FRAME_ROUNDING_TOLERANCE = 1L
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
