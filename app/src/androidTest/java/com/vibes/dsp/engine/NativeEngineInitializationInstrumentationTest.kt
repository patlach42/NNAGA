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

            assertTrue("Repeated direct nativeInit failed", engine.nativeInit())

            val after = engine.getTracks().firstOrNull { it.id == createdTrackId }
            assertTrue("Repeated nativeInit destroyed the added rack track", after != null)
            val afterTrack = after!!
            assertEquals("Repeated nativeInit changed rack track state", beforeTrack, afterTrack)
            assertEquals("Repeated nativeInit changed rack track id", createdTrackId, afterTrack.id)
        } finally {
            trackId?.takeIf { it > MASTER_PATH_ID }?.let { engine.removeTrack(it) }
        }
    }
}
