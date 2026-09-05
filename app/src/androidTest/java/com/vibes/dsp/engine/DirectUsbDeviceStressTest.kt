/*
 * Hardware-gated direct USB duplex stress/diagnostic instrumentation.
 *
 * This source set is never included in ordinary unit-test suites. The test
 * only proceeds when UsbManager reports a USB Audio device; probeFormats requests
 * permission and denied access fails the test rather than being skipped.
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
import org.junit.Assert.assertEquals
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
    private val allowedBuffers = AudioSettingsManager.BUFFER_SIZE_OPTIONS.map { it.first }.toIntArray()
    private val defaultMultipliers = intArrayOf(1, 2, 3)
    private val allowedMultipliers = (1..8).toList().toIntArray()

    @Test
    fun duplexRateBufferLifecycleStress() {
        val args = InstrumentationRegistry.getArguments()
        val selectedRates = argumentCsv(args, "direct_usb_rates", defaultRates, defaultRates)
        val selectedBuffers = argumentCsv(args, "direct_usb_buffers", defaultBuffers, allowedBuffers)
        val selectedMultipliers = argumentCsv(args, "direct_usb_multipliers", defaultMultipliers, allowedMultipliers)
        val requireLoopback = argumentBoolean(args, "direct_usb_require_loopback")
        // Step 1 of the diagnostics ladder. Off by default: recording is cheap
        // but the dump is verbose, and ordinary audit runs do not need it.
        val flightRecorder = argumentBoolean(args, "direct_usb_flight_recorder")
        // The validation loop allocates on every iteration - a stats object, a
        // 57-long array from JNI, transport info and the track list - so at the
        // default 10 ms it produces a steady stream of garbage in the process
        // that owns the render thread. A collection pause deschedules that
        // thread, which is a candidate for the clicks a listener reports at a
        // rate unrelated to any driver counter. Raising this trades validation
        // resolution for harness quiet.
        val pollIntervalMs = argumentLong(args, "direct_usb_poll_ms", "poll_ms", 10L, 1L, 1_000L)
        val discontinuityThreshold =
            argumentDouble(args, "direct_usb_discontinuity", 0.05).toFloat()
        // An interface that loops internally returns playback on the input pair
        // fed by the playback pair, not on input one. Both default to the first
        // pair and the first channel, which is the external-cable arrangement.
        // Submitted OUT runway, in transfers. This is the reserve that survives
        // a late completion, as distinct from PCM waiting in the ring, so a
        // sweep over it needs no rebuild. Zero keeps the automatic policy.
        // 0 waits for room, 1 paces by played frames. One build serves both.
        val admissionPolicy = argumentInt(args, "direct_usb_admission", "admission", 0, 0, 1)
        // Frames the producer may run ahead of the device under the credit
        // policy. Zero is strict credit, which holds no lead at all.
        val creditReserve = argumentInt(args, "direct_usb_credit_reserve", "credit_reserve", 0, 0, 1024)
        val transferCount = argumentInt(args, "direct_usb_transfers", "transfers", 0, 0, 8)
        val outputPair = argumentInt(args, "direct_usb_output_pair", "output_pair", 0, 0, 7)
        val inputChannel = argumentInt(args, "direct_usb_input_channel", "input_channel", 0, 0, 15)
        val cycles = argumentInt(args, "direct_usb_cycles", "cycles", 2, 1, 8)
        val durationMs = argumentLong(args, "direct_usb_duration_ms", "duration_ms", 5_000L, 5_000L, 600_000L)
        val warmupMs = minOf(1_000L, (durationMs / 3L).coerceAtLeast(250L))
        val context = InstrumentationRegistry.getInstrumentation().targetContext
        val usb = context.getSystemService(Context.USB_SERVICE) as UsbManager
        val audioDevices = usb.deviceList.values.filter(::isUsbAudio)
        if (audioDevices.isEmpty()) {
            Log.i(tag, "SKIP reason=no-usb-audio-device discovered=0")
        }
        assumeTrue("SKIP reason=no-usb-audio-device", audioDevices.isNotEmpty())

        val option = DirectUsbAudioManager.getAudioDevices(context)
            .firstOrNull { candidate -> audioDevices.any { it.deviceId == candidate.id } }
        assertTrue("USB audio device was not exposed by DirectUsbAudioManager", option != null)

        val engine = NativeEngine.getInstance()
        val originalDeviceId = AudioSettingsManager.getDirectUsbDeviceId(context)
        val originalVendorId = AudioSettingsManager.getDirectUsbVendorId(context)
        val originalProductId = AudioSettingsManager.getDirectUsbProductId(context)
        val originalDeviceName = AudioSettingsManager.getDirectUsbDeviceName(context)
        val originalCachedFormats = AudioSettingsManager.getDirectUsbCachedFormats(context)
        val originalRate = AudioSettingsManager.getDirectUsbRate(context)
        val originalBits = AudioSettingsManager.getDirectUsbBits(context)
        val originalSubslot = AudioSettingsManager.getDirectUsbSubslot(context)
        val originalChannels = AudioSettingsManager.getDirectUsbChannels(context)
        val originalOutputPair = AudioSettingsManager.getDirectUsbOutputPair(context)
        // Restored with the rest: a sweep that leaves the transfer count behind
        // silently biases every later run on the device.
        val originalTransferCount = AudioSettingsManager.getDirectUsbTransferCount(context)
        val originalBuffer = AudioSettingsManager.getBufferSize(context)
        val originalMultiplier = AudioSettingsManager.getDirectUsbPeriodMultiplier(context)
        var originalTransport: TransportInfo? = null
        val results = linkedMapOf<CaseKey, MutableList<CaseResult>>()
        var cases = 0
        try {
            AudioSettingsManager.setDirectUsbOutputPair(context, outputPair)
            if (transferCount > 0) {
                AudioSettingsManager.setDirectUsbTransferCount(context, transferCount)
            }
            // Separate line, not a TELEMETRY field: the analyzer's schema is
            // versioned and this is harness configuration, not a measurement.
            Log.i(tag, "ADMISSION_POLICY policy=$admissionPolicy reserve=$creditReserve")
            Log.i(tag, "LOOPBACK_CONFIG output_pair=$outputPair input_channel=$inputChannel " +
                "transfers=${AudioSettingsManager.getDirectUsbTransferCount(context)}")
            EngineInitHelper.preloadLilv(context.applicationInfo.nativeLibraryDir)
            assertTrue("Native engine initialization failed", EngineInitHelper.initEngine(context))
            originalTransport = runCatching { engine.getTransportInfo() }.getOrNull()
            val probe = runBlocking { DirectUsbAudioManager.probeFormats(context, option!!) }
            assertTrue("Direct USB probe failed: ${probe.exceptionOrNull()?.message}", probe.isSuccess)
            assertTrue(
                "USB permission was not granted after probing",
                audioDevices.any { it.deviceId == option!!.id && usb.hasPermission(it) }
            )
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
                // The requested pair and inspected channel must exist in the
                // negotiated format, or the run measures a channel nobody feeds.
                .filter { it.channels > outputPair * 2 + 1 && it.channels > inputChannel }
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
                            bucket += runCase(
                                context,
                                engine,
                                format,
                                buffer,
                                multiplier,
                                cycle,
                                durationMs,
                                warmupMs,
                                requireLoopback,
                                flightRecorder,
                                pollIntervalMs,
                                discontinuityThreshold,
                                inputChannel,
                                admissionPolicy,
                                creditReserve
                            )
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
            AudioSettingsManager.setDirectUsbDeviceId(context, originalDeviceId)
            AudioSettingsManager.setDirectUsbIdentity(
                context, originalVendorId, originalProductId, originalDeviceName
            )
            AudioSettingsManager.setDirectUsbCachedFormats(context, originalCachedFormats)
            AudioSettingsManager.setDirectUsbFormat(context, originalRate, originalBits, originalSubslot, originalChannels)
            AudioSettingsManager.setBufferSize(context, originalBuffer)
            AudioSettingsManager.setDirectUsbOutputPair(context, originalOutputPair)
            AudioSettingsManager.setDirectUsbTransferCount(context, originalTransferCount)
            AudioSettingsManager.setDirectUsbPeriodMultiplier(context, originalMultiplier)
            // Restore transport controls last. The exact frame cannot be restored
            // because no public API exposes a frame setter. Looping is a per-track
            // control and the temporary track is removed by each case.
            originalTransport?.let {
                runCatching { engine.setTransportBpm(it.beatsPerMinute) }
                runCatching { engine.setTransportPlaying(it.playing) }
            }
        }
    }
    @Test
    fun directUsbCsvArgumentsSelectTargetedCases() {
        val args = Bundle().apply {
            putString("direct_usb_rates", "48000")
            putString("direct_usb_buffers", "512,48,16")
            putString("direct_usb_multipliers", "8,4,5,6,7")
        }

        assertArrayEquals(intArrayOf(48_000), argumentCsv(args, "direct_usb_rates", defaultRates, defaultRates))
        assertArrayEquals(
            intArrayOf(16, 48, 512),
            argumentCsv(args, "direct_usb_buffers", defaultBuffers, allowedBuffers)
        )
        assertArrayEquals(
            intArrayOf(4, 5, 6, 7, 8),
            argumentCsv(args, "direct_usb_multipliers", defaultMultipliers, allowedMultipliers)
        )
        val invalidBuffer = assertThrows(IllegalArgumentException::class.java) {
            argumentCsv(Bundle().apply { putString("direct_usb_buffers", "17") }, "direct_usb_buffers", defaultBuffers, allowedBuffers)
        }
        assertTrue(invalidBuffer.message?.contains("allowed=") == true)
        val invalidMultiplier = assertThrows(IllegalArgumentException::class.java) {
            argumentCsv(Bundle().apply { putString("direct_usb_multipliers", "9") }, "direct_usb_multipliers", defaultMultipliers, allowedMultipliers)
        }
        assertTrue(invalidMultiplier.message?.contains("allowed=1,2,3,4,5,6,7,8") == true)

        assertTrue(argumentBoolean(Bundle().apply { putString("direct_usb_require_loopback", "true") }, "direct_usb_require_loopback"))
        assertTrue(!argumentBoolean(Bundle().apply { putString("direct_usb_require_loopback", "false") }, "direct_usb_require_loopback"))
        val invalidLoopback = assertThrows(IllegalArgumentException::class.java) {
            argumentBoolean(Bundle().apply { putString("direct_usb_require_loopback", "TRUE") }, "direct_usb_require_loopback")
        }
        assertTrue(invalidLoopback.message?.contains("must be true or false") == true)

    }
    @Test
    fun directUsbCycleArgumentsRespectBounds() {
        listOf(
            "minimum" to ("1" to 1),
            "below minimum clamps" to ("0" to 1),
            "maximum" to ("8" to 8),
            "above maximum clamps" to ("9" to 8),
        ).forEach { (label, inputAndExpected) ->
            val (input, expected) = inputAndExpected
            assertEquals(
                "direct_usb_cycles $label bound",
                expected,
                argumentInt(
                    Bundle().apply { putString("direct_usb_cycles", input) },
                    "direct_usb_cycles",
                    "cycles",
                    2,
                    1,
                    8,
                ),
            )
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
        requireLoopback: Boolean,
        flightRecorder: Boolean,
        pollIntervalMs: Long,
        discontinuityThreshold: Float,
        inputChannel: Int,
        admissionPolicy: Int,
        creditReserve: Int,
    ): CaseResult {
        val temporarySlot = 0
        val requestedBpm = 120.0
        var reason: String? = null
        var temporaryTrackId = 0L
        var wav: File? = null
        var warmupStats: DirectUsbStats? = null
        var warmupRaw: LongArray? = null
        var finalStats = runCatching { engine.getDirectUsbStats() }.getOrDefault(DirectUsbStats())
        var finalRaw = LongArray(0)
        var finalTransport: TransportInfo? = null
        var finalTrack: RackTrackInfo? = null
        var maxInputPeak = 0.0f
        var maxOutputPeak = 0.0f
        try {
            AudioSettingsManager.setBufferSize(context, buffer)
            // Enable before the session starts so the first admission and the
            // first completion are both recorded; enabling clears the history.
            runCatching {
                // Arm before enabling: the trigger keeps the run-up to the
                // first refusal, which is the event the recorder exists for.
                // Keeping only the newest records loses it - one cycle offers
                // about 143000 events into a 4096 slot buffer.
                if (flightRecorder) {
                    // Keep only the anomalies. A four minute run offers about
                    // 186000 events into a 4096 slot buffer, so recording the
                    // routine completions leaves room for a few seconds of
                    // history instead of a whole run's worth of faults.
                    engine.nativeSetDirectUsbFlightRecorderEventMask(
                        FlightRecord.ANOMALY_MASK
                    )
                    // A click is a discontinuity in the signal. At 440 Hz and
                    // 48 kHz consecutive samples differ by at most 0.058 of
                    // the tone's amplitude, so anything well above that is a
                    // break. A quarter of full scale proved far too coarse:
                    // it caught only the stop transient and missed the breaks
                    // a listener reported, because a jump of ten samples of
                    // phase still lands under it.
                    engine.nativeSetDirectUsbDiscontinuityThreshold(
                        discontinuityThreshold
                    )
                    engine.nativeSetDirectUsbTransferDiscontinuityThreshold(
                        discontinuityThreshold
                    )
                    // Relative to the loopback signal's own peak, so gain does
                    // not matter. A 440 Hz tone steps by 5.8% of its peak
                    // between samples; 30% is unambiguous.
                    engine.nativeSetDirectUsbCaptureDiscontinuityThreshold(0.30f)
                    // A steady tone must come back at a steady level. 8% is
                    // far above the RMS jitter of a clean loopback and far
                    // below the swing a drifting overlap produces.
                    engine.nativeSetDirectUsbCaptureModulationThreshold(0.08f)
                    // No freeze trigger: with the filter in place the whole run
                    // fits, and freezing on the first refusal would hide every
                    // deferral that followed it.
                    engine.nativeSetDirectUsbFlightRecorderFreezeTrigger(
                        FlightRecord.EVENT_UNKNOWN
                    )
                }
                engine.nativeSetDirectUsbFlightRecorderEnabled(flightRecorder)
            }.onFailure { error ->
                Log.i(
                    tag,
                    "FLIGHT_SUMMARY enable_failed=1 " +
                        "error=${error.javaClass.simpleName}-${error.message?.replace(Regex("[\r\n]"), " ")}"
                )
            }
            // Before the session starts, so the very first decoded block is
            // already inspected on the channel the loopback returns on.
            runCatching { engine.nativeSetDirectUsbCaptureInspectChannel(inputChannel) }
            runCatching { engine.nativeSetDirectUsbAdmissionPolicy(admissionPolicy) }
            runCatching { engine.nativeSetDirectUsbCreditReserve(creditReserve) }
            DirectUsbAudioManager.startSelected(context, format)
            val started = runBlocking {
                DirectUsbAudioManager.startConfigured(context)
            }
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
                if (temporaryTrackId <= 0L) {
                    reason = "temporary-track-create-failed"
                }
            }
            if (reason == null) {
                wav = createStressWav(context.cacheDir, format.sampleRate)
                if (!engine.loadTrackWav(temporaryTrackId, wav.absolutePath, wav.name)) {
                    reason = "track-wav-load-failed"
                } else if (!engine.setClipLooping(temporaryTrackId, temporarySlot, true)) {
                    reason = "track-looping-set-failed"
                }
            }
            if (reason == null) {
                if (!engine.setTransportBpm(requestedBpm)) {
                    reason = "transport-bpm-set-failed"
                } else if (!engine.restartTransport()) {
                    reason = "transport-start-failed"
                } else if (!engine.setClipTransportPlaying(
                        temporaryTrackId,
                        temporarySlot,
                        true,
                        TrackLaunchQuantization.Sixteenth
                    )
                ) {
                    reason = "track-play-set-failed"
                } else if (!engine.setTransportPlaying(true)) {
                    reason = "transport-start-failed"
                }
            }
            var transport: TransportInfo? = null
            var track: RackTrackInfo? = null
            if (reason == null) {
                val readyDeadline = SystemClock.elapsedRealtime() + 1_000L
                var ready = false
                while (SystemClock.elapsedRealtime() < readyDeadline) {
                    val candidate = engine.getTransportInfo()
                    val candidateTrack = engine.getTracks().firstOrNull { it.id == temporaryTrackId }
                    transport = candidate
                    track = candidateTrack
                    val candidateDurationFrames =
                        ceil((candidateTrack?.wavDurationSec ?: 0.0) * format.sampleRate.toDouble()).toLong()
                    ready = candidate.playing &&
                        candidateTrack?.wavLoaded == true &&
                        candidateTrack.playing &&
                        candidateTrack.looping &&
                        candidateTrack.wavDurationSec > 0.0 &&
                        candidateDurationFrames > 0L &&
                        candidateTrack.transportFrame < candidateDurationFrames &&
                        abs(candidate.beatsPerMinute - requestedBpm) <= 0.01
                    if (ready) break
                    SystemClock.sleep(10)
                }
                if (!ready) reason = "transport-state-not-applied"
            }

            if (reason == null && transport != null && track != null) {
                var pollCount = 0L
                var lastTransport: TransportInfo? = null
                var lastTrack: RackTrackInfo? = null
                val start = SystemClock.elapsedRealtime()
                val deadline = start + durationMs
                val warmupDeadline = start + warmupMs
                var previousSequence = engine.getDirectUsbStats().sequence
                var previousSamplePosition = transport.samplePosition
                var previousTrackFrame = track.transportFrame
                var samplePositionProgressed = false
                var trackFrameProgressed = false
                while (SystemClock.elapsedRealtime() < deadline && reason == null) {
                    val inputLevel = engine.getInputLevel()
                    if (inputLevel.isFinite()) maxInputPeak = maxOf(maxInputPeak, inputLevel)
                    val outputLevel = engine.getOutputLevel()
                    if (outputLevel.isFinite()) maxOutputPeak = maxOf(maxOutputPeak, outputLevel)
                    // One array per iteration, not two: getDirectUsbStats()
                    // decodes the same JNI array this call returns.
                    val raw = engine.nativeGetDirectUsbStats()
                    val stats = DirectUsbStats.fromRaw(raw)
                    reason = validateRunningStats(stats, raw, format, buffer, multiplier)
                    if (reason == null && stats.sequence < previousSequence) reason = "capture-sequence-regressed"
                    previousSequence = stats.sequence
                    // Transport and track state change at human speed, and
                    // getTracks() builds an array of objects. Sampling it every
                    // iteration was the harness's largest single allocation and
                    // it produced audible artefacts in a driver that is clean
                    // when the same configuration is used by hand.
                    val sampleTransport =
                        pollCount % TRANSPORT_POLL_DIVISOR == 0L || reason != null
                    ++pollCount
                    val current = if (sampleTransport) {
                        lastTransport = engine.getTransportInfo()
                        lastTransport
                    } else {
                        lastTransport
                    } ?: engine.getTransportInfo()
                    val currentTrack = if (sampleTransport) {
                        engine.getTracks().firstOrNull { it.id == temporaryTrackId }
                            .also { lastTrack = it }
                    } else {
                        lastTrack
                    }
                    finalTrack = currentTrack ?: finalTrack
                    val durationFrames =
                        ceil((currentTrack?.wavDurationSec ?: 0.0) * format.sampleRate.toDouble()).toLong().coerceAtLeast(1L)
                    val actualTrackFrame = currentTrack?.transportFrame ?: -1L
                    if (reason == null && currentTrack == null) reason = "track-state-disappeared"
                    if (reason == null && current.samplePosition < previousSamplePosition) reason = "sample-position-regressed"
                    if (reason == null && (actualTrackFrame < 0L || actualTrackFrame >= durationFrames)) {
                        reason = "track-frame-out-of-range"
                    }
                    if (reason == null && (!current.playing || currentTrack?.playing != true || currentTrack.looping != true)) {
                        reason = "transport-state-changed"
                    }
                    val trackFrameDelta =
                        if (actualTrackFrame >= 0L && actualTrackFrame != previousTrackFrame) {
                            if (actualTrackFrame > previousTrackFrame) {
                                actualTrackFrame - previousTrackFrame
                            } else {
                                durationFrames - previousTrackFrame + actualTrackFrame
                            }
                        } else {
                            0L
                        }
                    samplePositionProgressed = samplePositionProgressed || current.samplePosition > previousSamplePosition
                    trackFrameProgressed = trackFrameProgressed || trackFrameDelta > 0L
                    if (reason == null && abs(current.beatsPerMinute - requestedBpm) > 0.01) reason = "transport-bpm-incoherent"
                    previousSamplePosition = current.samplePosition
                    previousTrackFrame = actualTrackFrame.coerceAtLeast(0L)
                    if (warmupStats == null && SystemClock.elapsedRealtime() >= warmupDeadline) {
                        warmupStats = stats
                        warmupRaw = raw.copyOf()
                        if (raw.getOrZero(EVENT_THREAD_URGENT_AUDIO) != 1L || raw.getOrZero(RENDER_THREAD_URGENT_AUDIO) != 1L) {
                            reason = "urgent-audio-thread-not-enabled"
                        }
                        // Startup is its own gate: the pipeline fills and the
                        // first completions settle there, so a frame lost then
                        // is still a frame lost, but its extrema describe a
                        // different regime and must not be read as the steady
                        // state envelope.
                        // A refused admission is not a lost frame any more:
                        // the block is held and published on the next cycle,
                        // so this counter measures pressure while lostQuanta
                        // measures damage. Only damage fails the run.
                        if (reason == null && stats.lostQuanta > 0L) {
                            reason = "startup-lost-quantum-${stats.lostQuanta}"
                        }
                        if (reason == null && stats.minAdmissionMarginFrames < 0L) {
                            reason = "startup-admission-margin-${stats.minAdmissionMarginFrames}"
                        }
                        Log.i(tag, "STARTUP_ENVELOPE quantum_drops=${stats.playbackQuantumDrops} " +
                            "min_admission_margin=${stats.minAdmissionMarginFrames} " +
                            "max_completion_gap_ns=${stats.maxCompletionGapNs} " +
                            "max_missing_drains=${stats.maxMissingDrains} " +
                            "max_writes_between_drains=${stats.maxWritesBetweenDrains} " +
                            "first_loss_ring=${stats.firstLossRing} " +
                            "first_loss_queued=${stats.firstLossQueued} " +
                            "first_loss_had_room=${stats.firstLossHadRoom} " +
                            "first_loss_credit=${stats.firstLossCredit}")
                        runCatching { engine.nativeResetDirectUsbEnvelope() }
                    }
                    SystemClock.sleep(pollIntervalMs)
                }
                finalStats = engine.getDirectUsbStats()
                finalRaw = engine.nativeGetDirectUsbStats()
                finalTransport = engine.getTransportInfo()
                finalTrack = engine.getTracks().firstOrNull { it.id == temporaryTrackId } ?: finalTrack
                val baseline = warmupStats ?: finalStats
                val baselineRaw = warmupRaw ?: finalRaw
                val actualXrunGrowth = (finalStats.actualXruns - baseline.actualXruns).coerceAtLeast(0L)
                // The aggregate folds producer backpressure in with consumer
                // starvation, so a run that only ran the playback ring up to
                // its watermark reported the same verdict as one that starved
                // the DAC. Split them: quantum drops mean the render block was
                // refused because the ring was already at its target, while
                // the remainder is transport loss and starvation.
                val quantumDropGrowth =
                    (finalStats.playbackQuantumDrops - baseline.playbackQuantumDrops).coerceAtLeast(0L)
                val starvationGrowth = (actualXrunGrowth - quantumDropGrowth).coerceAtLeast(0L)
                val deadlineMissGrowth = (finalStats.deadlineMisses - baseline.deadlineMisses).coerceAtLeast(0L)
                val shortPacketGrowth = (finalStats.playbackShortPackets - baseline.playbackShortPackets).coerceAtLeast(0L)
                val shortFrameGrowth = (finalStats.playbackShortFrames - baseline.playbackShortFrames).coerceAtLeast(0L)
                val metadataFifoOverflowGrowth =
                    (finalRaw.getOrZero(METADATA_FIFO_OVERRUNS) - baselineRaw.getOrZero(METADATA_FIFO_OVERRUNS)).coerceAtLeast(0L)
                val zeroRunwayGrowth =
                    (finalRaw.getOrZero(ZERO_RUNWAY_EVENTS) - baselineRaw.getOrZero(ZERO_RUNWAY_EVENTS)).coerceAtLeast(0L)
                val captureDiscontinuityGrowth =
                    (finalStats.captureDiscontinuities - baseline.captureDiscontinuities).coerceAtLeast(0L)
                val signalDiscontinuityGrowth =
                    (finalStats.signalDiscontinuities - baseline.signalDiscontinuities).coerceAtLeast(0L)
                val transferDiscontinuityGrowth =
                    (finalStats.transferDiscontinuities - baseline.transferDiscontinuities).coerceAtLeast(0L)
                val implicitMetadataInvalidGrowth =
                    (finalStats.implicitMetadataInvalid - baseline.implicitMetadataInvalid).coerceAtLeast(0L)
                val lostQuantaGrowth =
                    (finalStats.lostQuanta - baseline.lostQuanta).coerceAtLeast(0L)
                val capturePartialReadGrowth =
                    (finalStats.capturePartialReads - baseline.capturePartialReads).coerceAtLeast(0L)
                // Deferral growth is deliberately NOT gated. It looked like the
                // audible fault on two runs, but across four it varies by three
                // orders of magnitude - 5 to 70855 - while a listener reports
                // seven to nine clicks regardless, and most of it tracks how
                // often this harness polls rather than anything the device
                // hears. It stays in telemetry as a pressure indicator.
                if (reason == null && !samplePositionProgressed) reason = "sample-position-did-not-advance"
                if (reason == null && !trackFrameProgressed) reason = "track-frame-did-not-advance"
                if (reason == null && quantumDropGrowth > 0L && lostQuantaGrowth == 0L) {
                    // Refusals without losses are pressure, not damage: the
                    // held block was delivered a cycle late. Recorded, not
                    // fatal, so the distinction stays visible in telemetry.
                    Log.i(tag, "ADMISSION_PRESSURE refusals=$quantumDropGrowth held=${finalStats.heldQuanta}")
                }
                if (reason == null && starvationGrowth > 0L) {
                    reason = "consumer-starvation-growth-exceeded-$starvationGrowth"
                }
                if (reason == null && deadlineMissGrowth > 0L) reason = "deadline-miss-growth-exceeded"
                if (reason == null && (shortPacketGrowth > 0L || shortFrameGrowth > 0L)) {
                    reason = "playback-short-packet-growth-exceeded-packets=$shortPacketGrowth-frames=$shortFrameGrowth"
                }
                if (reason == null && metadataFifoOverflowGrowth > 0L) {
                    reason = "metadata-fifo-overflow-growth-exceeded-$metadataFifoOverflowGrowth"
                }
                // Detector events now decide the verdict. A counter that only
                // reaches the flight log protects nothing: this run reported
                // 39 capture breaks and still passed on every other gate.
                if (reason == null && captureDiscontinuityGrowth > 0L) {
                    reason = "capture-discontinuity-growth-exceeded-$captureDiscontinuityGrowth"
                }
                if (reason == null && signalDiscontinuityGrowth > 0L) {
                    reason = "signal-discontinuity-growth-exceeded-$signalDiscontinuityGrowth"
                }
                if (reason == null && transferDiscontinuityGrowth > 0L) {
                    reason = "transfer-discontinuity-growth-exceeded-$transferDiscontinuityGrowth"
                }
                if (reason == null && lostQuantaGrowth > 0L) {
                    // A rendered block that never reached the ring is frame
                    // loss; it was counted only as a wait timeout before, so
                    // nothing failed on it.
                    reason = "lost-quantum-growth-exceeded-$lostQuantaGrowth"
                }
                if (reason == null && capturePartialReadGrowth > 0L) {
                    // A short capture read used to hand the graph a zero tail,
                    // which returned through the hardware loop as a capture
                    // break and was blamed on the environment.
                    reason = "capture-partial-read-growth-exceeded-$capturePartialReadGrowth"
                }
                if (reason == null && implicitMetadataInvalidGrowth > 0L) {
                    reason = "implicit-metadata-invalid-growth-exceeded-$implicitMetadataInvalidGrowth"
                }
                if (reason == null && finalStats.minAdmissionMarginFrames < 0L) {
                    // writable < quantum means the block was refused and its
                    // 64 frames are gone. Gating on counter growth alone let
                    // that pass whenever the loss landed outside the window.
                    reason = "admission-margin-negative-${finalStats.minAdmissionMarginFrames}"
                }
                if (reason == null && zeroRunwayGrowth > 0L) {
                    reason = "zero-runway-growth-exceeded-$zeroRunwayGrowth"
                }

                // Reported last: backpressure is a real defect but a different
                // one, and naming it separately keeps it from masquerading as
                // an audible dropout when ranking configurations.
                if (reason == null && quantumDropGrowth > 0L) {
                    reason = "producer-quantum-drop-growth-exceeded-$quantumDropGrowth"
                }
                if (reason == null) reason = validateRunningStats(finalStats, finalRaw, format, buffer, multiplier)
            }
            if (reason == null && requireLoopback && maxOutputPeak < LOOPBACK_OUTPUT_MIN_PEAK) {
                reason = "loopback-output-peak-below-threshold"
            }
            if (reason == null && requireLoopback && !finalStats.captureDetectorArmed) {
                // The loop can be present and still too quiet for the detector
                // to judge: below its arming level a silent run and a clean one
                // are the same run. That is a bench failure, not a pass.
                reason = "capture-detector-never-armed"
            }
            if (reason == null && requireLoopback && maxInputPeak < LOOPBACK_INPUT_MIN_PEAK) {
                reason = "loopback-input-peak-below-threshold"
            }
        } catch (t: Throwable) {
            reason = "exception-${t.message?.replace(Regex("[\\r\\n]"), " ") ?: t.javaClass.simpleName}"
        } finally {
            runCatching { engine.setTransportPlaying(false) }
            if (temporaryTrackId > 0L) {
                runCatching {
                    engine.setClipTransportPlaying(
                        temporaryTrackId,
                        temporarySlot,
                        false,
                        TrackLaunchQuantization.Sixteenth
                    )
                }
                runCatching { engine.unloadTrackWav(temporaryTrackId) }
                runCatching { engine.removeTrack(temporaryTrackId) }
            }
            wav?.delete()
            runCatching { DirectUsbAudioManager.disable(context) }
        }
        // Dump before anything else touches the engine: the records describe the
        // run that just ended, and the recorder is cleared on the next enable.
        if (flightRecorder) dumpFlightRecorder(engine, format, buffer, multiplier, cycle)
        val lifecycleStats = runCatching { engine.getDirectUsbStats() }.getOrDefault(finalStats)
        val lifecycleRaw = runCatching { engine.nativeGetDirectUsbStats() }.getOrDefault(finalRaw)
        val lifecycleOk = lifecycleStats.state == DirectUsbSessionState.Stopped &&
            lifecycleStats.failure == DirectUsbFailure.Ok
        if (!lifecycleOk && reason == null) reason = "lifecycle-after-stop-invalid-${lifecycleStats.state}-${lifecycleStats.failure}"
        val transport = finalTransport ?: runCatching { engine.getTransportInfo() }.getOrNull()
        Log.i(
            tag,
            telemetry(
                reason ?: "pass",
                cycle,
                format,
                buffer,
                multiplier,
                finalStats,
                finalRaw,
                lifecycleOk,
                transport,
                finalTrack,
                warmupStats,
                warmupRaw,
                requireLoopback,
                maxInputPeak,
                maxOutputPeak
            )
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
                finalTrack,
                warmupStats,
                warmupRaw,
                requireLoopback,
                maxInputPeak,
                maxOutputPeak
            )
        )
        return CaseResult(reason == null, reason)
    }

    private fun validateConfiguration(stats: DirectUsbStats, format: DirectUsbFormat, buffer: Int, multiplier: Int): String? {
        if (stats.schemaVersion != TELEMETRY_SCHEMA_VERSION) return "unsupported-stats-schema-${stats.schemaVersion}"
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
        if (rawStats.size < RAW_STAT_COUNT) return "unsupported-raw-stats-count-${rawStats.size}"
        if (stats.deadlineBudgetNs <= 0L || stats.lastCycleNs <= 0L || stats.peakCycleNs < stats.lastCycleNs) return "invalid-cycle-timing"
        if (stats.captureTransferErrors != 0L || stats.playbackTransferErrors != 0L ||
            rawStats.getOrZero(CAPTURE_TRANSFER_ERRORS) != 0L || rawStats.getOrZero(PLAYBACK_TRANSFER_ERRORS) != 0L ||
            rawStats.getOrZero(LIFECYCLE_FAILURES) != 0L || rawStats.getOrZero(TRANSPORT_FAILED) != 0L) {
            return "transfer-lifecycle-transport-failure"
        }
        if (rawStats.getOrZero(CAPTURE_RING_FRAMES) > ringFrameLimit(format) ||
            rawStats.getOrZero(PLAYBACK_RING_FRAMES) > ringFrameLimit(format) ||
            rawStats.getOrZero(IMPLICIT_FIFO_DEPTH) > MAX_IMPLICIT_FIFO) {
            return "usb-queue-depth-exceeded"
        }
        if (rawStats.getOrZero(PENDING_DEPTH) < 0L ||
            rawStats.getOrZero(PENDING_HIGH_WATER) < rawStats.getOrZero(PENDING_DEPTH) ||
            rawStats.getOrZero(MAX_PENDING_AGE_NS) < 0L) {
            return "invalid-pending-telemetry"
        }
        return null
    }

    /**
     * Dumps the flight recorder as one FLIGHT line per event.
     *
     * This exists because aggregate counters could not settle the questions
     * the campaigns kept raising: the same configuration reported twelve
     * producer quantum drops in one run and four in the next. A refusal is
     * only interpretable next to the accepted blocks around it, so every
     * admission is emitted, not just the failures.
     */
    private fun dumpFlightRecorder(
        engine: NativeEngine,
        format: DirectUsbFormat,
        buffer: Int,
        multiplier: Int,
        cycle: Int,
    ) {
        // Report the failure rather than swallowing it. The first run of this
        // dump produced no output at all because the native library on the
        // device was stale and the JNI method was missing; a silent return made
        // that indistinguishable from "the recorder had nothing to say".
        val snapshotResult = runCatching {
            FlightRecorderSnapshot.decode(
                engine.nativeGetDirectUsbFlightRecorderSnapshot(MAX_FLIGHT_RECORDS)
            )
        }
        val snapshot = snapshotResult.getOrElse { error ->
            Log.i(
                tag,
                "FLIGHT_SUMMARY rate=${format.sampleRate} buffer=$buffer " +
                    "multiplier=$multiplier cycle=$cycle unavailable=1 " +
                    "error=${error.javaClass.simpleName}-${error.message?.replace(Regex("[\r\n]"), " ")}"
            )
            return
        }
        val prefix = "rate=${format.sampleRate} buffer=$buffer multiplier=$multiplier cycle=$cycle"
        // A frozen buffer means the trigger fired and the tail is the run-up to
        // it; an unfrozen one means no refusal occurred during this cycle.
        val frozen = runCatching {
            engine.nativeIsDirectUsbFlightRecorderFrozen()
        }.getOrDefault(false)
        Log.i(
            tag,
            "FLIGHT_SUMMARY $prefix recorded=${snapshot.recorded} " +
                "dropped=${snapshot.dropped} emitted=${snapshot.records.size} " +
                "frozen=${if (frozen) 1 else 0}"
        )
        for (record in snapshot.records) {
            Log.i(
                tag,
                "FLIGHT $prefix seq=${record.sequence} t_ns=${record.timestampNs} " +
                    "event=${FlightRecord.eventName(record.event)} a=${record.a} b=${record.b} " +
                    "ring_frames=${record.ringFrames} queued_frames=${record.queuedFrames}"
            )
        }
    }

    private fun telemetry(
        reason: String,
        cycle: Int,
        format: DirectUsbFormat,
        buffer: Int,
        multiplier: Int,
        stats: DirectUsbStats,
        rawStats: LongArray,
        lifecycleOk: Boolean,
        transport: TransportInfo?,
        track: RackTrackInfo?,
        warmup: DirectUsbStats?,
        warmupRaw: LongArray?,
        requireLoopback: Boolean,
        inputPeak: Float,
        outputPeak: Float,
    ): String {
        val queueLatencyMs =
            if (stats.sampleRateHz > 0L) stats.knownHostLatencyFrames * 1_000.0 / stats.sampleRateHz else 0.0
        val actualXrunGrowth =
            (stats.actualXruns - (warmup?.actualXruns ?: stats.actualXruns)).coerceAtLeast(0L)
        val quantumDropGrowth =
            (stats.playbackQuantumDrops -
                (warmup?.playbackQuantumDrops ?: stats.playbackQuantumDrops)).coerceAtLeast(0L)
        val starvationGrowth = (actualXrunGrowth - quantumDropGrowth).coerceAtLeast(0L)
        val deadlineMissGrowth =
            (stats.deadlineMisses - (warmup?.deadlineMisses ?: stats.deadlineMisses)).coerceAtLeast(0L)
        val shortPacketGrowth =
            (stats.playbackShortPackets - (warmup?.playbackShortPackets ?: stats.playbackShortPackets))
                .coerceAtLeast(0L)
        val shortFrameGrowth =
            (stats.playbackShortFrames - (warmup?.playbackShortFrames ?: stats.playbackShortFrames))
                .coerceAtLeast(0L)
        val rawPlaybackXrunGrowth =
            (rawStats.getOrZero(RAW_PLAYBACK_XRUNS) -
                (warmupRaw?.getOrZero(RAW_PLAYBACK_XRUNS) ?: rawStats.getOrZero(RAW_PLAYBACK_XRUNS)))
                .coerceAtLeast(0L)
        val deferredTransfersGrowth =
            (rawStats.getOrZero(DEFERRED_TRANSFERS) -
                (warmupRaw?.getOrZero(DEFERRED_TRANSFERS) ?: rawStats.getOrZero(DEFERRED_TRANSFERS)))
                .coerceAtLeast(0L)
        val metadataFifoOverflowGrowth =
            (rawStats.getOrZero(METADATA_FIFO_OVERRUNS) -
                (warmupRaw?.getOrZero(METADATA_FIFO_OVERRUNS) ?: rawStats.getOrZero(METADATA_FIFO_OVERRUNS)))
                .coerceAtLeast(0L)
        val zeroRunwayGrowth =
            (rawStats.getOrZero(ZERO_RUNWAY_EVENTS) -
                (warmupRaw?.getOrZero(ZERO_RUNWAY_EVENTS) ?: rawStats.getOrZero(ZERO_RUNWAY_EVENTS)))
                .coerceAtLeast(0L)
        return "TELEMETRY reason=$reason cycle=$cycle rate=${format.sampleRate} bits=${format.bits} bytes=${format.subslotBytes} channels=${format.channels} " +
            "loopback_required=${if (requireLoopback) 1 else 0} input_peak=$inputPeak output_peak=$outputPeak " +
            "buffer=$buffer multiplier=$multiplier schema=${stats.schemaVersion} state=${stats.state} failure=${stats.failure} period_multiplier=${stats.periodMultiplier} " +
            "effective_quantum=${stats.effectiveQuantum} steady_target_frames=${stats.steadyTarget} startup_prime_frames=${stats.startupPrime} queued_out_frames=${stats.queuedOut} " +
            "known_host_latency_frames=${stats.knownHostLatencyFrames} estimated_host_queue_latency_ms=$queueLatencyMs sequence=${stats.sequence} " +
            "capture_overruns=${rawStats.getOrZero(CAPTURE_OVERRUNS)} capture_underruns=${rawStats.getOrZero(CAPTURE_UNDERRUNS)} " +
            "capture_transfer_errors=${stats.captureTransferErrors} playback_transfer_errors=${stats.playbackTransferErrors} " +
            "capture_packet_drops=${stats.capturePacketDrops} capture_wait_pressure=${stats.captureWaitPressure} " +
            "write_wait_pressure=${stats.writeWaitPressure} playback_xruns=${rawStats.getOrZero(RAW_PLAYBACK_XRUNS)} " +
            "playback_quantum_drops=${stats.playbackQuantumDrops} aggregate_xruns=${stats.actualXruns} " +
            "starvation_growth=$starvationGrowth quantum_drop_growth=$quantumDropGrowth " +
            "playback_backpressure=${stats.playbackBackpressure} playback_short_packets=${stats.playbackShortPackets} " +
            "playback_short_frames=${stats.playbackShortFrames} playback_short_packets_growth=$shortPacketGrowth " +
            "playback_short_frames_growth=$shortFrameGrowth performance_hint_active=${if (stats.performanceHintActive) 1 else 0} " +
            "lifecycle_failures=${rawStats.getOrZero(LIFECYCLE_FAILURES)} transport_failed=${rawStats.getOrZero(TRANSPORT_FAILED)} " +
            "capture_ring_frames=${rawStats.getOrZero(CAPTURE_RING_FRAMES)} playback_ring_frames=${rawStats.getOrZero(PLAYBACK_RING_FRAMES)} " +
            "implicit_fifo_depth=${rawStats.getOrZero(IMPLICIT_FIFO_DEPTH)} deferred_transfers=${rawStats.getOrZero(DEFERRED_TRANSFERS)} " +
            "deferred_transfers_growth=$deferredTransfersGrowth metadata_fifo_overruns=${rawStats.getOrZero(METADATA_FIFO_OVERRUNS)} " +
            "metadata_fifo_overruns_growth=$metadataFifoOverflowGrowth pending_depth=${rawStats.getOrZero(PENDING_DEPTH)} " +
            "pending_high_water=${rawStats.getOrZero(PENDING_HIGH_WATER)} max_pending_age_ns=${rawStats.getOrZero(MAX_PENDING_AGE_NS)} " +
            "zero_runway_events=${rawStats.getOrZero(ZERO_RUNWAY_EVENTS)} zero_runway_events_growth=$zeroRunwayGrowth " +
            "last_dsp_ns=${stats.lastDspNs} peak_dsp_ns=${stats.peakDspNs} " +
            "last_cycle_ns=${stats.lastCycleNs} peak_cycle_ns=${stats.peakCycleNs} deadline_budget_ns=${stats.deadlineBudgetNs} " +
            "deadline_misses=${stats.deadlineMisses} scheduler_deadline_misses=${stats.schedulerDeadlineMisses} " +
            "max_scheduler_lateness_ns=${stats.maxSchedulerLatenessNs} capture_target_frames=${stats.captureTargetFrames} " +
            "capture_headroom_frames=${stats.captureHeadroomFrames} capture_deadline_slack_frames=${stats.captureDeadlineSlackFrames} " +
            "deferred_no_metadata=${stats.deferredNoMetadata} deferred_no_pcm=${stats.deferredNoPcm} " +
            "queued_out_low_water=${stats.queuedOutLowWaterFrames} " +
            "capture_discontinuities=${stats.captureDiscontinuities} " +
            "signal_discontinuities=${stats.signalDiscontinuities} " +
            "transfer_discontinuities=${stats.transferDiscontinuities} " +
            "capture_modulations=${stats.captureModulations} " +
            "capture_detector_armed=${if (stats.captureDetectorArmed) 1 else 0} " +
            "implicit_metadata_invalid=${stats.implicitMetadataInvalid} " +
            "ring_low_water=${stats.ringLowWaterFrames} ring_high_water=${stats.ringHighWaterFrames} " +
            "drain_chunk_frames=${stats.drainChunkFrames} " +
            "ring_p05=${stats.ringOccupancyP05} ring_p50=${stats.ringOccupancyP50} " +
            "ring_p95=${stats.ringOccupancyP95} ring_samples=${stats.ringOccupancySamples} " +
            "max_completion_gap_ns=${stats.maxCompletionGapNs} max_missing_drains=${stats.maxMissingDrains} " +
            "drain_frames_min=${stats.drainFramesMin} drain_frames_max=${stats.drainFramesMax} " +
            "max_writes_between_drains=${stats.maxWritesBetweenDrains} " +
            "min_admission_margin=${stats.minAdmissionMarginFrames} " +
            "capture_partial_reads=${stats.capturePartialReads} " +
            "lost_quanta=${stats.lostQuanta} " +
            "held_quanta=${stats.heldQuanta} " +
            "live_queue_frames=${stats.liveQueueFrames} " +
            "raw_written_frames=${rawStats.getOrZero(RAW_WRITTEN_FRAMES)} raw_played_frames=${rawStats.getOrZero(RAW_PLAYED_FRAMES)} " +
            "raw_playback_xruns=${rawStats.getOrZero(RAW_PLAYBACK_XRUNS)} raw_playback_xrun_growth=$rawPlaybackXrunGrowth " +
            "actual_xruns=${stats.actualXruns} actual_xrun_growth=$actualXrunGrowth deadline_miss_growth=$deadlineMissGrowth " +
            "transport_playing=${transport?.playing == true} transport_bpm=${transport?.beatsPerMinute ?: 0.0} " +
            "sample_position=${transport?.samplePosition ?: 0L} track_playing=${track?.playing == true} " +
            "track_looping=${track?.looping == true} track_position=${track?.positionSec ?: 0.0} " +
            "track_frame=${track?.transportFrame ?: 0L} lifecycle_after_stop=${if (lifecycleOk) 1 else 0}"
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

    private fun argumentCsv(args: Bundle, key: String, defaults: IntArray, allowed: IntArray): IntArray {
        val raw = args.getString(key)?.trim()
        if (raw.isNullOrEmpty()) return defaults.copyOf()

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
    private fun argumentDouble(args: Bundle, key: String, default: Double): Double {
        val raw = args.getString(key)?.trim() ?: return default
        return raw.toDoubleOrNull()?.takeIf { it > 0.0 && it.isFinite() }
            ?: throw IllegalArgumentException("$key must be a positive number: '$raw'")
    }

    private fun argumentBoolean(args: Bundle, key: String, default: Boolean = false): Boolean {
        val raw = args.getString(key)?.trim() ?: return default
        return when (raw) {
            "true" -> true
            "false" -> false
            else -> throw IllegalArgumentException("$key must be true or false: '$raw'")
        }
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
        const val TELEMETRY_SCHEMA_VERSION = 17L
        const val RAW_STAT_COUNT = 55
        const val MAX_IMPLICIT_FIFO = 256L
        // One 30 s cycle at a 64-frame quantum offers about 22500 quanta, so a
        // full history does not fit a log dump. The recorder keeps the newest
        // records, which is the tail leading up to whatever went wrong.
        // Transport and track state are sampled at this fraction of the stats
        // rate. Driver statistics stay at full resolution; the object-building
        // accessors do not.
        const val TRANSPORT_POLL_DIVISOR = 20L
        const val MAX_FLIGHT_RECORDS = 4096
        const val LOOPBACK_OUTPUT_MIN_PEAK = 0.05f
        const val LOOPBACK_INPUT_MIN_PEAK = 0.005f
        const val CAPTURE_OVERRUNS = 1
        const val CAPTURE_UNDERRUNS = 2
        const val IMPLICIT_FIFO_DEPTH = 3
        const val DEFERRED_TRANSFERS = 4
        const val CAPTURE_TRANSFER_ERRORS = 5
        const val PLAYBACK_TRANSFER_ERRORS = 6
        const val PLAYBACK_RING_FRAMES = 7
        const val CAPTURE_RING_FRAMES = 8
        const val LIFECYCLE_FAILURES = 9
        const val TRANSPORT_FAILED = 10
        const val EVENT_THREAD_URGENT_AUDIO = 11
        const val RENDER_THREAD_URGENT_AUDIO = 12
        const val RAW_WRITTEN_FRAMES = 13
        const val RAW_PLAYED_FRAMES = 14
        const val RAW_PLAYBACK_XRUNS = 15
        const val METADATA_FIFO_OVERRUNS = 41
        const val PENDING_DEPTH = 42
        const val PENDING_HIGH_WATER = 43
        const val ZERO_RUNWAY_EVENTS = 44
        const val MAX_PENDING_AGE_NS = 45
    }
}
