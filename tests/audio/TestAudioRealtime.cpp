#include <gtest/gtest.h>

#include "engine/AudioRecorder.h"
#include "plugin/PluginChain.h"

#include "utils/WavIO.h"
#include <array>
#include <atomic>
#include <cstddef>
#include <thread>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <new>
#include <vector>

namespace allocation_probe {

thread_local bool enabled = false;
thread_local std::size_t allocations = 0;

void* allocate(std::size_t size, std::size_t alignment = alignof(std::max_align_t)) {
    void* result = nullptr;
    if (alignment <= alignof(std::max_align_t)) {
        result = std::malloc(size == 0 ? 1 : size);
    } else {
        const std::size_t requested = size == 0 ? 1 : size;
        const std::size_t rounded = (requested + alignment - 1) & ~(alignment - 1);
        result = std::aligned_alloc(alignment, rounded);
    }
    if (result == nullptr) {
        throw std::bad_alloc();
    }
    if (enabled) {
        ++allocations;
    }
    return result;
}

class NoAllocScope {
public:
    NoAllocScope() : wasEnabled_(enabled), previousCount_(allocations) {
        allocations = 0;
        enabled = true;
    }

    ~NoAllocScope() {
        enabled = wasEnabled_;
        if (wasEnabled_) {
            allocations = previousCount_;
        }
    }

    std::size_t count() const { return allocations; }

private:
    bool wasEnabled_;
    std::size_t previousCount_;
};

} // namespace allocation_probe

void* operator new(std::size_t size) {
    return allocation_probe::allocate(size);
}

void* operator new[](std::size_t size) {
    return allocation_probe::allocate(size);
}

void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    try {
        return allocation_probe::allocate(size);
    } catch (...) {
        return nullptr;
    }
}

void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
    try {
        return allocation_probe::allocate(size);
    } catch (...) {
        return nullptr;
    }
}

void* operator new(std::size_t size, std::align_val_t alignment) {
    return allocation_probe::allocate(size, static_cast<std::size_t>(alignment));
}

void* operator new[](std::size_t size, std::align_val_t alignment) {
    return allocation_probe::allocate(size, static_cast<std::size_t>(alignment));
}

void* operator new(std::size_t size, std::align_val_t alignment,
                   const std::nothrow_t&) noexcept {
    try {
        return allocation_probe::allocate(size, static_cast<std::size_t>(alignment));
    } catch (...) {
        return nullptr;
    }
}

void* operator new[](std::size_t size, std::align_val_t alignment,
                     const std::nothrow_t&) noexcept {
    try {
        return allocation_probe::allocate(size, static_cast<std::size_t>(alignment));
    } catch (...) {
        return nullptr;
    }
}

void operator delete(void* pointer) noexcept { std::free(pointer); }
void operator delete[](void* pointer) noexcept { std::free(pointer); }
void operator delete(void* pointer, std::size_t) noexcept { std::free(pointer); }
void operator delete[](void* pointer, std::size_t) noexcept { std::free(pointer); }
void operator delete(void* pointer, std::align_val_t) noexcept { std::free(pointer); }
void operator delete[](void* pointer, std::align_val_t) noexcept { std::free(pointer); }
void operator delete(void* pointer, std::size_t, std::align_val_t) noexcept { std::free(pointer); }
void operator delete[](void* pointer, std::size_t, std::align_val_t) noexcept { std::free(pointer); }

namespace {

using guitarrackcraft::IPlugin;
using guitarrackcraft::PluginChain;

class GainPlugin final : public IPlugin {
public:
    void activate(float, uint32_t) override {}
    void deactivate() override {}

    void process(const float* const* inputs, float* const* outputs,
                 uint32_t numFrames) override {
        for (uint32_t frame = 0; frame < numFrames; ++frame) {
            outputs[0][frame] = inputs[0][frame] * 2.0f;
            outputs[1][frame] = inputs[1][frame] * 2.0f;
        }
    }

    guitarrackcraft::PluginInfo getInfo() const override { return {}; }
    void setParameter(uint32_t, float) override {}
    float getParameter(uint32_t) const override { return 0.0f; }
    uint32_t getNumInputPorts() const override { return 2; }
    uint32_t getNumOutputPorts() const override { return 2; }
};

class PluginChainRealtimeTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_EQ(chain_.addPlugin(std::make_unique<GainPlugin>()), 0);
        chain_.setSampleRate(48000.0f, 1024);
        for (std::size_t i = 0; i < inputLeft_.size(); ++i) {
            inputLeft_[i] = 0.25f;
            inputRight_[i] = -0.5f;
            outputLeft_[i] = -99.0f;
            outputRight_[i] = -99.0f;
        }
    }

    PluginChain chain_;
    std::array<float, 2048> inputLeft_{};
    std::array<float, 2048> inputRight_{};
    std::array<float, 2048> outputLeft_{};
    std::array<float, 2048> outputRight_{};
};

