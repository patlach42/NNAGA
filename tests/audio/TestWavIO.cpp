#include <gtest/gtest.h>

#include "utils/WavIO.h"

#include <cstdint>
#include <fstream>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

class TempWavFile {
public:
    TempWavFile() {
        char pattern[] = "/tmp/grc_wavio_tests_XXXXXX";
        const int fd = ::mkstemp(pattern);
        if (fd >= 0) {
            ::close(fd);
            path_ = pattern;
        }
    }

    ~TempWavFile() {
        if (!path_.empty()) ::unlink(path_.c_str());
    }

    bool write(const std::vector<uint8_t>& bytes) const {
        std::ofstream file(path_, std::ios::binary | std::ios::trunc);
        if (!file.is_open()) return false;
        file.write(reinterpret_cast<const char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
        return file.good();
    }

    const std::string& path() const { return path_; }

private:
    std::string path_;
};

void appendU16(std::vector<uint8_t>& bytes, uint16_t value) {
    bytes.push_back(static_cast<uint8_t>(value));
    bytes.push_back(static_cast<uint8_t>(value >> 8));
}

void appendU32(std::vector<uint8_t>& bytes, uint32_t value) {
    bytes.push_back(static_cast<uint8_t>(value));
    bytes.push_back(static_cast<uint8_t>(value >> 8));
    bytes.push_back(static_cast<uint8_t>(value >> 16));
    bytes.push_back(static_cast<uint8_t>(value >> 24));
}

void appendTag(std::vector<uint8_t>& bytes, const char (&tag)[5]) {
    bytes.insert(bytes.end(), tag, tag + 4);
}

std::vector<uint8_t> makePcm24Wav(const std::vector<int32_t>& samples,
                                  uint16_t numChannels,
                                  uint32_t sampleRate = 48000) {
    constexpr uint16_t bytesPerSample = 3;
    const uint32_t dataSize = static_cast<uint32_t>(samples.size() * bytesPerSample);
    const uint16_t blockAlign = static_cast<uint16_t>(numChannels * bytesPerSample);

    std::vector<uint8_t> bytes;
    bytes.reserve(44 + dataSize);
    appendTag(bytes, "RIFF");
    appendU32(bytes, 36 + dataSize);
    appendTag(bytes, "WAVE");
    appendTag(bytes, "fmt ");
    appendU32(bytes, 16);
    appendU16(bytes, 1);
    appendU16(bytes, numChannels);
    appendU32(bytes, sampleRate);
    appendU32(bytes, sampleRate * blockAlign);
    appendU16(bytes, blockAlign);
    appendU16(bytes, 24);
    appendTag(bytes, "data");
    appendU32(bytes, dataSize);

    for (const int32_t sample : samples) {
        const uint32_t packed = static_cast<uint32_t>(sample) & 0x00ffffffU;
        bytes.push_back(static_cast<uint8_t>(packed));
        bytes.push_back(static_cast<uint8_t>(packed >> 8));
        bytes.push_back(static_cast<uint8_t>(packed >> 16));
    }
    return bytes;
}

std::vector<uint8_t> makePcm24WavWithListMetadata(
    const std::vector<int32_t>& samples,
    uint16_t numChannels,
    uint32_t sampleRate) {
    constexpr uint16_t bytesPerSample = 3;
    const uint32_t dataSize = static_cast<uint32_t>(samples.size() * bytesPerSample);
    const uint16_t blockAlign = static_cast<uint16_t>(numChannels * bytesPerSample);
    // A LIST metadata chunk may have an odd payload and must be RIFF-padded.
    const std::vector<uint8_t> listPayload = {'I', 'N', 'F', 'O', 0x01};
    const uint32_t listStorageSize =
        static_cast<uint32_t>(listPayload.size() + (listPayload.size() & 1U));
    const uint32_t riffSize = 4 + (8 + 16) + (8 + listStorageSize) + (8 + dataSize);

    std::vector<uint8_t> bytes;
    bytes.reserve(12 + 8 + 16 + 8 + listStorageSize + 8 + dataSize);
    appendTag(bytes, "RIFF");
    appendU32(bytes, riffSize);
    appendTag(bytes, "WAVE");
    appendTag(bytes, "fmt ");
    appendU32(bytes, 16);
    appendU16(bytes, 1);
    appendU16(bytes, numChannels);
    appendU32(bytes, sampleRate);
    appendU32(bytes, sampleRate * blockAlign);
    appendU16(bytes, blockAlign);
    appendU16(bytes, 24);
    appendTag(bytes, "LIST");
    appendU32(bytes, static_cast<uint32_t>(listPayload.size()));
    bytes.insert(bytes.end(), listPayload.begin(), listPayload.end());
    bytes.push_back(0);
    appendTag(bytes, "data");
    appendU32(bytes, dataSize);

    for (const int32_t sample : samples) {
        const uint32_t packed = static_cast<uint32_t>(sample) & 0x00ffffffU;
        bytes.push_back(static_cast<uint8_t>(packed));
        bytes.push_back(static_cast<uint8_t>(packed >> 8));
        bytes.push_back(static_cast<uint8_t>(packed >> 16));
    }
    return bytes;
}

} // namespace

