#include <gtest/gtest.h>

#include "UsbScheduling.h"

// The scheduler is pure C++ and does not instantiate Android/libusb.

#include <algorithm>
#include <cstddef>
#include <vector>

#include <limits>

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
TEST(UsbPacketSchedule, NominalTransferFramesUsesCeilingForTransferWindow) {
    struct TransferCase {
        int sampleRate;
        int packetsPerTransfer;
        int packetsPerSecond;
        int expectedFrames;
        const char* name;
    };
    const TransferCase cases[] = {
        {48000, 8, 8000, 48, "48 kHz exact division"},
        {44100, 8, 8000, 45, "44.1 kHz fractional ceiling"},
        {std::numeric_limits<int>::max(), std::numeric_limits<int>::max(),
         std::numeric_limits<int>::max(), std::numeric_limits<int>::max(),
         "large exact division avoids intermediate overflow"},
        {192000, 8, 8000, 192, "192 kHz upper supported rate"},
    };

    for (const auto& test : cases) {
        SCOPED_TRACE(test.name);
        EXPECT_EQ(monotrypt::usb::nominalTransferFrames(
                      test.sampleRate, test.packetsPerTransfer,
                      test.packetsPerSecond),
                  test.expectedFrames);
    }
}

TEST(UsbPacketSchedule, NominalTransferFramesRejectsNonPositiveInputs) {
    struct InvalidCase {
        int sampleRate;
        int packetsPerTransfer;
        int packetsPerSecond;
        const char* name;
    };
    const InvalidCase cases[] = {
        {0, 8, 8000, "zero sample rate"},
        {-1, 8, 8000, "negative sample rate"},
        {48000, 0, 8000, "zero packets per transfer"},
        {48000, -1, 8000, "negative packets per transfer"},
        {48000, 8, 0, "zero packets per second"},
        {48000, 8, -1, "negative packets per second"},
    };

    for (const auto& test : cases) {
        SCOPED_TRACE(test.name);
        EXPECT_EQ(monotrypt::usb::nominalTransferFrames(
                      test.sampleRate, test.packetsPerTransfer,
                      test.packetsPerSecond),
                  0);
    }
}




TEST(UsbPlaybackWatermark, SupportsExactExplicitGraphQuantaAndClampsBounds) {
    struct WatermarkCase {
        int request;
        int expectedGraphQuantum;
        int expectedTargetFrames;
        int expectedFrameLimit;
        const char* name;
    };
    const WatermarkCase cases[] = {
        {4, 4, 12, 16, "4-frame experimental quantum"},
        {6, 6, 18, 24, "6-frame experimental quantum"},
        {8, 8, 24, 32, "8-frame experimental quantum"},
        {12, 12, 36, 48, "12-frame experimental quantum"},
        {16, 16, 48, 64, "16-frame quantum"},
        {24, 24, 72, 96, "24-frame experimental quantum"},
        {32, 32, 96, 128, "32-frame quantum"},
        {48, 48, 144, 192, "48-frame experimental quantum"},
        {64, 64, 192, 256, "64-frame quantum"},
        {72, 72, 216, 288, "72-frame experimental quantum"},
        {96, 96, 288, 384, "96-frame experimental quantum"},
        {128, 128, 384, 512, "128-frame quantum"},
        {256, 256, 768, 1024, "256-frame quantum"},
        {512, 512, 1024, 1536, "512-frame quantum capped at target maximum"},
        {1024, 1024, 1024, 2048, "maximum graph quantum"},
        {0, 4, 12, 16, "zero request clamps to minimum quantum"},
        {-1, 4, 12, 16, "negative request clamps to minimum quantum"},
        {monotrypt::usb::kMaxGraphQuantum + 1, 1024, 1024, 2048,
         "oversized request clamps to maximum quantum"},
    };

    for (const auto& test : cases) {
        SCOPED_TRACE(test.name);
        const auto config = monotrypt::usb::playbackWatermarkConfig(test.request);
        EXPECT_EQ(config.graphQuantum, test.expectedGraphQuantum);
        EXPECT_EQ(config.targetFrames, test.expectedTargetFrames);
        EXPECT_EQ(config.frameLimit, test.expectedFrameLimit);
        EXPECT_EQ(config.targetFrames,
                  std::min(monotrypt::usb::kMaxGraphQuantum,
                           config.graphQuantum * 3));
        EXPECT_EQ(config.frameLimit, config.targetFrames + config.graphQuantum);
        EXPECT_GE(config.frameLimit, config.graphQuantum);
    }
}

