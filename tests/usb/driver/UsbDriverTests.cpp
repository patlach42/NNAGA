#include <gtest/gtest.h>

#include "libusb_uac_driver.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace monotrypt::usb {

// The production class exposes only public lifecycle/PCM APIs. This narrow
// friend keeps callback/ring assertions on the actual implementation without
// duplicating its state machine in a test model.
struct UsbDriverTestAccess {
    static void playbackFormat(LibusbUacDriver& d, int channels, int bytes) {
        d.format_.channels = channels;
        d.format_.bytesPerSample = bytes;
    }
    static void captureFormat(LibusbUacDriver& d, int channels, int bytes,
                              bool implicit = false) {
        d.captureFormat_.channels = channels;
        d.captureFormat_.bytesPerSample = bytes;
        d.captureFormat_.implicitFeedback = implicit;
    }
    static void captureActive(LibusbUacDriver& d, bool active) {
        d.captureActive_.store(active, std::memory_order_release);
    }
    static void streaming(LibusbUacDriver& d, bool active) {
        d.streaming_.store(active, std::memory_order_release);
    }
    static void playbackStarted(LibusbUacDriver& d, bool started) {
        d.playbackStarted_.store(started, std::memory_order_release);
    }
    static void stopRequested(LibusbUacDriver& d, bool requested) {
        d.stopRequested_.store(requested, std::memory_order_release);
    }
    static void captureInflight(LibusbUacDriver& d, int count) {
        d.captureInflight_.store(count, std::memory_order_release);
    }
    static void playbackInflight(LibusbUacDriver& d, int count) {
        d.inflight_.store(count, std::memory_order_release);
    }
    static void captureCursors(LibusbUacDriver& d, size_t head, size_t tail) {
        d.captureHead_.store(head, std::memory_order_release);
        d.captureTail_.store(tail, std::memory_order_release);
    }
    static void playbackCursors(LibusbUacDriver& d, size_t head, size_t tail) {
        d.ringHead_.store(head, std::memory_order_release);
        d.ringTail_.store(tail, std::memory_order_release);
    }
    static void implicitCursors(LibusbUacDriver& d, size_t write, size_t read) {
        d.implicitWrite_.store(write, std::memory_order_release);
        d.implicitRead_.store(read, std::memory_order_release);
    }
    static void pending(LibusbUacDriver& d, libusb_transfer* xfr) {
        d.pendingImplicitTransfers_[0] = xfr;
        d.pendingImplicitCount_ = 1;
    }
    static int drain(LibusbUacDriver& d, uint8_t* dst, int bytes) {
        return d.drainRing(dst, bytes);
    }
    static size_t pendingCount(const LibusbUacDriver& d) {
        return d.pendingImplicitCount_;
    }
    static void onCapture(LibusbUacDriver& d, libusb_transfer* xfr) { d.onCapture(xfr); }
    static void onIso(LibusbUacDriver& d, libusb_transfer* xfr) { d.onIso(xfr); }
    static void feedbackState(LibusbUacDriver& d, int interval, int maxFrames) {
        d.packetIntervalUframes_ = interval;
        d.maxFramesPerPacket_ = maxFrames;
    }
    static uint32_t feedbackRate(const LibusbUacDriver& d) {
        return d.framesPerUframe_q16_.load(std::memory_order_acquire);
    }
    static void onFeedback(LibusbUacDriver& d, libusb_transfer* xfr) {
        d.onFeedback(xfr);
    }
    static void submitPending(LibusbUacDriver& d) { d.submitPendingImplicitTransfers(); }
    static void setRingBytes(LibusbUacDriver& d, const std::vector<uint8_t>& bytes) {
        d.ring_ = bytes;
    }
    static bool stopRequested(const LibusbUacDriver& d) {
        return d.stopRequested_.load(std::memory_order_acquire);
    }
    static int inflight(const LibusbUacDriver& d) {
        return d.inflight_.load(std::memory_order_acquire);
    }
};

} // namespace monotrypt::usb

namespace {

extern "C" void usb_driver_mock_set_submit_result(int result);
extern "C" int usb_driver_mock_submit_calls();
extern "C" void usb_driver_mock_reset();

libusb_transfer* makeTransfer(std::vector<uint8_t>& payload, int packets = 1) {
    auto* xfr = libusb_alloc_transfer(packets);
    EXPECT_NE(xfr, nullptr);
    if (!xfr) return nullptr;
    xfr->buffer = payload.data();
    xfr->status = LIBUSB_TRANSFER_COMPLETED;
    xfr->num_iso_packets = packets;
    for (int i = 0; i < packets; ++i) {
        xfr->iso_packet_desc[i].status = LIBUSB_TRANSFER_COMPLETED;
        xfr->iso_packet_desc[i].length = static_cast<unsigned int>(payload.size() / packets);
        xfr->iso_packet_desc[i].actual_length =
            static_cast<int>(payload.size() / packets);
    }
    return xfr;
}
void resetMock() {
    usb_driver_mock_reset();
}

} // namespace

