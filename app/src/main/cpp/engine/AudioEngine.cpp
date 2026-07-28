/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * This file is part of Guitar RackCraft.
 *
 * Guitar RackCraft is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Guitar RackCraft is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Guitar RackCraft. If not, see <https://www.gnu.org/licenses/>.
 */

#include "AudioEngine.h"
#include "utils/WavIO.h"
#include "utils/ThreadUtils.h"
#include <oboe/OboeExtensions.h>
#include <android/log.h>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <unistd.h>
#include <algorithm>
#include <thread>
#include <limits>

#define LOG_TAG "AudioEngine"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace guitarrackcraft {

AudioEngine::AudioEngine()
    : sampleRate_(48000.0f)
    , isRunning_(false)
{
    inputPtrs_[0] = nullptr;
    inputPtrs_[1] = nullptr;
    outputPtrs_[0] = nullptr;
    outputPtrs_[1] = nullptr;
}

AudioEngine::~AudioEngine() {
    stop();
}

bool AudioEngine::start(float sampleRate, int32_t inputDeviceId,
                        int32_t outputDeviceId, int32_t bufferFrames) {
    LOGI("start() ENTER tid=%ld sampleRate=%.0f inputDev=%d outputDev=%d bufFrames=%d isRunning_=%d",
         getTid(), sampleRate, inputDeviceId, outputDeviceId, bufferFrames, isRunning_ ? 1 : 0);
    if (isRunning_) {
        LOGI("start() early return (already running)");
        return true;
    }

    sampleRate_ = sampleRate;
    inputDeviceId_ = inputDeviceId;
    outputDeviceId_ = outputDeviceId;
    requestedBufferFrames_ = bufferFrames;

    if (!createAudioStreams(sampleRate)) {
        LOGE("Failed to create audio streams");
        return false;
    }

    // Configure every track and the master chain for this callback quantum.
    LOGI("Using callback frame count: %u (power-of-2)", callbackFrameCount_);
    rackGraph_.setSampleRate(sampleRate_, callbackFrameCount_);
    rackGraph_.activate();
    rackGraph_.pauseAndResetTransport();

    cleanupStarted_.store(false, std::memory_order_release);
    isRunning_ = true;
    LOGI("start() EXIT tid=%ld Audio engine started at %.0f Hz", getTid(), sampleRate_);
    return true;
}

bool AudioEngine::startDirectUsbSession(float sampleRate, int32_t bitsPerSample,
                                        int32_t subslotBytes, int32_t channels,
                                        int32_t inputChannel, int32_t outputPair,
                                        int32_t bufferFrames) {
    if (!isRunning_.load(std::memory_order_acquire) &&
        !directUsbSession_.load(std::memory_order_acquire) &&
        directUsbRenderThread_.joinable()) {
        directUsbRenderThread_.join();
    }
    if (isRunning_.load() || !directUsbOutput_ || sampleRate <= 0.0f ||
        (bitsPerSample != 16 && bitsPerSample != 24 && bitsPerSample != 32) ||
        subslotBytes < (bitsPerSample + 7) / 8 ||
        subslotBytes > DirectUsbOutput::kMaxSubslotBytes ||
        channels < 2 || channels > DirectUsbOutput::kMaxDeviceChannels ||
        inputChannel < 0 ||
        inputChannel >= directUsbOutput_->captureChannelCount() ||
        outputPair < 0 || outputPair * 2 + 1 >= channels) {
        return false;
    }
    // A direct-USB graph runs on its own capture-clocked render thread. Keep
    // its quantum small even when the Android stream preference is large:
    // large 512/1024-frame quanta leave no scheduler headroom between USB
    // capture arrivals and cause playback-ring underruns.
    constexpr int32_t kDirectUsbMaxRenderFrames = 256;
    const int32_t renderFrames = bufferFrames > 0
        ? std::min<int32_t>(bufferFrames, kDirectUsbMaxRenderFrames) : 64;
    if (!directUsbOutput_->start(static_cast<int>(sampleRate), bitsPerSample,
                                 subslotBytes, channels, inputChannel,
                                 outputPair)) {
        LOGE("Direct USB transport start failed");
        return false;
    }
    sampleRate_ = sampleRate;
    callbackFrameCount_ = static_cast<uint32_t>(renderFrames);
    requestedBufferFrames_ = bufferFrames;
    directUsbBits_ = bitsPerSample;
    directUsbSubslotBytes_ = subslotBytes;
    directUsbChannels_ = channels;
    directUsbInputBuffer_.assign(static_cast<size_t>(renderFrames), 0.0f);
    directUsbOutputLeft_.assign(static_cast<size_t>(renderFrames), 0.0f);
    directUsbOutputRight_.assign(static_cast<size_t>(renderFrames), 0.0f);
    directUsbStartupLeft_.assign(static_cast<size_t>(renderFrames), 0.0f);
    directUsbStartupRight_.assign(static_cast<size_t>(renderFrames), 0.0f);
    directUsbCaptureWaitTimeouts_.store(0, std::memory_order_relaxed);
    directUsbWriteWaitTimeouts_.store(0, std::memory_order_relaxed);
    directUsbOutput_->setGraphQuantum(renderFrames);
    rackGraph_.setSampleRate(sampleRate_, callbackFrameCount_);
    rackGraph_.activate();
    rackGraph_.pauseAndResetTransport();
    cleanupStarted_.store(false, std::memory_order_release);
    directUsbRenderUrgentAudio_.store(false, std::memory_order_relaxed);
    directUsbSession_.store(true, std::memory_order_release);
    isRunning_.store(true, std::memory_order_release);
    directUsbRenderThread_ = std::thread(&AudioEngine::directUsbRenderLoop, this);
    return true;
}

