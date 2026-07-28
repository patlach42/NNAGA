#include <gtest/gtest.h>

#include "UsbScheduling.h"

// The scheduler is pure C++ and does not instantiate Android/libusb.

#include <algorithm>
#include <cstddef>
#include <vector>

namespace {

std::vector<int> schedulerFrames(uint32_t sampleRate, uint32_t packetsPerSecond) {
    monotrypt::usb::RationalPacketScheduler scheduler;
    scheduler.reset(sampleRate, packetsPerSecond);
    std::vector<int> result;
    result.reserve(packetsPerSecond);
    for (uint32_t i = 0; i < packetsPerSecond; ++i)
        result.push_back(static_cast<int>(scheduler.next()));
    return result;
}

void expectExactSchedule(uint32_t sampleRate,
                         uint32_t packetsPerSecond,
                         const char* caseName) {
    SCOPED_TRACE(caseName);
    const auto packets = schedulerFrames(sampleRate, packetsPerSecond);
    ASSERT_EQ(packets.size(), packetsPerSecond);

    const uint32_t baseFrames = sampleRate / packetsPerSecond;
    const uint32_t remainder = sampleRate % packetsPerSecond;
    const uint32_t maxFrames = baseFrames + (remainder != 0 ? 1u : 0u);
    uint64_t cumulative = 0;
    for (std::size_t i = 0; i < packets.size(); ++i) {
        const uint32_t frames = static_cast<uint32_t>(packets[i]);
        EXPECT_GE(frames, baseFrames) << "packet " << i;
        EXPECT_LE(frames, maxFrames) << "packet " << i;
        EXPECT_TRUE(frames == baseFrames || frames == maxFrames)
            << "packet " << i;
        cumulative += frames;
        EXPECT_EQ(cumulative,
                  (static_cast<uint64_t>(i + 1) * sampleRate) /
                      packetsPerSecond)
            << "packet " << i;
    }
    EXPECT_EQ(cumulative, sampleRate);

    if (remainder == 0) {
        EXPECT_EQ(std::count(packets.begin(), packets.end(),
                             static_cast<int>(baseFrames)),
                  packetsPerSecond);
    } else {
        EXPECT_EQ(std::count(packets.begin(), packets.end(),
                             static_cast<int>(baseFrames)),
                  packetsPerSecond - remainder);
        EXPECT_EQ(std::count(packets.begin(), packets.end(),
                             static_cast<int>(maxFrames)),
                  remainder);
    }
}

} // namespace

TEST(UsbPacketSchedule, SupportedRatesAtHighSpeedUseBoundedCadence) {
    constexpr uint32_t kHighSpeedPacketsPerSecond = 8000;
    struct ScheduleCase {
        uint32_t sampleRate;
        const char* name;
    };
    const ScheduleCase cases[] = {
        {44100, "44.1 kHz"},
        {48000, "48 kHz"},
        {88200, "88.2 kHz"},
        {96000, "96 kHz"},
        {176400, "176.4 kHz"},
        {192000, "192 kHz"},
    };

    for (const auto& test : cases)
        expectExactSchedule(test.sampleRate, kHighSpeedPacketsPerSecond,
                            test.name);
}

TEST(UsbPacketSchedule, SupportedRatesAtFullSpeedUseMillisecondCadence) {
    constexpr uint32_t kFullSpeedPacketsPerSecond = 1000;
    struct ScheduleCase {
        uint32_t sampleRate;
        const char* name;
    };
    const ScheduleCase cases[] = {
        {44100, "44.1 kHz"},
        {48000, "48 kHz"},
        {88200, "88.2 kHz"},
        {96000, "96 kHz"},
        {176400, "176.4 kHz"},
        {192000, "192 kHz"},
    };

    for (const auto& test : cases)
        expectExactSchedule(test.sampleRate, kFullSpeedPacketsPerSecond,
                            test.name);
}



TEST(UsbPlaybackWatermark, ClampsGraphQuantumsAndComputesFrameLimits) {
    struct WatermarkCase {
        int request;
        int expectedGraphQuantum;
        int expectedTargetFrames;
        int expectedFrameLimit;
        const char* name;
    };
    const WatermarkCase cases[] = {
        {16, 16, 32, 48, "minimum graph quantum"},
        {64, 64, 128, 192, "64-frame request"},
        {256, 256, 512, 768, "256-frame request"},
        {512, 512, 1024, 1536, "512-frame request"},
        {1024, 1024, 1024, 2048, "maximum graph quantum request"},
        {monotrypt::usb::kMaxGraphQuantum + 1, 1024, 1024, 2048,
         "oversized request"},
        {0, 16, 32, 48, "zero request"},
        {-1, 16, 32, 48, "negative request"},
    };

    for (const auto& test : cases) {
        SCOPED_TRACE(test.name);
        const auto config = monotrypt::usb::playbackWatermarkConfig(test.request);
        EXPECT_EQ(config.graphQuantum, test.expectedGraphQuantum);
        EXPECT_EQ(config.targetFrames, test.expectedTargetFrames);
        EXPECT_EQ(config.frameLimit, test.expectedFrameLimit);
        EXPECT_GE(config.frameLimit, config.graphQuantum);
    }
}

TEST(UsbPlaybackWatermark, EveryWatermarkFitsPhysicalWorstFormatRing) {
    constexpr std::size_t kRingCapacityFrames =
        monotrypt::usb::kPlaybackRingBytes /
        monotrypt::usb::kWorstTransportFrameBytes;
    const int requests[] = {
        monotrypt::usb::kMinGraphQuantum,
        64,
        256,
        512,
        monotrypt::usb::kMaxGraphQuantum,
        monotrypt::usb::kMaxGraphQuantum + 1,
        0,
        -1,
    };

    EXPECT_GT(monotrypt::usb::kWorstTransportFrameBytes, 0);
    EXPECT_EQ(monotrypt::usb::kWorstTransportFrameBytes,
              monotrypt::usb::kMaxTransportChannels *
                  monotrypt::usb::kMaxSubslotBytes);
    EXPECT_EQ(monotrypt::usb::kPlaybackRingBytes %
                  monotrypt::usb::kWorstTransportFrameBytes,
              std::size_t{0});
    EXPECT_EQ(kRingCapacityFrames *
                  monotrypt::usb::kWorstTransportFrameBytes,
              monotrypt::usb::kPlaybackRingBytes);

    for (const int request : requests) {
        SCOPED_TRACE(request);
        const auto config = monotrypt::usb::playbackWatermarkConfig(request);
        const auto frameBytes =
            static_cast<std::size_t>(config.frameLimit) *
            monotrypt::usb::kWorstTransportFrameBytes;
        EXPECT_LE(config.frameLimit, kRingCapacityFrames);
        EXPECT_LE(frameBytes, monotrypt::usb::kPlaybackRingBytes);
        EXPECT_GE(config.frameLimit, config.graphQuantum);
    }
}