TEST(UsbDriverLifecycle, StartWithoutDeviceReportsNoDeviceAndStopIsIdempotent) {
    monotrypt::usb::LibusbUacDriver driver;

    EXPECT_FALSE(driver.start(48000, 24, 2, 3));
    EXPECT_EQ(driver.lastError(), monotrypt::usb::StartError::NoDevice);
    EXPECT_NE(driver.lastErrorDetail().find("before open"), std::string::npos);
    EXPECT_FALSE(driver.isStreaming());

    driver.stop();
    driver.stop();
    EXPECT_FALSE(driver.isStreaming());
}

TEST(UsbDriverRing, WriteAndDrainPreserveWholeFramesAcrossWrap) {
    monotrypt::usb::LibusbUacDriver driver;
    monotrypt::usb::UsbDriverTestAccess::playbackFormat(driver, 2, 2);
    monotrypt::usb::UsbDriverTestAccess::playbackCursors(
        driver, monotrypt::usb::kPlaybackRingBytes - 4, monotrypt::usb::kPlaybackRingBytes - 4);
    driver.setGraphQuantum(64);

    std::vector<uint8_t> input(4 * 4);
    for (size_t i = 0; i < input.size(); ++i) input[i] = static_cast<uint8_t>(i + 1);
    ASSERT_EQ(driver.writePcm(input.data(), 4), 4);
    ASSERT_EQ(driver.bufferedFrames(), 4);

    std::vector<uint8_t> output(input.size(), 0);
    monotrypt::usb::UsbDriverTestAccess::playbackStarted(driver, true);
    ASSERT_EQ(monotrypt::usb::UsbDriverTestAccess::drain(
                  driver, output.data(), static_cast<int>(output.size())),
              static_cast<int>(output.size()));

    EXPECT_EQ(output, input);
    EXPECT_EQ(driver.bufferedFrames(), 0);
    EXPECT_EQ(driver.playedFrames(), 4);
}

TEST(UsbDriverRing, WatermarkRejectsPartialFrameAndCoalescesOverrun) {
    monotrypt::usb::LibusbUacDriver driver;
    monotrypt::usb::UsbDriverTestAccess::playbackFormat(driver, 2, 2);
    driver.setGraphQuantum(16);
    std::vector<uint8_t> input(49 * 4, 0xA5);

    EXPECT_EQ(driver.writePcm(input.data(), 49), 48);
    EXPECT_EQ(driver.bufferedFrames(), 48);
    EXPECT_EQ(driver.writableFrames(), 0);
    EXPECT_EQ(driver.playbackXRunCount(), 1u);
    EXPECT_EQ(driver.writePcm(input.data(), 1), 0);
    EXPECT_EQ(driver.playbackXRunCount(), 1u);
}

TEST(UsbDriverCapture, InactiveCaptureGatesReadsAndStaleCompletion) {
    resetMock();
    monotrypt::usb::LibusbUacDriver driver;

    monotrypt::usb::UsbDriverTestAccess::captureFormat(driver, 2, 2);
    std::vector<uint8_t> payload(8, 0x37);
    auto* xfr = makeTransfer(payload);
    ASSERT_NE(xfr, nullptr);
    monotrypt::usb::UsbDriverTestAccess::captureInflight(driver, 1);

    // A completion racing with stop must not make stale frames visible.
    monotrypt::usb::UsbDriverTestAccess::captureActive(driver, false);
    monotrypt::usb::UsbDriverTestAccess::onCapture(driver, xfr);
    EXPECT_EQ(driver.captureAvailableFrames(), 0);
    EXPECT_EQ(driver.captureSequence(), 0u);

    monotrypt::usb::UsbDriverTestAccess::captureActive(driver, true);
    std::vector<uint8_t> out(payload.size());
    EXPECT_EQ(driver.readCapturePcm(out.data(), 2), 0);
    EXPECT_EQ(driver.captureStats().underruns, 1u);
    monotrypt::usb::UsbDriverTestAccess::captureActive(driver, false);
    EXPECT_EQ(driver.readCapturePcm(out.data(), 2), 0);
    EXPECT_EQ(driver.captureStats().underruns, 1u);
    libusb_free_transfer(xfr);
}

