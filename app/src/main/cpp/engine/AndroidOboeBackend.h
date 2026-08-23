#pragma once

#include <oboe/Oboe.h>
#include <atomic>
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
    oboe::DataCallbackResult onAudioReady(oboe::AudioStream*, void*, int32_t) override;
    void onErrorBeforeClose(oboe::AudioStream*, oboe::Result) override;
    void onErrorAfterClose(oboe::AudioStream*, oboe::Result) override;
    bool hasError() const noexcept { return error_.load(std::memory_order_acquire); }
    void clearError() noexcept { error_.store(false, std::memory_order_release); }

private:
    RackGraph& graph_;
    std::shared_ptr<oboe::AudioStream> input_;
    std::shared_ptr<oboe::AudioStream> output_;
    std::vector<float> inputInterleaved_;
    std::vector<float> inputLeft_, inputRight_, outputInterleaved_;
    std::atomic<bool> running_{false};
    std::atomic<int32_t> actualSampleRate_{0};
    std::atomic<int32_t> actualFramesPerDataCallback_{0};
    std::atomic<int32_t> inputChannelCount_{0};
    std::atomic<bool> error_{false};
    void closeStreams() noexcept;
};
}
