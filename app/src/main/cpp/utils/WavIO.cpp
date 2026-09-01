/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * This file is part of NNAGA.
 *
 * NNAGA is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * NNAGA is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with NNAGA. If not, see <https://www.gnu.org/licenses/>.
 */

#include "WavIO.h"
#include <android/log.h>
#include <array>
#include <cerrno>
#include <algorithm>
#include <cstring>
#include <fstream>
#include <limits>
#include <fcntl.h>
#include <unistd.h>

#define LOG_TAG "WavIO"
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace guitarrackcraft {

bool readWavFile(const std::string& path,
                 std::vector<float>& samples,
                 uint32_t& sampleRate,
                 uint32_t& numChannels) {
    samples.clear();
    sampleRate = 0;
    numChannels = 0;
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        LOGE("Cannot open file: %s", path.c_str());
        return false;
    }

    file.seekg(0, std::ios::end);
    const std::streamoff fileSize = file.tellg();
    file.seekg(0, std::ios::beg);
    if (fileSize < 12) return false;

    char riff[4];
    uint32_t riffSize = 0;
    char wave[4];
    if (!file.read(riff, sizeof(riff)) ||
        !file.read(reinterpret_cast<char*>(&riffSize), sizeof(riffSize)) ||
        !file.read(wave, sizeof(wave)) ||
        std::memcmp(riff, "RIFF", 4) != 0 ||
        std::memcmp(wave, "WAVE", 4) != 0) {
        LOGE("Invalid WAV file format");
        return false;
    }

    uint16_t audioFormat = 0;
    uint16_t channels = 0;
    uint32_t rate = 0;
    uint32_t byteRate = 0;
    uint16_t blockAlign = 0;
    uint16_t bitsPerSample = 0;
    bool fmtFound = false;
    uint32_t dataSize = 0;
    std::streamoff dataOffset = -1;

    while (true) {
        const std::streamoff chunkHeaderOffset = file.tellg();
        if (chunkHeaderOffset < 0 || fileSize - chunkHeaderOffset < 8) break;

        char chunkId[4];
        uint32_t chunkSize = 0;
        if (!file.read(chunkId, sizeof(chunkId)) ||
            !file.read(reinterpret_cast<char*>(&chunkSize), sizeof(chunkSize))) {
            return false;
        }
        const std::streamoff payloadOffset = file.tellg();
        const std::streamoff remaining = fileSize - payloadOffset;
        const uint64_t paddedChunkSize =
            static_cast<uint64_t>(chunkSize) + (chunkSize & 1U);
        if (remaining < 0 || paddedChunkSize > static_cast<uint64_t>(remaining)) return false;

        if (std::memcmp(chunkId, "fmt ", 4) == 0 && !fmtFound) {
            if (chunkSize < 16 ||
                !file.read(reinterpret_cast<char*>(&audioFormat), sizeof(audioFormat)) ||
                !file.read(reinterpret_cast<char*>(&channels), sizeof(channels)) ||
                !file.read(reinterpret_cast<char*>(&rate), sizeof(rate)) ||
                !file.read(reinterpret_cast<char*>(&byteRate), sizeof(byteRate)) ||
                !file.read(reinterpret_cast<char*>(&blockAlign), sizeof(blockAlign)) ||
                !file.read(reinterpret_cast<char*>(&bitsPerSample), sizeof(bitsPerSample))) {
                return false;
            }
            fmtFound = true;
        } else if (std::memcmp(chunkId, "data", 4) == 0 && dataOffset < 0) {
            dataOffset = payloadOffset;
            dataSize = chunkSize;
        }

        if (fmtFound && dataOffset >= 0) break;
        file.seekg(payloadOffset + static_cast<std::streamoff>(paddedChunkSize));
        if (!file) return false;
    }

    if (!fmtFound || dataOffset < 0 || audioFormat != 1 ||
        (bitsPerSample != 16 && bitsPerSample != 24 && bitsPerSample != 32) ||
        rate == 0 || (channels != 1 && channels != 2)) {
        LOGE("Invalid WAV file format");
        return false;
    }
    const uint32_t bytesPerSample = bitsPerSample / 8;
    if (blockAlign != channels * bytesPerSample) return false;
    if (dataSize == 0 || dataSize % blockAlign != 0 ||
        dataSize > static_cast<uint64_t>(fileSize - dataOffset)) return false;
    const size_t count = dataSize / bytesPerSample;
    if (count == 0 || count > kMaxWavSamples) return false;
    file.seekg(dataOffset);
    if (!file) return false;

    try {
        samples.resize(count);
        if (bitsPerSample == 16) {
            std::vector<int16_t> source(count);
            file.read(reinterpret_cast<char*>(source.data()), dataSize);
            if (file.gcount() != static_cast<std::streamsize>(dataSize)) { samples.clear(); return false; }
            for (size_t index = 0; index < count; ++index) samples[index] = source[index] / kInt16MaxF;
        } else if (bitsPerSample == 24) {
            std::vector<uint8_t> source(dataSize);
            file.read(reinterpret_cast<char*>(source.data()), dataSize);
            if (file.gcount() != static_cast<std::streamsize>(dataSize)) { samples.clear(); return false; }
            for (size_t index = 0; index < count; ++index) {
                const size_t offset = index * 3;
                const uint32_t packed = static_cast<uint32_t>(source[offset]) |
                                        (static_cast<uint32_t>(source[offset + 1]) << 8) |
                                        (static_cast<uint32_t>(source[offset + 2]) << 16);
                const int32_t sample = (packed & 0x00800000U) != 0
                    ? static_cast<int32_t>(packed | 0xFF000000U)
                    : static_cast<int32_t>(packed);
                samples[index] = sample / 8388608.0f;
            }
        } else {
            std::vector<int32_t> source(count);
            file.read(reinterpret_cast<char*>(source.data()), dataSize);
            if (file.gcount() != static_cast<std::streamsize>(dataSize)) { samples.clear(); return false; }
            for (size_t index = 0; index < count; ++index) samples[index] = source[index] / kInt32MaxF;
        }
    } catch (const std::exception&) {
        samples.clear();
        return false;
    }
    sampleRate = rate;
    numChannels = channels;
    return true;
}

