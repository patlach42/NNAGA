package com.vibes.dsp.ui.rack

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

class ClipSourceBpmFilenameTest {
    @Test
    fun parsesSupportedBpmTokensIncludingDecimalAndRangeBoundaries() {
        val cases = listOf(
            "loop_172bpm.wav" to 172.0,
            "Loop 172 BPM.WAV" to 172.0,
            "x_172.5bpm.wav" to 172.5,
            "x_172,5bpm.wav" to 172.5,
            "loop_20bpm.wav" to 20.0,
            "loop_400BPM.wav" to 400.0,
        )

        cases.forEach { (filename, expected) ->
            assertEquals(filename, expected, parseClipSourceBpmFromFilename(filename) ?: Double.NaN, 0.0)
        }
    }

    @Test
    fun rejectsUnsupportedOrMissingBpmTokensAndEmbeddedNumericMatches() {
        val filenames = listOf(
            "loop_19bpm.wav",
            "loop_401bpm.wav",
            "loop_172.wav",
            "x_1172.5bpm.wav",
        )

        filenames.forEach { filename ->
            assertNull(filename, parseClipSourceBpmFromFilename(filename))
        }
    }
}
