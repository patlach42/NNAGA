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
#include <limits>
#include <thread>
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

    DirectUsbOutput()
        : pcm_(static_cast<size_t>(kMaxFramesPerWrite) * kMaxDeviceChannels * 4),
          capturePcm_(static_cast<size_t>(kMaxFramesPerWrite) * kMaxDeviceChannels * 4) {}
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
               int inputChannel, int outputPair) {
        if (!driver_.isOpen() || sampleRate <= 0 ||
            (bitsPerSample != 16 && bitsPerSample != 24 && bitsPerSample != 32) ||
            bytesPerSample < (bitsPerSample + 7) / 8 ||
            bytesPerSample > kMaxSubslotBytes ||
            channels < kChannels || channels > kMaxDeviceChannels ||
            inputChannel < 0 || inputChannel >= captureChannelCount() ||
            outputPair < 0 || outputPair * 2 + 1 >= channels) return false;
        stop();
        if (!driver_.startDuplex(sampleRate, bitsPerSample, channels, bytesPerSample))
            return false;
        formatBits_ = bitsPerSample;
        formatBytes_ = bytesPerSample;
        deviceChannels_ = driver_.currentFormat().channels;
        const auto& capture = driver_.currentCaptureFormat();
        if (deviceChannels_ < kChannels || deviceChannels_ > kMaxDeviceChannels ||
            capture.channels <= 0 || capture.channels > kMaxDeviceChannels ||
            capture.bytesPerSample <= 0 ||
            capture.bytesPerSample > kMaxSubslotBytes ||
            inputChannel >= capture.channels ||
            outputPair * 2 + 1 >= deviceChannels_) {
            driver_.stop();
            return false;
        }
        inputChannel_ = inputChannel;
        outputPair_ = outputPair;
        accepting_.store(true, std::memory_order_release);
        streaming_.store(true, std::memory_order_release);
        return true;
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
            const int count = (frames - offset > kMaxFramesPerWrite)
                    ? kMaxFramesPerWrite : (frames - offset);
            const int frameStride = deviceChannels_ * formatBytes_;
            for (int i = 0; i < count; ++i) {
                uint8_t* frame = pcm_.data() + static_cast<size_t>(i) * frameStride;
                const int selectedLeft = outputPair_ * 2;
                for (int channel = 0; channel < deviceChannels_; ++channel) {
                    const float value = channel == selectedLeft ? left[offset + i] :
                                        channel == selectedLeft + 1 ? right[offset + i] : 0.0f;
                    packPcm(value, frame + channel * formatBytes_);
                }
            }
            const int written = driver_.writePcm(pcm_.data(), count);
            if (written <= 0) break;
            offset += written;
            if (written < count) break;
        }
        activeWriters_.fetch_sub(1, std::memory_order_release);
        return offset;
    }

    // Reads the selected capture channel as normalized float. Non-blocking
    // and allocation-free; missing frames are zero-filled.
    int readMonoInput(float* dst, int frames) noexcept {
        if (!dst || frames <= 0) return 0;
        if (frames > kMaxFramesPerWrite) frames = kMaxFramesPerWrite;
        const auto& f = driver_.currentCaptureFormat();
        const int stride = f.channels * f.bytesPerSample;
        const int got = (stride > 0) ? driver_.readCapturePcm(capturePcm_.data(), frames) : 0;
        for (int i = 0; i < frames; ++i) {
            if (i >= got) { dst[i] = 0.0f; continue; }
            const uint8_t* p = capturePcm_.data() +
                static_cast<size_t>(i) * stride +
                static_cast<size_t>(inputChannel_) * f.bytesPerSample;
            uint32_t raw = 0;
            for (int b = 0; b < f.bytesPerSample; ++b)
                raw |= static_cast<uint32_t>(p[b]) << (8 * b);
            const int validBytes = (f.bitsPerSample + 7) / 8;
            const int shift = 8 * (f.bytesPerSample - validBytes);
            int32_t sample = static_cast<int32_t>(raw >> shift);
            const int validBits = f.bitsPerSample;
            if (validBits < 32) {
                const uint32_t sign = 1u << (validBits - 1);
                const uint32_t mask = (1u << validBits) - 1u;
                uint32_t v = static_cast<uint32_t>(sample) & mask;
                if (v & sign) v |= ~mask;
                sample = static_cast<int32_t>(v);
            }
            const float scale = validBits == 16 ? 32768.0f :
                                validBits == 24 ? 8388608.0f : 2147483648.0f;
            dst[i] = static_cast<float>(sample) / scale;
        }
        return got;
    }

    int captureAvailableFrames() const noexcept {
        return driver_.captureAvailableFrames();
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
    void setGraphQuantum(int frames) noexcept { driver_.setGraphQuantum(frames); }
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

private:
    void packPcm(float value, uint8_t* out) const noexcept {
        int32_t sample = 0;
        if (formatBits_ == 16) {
            sample = value >= 1.0f ? 32767 : value <= -1.0f ? -32768
                : static_cast<int32_t>(value * 32767.0f);
        } else if (formatBits_ == 24) {
            sample = value >= 1.0f ? 0x7FFFFF : value <= -1.0f ? -0x800000
                : static_cast<int32_t>(value * 8388607.0f);
        } else {
            sample = value >= 1.0f ? std::numeric_limits<int32_t>::max()
                : value <= -1.0f ? std::numeric_limits<int32_t>::min()
                : static_cast<int32_t>(value * 2147483647.0f);
        }

        // USB Audio PCM is left-justified in the audio subslot. In
        // particular, iD4 advertises 24 valid bits in a 4-byte subslot:
        // sending a sign-extended S24_LE value makes it 48 dB too quiet.
        const int validBytes = (formatBits_ + 7) / 8;
        const int shift = 8 * (formatBytes_ - validBytes);
        const uint32_t subslot = static_cast<uint32_t>(sample) << shift;
        for (int i = 0; i < formatBytes_; ++i)
            out[i] = static_cast<uint8_t>(subslot >> (8 * i));
    }
    int deviceChannels_ = kChannels;
    int inputChannel_ = 0;
    int outputPair_ = 0;
    monotrypt::usb::LibusbUacDriver driver_;
    std::vector<uint8_t> pcm_;
    std::vector<uint8_t> capturePcm_;
    int formatBits_ = kBitsPerSample;
    int formatBytes_ = 4;
    std::atomic<bool> accepting_{false};
    std::atomic<bool> streaming_{false};
    std::atomic<uint32_t> activeWriters_{0};
};

} // namespace guitarrackcraft

#endif // GUITARRACKCRAFT_DIRECT_USB_OUTPUT_H
