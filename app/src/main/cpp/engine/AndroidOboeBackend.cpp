#include "AndroidOboeBackend.h"
#include <algorithm>
#include <cstring>

namespace guitarrackcraft {

bool AndroidOboeBackend::start(int32_t sampleRate, int32_t inputDeviceId,
                               int32_t outputDeviceId, int32_t bufferFrames) {
    stop();
    error_.store(false, std::memory_order_release);
    oboe::AudioStreamBuilder in;
    in.setDirection(oboe::Direction::Input)->setFormat(oboe::AudioFormat::Float)
      ->setChannelCount(2)->setPerformanceMode(oboe::PerformanceMode::LowLatency)
      ->setSharingMode(oboe::SharingMode::Exclusive)->setErrorCallback(this);
    if (sampleRate > 0) in.setSampleRate(sampleRate);
    if (inputDeviceId > 0) in.setDeviceId(inputDeviceId);
    if (bufferFrames > 0) in.setFramesPerDataCallback(bufferFrames);
    auto result = in.openStream(input_);
    if (result != oboe::Result::OK) {
        input_.reset();
        in.setSharingMode(oboe::SharingMode::Shared)->setAudioApi(oboe::AudioApi::Unspecified);
        result = in.openStream(input_);
    }
    if (result != oboe::Result::OK) return false;
    oboe::AudioStreamBuilder out;
    out.setDirection(oboe::Direction::Output)->setFormat(oboe::AudioFormat::Float)
       ->setChannelCount(2)->setPerformanceMode(oboe::PerformanceMode::LowLatency)
       ->setSharingMode(oboe::SharingMode::Exclusive)->setDataCallback(this)
       ->setErrorCallback(this);
    if (sampleRate > 0) out.setSampleRate(sampleRate);
    if (outputDeviceId > 0) out.setDeviceId(outputDeviceId);
    if (bufferFrames > 0) out.setFramesPerDataCallback(bufferFrames);
    result = out.openStream(output_);
    if (result != oboe::Result::OK) {
        output_.reset();
        out.setSharingMode(oboe::SharingMode::Shared)->setAudioApi(oboe::AudioApi::Unspecified);
        result = out.openStream(output_);
    }
    if (result != oboe::Result::OK) { closeStreams(); return false; }
    const int32_t rate = output_->getSampleRate() > 0 ? output_->getSampleRate() : sampleRate;
    const int32_t callbackFrames = output_->getFramesPerDataCallback() > 0
        ? output_->getFramesPerDataCallback() : output_->getFramesPerBurst();
    const int32_t inputChannels = input_->getChannelCount() > 0 ? input_->getChannelCount() : 2;
    actualSampleRate_.store(rate, std::memory_order_release);
    actualFramesPerDataCallback_.store(callbackFrames, std::memory_order_release);
    inputChannelCount_.store(inputChannels, std::memory_order_release);
    const int32_t capacity = std::max<int32_t>(bufferFrames > 0 ? bufferFrames : callbackFrames, callbackFrames);
    inputInterleaved_.assign(static_cast<size_t>(capacity) * 2, 0.0f);
    inputLeft_.assign(capacity, 0.0f); inputRight_.assign(capacity, 0.0f);
    outputInterleaved_.assign(static_cast<size_t>(capacity) * 2, 0.0f);
    graph_.setSampleRate(static_cast<float>(rate), static_cast<uint32_t>(capacity));
    graph_.setAvailableInputChannelCount(inputChannels);
    running_.store(true, std::memory_order_release);
    if (input_->requestStart() != oboe::Result::OK || output_->requestStart() != oboe::Result::OK) {
        stop(); return false;
    }
    return true;
}

void AndroidOboeBackend::closeStreams() noexcept {
    if (output_) { output_->requestStop(); output_->close(); output_.reset(); }
    if (input_) { input_->requestStop(); input_->close(); input_.reset(); }
}
void AndroidOboeBackend::stop() noexcept {
    running_.store(false, std::memory_order_release);
    closeStreams();
}

oboe::DataCallbackResult AndroidOboeBackend::onAudioReady(oboe::AudioStream* stream, void* data, int32_t frames) {
    if (!running_.load(std::memory_order_acquire) || !data || frames <= 0) {
        if (data) std::memset(data, 0, static_cast<size_t>(std::max(frames, 0)) * 2 * sizeof(float));
        return oboe::DataCallbackResult::Continue;
    }
    if (stream != output_.get()) return oboe::DataCallbackResult::Continue;
    const int32_t capacity = static_cast<int32_t>(inputLeft_.size());
    if (frames > capacity) { std::memset(data, 0, static_cast<size_t>(frames) * 2 * sizeof(float)); return oboe::DataCallbackResult::Continue; }
    int32_t got = 0;
    if (input_) {
        const auto readResult = input_->read(inputInterleaved_.data(), frames, 0);
        if (readResult == oboe::Result::OK) got = readResult.value();
    }
    for (int32_t i = 0; i < frames; ++i) {
        inputLeft_[i] = (got > i) ? inputInterleaved_[static_cast<size_t>(i) * 2] : 0.0f;
        inputRight_[i] = (got > i) ? inputInterleaved_[static_cast<size_t>(i) * 2 + 1] : 0.0f;
    }
    const float* in[] = {inputLeft_.data(), inputRight_.data()};
    float* out[] = {outputInterleaved_.data(), outputInterleaved_.data() + frames};
    graph_.process(in, 2, out, static_cast<uint32_t>(frames));
    auto* interleaved = static_cast<float*>(data);
    for (int32_t i = 0; i < frames; ++i) {
        interleaved[static_cast<size_t>(i) * 2] = out[0][i];
        interleaved[static_cast<size_t>(i) * 2 + 1] = out[1][i];
    }
    return oboe::DataCallbackResult::Continue;
}
void AndroidOboeBackend::onErrorBeforeClose(oboe::AudioStream*, oboe::Result) { error_.store(true, std::memory_order_release); running_.store(false, std::memory_order_release); }
void AndroidOboeBackend::onErrorAfterClose(oboe::AudioStream*, oboe::Result) { error_.store(true, std::memory_order_release); }
}
