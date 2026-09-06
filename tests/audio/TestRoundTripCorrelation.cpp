#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "engine/RoundTripCorrelation.h"

#include <cstddef>
#include <cstdint>

namespace {

std::vector<float> makeProbe(int frames, uint32_t seed = 0x6d2b79f5u) {
    std::vector<float> probe(static_cast<std::size_t>(frames));
    uint32_t state = seed;
    double mean = 0.0;
    for (float& sample : probe) {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        sample = static_cast<float>(
            (static_cast<double>(state & 0xffffu) / 32767.5) - 1.0);
        mean += sample;
    }
    mean /= static_cast<double>(frames);
    for (float& sample : probe) {
        sample = static_cast<float>(sample - mean);
    }
    return probe;
}

float deterministicNoise(int index, float amplitude) {
    uint32_t state = 0x9e3779b9u + static_cast<uint32_t>(index) * 0x85ebca6bu;
    state ^= state >> 16;
    state *= 0x7feb352du;
    state ^= state >> 15;
    state *= 0x846ca68bu;
    state ^= state >> 16;
    return amplitude * static_cast<float>(
        (static_cast<double>(state & 0xffffu) / 32767.5) - 1.0);
}

std::vector<float> makeCapture(const std::vector<float>& probe,
                               int offset,
                               float scale,
                               float dc = 0.0f,
                               float noise = 0.0f,
                               int trailingFrames = 11) {
    const int frames = offset + static_cast<int>(probe.size()) + trailingFrames;
    std::vector<float> capture(static_cast<std::size_t>(frames));
    for (int i = 0; i < frames; ++i) {
        capture[static_cast<std::size_t>(i)] =
            dc + deterministicNoise(i, noise);
    }
    for (std::size_t i = 0; i < probe.size(); ++i) {
        capture[static_cast<std::size_t>(offset) + i] += scale * probe[i];
    }
    return capture;
}

// One path gives one peak; a path that sums the signal with a delayed copy of
// itself gives two. That second case is what a listener described as the wave
// overlapping itself, and it is invisible to a discontinuity detector because
// the sum is perfectly smooth.
TEST(RoundTripPeaks, SingleEchoYieldsOnePeak) {
    constexpr int kProbe = 64;
    constexpr int kCapture = 512;
    constexpr int kDelay = 100;
    std::vector<float> probe(kProbe), capture(kCapture, 0.0f);
    uint32_t random = 0x9e3779b9u;
    for (float& sample : probe) {
        random ^= random << 13u; random ^= random >> 17u; random ^= random << 5u;
        sample = (random & 1u) != 0u ? 0.25f : -0.25f;
    }
    for (int i = 0; i < kProbe; ++i) capture[kDelay + i] = probe[i];

    const auto profile = guitarrackcraft::analyzeRoundTripPeaks(
        probe.data(), kProbe, capture.data(), kCapture);
    ASSERT_EQ(profile.count, 1);
    EXPECT_EQ(profile.peaks[0].offset, kDelay);
    EXPECT_GT(profile.peaks[0].correlation, 0.9);
}

TEST(RoundTripPeaks, OverlappingCopiesYieldTwoPeaks) {
    constexpr int kProbe = 64;
    constexpr int kCapture = 512;
    constexpr int kFirst = 100;
    constexpr int kSecond = 180;
    std::vector<float> probe(kProbe), capture(kCapture, 0.0f);
    uint32_t random = 0x9e3779b9u;
    for (float& sample : probe) {
        random ^= random << 13u; random ^= random >> 17u; random ^= random << 5u;
        sample = (random & 1u) != 0u ? 0.25f : -0.25f;
    }
    for (int i = 0; i < kProbe; ++i) {
        capture[kFirst + i] += probe[i];
        capture[kSecond + i] += probe[i] * 0.8f;
    }

    const auto profile = guitarrackcraft::analyzeRoundTripPeaks(
        probe.data(), kProbe, capture.data(), kCapture);
    ASSERT_EQ(profile.count, 2) << "an overlapped path must not read as one";
    // The correlation is normalised, so a quieter copy scores as highly as a
    // loud one and the order between them is arbitrary. What matters is that
    // both arrivals are reported.
    bool sawFirst = false, sawSecond = false;
    for (int i = 0; i < profile.count; ++i) {
        if (profile.peaks[i].offset == kFirst) sawFirst = true;
        if (profile.peaks[i].offset == kSecond) sawSecond = true;
        EXPECT_GT(profile.peaks[i].correlation, 0.9);
    }
    EXPECT_TRUE(sawFirst) << "the direct arrival was missed";
    EXPECT_TRUE(sawSecond) << "the delayed copy was missed";
}

// One arrival smeared over a few samples, as any real analogue path smears it,
// must be reported once rather than as a burst of neighbouring arrivals.
TEST(RoundTripPeaks, SmearedSingleArrivalCountsOnce) {
    // A long probe on purpose: correlation of an N-sample random sequence with
    // an unrelated stretch sits around 1/sqrt(N), so a 64-sample probe has a
    // noise floor near 0.125 and a 0.2 threshold would admit noise anywhere.
    // The real probe is 1024 to 2048 samples, where the floor is about 0.022.
    constexpr int kProbe = 512;
    constexpr int kCapture = 2048;
    constexpr int kDelay = 100;
    std::vector<float> probe(kProbe), capture(kCapture, 0.0f);
    uint32_t random = 0x12345678u;
    for (float& sample : probe) {
        random ^= random << 13u; random ^= random >> 17u; random ^= random << 5u;
        sample = (random & 1u) != 0u ? 0.25f : -0.25f;
    }
    for (int i = 0; i < kProbe; ++i) {
        capture[kDelay + i] += probe[i];
        capture[kDelay + i + 1] += probe[i] * 0.5f;
        capture[kDelay + i + 2] += probe[i] * 0.25f;
    }

    // A low threshold admits the shoulders of the arrival, so suppressing the
    // cluster is what keeps this at one. At 0.5 the shoulders fall below the
    // floor on their own and the test would pass without that logic.
    const auto profile = guitarrackcraft::analyzeRoundTripPeaks(
        probe.data(), kProbe, capture.data(), kCapture, 0.2, 16);
    EXPECT_EQ(profile.count, 1)
        << "a smeared arrival was split into " << profile.count << " peaks";
    EXPECT_EQ(profile.peaks[0].offset, kDelay);
}

TEST(RoundTripPeaks, RejectsDegenerateInput) {
    std::vector<float> probe(8, 0.0f), capture(64, 0.0f);
    EXPECT_EQ(guitarrackcraft::analyzeRoundTripPeaks(
        probe.data(), 8, capture.data(), 64).count, 0)
        << "a silent probe has no energy to correlate";
    EXPECT_EQ(guitarrackcraft::analyzeRoundTripPeaks(
        nullptr, 8, capture.data(), 64).count, 0);
    EXPECT_EQ(guitarrackcraft::analyzeRoundTripPeaks(
        probe.data(), 8, capture.data(), 4).count, 0);
}

} // namespace