TEST(UsbDriverFeedback, HighSpeedFeedbackScalesMicroframeRateToPacketRate) {
    resetMock();
    monotrypt::usb::LibusbUacDriver driver;
    monotrypt::usb::UsbDriverTestAccess::playbackFormat(driver, 1, 2);
    monotrypt::usb::UsbDriverTestAccess::feedbackState(driver, 8, 64);

    std::vector<uint8_t> feedbackPayload{0x00, 0x00, 0x06, 0x00};
    auto* feedback = makeTransfer(feedbackPayload);
    ASSERT_NE(feedback, nullptr);
    monotrypt::usb::UsbDriverTestAccess::playbackInflight(driver, 1);
    monotrypt::usb::UsbDriverTestAccess::onFeedback(driver, feedback);
    EXPECT_EQ(monotrypt::usb::UsbDriverTestAccess::feedbackRate(driver),
              static_cast<uint32_t>(48u << 16));

    std::vector<uint8_t> packet(96, 0);
    auto* xfr = makeTransfer(packet);
    ASSERT_NE(xfr, nullptr);
    monotrypt::usb::UsbDriverTestAccess::playbackInflight(driver, 1);
    monotrypt::usb::UsbDriverTestAccess::onIso(driver, xfr);
    EXPECT_EQ(xfr->iso_packet_desc[0].length, 96u);
    EXPECT_EQ(driver.playedFrames(), 48);

    libusb_free_transfer(feedback);
    libusb_free_transfer(xfr);
}

TEST(UsbDriverCapture, CallbackWrapsFramesAndCountsOverflow) {
    resetMock();
    monotrypt::usb::LibusbUacDriver driver;
    monotrypt::usb::UsbDriverTestAccess::captureFormat(driver, 2, 2);
    constexpr size_t capacity = monotrypt::usb::kPlaybackRingBytes;
    monotrypt::usb::UsbDriverTestAccess::captureCursors(driver, capacity - 4, capacity - 4);
    monotrypt::usb::UsbDriverTestAccess::captureActive(driver, true);

    std::vector<uint8_t> payload{1, 2, 3, 4, 5, 6, 7, 8};
    auto* xfr = makeTransfer(payload);
    ASSERT_NE(xfr, nullptr);
    monotrypt::usb::UsbDriverTestAccess::captureInflight(driver, 1);
    monotrypt::usb::UsbDriverTestAccess::onCapture(driver, xfr);
    std::vector<uint8_t> out(payload.size());
    ASSERT_EQ(driver.readCapturePcm(out.data(), 2), 2);
    EXPECT_EQ(out, payload);
    EXPECT_EQ(driver.captureSequence(), 2u);

    // Leave only one frame of physical room; the second frame is dropped as
    // a whole frame and the production overrun counter records the event.
    monotrypt::usb::UsbDriverTestAccess::captureCursors(driver, capacity - 4, 0);
    monotrypt::usb::UsbDriverTestAccess::captureInflight(driver, 1);
    monotrypt::usb::UsbDriverTestAccess::onCapture(driver, xfr);
    EXPECT_EQ(driver.captureStats().overruns, 1u);
    libusb_free_transfer(xfr);
}

TEST(UsbDriverCapture, DiscardFramesPreservesNewestOrderWithoutUnderrun) {
    resetMock();
    monotrypt::usb::LibusbUacDriver driver;
    monotrypt::usb::UsbDriverTestAccess::captureFormat(driver, 1, 2);
    monotrypt::usb::UsbDriverTestAccess::captureActive(driver, true);
    driver.setGraphQuantum(16);

    std::vector<uint8_t> payload(2);
    auto* xfr = makeTransfer(payload);
    ASSERT_NE(xfr, nullptr);
    for (int i = 0; i < 5; ++i) {
        payload[0] = static_cast<uint8_t>(0x20 + i);
        payload[1] = static_cast<uint8_t>(0xA0 + i);
        monotrypt::usb::UsbDriverTestAccess::captureInflight(driver, 1);
        monotrypt::usb::UsbDriverTestAccess::onCapture(driver, xfr);
    }

    EXPECT_EQ(driver.captureAvailableFrames(), 5);
    const auto before = driver.captureStats();
    EXPECT_EQ(driver.discardCaptureFrames(2), 2);
    EXPECT_EQ(driver.captureAvailableFrames(), 3);
    EXPECT_EQ(driver.captureStats().underruns, before.underruns);

    std::vector<uint8_t> out(6);
    ASSERT_EQ(driver.readCapturePcm(out.data(), 3), 3);
    EXPECT_EQ(out, (std::vector<uint8_t>{0x22, 0xA2, 0x23, 0xA3, 0x24, 0xA4}));
    EXPECT_EQ(driver.captureStats().underruns, before.underruns);
    libusb_free_transfer(xfr);
}

