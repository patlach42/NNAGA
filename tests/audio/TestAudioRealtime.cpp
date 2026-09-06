#include <gtest/gtest.h>

#include "plugin/PluginChain.h"

#include "utils/WavIO.h"
#include "utils/BoundedSPSCQueue.h"
#include <liblowlatencyaudio/ThreadUtils.h>
#include "utils/VariablePayloadSPSCQueue.h"

#if defined(__linux__)
#include <sched.h>
#endif
#include <array>
#include <initializer_list>
#include <atomic>
#include <cmath>
#include <thread>
#include <cstring>
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

    uint32_t process(const float* const* inputs, float* const* outputs,
                     uint32_t numFrames,
                     const guitarrackcraft::AudioProcessContext&,
                     const guitarrackcraft::MidiEvent*, uint32_t,
                     guitarrackcraft::MidiEvent*, uint32_t) override {
        for (uint32_t frame = 0; frame < numFrames; ++frame) {
            outputs[0][frame] = inputs[0][frame] * 2.0f;
            outputs[1][frame] = inputs[1][frame] * 2.0f;
        }
        return 0;
    }

    guitarrackcraft::PluginInfo getInfo() const override {
        guitarrackcraft::PluginInfo info;
        info.realtimeClass = guitarrackcraft::RealtimeClass::CertifiedInProcess;
        return info;
    }
    void setParameter(uint32_t, float) override {}
    float getParameter(uint32_t) const override { return 0.0f; }
    uint32_t getNumInputPorts() const override { return 2; }
    uint32_t getNumOutputPorts() const override { return 2; }
};

class ParameterPlugin final : public IPlugin {
public:
    void activate(float, uint32_t) override {}
    void deactivate() override {}
    uint32_t process(const float* const* inputs, float* const* outputs,
                     uint32_t numFrames, const guitarrackcraft::AudioProcessContext&,
                     const guitarrackcraft::MidiEvent*, uint32_t,
                     guitarrackcraft::MidiEvent*, uint32_t) override {
        for (uint32_t frame = 0; frame < numFrames; ++frame) {
            outputs[0][frame] = inputs[0][frame];
            outputs[1][frame] = inputs[1][frame];
        }
        return 0;
    }
    guitarrackcraft::PluginInfo getInfo() const override {
        guitarrackcraft::PluginInfo info;
        info.realtimeClass = guitarrackcraft::RealtimeClass::CertifiedInProcess;
        return info;
    }
    void setParameter(uint32_t port, float value) override {
        if (port == 0) value_ = value;
    }
    float getParameter(uint32_t port) const override {
        return port == 0 ? value_ : 0.0f;
    }
    uint32_t getNumInputPorts() const override { return 2; }
    uint32_t getNumOutputPorts() const override { return 2; }

private:
    float value_ = 0.0f;
};

struct DestructionState {
    std::atomic<bool> entered{false};
    std::atomic<bool> release{false};
    std::atomic<bool> destroyed{false};
};

class HazardPlugin final : public IPlugin {
public:
    explicit HazardPlugin(std::shared_ptr<DestructionState> state)
        : state_(std::move(state)) {}
    ~HazardPlugin() override {
        state_->destroyed.store(true, std::memory_order_release);
    }
    void activate(float, uint32_t) override {}
    void deactivate() override {}
    uint32_t process(const float* const* inputs, float* const* outputs,
                     uint32_t numFrames, const guitarrackcraft::AudioProcessContext&,
                     const guitarrackcraft::MidiEvent*, uint32_t,
                     guitarrackcraft::MidiEvent*, uint32_t) override {
        state_->entered.store(true, std::memory_order_release);
        while (!state_->release.load(std::memory_order_acquire))
            std::this_thread::yield();
        for (uint32_t frame = 0; frame < numFrames; ++frame) {
            outputs[0][frame] = inputs[0][frame];
            outputs[1][frame] = inputs[1][frame];
        }
        return 0;
    }
    guitarrackcraft::PluginInfo getInfo() const override {
        guitarrackcraft::PluginInfo info;
        info.realtimeClass = guitarrackcraft::RealtimeClass::CertifiedInProcess;
        return info;
    }
    void setParameter(uint32_t, float) override {}
    float getParameter(uint32_t) const override { return 0.0f; }
    uint32_t getNumInputPorts() const override { return 2; }
    uint32_t getNumOutputPorts() const override { return 2; }

private:
    std::shared_ptr<DestructionState> state_;
};