TEST(RoundTripCorrelationTest, DetectsExactDelayedScaledProbe) {
    const std::vector<float> probe = makeProbe(97);
    constexpr int probeStartFrame = 29;
    constexpr int expectedLatency = 23;
    constexpr int captureOffset = probeStartFrame + expectedLatency;
    const std::vector<float> capture =
        makeCapture(probe, captureOffset, 1.75f);

    const auto result = guitarrackcraft::analyzeRoundTripCorrelation(
        probe.data(), static_cast<int>(probe.size()), capture.data(),
        static_cast<int>(capture.size()), probeStartFrame);

    EXPECT_EQ(result.latencyFrames, expectedLatency);
    EXPECT_NEAR(result.correlation, 1.0, 1.0e-6);
}

TEST(RoundTripCorrelationTest, DetectsInvertedPolarityWithoutChangingLatency) {
    const std::vector<float> probe = makeProbe(97);
    constexpr int probeStartFrame = 17;
    constexpr int expectedLatency = 31;
    constexpr int captureOffset = probeStartFrame + expectedLatency;
    const std::vector<float> capture =
        makeCapture(probe, captureOffset, -0.625f);

    const auto result = guitarrackcraft::analyzeRoundTripCorrelation(
        probe.data(), static_cast<int>(probe.size()), capture.data(),
        static_cast<int>(capture.size()), probeStartFrame);

    EXPECT_EQ(result.latencyFrames, expectedLatency);
    EXPECT_NEAR(result.correlation, 1.0, 1.0e-6);
}

TEST(RoundTripCorrelationTest, FindsDelayedProbeThroughDeterministicNoiseAndDc) {
    const std::vector<float> probe = makeProbe(113);
    constexpr int probeStartFrame = 41;
    constexpr int expectedLatency = 19;
    constexpr int captureOffset = probeStartFrame + expectedLatency;
    const std::vector<float> capture = makeCapture(
        probe, captureOffset, 1.25f, 0.02f, 0.003f, 23);

    const auto result = guitarrackcraft::analyzeRoundTripCorrelation(
        probe.data(), static_cast<int>(probe.size()), capture.data(),
        static_cast<int>(capture.size()), probeStartFrame);

    EXPECT_EQ(result.latencyFrames, expectedLatency);
    EXPECT_GT(result.correlation, 0.99);
}