TEST(UsbPlaybackWatermark, ExplicitMultiplierControlsTargetAndClampsBounds) {
    struct MultiplierCase {
        int multiplier;
        int expectedTargetFrames;
        int expectedFrameLimit;
        const char* name;
    };
    const MultiplierCase q256Cases[] = {
        {1, 256, 512, "one graph quantum"},
        {2, 512, 768, "two graph quanta"},
        {3, 768, 1024, "three graph quanta"},
        {8, 1024, 1280, "eight graph quanta capped by target maximum"},
    };

    for (const auto& test : q256Cases) {
        SCOPED_TRACE(test.name);
        const auto config =
            monotrypt::usb::playbackWatermarkConfig(256, test.multiplier);
        EXPECT_EQ(config.graphQuantum, 256);
        EXPECT_EQ(config.targetFrames, test.expectedTargetFrames);
        EXPECT_EQ(config.frameLimit, test.expectedFrameLimit);
        EXPECT_EQ(config.frameLimit, config.targetFrames + config.graphQuantum);
    }

    const auto belowMinimum =
        monotrypt::usb::playbackWatermarkConfig(64, 0);
    EXPECT_EQ(belowMinimum.graphQuantum, 64);
    EXPECT_EQ(belowMinimum.targetFrames, 64);
    EXPECT_EQ(belowMinimum.frameLimit, 128);

    const auto aboveMaximum =
        monotrypt::usb::playbackWatermarkConfig(64, 9);
    EXPECT_EQ(aboveMaximum.graphQuantum, 64);
    EXPECT_EQ(aboveMaximum.targetFrames, 512);
    EXPECT_EQ(aboveMaximum.frameLimit, 576);
}
TEST(UsbPlaybackWatermark, ResolvesAutomaticAndManualTargetsWithoutHiddenReserve) {
    struct ResolveCase {
        int automaticTargetFrames;
        int manualTargetFrames;
        int graphQuantum;
        int maxTargetFrames;
        int expected;
        const char* name;
    };
    const ResolveCase cases[] = {
        {144, 0, 16, 4096, 144, "zero manual uses automatic target exactly"},
        {144, -1, 16, 4096, 144, "negative manual uses automatic target exactly"},
        {-20, 0, 16, 4096, 0, "automatic target clamps below zero"},
        {5000, -1, 64, 4096, 4096,
         "automatic target clamps to physical maximum"},
        {512, 144, 16, 4096, 512,
         "stale calibration below automatic target cannot lower safety floor"},
        {512, 144, 64, 4096, 512,
         "stale calibration below automatic target cannot lower safety floor with larger quantum"},
        {512, 1, 16, 4096, 512,
         "positive manual target below automatic target keeps automatic floor"},
        {512, 63, 64, 4096, 512,
         "manual target below automatic target keeps automatic floor"},
        {32, 1, 64, 4096, 64,
         "graph quantum raises a positive manual target below quantum"},
        {512, 768, 16, 4096, 768,
         "manual target above automatic target raises the safety floor"},
        {512, 5000, 16, 4096, 4096,
         "manual target clamps down to physical maximum"},
        {512, 4097, 64, 4096, 4096,
         "graph64 manual target above maximum clamps down"},
        {512, 144, 64, 0, 0, "zero physical capacity disables target"},
        {512, 144, 64, -1, 0, "negative physical capacity disables target"},
    };

    for (const auto& test : cases) {
        SCOPED_TRACE(test.name);
        EXPECT_EQ(monotrypt::usb::resolvedPlaybackTargetFrames(
                      test.automaticTargetFrames, test.manualTargetFrames,
                      test.graphQuantum, test.maxTargetFrames),
                  test.expected);
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
TEST(UsbPlaybackWatermark, EffectiveTargetIncludesQueuedTransferFloor) {
    constexpr int kGraphQuantum = 16;
    constexpr int kInitialTransfers = 4;
    constexpr int kPacketsPerTransfer = 8;
    constexpr int kMaxFramesPerPacket = 7;
    constexpr int kNominalFloor = kMaxFramesPerPacket - 1;
    constexpr int kExactQueuedTransferFrames =
        kInitialTransfers * kPacketsPerTransfer * kNominalFloor;

    const int target = monotrypt::usb::effectivePlaybackTargetFrames(
        kGraphQuantum, kExactQueuedTransferFrames);
    EXPECT_EQ(kExactQueuedTransferFrames, 192);
    EXPECT_EQ(target, 192);
    EXPECT_EQ(target + kGraphQuantum, 208);

    EXPECT_EQ(monotrypt::usb::effectivePlaybackTargetFrames(
                  256, kExactQueuedTransferFrames),
              256);

    struct ClampCase {
        int configuredTarget;
        int queuedTransferFrames;
        int expectedTarget;
        const char* name;
    };
    const ClampCase cases[] = {
        {-1, -1, 0, "both inputs negative"},
        {-1, 0, 0, "negative configured target"},
        {0, -1, 0, "negative queued-transfer floor"},
        {16, -1, 16, "negative floor does not lower configured target"},
        {-1, 192, 192, "positive floor dominates negative target"},
    };

    for (const auto& test : cases) {
        SCOPED_TRACE(test.name);
        EXPECT_EQ(monotrypt::usb::effectivePlaybackTargetFrames(
                      test.configuredTarget, test.queuedTransferFrames),
                  test.expectedTarget);
    }
}
TEST(UsbPlaybackWatermark, ProfileAccountingIncludesOnlyRequiredTransfers) {
    struct AccountingCase {
        int inflightTransfers;
        int reserveTransfers;
        bool exactInFlightAccounting;
        int expected;
        const char* name;
    };
    const AccountingCase cases[] = {
        {4, 3, true, 3,
         "calibrated iD4 profile excludes already-submitted USB transfers"},
        {4, 3, false, 7,
         "generic device includes already-submitted USB transfers"},
    };

    for (const auto& test : cases) {
        SCOPED_TRACE(test.name);
        EXPECT_EQ(monotrypt::usb::playbackWatermarkTransferCount(
                      test.inflightTransfers, test.reserveTransfers,
                      test.exactInFlightAccounting),
                  test.expected);
    }
}

TEST(UsbPlaybackWatermark, TransferAccountingClampsNegativeCountsToZero) {
    struct ClampCase {
        int inflightTransfers;
        int reserveTransfers;
        bool exactInFlightAccounting;
        int expected;
        const char* name;
    };
    const ClampCase cases[] = {
        {0, 0, true, 0, "zero counts"},
        {-4, 7, true, 7, "negative in-flight count"},
        {4, -7, false, 4, "negative reserve count"},
        {-4, -7, false, 0, "both counts negative"},
    };

    for (const auto& test : cases) {
        SCOPED_TRACE(test.name);
        EXPECT_EQ(monotrypt::usb::playbackWatermarkTransferCount(
                      test.inflightTransfers, test.reserveTransfers,
                      test.exactInFlightAccounting),
                  test.expected);
    }
}

TEST(UsbPlaybackWatermark, GenericTransferAccountingSaturatesIntegerOverflow) {
    constexpr int kMax = std::numeric_limits<int>::max();

    EXPECT_EQ(monotrypt::usb::playbackWatermarkTransferCount(kMax, 1, false),
              kMax);
    EXPECT_EQ(monotrypt::usb::playbackWatermarkTransferCount(1, kMax, false),
              kMax);
    EXPECT_EQ(monotrypt::usb::playbackWatermarkTransferCount(kMax, kMax, false),
              kMax);
}

TEST(UsbPacketSchedule, PacketsPerTransferKeepsTransfersNearOneMillisecond) {
    struct RateCase {
        uint32_t packetsPerSecond;
        int expectedPacketsPerTransfer;
        const char* name;
    };
    const RateCase cases[] = {
        {8000, 8, "one packet per 125 microseconds"},
        {4000, 4, "one packet per 250 microseconds"},
        {2000, 2, "one packet per 500 microseconds"},
        {1000, 1, "one packet per millisecond"},
        {500, 1, "sub-millisecond packet rates still use one packet"},
    };

    for (const auto& test : cases) {
        SCOPED_TRACE(test.name);
        EXPECT_EQ(monotrypt::usb::packetsPerTransferForRate(
                      test.packetsPerSecond),
                  test.expectedPacketsPerTransfer);
    }
}
TEST(UsbPacketSchedule, PacketsPerSecondReflectsEndpointInterval) {
    struct IntervalCase {
        bool highSpeed;
        int bInterval;
        int expectedPacketsPerSecond;
        const char* name;
    };
    const IntervalCase cases[] = {
        {true, 1, 8000, "high-speed bInterval 1"},
        {true, 2, 4000, "high-speed bInterval 2"},
        {true, 4, 1000, "high-speed bInterval 4"},
        {false, 1, 1000, "full-speed bInterval 1"},
        {false, 4, 250, "full-speed bInterval 4"},
    };

    for (const auto& test : cases) {
        SCOPED_TRACE(test.name);
        EXPECT_EQ(monotrypt::usb::packetsPerSecondForInterval(
                      test.highSpeed, test.bInterval),
                  test.expectedPacketsPerSecond);
    }
}

TEST(UsbPacketSchedule, EndpointIntervalProducesNonzeroOutTransferBatch) {
    struct IntervalCase {
        bool highSpeed;
        int bInterval;
        int expectedPacketsPerSecond;
        int expectedPacketsPerTransfer;
        const char* name;
    };
    const IntervalCase cases[] = {
        {true, 1, 8000, 8, "high-speed bInterval 1"},
        {true, 2, 4000, 4, "high-speed bInterval 2"},
        {true, 4, 1000, 1, "high-speed bInterval 4"},
        {false, 1, 1000, 1, "full-speed bInterval 1"},
        {false, 4, 250, 1, "full-speed bInterval 4"},
    };

    for (const auto& test : cases) {
        SCOPED_TRACE(test.name);
        const int packetsPerSecond =
            monotrypt::usb::packetsPerSecondForInterval(
                test.highSpeed, test.bInterval);
        const int packetsPerTransfer =
            monotrypt::usb::packetsPerTransferForRate(packetsPerSecond);
        EXPECT_EQ(packetsPerSecond, test.expectedPacketsPerSecond);
        EXPECT_EQ(packetsPerTransfer, test.expectedPacketsPerTransfer);
        EXPECT_GT(packetsPerTransfer, 0);
    }
}
TEST(UsbPlaybackRunway, ConvertsQueuedFramesAtCommonSampleRates) {
    struct RunwayCase {
        uint64_t queuedFrames;
        uint32_t sampleRate;
        uint64_t expectedNanoseconds;
        const char* name;
    };
    const RunwayCase cases[] = {
        {441, 44100, 10'000'000, "10 ms at 44.1 kHz"},
        {480, 48000, 10'000'000, "10 ms at 48 kHz"},
        {1, 44100, 22'675, "one frame at 44.1 kHz rounds down"},
    };

    for (const auto& test : cases) {
        SCOPED_TRACE(test.name);
        EXPECT_EQ(monotrypt::usb::playbackRunwayNanoseconds(
                      test.queuedFrames, test.sampleRate),
                  test.expectedNanoseconds);
    }
}

TEST(UsbPlaybackRunway, RejectsInvalidInputs) {
    EXPECT_EQ(monotrypt::usb::playbackRunwayNanoseconds(0, 44100), 0u);
    EXPECT_EQ(monotrypt::usb::playbackRunwayNanoseconds(480, 0), 0u);
    EXPECT_EQ(monotrypt::usb::playbackRunwayNanoseconds(0, 0), 0u);
}

TEST(UsbPlaybackRunway, PreservesRepresentableResultAndSaturatesOverflow) {
    constexpr uint64_t kMax = std::numeric_limits<uint64_t>::max();
    constexpr uint64_t kNanosecondsPerSecond = 1'000'000'000ULL;
    constexpr uint32_t kLargestSampleRate =
        std::numeric_limits<uint32_t>::max();

    // The intermediate product overflows uint64_t, while the final quotient
    // is representable. This must not be treated as saturation.
    const uint64_t quotient = kMax / kLargestSampleRate;
    const uint64_t remainder = kMax % kLargestSampleRate;
    const uint64_t expected =
        quotient * kNanosecondsPerSecond +
        (remainder * kNanosecondsPerSecond) / kLargestSampleRate;
    EXPECT_LT(expected, kMax);
    EXPECT_EQ(monotrypt::usb::playbackRunwayNanoseconds(
                  kMax, kLargestSampleRate),
              expected);

    // At one frame per second, the exact duration exceeds uint64_t.
    EXPECT_EQ(monotrypt::usb::playbackRunwayNanoseconds(kMax, 1), kMax);
}

TEST(UsbPlaybackPrime, FillsHighWatermarkAndClampsPhysicalCapacity) {
    struct PrimeCase {
        int maxTarget;
        int exactInitialPacketFrames;
        int playbackTargetFrames;
        int graphQuantum;
        int expectedPrimeFrames;
        const char* name;
    };
    const PrimeCase cases[] = {
        {2048, 192, 256, 16, 272,
         "target plus one graph quantum leaves jitter reserve"},
        {2048, 512, 256, 64, 512,
         "exact initial packet coverage wins when larger"},
        {2048, 2000, 2040, 64, 2048,
         "high watermark is clamped to physical capacity"},
        {100, 1, 90, 16, 100,
         "small physical ring clamps high-watermark prime"},
    };

    for (const auto& test : cases) {
        SCOPED_TRACE(test.name);
        EXPECT_EQ(monotrypt::usb::startupPlaybackPrimeFrames(
                      test.maxTarget, test.exactInitialPacketFrames,
                      test.playbackTargetFrames, test.graphQuantum),
                  test.expectedPrimeFrames);
    }
}