TEST_F(PluginChainRealtimeTest, SupportedFrameQuantaProcessWithoutAllocations) {
    const std::array<uint32_t, 4> frameQuanta = {16, 64, 512, 1024};
    const float* inputs[] = {inputLeft_.data(), inputRight_.data()};
    float* outputs[] = {outputLeft_.data(), outputRight_.data()};

    for (const uint32_t frames : frameQuanta) {
        SCOPED_TRACE(frames);
        outputLeft_.fill(-99.0f);
        outputRight_.fill(-99.0f);

        std::size_t allocations = 0;
        {
            allocation_probe::NoAllocScope noAlloc;
            chain_.process(inputs, outputs, frames);
            allocations = noAlloc.count();
        }

        EXPECT_EQ(allocations, 0u);
        for (uint32_t frame = 0; frame < frames; ++frame) {
            EXPECT_FLOAT_EQ(outputLeft_[frame], 0.5f);
            EXPECT_FLOAT_EQ(outputRight_[frame], -1.0f);
        }
        EXPECT_FLOAT_EQ(outputLeft_[frames], -99.0f);
        EXPECT_FLOAT_EQ(outputRight_[frames], -99.0f);
    }
}

TEST_F(PluginChainRealtimeTest, OversizedCallbackPassthroughsWithoutAllocating) {
    const uint32_t frames = 2048;
    const float* inputs[] = {inputLeft_.data(), inputRight_.data()};
    float* outputs[] = {outputLeft_.data(), outputRight_.data()};

    std::size_t allocations = 0;
    {
        allocation_probe::NoAllocScope noAlloc;
        chain_.process(inputs, outputs, frames);
        allocations = noAlloc.count();
    }

    EXPECT_EQ(allocations, 0u);
    for (uint32_t frame = 0; frame < frames; ++frame) {
        EXPECT_FLOAT_EQ(outputLeft_[frame], inputLeft_[frame]);
        EXPECT_FLOAT_EQ(outputRight_[frame], inputRight_[frame]);
    }
}

constexpr const char* kRawPath = "/tmp/guitarrackcraft-audio-recorder-test-raw.wav";
constexpr const char* kProcessedPath =
    "/tmp/guitarrackcraft-audio-recorder-test-processed.wav";
constexpr const char* kSaturatedRawPath =
    "/tmp/guitarrackcraft-audio-recorder-saturated-raw.wav";
constexpr const char* kSaturatedProcessedPath =
    "/tmp/guitarrackcraft-audio-recorder-saturated-processed.wav";

std::uintmax_t fileSize(const char* path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) return 0;
    return static_cast<std::uintmax_t>(file.tellg());
}

std::uint32_t readLe32(const char* path, std::streamoff offset) {
    std::ifstream file(path, std::ios::binary);
    file.seekg(offset);
    std::array<unsigned char, 4> bytes{};
    file.read(reinterpret_cast<char*>(bytes.data()), bytes.size());
    return static_cast<std::uint32_t>(bytes[0]) |
           (static_cast<std::uint32_t>(bytes[1]) << 8) |
           (static_cast<std::uint32_t>(bytes[2]) << 16) |
           (static_cast<std::uint32_t>(bytes[3]) << 24);
}

std::int16_t readPcmSample(const char* path, std::streamoff offset) {
    std::ifstream file(path, std::ios::binary);
    file.seekg(offset);
    std::int16_t sample = 0;
    file.read(reinterpret_cast<char*>(&sample), sizeof(sample));
    return sample;
}

class AudioRecorderRealtimeTest : public ::testing::Test {
protected:
    void SetUp() override {
        std::remove(kRawPath);
        std::remove(kProcessedPath);
        ASSERT_TRUE(recorder_.startRecording(kRawPath, kProcessedPath, 48000.0f));
    }

    void TearDown() override {
        if (recorder_.isRecording()) recorder_.stopRecording();
        std::remove(kRawPath);
        std::remove(kProcessedPath);
    }

    guitarrackcraft::AudioRecorder recorder_;
};

class AudioRecorderSaturationTest : public ::testing::Test {
protected:
    void SetUp() override {
        std::remove(kSaturatedRawPath);
        std::remove(kSaturatedProcessedPath);
        ASSERT_TRUE(recorder_.startRecording(
            kSaturatedRawPath, kSaturatedProcessedPath, 1.0f));
    }

    void TearDown() override {
        if (recorder_.isRecording()) recorder_.stopRecording();
        std::remove(kSaturatedRawPath);
        std::remove(kSaturatedProcessedPath);
    }

    guitarrackcraft::AudioRecorder recorder_;
};

