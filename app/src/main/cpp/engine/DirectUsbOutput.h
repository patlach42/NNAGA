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

// Playback-only USB sink. All methods called by the Oboe callback are bounded
// and lock-free; open/start/stop are control-thread operations.
class DirectUsbOutput {
public:
    static constexpr int kSampleRate = 48000;
    static constexpr int kBitsPerSample = 32;
    static constexpr int kChannels = 2;
    static constexpr int kMaxDeviceChannels = 8;
    static constexpr int kMaxFramesPerWrite = 8192;

    DirectUsbOutput()
        : pcm_(static_cast<size_t>(kMaxFramesPerWrite) * kMaxDeviceChannels * 4) {}
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

    bool start(int sampleRate, int bitsPerSample, int bytesPerSample, int channels) {
        if (!driver_.isOpen() || sampleRate <= 0 ||
            (bitsPerSample != 16 && bitsPerSample != 24 && bitsPerSample != 32) ||
            bytesPerSample < (bitsPerSample + 7) / 8 || bytesPerSample > 4 ||
            channels < kChannels || channels > kMaxDeviceChannels) return false;
        stop();
        if (!driver_.start(sampleRate, bitsPerSample, channels, bytesPerSample))
            return false;
        formatBits_ = bitsPerSample;
        formatBytes_ = bytesPerSample;
        deviceChannels_ = driver_.currentFormat().channels;
        if (deviceChannels_ < kChannels || deviceChannels_ > kMaxDeviceChannels) {
            driver_.stop();
            return false;
        }
        accepting_.store(true, std::memory_order_release);
        streaming_.store(true, std::memory_order_release);
        return true;
    }

    std::vector<monotrypt::usb::UsbFormatCandidate> enumerateFormats() {
        return driver_.enumerateFormats();
    }

    void stop() {
        accepting_.store(false, std::memory_order_release);
        while (activeWriters_.load(std::memory_order_acquire) != 0) {
            // Control thread only: callback never enters this path.
            std::this_thread::yield();
        }
        streaming_.store(false, std::memory_order_release);
        driver_.stop();
    }

    bool isStreaming() const {
        return streaming_.load(std::memory_order_acquire) && driver_.isStreaming();
    }

    // Called only from AudioEngine::onAudioReady. No allocation, locks, I/O,
    // or blocking calls occur here.
    void writeStereo(const float* left, const float* right, int frames) noexcept {
        if (!left || !right || frames <= 0 ||
            !accepting_.load(std::memory_order_acquire)) return;
        activeWriters_.fetch_add(1, std::memory_order_acq_rel);
        if (!accepting_.load(std::memory_order_acquire)) {
            activeWriters_.fetch_sub(1, std::memory_order_release);
            return;
        }
        int offset = 0;
        while (offset < frames) {
            const int count = (frames - offset > kMaxFramesPerWrite)
                    ? kMaxFramesPerWrite : (frames - offset);
            const int frameStride = deviceChannels_ * formatBytes_;
            for (int i = 0; i < count; ++i) {
                uint8_t* frame = pcm_.data() + static_cast<size_t>(i) * frameStride;
                for (int channel = 0; channel < deviceChannels_; ++channel) {
                    packPcm((channel & 1) == 0 ? left[offset + i] : right[offset + i],
                            frame + channel * formatBytes_);
                }
            }
            const int written = driver_.writePcm(pcm_.data(), count);
            if (written <= 0) break;
            offset += written;
            if (written < count) break;
        }
        activeWriters_.fetch_sub(1, std::memory_order_release);
    }

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
    monotrypt::usb::LibusbUacDriver driver_;
    std::vector<uint8_t> pcm_;
    int formatBits_ = kBitsPerSample;
    int formatBytes_ = 4;
    std::atomic<bool> accepting_{false};
    std::atomic<bool> streaming_{false};
    std::atomic<uint32_t> activeWriters_{0};
};

} // namespace guitarrackcraft

#endif // GUITARRACKCRAFT_DIRECT_USB_OUTPUT_H