bool writeWavFile(const std::string& path,
                  const std::vector<float>& samples,
                  uint32_t sampleRate,
                  uint32_t numChannels) {
    if (sampleRate == 0 || (numChannels != 1 && numChannels != 2) ||
        samples.empty() || samples.size() > kMaxWavSamples ||
        samples.size() > (std::numeric_limits<uint32_t>::max() / 2U)) {
        return false;
    }
    std::ofstream file(path, std::ios::binary);
    if (!file.is_open()) {
        LOGE("Cannot create file: %s", path.c_str());
        return false;
    }

    uint16_t bitsPerSample = 16;
    uint32_t dataSize = static_cast<uint32_t>(samples.size() * (bitsPerSample / 8));
    uint32_t fileSize = 36 + dataSize;

    WavHeader header{};
    std::memcpy(header.riff, "RIFF", 4);
    header.fileSize = fileSize - 8;
    std::memcpy(header.wave, "WAVE", 4);
    std::memcpy(header.fmt, "fmt ", 4);
    header.fmtSize = 16;
    header.audioFormat = 1;
    header.numChannels = static_cast<uint16_t>(numChannels);
    header.sampleRate = sampleRate;
    header.bitsPerSample = bitsPerSample;
    header.blockAlign = header.numChannels * (bitsPerSample / 8);
    header.byteRate = sampleRate * numChannels * bitsPerSample / 8;
    std::memcpy(header.data, "data", 4);
    header.dataSize = dataSize;
    file.write(reinterpret_cast<const char*>(&header), sizeof(WavHeader));
    if (!file) return false;
    

    std::vector<int16_t> intSamples(samples.size());
    for (size_t i = 0; i < samples.size(); ++i) {
        float clamped = std::max(-1.0f, std::min(1.0f, samples[i]));
        intSamples[i] = static_cast<int16_t>(clamped * 32767.0f);
    }

    file.write(reinterpret_cast<const char*>(intSamples.data()), dataSize);
    return file.good();
}

bool writeStereoWavFile(const std::string& path,
                        const std::vector<float>& left,
                        const std::vector<float>& right,
                        uint32_t sampleRate) {
    if (sampleRate == 0 || left.empty() || left.size() != right.size() ||
        left.size() > (kMaxWavSamples / 2U) ||
        left.size() > (std::numeric_limits<uint32_t>::max() / 4U)) {
        return false;
    }

    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) {
        LOGE("Cannot create file: %s", path.c_str());
        return false;
    }
    const auto writeAll = [&](const void* source, size_t bytes) {
        const auto* cursor = static_cast<const uint8_t*>(source);
        while (bytes > 0) {
            const ssize_t written = ::write(fd, cursor, bytes);
            if (written < 0 && errno == EINTR) continue;
            if (written <= 0) return false;
            cursor += static_cast<size_t>(written);
            bytes -= static_cast<size_t>(written);
        }
        return true;
    };

    constexpr uint16_t kChannels = 2;
    constexpr uint16_t kBitsPerSample = 16;
    const uint32_t dataSize = static_cast<uint32_t>(left.size() * 4U);
    WavHeader header{};
    std::memcpy(header.riff, "RIFF", 4);
    header.fileSize = 36U + dataSize;
    std::memcpy(header.wave, "WAVE", 4);
    std::memcpy(header.fmt, "fmt ", 4);
    header.fmtSize = 16;
    header.audioFormat = 1;
    header.numChannels = kChannels;
    header.sampleRate = sampleRate;
    header.bitsPerSample = kBitsPerSample;
    header.blockAlign = kChannels * (kBitsPerSample / 8U);
    header.byteRate = sampleRate * header.blockAlign;
    std::memcpy(header.data, "data", 4);
    header.dataSize = dataSize;

    bool ok = writeAll(&header, sizeof(header));
    std::array<int16_t, 2048> chunk{};
    for (size_t offset = 0; ok && offset < left.size();) {
        const size_t frames = std::min<size_t>(chunk.size() / 2U, left.size() - offset);
        for (size_t frame = 0; frame < frames; ++frame) {
            const float leftSample = std::clamp(left[offset + frame], -1.0f, 1.0f);
            const float rightSample = std::clamp(right[offset + frame], -1.0f, 1.0f);
            chunk[frame * 2U] = static_cast<int16_t>(leftSample * 32767.0f);
            chunk[frame * 2U + 1U] = static_cast<int16_t>(rightSample * 32767.0f);
        }
        ok = writeAll(chunk.data(), frames * 2U * sizeof(int16_t));
        offset += frames;
    }
    if (ok) ok = ::fsync(fd) == 0;
    const int closeResult = ::close(fd);
    return ok && closeResult == 0;
}

} // namespace guitarrackcraft