class CountingPlugin final : public IPlugin {
public:
    explicit CountingPlugin(std::shared_ptr<std::atomic<uint32_t>> calls)
        : calls_(std::move(calls)) {}
    void activate(float, uint32_t) override {}
    void deactivate() override {}
    uint32_t process(const float* const* inputs, float* const* outputs,
                     uint32_t numFrames, const guitarrackcraft::AudioProcessContext&,
                     const guitarrackcraft::MidiEvent*, uint32_t,
                     guitarrackcraft::MidiEvent*, uint32_t) override {
        calls_->fetch_add(1, std::memory_order_relaxed);
        for (uint32_t frame = 0; frame < numFrames; ++frame) {
            outputs[0][frame] = 42.0f;
            outputs[1][frame] = -42.0f;
        }
        return 0;
    }
    guitarrackcraft::PluginInfo getInfo() const override {
        guitarrackcraft::PluginInfo info;
        info.realtimeClass = guitarrackcraft::RealtimeClass::CertifiedInProcess;
        return info;
    }
    void setParameter(uint32_t, float) override {}
    float getParameter(uint32_t) const override { return 0.0f; }
    uint32_t getNumInputPorts() const override { return 2; }
    uint32_t getNumOutputPorts() const override { return 2; }

private:
    std::shared_ptr<std::atomic<uint32_t>> calls_;
};

class PluginChainRealtimeTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_EQ(chain_.addPlugin(std::make_unique<GainPlugin>()), 0);
        chain_.setSampleRate(48000.0f, 1024);
        chain_.activate();
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
            chain_.process(inputs, outputs, frames,
                           guitarrackcraft::AudioProcessContext{},
                           nullptr, 0, nullptr, 0);
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

TEST_F(PluginChainRealtimeTest, OversizedCallbackClearsWithoutAllocating) {
    const uint32_t frames = 2048;
    const float* inputs[] = {inputLeft_.data(), inputRight_.data()};
    float* outputs[] = {outputLeft_.data(), outputRight_.data()};

    std::size_t allocations = 0;
    {
        allocation_probe::NoAllocScope noAlloc;
        chain_.process(inputs, outputs, frames,
                       guitarrackcraft::AudioProcessContext{},
                       nullptr, 0, nullptr, 0);
        allocations = noAlloc.count();
    }

    EXPECT_EQ(allocations, 0u);
    for (uint32_t frame = 0; frame < frames; ++frame) {
        EXPECT_FLOAT_EQ(outputLeft_[frame], 0.0f);
        EXPECT_FLOAT_EQ(outputRight_[frame], 0.0f);
    }
}

