package com.vibes.dsp.ui

import com.vibes.dsp.ui.components.MusicalPosition
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

class MusicalPositionTest {
    @Test
    fun formatUsesCanonicalZeroAndBarBeatSixteenthBoundaries() {
        val cases = listOf(
            0.0 to "1:1:1:0",
            (1.0 / 960.0) to "1:1:1:1",
            (239.0 / 960.0) to "1:1:1:239",
            0.25 to "1:1:2:0",
            1.0 to "1:2:1:0",
            4.0 to "2:1:1:0",
        )

        cases.forEach { (quarterNotes, expected) ->
            assertEquals(expected, MusicalPosition.format(quarterNotes))
        }
    }

    @Test
    fun parseConvertsExactGridPositionsToQuarterNotes() {
        val cases = listOf(
            "1:1:1:0" to 0.0,
            "1:1:1:239" to (239.0 / 960.0),
            "1:1:2:0" to 0.25,
            "1:2:1:0" to 1.0,
            "1:4:4:239" to (3839.0 / 960.0),
            "2:1:1:0" to 4.0,
        )

        cases.forEach { (text, expected) ->
            assertEquals(text, expected, MusicalPosition.parse(text) ?: Double.NaN, 0.0)
        }
    }

    @Test
    fun parseRejectsMalformedNegativeAndOutOfRangePositions() {
        val invalidPositions = listOf(
            "",
            "1:1:1",
            "1:1:1:0:0",
            "one:1:1:0",
            "1.0:1:1:0",
            "-1:1:1:0",
            "1:0:1:0",
            "1:5:1:0",
            "1:1:0:0",
            "1:1:1:-1",
            "1:1:1:240",
        )

        invalidPositions.forEach { text ->
            assertNull(text, MusicalPosition.parse(text))
        }
    }

    @Test
    fun parseRejectsValuesThatOverflowGridArithmetic() {
        assertNull(
            MusicalPosition.parse("9223372036854775807:1:1:0"),
        )
        assertNull(
            MusicalPosition.parse("999999999999999999999999:1:1:0"),
        )
    }
}
