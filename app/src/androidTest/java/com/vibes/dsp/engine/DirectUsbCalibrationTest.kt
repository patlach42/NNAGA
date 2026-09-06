package com.vibes.dsp.engine

import android.content.Context
import android.hardware.usb.UsbConstants
import android.hardware.usb.UsbDevice
import android.hardware.usb.UsbManager
import android.os.Bundle
import android.util.Log
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.withTimeout
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Assume.assumeTrue
import org.junit.Test
import org.junit.runner.RunWith
import java.util.Locale

@RunWith(AndroidJUnit4::class)
class DirectUsbCalibrationTest {
    @Test(timeout = MAX_TIMEOUT_MS)
    fun onDeviceWatermarkCalibrationPersistsOnlyMeasuredSafeTarget() {
        val context = InstrumentationRegistry.getInstrumentation().targetContext
        val usb = context.getSystemService(Context.USB_SERVICE) as UsbManager
        val audioDevices = usb.deviceList.values.filter(::isUsbAudio)
        val authorized = audioDevices.filter(usb::hasPermission)
        if (authorized.isEmpty()) {
            Log.i(TAG, "SKIP reason=no-authorized-usb-audio-device discovered=${audioDevices.size}")
        }
        assumeTrue("SKIP reason=no-authorized-usb-audio-device", authorized.isNotEmpty())

        val configuredId = AudioSettingsManager.getDirectUsbDeviceId(context)
        val option = DirectUsbAudioManager.getAudioDevices(context)
            .firstOrNull { candidate ->
                candidate.id == configuredId && authorized.any { it.deviceId == candidate.id }
            }
        if (option == null) {
            Log.i(TAG, "SKIP reason=no-configured-authorized-usb-audio-device configured_id=$configuredId")
        }
        assumeTrue(
            "SKIP reason=no-configured-authorized-usb-audio-device",
            option != null
        )

        val format = DirectUsbFormat(
            AudioSettingsManager.getDirectUsbRate(context),
            AudioSettingsManager.getDirectUsbBits(context),
            AudioSettingsManager.getDirectUsbSubslot(context),
            AudioSettingsManager.getDirectUsbChannels(context)
        )
        val bufferFrames = AudioSettingsManager.getBufferSize(context)
        val periodMultiplier = AudioSettingsManager.getDirectUsbPeriodMultiplier(context)
        val outputPair = AudioSettingsManager.getDirectUsbOutputPair(context)
        val engine = NativeEngine.getInstance()
        EngineInitHelper.preloadLilv(context.applicationInfo.nativeLibraryDir)
        assertTrue("Native engine initialization failed", EngineInitHelper.initEngine(context))

        try {
            val probe = runBlocking {
                DirectUsbAudioManager.probeFormats(context, option!!)
            }
            assertTrue(
                "Direct USB probe failed: ${probe.exceptionOrNull()?.message}",
                probe.isSuccess
            )

            val nativeFormats = engine.nativeGetDirectUsbOutputFormats()
                .asSequence()
                .chunked(4)
                .filter { tuple -> tuple.size == 4 && tuple.all { value -> value > 0 } }
                .map { tuple -> DirectUsbFormat(tuple[0], tuple[1], tuple[2], tuple[3]) }
                .distinct()
                .toList()
            if (nativeFormats.isEmpty()) {
                Log.i(TAG, "SKIP reason=no-verified-native-usb-format-descriptors")
            }
            assumeTrue(
                "SKIP reason=no-verified-native-usb-format-descriptors",
                nativeFormats.isNotEmpty()
            )
            assumeTrue(
                "SKIP reason=configured-format-not-in-native-descriptors format=${formatToken(format)}",
                format in nativeFormats
            )
            assertTrue(
                "Configured format was not returned by the real interface probe",
                format in probe.getOrThrow()
            )
            assumeTrue(
                "SKIP reason=no-usb-capture-channels",
                DirectUsbAudioManager.getInputChannelCount() > 0
            )

            val result = runBlocking {
                withTimeout(calibrationTimeoutMs(InstrumentationRegistry.getArguments())) {
                    DirectUsbAudioManager.calibrate(
                        context,
                        option!!,
                        format,
                        fixedSampleRate = format.sampleRate,
                        onProgress = {},
                        stopAfterFirstStable = true
                    )
                }
            }

            Log.i(
                TAG,
                "CALIBRATION_RESULT buffer_frames=$bufferFrames " +
                    "output_pair=$outputPair vid=${formatId(result.vendorId)} " +
                    "pid=${formatId(result.productId)} format=${formatToken(result.format)} " +
                    "selected_frames=${result.selectedFrames} " +
                    "selected_ms=${formatMilliseconds(result.selectedMilliseconds)} " +
                    "passed=${result.passedCandidates.joinToString(",")} " +
                    "failed=${result.failedCandidates.joinToString(",")}"
            )
            assertEquals("Calibration used a different interface", option!!.vendorId, result.vendorId)
            assertEquals("Calibration used a different interface", option!!.productId, result.productId)
            assertEquals("Calibration used a different full format", format, result.format)
            assertTrue("Calibration did not succeed: ${result.message}", result.success)
            assertTrue("Calibration selected no frames", result.selectedFrames > 0)
            val expectedMilliseconds = result.selectedFrames * 1000.0 / format.sampleRate
            assertEquals(
                "Selected milliseconds do not match selected frames and sample rate",
                expectedMilliseconds,
                result.selectedMilliseconds,
                0.001
            )
            assertTrue(
                "Selected target was not among passed candidates",
                result.selectedFrames in result.passedCandidates
            )
            val persisted = AudioSettingsManager.getDirectUsbWatermark(
                context,
                result.vendorId,
                result.productId,
                result.format.sampleRate,
                result.format.bits,
                result.format.subslotBytes,
                result.format.channels
            )
            assertEquals("Measured target was not persisted for the full tuple", result.selectedFrames, persisted)
        } finally {
            runCatching { DirectUsbAudioManager.disable(context) }
            val stopped = runCatching { engine.getDirectUsbStats() }.getOrNull()
            assertTrue(
                "Direct USB transport was not stopped after calibration",
                    stopped?.state == DirectUsbSessionState.Stopped && !engine.isEngineRunning()
            )
        }
    }

    private fun calibrationTimeoutMs(args: Bundle): Long =
        (args.getString("direct_usb_calibration_timeout_ms") ?: DEFAULT_TIMEOUT_MS.toString())
            .toLongOrNull()
            ?.coerceIn(MIN_TIMEOUT_MS, MAX_TIMEOUT_MS)
            ?: DEFAULT_TIMEOUT_MS

    private fun formatToken(format: DirectUsbFormat): String =
        "${format.sampleRate}/${format.bits}/${format.subslotBytes}/${format.channels}"

    private fun formatMilliseconds(milliseconds: Double): String =
        String.format(Locale.US, "%.3f", milliseconds)

    private fun formatId(value: Int): String =
        String.format(Locale.US, "%04x", value)

    private fun isUsbAudio(device: UsbDevice): Boolean =
        device.deviceClass == UsbConstants.USB_CLASS_AUDIO ||
            (0 until device.interfaceCount).any {
                device.getInterface(it).interfaceClass == UsbConstants.USB_CLASS_AUDIO
            }

    private companion object {
        const val TAG = "DirectUsbCalibration"
        const val MIN_TIMEOUT_MS = 60_000L
        const val DEFAULT_TIMEOUT_MS = 540_000L
        const val MAX_TIMEOUT_MS = 600_000L
    }
}