TEST(PluginChainEmptyTest, InPlacePassesThroughWithoutAllocating) {
    constexpr uint32_t frames = 64;
    PluginChain chain;
    chain.setSampleRate(48000.0f, frames);
    chain.activate();

    std::array<float, frames> left{};
    std::array<float, frames> right{};
    for (uint32_t frame = 0; frame < frames; ++frame) {
        left[frame] = 0.25f + static_cast<float>(frame) * 0.03125f;
        right[frame] = -0.5f - static_cast<float>(frame) * 0.0625f;
    }
    const auto expectedLeft = left;
    const auto expectedRight = right;
    const float* inputs[] = {left.data(), right.data()};
    float* outputs[] = {left.data(), right.data()};

    std::size_t allocations = 0;
    {
        allocation_probe::NoAllocScope noAlloc;
        chain.process(inputs, outputs, frames,
                      guitarrackcraft::AudioProcessContext{},
                      nullptr, 0, nullptr, 0);
        allocations = noAlloc.count();
    }

    EXPECT_EQ(allocations, 0u);
    for (uint32_t frame = 0; frame < frames; ++frame) {
        EXPECT_EQ(left[frame], expectedLeft[frame]);
        EXPECT_EQ(right[frame], expectedRight[frame]);
    }
}
TEST_F(PluginChainRealtimeTest,
       ConcurrentAddRemoveReorderAndRateChangesNeverForceDryFallback) {
    constexpr uint32_t frames = 64;
    std::array<float, frames> left{};
    std::array<float, frames> right{};
    std::array<float, frames> outLeft{};
    std::array<float, frames> outRight{};
    left.fill(0.25f);
    right.fill(-0.5f);
    const float* inputs[] = {left.data(), right.data()};
    float* outputs[] = {outLeft.data(), outRight.data()};
    std::atomic<bool> start{false};
    std::atomic<bool> invalidOutput{false};
    std::atomic<uint32_t> invalidLeftBits{0};
    std::atomic<uint32_t> invalidRightBits{0};
    std::atomic<uint32_t> invalidFrame{0};
    std::atomic<std::size_t> callbackAllocations{0};
    const auto isCompleteGain = [](float ratio) {
        uint32_t bits = 0;
        std::memcpy(&bits, &ratio, sizeof(bits));
        return std::isfinite(ratio) && ratio >= 2.0f &&
               (bits & 0x007fffffu) == 0u &&
               ((bits >> 23u) & 0xffu) < 0xffu;
    };

    std::thread controls([&] {
        while (!start.load(std::memory_order_acquire))
            std::this_thread::yield();
        for (uint32_t iteration = 0; iteration < 200; ++iteration) {
            chain_.setSampleRate(48000.0f, 1024);
            const int added =
                chain_.addPlugin(std::make_unique<GainPlugin>());
            if (added >= 1) {
                if (chain_.getSize() >= 3)
                    (void)chain_.reorderPlugins(1, 2);
                (void)chain_.removePlugin(1);
            }
        }
    });
    std::thread callback([&] {
        while (!start.load(std::memory_order_acquire))
            std::this_thread::yield();
        allocation_probe::NoAllocScope noAlloc;
        for (uint32_t iteration = 0; iteration < 2000; ++iteration) {
            chain_.process(inputs, outputs, frames,
                           guitarrackcraft::AudioProcessContext{},
                           nullptr, 0, nullptr, 0);
            for (uint32_t frame = 0; frame < frames; ++frame) {
                const float leftRatio = outLeft[frame] / left[frame];
                const float rightRatio = outRight[frame] / right[frame];
                const bool completeGain =
                    leftRatio == rightRatio && isCompleteGain(leftRatio);
                if (!completeGain &&
                    !invalidOutput.exchange(true, std::memory_order_acq_rel)) {
                    uint32_t leftBits = 0;
                    uint32_t rightBits = 0;
                    std::memcpy(&leftBits, &outLeft[frame], sizeof(leftBits));
                    std::memcpy(&rightBits, &outRight[frame], sizeof(rightBits));
                    invalidLeftBits.store(leftBits, std::memory_order_release);
                    invalidRightBits.store(rightBits, std::memory_order_release);
                    invalidFrame.store(frame, std::memory_order_release);
                }
            }
        }
        callbackAllocations.store(noAlloc.count(), std::memory_order_release);
    });
    start.store(true, std::memory_order_release);
    callback.join();
    controls.join();

    const auto bitsToFloat = [](uint32_t bits) {
        float value = 0.0f;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    };
    const uint32_t badLeftBits =
        invalidLeftBits.load(std::memory_order_acquire);
    const uint32_t badRightBits =
        invalidRightBits.load(std::memory_order_acquire);
    EXPECT_FALSE(invalidOutput.load(std::memory_order_acquire))
        << "frame " << invalidFrame.load(std::memory_order_acquire)
        << " produced " << bitsToFloat(badLeftBits) << ", "
        << bitsToFloat(badRightBits);
    EXPECT_EQ(callbackAllocations.load(std::memory_order_acquire), 0u);
}

