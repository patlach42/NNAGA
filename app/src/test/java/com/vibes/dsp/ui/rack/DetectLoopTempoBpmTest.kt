package com.vibes.dsp.ui.rack

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

class DetectLoopTempoBpmTest {
    @Test
    fun detects120BpmFromAnExactFourBarDuration() {
        assertBpm(expected = 120.0, durationSeconds = 8.0, referenceBpm = 120.0)
    }

    @Test
    fun considersEveryCandidateBarCount() {
        val cases = listOf(
            CandidateCase(bars = 2, durationSeconds = 4.8, expectedBpm = 100.0),
            CandidateCase(bars = 4, durationSeconds = 9.6, expectedBpm = 100.0),
            CandidateCase(bars = 8, durationSeconds = 19.2, expectedBpm = 100.0),
            CandidateCase(bars = 16, durationSeconds = 38.4, expectedBpm = 100.0),
        )

        cases.forEach { case ->
            assertBpm(
                expected = case.expectedBpm,
                durationSeconds = case.durationSeconds,
                referenceBpm = 100.0,
                message = "bars=${case.bars}",
            )
        }
    }

    @Test
    fun keepsInclusive50And200BpmCandidateBounds() {
        val cases = listOf(
            50.0 to 50.0,
            200.0 to 200.0,
        )

        cases.forEach { (referenceBpm, expectedBpm) ->
            assertBpm(expected = expectedBpm, durationSeconds = 9.6, referenceBpm = referenceBpm)
        }
    }

    @Test
    fun choosesCandidateNearestToReferenceTempo() {
        val cases = listOf(
            80.0 to 100.0,
            170.0 to 200.0,
        )

        cases.forEach { (referenceBpm, expectedBpm) ->
            assertBpm(expected = expectedBpm, durationSeconds = 9.6, referenceBpm = referenceBpm)
        }
    }

    @Test
    fun prefersFewerBarsWhenCandidatesAreEquidistant() {
        // At 9.6 seconds, 4 bars gives 100 BPM and 8 bars gives 200 BPM.
        assertBpm(expected = 100.0, durationSeconds = 9.6, referenceBpm = 150.0)
    }

    @Test
    fun rejectsInvalidDurations() {
        val invalidDurations = listOf(
            0.0,
            -1.0,
            Double.NaN,
            Double.POSITIVE_INFINITY,
            Double.NEGATIVE_INFINITY,
        )

        invalidDurations.forEach { durationSeconds ->
            assertNull(
                "duration=$durationSeconds",
                detectLoopTempoBpm(durationSeconds = durationSeconds, referenceBpm = 120.0),
            )
        }
    }

    @Test
    fun returnsNullWhenDurationProducesNoInRangeCandidate() {
        assertNull(detectLoopTempoBpm(durationSeconds = 100.0, referenceBpm = 120.0))
    }

    @Test
    fun fallsBackTo120ForNonFiniteOrNonPositiveReference() {
        val invalidReferences = listOf(
            0.0,
            -80.0,
            Double.NaN,
            Double.POSITIVE_INFINITY,
            Double.NEGATIVE_INFINITY,
        )

        invalidReferences.forEach { referenceBpm ->
            assertBpm(
                expected = 100.0,
                durationSeconds = 4.8,
                referenceBpm = referenceBpm,
            )
        }
    }

    private fun assertBpm(
        expected: Double,
        durationSeconds: Double,
        referenceBpm: Double,
        message: String = "",
    ) {
        assertEquals(
            message,
            expected,
            detectLoopTempoBpm(durationSeconds = durationSeconds, referenceBpm = referenceBpm) ?: Double.NaN,
            0.0,
        )
    }

    private data class CandidateCase(
        val bars: Int,
        val durationSeconds: Double,
        val expectedBpm: Double,
    )
}
