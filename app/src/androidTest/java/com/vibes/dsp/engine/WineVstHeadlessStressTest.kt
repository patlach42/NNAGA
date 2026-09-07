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
import com.vibes.dsp.StartupPrerequisite
import kotlinx.coroutines.runBlocking
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Assume.assumeTrue
import org.junit.Test
import org.junit.runner.RunWith

/**
 * Headless, hardware-gated regression coverage for the Wine VST bridge.
 *
 * The startup/warmup interval is deliberately excluded from the verdict.  It
 * is needed to fill the direct-USB pipeline and the guest's shared-memory
 * rings, but a transient startup miss must not make a steady-state quantum
 * fail (or pass).
 */
@RunWith(AndroidJUnit4::class)
class WineVstHeadlessStressTest {
    @Test(timeout = MAX_TEST_TIMEOUT_MS)
    fun wineVstDirectUsbQuantumStress() {
        val args = InstrumentationRegistry.getArguments()
        val vstName = args.getString("vst_name")?.trim()
            ?.takeIf { it.isNotEmpty() } ?: DEFAULT_VST_NAME
        val quanta = parseQuanta(args)
        val warmupMs = argumentLong(args, "vst_warmup_ms", "warmup_ms", DEFAULT_WARMUP_MS, 0L)
        val steadyMs = argumentLong(args, "vst_steady_ms", "steady_ms", DEFAULT_STEADY_MS, 1L)
        val requireVst2 = parseBooleanArgument(args, "vst_require_vst2")
        val context = InstrumentationRegistry.getInstrumentation().targetContext
        val usbManager = context.getSystemService(Context.USB_SERVICE) as UsbManager
        val startupReady = runBlocking {
            val prerequisite = context.applicationContext as? StartupPrerequisite
                ?: throw AssertionError("Application does not expose StartupPrerequisite")
            prerequisite.awaitStartupPrerequisite()
        }
        assertTrue("Wine runtime staging failed", startupReady)

        val usbDevices = usbManager.deviceList.values.filter(::isUsbAudio)
        if (usbDevices.isEmpty()) {
            Log.i(TAG, "SKIP reason=no-usb-audio-device")
        }
        assumeTrue("SKIP reason=no-usb-audio-device", usbDevices.isNotEmpty())

        val usbOptions = DirectUsbAudioManager.getAudioDevices(context)
        val persistedDeviceId = AudioSettingsManager.getDirectUsbDeviceId(context)
        val persistedVendorId = AudioSettingsManager.getDirectUsbVendorId(context)
        val persistedProductId = AudioSettingsManager.getDirectUsbProductId(context)
        val selectedOption = usbOptions.firstOrNull { it.id == persistedDeviceId }
            ?: usbOptions.firstOrNull {
                it.vendorId == persistedVendorId && it.productId == persistedProductId
            }
        if (selectedOption == null) {
            Log.i(TAG, "SKIP reason=no-persisted-usb-audio-device")
        }
        assumeTrue("SKIP reason=no-persisted-usb-audio-device", selectedOption != null)

        val selectedUsbDevice = usbDevices.firstOrNull { it.deviceId == selectedOption!!.id }
        if (selectedUsbDevice == null) {
            Log.i(TAG, "SKIP reason=persisted-usb-device-not-present")
        }
        assumeTrue("SKIP reason=persisted-usb-device-not-present", selectedUsbDevice != null)
        if (!usbManager.hasPermission(selectedUsbDevice!!)) {
            Log.i(TAG, "SKIP reason=usb-permission-not-granted device=${selectedUsbDevice.deviceId}")
        }
        assumeTrue(
            "SKIP reason=usb-permission-not-granted",
            usbManager.hasPermission(selectedUsbDevice),
        )

        EngineInitHelper.preloadLilv(context.applicationInfo.nativeLibraryDir)
        assertTrue("Native engine initialization failed", EngineInitHelper.initEngine(context))
        val engine = NativeEngine.getInstance()
        val availableVsts = engine.getAvailablePlugins()
            .filter {
                it.format.equals("VST2", ignoreCase = true) ||
                    it.format.equals("VST3", ignoreCase = true)
            }
            .sortedWith(compareBy<PluginInfo> { it.fullId }.thenBy { it.originPath })
        val availableVst3 = availableVsts.filter { it.format.equals("VST3", ignoreCase = true) }
        val availableVst2 = availableVsts.filter { it.format.equals("VST2", ignoreCase = true) }
        val vst3 = availableVst3.firstOrNull { info ->
            info.name == vstName || info.id == vstName || info.fullId == vstName ||
                info.originPath == vstName
        }
        if (vst3 == null && vstName == DEFAULT_VST_NAME) {
            Log.i(TAG, "SKIP reason=missing-valhalla-vst3 requested=$vstName")
        }
        assumeTrue("SKIP reason=missing-vst3 requested=$vstName", vst3 != null)

        val vst2Override = args.getString("vst_vst2_name")?.trim()
            ?.takeIf { it.isNotEmpty() }
        val vst2 = if (vst2Override == null) {
            availableVst2.firstOrNull()
        } else {
            availableVst2.firstOrNull { info ->
                info.name == vst2Override || info.id == vst2Override ||
                    info.fullId == vst2Override || info.originPath == vst2Override
            }
        }
        if (vst2 == null && requireVst2) {
            throw AssertionError(
                "vst_require_vst2=true but no matching VST2 entry was discovered " +
                    "for override=${vst2Override ?: "<first>"}",
            )
        }
        if (vst2 == null) {
            val reason = if (availableVst2.isEmpty()) {
                "no-vst2-entry"
            } else {
                "vst2-override-not-found"
            }
            Log.i(
                TAG,
                "SKIP reason=$reason override=${vst2Override ?: "<first-discovered>"}",
            )
        }
        val vsts = listOfNotNull(vst3, vst2)

        val savedRackState = engine.exportRackState()
        val savedAudio = AudioSettingsSnapshot.capture(context)
        val priorEngineRunning = engine.isEngineRunning()
        try {
            // Stop any user session before changing the persisted backend and
            // quantum. The finalizer below restores and, when necessary,
            // restarts that exact session.
            DirectUsbAudioManager.disable(context)
            engine.stopEngine()
            AudioSettingsManager.setAudioBackend(context, AudioBackend.DirectUsb)

            vsts.forEach { vst ->
                quanta.forEach { quantum ->
                    runSteadyCase(
                        context = context,
                        engine = engine,
                        vst = vst,
                        quantum = quantum,
                        instanceCount = 1,
                        warmupMs = warmupMs,
                        steadyMs = steadyMs,
                    )
                    if (quantum == 16 || quantum == 64) {
                        runSteadyCase(
                            context = context,
                            engine = engine,
                            vst = vst,
                            quantum = quantum,
                            instanceCount = 2,
                            warmupMs = warmupMs,
                            steadyMs = steadyMs,
                        )
                    }
                }
            }
        } finally {
            var cleanupFailure: Throwable? = null
            fun cleanupStep(step: String, block: () -> Unit) {
                runCatching(block).onFailure { error ->
                    if (cleanupFailure == null) {
                        cleanupFailure = AssertionError("$step failed", error)
                    }
                }
            }

            // Preserve the user's session/data: stop, restore settings,
            // import the rack, then restart only if it was running.
            cleanupStep("Stopping Direct USB session") {
                DirectUsbAudioManager.disable(context)
                engine.stopEngine()
            }
            cleanupStep("Restoring audio settings") { savedAudio.restore(context) }
            cleanupStep("Restoring rack state") {
                val diagnostic = engine.importRackState(savedRackState, true)
                if (diagnostic != null) {
                    throw AssertionError("Rack state restoration failed: $diagnostic")
                }
            }
            if (priorEngineRunning) {
                cleanupStep("Restarting prior audio session") {
                    val restarted = runBlocking { DirectUsbAudioManager.startConfigured(context) }
                    if (restarted.isFailure) {
                        throw AssertionError(
                            "Prior audio session restart failed: " +
                                (restarted.exceptionOrNull()?.message ?: "unknown"),
                        )
                    }
                }
            }
            cleanupFailure?.let { throw it }
        }
    }

