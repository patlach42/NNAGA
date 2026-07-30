/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * This file is part of Guitar RackCraft.
 *
 * Guitar RackCraft is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef GUITARRACKCRAFT_DIRECT_USB_OUTPUT_H
#define GUITARRACKCRAFT_DIRECT_USB_OUTPUT_H

#include "../usb/libusb_uac_driver.h"
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>
#include <thread>
#include <string>
#include <vector>

namespace guitarrackcraft {

// Full-duplex custom USB UAC bridge. The render thread uses bounded,
// lock-free playback and capture rings; lifecycle stays on control threads.
class DirectUsbOutput {
public:
    static constexpr int kSampleRate = 48000;
    static constexpr int kBitsPerSample = 32;
    static constexpr int kChannels = 2;
    static constexpr int kMaxDeviceChannels = monotrypt::usb::kMaxTransportChannels;
    static constexpr int kMaxSubslotBytes = monotrypt::usb::kMaxSubslotBytes;
    // Keep engine blocks within the driver's bounded playback watermark.
    static constexpr int kMaxGraphQuantum = monotrypt::usb::kMaxGraphQuantum;
    static constexpr int kMaxFramesPerWrite = kMaxGraphQuantum;

    DirectUsbOutput() = default;
    ~DirectUsbOutput() { stop(); close(); }

    bool open(int fd) {
        if (fd < 0) return false;
        stop();
        close();
        if (!driver_.ensureContext() || !driver_.open(fd)) return false;
        accepting_.store(false, std::memory_order_release);
        return true;
    }

    void close() {
        stop();
        driver_.close();
    }

    bool start(int sampleRate, int bitsPerSample, int bytesPerSample, int channels,
               int outputPair) {
        {
            std::lock_guard<std::mutex> lock(errorMutex_);
            startErrorDetail_.clear();
        }
        if (!driver_.isOpen() || sampleRate <= 0 ||
            (bitsPerSample != 16 && bitsPerSample != 24 && bitsPerSample != 32) ||
            bytesPerSample < (bitsPerSample + 7) / 8 ||
            bytesPerSample > kMaxSubslotBytes ||
            channels < kChannels || channels > kMaxDeviceChannels ||
            outputPair < 0 || outputPair * 2 + 1 >= channels) return false;
        stop();
        if (!driver_.startDuplex(sampleRate, bitsPerSample, channels, bytesPerSample)) {
            std::lock_guard<std::mutex> lock(errorMutex_);
            startErrorDetail_ = driver_.lastErrorDetail();
            return false;
        }
        formatBits_ = bitsPerSample;
        formatBytes_ = bytesPerSample;
        deviceChannels_ = driver_.currentFormat().channels;
        const auto& capture = driver_.currentCaptureFormat();
        if (deviceChannels_ < kChannels || deviceChannels_ > kMaxDeviceChannels ||
            capture.channels <= 0 || capture.channels > kMaxDeviceChannels ||
            (capture.bitsPerSample != 16 && capture.bitsPerSample != 24 &&
             capture.bitsPerSample != 32) ||
            capture.bytesPerSample < (capture.bitsPerSample + 7) / 8 ||
            capture.bytesPerSample > kMaxSubslotBytes ||
            outputPair * 2 + 1 >= deviceChannels_) {
            {
                std::lock_guard<std::mutex> lock(errorMutex_);
                startErrorDetail_ =
                    "negotiated USB format is incompatible with the selected channels";
            }
            driver_.stop();
            return false;
        }
        outputPair_ = outputPair;
        streaming_.store(true, std::memory_order_release);
        return true;
    }

    bool startPlayback() noexcept {
        if (driver_.startPlayback()) return true;
        std::lock_guard<std::mutex> lock(errorMutex_);
        startErrorDetail_ = driver_.lastErrorDetail();
        return false;
    }

    int lastErrorCode() const noexcept {
        return static_cast<int>(driver_.lastError());
    }
    std::string lastErrorDetail() const {
        std::lock_guard<std::mutex> lock(errorMutex_);
        return startErrorDetail_.empty() ? driver_.lastErrorDetail() : startErrorDetail_;
    }

    int startupPrimeFrames() const noexcept {
        return driver_.startupPrimeFrames();
    }
    uint64_t queuedOutFrames() const noexcept {
        return driver_.queuedOutFrames();
    }
    int captureTransferFrames() const noexcept {
        return driver_.captureTransferFrames();
    }
    int playbackTargetFrames() const noexcept {
        return driver_.playbackTargetFrames();
    }

