#pragma once
#include <cstdint>
#include <algorithm>
#include <cstddef>

namespace monotrypt::usb {

constexpr int kMinGraphQuantum = 16;
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

inline int clampPeriodMultiplier(int multiplier) noexcept {
    return std::clamp(multiplier, kMinPeriodMultiplier, kMaxPeriodMultiplier);
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
inline int effectivePlaybackPrimeFrames(int configured, int exact) noexcept {
    return std::max(0, std::max(configured, exact));
}

// Exact rational packet scheduler. Each next() returns floor((rate + remainder)/period)
// while retaining the remainder, so the long-run sum is exactly rate frames.
class RationalPacketScheduler {
public:
    void reset(uint32_t rate, uint32_t packetsPerSecond) {
        rate_ = rate;
        period_ = packetsPerSecond ? packetsPerSecond : 1;
        remainder_ = 0;
    }
    uint32_t next() {
        const uint64_t total = static_cast<uint64_t>(remainder_) + rate_;
        const uint32_t frames = static_cast<uint32_t>(total / period_);
        remainder_ = static_cast<uint32_t>(total % period_);
        return frames;
    }
private:
    uint32_t rate_ = 0;
    uint32_t period_ = 1;
    uint32_t remainder_ = 0;
};

} // namespace monotrypt::usb
