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

} // namespace guitarrackcraft