TEST_F(PluginChainRealtimeTest, StableInstanceIdControlsSurviveReorderAndRemoval) {
    PluginChain chain;
    ASSERT_EQ(chain.addPlugin(std::make_unique<ParameterPlugin>()), 0);
    ASSERT_EQ(chain.addPlugin(std::make_unique<ParameterPlugin>()), 1);
    const uint64_t firstId = chain.getPluginInstanceId(0);
    const uint64_t secondId = chain.getPluginInstanceId(1);
    ASSERT_NE(firstId, 0u);
    ASSERT_NE(secondId, 0u);
    ASSERT_NE(firstId, secondId);

    ASSERT_TRUE(chain.submitParameter(firstId, 0, 0.25f));
    ASSERT_TRUE(chain.reorderPlugins(0, 1));
    EXPECT_FLOAT_EQ(chain.getParameter(firstId, 0), 0.25f);
    ASSERT_TRUE(chain.submitParameter(firstId, 0, 0.875f));
    EXPECT_FLOAT_EQ(chain.getParameter(firstId, 0), 0.875f);

    ASSERT_TRUE(chain.removePlugin(1));
    EXPECT_FALSE(chain.submitParameter(firstId, 0, 0.5f));
    EXPECT_FLOAT_EQ(chain.getParameter(firstId, 0), 0.0f);
    EXPECT_FALSE(chain.submitParameter(0xdeadbeefULL, 0, 0.5f));
    EXPECT_FLOAT_EQ(chain.getParameter(0xdeadbeefULL, 0), 0.0f);
}

TEST_F(PluginChainRealtimeTest, RetiredPluginDestructionWaitsForAudioHazard) {
    auto state = std::make_shared<DestructionState>();
    PluginChain chain;
    ASSERT_EQ(chain.addPlugin(std::make_unique<HazardPlugin>(state)), 0);
    chain.setSampleRate(48000.0f, 64);
    chain.activate();

    std::array<float, 64> inputLeft{};
    std::array<float, 64> inputRight{};
    std::array<float, 64> outputLeft{};
    std::array<float, 64> outputRight{};
    const float* inputs[] = {inputLeft.data(), inputRight.data()};
    float* outputs[] = {outputLeft.data(), outputRight.data()};
    std::thread audio([&] {
        chain.process(inputs, outputs, 64,
                      guitarrackcraft::AudioProcessContext{},
                      nullptr, 0, nullptr, 0);
    });
    for (uint32_t spin = 0;
         spin < 100'000 && !state->entered.load(std::memory_order_acquire);
         ++spin)
        std::this_thread::yield();
    if (!state->entered.load(std::memory_order_acquire)) {
        state->release.store(true, std::memory_order_release);
        audio.join();
        FAIL() << "audio callback did not reach the hazard barrier";
        return;
    }

    std::atomic<bool> removed{false};
    std::thread remover([&] {
        removed.store(chain.removePlugin(0), std::memory_order_release);
    });
    for (uint32_t spin = 0;
         spin < 100'000 && !removed.load(std::memory_order_acquire);
         ++spin)
        std::this_thread::yield();
    if (!removed.load(std::memory_order_acquire)) {
        state->release.store(true, std::memory_order_release);
        audio.join();
        remover.join();
        FAIL() << "plugin removal did not complete";
        return;
    }
    EXPECT_FALSE(state->destroyed.load(std::memory_order_acquire));

    state->release.store(true, std::memory_order_release);
    audio.join();
    remover.join();
    ASSERT_EQ(chain.addPlugin(std::make_unique<GainPlugin>()), 0);
    for (uint32_t spin = 0;
         spin < 100'000 && !state->destroyed.load(std::memory_order_acquire);
         ++spin)
        std::this_thread::yield();
    EXPECT_TRUE(state->destroyed.load(std::memory_order_acquire));
}