TEST(UsbDriverCapture, ImplicitMetadataFifoResynchronizesAfterSaturation) {
    resetMock();
    monotrypt::usb::LibusbUacDriver driver;
    monotrypt::usb::UsbDriverTestAccess::playbackFormat(driver, 1, 4);
    monotrypt::usb::UsbDriverTestAccess::captureFormat(driver, 1, 4, true);
    monotrypt::usb::UsbDriverTestAccess::captureActive(driver, true);
    driver.setGraphQuantum(16);

    std::vector<uint8_t> payload(4, 0);
    auto* xfr = makeTransfer(payload);
    ASSERT_NE(xfr, nullptr);
    for (int i = 0; i < 257; ++i) {
        payload[0] = static_cast<uint8_t>(i & 0xFF);
        payload[1] = static_cast<uint8_t>((i >> 8) & 0xFF);
        payload[2] = 0xA5;
        payload[3] = 0x5A;
        monotrypt::usb::UsbDriverTestAccess::captureInflight(driver, 1);
        monotrypt::usb::UsbDriverTestAccess::onCapture(driver, xfr);
    }

    const auto stats = driver.implicitFeedbackStats();
    EXPECT_EQ(stats.fifoDepth, 1u);
    EXPECT_EQ(stats.fallbackPackets, 0u);
    EXPECT_EQ(driver.captureStats().overruns, 1u);
    EXPECT_EQ(driver.captureAvailableFrames(), 32);

    std::vector<uint8_t> output(257 * 4, 0);
    ASSERT_EQ(driver.readCapturePcm(output.data(), 257), 32);
    // The bounded read exposes the newest 32 frames (225..256), not the
    // oldest physical backlog.
    EXPECT_EQ(output[0], 225);
    EXPECT_EQ(output[1], 0);
    constexpr size_t last = 31 * 4;
    EXPECT_EQ(output[last], 0);
    EXPECT_EQ(output[last + 1], 1);
    EXPECT_EQ(driver.captureAvailableFrames(), 0);
    libusb_free_transfer(xfr);
}

TEST(UsbDriverLifecycle, CaptureResubmitFailureTerminatesPump) {
    resetMock();
    usb_driver_mock_set_submit_result(LIBUSB_ERROR_IO);
    monotrypt::usb::LibusbUacDriver driver;
    monotrypt::usb::UsbDriverTestAccess::captureFormat(driver, 1, 2);
    monotrypt::usb::UsbDriverTestAccess::captureActive(driver, true);
    monotrypt::usb::UsbDriverTestAccess::streaming(driver, true);
    monotrypt::usb::UsbDriverTestAccess::captureInflight(driver, 1);
    std::vector<uint8_t> payload(2, 0x11);
    auto* xfr = makeTransfer(payload);
    ASSERT_NE(xfr, nullptr);

    monotrypt::usb::UsbDriverTestAccess::onCapture(driver, xfr);

    EXPECT_EQ(driver.implicitFeedbackStats().captureTransferErrors, 1u);
    EXPECT_FALSE(driver.isStreaming());
    EXPECT_FALSE(driver.captureAvailableFrames() > 0);
    EXPECT_EQ(usb_driver_mock_submit_calls(), 1);
    libusb_free_transfer(xfr);
}

TEST(UsbDriverLifecycle, PendingImplicitSubmitFailureRetainsOwnershipAndStops) {
    resetMock();
    usb_driver_mock_set_submit_result(LIBUSB_ERROR_IO);
    monotrypt::usb::LibusbUacDriver driver;
    monotrypt::usb::UsbDriverTestAccess::playbackFormat(driver, 1, 2);
    monotrypt::usb::UsbDriverTestAccess::captureFormat(driver, 1, 2, true);
    monotrypt::usb::UsbDriverTestAccess::captureActive(driver, true);
    monotrypt::usb::UsbDriverTestAccess::streaming(driver, true);
    monotrypt::usb::UsbDriverTestAccess::implicitCursors(driver, 0, 0);
    std::vector<uint8_t> payload(2, 0x22);
    auto* xfr = makeTransfer(payload);
    ASSERT_NE(xfr, nullptr);
    monotrypt::usb::UsbDriverTestAccess::pending(driver, xfr);

    // One metadata entry lets prepareImplicitTransfer consume the packet;
    // submission then fails at the mocked libusb boundary.
    monotrypt::usb::UsbDriverTestAccess::implicitCursors(driver, 1, 0);
    monotrypt::usb::UsbDriverTestAccess::submitPending(driver);

    EXPECT_EQ(driver.implicitFeedbackStats().playbackTransferErrors, 1u);
    EXPECT_FALSE(driver.isStreaming());
    EXPECT_TRUE(monotrypt::usb::UsbDriverTestAccess::stopRequested(driver));
    EXPECT_EQ(monotrypt::usb::UsbDriverTestAccess::inflight(driver), 0);
    EXPECT_EQ(monotrypt::usb::UsbDriverTestAccess::pendingCount(driver), 1u);
    libusb_free_transfer(xfr);
}
