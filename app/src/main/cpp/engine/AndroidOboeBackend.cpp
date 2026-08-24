#include "AndroidOboeBackend.h"
#include <algorithm>
#include <cstring>
#include <android/log.h>

#define LOG_TAG "AndroidOboeBackend"
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)


namespace guitarrackcraft {

bool AndroidOboeBackend::start(int32_t sampleRate, int32_t inputDeviceId,
                               int32_t outputDeviceId, int32_t bufferFrames) {
    stop();
    error_.store(false, std::memory_order_release);

    auto configureInput = [&](oboe::AudioStreamBuilder& builder,
                              oboe::SharingMode sharing) {
        builder.setDirection(oboe::Direction::Input)
            ->setFormat(oboe::AudioFormat::Float)
            // Leave the input channel count unspecified: phone microphones may
            // be mono, and forcing two channels makes Oboe reject the stream.
            ->setPerformanceMode(oboe::PerformanceMode::LowLatency)
            ->setSharingMode(sharing)
            ->setFormatConversionAllowed(true)
            ->setSampleRateConversionQuality(oboe::SampleRateConversionQuality::Medium)
            ->setErrorCallback(this);
        if (sampleRate > 0) builder.setSampleRate(sampleRate);
        if (inputDeviceId > 0) builder.setDeviceId(inputDeviceId);
        if (bufferFrames > 0) builder.setFramesPerDataCallback(bufferFrames);
    };

    oboe::AudioStreamBuilder in;
    configureInput(in, oboe::SharingMode::Exclusive);
    auto result = in.openStream(input_);
    if (result != oboe::Result::OK) {
        LOGE("input open (exclusive) failed: %s", oboe::convertToText(result));
    }
    if (result != oboe::Result::OK) {
        input_.reset();
        // Rebuild the builder for the shared retry; do not carry stale stream
        // configuration (especially a forced channel count) across attempts.
        oboe::AudioStreamBuilder sharedIn;
        configureInput(sharedIn, oboe::SharingMode::Shared);
        sharedIn.setAudioApi(oboe::AudioApi::Unspecified);
        result = sharedIn.openStream(input_);
    }
    if (result != oboe::Result::OK) {
        LOGE("input open failed after shared retry: %s", oboe::convertToText(result));
        closeStreams();
        return false;
    }

    auto configureOutput = [&](oboe::AudioStreamBuilder& builder,
                               oboe::SharingMode sharing) {
        builder.setDirection(oboe::Direction::Output)
            ->setFormat(oboe::AudioFormat::Float)
            ->setChannelCount(2)
            ->setPerformanceMode(oboe::PerformanceMode::LowLatency)
            ->setSharingMode(sharing)
            ->setFormatConversionAllowed(true)
            ->setSampleRateConversionQuality(oboe::SampleRateConversionQuality::Medium)
            ->setDataCallback(this)
            ->setErrorCallback(this);
        if (sampleRate > 0) builder.setSampleRate(sampleRate);
        if (outputDeviceId > 0) builder.setDeviceId(outputDeviceId);
        if (bufferFrames > 0) builder.setFramesPerDataCallback(bufferFrames);
    };

    oboe::AudioStreamBuilder out;
    configureOutput(out, oboe::SharingMode::Exclusive);
    result = out.openStream(output_);
    if (result != oboe::Result::OK) {
        LOGE("output open (exclusive) failed: %s", oboe::convertToText(result));
    }
    if (result != oboe::Result::OK) {
        output_.reset();
        oboe::AudioStreamBuilder sharedOut;
        configureOutput(sharedOut, oboe::SharingMode::Shared);
        sharedOut.setAudioApi(oboe::AudioApi::Unspecified);
        result = sharedOut.openStream(output_);
    }
    if (result != oboe::Result::OK) {
        LOGE("output open failed after shared retry: %s", oboe::convertToText(result));
        closeStreams();
        return false;
    }

    const int32_t rate = output_->getSampleRate() > 0 ? output_->getSampleRate() : sampleRate;
    const int32_t callbackFrames = output_->getFramesPerDataCallback() > 0
        ? output_->getFramesPerDataCallback() : output_->getFramesPerBurst();
    const int32_t inputChannels = input_->getChannelCount();
    // RackGraph's hardware input contract is one or two channels. Do not
    // publish a channel count for which the callback cannot provide pointers.
    if (inputChannels < 1 || inputChannels > 2) { closeStreams(); return false; }
    actualSampleRate_.store(rate, std::memory_order_release);
    actualFramesPerDataCallback_.store(callbackFrames, std::memory_order_release);
    inputChannelCount_.store(inputChannels, std::memory_order_release);
    const int32_t capacity = std::max<int32_t>(bufferFrames > 0 ? bufferFrames : callbackFrames, callbackFrames);
    inputInterleaved_.assign(static_cast<size_t>(capacity) * static_cast<size_t>(inputChannels), 0.0f);
    inputLeft_.assign(capacity, 0.0f); inputRight_.assign(capacity, 0.0f);
    outputInterleaved_.assign(static_cast<size_t>(capacity) * 2, 0.0f);
    graph_.setSampleRate(static_cast<float>(rate), static_cast<uint32_t>(capacity));
    graph_.setAvailableInputChannelCount(inputChannels);
    running_.store(true, std::memory_order_release);
    const auto inputStartResult = input_->requestStart();
    if (inputStartResult != oboe::Result::OK) {
        LOGE("input requestStart failed: %s", oboe::convertToText(inputStartResult));
    }
    const auto outputStartResult = output_->requestStart();
    if (outputStartResult != oboe::Result::OK) {
        LOGE("output requestStart failed: %s", oboe::convertToText(outputStartResult));
    }
    if (inputStartResult != oboe::Result::OK || outputStartResult != oboe::Result::OK) {
        stop();
        return false;
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
    if (frames > capacity) {
        std::memset(data, 0, static_cast<size_t>(frames) * 2 * sizeof(float));
        return oboe::DataCallbackResult::Continue;
    }
    int32_t got = 0;
    const int32_t inputChannels = inputChannelCount_.load(std::memory_order_acquire);
    if (input_) {
        const auto readResult = input_->read(inputInterleaved_.data(), frames, 0);
        if (readResult == oboe::Result::OK) got = readResult.value();
    }
    for (int32_t i = 0; i < frames; ++i) {
        const bool available = got > i;
        const size_t offset = static_cast<size_t>(i) * static_cast<size_t>(inputChannels);
        inputLeft_[i] = available ? inputInterleaved_[offset] : 0.0f;
        inputRight_[i] = (available && inputChannels > 1)
            ? inputInterleaved_[offset + 1] : 0.0f;
    }
    const float* in[] = {inputLeft_.data(), inputRight_.data()};
    float* out[] = {outputInterleaved_.data(), outputInterleaved_.data() + frames};
    graph_.process(in, inputChannels, out, static_cast<uint32_t>(frames));
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