    void requestStop() noexcept {
        accepting_.store(false, std::memory_order_release);
        streaming_.store(false, std::memory_order_release);
        driver_.requestStop();
    }

    int captureChannelCount() const noexcept {
        return driver_.captureChannelCount();
    }

    std::vector<monotrypt::usb::UsbFormatCandidate> enumerateFormats() {
        return driver_.enumerateFormats();
    }

    void stop() {
        accepting_.store(false, std::memory_order_release);
        while (activeWriters_.load(std::memory_order_acquire) != 0) {
            // Control thread only: the render thread never enters this path.
            std::this_thread::yield();
        }
        streaming_.store(false, std::memory_order_release);
        driver_.stop();
    }

    bool isStreaming() const {
        return streaming_.load(std::memory_order_acquire) && driver_.isStreaming();
    }
    bool adapterStreaming() const noexcept {
        return streaming_.load(std::memory_order_acquire);
    }
    bool driverStreaming() const noexcept {
        return driver_.isStreaming();
    }

    // Called only from the dedicated render thread. No allocation, locks,
    // I/O, or blocking calls occur here. Returns the number of whole frames
    // admitted to the playback ring so the caller can retry an unqueued tail.
    int writeStereo(const float* left, const float* right, int frames) noexcept {
        if (!left || !right || frames <= 0 ||
            !accepting_.load(std::memory_order_acquire)) return 0;
        activeWriters_.fetch_add(1, std::memory_order_acq_rel);
        if (!accepting_.load(std::memory_order_acquire)) {
            activeWriters_.fetch_sub(1, std::memory_order_release);
            return 0;
        }

        int offset = 0;
        while (offset < frames) {
            const int requested = std::min(
                frames - offset, kMaxFramesPerWrite);
            const auto region = driver_.preparePlaybackWrite(requested);
            if (region.frames <= 0) break;
            switch (formatBits_) {
                case 16:
                    packPlaybackRegion<16>(
                        region, left + offset, right + offset);
                    break;
                case 24:
                    packPlaybackRegion<24>(
                        region, left + offset, right + offset);
                    break;
                case 32:
                    packPlaybackRegion<32>(
                        region, left + offset, right + offset);
                    break;
                default:
                    break;
            }
            driver_.commitPlaybackWrite(region);
            offset += region.frames;
            if (region.frames < requested) break;
        }
        activeWriters_.fetch_sub(1, std::memory_order_release);
        return offset;
    }

    // Reads all negotiated capture channels as normalized channel-major planes.
    int readInputChannels(float* const* destinations, int destinationChannels,
                          int frames) noexcept {
        if (!destinations || destinationChannels <= 0 || frames <= 0) return 0;
        frames = std::min(frames, kMaxFramesPerWrite);
        for (int channel = 0; channel < destinationChannels; ++channel) {
            if (!destinations[channel]) return 0;
            std::memset(destinations[channel], 0,
                        static_cast<size_t>(frames) * sizeof(float));
        }
        const auto region = driver_.prepareCaptureRead(frames);
        const auto& format = driver_.currentCaptureFormat();
        const int available = std::max(0, format.channels);
        const int decodedFrames = std::max(0, std::min(region.frames, frames));
        const int decodeChannels = std::min(destinationChannels, available);
        for (int channel = 0; channel < decodeChannels; ++channel) {
            const int sampleOffset = channel * format.bytesPerSample +
                (format.bytesPerSample - (format.bitsPerSample + 7) / 8);
            switch (format.bitsPerSample) {
                case 16: unpackCaptureChannel<16>(region, sampleOffset,
                    destinations[channel], decodedFrames); break;
                case 24: unpackCaptureChannel<24>(region, sampleOffset,
                    destinations[channel], decodedFrames); break;
                case 32: unpackCaptureChannel<32>(region, sampleOffset,
                    destinations[channel], decodedFrames); break;
                default: break;
            }
        }
        driver_.commitCaptureRead(region);
        return decodedFrames;
    }

    bool waitForCaptureFrames(int frames, int timeoutMs) const noexcept {
        return driver_.waitForCaptureFrames(frames, timeoutMs);
    }
    bool waitForWritableFrames(int frames, int timeoutMs) const noexcept {
        return driver_.waitForWritableFrames(frames, timeoutMs);
    }
    int discardCaptureFrames(int frames) noexcept {
        return driver_.discardCaptureFrames(frames);
    }

