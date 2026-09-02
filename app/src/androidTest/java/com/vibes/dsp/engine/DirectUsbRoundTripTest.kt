package com.vibes.dsp.engine

import android.content.Context
import android.hardware.usb.UsbConstants
import android.hardware.usb.UsbDevice
import android.hardware.usb.UsbManager
import android.util.Log
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import kotlinx.coroutines.runBlocking
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Assume.assumeTrue
import org.junit.Test
import org.junit.runner.RunWith
import java.util.Locale

/**
 * Hardware-gated full analog Direct USB round-trip measurement.
 *
 * Connect the selected interface's output 1 to input 1 before running this
 * test. A missing USB audio interface is skipped; once hardware is present,
 * permission, probing, startup, and measurement failures are test failures.
 */
@RunWith(AndroidJUnit4::class)
class DirectUsbRoundTripTest {
    @Test(timeout = MAX_TIMEOUT_MS)
    fun connectedOutputOneToInputOneCableProducesAnalogRoundTrip() {
        val context = InstrumentationRegistry.getInstrumentation().targetContext
        val usb = context.getSystemService(Context.USB_SERVICE) as UsbManager
        val audioDevices = usb.deviceList.values.filter(::isUsbAudio)
        if (audioDevices.isEmpty()) {
            Log.i(TAG, "SKIP reason=no-usb-audio-device discovered=0")
        }
        assumeTrue("SKIP reason=no-usb-audio-device", audioDevices.isNotEmpty())

        val configuredId = AudioSettingsManager.getDirectUsbDeviceId(context)
        val option = DirectUsbAudioManager.getAudioDevices(context).firstOrNull { candidate ->
            candidate.id == configuredId && audioDevices.any { it.deviceId == candidate.id }
        }
        if (option == null) {
            Log.i(TAG, "SKIP reason=no-configured-usb-audio-device configured_id=$configuredId")
        }
        assumeTrue(
            "SKIP reason=no-configured-usb-audio-device",
            option != null,
        )
        val selectedDevice = option!!
        val originalOutputPair = AudioSettingsManager.getDirectUsbOutputPair(context)
        val configuredFormat = DirectUsbFormat(
            AudioSettingsManager.getDirectUsbRate(context),
            AudioSettingsManager.getDirectUsbBits(context),
            AudioSettingsManager.getDirectUsbSubslot(context),
            AudioSettingsManager.getDirectUsbChannels(context),
        )
        val engine = NativeEngine.getInstance()

        try {
            // Output pair 0 is the physical output 1/2 pair; the cable is on output 1.
            AudioSettingsManager.setDirectUsbOutputPair(context, 0)
            EngineInitHelper.preloadLilv(context.applicationInfo.nativeLibraryDir)
            assertTrue("Native engine initialization failed", EngineInitHelper.initEngine(context))

            val probe = runBlocking {
                DirectUsbAudioManager.probeFormats(context, selectedDevice)
            }
            assertTrue(
                "Direct USB probe failed: ${probe.exceptionOrNull()?.message}",
                probe.isSuccess,
            )
            assertTrue(
                "USB permission was not granted after probing",
                audioDevices.any { it.deviceId == selectedDevice.id && usb.hasPermission(it) },
            )
            assertTrue(
                "Direct USB interface exposes no capture channels",
                DirectUsbAudioManager.getInputChannelCount() > 0,
            )
            assertTrue(
                "Configured Direct USB format was not returned by the real interface probe",
                configuredFormat in probe.getOrThrow(),
            )

            // Keep the configured format/profile path under test rather than starting JNI directly.
            DirectUsbAudioManager.startSelected(context, configuredFormat)
            val started = runBlocking {
                DirectUsbAudioManager.startConfigured(context)
            }
            assertTrue(
                "Direct USB start failed: ${started.exceptionOrNull()?.message}",
                started.isSuccess,
            )
            val running = engine.getDirectUsbStats()
            assertEquals("Direct USB transport did not reach Running", DirectUsbSessionState.Running, running.state)
            assertEquals("Direct USB transport reported a startup failure", DirectUsbFailure.Ok, running.failure)

            for (runIndex in 1..2) {
                val measurement = runBlocking {
                    DirectUsbAudioManager.measureRoundTrip()
                }
                assertTrue(
                    "Direct USB round-trip measurement failed on run $runIndex: " +
                        measurement.exceptionOrNull()?.message,
                    measurement.isSuccess,
                )
                val result = measurement.getOrThrow()
                Log.i(
                    TAG,
                    "ROUNDTRIP_RESULT run=$runIndex frames=${result.latencyFrames} " +
                        "ms=${formatMilliseconds(result.latencyMilliseconds)} " +
                        "correlation=${formatValue(result.correlation)} " +
                        "input_peak=${formatValue(result.inputPeak)} " +
                        "output_peak=${formatValue(result.outputPeak)} " +
                        "rate=${configuredFormat.sampleRate} output_pair=0",
                )
                assertTrue(
                    "Round-trip latency frames must be positive on run $runIndex",
                    result.latencyFrames > 0,
                )
                assertEquals(
                    "Round-trip milliseconds do not match frames and sample rate on run $runIndex",
                    result.latencyFrames * 1000.0 / configuredFormat.sampleRate,
                    result.latencyMilliseconds,
                    0.001,
                )
                assertTrue(
                    "Round-trip correlation is too weak on run $runIndex",
                    result.correlation >= MIN_CORRELATION,
                )
                assertTrue(
                    "Round-trip output peak is too weak on run $runIndex",
                    result.outputPeak >= MIN_OUTPUT_PEAK,
                )
                assertTrue(
                    "Round-trip input peak is too weak on run $runIndex",
                    result.inputPeak >= MIN_INPUT_PEAK,
                )
            }

            val afterMeasurement = engine.getDirectUsbStats()
            assertEquals(
                "Direct USB transport stopped or failed during measurement",
                DirectUsbSessionState.Running,
                afterMeasurement.state,
            )
            assertEquals(
                "Direct USB transport reported a failure after measurement",
                DirectUsbFailure.Ok,
                afterMeasurement.failure,
            )
        } finally {
            val disableFailure = runCatching {
                DirectUsbAudioManager.disable(context)
            }.exceptionOrNull()
            val stopped = runCatching { engine.getDirectUsbStats() }.getOrNull()
            val engineRunning = runCatching { engine.isEngineRunning() }.getOrDefault(true)
            AudioSettingsManager.setDirectUsbOutputPair(context, originalOutputPair)
            assertTrue(
                "Direct USB disable failed: ${disableFailure?.message}",
                disableFailure == null,
            )
            assertTrue(
                "Direct USB transport was not stopped after round-trip measurement",
                stopped?.state == DirectUsbSessionState.Stopped &&
                    stopped.failure == DirectUsbFailure.Ok &&
                    !engineRunning,
            )
        }
    }

    private fun formatMilliseconds(milliseconds: Double): String =
        String.format(Locale.US, "%.3f", milliseconds)

    private fun formatValue(value: Double): String =
        String.format(Locale.US, "%.6f", value)

    private fun isUsbAudio(device: UsbDevice): Boolean =
        device.deviceClass == UsbConstants.USB_CLASS_AUDIO ||
            (0 until device.interfaceCount).any {
                device.getInterface(it).interfaceClass == UsbConstants.USB_CLASS_AUDIO
            }

    private companion object {
        const val TAG = "DirectUsbRoundTrip"
        const val MAX_TIMEOUT_MS = 30_000L
        const val MIN_CORRELATION = 0.2
        const val MIN_OUTPUT_PEAK = 0.2
        const val MIN_INPUT_PEAK = 0.005
    }
}