void AudioEngine::cleanupEngineState() {
    if (cleanupStarted_.exchange(true, std::memory_order_acq_rel)) return;
    if (recorder_.isRecording()) recorder_.stopRecording();
    rackGraph_.pauseAndResetTransport();
    rackGraph_.deactivate();
}

void AudioEngine::stop() {
    LOGI("stop() entered tid=%ld isRunning_=%d", getTid(),
         isRunning_ ? 1 : 0);
    if (directUsbSession_.exchange(false, std::memory_order_acq_rel)) {
        isRunning_.store(false, std::memory_order_release);
        if (directUsbOutput_) directUsbOutput_->stop();
        if (directUsbRenderThread_.joinable()) directUsbRenderThread_.join();
        cleanupEngineState();
        return;
    }

    if (!isRunning_.exchange(false, std::memory_order_acq_rel)) {
        // Error callbacks can clear isRunning_ before the control thread
        // arrives. Reap every resource anyway; cleanupEngineState is
        // idempotent across concurrent USB/Oboe failure paths.
        closeStreams();
        if (directUsbRenderThread_.joinable())
            directUsbRenderThread_.join();
        if (directUsbOutput_) directUsbOutput_->stop();
        cleanupEngineState();
        return;
    }

    if (directUsbOutput_) directUsbOutput_->stop();
    closeStreams();
    cleanupEngineState();
}

bool AudioEngine::isRunning() const {
    return isRunning_;
}

AudioEngine::StreamInfo AudioEngine::getStreamInfo() const {
    StreamInfo info;
    if (outputStream_) {
        info.isAAudio = outputStream_->getAudioApi() == oboe::AudioApi::AAudio;
        info.outputExclusive = outputStream_->getSharingMode() == oboe::SharingMode::Exclusive;
        info.outputLowLatency = outputStream_->getPerformanceMode() == oboe::PerformanceMode::LowLatency;
        info.outputMMap = oboe::OboeExtensions::isMMapUsed(outputStream_.get());
        info.outputCallback = true; // always using callback
        info.framesPerBurst = outputStream_->getFramesPerBurst();
    }
    if (inputStream_) {
        info.inputExclusive = inputStream_->getSharingMode() == oboe::SharingMode::Exclusive;
        info.inputLowLatency = inputStream_->getPerformanceMode() == oboe::PerformanceMode::LowLatency;
    }
    return info;
}

double AudioEngine::getLatencyMs() const {
    if (!outputStream_) {
        return 0.0;
    }
    
    int64_t framesWritten = outputStream_->getFramesWritten();
    int64_t framesRead = outputStream_->getFramesRead();
    int32_t bufferSize = outputStream_->getBufferSizeInFrames();
    
    double latencyFrames = bufferSize + (framesWritten - framesRead);
    return (latencyFrames / sampleRate_) * 1000.0;
}