TEST_F(PluginChainRealtimeTest, OversizedQuantumIsRejectedWithoutPartialProcess) {
    auto calls = std::make_shared<std::atomic<uint32_t>>(0);
    PluginChain chain;
    ASSERT_EQ(chain.addPlugin(std::make_unique<CountingPlugin>(calls)), 0);
    chain.setSampleRate(48000.0f, 64);
    chain.activate();

    std::array<float, 65> inputLeft{};
    std::array<float, 65> inputRight{};
    std::array<float, 65> outputLeft{};
    std::array<float, 65> outputRight{};
    outputLeft.fill(-7.0f);
    outputRight.fill(9.0f);
    const float* inputs[] = {inputLeft.data(), inputRight.data()};
    float* outputs[] = {outputLeft.data(), outputRight.data()};

    chain.process(inputs, outputs, 65,
                  guitarrackcraft::AudioProcessContext{},
                  nullptr, 0, nullptr, 0);
    EXPECT_EQ(calls->load(std::memory_order_acquire), 0u);
    for (uint32_t frame = 0; frame < 65; ++frame) {
        EXPECT_FLOAT_EQ(outputLeft[frame], 0.0f);
        EXPECT_FLOAT_EQ(outputRight[frame], 0.0f);
    }

    chain.process(inputs, outputs, 64,
                  guitarrackcraft::AudioProcessContext{},
                  nullptr, 0, nullptr, 0);
    EXPECT_EQ(calls->load(std::memory_order_acquire), 1u);
    EXPECT_FLOAT_EQ(outputLeft[0], 42.0f);
    EXPECT_FLOAT_EQ(outputRight[0], -42.0f);
}




