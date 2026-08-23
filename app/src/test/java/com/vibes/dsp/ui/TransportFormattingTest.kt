package com.vibes.dsp.ui

import org.junit.Assert.assertEquals
import org.junit.Test

class TransportFormattingTest {
    @Test
    fun formatMusicalPositionUsesCanonicalQuarterNoteBoundaries() {
        val cases = listOf(
            0.0 to "1:1:1:0",
            (1.0 / 960.0) to "1:1:1:1",
            0.25 to "1:1:2:0",
            1.0 to "1:2:1:0",
            4.0 to "2:1:1:0",
            4.25 to "2:1:2:0",
        )

        cases.forEach { (quarterNotes, expected) ->
            assertEquals(expected, formatMusicalPosition(quarterNotes))
        }
    }

    @Test
    fun interpolatedMusicalQuarterNotesAdvanceAtBpmFromValidPlayingCapture() {
        assertEquals(
            3.0,
            interpolatedMusicalQuarterNotes(
                musicalQuarterNotes = 0.5,
                beatsPerMinute = 120.0,
                playing = true,
                capturedAtMonotonicNanos = 1_000_000_000L,
                nowMonotonicNanos = 2_250_000_000L,
            ),
            0.0,
        )
    }

    @Test
    fun interpolatedMusicalQuarterNotesFreezesPausedInvalidAndNegativeElapsedSnapshots() {
        val paused = interpolatedMusicalQuarterNotes(
            musicalQuarterNotes = 0.5,
            beatsPerMinute = 120.0,
            playing = false,
            capturedAtMonotonicNanos = 1_000_000_000L,
            nowMonotonicNanos = 2_250_000_000L,
        )
        val invalidCapture = interpolatedMusicalQuarterNotes(
            musicalQuarterNotes = 0.5,
            beatsPerMinute = 120.0,
            playing = true,
            capturedAtMonotonicNanos = 0L,
            nowMonotonicNanos = 2_250_000_000L,
        )
        val negativeElapsed = interpolatedMusicalQuarterNotes(
            musicalQuarterNotes = 0.5,
            beatsPerMinute = 120.0,
            playing = true,
            capturedAtMonotonicNanos = 2_000_000_000L,
            nowMonotonicNanos = 1_000_000_000L,
        )

        assertEquals(0.5, paused, 0.0)
        assertEquals(0.5, invalidCapture, 0.0)
        assertEquals(0.5, negativeElapsed, 0.0)
    }

    @Test
    fun interpolatedElapsedSecondsAdvanceOnlyWhilePlayingFromValidCapture() {
        assertEquals(
            3.25,
            interpolatedElapsedSeconds(
                elapsedSeconds = 2.0,
                playing = true,
                capturedAtMonotonicNanos = 1_000_000_000L,
                nowMonotonicNanos = 2_250_000_000L,
            ),
            0.0,
        )
    }

    @Test
    fun interpolatedElapsedSecondsFreezePausedInvalidAndNegativeElapsedSnapshots() {
        val paused = interpolatedElapsedSeconds(
            elapsedSeconds = 2.0,
            playing = false,
            capturedAtMonotonicNanos = 1_000_000_000L,
            nowMonotonicNanos = 2_250_000_000L,
        )
        val invalidCapture = interpolatedElapsedSeconds(
            elapsedSeconds = 2.0,
            playing = true,
            capturedAtMonotonicNanos = 0L,
            nowMonotonicNanos = 2_250_000_000L,
        )
        val negativeElapsed = interpolatedElapsedSeconds(
            elapsedSeconds = 2.0,
            playing = true,
            capturedAtMonotonicNanos = 2_000_000_000L,
            nowMonotonicNanos = 1_000_000_000L,
        )

        assertEquals(2.0, paused, 0.0)
        assertEquals(2.0, invalidCapture, 0.0)
        assertEquals(2.0, negativeElapsed, 0.0)
    }
}
