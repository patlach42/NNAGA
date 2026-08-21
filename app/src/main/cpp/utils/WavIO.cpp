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
#include <algorithm>
#include <cstring>
#include <fstream>

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
    if (fileSize < static_cast<std::streamoff>(sizeof(WavHeader))) return false;

    WavHeader header{};
    file.read(reinterpret_cast<char*>(&header), sizeof(header));
    if (!file || std::memcmp(header.riff, "RIFF", 4) != 0 ||
        std::memcmp(header.wave, "WAVE", 4) != 0 ||
        std::memcmp(header.fmt, "fmt ", 4) != 0 || header.audioFormat != 1 ||
        (header.bitsPerSample != 16 && header.bitsPerSample != 24 && header.bitsPerSample != 32) ||
        header.sampleRate == 0 || (header.numChannels != 1 && header.numChannels != 2)) {
        LOGE("Invalid WAV file format");
        return false;
    }
    const uint32_t bytesPerSample = header.bitsPerSample / 8;
    if (header.blockAlign != header.numChannels * bytesPerSample) return false;
    if (header.fmtSize < 16) return false;
    if (header.fmtSize > 16) {
        const auto extra = static_cast<std::streamoff>(header.fmtSize - 16);
        if (extra > fileSize - file.tellg()) return false;
        file.seekg(extra, std::ios::cur);
    }

    uint32_t dataSize = 0;
    if (std::memcmp(header.data, "data", 4) == 0) {
        dataSize = header.dataSize;
    } else {
        char chunkId[4];
        uint32_t chunkSize = 0;
        while (file.read(chunkId, sizeof(chunkId)) &&
               file.read(reinterpret_cast<char*>(&chunkSize), sizeof(chunkSize))) {
            const auto remaining = fileSize - file.tellg();
            if (chunkSize > remaining) return false;
            if (std::memcmp(chunkId, "data", 4) == 0) {
                dataSize = chunkSize;
                break;
            }
            file.seekg(static_cast<std::streamoff>(chunkSize) + (chunkSize & 1U), std::ios::cur);
        }
    }
    if (dataSize == 0 || dataSize % header.blockAlign != 0 ||
        dataSize > static_cast<uint64_t>(fileSize - file.tellg())) return false;
    const size_t count = dataSize / bytesPerSample;
    if (count == 0 || count > kMaxWavSamples) return false;

    try {
        samples.resize(count);
        if (header.bitsPerSample == 16) {
            std::vector<int16_t> source(count);
            file.read(reinterpret_cast<char*>(source.data()), dataSize);
            if (file.gcount() != static_cast<std::streamsize>(dataSize)) { samples.clear(); return false; }
            for (size_t index = 0; index < count; ++index) samples[index] = source[index] / kInt16MaxF;
        } else if (header.bitsPerSample == 24) {
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
    sampleRate = header.sampleRate;
    numChannels = header.numChannels;
    return true;
}

bool writeWavFile(const std::string& path,
                  const std::vector<float>& samples,
                  uint32_t sampleRate,
                  uint32_t numChannels) {
    std::ofstream file(path, std::ios::binary);
    if (!file.is_open()) {
        LOGE("Cannot create file: %s", path.c_str());
        return false;
    }

    uint16_t bitsPerSample = 16;
    uint32_t dataSize = static_cast<uint32_t>(samples.size() * (bitsPerSample / 8));
    uint32_t fileSize = 36 + dataSize;

    WavHeader header;
    std::memcpy(header.riff, "RIFF", 4);
    header.fileSize = fileSize - 8;
    std::memcpy(header.wave, "WAVE", 4);
    std::memcpy(header.fmt, "fmt ", 4);
    header.fmtSize = 16;
    header.audioFormat = 1;
    header.numChannels = static_cast<uint16_t>(numChannels);
    header.sampleRate = sampleRate;
    header.byteRate = sampleRate * numChannels * bitsPerSample / 8;
    header.blockAlign = static_cast<uint16_t>(numChannels * bitsPerSample / 8);
    header.bitsPerSample = bitsPerSample;
    std::memcpy(header.data, "data", 4);
    header.dataSize = dataSize;

    file.write(reinterpret_cast<const char*>(&header), sizeof(WavHeader));

    std::vector<int16_t> intSamples(samples.size());
    for (size_t i = 0; i < samples.size(); ++i) {
        float clamped = std::max(-1.0f, std::min(1.0f, samples[i]));
        intSamples[i] = static_cast<int16_t>(clamped * 32767.0f);
    }

    file.write(reinterpret_cast<const char*>(intSamples.data()), dataSize);

    return true;
}

} // namespace guitarrackcraft
