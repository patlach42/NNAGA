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

inline PlaybackWatermarkConfig playbackWatermarkConfig(int requestedFrames) {
    const int quantum = std::clamp(requestedFrames,
                                   kMinGraphQuantum,
                                   kMaxGraphQuantum);
    const int target = std::min(kMaxGraphQuantum, quantum * 2);
    return {quantum, target, quantum + target};
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