TEST(VariablePayloadSPSCQueueTest, MaxPayloadFifoWrapFullAndReuse) {
    constexpr std::size_t payloadSize = 524288;
    guitarrackcraft::VariablePayloadSPSCQueue queue(4, payloadSize);

    const auto pattern = [](uint8_t seed, std::size_t index) {
        return static_cast<uint8_t>(seed + index * 37u);
    };
    const auto push = [&](uint8_t seed) {
        EXPECT_TRUE(queue.tryEmplace(payloadSize,
                                     [&, seed](uint8_t* data, std::size_t size) {
                                         ASSERT_EQ(size, payloadSize);
                                         for (std::size_t i = 0; i < size; ++i)
                                             data[i] = pattern(seed, i);
                                     }));
    };
    const auto pop = [&](uint8_t seed) {
        EXPECT_TRUE(queue.consume([&, seed](const uint8_t* data, std::size_t size) {
            ASSERT_EQ(size, payloadSize);
            bool matches = true;
            for (std::size_t i = 0; i < size; ++i)
                matches = matches && data[i] == pattern(seed, i);
            EXPECT_TRUE(matches);
        }));
    };

    push(10);
    push(20);
    push(30);
    push(40);
    EXPECT_FALSE(queue.tryEmplace(payloadSize,
                                  [](uint8_t* data, std::size_t size) {
                                      if (size > 0) data[0] = 0;
                                  }));

    pop(10);
    pop(20);
    push(50);
    push(60);
    pop(30);
    pop(40);
    pop(50);
    pop(60);
    EXPECT_FALSE(queue.consume([](const uint8_t*, std::size_t) {}));

    push(70);
    pop(70);
    EXPECT_FALSE(queue.consume([](const uint8_t*, std::size_t) {}));
}
TEST(VariablePayloadSPSCQueueTest,
     LargeDescriptorRingPreservesVariablePayloadFifoAndRejectsByteStorageOverflow) {
    constexpr std::size_t kCapacity = 256;
    constexpr std::size_t kByteCapacity = 4u * 1024u * 1024u;
    constexpr std::size_t kRecordCount = 192;
    guitarrackcraft::VariablePayloadSPSCQueue queue(kCapacity, kByteCapacity);

    const auto makeBatch = [](uint8_t batch) {
        std::vector<std::vector<uint8_t>> payloads;
        payloads.reserve(kRecordCount);
        for (std::size_t index = 0; index < kRecordCount; ++index) {
            const std::size_t size = 3u + ((index * 13u + batch * 7u) % 67u);
            std::vector<uint8_t> payload(size);
            for (std::size_t byte = 0; byte < size; ++byte) {
                payload[byte] = static_cast<uint8_t>(
                    batch * 97u + index * 31u + byte * 17u + (index ^ byte));
            }
            payloads.push_back(payload);
        }
        return payloads;
    };
    const auto enqueue = [&](const std::vector<std::vector<uint8_t>>& payloads) {
        for (const auto& payload : payloads) {
            ASSERT_TRUE(queue.tryEmplace(
                payload.size(), [&](uint8_t* data, std::size_t size) {
                    ASSERT_EQ(size, payload.size());
                    for (std::size_t byte = 0; byte < size; ++byte)
                        data[byte] = payload[byte];
                }));
        }
    };
    const auto drain = [&](const std::vector<std::vector<uint8_t>>& payloads) {
        for (const auto& payload : payloads) {
            ASSERT_TRUE(queue.consume([&](const uint8_t* data, std::size_t size) {
                ASSERT_EQ(size, payload.size());
                for (std::size_t byte = 0; byte < size; ++byte)
                    EXPECT_EQ(data[byte], payload[byte]);
            }));
        }
        EXPECT_FALSE(queue.consume([](const uint8_t*, std::size_t) {}));
    };

    const auto firstBatch = makeBatch(1);
    enqueue(firstBatch);

    bool writerCalled = false;
    EXPECT_FALSE(queue.tryEmplace(
        kByteCapacity, [&](uint8_t*, std::size_t) { writerCalled = true; }));
    EXPECT_FALSE(writerCalled);
    drain(firstBatch);
    const auto reusedBatch = makeBatch(2);
    enqueue(reusedBatch);
    drain(reusedBatch);

    constexpr std::size_t kFullStorageRecordSize = kByteCapacity / 4u;
    std::vector<std::vector<uint8_t>> fullStorage;
    fullStorage.reserve(4);
    for (std::size_t index = 0; index < 4; ++index) {
        fullStorage.emplace_back(kFullStorageRecordSize);
        auto& payload = fullStorage.back();
        for (std::size_t byte = 0; byte < payload.size(); ++byte) {
            payload[byte] = static_cast<uint8_t>(
                0xa1u + index * 37u + byte * 19u);
        }
    }
    guitarrackcraft::VariablePayloadSPSCQueue storageQueue(kCapacity, kByteCapacity);
    for (const auto& payload : fullStorage) {
        ASSERT_TRUE(storageQueue.tryEmplace(
            payload.size(), [&](uint8_t* data, std::size_t size) {
                ASSERT_EQ(size, payload.size());
                for (std::size_t byte = 0; byte < size; ++byte)
                    data[byte] = payload[byte];
            }));
    }
    writerCalled = false;
    EXPECT_FALSE(storageQueue.tryEmplace(
        kFullStorageRecordSize,
        [&](uint8_t*, std::size_t) { writerCalled = true; }));
    EXPECT_FALSE(writerCalled);
    for (const auto& payload : fullStorage) {
        ASSERT_TRUE(storageQueue.consume(
            [&](const uint8_t* data, std::size_t size) {
                ASSERT_EQ(size, payload.size());
                for (std::size_t byte = 0; byte < size; ++byte)
                    EXPECT_EQ(data[byte], payload[byte]);
            }));
    }
    EXPECT_FALSE(storageQueue.consume([](const uint8_t*, std::size_t) {}));
}




TEST(BoundedSPSCQueueTest, CapacityLeavesOneSlotAndPreservesFifoAcrossWrap) {
    guitarrackcraft::BoundedSPSCQueue<uint32_t, 8> queue;

    for (uint32_t value = 0; value < 7; ++value) {
        ASSERT_TRUE(queue.push(value));
    }
    EXPECT_FALSE(queue.push(7u));

    uint32_t value = 0;
    for (uint32_t expected = 0; expected < 7; ++expected) {
        ASSERT_TRUE(queue.pop(value));
        EXPECT_EQ(value, expected);
    }
    EXPECT_FALSE(queue.pop(value));

    for (uint32_t valueToPush = 100; valueToPush < 107; ++valueToPush) {
        ASSERT_TRUE(queue.push(valueToPush));
    }
    EXPECT_FALSE(queue.push(107u));
    for (uint32_t expected = 100; expected < 107; ++expected) {
        ASSERT_TRUE(queue.pop(value));
        EXPECT_EQ(value, expected);
    }
    EXPECT_FALSE(queue.pop(value));
}

