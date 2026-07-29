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
#include <chrono>
#include <future>
#include <mutex>
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

class TailOutputPlugin final : public IPlugin {
public:
    void activate(float, uint32_t) override {}
    void deactivate() override {}

    void process(const float* const* inputs, float* const* outputs,
                 uint32_t numFrames) override {
        for (uint32_t frame = 0; frame < numFrames; ++frame) {
            const bool hasInput = inputs[0][frame] != 0.0f ||
                                  inputs[1][frame] != 0.0f;
            if (hasInput) {
                tailActive_ = true;
                outputs[0][frame] = inputs[0][frame] * 2.0f;
                outputs[1][frame] = inputs[1][frame] * 2.0f;
            } else {
                outputs[0][frame] = tailActive_ ? 0.75f : 0.0f;
                outputs[1][frame] = tailActive_ ? 0.75f : 0.0f;
            }
        }
    }

    guitarrackcraft::PluginInfo getInfo() const override { return {}; }
    void setParameter(uint32_t, float) override {}
    float getParameter(uint32_t) const override { return 0.0f; }
    uint32_t getNumInputPorts() const override { return 2; }
    uint32_t getNumOutputPorts() const override { return 2; }

private:
    bool tailActive_ = false;
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

TEST(PluginChainContentionTest,
     ExclusiveControlContentionRepeatsLastCompleteWetBlockWithoutBlocking) {
    constexpr uint32_t frames = 64;
    PluginChain chain;
    ASSERT_EQ(chain.addPlugin(std::make_unique<TailOutputPlugin>()), 0);
    chain.setSampleRate(48000.0f, frames);

    std::array<float, frames> primeLeft{};
    std::array<float, frames> primeRight{};
    std::array<float, frames> primeOutputLeft{};
    std::array<float, frames> primeOutputRight{};
    primeLeft.fill(0.25f);
    primeRight.fill(-0.5f);
    const float* primeInputs[] = {primeLeft.data(), primeRight.data()};
    float* primeOutputs[] = {primeOutputLeft.data(), primeOutputRight.data()};
    chain.process(primeInputs, primeOutputs, frames);
    EXPECT_FLOAT_EQ(primeOutputLeft[0], 0.5f);
    EXPECT_FLOAT_EQ(primeOutputRight[0], -1.0f);

    std::array<float, frames> silentInput{};
    std::array<float, frames> tailOutputLeft{};
    std::array<float, frames> tailOutputRight{};
    const float* silentInputs[] = {silentInput.data(), silentInput.data()};
    float* tailOutputs[] = {tailOutputLeft.data(), tailOutputRight.data()};

    // This is the same control-plane exclusion used by add/remove/reorder.
    std::unique_lock controlLock(*chain.getChainMutex());
    std::promise<void> processFinished;
    std::future<void> completion = processFinished.get_future();
    std::thread callback([&] {
        chain.process(silentInputs, tailOutputs, frames);
        processFinished.set_value();
    });

    const bool completedWhileContended =
        completion.wait_for(std::chrono::seconds(1)) ==
        std::future_status::ready;
    controlLock.unlock();
    callback.join();

    ASSERT_TRUE(completedWhileContended);
    for (uint32_t frame = 0; frame < frames; ++frame) {
        SCOPED_TRACE(frame);
        ASSERT_FLOAT_EQ(tailOutputLeft[frame], 0.5f);
        ASSERT_FLOAT_EQ(tailOutputRight[frame], -1.0f);
        EXPECT_NE(tailOutputLeft[frame], silentInput[frame]);
        EXPECT_NE(tailOutputRight[frame], silentInput[frame]);
    }
}
template <std::size_t PrimeFrames, std::size_t ContendedFrames>
void assertVariableFrameContentionRepeatsWetHistory() {
    PluginChain chain;
    ASSERT_EQ(chain.addPlugin(std::make_unique<TailOutputPlugin>()), 0);
    // Allocate lifecycle-sized buffers for the larger callback while allowing
    // the priming block to establish a shorter wet-history quantum.
    chain.setSampleRate(48000.0f,
                        static_cast<uint32_t>(
                            PrimeFrames > ContendedFrames ? PrimeFrames
                                                          : ContendedFrames));

    std::array<float, PrimeFrames> primeLeft{};
    std::array<float, PrimeFrames> primeRight{};
    std::array<float, PrimeFrames> primeOutputLeft{};
    std::array<float, PrimeFrames> primeOutputRight{};
    for (std::size_t frame = 0; frame < PrimeFrames; ++frame) {
        primeLeft[frame] = 0.25f + static_cast<float>(frame) * 0.01f;
        primeRight[frame] = -0.5f - static_cast<float>(frame) * 0.02f;
    }
    const float* primeInputs[] = {primeLeft.data(), primeRight.data()};
    float* primeOutputs[] = {primeOutputLeft.data(), primeOutputRight.data()};
    chain.process(primeInputs, primeOutputs, static_cast<uint32_t>(PrimeFrames));

    std::array<float, ContendedFrames> contendedInputLeft{};
    std::array<float, ContendedFrames> contendedInputRight{};
    std::array<float, ContendedFrames> contendedOutputLeft{};
    std::array<float, ContendedFrames> contendedOutputRight{};
    contendedInputLeft.fill(7.0f);
    contendedInputRight.fill(-7.0f);
    const float* contendedInputs[] = {
        contendedInputLeft.data(), contendedInputRight.data()};
    float* contendedOutputs[] = {
        contendedOutputLeft.data(), contendedOutputRight.data()};

    // Hold the exclusive control lock so the callback must take the
    // allocation-free try-lock continuity path.
    std::unique_lock controlLock(*chain.getChainMutex());
    std::promise<void> processFinished;
    std::future<void> completion = processFinished.get_future();
    std::atomic<std::size_t> callbackAllocations{0};
    std::thread callback([&] {
        allocation_probe::NoAllocScope noAlloc;
        chain.process(contendedInputs, contendedOutputs,
                      static_cast<uint32_t>(ContendedFrames));
        callbackAllocations.store(noAlloc.count(), std::memory_order_release);
        processFinished.set_value();
    });

    const bool completedWhileContended =
        completion.wait_for(std::chrono::seconds(1)) ==
        std::future_status::ready;
    controlLock.unlock();
    callback.join();

    ASSERT_TRUE(completedWhileContended);
    EXPECT_EQ(callbackAllocations.load(std::memory_order_acquire), 0u);
    for (std::size_t frame = 0; frame < ContendedFrames; ++frame) {
        SCOPED_TRACE(frame);
        const std::size_t historyFrame = frame % PrimeFrames;
        EXPECT_FLOAT_EQ(contendedOutputLeft[frame], primeOutputLeft[historyFrame]);
        EXPECT_FLOAT_EQ(contendedOutputRight[frame], primeOutputRight[historyFrame]);
        EXPECT_NE(contendedOutputLeft[frame], contendedInputLeft[frame]);
        EXPECT_NE(contendedOutputRight[frame], contendedInputRight[frame]);
        EXPECT_NE(contendedOutputLeft[frame], 0.0f);
        EXPECT_NE(contendedOutputRight[frame], 0.0f);
    }
}

TEST(PluginChainContentionTest,
     DifferentLargerCallbackRepeatsShortWetHistoryWithoutAllocating) {
    assertVariableFrameContentionRepeatsWetHistory<64, 128>();
}

TEST(PluginChainContentionTest,
     DifferentSmallerCallbackUsesWetHistoryPrefixWithoutAllocating) {
    assertVariableFrameContentionRepeatsWetHistory<128, 64>();
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
