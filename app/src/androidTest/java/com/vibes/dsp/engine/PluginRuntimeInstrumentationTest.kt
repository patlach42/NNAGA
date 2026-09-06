package com.vibes.dsp.engine

import android.Manifest
import android.content.pm.PackageManager
import android.os.SystemClock
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import androidx.test.rule.GrantPermissionRule
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertTrue
import org.junit.Assume.assumeTrue
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith
import java.io.File

/**
 * Device-level admission and processing smoke test for the supported plugin runtimes.
 *
 * The test deliberately adds each plugin after the Oboe session is running: this
 * exercises runtime activation and publication rather than only registry metadata.
 */
@RunWith(AndroidJUnit4::class)
class PluginRuntimeInstrumentationTest {
    @get:Rule
    val recordAudioPermission: GrantPermissionRule =
        GrantPermissionRule.grant(Manifest.permission.RECORD_AUDIO)

    @Test(timeout = MAX_TIMEOUT_MS)
    fun discoversActivatesPublishesAndProcessesNativeJsfxAndLv2() {
        val instrumentation = InstrumentationRegistry.getInstrumentation()
        val context = instrumentation.targetContext
        assumeTrue(
            "RECORD_AUDIO permission is unavailable",
            context.checkSelfPermission(Manifest.permission.RECORD_AUDIO) ==
                PackageManager.PERMISSION_GRANTED,
        )

        val jsfxRoot = File(context.filesDir, "jsfx/Effects")
        val jsfxDataRoot = File(context.filesDir, "jsfx/Data")
        assertTrue("Could not create JSFX effects root", jsfxRoot.exists() || jsfxRoot.mkdirs())
        assertTrue("Could not create JSFX data root", jsfxDataRoot.exists() || jsfxDataRoot.mkdirs())

        val engine = NativeEngine.getInstance()
        engine.setJsfxRoot(jsfxRoot.absolutePath)
        EngineInitHelper.preloadLilv(context.applicationInfo.nativeLibraryDir)
        assertTrue("Native engine initialization failed", EngineInitHelper.initEngine(context))

        val available = engine.getAvailablePlugins()
        val native = available.firstOrNull {
            it.format == NATIVE_FORMAT && it.id == NATIVE_PLUGIN_ID
        }
        val jsfx = available.firstOrNull {
            it.format == JSFX_FORMAT && it.id == JSFX_PLUGIN_ID
        }
        val lv2 = available.firstOrNull {
            it.format == LV2_FORMAT && it.id in PREFERRED_BUNDLED_LV2_IDS
        } ?: available.firstOrNull { it.format == LV2_FORMAT }

        assertNotNull("Native filter was not discovered", native)
        assertNotNull("JSFX smoke plugin was not discovered", jsfx)
        assertNotNull("No LV2 plugin was discovered", lv2)

        val plugins = listOfNotNull(native, jsfx, lv2)
        plugins.forEach { info ->
            assertEquals(
                "Unexpected runtime class for ${info.fullId}",
                PluginRuntimeClass.CERTIFIED_IN_PROCESS,
                info.runtimeClass,
            )
            assertEquals(
                "Runtime class ordinal disagrees for ${info.fullId}",
                PluginRuntimeClass.CERTIFIED_IN_PROCESS.ordinalValue,
                info.realtimeClassOrdinal,
            )
        }

        val pathId = MASTER_PATH_ID
        val addedIndices = mutableListOf<Int>()
        try {
            assertTrue("Android Oboe session failed to start", engine.nativeStartAndroidOboeSession())
            awaitCondition("Android Oboe engine running") { engine.isEngineRunning() }
            assertTrue("Android Oboe session reported an error", !engine.nativeIsEngineError())
            val sessionStartCallbacks = engine.getRealtimeStats().callbackCount
            awaitCondition("Android Oboe warm-up callbacks") {
                engine.getRealtimeStats().callbackCount >=
                    sessionStartCallbacks + WARMUP_CALLBACKS
            }
            val baseline = engine.getRealtimeStats()

            val beforeUnknown = engine.getRackPlugins(pathId).toList()
            val unknownId = "NATIVE:__nnaga_missing_plugin__"
            assertEquals(
                "Unknown plugin unexpectedly admitted",
                -1,
                engine.addPluginToRack(pathId, unknownId),
            )
            assertEquals(
                "Unknown plugin did not report create failure",
                "plugin-create-failed:$unknownId",
                engine.getRackRealtimeDiagnostic(pathId),
            )
            assertEquals(
                "Unknown plugin changed rack contents",
                beforeUnknown,
                engine.getRackPlugins(pathId).toList(),
            )

            plugins.forEach { expected ->
                val previousCallbacks = engine.getRealtimeStats().callbackCount
                val index = engine.addPluginToRack(pathId, expected.fullId)
                assertTrue("${expected.fullId} was not admitted", index >= 0)
                addedIndices += index

                awaitCondition("${expected.fullId} rack publication") {
                    engine.getRackPlugins(pathId).any { entry ->
                        entry.index == index && entry.info.fullId == expected.fullId
                    } && engine.getRackRealtimeDiagnostic(pathId).isBlank()
                }
                val entry = engine.getRackPlugins(pathId).firstOrNull { it.index == index }
                assertNotNull("${expected.fullId} metadata was not published", entry)
                assertEquals(index, entry!!.index)
                assertEquals(expected.fullId, entry.info.fullId)
                assertEquals(expected.id, entry.info.id)
                assertEquals(expected.format, entry.info.format)
                assertEquals(expected.name, entry.info.name)
                assertEquals(expected.runtimeClass, entry.info.runtimeClass)
                assertEquals(
                    "${expected.fullId} instance identity was not published",
                    entry.instanceId,
                    engine.getRackPluginInstanceId(pathId, index),
                )

                awaitCondition("${expected.fullId} audio callback") {
                    engine.getRealtimeStats().callbackCount > previousCallbacks
                }
            }

            val after = engine.getRealtimeStats()
            val before = baseline
            assertEquals(
                "Audio frame-capacity violations increased",
                before.frameCapacityViolations,
                after.frameCapacityViolations,
            )
            assertEquals(
                "MIDI event drops increased",
                before.midiEventDrops,
                after.midiEventDrops,
            )
            assertEquals(
                "Plan-publication deferrals increased",
                before.planPublishDeferrals,
                after.planPublishDeferrals,
            )
            assertEquals(
                "Audio xruns increased",
                before.xRunCount,
                after.xRunCount,
            )
            assertTrue(
                "Realtime callback count did not advance",
                after.callbackCount > before.callbackCount,
            )
        } finally {
            addedIndices.asReversed().forEach { index ->
                runCatching { engine.removePluginFromRack(pathId, index) }
            }
            runCatching { engine.stopEngine() }
        }
    }

