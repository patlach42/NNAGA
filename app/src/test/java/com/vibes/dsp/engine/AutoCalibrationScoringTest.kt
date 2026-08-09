package com.vibes.dsp.engine

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

class AutoCalibrationScoringTest {
    @Test
    fun standardCalibrationAttemptsAllCandidatesWhenEveryMeasurementFails() {
        val format = DirectUsbFormat(sampleRate = 44_100, bits = 32, subslotBytes = 4)

        val attempted = sequenceDirectUsbCalibrationCandidates(
            formats = listOf(format),
            bufferFrames = listOf(256, 64, 128),
            periodMultipliers = listOf(2, 1)
        ) { _, _ -> false }

        assertEquals(
            listOf(
                DirectUsbCalibrationCandidate(64, 1),
                DirectUsbCalibrationCandidate(64, 2),
                DirectUsbCalibrationCandidate(128, 1),
                DirectUsbCalibrationCandidate(128, 2),
                DirectUsbCalibrationCandidate(256, 1),
                DirectUsbCalibrationCandidate(256, 2)
            ),
            attempted.map { it.second }
        )
    }
    @Test
    fun automaticCalibrationContinuesAfterStableCandidateWhenEarlyStoppingIsDisabled() {
        val stableCandidate = DirectUsbCalibrationCandidate(128, 1)
        val laterCandidates = listOf(
            DirectUsbCalibrationCandidate(128, 2),
            DirectUsbCalibrationCandidate(256, 1)
        )

        assertEquals(
            listOf(true, true),
            laterCandidates.map { candidate ->
                shouldAttemptDirectUsbCalibrationCandidate(
                    candidate = candidate,
                    stableCandidate = stableCandidate,
                    stopAfterFirstStable = false
                )
            }
        )
    }


    @Test
    fun standardCalibrationStopsAfterFirstStableCandidatePerFormat() {
        val firstFormat = DirectUsbFormat(sampleRate = 44_100, bits = 32, subslotBytes = 4)
        val secondFormat = DirectUsbFormat(sampleRate = 48_000, bits = 32, subslotBytes = 4)
        val stableCandidates = mapOf(
            firstFormat to DirectUsbCalibrationCandidate(128, 1),
            secondFormat to DirectUsbCalibrationCandidate(256, 2)
        )

        val attempted = sequenceDirectUsbCalibrationCandidates(
            formats = listOf(firstFormat, secondFormat),
            bufferFrames = listOf(256, 64, 128),
            periodMultipliers = listOf(2, 1)
        ) { format, candidate -> candidate == stableCandidates[format] }

        assertEquals(
            listOf(
                firstFormat to DirectUsbCalibrationCandidate(64, 1),
                firstFormat to DirectUsbCalibrationCandidate(64, 2),
                firstFormat to DirectUsbCalibrationCandidate(128, 1),
                secondFormat to DirectUsbCalibrationCandidate(64, 1),
                secondFormat to DirectUsbCalibrationCandidate(64, 2),
                secondFormat to DirectUsbCalibrationCandidate(128, 1),
                secondFormat to DirectUsbCalibrationCandidate(128, 2),
                secondFormat to DirectUsbCalibrationCandidate(256, 1),
                secondFormat to DirectUsbCalibrationCandidate(256, 2)
            ),
            attempted
        )
    }

    @Test
    fun fixedSampleRateFilterKeepsOnlyAdvertisedFormatsAtRequestedRate() {
        val formats = listOf(
            DirectUsbFormat(sampleRate = 44_100, bits = 32, subslotBytes = 4),
            DirectUsbFormat(sampleRate = 48_000, bits = 24, subslotBytes = 3),
            DirectUsbFormat(sampleRate = 96_000, bits = 32, subslotBytes = 4)
        )

        assertEquals(
            listOf(formats[1]),
            filterDirectUsbFormatsForSampleRate(formats, fixedSampleRate = 48_000)
        )
        assertEquals(
            emptyList<DirectUsbFormat>(),
            filterDirectUsbFormatsForSampleRate(formats, fixedSampleRate = 192_000)
        )
    }



    @Test
    fun assignsFourTwoOnePriorityAndSelectsSevenPointWinner() {
        val profiles = listOf(
            profile(id = "latency-and-buffer", latencyMs = 10.0, bufferFrames = 128),
            profile(id = "latency-only", latencyMs = 10.0, bufferFrames = 256),
            profile(id = "buffer-only", latencyMs = 20.0, bufferFrames = 128),
            profile(id = "baseline", latencyMs = 20.0, bufferFrames = 256)
        )

        assertEquals(
            mapOf(
                "latency-and-buffer" to 7,
                "latency-only" to 6,
                "buffer-only" to 5,
                "baseline" to 4
            ),
            scoreAutoCalibrationProfileList(profiles).associate { it.id to it.score }
        )
        assertEquals("latency-and-buffer", scoreAutoCalibrationProfiles(profiles)?.id)
    }

