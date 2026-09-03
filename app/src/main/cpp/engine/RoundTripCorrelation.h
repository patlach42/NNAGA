#pragma once

#include <algorithm>
#include <cmath>

namespace guitarrackcraft {

struct RoundTripCorrelationResult {
    int latencyFrames = -1;
    double correlation = 0.0;
};

inline RoundTripCorrelationResult analyzeRoundTripCorrelation(
        const float* probe,
        int probeFrames,
        const float* capture,
        int captureFrames,
        int probeStartFrame) noexcept {
    RoundTripCorrelationResult result;
    if (!probe || !capture || probeFrames <= 0 ||
        captureFrames < probeFrames || probeStartFrame < 0) {
        return result;
    }

    double probeEnergy = 0.0;
    for (int i = 0; i < probeFrames; ++i) {
        const double sample = probe[i];
        probeEnergy += sample * sample;
    }
    if (probeEnergy <= 0.0) return result;

    int bestOffset = -1;
    double bestCorrelation = 0.0;
    for (int offset = 0; offset + probeFrames <= captureFrames; ++offset) {
        double dot = 0.0;
        double captureEnergy = 0.0;
        for (int i = 0; i < probeFrames; ++i) {
            const double expected = probe[i];
            const double observed = capture[offset + i];
            dot += expected * observed;
            captureEnergy += observed * observed;
        }
        const double denominator = std::sqrt(probeEnergy * captureEnergy);
        const double correlation = denominator > 0.0
            ? std::fabs(dot / denominator)
            : 0.0;
        if (correlation > bestCorrelation) {
            bestCorrelation = correlation;
            bestOffset = offset;
        }
    }

    if (bestOffset >= probeStartFrame) {
        result.latencyFrames = bestOffset - probeStartFrame;
        result.correlation = bestCorrelation;
    }
    return result;
}

// A single latency figure answers "how far away is the echo" but not "how many
// echoes are there". A path that sums the signal with a delayed copy of itself
// - the wave overlapping itself, as a listener described it - is perfectly
// smooth, so a discontinuity detector cannot see it, and the best-peak search
// above reports only the strongest copy.
//
// The probe is a pseudorandom binary sequence, so its autocorrelation is a
// single sharp spike: one peak means one path, and every additional peak is
// another copy arriving at another delay. Do not use this with a periodic
// probe, which correlates at every period and would show peaks that mean
// nothing.
struct RoundTripPeak {
    int offset = -1;
    double correlation = 0.0;
};

struct RoundTripPeakProfile {
    static constexpr int kMaxPeaks = 8;
    int count = 0;
    RoundTripPeak peaks[kMaxPeaks];
};

// Correlation of the probe against the capture at one offset.
inline double roundTripCorrelationAt(
        const float* probe, int probeFrames, double probeEnergy,
        const float* capture, int offset) noexcept {
    double dot = 0.0;
    double captureEnergy = 0.0;
    for (int i = 0; i < probeFrames; ++i) {
        const double expected = probe[i];
        const double observed = capture[offset + i];
        dot += expected * observed;
        captureEnergy += observed * observed;
    }
    const double denominator = std::sqrt(probeEnergy * captureEnergy);
    return denominator > 0.0 ? std::fabs(dot / denominator) : 0.0;
}

// Peaks at or above `relativeThreshold` of the strongest, separated by at least
// `minSeparationFrames` so the shoulders of one peak are not counted twice.
// Control-thread only: two passes over the capture, allocation-free but not
// cheap. Returns peaks strongest first.
inline RoundTripPeakProfile analyzeRoundTripPeaks(
        const float* probe,
        int probeFrames,
        const float* capture,
        int captureFrames,
        double relativeThreshold = 0.5,
        int minSeparationFrames = 16) noexcept {
    RoundTripPeakProfile profile;
    if (!probe || !capture || probeFrames <= 0 ||
        captureFrames < probeFrames || minSeparationFrames < 1) {
        return profile;
    }

    double probeEnergy = 0.0;
    for (int i = 0; i < probeFrames; ++i) {
        const double sample = probe[i];
        probeEnergy += sample * sample;
    }
    if (probeEnergy <= 0.0) return profile;

    const int lastOffset = captureFrames - probeFrames;
    double best = 0.0;
    for (int offset = 0; offset <= lastOffset; ++offset) {
        const double correlation = roundTripCorrelationAt(
            probe, probeFrames, probeEnergy, capture, offset);
        if (correlation > best) best = correlation;
    }
    if (best <= 0.0) return profile;

    const double floor = best * (relativeThreshold > 0.0 ? relativeThreshold : 0.0);
    for (int offset = 0; offset <= lastOffset; ++offset) {
        const double correlation = roundTripCorrelationAt(
            probe, probeFrames, probeEnergy, capture, offset);
        if (correlation < floor) continue;

        // Keep the strongest point of each cluster: replace a recorded peak
        // that this one is close to and stronger than, otherwise append.
        int nearest = -1;
        for (int i = 0; i < profile.count; ++i) {
            const int distance = offset - profile.peaks[i].offset;
            if ((distance < 0 ? -distance : distance) < minSeparationFrames) {
                nearest = i;
                break;
            }
        }
        if (nearest >= 0) {
            if (correlation > profile.peaks[nearest].correlation) {
                profile.peaks[nearest].offset = offset;
                profile.peaks[nearest].correlation = correlation;
            }
            continue;
        }
        if (profile.count < RoundTripPeakProfile::kMaxPeaks) {
            profile.peaks[profile.count].offset = offset;
            profile.peaks[profile.count].correlation = correlation;
            ++profile.count;
        } else {
            int weakest = 0;
            for (int i = 1; i < profile.count; ++i) {
                if (profile.peaks[i].correlation <
                    profile.peaks[weakest].correlation) {
                    weakest = i;
                }
            }
            if (correlation > profile.peaks[weakest].correlation) {
                profile.peaks[weakest].offset = offset;
                profile.peaks[weakest].correlation = correlation;
            }
        }
    }

    for (int i = 1; i < profile.count; ++i) {
        RoundTripPeak key = profile.peaks[i];
        int j = i - 1;
        while (j >= 0 && profile.peaks[j].correlation < key.correlation) {
            profile.peaks[j + 1] = profile.peaks[j];
            --j;
        }
        profile.peaks[j + 1] = key;
    }
    return profile;
}

} // namespace guitarrackcraft