    private fun runSteadyCase(
        context: Context,
        engine: NativeEngine,
        vst: PluginInfo,
        quantum: Int,
        instanceCount: Int,
        warmupMs: Long,
        steadyMs: Long,
    ) {
        val pluginIndices = mutableListOf<Int>()
        try {
            AudioSettingsManager.setBufferSize(context, quantum)
            clearRack(engine)
            repeat(instanceCount) {
                val pluginIndex = engine.addPluginToRack(MASTER_PATH_ID, vst.fullId)
                assertTrue(
                    "${vst.format} instance was not admitted for quantum=$quantum " +
                        "instanceCount=$instanceCount",
                    pluginIndex >= 0,
                )
                pluginIndices += pluginIndex
                awaitCondition("${vst.format} rack publication for quantum=$quantum") {
                    engine.getRackPlugins(MASTER_PATH_ID).any { it.index == pluginIndex }
                }
            }
            val entries = engine.getRackPlugins(MASTER_PATH_ID)
            assertEquals(
                "Expected exactly $instanceCount rack plugin(s) for quantum=$quantum",
                instanceCount,
                entries.size,
            )
            assertTrue(
                "Rack contains a different plugin for quantum=$quantum",
                entries.all { it.info.fullId == vst.fullId },
            )
            assertEquals(
                "Same-UUID copies did not receive independent instance identities",
                instanceCount,
                entries.map { it.instanceId }.filter { it > 0L }.distinct().size,
            )

            val started = runBlocking { DirectUsbAudioManager.startConfigured(context) }
            assertTrue(
                "Direct USB start failed for quantum=$quantum instanceCount=$instanceCount: " +
                    started.exceptionOrNull()?.message,
                started.isSuccess,
            )

            // Startup is intentionally outside the verdict. In particular, do
            // not baseline before the guest has consumed its first wake.
            SystemClock.sleep(warmupMs)
            val guestFrames = ((128 + quantum - 1) / quantum) * quantum
            val expectedMinimumLatencyFrames = 4L * guestFrames - quantum
            val reportedPluginLatencyFrames = pluginIndices.associateWith { index ->
                val latencyFrames = engine.getPluginLatencyFrames(MASTER_PATH_ID, index)
                assertTrue(
                    "Reported plugin latency must be positive for quantum=$quantum " +
                        "instanceCount=$instanceCount index=$index: $latencyFrames",
                    latencyFrames > 0L,
                )
                assertTrue(
                    "Reported plugin latency too small for quantum=$quantum " +
                        "instanceCount=$instanceCount index=$index: reported=$latencyFrames " +
                        "expectedMinimum=$expectedMinimumLatencyFrames",
                    latencyFrames >= expectedMinimumLatencyFrames,
                )
                latencyFrames
            }
            val baselineRealtime = engine.getRealtimeStats()
            val baselineUsb = engine.getDirectUsbStats()
            SystemClock.sleep(steadyMs)
            val finalRealtime = engine.getRealtimeStats()
            val finalUsb = engine.getDirectUsbStats()

            val callbackFrames = delta(finalRealtime.callbackFrames, baselineRealtime.callbackFrames)
            val guestFramesProduced = delta(
                finalRealtime.vstGuestFramesProduced,
                baselineRealtime.vstGuestFramesProduced,
            )
            val minimumGuestFrames = callbackFrames.toDouble() * instanceCount * GUEST_PROGRESS_RATIO
            val vstOutputUnderruns = delta(
                finalRealtime.vstOutputUnderrunFrames,
                baselineRealtime.vstOutputUnderrunFrames,
            )
            val vstInputStarvations = delta(
                finalRealtime.vstInputStarvations,
                baselineRealtime.vstInputStarvations,
            )
            val vstGuestDeadlineMisses = delta(
                finalRealtime.vstGuestDeadlineMisses,
                baselineRealtime.vstGuestDeadlineMisses,
            )
            val callbackDeadlineMisses = delta(
                finalRealtime.callbackDeadlineMisses,
                baselineRealtime.callbackDeadlineMisses,
            )
            val xruns = delta(finalRealtime.xRunCount, baselineRealtime.xRunCount)
            val playbackXruns = delta(finalUsb.playbackXruns, baselineUsb.playbackXruns)
            val lostQuanta = delta(finalUsb.lostQuanta, baselineUsb.lostQuanta)
            val playbackQuantumDrops = delta(
                finalUsb.playbackQuantumDrops,
                baselineUsb.playbackQuantumDrops,
            )

            assertEquals(
                "Direct USB effective quantum changed",
                quantum.toLong(),
                finalUsb.effectiveQuantum,
            )
            assertTrue(
                "No realtime callback frame progress for quantum=$quantum " +
                    "instanceCount=$instanceCount",
                callbackFrames > 0L,
            )
            assertTrue(
                "Guest processing progress too low for quantum=$quantum " +
                    "instanceCount=$instanceCount: guest=$guestFramesProduced " +
                    "required>=$minimumGuestFrames callbackFrames=$callbackFrames",
                guestFramesProduced.toDouble() >= minimumGuestFrames,
            )
            assertEquals("VST output underruns for quantum=$quantum", 0L, vstOutputUnderruns)
            assertTrue(
                "VST input starvation count exceeded allowance for quantum=$quantum: " +
                    vstInputStarvations,
                vstInputStarvations <= MAX_GUEST_TIMING_MISSES,
            )
            assertTrue(
                "VST guest deadline misses exceeded allowance for quantum=$quantum: " +
                    vstGuestDeadlineMisses,
                vstGuestDeadlineMisses <= MAX_GUEST_TIMING_MISSES,
            )
            assertEquals("Direct USB xruns for quantum=$quantum", 0L, playbackXruns)
            assertEquals("Lost quanta for quantum=$quantum", 0L, lostQuanta)
            assertEquals("Playback quantum drops for quantum=$quantum", 0L, playbackQuantumDrops)
            Log.i(
                TAG,
                "VST_HEADLESS_RESULT format=${vst.format} plugin=${vst.fullId} " +
                    "quantum=$quantum requestedQuantum=$quantum " +
                    "effectiveQuantum=${finalUsb.effectiveQuantum} instanceCount=$instanceCount " +
                    "guestFrames=$guestFrames expectedMinimumLatencyFrames=$expectedMinimumLatencyFrames " +
                    "reportedPluginLatencyFrames=" +
                    reportedPluginLatencyFrames.entries.joinToString(
                        separator = ",",
                    ) { (index, latencyFrames) -> "$index:$latencyFrames" } + " " +
                    "callbackFrames=$callbackFrames guestFramesProduced=$guestFramesProduced " +
                    "vstOutputUnderrunFrames=$vstOutputUnderruns " +
                    "vstInputStarvations=$vstInputStarvations " +
                    "vstGuestDeadlineMisses=$vstGuestDeadlineMisses " +
                    "callbackDeadlineMisses=$callbackDeadlineMisses xruns=$xruns " +
                    "playbackXruns=$playbackXruns lostQuanta=$lostQuanta " +
                    "playbackQuantumDrops=$playbackQuantumDrops result=PASS",
            )
        } finally {
            // Stop the transport before removing the instances. This closes
            // the guest bridge and prevents the next case inheriting a live
            // callback.
            runCatching { DirectUsbAudioManager.disable(context) }
            runCatching { engine.stopEngine() }
            pluginIndices.sortedDescending().forEach { index ->
                runCatching { engine.removePluginFromRack(MASTER_PATH_ID, index) }
            }
        }
    }

