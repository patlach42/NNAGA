package com.vibes.dsp.engine

import androidx.test.ext.junit.runners.AndroidJUnit4
import org.junit.Assert.assertFalse
import org.junit.Test
import org.junit.runner.RunWith

/**
 * Verifies that transport JNI entry points are exported by the loaded native library.
 * No native engine initialization is needed: an unavailable engine may return false,
 * but a missing export must raise UnsatisfiedLinkError.
 */
@RunWith(AndroidJUnit4::class)
class NativeEngineTransportJniInstrumentationTest {
    @Test
    fun transportControlsResolveWithoutEngineInitialization() {
        val engine = NativeEngine.getInstance()
        assertFalse("restartTransport must report unavailable engine", invokeNative("restartTransport") {
            engine.restartTransport()
        })
        assertFalse("stopTransport must report unavailable engine", invokeNative("stopTransport") {
            engine.stopTransport()
        })
    }

    private fun invokeNative(name: String, action: () -> Boolean): Boolean {
        return try {
            action()
        } catch (error: UnsatisfiedLinkError) {
            throw AssertionError("JNI export $name could not be resolved: ${error.message}", error)
        }
    }
}
