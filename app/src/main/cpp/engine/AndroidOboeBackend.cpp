#include "AndroidOboeBackend.h"
#include <algorithm>
#if defined(GRC_ENABLE_RT_TIMING)
#include <chrono>
#endif
#include <cstring>

namespace guitarrackcraft {

void AndroidOboeBackend::AudioFrameSpscRing::prepare(uint32_t frames, uint32_t channels) {
    capacity_ = frames;
    channels_ = channels;
    storage_.assign(static_cast<size_t>(frames) * channels, 0.0f);
    // Touch every page while stopped, so neither callback faults pages in.
    for (volatile float& value : storage_) value = 0.0f;
    head_.store(0, std::memory_order_relaxed);
    tail_.store(0, std::memory_order_relaxed);
}

void AndroidOboeBackend::AudioFrameSpscRing::reset() noexcept {
    head_.store(0, std::memory_order_relaxed);
    tail_.store(0, std::memory_order_relaxed);
}

uint32_t AndroidOboeBackend::AudioFrameSpscRing::push(
    const float* source, uint32_t frames, std::atomic<uint64_t>& drops,
    std::atomic<uint64_t>& violations) noexcept {
    if (frames == 0 || !source || capacity_ == 0 || channels_ == 0) return 0;
    if (frames > capacity_) violations.fetch_add(frames, std::memory_order_relaxed);
    const uint64_t head = head_.load(std::memory_order_relaxed);
    const uint64_t tail = tail_.load(std::memory_order_acquire);
    const uint32_t available = static_cast<uint32_t>(std::min<uint64_t>(
        frames, capacity_ - std::min<uint64_t>(capacity_, head - tail)));
    for (uint32_t frame = 0; frame < available; ++frame) {
        const size_t slot = static_cast<size_t>((head + frame) % capacity_) * channels_;
        std::memcpy(storage_.data() + slot, source + static_cast<size_t>(frame) * channels_,
                    static_cast<size_t>(channels_) * sizeof(float));
    }
    if (available != 0) head_.store(head + available, std::memory_order_release);
    drops.fetch_add(frames - available, std::memory_order_relaxed);
    return available;
}

uint32_t AndroidOboeBackend::AudioFrameSpscRing::pop(
    float* destination, uint32_t frames, std::atomic<uint64_t>& underflows,
    std::atomic<uint64_t>& staleDrops) noexcept {
    if (frames == 0 || !destination || capacity_ == 0 || channels_ == 0) return 0;
    uint64_t tail = tail_.load(std::memory_order_relaxed);
    const uint64_t head = head_.load(std::memory_order_acquire);
    const uint64_t backlog = head - tail;
    const uint64_t targetBacklog = static_cast<uint64_t>(frames) * 2U;
    if (backlog > targetBacklog) {
        const uint64_t trimmed = backlog - targetBacklog;
        tail += trimmed;
        tail_.store(tail, std::memory_order_release);
        staleDrops.fetch_add(trimmed, std::memory_order_relaxed);
    }
    const uint32_t available = static_cast<uint32_t>(std::min<uint64_t>(frames, head - tail));
    for (uint32_t frame = 0; frame < available; ++frame) {
        const size_t slot = static_cast<size_t>((tail + frame) % capacity_) * channels_;
        std::memcpy(destination + static_cast<size_t>(frame) * channels_, storage_.data() + slot,
                    static_cast<size_t>(channels_) * sizeof(float));
    }
    if (available != 0) tail_.store(tail + available, std::memory_order_release);
    if (available < frames) {
        std::memset(destination + static_cast<size_t>(available) * channels_, 0,
                    static_cast<size_t>(frames - available) * channels_ * sizeof(float));
        underflows.fetch_add(frames - available, std::memory_order_relaxed);
    }
    return available;
}

bool AndroidOboeBackend::start(int32_t sampleRate, int32_t inputDeviceId,
                               int32_t outputDeviceId, int32_t bufferFrames) {
    stop();
    error_.store(false, std::memory_order_release);
    callbackCount_.store(0, std::memory_order_relaxed);
    callbackFrames_.store(0, std::memory_order_relaxed);
    lastCallbackNanoseconds_.store(0, std::memory_order_relaxed);
    peakCallbackNanoseconds_.store(0, std::memory_order_relaxed);
    callbackDeadlineBudgetNanoseconds_.store(0, std::memory_order_relaxed);
    callbackDeadlineMisses_.store(0, std::memory_order_relaxed);
    reopenRequested_.store(false, std::memory_order_release);

    auto configureInput = [&](oboe::AudioStreamBuilder& builder, oboe::SharingMode sharing) {
        builder.setDirection(oboe::Direction::Input)->setFormat(oboe::AudioFormat::Float)
            ->setPerformanceMode(oboe::PerformanceMode::LowLatency)->setSharingMode(sharing)
            ->setFormatConversionAllowed(true)
            ->setSampleRateConversionQuality(oboe::SampleRateConversionQuality::Medium)
            ->setDataCallback(this)->setErrorCallback(this);
        if (sampleRate > 0) builder.setSampleRate(sampleRate);
        if (inputDeviceId > 0) builder.setDeviceId(inputDeviceId);
        if (bufferFrames > 0) builder.setFramesPerDataCallback(bufferFrames);
    };
    oboe::AudioStreamBuilder in;
    configureInput(in, oboe::SharingMode::Exclusive);
    auto result = in.openStream(input_);
    if (result != oboe::Result::OK) {
        input_.reset();
        oboe::AudioStreamBuilder sharedIn;
        configureInput(sharedIn, oboe::SharingMode::Shared);
        sharedIn.setAudioApi(oboe::AudioApi::Unspecified);
        result = sharedIn.openStream(input_);
    }
    if (result != oboe::Result::OK) { closeStreams(); return false; }

    auto configureOutput = [&](oboe::AudioStreamBuilder& builder, oboe::SharingMode sharing) {
        builder.setDirection(oboe::Direction::Output)->setFormat(oboe::AudioFormat::Float)
            ->setChannelCount(2)->setPerformanceMode(oboe::PerformanceMode::LowLatency)
            ->setSharingMode(sharing)->setFormatConversionAllowed(true)
            ->setSampleRateConversionQuality(oboe::SampleRateConversionQuality::Medium)
            ->setDataCallback(this)->setErrorCallback(this);
        if (sampleRate > 0) builder.setSampleRate(sampleRate);
        if (outputDeviceId > 0) builder.setDeviceId(outputDeviceId);
        if (bufferFrames > 0) builder.setFramesPerDataCallback(bufferFrames);
    };
    oboe::AudioStreamBuilder out;
    configureOutput(out, oboe::SharingMode::Exclusive);
    result = out.openStream(output_);
    if (result != oboe::Result::OK) {
        output_.reset();
        oboe::AudioStreamBuilder sharedOut;
        configureOutput(sharedOut, oboe::SharingMode::Shared);
        sharedOut.setAudioApi(oboe::AudioApi::Unspecified);
        result = sharedOut.openStream(output_);
    }
    if (result != oboe::Result::OK) { closeStreams(); return false; }

    const int32_t rate = output_->getSampleRate() > 0 ? output_->getSampleRate() : sampleRate;
    const int32_t burst = std::max<int32_t>(1, output_->getFramesPerBurst());
    const int32_t callbackFrames = output_->getFramesPerDataCallback() > 0
        ? output_->getFramesPerDataCallback() : burst;
    const int32_t inputChannels = input_->getChannelCount();
    if (inputChannels < 1 || inputChannels > 2 || callbackFrames <= 0) { closeStreams(); return false; }
    const int32_t capacity = 8 * std::max(burst, callbackFrames);
    inputRing_.prepare(static_cast<uint32_t>(capacity), static_cast<uint32_t>(inputChannels));
    inputInterleaved_.assign(static_cast<size_t>(capacity) * inputChannels, 0.0f);
    inputLeft_.assign(capacity, 0.0f); inputRight_.assign(capacity, 0.0f);
    outputInterleaved_.assign(static_cast<size_t>(capacity) * 2, 0.0f);
    for (volatile float& value : inputInterleaved_) value = 0.0f;
    for (volatile float& value : inputLeft_) value = 0.0f;
    for (volatile float& value : inputRight_) value = 0.0f;
    for (volatile float& value : outputInterleaved_) value = 0.0f;

    const int32_t twoBursts = 2 * burst;
    output_->setBufferSizeInFrames(twoBursts);
    actualAudioApi_.store(static_cast<int32_t>(output_->getAudioApi()), std::memory_order_release);
    actualSampleRate_.store(rate, std::memory_order_release);
    actualFramesPerBurst_.store(burst, std::memory_order_release);
    actualFramesPerDataCallback_.store(callbackFrames, std::memory_order_release);
    preparedCapacity_.store(capacity, std::memory_order_release);
    actualBufferSize_.store(output_->getBufferSizeInFrames(), std::memory_order_release);
    actualPerformanceMode_.store(static_cast<int32_t>(output_->getPerformanceMode()), std::memory_order_release);
    actualSharingMode_.store(static_cast<int32_t>(output_->getSharingMode()), std::memory_order_release);
    actualDeviceId_.store(output_->getDeviceId(), std::memory_order_release);
    inputChannelCount_.store(inputChannels, std::memory_order_release);
    inputCallbackStream_.store(input_.get(), std::memory_order_release);
    outputCallbackStream_.store(output_.get(), std::memory_order_release);
    graph_.setSampleRate(static_cast<float>(rate), static_cast<uint32_t>(capacity));
    graph_.setAvailableInputChannelCount(inputChannels);
    running_.store(true, std::memory_order_release);
    const auto inputStart = input_->requestStart();
    const auto outputStart = output_->requestStart();
    if (inputStart != oboe::Result::OK || outputStart != oboe::Result::OK) { stop(); return false; }
    return true;
}

void AndroidOboeBackend::closeStreams() noexcept {
    if (output_) { output_->requestStop(); output_->close(); }
    outputCallbackStream_.store(nullptr, std::memory_order_release);
    output_.reset();
    if (input_) { input_->requestStop(); input_->close(); }
    inputCallbackStream_.store(nullptr, std::memory_order_release);
    input_.reset();
}
void AndroidOboeBackend::stop() noexcept {
    running_.store(false, std::memory_order_release);
    closeStreams();
    inputRing_.reset();
}

oboe::DataCallbackResult AndroidOboeBackend::onAudioReady(
        oboe::AudioStream* stream, void* data, int32_t frames) {
    const auto* inputStream = inputCallbackStream_.load(std::memory_order_acquire);
    const auto* outputStream = outputCallbackStream_.load(std::memory_order_acquire);
    const bool isInput = stream == inputStream;
    const bool isOutput = stream == outputStream;
#if defined(GRC_ENABLE_RT_TIMING)
    const auto timingStart = isOutput ? std::chrono::steady_clock::now()
                                      : std::chrono::steady_clock::time_point{};
    const uint32_t timingFrames = frames > 0 ? static_cast<uint32_t>(frames) : 0;
    auto recordTiming = [this, isOutput, timingStart, timingFrames](void*) noexcept {
        if (!isOutput) return;
        const auto elapsed = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - timingStart).count());
        lastCallbackNanoseconds_.store(elapsed, std::memory_order_relaxed);
        uint64_t peak = peakCallbackNanoseconds_.load(std::memory_order_relaxed);
        while (elapsed > peak && !peakCallbackNanoseconds_.compare_exchange_weak(
                   peak, elapsed, std::memory_order_relaxed, std::memory_order_relaxed)) {}
        const int32_t rate = actualSampleRate_.load(std::memory_order_relaxed);
        const uint64_t budget = rate > 0
            ? static_cast<uint64_t>(timingFrames) * 1'000'000'000ULL /
                  static_cast<uint32_t>(rate)
            : 0;
        callbackDeadlineBudgetNanoseconds_.store(budget, std::memory_order_relaxed);
        if (budget > 0 && elapsed > budget) {
            callbackDeadlineMisses_.fetch_add(1, std::memory_order_relaxed);
        }
    };
    std::unique_ptr<void, decltype(recordTiming)> timingGuard(
        isOutput ? reinterpret_cast<void*>(1) : nullptr, recordTiming);
