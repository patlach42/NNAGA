#pragma once

#include <oboe/Oboe.h>
#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>
#include "../plugin/RackGraph.h"

namespace guitarrackcraft {

class AndroidOboeBackend final : public oboe::AudioStreamCallback {
public:
    explicit AndroidOboeBackend(RackGraph& graph) noexcept : graph_(graph) {}
    ~AndroidOboeBackend() override { stop(); }
    AndroidOboeBackend(const AndroidOboeBackend&) = delete;
    AndroidOboeBackend& operator=(const AndroidOboeBackend&) = delete;

    bool start(int32_t sampleRate, int32_t inputDeviceId, int32_t outputDeviceId,
               int32_t bufferFrames);
    void stop() noexcept;
    bool isRunning() const noexcept { return running_.load(std::memory_order_acquire); }
    int32_t actualSampleRate() const noexcept { return actualSampleRate_.load(std::memory_order_acquire); }
    int32_t actualFramesPerDataCallback() const noexcept { return actualFramesPerDataCallback_.load(std::memory_order_acquire); }
    int32_t inputChannelCount() const noexcept { return inputChannelCount_.load(std::memory_order_acquire); }
    int32_t actualAudioApi() const noexcept { return actualAudioApi_.load(std::memory_order_acquire); }
    int32_t actualFramesPerBurst() const noexcept { return actualFramesPerBurst_.load(std::memory_order_acquire); }
    int32_t actualBufferSize() const noexcept { return actualBufferSize_.load(std::memory_order_acquire); }
    int32_t preparedCapacity() const noexcept { return preparedCapacity_.load(std::memory_order_acquire); }
    int32_t actualPerformanceMode() const noexcept { return actualPerformanceMode_.load(std::memory_order_acquire); }
    int32_t actualSharingMode() const noexcept { return actualSharingMode_.load(std::memory_order_acquire); }
    int32_t actualDeviceId() const noexcept { return actualDeviceId_.load(std::memory_order_acquire); }
    uint64_t inputOverflowCount() const noexcept { return inputOverflowCount_.load(std::memory_order_acquire); }
    uint64_t inputUnderflowCount() const noexcept { return inputUnderflowCount_.load(std::memory_order_acquire); }
    uint64_t capacityViolationCount() const noexcept { return capacityViolationCount_.load(std::memory_order_acquire); }
    uint64_t callbackCount() const noexcept { return callbackCount_.load(std::memory_order_acquire); }
    uint64_t callbackFrames() const noexcept { return callbackFrames_.load(std::memory_order_acquire); }
    uint64_t lastCallbackNanoseconds() const noexcept {
        return lastCallbackNanoseconds_.load(std::memory_order_relaxed);
    }
    uint64_t peakCallbackNanoseconds() const noexcept {
        return peakCallbackNanoseconds_.load(std::memory_order_relaxed);
    }
    uint64_t callbackDeadlineBudgetNanoseconds() const noexcept {
        return callbackDeadlineBudgetNanoseconds_.load(std::memory_order_relaxed);
    }
    uint64_t callbackDeadlineMisses() const noexcept {
        return callbackDeadlineMisses_.load(std::memory_order_relaxed);
    }
    bool reopenRequested() const noexcept { return reopenRequested_.load(std::memory_order_acquire); }
    void clearReopenRequest() noexcept { reopenRequested_.store(false, std::memory_order_release); }

    oboe::DataCallbackResult onAudioReady(oboe::AudioStream*, void*, int32_t) override;
    void onErrorBeforeClose(oboe::AudioStream*, oboe::Result) override;
    void onErrorAfterClose(oboe::AudioStream*, oboe::Result) override;
    bool hasError() const noexcept { return error_.load(std::memory_order_acquire); }
    void clearError() noexcept { error_.store(false, std::memory_order_release); }

private:
    class AudioFrameSpscRing {
    public:
        void prepare(uint32_t frames, uint32_t channels);
        void reset() noexcept;
        uint32_t capacity() const noexcept { return capacity_; }
        uint32_t channels() const noexcept { return channels_; }
        uint32_t push(const float* source, uint32_t frames, std::atomic<uint64_t>& drops,
                      std::atomic<uint64_t>& violations) noexcept;
        uint32_t pop(float* destination, uint32_t frames,
                     std::atomic<uint64_t>& underflows,
                     std::atomic<uint64_t>& staleDrops) noexcept;
    private:
        std::vector<float> storage_;
        uint32_t capacity_ = 0;
        uint32_t channels_ = 0;
        std::atomic<uint64_t> head_{0};
        std::atomic<uint64_t> tail_{0};
    };

    RackGraph& graph_;
    std::shared_ptr<oboe::AudioStream> input_;
    std::shared_ptr<oboe::AudioStream> output_;
    std::atomic<oboe::AudioStream*> inputCallbackStream_{nullptr};
    std::atomic<oboe::AudioStream*> outputCallbackStream_{nullptr};
    AudioFrameSpscRing inputRing_;
    std::vector<float> inputInterleaved_;
    std::vector<float> inputLeft_, inputRight_, outputInterleaved_;
    std::atomic<bool> running_{false};
    std::atomic<bool> error_{false};
    std::atomic<bool> reopenRequested_{false};
    std::atomic<int32_t> actualAudioApi_{0}, actualSampleRate_{0};
    std::atomic<int32_t> actualFramesPerBurst_{0}, actualFramesPerDataCallback_{0};
    std::atomic<int32_t> preparedCapacity_{0}, actualBufferSize_{0};
    std::atomic<int32_t> actualPerformanceMode_{0}, actualSharingMode_{0}, actualDeviceId_{0};
    std::atomic<uint64_t> callbackCount_{0}, callbackFrames_{0};
    std::atomic<int32_t> inputChannelCount_{0};
    std::atomic<uint64_t> inputOverflowCount_{0}, inputUnderflowCount_{0}, capacityViolationCount_{0};
    std::atomic<uint64_t> lastCallbackNanoseconds_{0};
    std::atomic<uint64_t> peakCallbackNanoseconds_{0};
    std::atomic<uint64_t> callbackDeadlineBudgetNanoseconds_{0};
    std::atomic<uint64_t> callbackDeadlineMisses_{0};
    void closeStreams() noexcept;
};
}