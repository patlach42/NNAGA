#pragma once
#include <cstdint>
#include <algorithm>
#include <cstddef>
#include <limits>

namespace monotrypt::usb {

constexpr int kMinGraphQuantum = 4;
constexpr int kMaxGraphQuantum = 1024;
constexpr std::size_t kPlaybackRingBytes = 1u << 16;
constexpr int kMaxTransportChannels = 8;
constexpr int kMaxSubslotBytes = 4;
constexpr int kWorstTransportFrameBytes =
    kMaxTransportChannels * kMaxSubslotBytes;


struct PlaybackWatermarkConfig {
    int graphQuantum;
    int targetFrames;
    int frameLimit;
};

constexpr int kDefaultPeriodMultiplier = 3;
constexpr int kMinPeriodMultiplier = 1;
constexpr int kMaxPeriodMultiplier = 8;
constexpr int kMinPacketsPerTransfer = 1;
constexpr int kMaxPacketsPerTransfer = 8;

constexpr int packetsPerSecondForInterval(bool highSpeed,
                                          int bInterval) noexcept {
    const int hostPeriods = highSpeed ? 8000 : 1000;
    const int interval = highSpeed
        ? (1 << std::clamp(bInterval - 1, 0, 15))
        : std::max(1, bInterval);
    return std::max(1, hostPeriods / interval);
}

constexpr int packetsPerTransferForRate(int packetsPerSecond) noexcept {
    if (packetsPerSecond >= 8000) return 8;
    if (packetsPerSecond >= 4000) return 4;
    if (packetsPerSecond >= 2000) return 2;
    return 1;
}

constexpr int nominalTransferFrames(int sampleRate, int packetsPerTransfer,
                                    int packetsPerSecond) noexcept {
    if (sampleRate <= 0 || packetsPerTransfer <= 0 || packetsPerSecond <= 0) {
        return 0;
    }
    const int64_t numerator =
        static_cast<int64_t>(sampleRate) * packetsPerTransfer;
    return static_cast<int>(
        std::min<int64_t>(std::numeric_limits<int>::max(),
                          (numerator + packetsPerSecond - 1) /
                              packetsPerSecond));
}

inline int clampPeriodMultiplier(int multiplier) noexcept {
    return std::clamp(multiplier, kMinPeriodMultiplier, kMaxPeriodMultiplier);
}

constexpr int playbackWatermarkTransferCount(
        int inflightTransfers, int reserveTransfers,
        bool exactInFlightAccounting) noexcept {
    const int inflight = std::max(0, inflightTransfers);
    const int reserve = std::max(0, reserveTransfers);
    if (exactInFlightAccounting)
        return reserve;
    return inflight > std::numeric_limits<int>::max() - reserve
        ? std::numeric_limits<int>::max()
        : inflight + reserve;
}

// Keep the requested number of graph quanta queued before admitting one more.
inline PlaybackWatermarkConfig playbackWatermarkConfig(
        int requestedFrames, int periodMultiplier = kDefaultPeriodMultiplier) {
    const int quantum = std::clamp(requestedFrames,
                                   kMinGraphQuantum,
                                   kMaxGraphQuantum);
    const int multiplier = clampPeriodMultiplier(periodMultiplier);
    const int target = std::min(kMaxGraphQuantum, quantum * multiplier);
    return {quantum, target, quantum + target};
}
inline int effectivePlaybackTargetFrames(int configured,
                                         int queuedTransferFrames) noexcept {
    return std::max(0, std::max(configured, queuedTransferFrames));
}

constexpr int resolvedPlaybackTargetFrames(
        int automaticTargetFrames, int manualTargetFrames,
        int graphQuantum, int maxTargetFrames) noexcept {
    const int maximum = std::max(0, maxTargetFrames);
    if (maximum == 0)
        return 0;
    const int automatic =
        std::min(maximum, std::max(0, automaticTargetFrames));
    if (manualTargetFrames <= 0)
        return automatic;
    // Calibration may raise the production runway, but a stale cached result
    // must never undercut a newer automatic safety floor.
    const int minimum = std::min(
        maximum, std::max(std::max(0, graphQuantum), automatic));
    return std::min(maximum, std::max(minimum, manualTargetFrames));
}
constexpr uint64_t playbackRunwayNanoseconds(
        uint64_t queuedFrames, uint32_t sampleRate) noexcept {
    if (queuedFrames == 0 || sampleRate == 0) return 0;
    constexpr uint64_t kNanosecondsPerSecond = 1'000'000'000ULL;
    const uint64_t wholeSeconds = queuedFrames / sampleRate;
    const uint64_t remainderFrames = queuedFrames % sampleRate;
    if (wholeSeconds > std::numeric_limits<uint64_t>::max() /
                           kNanosecondsPerSecond) {
        return std::numeric_limits<uint64_t>::max();
    }
    uint64_t runway = wholeSeconds * kNanosecondsPerSecond;
    const uint64_t remainderNs =
        (remainderFrames * kNanosecondsPerSecond) / sampleRate;
    if (runway > std::numeric_limits<uint64_t>::max() - remainderNs) {
        return std::numeric_limits<uint64_t>::max();
    }
    return runway + remainderNs;
}
constexpr int startupPlaybackPrimeFrames(
        int maxTarget, int exactInitialPacketFrames,
        int playbackTargetFrames, int graphQuantum) noexcept {
    if (maxTarget <= 0) return 0;
    const int target = std::max(0, playbackTargetFrames);
    const int quantum = std::max(0, graphQuantum);
    const int reserve = target > std::numeric_limits<int>::max() - quantum
        ? std::numeric_limits<int>::max() : target + quantum;
    return std::min(maxTarget,
                    std::max(0, std::max(exactInitialPacketFrames, reserve)));
}

// Exact rational packet scheduler. Each next() returns floor((rate + remainder)/period)
// while retaining the remainder, so the long-run sum is exactly rate frames.
class RationalPacketScheduler {
public:
    void reset(uint32_t rate, uint32_t packetsPerSecond) {
        period_ = packetsPerSecond ? packetsPerSecond : 1;
        whole_ = rate / period_;
        fraction_ = rate % period_;
        remainder_ = 0;
    }
    uint32_t next() noexcept {
        uint32_t frames = whole_;
        if (remainder_ >= period_ - fraction_) {
            remainder_ = remainder_ - (period_ - fraction_);
            ++frames;
        } else {
            remainder_ += fraction_;
        }
        return frames;
    }
private:
    uint32_t whole_ = 0;
    uint32_t fraction_ = 0;
    uint32_t period_ = 1;
    uint32_t remainder_ = 0;
};

} // namespace monotrypt::usb
