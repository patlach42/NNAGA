#include <gtest/gtest.h>

#include "libusb_uac_driver.h"
#define private public
#include "engine/DirectUsbOutput.h"
#undef private

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>
#include <future>

static_assert(
    std::is_same_v<
        decltype(std::declval<guitarrackcraft::DirectUsbOutput&>().writeStereo(
            static_cast<const float*>(nullptr),
            static_cast<const float*>(nullptr),
            0)),
        int>,
    "DirectUsbOutput::writeStereo must report submitted whole frames");

namespace monotrypt::usb {

// The production class exposes only public lifecycle/PCM APIs. This narrow
// friend keeps callback/ring assertions on the actual implementation without
// duplicating its state machine in a test model.
struct UsbDriverTestAccess {
    struct PlaybackRegionView {
        uint8_t* first = nullptr;
        size_t firstBytes = 0;
        uint8_t* second = nullptr;
        size_t secondBytes = 0;
        size_t producerCursor = 0;
        int frames = 0;
        int frameStride = 0;
    };
    struct CaptureRegionView {
        const uint8_t* first = nullptr;
        size_t firstBytes = 0;
        const uint8_t* second = nullptr;
        size_t secondBytes = 0;
        size_t consumerCursor = 0;
        int frames = 0;
        int frameStride = 0;
    };
    static PlaybackRegionView preparePlaybackWrite(
            LibusbUacDriver& d, int requestedFrames) {
        const auto r = d.preparePlaybackWrite(requestedFrames);
        return {r.first, r.firstBytes, r.second, r.secondBytes,
                r.producerCursor, r.frames, r.frameStride};
    }
    static void commitPlaybackWrite(
            LibusbUacDriver& d, const PlaybackRegionView& r) {
        LibusbUacDriver::PlaybackWriteRegion privateRegion;
        privateRegion.first = r.first;
        privateRegion.firstBytes = r.firstBytes;
        privateRegion.second = r.second;
        privateRegion.secondBytes = r.secondBytes;
        privateRegion.producerCursor = r.producerCursor;
        privateRegion.frames = r.frames;
        privateRegion.frameStride = r.frameStride;
        d.commitPlaybackWrite(privateRegion);
    }
    static CaptureRegionView prepareCaptureRead(
            LibusbUacDriver& d, int requestedFrames) {
        const auto r = d.prepareCaptureRead(requestedFrames);
        return {r.first, r.firstBytes, r.second, r.secondBytes,
                r.consumerCursor, r.frames, r.frameStride};
    }
    static void commitCaptureRead(
            LibusbUacDriver& d, const CaptureRegionView& r) {
        LibusbUacDriver::CaptureReadRegion privateRegion;
        privateRegion.first = r.first;
        privateRegion.firstBytes = r.firstBytes;
        privateRegion.second = r.second;
        privateRegion.secondBytes = r.secondBytes;
        privateRegion.consumerCursor = r.consumerCursor;
        privateRegion.frames = r.frames;
        privateRegion.frameStride = r.frameStride;
        d.commitCaptureRead(privateRegion);
    }
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
    static void captureBits(LibusbUacDriver& d, int bits) {
        d.captureFormat_.bitsPerSample = bits;
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
    static size_t implicitRead(const LibusbUacDriver& d) {
        return d.implicitRead_.load(std::memory_order_acquire);
    }
    static void implicitFrameMetadata(
            LibusbUacDriver& d, size_t index, uint32_t frames) {
        d.implicitFrames_[index % d.implicitFrames_.size()].store(
            frames, std::memory_order_release);
    }
    static bool prepareImplicit(
            LibusbUacDriver& d, libusb_transfer* xfr) {
        return d.prepareImplicitTransfer(xfr);
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
    static libusb_transfer* playbackTransfer(LibusbUacDriver& d,
                                              size_t index) {
        return d.transfers_.at(index);
    }
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
    static void setCaptureRingBytes(
            LibusbUacDriver& d, const std::vector<uint8_t>& bytes) {
        d.captureRing_ = bytes;
    }
    static bool stopRequested(const LibusbUacDriver& d) {
        return d.stopRequested_.load(std::memory_order_acquire);
    }
    static int inflight(const LibusbUacDriver& d) {
        return d.inflight_.load(std::memory_order_acquire);
    }
    static void isoStartupState(LibusbUacDriver& d,
                                int sampleRate = 48000,
                                int bitsPerSample = 16,
                                int channels = 2,
                                int bytesPerSample = 2,
                                bool highSpeed = true,
                                int bInterval = 1) {
        d.ctx_ = reinterpret_cast<libusb_context*>(static_cast<uintptr_t>(1));
        d.device_ = reinterpret_cast<libusb_device_handle*>(
            static_cast<uintptr_t>(1));
        d.format_.sampleRateHz = sampleRate;
        d.format_.bitsPerSample = bitsPerSample;
        d.format_.bytesPerSample = bytesPerSample;
        d.format_.channels = channels;
        d.format_.endpointAddress = 1;
        d.format_.isHighSpeed = highSpeed;
        d.format_.bInterval = bInterval;
        d.format_.feedbackEndpointAddress = 0;
        d.stopRequested_.store(false, std::memory_order_release);
        d.streaming_.store(false, std::memory_order_release);
    }
    static bool startIsoPump(LibusbUacDriver& d) { return d.startIsoPump(); }
    static bool prepareIsoPump(LibusbUacDriver& d) {
        return d.startIsoPump(false);
    }
    static bool stopIsoPump(LibusbUacDriver& d) { return d.stopIsoPump(); }
};

} // namespace monotrypt::usb

namespace {

extern "C" void usb_driver_mock_set_submit_result(int result);
extern "C" int usb_driver_mock_submit_calls();
extern "C" void usb_driver_mock_set_max_iso_packet_size(int bytes);
extern "C" int usb_driver_mock_submitted_transfer_count();
extern "C" int usb_driver_mock_submitted_payload_size(int transfer);
extern "C" uint8_t usb_driver_mock_submitted_payload_byte(int transfer,
                                                            int offset);
extern "C" int usb_driver_mock_cancel_callback_calls();
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

void LIBUSB_CALL noopTransferCallback(libusb_transfer*) {}

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
TEST(UsbDriverMock, CancelOnlyInvokesAcceptedTransferOnce) {
    resetMock();

    auto* unsubmitted = libusb_alloc_transfer(0);
    ASSERT_NE(unsubmitted, nullptr);
    unsubmitted->callback = &noopTransferCallback;
    EXPECT_EQ(libusb_cancel_transfer(unsubmitted), LIBUSB_ERROR_NOT_FOUND);
    EXPECT_EQ(usb_driver_mock_cancel_callback_calls(), 0);
    libusb_free_transfer(unsubmitted);

    auto* accepted = libusb_alloc_transfer(0);
    ASSERT_NE(accepted, nullptr);
    accepted->callback = &noopTransferCallback;
    ASSERT_EQ(libusb_submit_transfer(accepted), LIBUSB_SUCCESS);
    EXPECT_EQ(libusb_cancel_transfer(accepted), LIBUSB_SUCCESS);
    EXPECT_EQ(usb_driver_mock_cancel_callback_calls(), 1);
    EXPECT_EQ(libusb_cancel_transfer(accepted), LIBUSB_ERROR_NOT_FOUND);
    EXPECT_EQ(usb_driver_mock_cancel_callback_calls(), 1);
    libusb_free_transfer(accepted);
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

TEST(UsbDriverRing, WatermarkRejectsPartialFrameAsBackpressureWithoutPlaybackXrun) {
    monotrypt::usb::LibusbUacDriver driver;
    monotrypt::usb::UsbDriverTestAccess::playbackFormat(driver, 2, 2);
    driver.setGraphQuantum(16, 2);  // startup high watermark admits 64 of 65
    std::vector<uint8_t> input(65 * 4, 0xA5);

    EXPECT_EQ(driver.writePcm(input.data(), 65), 64);
    EXPECT_EQ(driver.bufferedFrames(), 64);
    EXPECT_EQ(driver.writableFrames(), 0);
    EXPECT_EQ(driver.playbackBackpressureCount(), 1u);
    EXPECT_EQ(driver.playbackXRunCount(), 0u);

    EXPECT_EQ(driver.writePcm(input.data(), 1), 0);
    EXPECT_EQ(driver.playbackBackpressureCount(), 1u);
    EXPECT_EQ(driver.playbackXRunCount(), 0u);
}
TEST(UsbDriverRing, DrainStarvationPadsSilenceAndCountsPlaybackUnderrun) {
    monotrypt::usb::LibusbUacDriver driver;
    monotrypt::usb::UsbDriverTestAccess::playbackFormat(driver, 2, 2);

    constexpr int frameStride = 4;
    constexpr int outputFrames = 2;
    const int outputBytes = outputFrames * frameStride;
    std::vector<uint8_t> output(outputBytes, 0xA5);
    EXPECT_EQ(monotrypt::usb::UsbDriverTestAccess::drain(
                  driver, output.data(), outputBytes),
              0);
    EXPECT_EQ(driver.playbackXRunCount(), 0u);
    EXPECT_EQ(driver.playbackBackpressureCount(), 0u);

    // Any pre-start padding is not an audible underrun. Compare exact
    // silence totals against this baseline so the post-start contract is
    // independent of whether the driver records pre-start padding.
    const uint64_t silentPacketBaseline = driver.playbackSilentPacketCount();
    const uint64_t silentFrameBaseline = driver.playbackSilentFrameCount();

    monotrypt::usb::UsbDriverTestAccess::playbackStarted(driver, true);
    std::fill(output.begin(), output.end(), 0xA5);
    EXPECT_EQ(monotrypt::usb::UsbDriverTestAccess::drain(
                  driver, output.data(), outputBytes),
              0);
    EXPECT_EQ(output, std::vector<uint8_t>(output.size(), 0));
    EXPECT_EQ(driver.playbackXRunCount(), 1u);
    EXPECT_EQ(driver.playbackBackpressureCount(), 0u);
    EXPECT_EQ(driver.playbackSilentPacketCount(), silentPacketBaseline + 1);
    EXPECT_EQ(driver.playbackSilentFrameCount(), silentFrameBaseline + outputFrames);

    // Adjacent starvation inserts another padded packet and its exact frame
    // count, while the xrun transition remains latched at one event.
    EXPECT_EQ(monotrypt::usb::UsbDriverTestAccess::drain(
                  driver, output.data(), outputBytes),
              0);
    EXPECT_EQ(driver.playbackXRunCount(), 1u);
    EXPECT_EQ(driver.playbackSilentPacketCount(), silentPacketBaseline + 2);
    EXPECT_EQ(driver.playbackSilentFrameCount(), silentFrameBaseline + 2 * outputFrames);

    // A full packet clears the starvation latch. The next empty packet must
    // therefore create a new xrun transition and continue both totals.
    const std::vector<uint8_t> input(outputBytes, 0x5A);
    ASSERT_EQ(driver.writePcm(input.data(), outputFrames), outputFrames);
    std::fill(output.begin(), output.end(), 0xA5);
    ASSERT_EQ(monotrypt::usb::UsbDriverTestAccess::drain(
                  driver, output.data(), outputBytes),
              outputBytes);
    EXPECT_EQ(output, input);
    EXPECT_EQ(driver.playbackXRunCount(), 1u);
    EXPECT_EQ(driver.playbackSilentPacketCount(), silentPacketBaseline + 2);
    EXPECT_EQ(driver.playbackSilentFrameCount(), silentFrameBaseline + 2 * outputFrames);

    std::fill(output.begin(), output.end(), 0xA5);
    EXPECT_EQ(monotrypt::usb::UsbDriverTestAccess::drain(
                  driver, output.data(), outputBytes),
              0);
    EXPECT_EQ(output, std::vector<uint8_t>(output.size(), 0));
    EXPECT_EQ(driver.playbackXRunCount(), 2u);
    EXPECT_EQ(driver.playbackSilentPacketCount(), silentPacketBaseline + 3);
    EXPECT_EQ(driver.playbackSilentFrameCount(), silentFrameBaseline + 3 * outputFrames);
}
TEST(UsbDriverRing, DefaultWatermarkUsesHighWatermarkStartupLimit) {
    monotrypt::usb::LibusbUacDriver driver;
    monotrypt::usb::UsbDriverTestAccess::playbackFormat(driver, 2, 2);
    driver.setGraphQuantum(16);  // startup prime 64 + graph quantum = 80

    constexpr int frameStride = 4;
    std::vector<uint8_t> input(80 * frameStride, 0xA5);

    EXPECT_EQ(driver.writePcm(input.data(), 49), 49);
    EXPECT_EQ(driver.bufferedFrames(), 49);
    EXPECT_EQ(driver.writableFrames(), 31);
    EXPECT_EQ(driver.playbackXRunCount(), 0u);

    EXPECT_EQ(driver.writePcm(input.data() + 49 * frameStride, 31), 31);
    EXPECT_EQ(driver.bufferedFrames(), 80);
    EXPECT_EQ(driver.writableFrames(), 0);
    EXPECT_EQ(driver.playbackXRunCount(), 0u);
}


TEST(UsbDriverRing, PartialAdmissionReportsWholeFramesAndCallerCanSubmitTail) {
    monotrypt::usb::LibusbUacDriver driver;
    monotrypt::usb::UsbDriverTestAccess::playbackFormat(driver, 2, 2);
    driver.setGraphQuantum(16, 2);  // startup high watermark admits 64 of 65

    constexpr int frameStride = 4;
    constexpr int requestedFrames = 65;
    std::vector<uint8_t> input(requestedFrames * frameStride);
    for (int frame = 0; frame < requestedFrames; ++frame) {
        for (int byte = 0; byte < frameStride; ++byte) {
            input[frame * frameStride + byte] =
                static_cast<uint8_t>(0x10 + frame + byte);
        }
    }

    const int submitted = driver.writePcm(input.data(), requestedFrames);
    ASSERT_EQ(submitted, 64);
    EXPECT_EQ(driver.bufferedFrames(), submitted);
    EXPECT_EQ(driver.writableFrames(), 0);

    std::vector<uint8_t> admitted(submitted * frameStride);
    monotrypt::usb::UsbDriverTestAccess::playbackStarted(driver, true);
    ASSERT_EQ(monotrypt::usb::UsbDriverTestAccess::drain(
                  driver, admitted.data(), static_cast<int>(admitted.size())),
              static_cast<int>(admitted.size()));
    EXPECT_EQ(admitted,
              std::vector<uint8_t>(input.begin(),
                                    input.begin() + submitted * frameStride));

    const int remainingFrames = requestedFrames - submitted;
    ASSERT_EQ(remainingFrames, 1);
    ASSERT_EQ(driver.writePcm(input.data() + submitted * frameStride,
                              remainingFrames),
              remainingFrames);

    std::vector<uint8_t> tail(frameStride);
    ASSERT_EQ(monotrypt::usb::UsbDriverTestAccess::drain(
                  driver, tail.data(), static_cast<int>(tail.size())),
              frameStride);
    EXPECT_EQ(tail,
              std::vector<uint8_t>(input.begin() + submitted * frameStride,
                                    input.end()));
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
    EXPECT_EQ(stats.deferredTransfers, 0u);
    EXPECT_EQ(stats.metadataFifoOverruns, 1u);
    EXPECT_EQ(driver.captureStats().overruns, 0u);
    EXPECT_EQ(driver.captureAvailableFrames(), 64);

    std::vector<uint8_t> output(257 * 4, 0);
    ASSERT_EQ(driver.readCapturePcm(output.data(), 257), 64);
    // The bounded read exposes the newest 64 frames (193..256), not the
    // oldest physical backlog.
    EXPECT_EQ(output[0], 193);
    EXPECT_EQ(output[1], 0);
    constexpr size_t last = 63 * 4;
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

TEST(UsbDriverImplicit, PrepareDefersUntilWholeTransferPcmAndPreservesPendingOrder) {
    resetMock();

    monotrypt::usb::LibusbUacDriver driver;
    monotrypt::usb::UsbDriverTestAccess::playbackFormat(driver, 2, 2);
    monotrypt::usb::UsbDriverTestAccess::captureFormat(driver, 2, 2, true);
    monotrypt::usb::UsbDriverTestAccess::captureActive(driver, true);
    monotrypt::usb::UsbDriverTestAccess::playbackInflight(driver, 2);
    monotrypt::usb::UsbDriverTestAccess::feedbackState(driver, 1, 8);
    monotrypt::usb::UsbDriverTestAccess::implicitFrameMetadata(driver, 0, 3);
    monotrypt::usb::UsbDriverTestAccess::implicitFrameMetadata(driver, 1, 2);
    monotrypt::usb::UsbDriverTestAccess::implicitFrameMetadata(driver, 2, 2);
    monotrypt::usb::UsbDriverTestAccess::implicitCursors(driver, 3, 0);

    std::vector<uint8_t> firstPayload(24, 0xCD);
    std::vector<uint8_t> secondPayload(8, 0xCD);
    const std::vector<uint8_t> untouchedFirst = firstPayload;
    const std::vector<uint8_t> untouchedSecond = secondPayload;
    auto* first = makeTransfer(firstPayload, 2);
    auto* second = makeTransfer(secondPayload);
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);

    std::vector<uint8_t> pcm(7 * 4);
    for (size_t i = 0; i < pcm.size(); ++i)
        pcm[i] = static_cast<uint8_t>(0x40 + i);
    monotrypt::usb::UsbDriverTestAccess::playbackStarted(driver, true);
    ASSERT_EQ(driver.writePcm(pcm.data(), 4), 4);

    // The first URB needs 3 + 2 = 5 complete frames. A short ring must
    // leave both metadata and packet storage untouched.
    EXPECT_FALSE(monotrypt::usb::UsbDriverTestAccess::prepareImplicit(
        driver, first));
    EXPECT_EQ(monotrypt::usb::UsbDriverTestAccess::implicitRead(driver), 0u);
    EXPECT_EQ(driver.bufferedFrames(), 4);
    EXPECT_EQ(driver.playbackXRunCount(), 0u);
    EXPECT_EQ(driver.playbackSilentPacketCount(), 0u);
    EXPECT_EQ(driver.playbackSilentFrameCount(), 0u);
    EXPECT_EQ(firstPayload, untouchedFirst);
    EXPECT_EQ(first->iso_packet_desc[0].length, 12u);
    EXPECT_EQ(first->iso_packet_desc[1].length, 12u);

    // Once the first completion is deferred, a later completion joins the
    // FIFO without overtaking it or consuming newer metadata.
    monotrypt::usb::UsbDriverTestAccess::onIso(driver, first);
    EXPECT_EQ(monotrypt::usb::UsbDriverTestAccess::pendingCount(driver), 1u);
    EXPECT_EQ(monotrypt::usb::UsbDriverTestAccess::implicitRead(driver), 0u);
    monotrypt::usb::UsbDriverTestAccess::onIso(driver, second);
    EXPECT_EQ(monotrypt::usb::UsbDriverTestAccess::pendingCount(driver), 2u);
    EXPECT_EQ(monotrypt::usb::UsbDriverTestAccess::implicitRead(driver), 0u);
    EXPECT_EQ(secondPayload, untouchedSecond);
    // Both completions belong to one contiguous zero-runway episode: the
    // second pending completion must not double-count the same gap.
    EXPECT_EQ(driver.playbackXRunCount(), 1u);
    EXPECT_EQ(driver.playbackSilentPacketCount(), 0u);
    EXPECT_EQ(driver.playbackSilentFrameCount(), 0u);

    ASSERT_EQ(driver.writePcm(pcm.data() + 4 * 4, 3), 3);
    ASSERT_EQ(driver.bufferedFrames(), 7);
    monotrypt::usb::UsbDriverTestAccess::submitPending(driver);

    EXPECT_EQ(monotrypt::usb::UsbDriverTestAccess::pendingCount(driver), 0u);
    EXPECT_EQ(monotrypt::usb::UsbDriverTestAccess::implicitRead(driver), 3u);
    EXPECT_EQ(driver.bufferedFrames(), 0);
    EXPECT_EQ(driver.playedFrames(), 7);
    EXPECT_EQ(driver.playbackXRunCount(), 1u);
    EXPECT_EQ(driver.playbackSilentPacketCount(), 0u);
    EXPECT_EQ(driver.playbackSilentFrameCount(), 0u);
    // Completing the restored seven-frame runway clears the episode latch.
    // A later short completion therefore starts exactly one new episode.
    std::vector<uint8_t> thirdPayload(7 * 4, 0xCD);
    auto* third = makeTransfer(thirdPayload);
    ASSERT_NE(third, nullptr);
    monotrypt::usb::UsbDriverTestAccess::onIso(driver, third);
    EXPECT_EQ(monotrypt::usb::UsbDriverTestAccess::pendingCount(driver), 1u);
    EXPECT_EQ(driver.playbackXRunCount(), 2u);

    std::vector<uint8_t> secondPcm(7 * 4, 0x23);
    ASSERT_EQ(driver.writePcm(secondPcm.data(), 7), 7);
    monotrypt::usb::UsbDriverTestAccess::implicitFrameMetadata(driver, 3, 7);
    monotrypt::usb::UsbDriverTestAccess::implicitCursors(driver, 4, 3);
    monotrypt::usb::UsbDriverTestAccess::submitPending(driver);
    EXPECT_EQ(monotrypt::usb::UsbDriverTestAccess::pendingCount(driver), 0u);
    EXPECT_EQ(driver.playbackXRunCount(), 2u);
    EXPECT_EQ(usb_driver_mock_submitted_transfer_count(), 3);
    EXPECT_EQ(usb_driver_mock_submitted_payload_size(2), 28);
    EXPECT_EQ(usb_driver_mock_submitted_payload_size(0), 20);
    EXPECT_EQ(usb_driver_mock_submitted_payload_size(1), 8);
    for (int offset = 0; offset < 20; ++offset) {
        SCOPED_TRACE(offset);
        EXPECT_EQ(usb_driver_mock_submitted_payload_byte(0, offset),
                  pcm[static_cast<size_t>(offset)]);
    }
    for (int offset = 0; offset < 8; ++offset) {
        SCOPED_TRACE(offset);
        EXPECT_EQ(usb_driver_mock_submitted_payload_byte(1, offset),
                  pcm[20 + static_cast<size_t>(offset)]);
    }
    EXPECT_EQ(first->iso_packet_desc[0].length, 12u);
    EXPECT_EQ(first->iso_packet_desc[1].length, 8u);
    EXPECT_EQ(second->iso_packet_desc[0].length, 8u);

    libusb_free_transfer(third);
    libusb_free_transfer(first);
    libusb_free_transfer(second);
}
TEST(UsbDriverImplicit, VariablePacketLengthsPreserveSubmittedPcmAcrossBoundaries) {
    resetMock();

    monotrypt::usb::LibusbUacDriver driver;
    monotrypt::usb::UsbDriverTestAccess::playbackFormat(driver, 2, 2);
    monotrypt::usb::UsbDriverTestAccess::feedbackState(driver, 1, 8);
    monotrypt::usb::UsbDriverTestAccess::implicitFrameMetadata(driver, 0, 3);
    monotrypt::usb::UsbDriverTestAccess::implicitFrameMetadata(driver, 1, 2);
    monotrypt::usb::UsbDriverTestAccess::implicitFrameMetadata(driver, 2, 2);
    monotrypt::usb::UsbDriverTestAccess::implicitCursors(driver, 3, 0);
    monotrypt::usb::UsbDriverTestAccess::playbackStarted(driver, true);

    std::vector<uint8_t> pcm(7 * 4);
    for (size_t i = 0; i < pcm.size(); ++i)
        pcm[i] = static_cast<uint8_t>(i + 1);
    ASSERT_EQ(driver.writePcm(pcm.data(), 7), 7);

    // Keep three equal-capacity packet slots in the transfer buffer. The
    // implicit frame counts then shorten packets 1 and 2 to 8 bytes each.
    std::vector<uint8_t> transferBuffer(3 * 12, 0xEE);
    auto* transfer = makeTransfer(transferBuffer, 3);
    ASSERT_NE(transfer, nullptr);
    monotrypt::usb::UsbDriverTestAccess::pending(driver, transfer);
    monotrypt::usb::UsbDriverTestAccess::submitPending(driver);

    EXPECT_EQ(monotrypt::usb::UsbDriverTestAccess::pendingCount(driver), 0u);
    EXPECT_EQ(usb_driver_mock_submitted_transfer_count(), 1);
    ASSERT_EQ(usb_driver_mock_submitted_payload_size(0),
              static_cast<int>(pcm.size()));
    EXPECT_EQ(transfer->iso_packet_desc[0].length, 12u);
    EXPECT_EQ(transfer->iso_packet_desc[1].length, 8u);
    EXPECT_EQ(transfer->iso_packet_desc[2].length, 8u);
    for (size_t offset = 0; offset < pcm.size(); ++offset) {
        SCOPED_TRACE(offset);
        EXPECT_EQ(usb_driver_mock_submitted_payload_byte(
                      0, static_cast<int>(offset)),
                  pcm[offset]);
    }

    libusb_free_transfer(transfer);
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

TEST(UsbDriverTelemetry, IsoDeviceRemovalIncrementsDirectionSpecificCounters) {
    resetMock();
    monotrypt::usb::LibusbUacDriver captureDriver;
    monotrypt::usb::UsbDriverTestAccess::captureActive(captureDriver, true);
    monotrypt::usb::UsbDriverTestAccess::captureInflight(captureDriver, 1);
    std::vector<uint8_t> capturePayload(2, 0xC1);
    auto* captureTransfer = makeTransfer(capturePayload);
    ASSERT_NE(captureTransfer, nullptr);
    captureTransfer->status = LIBUSB_TRANSFER_NO_DEVICE;

    monotrypt::usb::UsbDriverTestAccess::onCapture(captureDriver,
                                                    captureTransfer);
    const auto captureStats = captureDriver.implicitFeedbackStats();
    EXPECT_EQ(captureStats.captureTransferErrors, 1u);
    EXPECT_EQ(captureStats.playbackTransferErrors, 0u);
    libusb_free_transfer(captureTransfer);

    monotrypt::usb::LibusbUacDriver playbackDriver;
    monotrypt::usb::UsbDriverTestAccess::playbackInflight(playbackDriver, 1);
    std::vector<uint8_t> playbackPayload(2, 0xD2);
    auto* playbackTransfer = makeTransfer(playbackPayload);
    ASSERT_NE(playbackTransfer, nullptr);
    playbackTransfer->status = LIBUSB_TRANSFER_NO_DEVICE;

    monotrypt::usb::UsbDriverTestAccess::onIso(playbackDriver,
                                               playbackTransfer);
    const auto playbackStats = playbackDriver.implicitFeedbackStats();
    EXPECT_EQ(playbackStats.captureTransferErrors, 0u);
    EXPECT_EQ(playbackStats.playbackTransferErrors, 1u);
    libusb_free_transfer(playbackTransfer);
}
TEST(UsbDriverLifecycle, InitialOutQueueConsumesOrderedPcmBeforeSubmit) {
    resetMock();
    usb_driver_mock_set_max_iso_packet_size(7 * 4);

    monotrypt::usb::LibusbUacDriver driver;
    monotrypt::usb::UsbDriverTestAccess::isoStartupState(driver);

    constexpr int kTransfers = 4;
    constexpr int kPacketsPerTransfer = 8;
    constexpr int kFramesPerPacket = 6;  // 48 kHz / 8 kHz packet cadence.
    constexpr int kFrameBytes = 4;       // stereo 16-bit PCM.
    constexpr int kInitialFrames =
        kTransfers * kPacketsPerTransfer * kFramesPerPacket;
    constexpr int kInitialBytes = kInitialFrames * kFrameBytes;

    std::vector<uint8_t> ring(monotrypt::usb::kPlaybackRingBytes, 0);
    for (int frame = 0; frame < kInitialFrames; ++frame) {
        for (int byte = 0; byte < kFrameBytes; ++byte) {
            ring[frame * kFrameBytes + byte] =
                static_cast<uint8_t>(0x40 + frame + byte);
        }
    }
    monotrypt::usb::UsbDriverTestAccess::setRingBytes(driver, ring);
    monotrypt::usb::UsbDriverTestAccess::playbackCursors(
        driver, kInitialBytes, 0);

    ASSERT_TRUE(monotrypt::usb::UsbDriverTestAccess::prepareIsoPump(driver));
    ASSERT_TRUE(driver.startPlayback());
    EXPECT_FALSE(driver.startPlayback());
    ASSERT_EQ(usb_driver_mock_submitted_transfer_count(), kTransfers);
    EXPECT_EQ(driver.bufferedFrames(), 0);

    for (int transfer = 0; transfer < kTransfers; ++transfer) {
        SCOPED_TRACE(transfer);
        ASSERT_EQ(usb_driver_mock_submitted_payload_size(transfer),
                  kPacketsPerTransfer * kFramesPerPacket * kFrameBytes);
        for (int offset = 0;
             offset < kPacketsPerTransfer * kFramesPerPacket * kFrameBytes;
             ++offset) {
            const int frame =
                (transfer * kPacketsPerTransfer * kFramesPerPacket) +
                (offset / kFrameBytes);
            const int byte = offset % kFrameBytes;
            ASSERT_EQ(usb_driver_mock_submitted_payload_byte(transfer, offset),
                      static_cast<uint8_t>(0x40 + frame + byte))
                << "offset " << offset;
        }
    }
}
TEST(UsbDriverLifecycle, PreparedHighRateQueueUsesOneMillisecondTransfers) {
    resetMock();

    constexpr int kTransfers = 4;
    constexpr int kPacketRate = 1000;  // high-speed bInterval=4
    constexpr int kPacketsPerTransfer =
        monotrypt::usb::packetsPerTransferForRate(kPacketRate);
    constexpr int kFramesPerPacket = 192;  // 192 kHz / 1 kHz
    constexpr int kFrameBytes = 4 * 4;     // 4ch S32
    constexpr int kInitialFrames =
        kTransfers * kPacketsPerTransfer * kFramesPerPacket;
    constexpr int kInitialBytes = kInitialFrames * kFrameBytes;
    ASSERT_LE(kInitialBytes,
              static_cast<int>(monotrypt::usb::kPlaybackRingBytes));

    usb_driver_mock_set_max_iso_packet_size(kFramesPerPacket * kFrameBytes);
    monotrypt::usb::LibusbUacDriver driver;
    monotrypt::usb::UsbDriverTestAccess::isoStartupState(
        driver, 192000, 32, 4, 4, true, 4);

    std::vector<uint8_t> ring(monotrypt::usb::kPlaybackRingBytes, 0);
    for (int frame = 0; frame < kInitialFrames; ++frame) {
        for (int byte = 0; byte < kFrameBytes; ++byte) {
            ring[frame * kFrameBytes + byte] =
                static_cast<uint8_t>(0x70 + frame + byte);
        }
    }
    monotrypt::usb::UsbDriverTestAccess::setRingBytes(driver, ring);
    monotrypt::usb::UsbDriverTestAccess::playbackCursors(
        driver, kInitialBytes, 0);

    ASSERT_TRUE(monotrypt::usb::UsbDriverTestAccess::prepareIsoPump(driver));
    EXPECT_EQ(driver.startupPrimeFrames(), kInitialFrames);
    ASSERT_TRUE(driver.startPlayback());
    EXPECT_FALSE(driver.startPlayback());
    ASSERT_EQ(usb_driver_mock_submitted_transfer_count(), kTransfers);

    for (int transfer = 0; transfer < kTransfers; ++transfer) {
        SCOPED_TRACE(transfer);
        ASSERT_EQ(usb_driver_mock_submitted_payload_size(transfer),
                  kFramesPerPacket * kFrameBytes);
        for (int offset = 0; offset < kFramesPerPacket * kFrameBytes;
             ++offset) {
            const int frame = transfer * kFramesPerPacket +
                              offset / kFrameBytes;
            const int byte = offset % kFrameBytes;
            ASSERT_EQ(usb_driver_mock_submitted_payload_byte(transfer, offset),
                      static_cast<uint8_t>(0x70 + frame + byte))
                << "offset " << offset;
        }
    }
}

TEST(UsbDriverLifecycle, StopPreparedPumpDoesNotCallbackUnsubmittedTransfers) {
    resetMock();
    usb_driver_mock_set_max_iso_packet_size(7 * 4);

    monotrypt::usb::LibusbUacDriver driver;
    monotrypt::usb::UsbDriverTestAccess::isoStartupState(driver);
    ASSERT_TRUE(monotrypt::usb::UsbDriverTestAccess::prepareIsoPump(driver));
    EXPECT_EQ(usb_driver_mock_submit_calls(), 0);
    EXPECT_EQ(monotrypt::usb::UsbDriverTestAccess::inflight(driver), 0);

    driver.stop();

    EXPECT_EQ(usb_driver_mock_cancel_callback_calls(), 0);
    EXPECT_EQ(monotrypt::usb::UsbDriverTestAccess::inflight(driver), 0);
    EXPECT_FALSE(driver.isStreaming());
}



TEST(UsbDriverLifecycle, StopWakesBlockedWritableWait) {
    monotrypt::usb::LibusbUacDriver driver;
    monotrypt::usb::UsbDriverTestAccess::playbackFormat(driver, 2, 2);
    driver.setGraphQuantum(16, 1);

    // Startup prime is target + graph quantum (32), and the write limit adds
    // one graph quantum. Fill the resulting 48-frame limit before waiting.
    std::vector<uint8_t> full(48 * 4, 0x55);
    ASSERT_EQ(driver.writePcm(full.data(), 48), 48);
    monotrypt::usb::UsbDriverTestAccess::streaming(driver, true);

    std::promise<void> entered;
    auto enteredFuture = entered.get_future();
    std::atomic<bool> waitResult{true};
    std::thread waiter([&] {
        entered.set_value();
        waitResult.store(driver.waitForWritableFrames(1, -1),
                         std::memory_order_release);
    });

    enteredFuture.wait();
    driver.stop();
    waiter.join();

    EXPECT_FALSE(waitResult.load(std::memory_order_acquire));
    EXPECT_FALSE(driver.isStreaming());
}
TEST(UsbDriverLifecycle, QueuedOutFramesTracksCompletionAndStop) {
    resetMock();
    usb_driver_mock_set_max_iso_packet_size(7 * 4);

    constexpr int kTransfers = 4;
    constexpr int kPacketsPerTransfer = 8;
    constexpr int kFrameBytes = 4;
    constexpr int kFramesPerPacket = 6;
    constexpr int kInitialFrames =
        kTransfers * kPacketsPerTransfer * kFramesPerPacket;

    monotrypt::usb::LibusbUacDriver driver;
    monotrypt::usb::UsbDriverTestAccess::isoStartupState(driver);
    std::vector<uint8_t> ring(monotrypt::usb::kPlaybackRingBytes, 0);
    monotrypt::usb::UsbDriverTestAccess::setRingBytes(driver, ring);
    monotrypt::usb::UsbDriverTestAccess::playbackCursors(
        driver, kInitialFrames * kFrameBytes, 0);

    ASSERT_TRUE(monotrypt::usb::UsbDriverTestAccess::prepareIsoPump(driver));
    ASSERT_TRUE(driver.startPlayback());

    uint64_t descriptorFrames = 0;
    auto* completed =
        monotrypt::usb::UsbDriverTestAccess::playbackTransfer(driver, 0);
    ASSERT_NE(completed, nullptr);
    for (int transfer = 0; transfer < kTransfers; ++transfer) {
        auto* submitted =
            monotrypt::usb::UsbDriverTestAccess::playbackTransfer(
                driver, static_cast<size_t>(transfer));
        ASSERT_NE(submitted, nullptr);
        for (int packet = 0; packet < submitted->num_iso_packets; ++packet) {
            descriptorFrames +=
                submitted->iso_packet_desc[packet].length / kFrameBytes;
        }
    }
    ASSERT_EQ(descriptorFrames, static_cast<uint64_t>(kInitialFrames));
    EXPECT_EQ(driver.queuedOutFrames(), descriptorFrames);

    completed->status = LIBUSB_TRANSFER_COMPLETED;
    for (int packet = 0; packet < completed->num_iso_packets; ++packet) {
        completed->iso_packet_desc[packet].status = LIBUSB_TRANSFER_COMPLETED;
    }
    monotrypt::usb::UsbDriverTestAccess::onIso(driver, completed);

    EXPECT_EQ(driver.queuedOutFrames(), descriptorFrames);
    EXPECT_EQ(driver.queuedOutFrames(), static_cast<uint64_t>(kInitialFrames));

    driver.stop();
    EXPECT_EQ(driver.queuedOutFrames(), uint64_t{0});
}

TEST(UsbDriverRing, PrepareCommitPlaybackPublishesWholeWrappedFrames) {
    monotrypt::usb::LibusbUacDriver driver;
    monotrypt::usb::UsbDriverTestAccess::playbackFormat(driver, 2, 2);
    driver.setGraphQuantum(8);

    constexpr size_t capacity = monotrypt::usb::kPlaybackRingBytes;
    constexpr size_t frameStride = 4;
    monotrypt::usb::UsbDriverTestAccess::playbackCursors(
        driver, capacity - 2, capacity - 2);
    const auto region =
        monotrypt::usb::UsbDriverTestAccess::preparePlaybackWrite(driver, 2);

    ASSERT_EQ(region.frames, 2);
    ASSERT_EQ(region.frameStride, static_cast<int>(frameStride));
    ASSERT_EQ(region.firstBytes, 2u);
    ASSERT_EQ(region.secondBytes, 6u);
    EXPECT_EQ(driver.bufferedFrames(), 0);
    EXPECT_EQ(driver.writtenFrames(), 0);

    const std::vector<uint8_t> input{1, 2, 3, 4, 5, 6, 7, 8};
    std::memcpy(region.first, input.data(), region.firstBytes);
    std::memcpy(region.second, input.data() + region.firstBytes,
                region.secondBytes);
    EXPECT_EQ(driver.bufferedFrames(), 0);

    monotrypt::usb::UsbDriverTestAccess::commitPlaybackWrite(driver, region);
    EXPECT_EQ(driver.bufferedFrames(), 2);
    EXPECT_EQ(driver.writtenFrames(), 2);

    monotrypt::usb::UsbDriverTestAccess::playbackStarted(driver, true);
    std::vector<uint8_t> output(input.size(), 0);
    ASSERT_EQ(monotrypt::usb::UsbDriverTestAccess::drain(
                  driver, output.data(), static_cast<int>(output.size())),
              static_cast<int>(output.size()));
    EXPECT_EQ(output, input);
    EXPECT_EQ(driver.bufferedFrames(), 0);
}

TEST(UsbDriverRing, PrepareCommitPlaybackReportsWholeFramePartialAdmission) {
    monotrypt::usb::LibusbUacDriver driver;
    monotrypt::usb::UsbDriverTestAccess::playbackFormat(driver, 2, 2);
    driver.setGraphQuantum(2);

    const auto region =
        monotrypt::usb::UsbDriverTestAccess::preparePlaybackWrite(driver, 1000);
    ASSERT_GT(region.frames, 0);
    ASSERT_LT(region.frames, 1000);
    EXPECT_EQ(region.firstBytes + region.secondBytes,
              static_cast<size_t>(region.frames * region.frameStride));
    EXPECT_EQ(driver.bufferedFrames(), 0);

    monotrypt::usb::UsbDriverTestAccess::commitPlaybackWrite(driver, region);
    EXPECT_EQ(driver.bufferedFrames(), region.frames);
    const auto full =
        monotrypt::usb::UsbDriverTestAccess::preparePlaybackWrite(driver, 1);
    EXPECT_EQ(full.frames, 0);
    EXPECT_EQ(driver.bufferedFrames(), region.frames);
}

TEST(UsbDriverCapture, PrepareCommitCaptureKeepsWrappedFramesPrivate) {
    monotrypt::usb::LibusbUacDriver driver;
    monotrypt::usb::UsbDriverTestAccess::captureFormat(driver, 2, 2);
    constexpr size_t capacity = monotrypt::usb::kPlaybackRingBytes;
    monotrypt::usb::UsbDriverTestAccess::captureActive(driver, true);

    monotrypt::usb::UsbDriverTestAccess::captureCursors(
        driver, capacity - 2, capacity - 2);
    std::vector<uint8_t> ring(capacity, 0);
    const std::vector<uint8_t> input{0x11, 0x22, 0x33, 0x44,
                                     0x55, 0x66, 0x77, 0x88};
    std::memcpy(ring.data() + capacity - 2, input.data(), 2);
    std::memcpy(ring.data(), input.data() + 2, input.size() - 2);
    monotrypt::usb::UsbDriverTestAccess::setCaptureRingBytes(driver, ring);
    monotrypt::usb::UsbDriverTestAccess::captureCursors(
        driver, capacity - 2 + input.size(), capacity - 2);

    const auto region =
        monotrypt::usb::UsbDriverTestAccess::prepareCaptureRead(driver, 2);
    ASSERT_EQ(region.frames, 2);
    ASSERT_EQ(region.frameStride, 4);
    ASSERT_EQ(region.firstBytes, 2u);
    ASSERT_EQ(region.secondBytes, 6u);
    EXPECT_EQ(driver.captureAvailableFrames(), 2);

    std::vector<uint8_t> output(input.size(), 0);
    std::memcpy(output.data(), region.first, region.firstBytes);
    std::memcpy(output.data() + region.firstBytes, region.second,
                region.secondBytes);
    EXPECT_EQ(output, input);
    EXPECT_EQ(driver.captureAvailableFrames(), 2);

    monotrypt::usb::UsbDriverTestAccess::commitCaptureRead(driver, region);
    EXPECT_EQ(driver.captureAvailableFrames(), 0);
}

struct DirectPcmCase {
    int bits;
    int bytes;
    int channels;
    int outputPair;
    std::vector<uint8_t> expected;
};

TEST(DirectUsbOutput, WriteStereoPacksExactWrappedLeftJustifiedSamples) {
    const std::vector<DirectPcmCase> cases{
        {16, 2, 2, 0, {0x00, 0x80, 0xff, 0x7f,
                       0xff, 0x7f, 0x00, 0x80}},
        {24, 4, 4, 1, {
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x80, 0x00, 0xff, 0xff, 0x7f,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0xff, 0xff, 0x7f, 0x00, 0x00, 0x00, 0x80}},
        {32, 4, 2, 0, {0x00, 0x00, 0x00, 0x80,
                       0xff, 0xff, 0xff, 0x7f,
                       0xff, 0xff, 0xff, 0x7f,
                       0x00, 0x00, 0x00, 0x80}},
    };

    for (const auto& test : cases) {
        SCOPED_TRACE(test.bits);
        guitarrackcraft::DirectUsbOutput output;
        auto& driver = output.driver_;
        monotrypt::usb::UsbDriverTestAccess::playbackFormat(
            driver, test.channels, test.bytes);
        driver.setGraphQuantum(4);
        output.formatBits_ = test.bits;
        output.formatBytes_ = test.bytes;
        output.deviceChannels_ = test.channels;
        output.outputPair_ = test.outputPair;
        output.accepting_.store(true, std::memory_order_release);

        const int stride = test.channels * test.bytes;
        const size_t head =
            monotrypt::usb::kPlaybackRingBytes - stride / 2;
        monotrypt::usb::UsbDriverTestAccess::playbackCursors(
            driver, head, head);
        const float left[] = {-1.0f, 1.0f};
        const float right[] = {1.0f, -1.0f};
        ASSERT_EQ(output.writeStereo(left, right, 2), 2);
        EXPECT_EQ(driver.bufferedFrames(), 2);

        monotrypt::usb::UsbDriverTestAccess::playbackStarted(driver, true);
        std::vector<uint8_t> actual(test.expected.size(), 0xcd);
        ASSERT_EQ(monotrypt::usb::UsbDriverTestAccess::drain(
                      driver, actual.data(), static_cast<int>(actual.size())),
                  static_cast<int>(actual.size()));
        EXPECT_EQ(actual, test.expected);
    }
}

TEST(DirectUsbOutput, ReadMonoInputSignExtendsSelectedChannelAcrossWrap) {
    struct CaptureCase {
        int bits;
        int bytes;
        int32_t minimum;
        int32_t maximum;
        float maximumExpected;
    };
    const std::vector<CaptureCase> cases{
        {16, 2, -32768, 32767, 32767.0f / 32768.0f},
        {24, 4, -8388608, 8388607, 8388607.0f / 8388608.0f},
        {32, 4, std::numeric_limits<int32_t>::min(),
         std::numeric_limits<int32_t>::max(), 1.0f},
    };

    for (const auto& test : cases) {
        SCOPED_TRACE(test.bits);
        guitarrackcraft::DirectUsbOutput output;
        auto& driver = output.driver_;
        monotrypt::usb::UsbDriverTestAccess::captureFormat(
            driver, 2, test.bytes);
        monotrypt::usb::UsbDriverTestAccess::captureBits(driver, test.bits);
        driver.setGraphQuantum(4);
        monotrypt::usb::UsbDriverTestAccess::captureActive(driver, true);
        output.inputChannel_ = 1;

        const int stride = 2 * test.bytes;
        const size_t tail =
            monotrypt::usb::kPlaybackRingBytes - stride / 2;
        std::vector<uint8_t> ring(monotrypt::usb::kPlaybackRingBytes, 0xa5);
        auto writeSample = [&](size_t frame, int32_t sample) {
            const int validBytes = (test.bits + 7) / 8;
            const uint32_t packed = static_cast<uint32_t>(sample) <<
                (8 * (test.bytes - validBytes));
            const size_t base = (tail + frame * stride + test.bytes) %
                                ring.size();
            for (int byte = 0; byte < test.bytes; ++byte) {
                ring[(base + byte) % ring.size()] =
                    static_cast<uint8_t>(packed >> (8 * byte));
            }
        };
        writeSample(0, test.minimum);
        writeSample(1, test.maximum);
        monotrypt::usb::UsbDriverTestAccess::setCaptureRingBytes(driver, ring);
        monotrypt::usb::UsbDriverTestAccess::captureCursors(
            driver, tail + 2 * stride, tail);

        float outputSamples[2] = {};
        ASSERT_EQ(output.readMonoInput(outputSamples, 2), 2);
        EXPECT_FLOAT_EQ(outputSamples[0], -1.0f);
        EXPECT_NEAR(outputSamples[1], test.maximumExpected, 1.0e-6f);
        EXPECT_EQ(driver.captureAvailableFrames(), 0);
    }
}