    uint64_t xrunCount() const noexcept {
        return driver_.playbackXRunCount();
    }
    uint64_t playbackSilentPacketCount() const noexcept {
        return driver_.playbackSilentPacketCount();
    }
    uint64_t playbackSilentFrameCount() const noexcept {
        return driver_.playbackSilentFrameCount();
    }
    uint64_t playbackBackpressureCount() const noexcept {
        return driver_.playbackBackpressureCount();
    }
    void setGraphQuantum(
            int frames,
            int periodMultiplier = monotrypt::usb::kDefaultPeriodMultiplier,
            int watermarkFrames = 0) noexcept {
        driver_.setGraphQuantum(frames, periodMultiplier, watermarkFrames);
    }
    int bufferedFrames() const noexcept { return driver_.bufferedFrames(); }
    int writableFrames() const noexcept { return driver_.writableFrames(); }
    uint64_t captureXRunCount() const noexcept {
        const auto stats = driver_.captureStats();
        return stats.overruns + stats.underruns;
    }
    monotrypt::usb::CaptureStats captureStats() const noexcept {
        return driver_.captureStats();
    }
    monotrypt::usb::ImplicitFeedbackStats transportStats() const noexcept {
        return driver_.implicitFeedbackStats();
    }
    long writtenFrames() const noexcept { return driver_.writtenFrames(); }
    long playedFrames() const noexcept { return driver_.playedFrames(); }
    int32_t eventThreadTid() const noexcept {
        return driver_.eventThreadTid();
    }

private:
    template <int Bits>
    void packPcm(float value, uint8_t* out) const noexcept {
        int32_t sample;
        if constexpr (Bits == 16) {
            sample = value >= 1.0f ? 32767 : value <= -1.0f ? -32768
                : static_cast<int32_t>(value * 32767.0f);
        } else if constexpr (Bits == 24) {
            sample = value >= 1.0f ? 0x7FFFFF : value <= -1.0f ? -0x800000
                : static_cast<int32_t>(value * 8388607.0f);
        } else {
            sample = value >= 1.0f ? std::numeric_limits<int32_t>::max()
                : value <= -1.0f ? std::numeric_limits<int32_t>::min()
                : static_cast<int32_t>(value * 2147483647.0f);
        }
        constexpr int validBytes = (Bits + 7) / 8;
        const int shift = 8 * (formatBytes_ - validBytes);
        const uint32_t subslot = static_cast<uint32_t>(sample) << shift;
        for (int byte = 0; byte < formatBytes_; ++byte) {
            out[byte] = static_cast<uint8_t>(subslot >> (8 * byte));
        }
    }

    template <int Bits>
    void packStereoRun(
            uint8_t* destination, int frames,
            const float* left, const float* right) const noexcept {
        if (frames <= 0) return;
        const int frameStride = deviceChannels_ * formatBytes_;
        if (deviceChannels_ != kChannels) {
            std::memset(
                destination, 0,
                static_cast<size_t>(frames) * frameStride);
        }
        const size_t leftOffset =
            static_cast<size_t>(outputPair_ * 2) * formatBytes_;
        const size_t rightOffset = leftOffset + formatBytes_;
        for (int frame = 0; frame < frames; ++frame) {
            uint8_t* output =
                destination + static_cast<size_t>(frame) * frameStride;
            packPcm<Bits>(left[frame], output + leftOffset);
            packPcm<Bits>(right[frame], output + rightOffset);
        }
    }

    template <int Bits>
    void packPlaybackRegion(
            const monotrypt::usb::LibusbUacDriver::PlaybackWriteRegion& region,
            const float* left, const float* right) const noexcept {
        const size_t stride = static_cast<size_t>(region.frameStride);
        const int firstFrames =
            static_cast<int>(region.firstBytes / stride);
        packStereoRun<Bits>(region.first, firstFrames, left, right);

        int sourceFrame = firstFrames;
        size_t secondOffset = 0;
        const size_t splitBytes =
            region.firstBytes - static_cast<size_t>(firstFrames) * stride;
        if (splitBytes > 0) {
            uint8_t splitFrame[kMaxDeviceChannels * kMaxSubslotBytes]{};
            packStereoRun<Bits>(
                splitFrame, 1, left + sourceFrame, right + sourceFrame);
            std::memcpy(
                region.first + static_cast<size_t>(firstFrames) * stride,
                splitFrame, splitBytes);
            std::memcpy(
                region.second, splitFrame + splitBytes, stride - splitBytes);
            ++sourceFrame;
            secondOffset = stride - splitBytes;
        }
        const int remaining = region.frames - sourceFrame;
        if (remaining > 0) {
            packStereoRun<Bits>(
                region.second + secondOffset, remaining,
                left + sourceFrame, right + sourceFrame);
        }
    }


