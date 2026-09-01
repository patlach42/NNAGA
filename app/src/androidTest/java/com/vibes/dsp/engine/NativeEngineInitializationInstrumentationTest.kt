package com.vibes.dsp.engine

import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.runner.RunWith

/**
 * Regression coverage for activity recreation calling nativeInit more than once.
 * The second direct JNI init must leave the existing rack graph untouched.
 */
@RunWith(AndroidJUnit4::class)
class NativeEngineInitializationInstrumentationTest {
    @Test
    fun nativeInitAfterSuccessfulInitPreservesAddedRackTrack() {
        val context = InstrumentationRegistry.getInstrumentation().targetContext
        val engine = NativeEngine.getInstance()
        EngineInitHelper.preloadLilv(context.applicationInfo.nativeLibraryDir)
        assertTrue("Initial native engine initialization failed", EngineInitHelper.initEngine(context))

        var trackId: Long? = null
        try {
            val createdTrackId = engine.addTrack()
            trackId = createdTrackId
            assertTrue("Adding a rack track failed", createdTrackId > MASTER_PATH_ID)
            assertTrue("Setting track volume failed", engine.setTrackVolume(createdTrackId, 0.37f))
            assertTrue("Arming track input failed", engine.setTrackInputArmed(createdTrackId, true))

            val before = engine.getTracks().firstOrNull { it.id == createdTrackId }
            assertTrue("Added rack track was not returned by getTracks", before != null)
            val beforeTrack = before!!
            assertEquals(createdTrackId, beforeTrack.id)
            assertEquals(0.37f, beforeTrack.volume, 0.0001f)
            assertTrue("Track input arm state was not applied", beforeTrack.inputArmed)
            assertTrue(
                "Initial rack track capture timestamp was not set",
                beforeTrack.capturedAtMonotonicNanos > 0L,
            )

            assertTrue("Repeated direct nativeInit failed", engine.nativeInit())

            val after = engine.getTracks().firstOrNull { it.id == createdTrackId }
            assertTrue("Repeated nativeInit destroyed the added rack track", after != null)
            val afterTrack = after!!
            assertEquals("Repeated nativeInit changed rack track id", beforeTrack.id, afterTrack.id)
            assertEquals("Repeated nativeInit changed rack track volume", beforeTrack.volume, afterTrack.volume, 0.0f)
            assertEquals("Repeated nativeInit changed input arm state", beforeTrack.inputArmed, afterTrack.inputArmed)
            assertEquals("Repeated nativeInit changed input arm lock state", beforeTrack.inputArmLocked, afterTrack.inputArmLocked)
            assertEquals("Repeated nativeInit changed wav loaded state", beforeTrack.wavLoaded, afterTrack.wavLoaded)
            assertEquals("Repeated nativeInit changed wav display name", beforeTrack.wavDisplayName, afterTrack.wavDisplayName)
            assertEquals("Repeated nativeInit changed wav duration", beforeTrack.wavDurationSec, afterTrack.wavDurationSec, 0.0)
            assertEquals("Repeated nativeInit changed playing state", beforeTrack.playing, afterTrack.playing)
            assertEquals("Repeated nativeInit changed looping state", beforeTrack.looping, afterTrack.looping)
            assertEquals("Repeated nativeInit changed position", beforeTrack.positionSec, afterTrack.positionSec, 0.0)
            assertEquals("Repeated nativeInit changed transport frame", beforeTrack.transportFrame, afterTrack.transportFrame)
            assertEquals("Repeated nativeInit changed record pending state", beforeTrack.recordPending, afterTrack.recordPending)
            assertEquals("Repeated nativeInit changed recording state", beforeTrack.recording, afterTrack.recording)
            assertEquals("Repeated nativeInit changed punch armed state", beforeTrack.punchArmed, afterTrack.punchArmed)
            assertEquals("Repeated nativeInit changed input source kind", beforeTrack.inputSourceKind, afterTrack.inputSourceKind)
            assertEquals(
                "Repeated nativeInit changed input source first channel",
                beforeTrack.inputSourceFirstChannel,
                afterTrack.inputSourceFirstChannel,
            )
            assertEquals("Repeated nativeInit changed input source track", beforeTrack.inputSourceTrackId, afterTrack.inputSourceTrackId)
            assertEquals("Repeated nativeInit changed input tap", beforeTrack.inputTap, afterTrack.inputTap)
            assertEquals("Repeated nativeInit changed midi loaded state", beforeTrack.midiLoaded, afterTrack.midiLoaded)
            assertEquals("Repeated nativeInit changed midi playing state", beforeTrack.midiPlaying, afterTrack.midiPlaying)
            assertEquals("Repeated nativeInit changed selected slot", beforeTrack.selectedSlot, afterTrack.selectedSlot)
            assertEquals(
                "Repeated nativeInit changed default loop length",
                beforeTrack.defaultLoopLengthBars,
                afterTrack.defaultLoopLengthBars,
                0.0,
            )
            assertEquals("Repeated nativeInit changed active slot", beforeTrack.activeSlot, afterTrack.activeSlot)
            assertEquals(
                "Repeated nativeInit changed musical quarter notes",
                beforeTrack.musicalQuarterNotes,
                afterTrack.musicalQuarterNotes,
                0.0,
            )
            assertEquals("Repeated nativeInit changed sample rate", beforeTrack.sampleRate, afterTrack.sampleRate, 0.0)
            assertEquals("Repeated nativeInit changed recording slot", beforeTrack.recordingSlot, afterTrack.recordingSlot)
            assertEquals("Repeated nativeInit changed track name", beforeTrack.name, afterTrack.name)
            assertEquals("Repeated nativeInit changed track color", beforeTrack.colorArgb, afterTrack.colorArgb)
            assertTrue(
                "Initial rack track capture timestamp was not set",
                afterTrack.capturedAtMonotonicNanos > 0L,
            )
            assertTrue(
                "Repeated nativeInit moved rack track capture timestamp backwards",
                afterTrack.capturedAtMonotonicNanos >= beforeTrack.capturedAtMonotonicNanos,
            )
            assertEquals("Repeated nativeInit changed rack track id", createdTrackId, afterTrack.id)
        } finally {
            trackId?.takeIf { it > MASTER_PATH_ID }?.let { engine.removeTrack(it) }
        }
    }
}