float AudioEngine::getInputLevel() const {
    return inputPeakLevel_.load();
}

float AudioEngine::getOutputLevel() const {
    return outputPeakLevel_.load();
}

float AudioEngine::getCpuLoad() const {
    return cpuLoad_.load();
}

int32_t AudioEngine::getXRunCount() const {
    int32_t oboeXruns = 0;
    if (outputStream_) {
        auto result = outputStream_->getXRunCount();
        oboeXruns = result ? result.value() : 0;
    }
    uint64_t directUsbDiscontinuities = 0;
    if (directUsbOutput_) {
        const auto transport = directUsbOutput_->transportStats();
        directUsbDiscontinuities =
            directUsbOutput_->xrunCount() +
            directUsbOutput_->captureXRunCount() +
            transport.captureTransferErrors +
            transport.playbackTransferErrors +
            directUsbCaptureWaitTimeouts_.load(std::memory_order_acquire) +
            directUsbWriteWaitTimeouts_.load(std::memory_order_acquire);
    }
    const uint64_t total =
        static_cast<uint64_t>(std::max(0, oboeXruns)) + directUsbDiscontinuities;
    return static_cast<int32_t>(std::min<uint64_t>(
        total, static_cast<uint64_t>(std::numeric_limits<int32_t>::max())));
}

bool AudioEngine::isInputClipping() const {
    return inputClipping_.load();
}

bool AudioEngine::isOutputClipping() const {
    return outputClipping_.load();
}

void AudioEngine::resetClipping() {
    inputClipping_.store(false);
    outputClipping_.store(false);
}

bool AudioEngine::loadTrackWav(RackPathId trackId, const std::string& path,
                               const std::string& displayName) {
    try {
        std::vector<float> samples;
        uint32_t fileRate = 0;
        uint32_t channels = 0;
        if (!readWavFile(path, samples, fileRate, channels) || samples.empty() ||
            fileRate == 0 || (channels != 1 && channels != 2) ||
            samples.size() % channels != 0) {
            LOGE("loadTrackWav: invalid WAV %s", path.c_str());
            return false;
        }
        auto clip = std::make_shared<WavClip>();
        clip->sampleRate = fileRate;
        clip->displayName = displayName;
        const size_t frames = samples.size() / channels;
        clip->left.resize(frames);
        if (channels == 1) {
            clip->left = std::move(samples);
        } else {
            clip->right.resize(frames);
            for (size_t frame = 0; frame < frames; ++frame) {
                clip->left[frame] = samples[frame * 2];
                clip->right[frame] = samples[frame * 2 + 1];
            }
        }
        return rackGraph_.attachTrackWav(trackId, std::move(clip));
    } catch (const std::exception& error) {
        LOGE("loadTrackWav failed: %s", error.what());
        return false;
    }
}

bool AudioEngine::unloadTrackWav(RackPathId trackId) {
    return rackGraph_.unloadTrackWav(trackId);
}

void AudioEngine::processRackBlock(const float* const* liveInputs, float* const* outputs,
                                   uint32_t numFrames) noexcept {
    if (rackBypass_.load(std::memory_order_relaxed)) {
        for (uint32_t channel = 0; channel < 2; ++channel) {
            if (outputs && outputs[channel]) {
                std::memset(outputs[channel], 0, sizeof(float) * numFrames);
            }
        }
        return;
    }
    rackGraph_.process(liveInputs, outputs, numFrames);
}

