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
        var midiSourceHandle = 0L
        try {
            val createdTrackId = engine.addTrack()
            trackId = createdTrackId
            assertTrue("Adding a rack track failed", createdTrackId > MASTER_PATH_ID)
            assertTrue("Setting track volume failed", engine.setTrackVolume(createdTrackId, 0.37f))
            assertTrue("Arming track input failed", engine.setTrackInputArmed(createdTrackId, true))
            midiSourceHandle = engine.registerUsbMidiSource(
                vendorId = 0x1234,
                productId = 0xabcd,
                serialNumber = "instrumentation-midi",
                portNumber = 7,
            )
            assertTrue("Registering MIDI source failed", midiSourceHandle > 0L)
            assertTrue(
                "Setting MIDI input failed",
                engine.setTrackMidiInputUsb(
                    createdTrackId,
                    0x1234,
                    0xabcd,
                    "instrumentation-midi",
                    7,
                    "Instrumentation MIDI",
                    midiSourceHandle,
                ),
            )
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
            assertEquals("Initial MIDI input kind was not returned", 1, beforeTrack.midiInputKind)
            assertEquals("Initial MIDI vendor id was not returned", 0x1234, beforeTrack.midiVendorId)
            assertEquals("Initial MIDI product id was not returned", 0xabcd, beforeTrack.midiProductId)
            assertEquals("Initial MIDI serial was not returned", "instrumentation-midi", beforeTrack.midiSerialNumber)
            assertEquals("Initial MIDI port was not returned", 7, beforeTrack.midiPortNumber)
            assertEquals("Initial MIDI display name was not returned", "Instrumentation MIDI", beforeTrack.midiDisplayName)
            assertEquals("Initial MIDI source track was not returned", 0L, beforeTrack.midiInputSourceTrackId)
            assertTrue("Initial MIDI source connection was not returned", beforeTrack.midiInputConnected)
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
            assertEquals("Repeated nativeInit changed MIDI input kind", beforeTrack.midiInputKind, afterTrack.midiInputKind)
            assertEquals("Repeated nativeInit changed MIDI vendor id", beforeTrack.midiVendorId, afterTrack.midiVendorId)
            assertEquals("Repeated nativeInit changed MIDI product id", beforeTrack.midiProductId, afterTrack.midiProductId)
            assertEquals("Repeated nativeInit changed MIDI serial", beforeTrack.midiSerialNumber, afterTrack.midiSerialNumber)
            assertEquals("Repeated nativeInit changed MIDI port", beforeTrack.midiPortNumber, afterTrack.midiPortNumber)
            assertEquals("Repeated nativeInit changed MIDI display name", beforeTrack.midiDisplayName, afterTrack.midiDisplayName)
            assertEquals("Repeated nativeInit changed MIDI source track", beforeTrack.midiInputSourceTrackId, afterTrack.midiInputSourceTrackId)
            assertEquals("Repeated nativeInit changed MIDI connection", beforeTrack.midiInputConnected, afterTrack.midiInputConnected)
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
            if (midiSourceHandle != 0L) engine.unregisterUsbMidiSource(midiSourceHandle)
            trackId?.takeIf { it > MASTER_PATH_ID }?.let { engine.removeTrack(it) }
        }
    }
}