TEST(BoundedSPSCQueueTest, SingleProducerConsumerStressRemainsFifo) {
    guitarrackcraft::BoundedSPSCQueue<uint64_t, 64> queue;
    constexpr uint64_t count = 100000;
    std::atomic<bool> producerDone{false};
    std::atomic<bool> orderError{false};
    std::thread producer([&] {
        for (uint64_t value = 0; value < count; ++value) {
            while (!queue.push(value)) std::this_thread::yield();
        }
        producerDone.store(true, std::memory_order_release);
    });

    uint64_t expected = 0;
    uint64_t value = 0;
    while (expected < count) {
        if (queue.pop(value)) {
            if (value != expected) orderError.store(true, std::memory_order_release);
            ++expected;
        } else if (!producerDone.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
    }
    producer.join();
    EXPECT_FALSE(orderError.load(std::memory_order_acquire));
    EXPECT_FALSE(queue.pop(value));
}

#if defined(__linux__)
namespace {

cpu_set_t makeCpuSet(std::initializer_list<int> cpus) {
    cpu_set_t set;
    CPU_ZERO(&set);
    for (const int cpu : cpus)
        CPU_SET(cpu, &set);
    return set;
}

TEST(ThreadUtilsAffinityTest, SelectsRankedPerformanceSubsetWithoutChangingAffinity) {
    struct AffinityCase {
        const char* name;
        cpu_set_t allowed;
        cpu_set_t expected;
    };
    const AffinityCase cases[] = {
        {"8 allowed CPUs choose fastest two", makeCpuSet({0, 1, 2, 3, 4, 5, 6, 7}),
         makeCpuSet({6, 7})},
        {"6 allowed CPUs choose fastest two", makeCpuSet({0, 1, 2, 3, 4, 5}),
         makeCpuSet({4, 5})},
        {"4 allowed CPUs choose fastest two", makeCpuSet({0, 1, 2, 3}),
         makeCpuSet({2, 3})},
        {"2 allowed CPUs keep both", makeCpuSet({0, 1}), makeCpuSet({0, 1})},
        {"empty mask remains empty", makeCpuSet({}), makeCpuSet({})},
        {"singleton remains available", makeCpuSet({7}), makeCpuSet({7})},
        {"sparse 8 CPU mask selects final two by rank",
         makeCpuSet({1, 4, 9, 16, 25, 36, 49, 64}),
         makeCpuSet({49, 64})},
        {"sparse 4 CPU mask selects final two by rank",
         makeCpuSet({2, 7, 19, 31}), makeCpuSet({19, 31})},
    };

    cpu_set_t processBefore;
    ASSERT_EQ(sched_getaffinity(0, sizeof(processBefore), &processBefore), 0);

    for (const auto& test : cases) {
        SCOPED_TRACE(test.name);
        const cpu_set_t inputBefore = test.allowed;
        const cpu_set_t selected =
            guitarrackcraft::deriveAudioCpuMask(test.allowed);

        EXPECT_TRUE(CPU_EQUAL(&selected, &test.expected));
        EXPECT_EQ(CPU_COUNT(&selected), CPU_COUNT(&test.expected));
        for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu)
            EXPECT_TRUE(!CPU_ISSET(cpu, &selected) ||
                        CPU_ISSET(cpu, &test.allowed))
                << "selected CPU " << cpu << " was not allowed";
        EXPECT_TRUE(CPU_EQUAL(&test.allowed, &inputBefore));
    }

    cpu_set_t processAfter;
    ASSERT_EQ(sched_getaffinity(0, sizeof(processAfter), &processAfter), 0);
    EXPECT_TRUE(CPU_EQUAL(&processBefore, &processAfter));
}

} // namespace
#endif
} // namespace