#endif
    if (!data || frames <= 0 || (!isInput && !isOutput)) {
        return oboe::DataCallbackResult::Continue;
    }
    const uint32_t count = static_cast<uint32_t>(frames);
    const int32_t channels = inputChannelCount_.load(std::memory_order_acquire);
    const int32_t capacity = preparedCapacity_.load(std::memory_order_acquire);
    if (!running_.load(std::memory_order_acquire) ||
        count > static_cast<uint32_t>(std::max(capacity, 0))) {
        if (isOutput) std::memset(data, 0, static_cast<size_t>(frames) * 2 * sizeof(float));
        if (count > static_cast<uint32_t>(std::max(capacity, 0))) {
            capacityViolationCount_.fetch_add(count, std::memory_order_relaxed);
        }
        return oboe::DataCallbackResult::Continue;
    }
    if (isInput) {
        inputRing_.push(static_cast<const float*>(data), count, inputOverflowCount_,
                        capacityViolationCount_);
        return oboe::DataCallbackResult::Continue;
    }
    if (channels < 1 || channels > 2) return oboe::DataCallbackResult::Continue;
    callbackCount_.fetch_add(1, std::memory_order_relaxed);
    callbackFrames_.fetch_add(count, std::memory_order_relaxed);
    inputRing_.pop(inputInterleaved_.data(), count, inputUnderflowCount_, inputOverflowCount_);
    for (uint32_t i = 0; i < count; ++i) {
        const size_t offset = static_cast<size_t>(i) * channels;
        inputLeft_[i] = inputInterleaved_[offset];
        inputRight_[i] = channels > 1 ? inputInterleaved_[offset + 1] : 0.0f;
    }
    const float* inputs[] = {inputLeft_.data(), inputRight_.data()};
    float* outputs[] = {outputInterleaved_.data(), outputInterleaved_.data() + capacity};
    graph_.process(inputs, channels, outputs, count);
    auto* interleaved = static_cast<float*>(data);
    for (uint32_t i = 0; i < count; ++i) {
        interleaved[static_cast<size_t>(i) * 2] = outputs[0][i];
        interleaved[static_cast<size_t>(i) * 2 + 1] = outputs[1][i];
    }
    return oboe::DataCallbackResult::Continue;
}

void AndroidOboeBackend::onErrorBeforeClose(oboe::AudioStream*, oboe::Result) {
    error_.store(true, std::memory_order_release);
    reopenRequested_.store(true, std::memory_order_release);
}
void AndroidOboeBackend::onErrorAfterClose(oboe::AudioStream*, oboe::Result) {
    error_.store(true, std::memory_order_release);
    reopenRequested_.store(true, std::memory_order_release);
}
}