    private fun clearRack(engine: NativeEngine) {
        val paths = (listOf(MASTER_PATH_ID) + engine.getTracks().map { it.id }).distinct()
        paths.forEach { pathId ->
            engine.getRackPlugins(pathId)
                .map { it.index }
                .sortedDescending()
                .forEach { index ->
                    assertTrue(
                        "Could not clear existing rack plugin path=$pathId index=$index",
                        engine.removePluginFromRack(pathId, index),
                    )
                }
            assertTrue(
                "Rack was not empty before VST case path=$pathId",
                engine.getRackPlugins(pathId).isEmpty(),
            )
        }
    }

    private fun awaitCondition(description: String, condition: () -> Boolean) {
        val deadline = SystemClock.elapsedRealtime() + RACK_PUBLICATION_TIMEOUT_MS
        while (!condition()) {
            if (SystemClock.elapsedRealtime() >= deadline) {
                throw AssertionError("Timed out waiting for $description")
            }
            Thread.sleep(CONDITION_POLL_MS)
        }
    }

    private fun parseQuanta(args: Bundle): List<Int> {
        val diagnostic = args.getString("vst_single_quantum")?.trim()
        if (diagnostic != null) {
            require(diagnostic.isNotEmpty()) {
                "vst_single_quantum must name exactly one allowed quantum"
            }
            val quantum = diagnostic.toIntOrNull()
                ?: throw IllegalArgumentException(
                    "vst_single_quantum must be an integer, got=$diagnostic",
                )
            require(quantum in ALLOWED_QUANTA) {
                "vst_single_quantum must be one of " +
                    "${ALLOWED_QUANTA.joinToString(",")}, got=$quantum"
            }
            return listOf(quantum)
        }
        require(args.getString("vst_quanta")?.trim().isNullOrEmpty()) {
            "vst_quanta cannot subset the default matrix; use vst_single_quantum for diagnostics"
        }
        return DEFAULT_QUANTA
    }