    template <int Bits>
    static float unpackPcm(
            const uint8_t* input, float scale) noexcept {
        if constexpr (Bits == 16) {
            const uint32_t bits =
                static_cast<uint32_t>(input[0]) |
                (static_cast<uint32_t>(input[1]) << 8);
            const uint32_t extended =
                (bits & 0x8000u) ? bits | 0xffff0000u : bits;
            return static_cast<float>(
                static_cast<int32_t>(extended)) / scale;
        } else if constexpr (Bits == 24) {
            uint32_t bits =
                static_cast<uint32_t>(input[0]) |
                (static_cast<uint32_t>(input[1]) << 8) |
                (static_cast<uint32_t>(input[2]) << 16);
            if (bits & 0x00800000u) bits |= 0xff000000u;
            return static_cast<float>(static_cast<int32_t>(bits)) / scale;
        } else {
            const uint32_t bits =
                static_cast<uint32_t>(input[0]) |
                (static_cast<uint32_t>(input[1]) << 8) |
                (static_cast<uint32_t>(input[2]) << 16) |
                (static_cast<uint32_t>(input[3]) << 24);
            return static_cast<float>(static_cast<int32_t>(bits)) / scale;
        }
    }

    template <int Bits>
    void unpackCaptureRun(
            const uint8_t* source, int frames, int frameStride,
            int sampleOffset, float* destination) const noexcept {
        constexpr float scale = Bits == 16 ? 32768.0f :
                                Bits == 24 ? 8388608.0f : 2147483648.0f;
        for (int frame = 0; frame < frames; ++frame) {
            destination[frame] = unpackPcm<Bits>(
                source + static_cast<size_t>(frame) * frameStride +
                    sampleOffset,
                scale);
        }
    }

    template <int Bits>
    void unpackCaptureChannel(
            const monotrypt::usb::LibusbUacDriver::CaptureReadRegion& region,
            int sampleOffset, float* destination, int frames) const noexcept {
        if (frames <= 0 || region.frameStride <= 0) return;
        const size_t stride = static_cast<size_t>(region.frameStride);
        const int firstFrames = std::min(frames,
            static_cast<int>(region.firstBytes / stride));
        unpackCaptureRun<Bits>(region.first, firstFrames, region.frameStride,
                               sampleOffset, destination);
        int out = firstFrames;
        const size_t splitBytes = region.firstBytes -
            static_cast<size_t>(firstFrames) * stride;
        size_t secondOffset = 0;
        if (splitBytes > 0 && out < frames) {
            uint8_t splitFrame[kMaxDeviceChannels * kMaxSubslotBytes]{};
            std::memcpy(splitFrame, region.first +
                static_cast<size_t>(firstFrames) * stride, splitBytes);
            std::memcpy(splitFrame + splitBytes, region.second,
                        stride - splitBytes);
            unpackCaptureRun<Bits>(splitFrame, 1, region.frameStride,
                                   sampleOffset, destination + out);
            ++out;
            secondOffset = stride - splitBytes;
        }
        if (out < frames) unpackCaptureRun<Bits>(region.second + secondOffset,
            frames - out, region.frameStride, sampleOffset, destination + out);
    }

    int deviceChannels_ = kChannels;
    int outputPair_ = 0;
    mutable std::mutex errorMutex_;
    std::string startErrorDetail_;
    monotrypt::usb::LibusbUacDriver driver_;
    int formatBits_ = kBitsPerSample;
    int formatBytes_ = 4;
    std::atomic<bool> accepting_{false};
    std::atomic<bool> streaming_{false};
    std::atomic<uint32_t> activeWriters_{0};
};

} // namespace guitarrackcraft

#endif // GUITARRACKCRAFT_DIRECT_USB_OUTPUT_H