TEST(RoundTripCorrelationTest, SubtractsRenderedProbeStartFromDetectedOffset) {
    const std::vector<float> probe = makeProbe(83);
    constexpr int probeStartFrame = 137;
    constexpr int expectedLatency = 7;
    constexpr int captureOffset = probeStartFrame + expectedLatency;
    const std::vector<float> capture = makeCapture(probe, captureOffset, 2.0f);

    const auto result = guitarrackcraft::analyzeRoundTripCorrelation(
        probe.data(), static_cast<int>(probe.size()), capture.data(),
        static_cast<int>(capture.size()), probeStartFrame);

    EXPECT_EQ(result.latencyFrames, expectedLatency);
}

TEST(RoundTripCorrelationTest, RejectsInvalidPointersAndFrameRanges) {
    const std::vector<float> probe = makeProbe(9);
    const std::vector<float> capture = makeCapture(probe, 4, 1.0f);
    const float sample = 1.0f;

    const auto expectInvalid = [](const auto result) {
        EXPECT_EQ(result.latencyFrames, -1);
        EXPECT_DOUBLE_EQ(result.correlation, 0.0);
    };

    expectInvalid(guitarrackcraft::analyzeRoundTripCorrelation(
        nullptr, static_cast<int>(probe.size()), capture.data(),
        static_cast<int>(capture.size()), 0));
    expectInvalid(guitarrackcraft::analyzeRoundTripCorrelation(
        probe.data(), static_cast<int>(probe.size()), nullptr,
        static_cast<int>(capture.size()), 0));
    expectInvalid(guitarrackcraft::analyzeRoundTripCorrelation(
        probe.data(), 0, capture.data(), static_cast<int>(capture.size()), 0));
    expectInvalid(guitarrackcraft::analyzeRoundTripCorrelation(
        probe.data(), -1, capture.data(), static_cast<int>(capture.size()), 0));
    expectInvalid(guitarrackcraft::analyzeRoundTripCorrelation(
        probe.data(), static_cast<int>(probe.size()), capture.data(),
        static_cast<int>(probe.size()) - 1, 0));
    expectInvalid(guitarrackcraft::analyzeRoundTripCorrelation(
        probe.data(), static_cast<int>(probe.size()), capture.data(), 0, 0));
    expectInvalid(guitarrackcraft::analyzeRoundTripCorrelation(
        &sample, 1, &sample, 1, -1));
}

TEST(RoundTripCorrelationTest, ZeroEnergyProbeProducesWeakResult) {
    const std::vector<float> probe(64, 0.0f);
    const std::vector<float> capture(128, 0.5f);

    const auto result = guitarrackcraft::analyzeRoundTripCorrelation(
        probe.data(), static_cast<int>(probe.size()), capture.data(),
        static_cast<int>(capture.size()), 0);

    EXPECT_EQ(result.latencyFrames, -1);
    EXPECT_DOUBLE_EQ(result.correlation, 0.0);
}

TEST(RoundTripCorrelationTest, UnrelatedCaptureDoesNotCorrelateStrongly) {
    const std::vector<float> probe = makeProbe(127);
    std::vector<float> capture = makeProbe(127, 0xa5a5a5a5u);
    for (std::size_t i = 0; i < capture.size(); ++i) {
        capture[i] = capture[i] * 0.8f + deterministicNoise(
            static_cast<int>(i), 0.01f);
    }

    const auto result = guitarrackcraft::analyzeRoundTripCorrelation(
        probe.data(), static_cast<int>(probe.size()), capture.data(),
        static_cast<int>(capture.size()), 0);

    EXPECT_LT(result.correlation, 0.6);
}

TEST(RoundTripCorrelationTest, ZeroCaptureRemainsWeak) {
    const std::vector<float> probe = makeProbe(64);
    const std::vector<float> capture(96, 0.0f);

    const auto result = guitarrackcraft::analyzeRoundTripCorrelation(
        probe.data(), static_cast<int>(probe.size()), capture.data(),
        static_cast<int>(capture.size()), 0);

    EXPECT_EQ(result.latencyFrames, -1);
    EXPECT_DOUBLE_EQ(result.correlation, 0.0);
}