void AudioEngine::directUsbRenderLoop() {
    directUsbRenderUrgentAudio_.store(
        setCurrentThreadUrgentAudio("UsbAudioRender"),
        std::memory_order_release);
    const int32_t frames = static_cast<int32_t>(callbackFrameCount_);
    // A capture completion can overshoot its wait threshold by one ISO
    // transfer. Leave that margin below the driver's two-quantum capture
    // latency ceiling; the second block is then collected immediately.
    constexpr int32_t kCapturePreRollMarginFrames = 64;
    const int32_t startupFrames = std::max(
        frames, frames * 2 - kCapturePreRollMarginFrames);
    const auto period = std::chrono::duration<double>(
        static_cast<double>(frames) / static_cast<double>(sampleRate_));
    const int writeTimeoutMs = std::max(
        2, static_cast<int>(std::ceil(period.count() * 2000.0)));

    const auto canContinue = [this]() noexcept {
        return directUsbSession_.load(std::memory_order_acquire) &&
            directUsbOutput_ && directUsbOutput_->isStreaming();
    };
    const auto waitForCapture = [this, &canContinue](int32_t required) noexcept {
        if (directUsbOutput_ &&
            directUsbOutput_->waitForCaptureFrames(required, 1000)) {
            return true;
        }
        if (canContinue()) {
            directUsbCaptureWaitTimeouts_.fetch_add(1, std::memory_order_relaxed);
        }
        return false;
    };
    const auto renderBlock = [this, frames, period]() noexcept {
        directUsbOutput_->readMonoInput(directUsbInputBuffer_.data(), frames);
        inputPtrs_[0] = directUsbInputBuffer_.data();
        inputPtrs_[1] = directUsbInputBuffer_.data();
        outputPtrs_[0] = directUsbOutputLeft_.data();
        outputPtrs_[1] = directUsbOutputRight_.data();
        const auto began = std::chrono::steady_clock::now();
        processRackBlock(inputPtrs_, outputPtrs_, frames);
        const auto elapsed = std::chrono::steady_clock::now() - began;
        cpuLoad_.store(std::min(
            1.0f,
            static_cast<float>(std::chrono::duration<double>(elapsed).count() / period.count())
        ), std::memory_order_relaxed);

        float inputPeak = 0.0f;
        float outputPeak = 0.0f;
        for (int32_t i = 0; i < frames; ++i) {
            inputPeak = std::max(inputPeak, std::fabs(directUsbInputBuffer_[i]));
            outputPeak = std::max(outputPeak, std::max(
                std::fabs(directUsbOutputLeft_[i]), std::fabs(directUsbOutputRight_[i])));
        }
        inputPeakHold_ = std::max(inputPeak, inputPeakHold_ * kPeakDecay);
        outputPeakHold_ = std::max(outputPeak, outputPeakHold_ * kPeakDecay);
        inputPeakLevel_.store(inputPeakHold_, std::memory_order_relaxed);
        outputPeakLevel_.store(outputPeakHold_, std::memory_order_relaxed);
        if (inputPeak >= kClippingThreshold) inputClipping_.store(true, std::memory_order_relaxed);
        if (outputPeak >= kClippingThreshold) outputClipping_.store(true, std::memory_order_relaxed);
        if (recorder_.isRecording()) {
            recorder_.feedAudio(directUsbInputBuffer_.data(), directUsbOutputLeft_.data(),
                                directUsbOutputRight_.data(), frames);
        }
    };
    const auto submitBlock = [this, frames, writeTimeoutMs, &canContinue](
                                 const float* left, const float* right) noexcept {
        int32_t submittedFrames = 0;
        while (submittedFrames < frames) {
            const int32_t remainingFrames = frames - submittedFrames;
            if (!directUsbOutput_ ||
                !directUsbOutput_->waitForWritableFrames(
                    remainingFrames, writeTimeoutMs)) {
                if (canContinue()) {
                    directUsbWriteWaitTimeouts_.fetch_add(1, std::memory_order_relaxed);
                }
                return false;
            }
            const int written = directUsbOutput_->writeStereo(
                left + submittedFrames, right + submittedFrames, remainingFrames);
            if (written <= 0) {
                directUsbWriteWaitTimeouts_.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
            submittedFrames += written;
        }
        return true;
    };

    bool outputPrimed = false;
    while (directUsbSession_.load(std::memory_order_acquire) &&
           isRunning_.load(std::memory_order_acquire)) {
        if (!outputPrimed) {
            if (!waitForCapture(startupFrames)) {
                if (!canContinue()) break;
                continue;
            }
            renderBlock();
            std::memcpy(directUsbStartupLeft_.data(), directUsbOutputLeft_.data(),
                        static_cast<size_t>(frames) * sizeof(float));
            std::memcpy(directUsbStartupRight_.data(), directUsbOutputRight_.data(),
                        static_cast<size_t>(frames) * sizeof(float));

            if (!waitForCapture(frames)) {
                if (!canContinue()) break;
                continue;
            }
            renderBlock();
            if (!submitBlock(directUsbStartupLeft_.data(), directUsbStartupRight_.data()) ||
                !submitBlock(directUsbOutputLeft_.data(), directUsbOutputRight_.data())) {
                if (!canContinue()) break;
                continue;
            }
            outputPrimed = true;
            continue;
        }

        if (!waitForCapture(frames)) {
            if (!canContinue()) break;
            continue;
        }
        renderBlock();
        if (!submitBlock(directUsbOutputLeft_.data(), directUsbOutputRight_.data()) &&
            !canContinue()) {
            break;
        }
    }
    // Runtime transfer failure: retire all transport, recording, and plugin
    // state instead of spinning on empty rings or leaving an active graph.
    if (directUsbSession_.exchange(false, std::memory_order_acq_rel)) {
        isRunning_.store(false, std::memory_order_release);
        if (directUsbOutput_) directUsbOutput_->stop();
        cleanupEngineState();
    }
}

oboe::DataCallbackResult AudioEngine::onAudioReady(
    oboe::AudioStream* audioStream,
    void* audioData,
    int32_t numFrames) {
    if (!isRunning_.load(std::memory_order_acquire) || numFrames <= 0)
        return oboe::DataCallbackResult::Continue;
    // Only process when the output stream needs data (we do not set callback on input).
    if (audioStream != outputStream_.get()) {
        return oboe::DataCallbackResult::Continue;
    }
    // Direct USB owns the graph and the USB sink on its capture-clocked
    // render thread. Never process or write it from an Oboe callback: that
    // would race the graph and violate the driver's SPSC playback ring.
    if (directUsbSession_.load(std::memory_order_acquire)) {
        const int32_t channels = std::max<int32_t>(1, audioStream->getChannelCount());
        std::memset(audioData, 0,
                    static_cast<size_t>(numFrames) * channels * sizeof(float));
        return oboe::DataCallbackResult::Continue;
    }
    float* outputData = static_cast<float*>(audioData);
    const int32_t numChannels =
        std::max<int32_t>(1, audioStream->getChannelCount());
    if (inputBuffer_.size() < static_cast<size_t>(numFrames) ||
        outputBufferLeft_.size() < static_cast<size_t>(numFrames) ||
        outputBufferRight_.size() < static_cast<size_t>(numFrames)) {
        std::memset(outputData, 0,
                    static_cast<size_t>(numFrames) * numChannels *
                        sizeof(float));
        return oboe::DataCallbackResult::Continue;
    }

    // Always acquire physical live input; per-track WAV substitution is in RackGraph.
    if (directUsbOutput_ && directUsbOutput_->isStreaming()) {
        std::memset(inputBuffer_.data(), 0, numFrames * sizeof(float));
    } else {
        int32_t framesRead = 0;
        if (inputStream_) {
            auto result = inputStream_->read(inputBuffer_.data(), numFrames, 0);
            if (result == oboe::Result::OK) framesRead = result.value();
        }
        if (framesRead < numFrames) {
            std::memset(inputBuffer_.data() + framesRead, 0,
                        (numFrames - framesRead) * sizeof(float));
        }
    }

    // Input peak metering and clipping
    float inputPeak = 0.0f;
    bool inputClip = false;
    for (int32_t i = 0; i < numFrames; ++i) {
        float s = std::fabs(inputBuffer_[i]);
        if (s > inputPeak) inputPeak = s;
        if (s >= kClippingThreshold) inputClip = true;
    }
    inputPeakHold_ = std::max(inputPeak, inputPeakHold_ * kPeakDecay);
    inputPeakLevel_.store(inputPeakHold_);
    if (inputClip) inputClipping_.store(true);


    // Set up input pointers (mono guitar input -> stereo)
    inputPtrs_[0] = inputBuffer_.data();
    inputPtrs_[1] = inputBuffer_.data();  // Duplicate mono to stereo

    // Set up output pointers (always process into our buffers for metering)
    outputPtrs_[0] = outputBufferLeft_.data();
    outputPtrs_[1] = outputBufferRight_.data();

    // Process the complete parallel rack and master chain.
    const double bufferDurationMs =
        (numFrames / static_cast<double>(sampleRate_)) * 1000.0;
    const auto t0 = std::chrono::high_resolution_clock::now();
    processRackBlock(inputPtrs_, outputPtrs_, numFrames);
    const auto t1 = std::chrono::high_resolution_clock::now();
    const double processMs =
        std::chrono::duration<double, std::milli>(t1 - t0).count();
    cpuLoad_.store(std::min(1.0f, static_cast<float>(processMs / bufferDurationMs)),
                   std::memory_order_relaxed);

    // Output peak metering (from buffers we wrote to)
    float outputPeak = 0.0f;
    bool outputClip = false;
    for (int32_t i = 0; i < numFrames; ++i) {
        float s = std::max(std::fabs(outputBufferLeft_[i]), std::fabs(outputBufferRight_[i]));
        if (s > outputPeak) outputPeak = s;
        if (s >= kClippingThreshold) outputClip = true;
    }
    outputPeakHold_ = std::max(outputPeak, outputPeakHold_ * kPeakDecay);
    outputPeakLevel_.store(outputPeakHold_);
    if (outputClip) outputClipping_.store(true);

    if (recorder_.isRecording()) {
        recorder_.feedAudio(inputBuffer_.data(),
                            outputBufferLeft_.data(),
                            outputBufferRight_.data(),
                            numFrames);
    }

    if (numChannels == 2) {
        for (int32_t i = 0; i < numFrames; ++i) {
            outputData[i * 2] = outputBufferLeft_[i];
            outputData[i * 2 + 1] = outputBufferRight_[i];
        }
    } else {
        for (int32_t i = 0; i < numFrames; ++i) {
            outputData[i] = (outputBufferLeft_[i] + outputBufferRight_[i]) * 0.5f;
        }
    }

    return oboe::DataCallbackResult::Continue;
}

void AudioEngine::onErrorBeforeClose(oboe::AudioStream* oboeStream, oboe::Result error) {
    LOGE("onErrorBeforeClose tid=%ld stream=%p error=%s (set isRunning_=false so callback bails)",
         getTid(), static_cast<void*>(oboeStream), oboe::convertToText(error));
    // Signal callback to exit immediately; Oboe will close the stream after we return.
    isRunning_ = false;
}

void AudioEngine::onErrorAfterClose(oboe::AudioStream* oboeStream, oboe::Result error) {
    LOGE("onErrorAfterClose tid=%ld stream=%p error=%s", getTid(), static_cast<void*>(oboeStream), oboe::convertToText(error));
    isRunning_ = false;
    // Do NOT reset() the stream here. The underlying AAudio stream is already closed by
    // Oboe/the system (e.g. AudioBoost cancelling boost can trigger this). If we run
    // outputStream_.reset() on this thread, the destructor (~AAudioLoader) runs while
    // the audio callback thread may still be inside Oboe -> pthread_mutex_lock on
    // destroyed mutex (SIGABRT). Leave the stream object alive; stop() -> closeStreams()
    // will run later (from lifecycle or user) and destroy it on the main thread after
    // the 250ms sleep, when the callback thread is guaranteed idle.
}

bool AudioEngine::createAudioStreams(float sampleRate) {
    // Enable MMAP data path for lowest latency (must be set before opening streams).
    // Without this, AAudio uses the legacy non-MMAP path and cannot grant exclusive mode.
    oboe::OboeExtensions::setMMapEnabled(true);
    LOGI("MMAP supported=%d enabled=%d", oboe::OboeExtensions::isMMapSupported(),
         oboe::OboeExtensions::isMMapEnabled());

    // --- Input stream (mono, for guitar) ---
    // Force AAudio API — OpenSL ES cannot do exclusive or MMAP.
    // Oboe's QuirksManager may silently choose OpenSL ES otherwise.
    // Use a dedicated builder to avoid leaking input-only settings to output.
    // Do NOT set a callback on input — only the output stream drives the callback.
    // We read from the input inside the output stream's onAudioReady.
    oboe::AudioStreamBuilder inputBuilder;
    inputBuilder.setDirection(oboe::Direction::Input)
           ->setAudioApi(oboe::AudioApi::AAudio)
           ->setPerformanceMode(oboe::PerformanceMode::LowLatency)
           ->setSharingMode(oboe::SharingMode::Exclusive)
           ->setFormat(oboe::AudioFormat::Float)
           ->setChannelCount(1)
           ->setSampleRate(static_cast<int32_t>(sampleRate))
           ->setInputPreset(oboe::InputPreset::VoiceRecognition);

    if (inputDeviceId_ != 0) {
        inputBuilder.setDeviceId(inputDeviceId_);
        LOGI("Input device ID set to %d", inputDeviceId_);
    }

    oboe::AudioStream* inputStreamPtr = nullptr;
    oboe::Result result = inputBuilder.openStream(&inputStreamPtr);
    if (result != oboe::Result::OK) {
        // AAudio failed — retry without forcing API (let Oboe pick)
        LOGE("AAudio input open failed (%s), retrying with default API", oboe::convertToText(result));
        inputBuilder.setAudioApi(oboe::AudioApi::Unspecified);
        result = inputBuilder.openStream(&inputStreamPtr);
        if (result != oboe::Result::OK) {
            LOGE("Failed to open input stream: %s", oboe::convertToText(result));
            return false;
        }
    }
    inputStream_.reset(inputStreamPtr);

    LOGI("Input stream opened: api=%d sharing=%d perf=%d mmap=%d",
         static_cast<int>(inputStream_->getAudioApi()),
         static_cast<int>(inputStream_->getSharingMode()),
         static_cast<int>(inputStream_->getPerformanceMode()),
         oboe::OboeExtensions::isMMapUsed(inputStream_.get()));

    // Use actual sample rate from stream
    sampleRate_ = static_cast<float>(inputStream_->getSampleRate());

    // --- Output stream (stereo) ---
    oboe::AudioStreamBuilder outputBuilder;
    outputBuilder.setDirection(oboe::Direction::Output)
           ->setAudioApi(oboe::AudioApi::AAudio)
           ->setPerformanceMode(oboe::PerformanceMode::LowLatency)
           ->setSharingMode(oboe::SharingMode::Exclusive)
           ->setFormat(oboe::AudioFormat::Float)
           ->setChannelCount(2)
           ->setSampleRate(static_cast<int32_t>(sampleRate_))
           ->setUsage(oboe::Usage::Game)
           ->setCallback(this);

    if (outputDeviceId_ != 0) {
        outputBuilder.setDeviceId(outputDeviceId_);
        LOGI("Output device ID set to %d", outputDeviceId_);
    }

    oboe::AudioStream* outputStreamPtr = nullptr;
    result = outputBuilder.openStream(&outputStreamPtr);
    if (result != oboe::Result::OK) {
        // AAudio failed — retry without forcing API
        LOGE("AAudio output open failed (%s), retrying with default API", oboe::convertToText(result));
        outputBuilder.setAudioApi(oboe::AudioApi::Unspecified);
        result = outputBuilder.openStream(&outputStreamPtr);
        if (result != oboe::Result::OK) {
            LOGE("Failed to open output stream: %s", oboe::convertToText(result));
            closeStreams();
            return false;
        }
    }

    LOGI("Output stream opened: api=%d sharing=%d perf=%d mmap=%d",
         static_cast<int>(outputStreamPtr->getAudioApi()),
         static_cast<int>(outputStreamPtr->getSharingMode()),
         static_cast<int>(outputStreamPtr->getPerformanceMode()),
         oboe::OboeExtensions::isMMapUsed(outputStreamPtr));

    // Determine callback block size.
    // If the user requested a specific buffer size, use it directly.
    // Otherwise, ensure power-of-2 for convolver plugin compatibility.
    {
        if (requestedBufferFrames_ > 0) {
            callbackFrameCount_ = static_cast<uint32_t>(requestedBufferFrames_);
            LOGI("Using user-requested buffer frames: %u", callbackFrameCount_);
            outputStreamPtr->close();
            delete outputStreamPtr;
            outputStreamPtr = nullptr;

            outputBuilder.setFramesPerCallback(requestedBufferFrames_);
            result = outputBuilder.openStream(&outputStreamPtr);
            if (result != oboe::Result::OK) {
                LOGE("Failed to reopen output stream with requested buffer: %s",
                     oboe::convertToText(result));
                closeStreams();
                return false;
            }
        } else {
            int32_t framesPerBurst = outputStreamPtr->getFramesPerBurst();
            uint32_t po2 = 1;
            while (po2 < static_cast<uint32_t>(framesPerBurst)) po2 <<= 1;
            callbackFrameCount_ = po2;

            bool isPo2 = (framesPerBurst > 0) &&
                          ((framesPerBurst & (framesPerBurst - 1)) == 0);
            if (!isPo2) {
                LOGI("framesPerBurst=%d not power-of-2, reopening with framesPerCallback=%u",
                     framesPerBurst, po2);
                outputStreamPtr->close();
                delete outputStreamPtr;
                outputStreamPtr = nullptr;

                outputBuilder.setFramesPerCallback(static_cast<int32_t>(po2));
                result = outputBuilder.openStream(&outputStreamPtr);
                if (result != oboe::Result::OK) {
                    LOGE("Failed to reopen output stream with po2 callback: %s",
                         oboe::convertToText(result));
                    closeStreams();
                    return false;
                }
            } else {
                LOGI("framesPerBurst=%d already power-of-2", framesPerBurst);
            }
        }
    }

    outputStream_.reset(outputStreamPtr);

    // Start streams
    // Allocate every callback buffer before the output stream can invoke us.
    const int32_t bufferSize = std::max<int32_t>(
        outputStream_->getBufferSizeInFrames(),
        static_cast<int32_t>(callbackFrameCount_));
    inputBuffer_.resize(bufferSize);
    outputBufferLeft_.resize(bufferSize);
    outputBufferRight_.resize(bufferSize);

    result = inputStream_->requestStart();
    if (result != oboe::Result::OK) {
        LOGE("Failed to start input stream: %s", oboe::convertToText(result));
        closeStreams();
        return false;
    }

    result = outputStream_->requestStart();
    if (result != oboe::Result::OK) {
        LOGE("Failed to start output stream: %s", oboe::convertToText(result));
        closeStreams();
        return false;
    }


    LOGI("Audio streams created: %d Hz, buffer size: %d frames", 
         static_cast<int>(sampleRate_), bufferSize);

    return true;
}

void AudioEngine::closeStreams() {
    LOGI("closeStreams() ENTER tid=%ld (caller thread; AudioTrack callback is different tid)", getTid());
    
    // First, signal the callback to stop immediately to prevent new callbacks
    // from starting while we're tearing down.
    isRunning_.store(false);
    
    if (inputStream_) {
        LOGI("closeStreams() input stream stop+close+reset");
        inputStream_->stop();
        inputStream_->close();
        inputStream_.reset();
    }

    if (outputStream_) {
        LOGI("closeStreams() output stream stop");
        outputStream_->stop();
        LOGI("closeStreams() output stream close");
        outputStream_->close();
        // Wait for any in-flight AAudio callback to finish before destroying the
        // stream object. close() already removed us from AAudioStreamCollection,
        // so late callbacks will see !isStreamAlive and return Stop. If we reset()
        // too soon, ~AudioStreamAAudio / ~AAudioLoader run while the AudioTrack
        // thread is still in getStream() -> destroyed mutex (SIGABRT).
        // 
        // INCREASED from 250ms to 500ms: The 250ms delay was not sufficient on some
        // devices (e.g., OnePlus) where the audio callback thread takes longer to
        // fully exit, especially when the app is being force-closed or when there
        // are concurrent EGL/GL operations that may interfere with the audio subsystem.
        static constexpr int kStreamCloseSleepMs = 500;
        LOGI("closeStreams() sleep %dms before outputStream_.reset() [tid=%ld]", kStreamCloseSleepMs, getTid());
        std::this_thread::sleep_for(std::chrono::milliseconds(kStreamCloseSleepMs));
        LOGI("closeStreams() outputStream_.reset() NOW tid=%ld", getTid());
        outputStream_.reset();
        LOGI("closeStreams() output stream destroyed tid=%ld", getTid());
    }
    LOGI("closeStreams() done");
}

} // namespace guitarrackcraft
