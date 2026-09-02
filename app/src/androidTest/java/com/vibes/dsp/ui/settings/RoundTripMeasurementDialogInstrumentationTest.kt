package com.vibes.dsp.ui.settings

import androidx.test.core.app.ActivityScenario
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import androidx.test.uiautomator.By
import androidx.test.uiautomator.Direction
import androidx.test.uiautomator.UiDevice
import androidx.test.uiautomator.UiObject2
import androidx.test.uiautomator.Until
import com.vibes.dsp.MainActivity
import com.vibes.dsp.engine.AudioBackend
import com.vibes.dsp.engine.AudioSettingsManager
import com.vibes.dsp.engine.EngineInitHelper
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.runner.RunWith

/**
 * Verifies that the hardware-independent round-trip instructions remain reachable from Settings.
 *
 * This test deliberately does not activate the measurement: the engine test suite covers the
 * hardware operation, while this test protects the user-facing entry point and instructions.
 */
@RunWith(AndroidJUnit4::class)
class RoundTripMeasurementDialogInstrumentationTest {
    @Test
    fun settingsRoundTripDialogExplainsPhysicalPathAndControls() {
        val context = InstrumentationRegistry.getInstrumentation().targetContext
        val previousBackend = AudioSettingsManager.getAudioBackend(context)
        AudioSettingsManager.setAudioBackend(context, AudioBackend.DirectUsb)

        var scenario: ActivityScenario<MainActivity>? = null
        try {
            EngineInitHelper.preloadLilv(context.applicationInfo.nativeLibraryDir)
            assertTrue(
                "Native engine initialization failed",
                EngineInitHelper.initEngine(context),
            )

            scenario = ActivityScenario.launch(MainActivity::class.java)
            val device = UiDevice.getInstance(InstrumentationRegistry.getInstrumentation())

            await(device, By.desc("NNAGA"), "live dashboard control")!!.click()
            await(device, By.text("Settings"), "Settings tab")!!.click()
            await(device, By.text("Driver"), "Audio settings tab")!!.click()
            val measureButton = awaitAfterScrolling(
                device = device,
                selector = By.text("Measure round-trip"),
                description = "round-trip measurement button",
            )
            measureButton.click()

            assertTextPresent(device, "Measure full analog round trip")
            assertTextPresent(
                device,
                "Connect a physical cable from USB interface output 1 to input 1.",
            )
            assertTextPresent(device, "NNAGA temporarily mutes the rack output")
            assertTextPresent(device, "plays a short audible probe on output 1")
            assertTextPresent(device, "complete analog path through the DAC, cable, and ADC")
            assertTextPresent(device, "not the estimated host queue shown in Current USB Session")
            assertTextPresent(device, "Start measurement")
            assertTextPresent(device, "Cancel")

            // Cancel is the initial dialog's close action; no measurement is started.
            await(device, By.text("Cancel"), "round-trip dialog close control")!!.click()
            assertTrue(
                "Round-trip dialog remained visible after closing",
                device.wait(Until.gone(By.text("Measure full analog round trip")), DIALOG_TIMEOUT_MS),
            )
        } finally {
            scenario?.close()
            AudioSettingsManager.setAudioBackend(context, previousBackend)
        }
    }

    private fun await(device: UiDevice, selector: androidx.test.uiautomator.BySelector, description: String): UiObject2? {
        val result = device.wait(Until.findObject(selector), UI_TIMEOUT_MS)
        assertNotNull("Timed out waiting for $description", result)
        return result
    }

    private fun awaitAfterScrolling(
        device: UiDevice,
        selector: androidx.test.uiautomator.BySelector,
        description: String,
    ): UiObject2 {
        device.findObject(selector)?.let { found ->
            if (!found.visibleBounds.isEmpty) return found
        }

        val scrollable = device.findObject(By.scrollable(true))
        assertNotNull("Could not find settings scroll container while looking for $description", scrollable)
        repeat(MAX_SCROLL_ATTEMPTS) {
            scrollable!!.scroll(Direction.DOWN, 0.8f)
            device.findObject(selector)?.let { found ->
                if (!found.visibleBounds.isEmpty) return found
            }
        }
        return await(device, selector, description)!!
    }

    private fun assertTextPresent(device: UiDevice, expected: String) {
        assertNotNull(
            "Round-trip dialog is missing instruction: $expected",
            device.wait(Until.findObject(By.textContains(expected)), DIALOG_TIMEOUT_MS),
        )
    }

    private companion object {
        const val UI_TIMEOUT_MS = 30_000L
        const val DIALOG_TIMEOUT_MS = 5_000L
        const val MAX_SCROLL_ATTEMPTS = 24
    }
}