TEST_F(AudioRecorderRealtimeTest, SupportedInterleavedQuantaFeedWithoutAllocations) {
    std::array<float, 1024> raw{};
    std::array<float, 1024> left{};
    std::array<float, 1024> right{};
    raw.fill(0.25f);
    left.fill(0.125f);
    right.fill(-0.25f);

    std::size_t allocations512 = 0;
    {
        allocation_probe::NoAllocScope noAlloc;
        recorder_.feedAudio(raw.data(), left.data(), right.data(), 512);
        allocations512 = noAlloc.count();
    }
    std::size_t allocations1024 = 0;
    {
        allocation_probe::NoAllocScope noAlloc;
        recorder_.feedAudio(raw.data(), left.data(), right.data(), 1024);
        allocations1024 = noAlloc.count();
    }

    EXPECT_EQ(allocations512, 0u);
    EXPECT_EQ(allocations1024, 0u);
    EXPECT_DOUBLE_EQ(recorder_.getDurationSec(), 1536.0 / 48000.0);
    recorder_.stopRecording();

    EXPECT_EQ(fileSize(kRawPath), 44u + 1536u * sizeof(std::int16_t));
    EXPECT_EQ(fileSize(kProcessedPath),
              44u + 1536u * 2u * sizeof(std::int16_t));
    EXPECT_EQ(readPcmSample(kProcessedPath, 44), static_cast<std::int16_t>(4095));
    EXPECT_EQ(readPcmSample(kProcessedPath, 46), static_cast<std::int16_t>(-8191));
}

TEST_F(AudioRecorderSaturationTest, SaturatedFeedHeadersMatchShortWritePayload) {
    constexpr std::size_t kFrames = 1u << 20;
    std::vector<float> raw(kFrames, 0.25f);
    std::vector<float> left(kFrames, 0.125f);
    std::vector<float> right(kFrames, -0.25f);

    std::size_t allocations = 0;
    {
        allocation_probe::NoAllocScope noAlloc;
        recorder_.feedAudio(raw.data(), left.data(), right.data(),
                            static_cast<int32_t>(kFrames));
        allocations = noAlloc.count();
    }
    EXPECT_EQ(allocations, 0u);
    recorder_.stopRecording();

    const std::uintmax_t rawBytes = fileSize(kSaturatedRawPath);
    const std::uintmax_t processedBytes = fileSize(kSaturatedProcessedPath);
    ASSERT_GE(rawBytes, 44u);
    ASSERT_GE(processedBytes, 44u);
    EXPECT_LT(rawBytes - 44u, kFrames * sizeof(std::int16_t));
    EXPECT_LT(processedBytes - 44u, kFrames * 2u * sizeof(std::int16_t));

    EXPECT_EQ(readLe32(kSaturatedRawPath, 4),
              static_cast<std::uint32_t>(rawBytes - 8u));
    EXPECT_EQ(readLe32(kSaturatedRawPath, 40),
              static_cast<std::uint32_t>(rawBytes - 44u));
    EXPECT_EQ(readLe32(kSaturatedProcessedPath, 4),
              static_cast<std::uint32_t>(processedBytes - 8u));
    EXPECT_EQ(readLe32(kSaturatedProcessedPath, 40),
              static_cast<std::uint32_t>(processedBytes - 44u));
}

TEST(AudioRecorderConcurrentTest, FeedAndStopProducesValidHeaders) {
    constexpr std::size_t kFrames = 1u << 20;
    constexpr int kIterations = 16;
    std::vector<float> raw(kFrames, 0.25f);
    std::vector<float> left(kFrames, 0.125f);
    std::vector<float> right(kFrames, -0.25f);

    for (int iteration = 0; iteration < kIterations; ++iteration) {
        std::remove(kSaturatedRawPath);
        std::remove(kSaturatedProcessedPath);
        guitarrackcraft::AudioRecorder recorder;
        ASSERT_TRUE(recorder.startRecording(
            kSaturatedRawPath, kSaturatedProcessedPath, 1.0f));

        std::atomic<bool> feedStarted{false};
        std::thread feedThread([&] {
            feedStarted.store(true, std::memory_order_release);
            recorder.feedAudio(raw.data(), left.data(), right.data(),
                               static_cast<int32_t>(kFrames));
        });
        while (!feedStarted.load(std::memory_order_acquire))
            std::this_thread::yield();
        std::this_thread::yield();

        recorder.stopRecording();
        feedThread.join();

        EXPECT_FALSE(recorder.isRecording()) << "iteration " << iteration;
        const std::uintmax_t rawBytes = fileSize(kSaturatedRawPath);
        const std::uintmax_t processedBytes = fileSize(kSaturatedProcessedPath);
        ASSERT_GE(rawBytes, 44u) << "iteration " << iteration;
        ASSERT_GE(processedBytes, 44u) << "iteration " << iteration;
        EXPECT_EQ(readLe32(kSaturatedRawPath, 4),
                  static_cast<std::uint32_t>(rawBytes - 8u));
        EXPECT_EQ(readLe32(kSaturatedRawPath, 40),
                  static_cast<std::uint32_t>(rawBytes - 44u));
        EXPECT_EQ(readLe32(kSaturatedProcessedPath, 4),
                  static_cast<std::uint32_t>(processedBytes - 8u));
        EXPECT_EQ(readLe32(kSaturatedProcessedPath, 40),
                  static_cast<std::uint32_t>(processedBytes - 44u));
    }

    std::remove(kSaturatedRawPath);
    std::remove(kSaturatedProcessedPath);
}


} // namespace