    private fun parseBooleanArgument(args: Bundle, key: String): Boolean {
        val raw = args.getString(key)?.trim() ?: return false
        return when (raw.lowercase()) {
            "true" -> true
            "false" -> false
            else -> throw IllegalArgumentException("$key must be true or false, got=$raw")
        }
    }

    private fun argumentLong(args: Bundle, primary: String, alias: String, default: Long, minimum: Long): Long {
        val raw = args.getString(primary) ?: args.getString(alias) ?: return default
        val value = raw.trim().toLongOrNull()
            ?: throw IllegalArgumentException("$primary must be an integer, got=$raw")
        require(value >= minimum) { "$primary must be >= $minimum, got=$value" }
        return value
    }

    private fun delta(after: Long, before: Long): Long {
        require(after >= before) { "Monotonic counter regressed: before=$before after=$after" }
        return after - before
    }

    private fun isUsbAudio(device: UsbDevice): Boolean =
        (0 until device.interfaceCount).any { index ->
            device.getInterface(index).interfaceClass == UsbConstants.USB_CLASS_AUDIO
        }

    private data class AudioSettingsSnapshot(
        val backend: AudioBackend,
        val bufferSize: Int,
        val deviceId: Int,
        val vendorId: Int,
        val productId: Int,
        val deviceName: String,
        val cachedFormats: List<DirectUsbFormat>,
        val rate: Int,
        val bits: Int,
        val subslot: Int,
        val channels: Int,
        val outputPair: Int,
    ) {
        fun restore(context: Context) {
            AudioSettingsManager.setAudioBackend(context, backend)
            AudioSettingsManager.setBufferSize(context, bufferSize)
            AudioSettingsManager.setDirectUsbDeviceId(context, deviceId)
            AudioSettingsManager.setDirectUsbIdentity(context, vendorId, productId, deviceName)
            AudioSettingsManager.setDirectUsbCachedFormats(context, cachedFormats)
            AudioSettingsManager.setDirectUsbFormat(context, rate, bits, subslot, channels)
            AudioSettingsManager.setDirectUsbOutputPair(context, outputPair)
        }

        companion object {
            fun capture(context: Context) = AudioSettingsSnapshot(
                backend = AudioSettingsManager.getAudioBackend(context),
                bufferSize = AudioSettingsManager.getBufferSize(context),
                deviceId = AudioSettingsManager.getDirectUsbDeviceId(context),
                vendorId = AudioSettingsManager.getDirectUsbVendorId(context),
                productId = AudioSettingsManager.getDirectUsbProductId(context),
                deviceName = AudioSettingsManager.getDirectUsbDeviceName(context),
                cachedFormats = AudioSettingsManager.getDirectUsbCachedFormats(context),
                rate = AudioSettingsManager.getDirectUsbRate(context),
                bits = AudioSettingsManager.getDirectUsbBits(context),
                subslot = AudioSettingsManager.getDirectUsbSubslot(context),
                channels = AudioSettingsManager.getDirectUsbChannels(context),
                outputPair = AudioSettingsManager.getDirectUsbOutputPair(context),
            )
        }
    }

    private companion object {
        const val TAG = "WineVstHeadlessStress"
        const val DEFAULT_VST_NAME = "ValhallaSupermassive"
        const val DEFAULT_WARMUP_MS = 2_000L
        const val DEFAULT_STEADY_MS = 5_000L
        const val MAX_TEST_TIMEOUT_MS = 900_000L
        const val RACK_PUBLICATION_TIMEOUT_MS = 15_000L
        const val MAX_GUEST_TIMING_MISSES = 1L
        const val CONDITION_POLL_MS = 10L
        const val GUEST_PROGRESS_RATIO = 0.98
        val DEFAULT_QUANTA = listOf(16, 32, 64, 128)
        val ALLOWED_QUANTA = DEFAULT_QUANTA.toSet()
    }
}