TEST(WavIOPcm24Test, DecodesStereoPackedLittleEndianSamplesAndMetadata) {
    // Distinct byte patterns make a byte-order regression observable, while the
    // extrema and -2 exercise the signed 24-bit boundaries and sign extension.
    const std::vector<int32_t> encoded = {0x010203, -2, 0x7fffff, -0x800000};
    TempWavFile wav;
    ASSERT_FALSE(wav.path().empty());
    ASSERT_TRUE(wav.write(makePcm24Wav(encoded, 2)));

    std::vector<float> samples;
    uint32_t sampleRate = 0;
    uint32_t numChannels = 0;
    ASSERT_TRUE(guitarrackcraft::readWavFile(wav.path(), samples, sampleRate, numChannels));

    EXPECT_EQ(sampleRate, 48000u);
    EXPECT_EQ(numChannels, 2u);
    ASSERT_EQ(samples.size(), encoded.size());
    constexpr float divisor = 8388608.0f;
    EXPECT_FLOAT_EQ(samples[0], 66051.0f / divisor);
    EXPECT_FLOAT_EQ(samples[1], -2.0f / divisor);
    EXPECT_FLOAT_EQ(samples[2], 8388607.0f / divisor);
    EXPECT_FLOAT_EQ(samples[3], -1.0f);
}

TEST(WavIOPcm24Test, DecodesStereoPcm24AfterListMetadataChunk) {
    const std::vector<int32_t> encoded = {0x010203, -2, 0x7fffff, -0x800000};
    TempWavFile wav;
    ASSERT_FALSE(wav.path().empty());
    ASSERT_TRUE(wav.write(makePcm24WavWithListMetadata(encoded, 2, 44100)));

    std::vector<float> samples;
    uint32_t sampleRate = 0;
    uint32_t numChannels = 0;
    ASSERT_TRUE(guitarrackcraft::readWavFile(wav.path(), samples, sampleRate, numChannels));

    EXPECT_EQ(sampleRate, 44100u);
    EXPECT_EQ(numChannels, 2u);
    ASSERT_EQ(samples.size(), encoded.size());
    constexpr float divisor = 8388608.0f;
    EXPECT_FLOAT_EQ(samples[0], 66051.0f / divisor);
    EXPECT_FLOAT_EQ(samples[1], -2.0f / divisor);
    EXPECT_FLOAT_EQ(samples[2], 8388607.0f / divisor);
    EXPECT_FLOAT_EQ(samples[3], -1.0f);
}

TEST(WavIOPcm24Test, RejectsDataChunkWithPartialFrame) {
    const std::vector<uint8_t> wav = makePcm24Wav({1}, 2);
    TempWavFile file;
    ASSERT_FALSE(file.path().empty());
    ASSERT_TRUE(file.write(wav));

    std::vector<float> samples;
    uint32_t sampleRate = 123;
    uint32_t numChannels = 456;
    EXPECT_FALSE(guitarrackcraft::readWavFile(file.path(), samples, sampleRate, numChannels));
}