    private fun awaitCondition(description: String, condition: () -> Boolean) {
        val deadline = SystemClock.elapsedRealtime() + CONDITION_TIMEOUT_MS
        while (!condition()) {
            if (SystemClock.elapsedRealtime() >= deadline) {
                throw AssertionError("Timed out waiting for $description")
            }
            Thread.sleep(POLL_INTERVAL_MS)
        }
    }

    private companion object {
        const val MAX_TIMEOUT_MS = 60_000L
        const val CONDITION_TIMEOUT_MS = 15_000L
        const val POLL_INTERVAL_MS = 10L
        const val WARMUP_CALLBACKS = 20L
        const val NATIVE_FORMAT = "NATIVE"
        const val NATIVE_PLUGIN_ID = "com.vibes.dsp.filter"
        const val JSFX_FORMAT = "JSFX"
        const val JSFX_PLUGIN_ID = "NNAGA/NNAGA_Smoke.jsfx"
        const val LV2_FORMAT = "LV2"
        val PREFERRED_BUNDLED_LV2_IDS = listOf(
            "https://faustlv2.bitbucket.io/doubletracker",
            "http://gareus.org/oss/lv2/fil4#mono",
            "http://gareus.org/oss/lv2/fil4#stereo",
            "https://github.com/brummer10/CollisionDrive",
        )
    }
}