    @Test
    fun givesLatencyBonusToEveryLatencyTie() {
        val profiles = listOf(
            profile(id = "latency-tie-large-buffer", latencyMs = 8.0, bufferFrames = 256),
            profile(id = "latency-tie-small-buffer", latencyMs = 8.0, bufferFrames = 128),
            profile(id = "buffer-only", latencyMs = 16.0, bufferFrames = 64)
        )

        assertEquals(
            mapOf(
                "latency-tie-large-buffer" to 6,
                "latency-tie-small-buffer" to 6,
                "buffer-only" to 5
            ),
            scoreAutoCalibrationProfileList(profiles).associate { it.id to it.score }
        )
        assertEquals("latency-tie-small-buffer", scoreAutoCalibrationProfiles(profiles)?.id)
    }

    @Test
    fun givesBufferBonusToEveryBufferTie() {
        val profiles = listOf(
            profile(id = "buffer-tie-slower-first", latencyMs = 16.0, bufferFrames = 128),
            profile(id = "buffer-tie-latency-winner", latencyMs = 8.0, bufferFrames = 128),
            profile(id = "baseline", latencyMs = 16.0, bufferFrames = 256)
        )

        assertEquals(
            mapOf(
                "buffer-tie-slower-first" to 5,
                "buffer-tie-latency-winner" to 7,
                "baseline" to 4
            ),
            scoreAutoCalibrationProfileList(profiles).associate { it.id to it.score }
        )
        assertEquals("buffer-tie-latency-winner", scoreAutoCalibrationProfiles(profiles)?.id)
    }

    @Test
    fun excludesUnstableCandidatesAndReturnsNullWhenNoneAreStable() {
        val unstable = profile(
            id = "unstable-fast",
            latencyMs = 1.0,
            bufferFrames = 1,
            stable = false
        )
        val profiles = listOf(unstable, profile(id = "stable", latencyMs = 20.0, bufferFrames = 256))

        assertEquals(
            mapOf("unstable-fast" to 0, "stable" to 7),
            scoreAutoCalibrationProfileList(profiles).associate { it.id to it.score }
        )
        assertEquals("stable", scoreAutoCalibrationProfiles(profiles)?.id)
        assertNull(scoreAutoCalibrationProfiles(listOf(unstable)))
    }

    @Test
    fun breaksEqualScoresByBufferThenId() {
        val lowerBuffer = scoreAutoCalibrationProfiles(
            listOf(
                profile(id = "larger-buffer", latencyMs = 8.0, bufferFrames = 512),
                profile(id = "smaller-buffer", latencyMs = 8.0, bufferFrames = 256),
                profile(id = "buffer-minimum", latencyMs = 16.0, bufferFrames = 64)
            )
        )
        assertEquals("smaller-buffer", lowerBuffer?.id)
        assertEquals(6, lowerBuffer?.score)

        val lowerId = scoreAutoCalibrationProfiles(
            listOf(
                profile(id = "z-profile", latencyMs = 8.0, bufferFrames = 256),
                profile(id = "a-profile", latencyMs = 8.0, bufferFrames = 256),
                profile(id = "buffer-minimum", latencyMs = 16.0, bufferFrames = 64)
            )
        )
        assertEquals("a-profile", lowerId?.id)
        assertEquals(6, lowerId?.score)
    }

    private fun profile(
        id: String,
        latencyMs: Double,
        bufferFrames: Int,
        stable: Boolean = true
    ): DirectUsbCalibrationProfile = DirectUsbCalibrationProfile(
        id = id,
        format = DirectUsbFormat(sampleRate = 48_000, bits = 32, subslotBytes = 4),
        bufferFrames = bufferFrames,
        periodMultiplier = 1,
        bufferConfig = DirectUsbBufferConfig(
            playbackTargetFrames = bufferFrames,
            startupPrimeFrames = bufferFrames,
            writeHeadroomFrames = bufferFrames,
            captureLimitFrames = bufferFrames,
            transferCount = 1,
            packetsPerTransfer = 1,
            ringCapacityBytes = 1
        ),
        experimental = false,
        ranAtEpochMs = 0L,
        started = true,
        stable = stable,
        failure = if (stable) null else "unstable",
        latencyFrames = latencyMs.toLong(),
        latencyMilliseconds = latencyMs,
        xruns = 0L,
        deadlineMisses = 0L,
        transferErrors = 0L,
        autoGenerated = true,
        attemptedRuns = 5,
        successfulRuns = if (stable) 5 else 5
    )
}
