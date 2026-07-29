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
namespace {
constexpr int32_t usbFailureCode(monotrypt::usb::StartError error) noexcept {
    return static_cast<int32_t>(error);
}
constexpr float kMeterReleaseSeconds = 0.15f;

float meterDecayForBlock(int32_t frames, float sampleRate) noexcept {
    if (frames <= 0 || sampleRate <= 0.0f) return 0.0f;
    return std::max(
        0.0f,
        1.0f - static_cast<float>(frames) / (sampleRate * kMeterReleaseSeconds)
    );
}
} // namespace

AudioEngine::AudioEngine()
    : sampleRate_(48000.0f)
    , isRunning_(false)
{
    inputPtrs_[0] = nullptr;
    inputPtrs_[1] = nullptr;
    outputPtrs_[0] = nullptr;
    outputPtrs_[1] = nullptr;
    cleanupWorker_ = std::thread(&AudioEngine::cleanupWorkerLoop, this);
}

AudioEngine::~AudioEngine() {
    stop();
    cleanupWorkerStop_.store(true, std::memory_order_release);
    lifecycleCv_.notify_one();
    if (cleanupWorker_.joinable()) cleanupWorker_.join();
}


bool AudioEngine::startDirectUsbSession(float sampleRate, int32_t bitsPerSample,
                                        int32_t subslotBytes, int32_t channels,
                                        int32_t inputChannel, int32_t outputPair,
                                        int32_t bufferFrames, int32_t periodMultiplier) {
    std::lock_guard<std::mutex> lifecycleCallLock(publicLifecycleMutex_);
    if (directUsbSession_.load(std::memory_order_acquire)) {
        return true;
    }
    waitForDirectUsbCleanup();
    if (isRunning_.load(std::memory_order_acquire) || !directUsbOutput_ ||
        sampleRate <= 0.0f ||
        (bitsPerSample != 16 && bitsPerSample != 24 && bitsPerSample != 32) ||
        subslotBytes < (bitsPerSample + 7) / 8 ||
        subslotBytes > DirectUsbOutput::kMaxSubslotBytes ||
        channels < 2 || channels > DirectUsbOutput::kMaxDeviceChannels ||
        inputChannel < 0 ||
        inputChannel >= directUsbOutput_->captureChannelCount() ||
        outputPair < 0 || outputPair * 2 + 1 >= channels) {
        return false;
    }
    const int32_t renderFrames = bufferFrames > 0
        ? std::clamp<int32_t>(bufferFrames, monotrypt::usb::kMinGraphQuantum,
                              DirectUsbOutput::kMaxGraphQuantum)
        : 64;
    const int32_t multiplier = monotrypt::usb::clampPeriodMultiplier(periodMultiplier);
    directUsbState_.store(DirectUsbState::Starting, std::memory_order_release);
    directUsbSessionId_.fetch_add(1, std::memory_order_acq_rel);
    directUsbFailureCode_.store(0, std::memory_order_release);

    // start() negotiates the device and prepares duplex capture/storage, but
    // deliberately leaves OUT unarmed until the render thread has primed it.
    directUsbEffectiveQuantum_.store(static_cast<uint32_t>(renderFrames), std::memory_order_release);
    directUsbPeriodMultiplier_.store(multiplier, std::memory_order_release);
    directUsbLastDspNs_.store(0, std::memory_order_relaxed);
    directUsbPeakDspNs_.store(0, std::memory_order_relaxed);
    directUsbLastCycleNs_.store(0, std::memory_order_relaxed);
    directUsbPeakCycleNs_.store(0, std::memory_order_relaxed);
    directUsbDeadlineBudgetNs_.store(0, std::memory_order_relaxed);
    directUsbDeadlineMisses_.store(0, std::memory_order_relaxed);
    if (!directUsbOutput_->start(static_cast<int>(sampleRate), bitsPerSample,
                                 subslotBytes, channels, inputChannel,
                                 outputPair)) {
        const int32_t failure = directUsbOutput_->lastErrorCode();
        LOGE("Direct USB transport start failed: code=%d detail=%s",
             failure, directUsbOutput_->lastErrorDetail().c_str());
        directUsbFailureCode_.store(
            failure != usbFailureCode(monotrypt::usb::StartError::Ok)
                ? failure
                : usbFailureCode(monotrypt::usb::StartError::Unknown),
            std::memory_order_release);
        directUsbState_.store(DirectUsbState::Failed, std::memory_order_release);
        return false;
    }
    directUsbOutput_->setGraphQuantum(renderFrames, multiplier);
    const int32_t primeFrames = std::max(0, directUsbOutput_->startupPrimeFrames());
    const int32_t startupBlocks =
        (primeFrames + renderFrames - 1) / renderFrames;
    directUsbPrimeFrames_.store(static_cast<uint32_t>(primeFrames), std::memory_order_release);
    directUsbSteadyTargetFrames_.store(
        static_cast<uint32_t>(std::max(0, directUsbOutput_->playbackTargetFrames())),
        std::memory_order_release);
    try {
        directUsbInputBuffer_.assign(static_cast<size_t>(renderFrames), 0.0f);
        directUsbOutputLeft_.assign(static_cast<size_t>(renderFrames), 0.0f);
        directUsbOutputRight_.assign(static_cast<size_t>(renderFrames), 0.0f);
        directUsbStartupLeft_.assign(
            static_cast<size_t>(startupBlocks) * renderFrames, 0.0f);
        directUsbStartupRight_.assign(
            static_cast<size_t>(startupBlocks) * renderFrames, 0.0f);
    } catch (const std::bad_alloc&) {
        LOGE("Direct USB startup buffer allocation failed");
        directUsbOutput_->requestStop();
        directUsbFailureCode_.store(
            usbFailureCode(monotrypt::usb::StartError::Unknown),
            std::memory_order_release);
        directUsbState_.store(DirectUsbState::Failed, std::memory_order_release);
        directUsbOutput_->stop();
        return false;
    }
    sampleRate_ = sampleRate;
    callbackFrameCount_ = static_cast<uint32_t>(renderFrames);
    publishedSampleRate_.store(sampleRate, std::memory_order_release);
    publishedCallbackFrameCount_.store(callbackFrameCount_, std::memory_order_release);
    directUsbBits_ = bitsPerSample;
    directUsbSubslotBytes_ = subslotBytes;
    directUsbChannels_ = channels;
    directUsbStartupBlocks_ = startupBlocks;
    directUsbCaptureWaitTimeouts_.store(0, std::memory_order_relaxed);
    directUsbWriteWaitTimeouts_.store(0, std::memory_order_relaxed);
    rackGraph_.setSampleRate(sampleRate_, callbackFrameCount_);
    rackGraph_.activate();
    rackGraph_.pauseAndResetTransport();
    cleanupStarted_.store(false, std::memory_order_release);
    directUsbRenderUrgentAudio_.store(false, std::memory_order_relaxed);
    directUsbSession_.store(true, std::memory_order_release);
    isRunning_.store(false, std::memory_order_release);
    try {
        directUsbRenderThread_ = std::thread(&AudioEngine::directUsbRenderLoop, this);
    } catch (...) {
        directUsbSession_.store(false, std::memory_order_release);
        isRunning_.store(false, std::memory_order_release);
        directUsbOutput_->requestStop();
        directUsbFailureCode_.store(
            usbFailureCode(monotrypt::usb::StartError::Unknown),
            std::memory_order_release);
        directUsbState_.store(DirectUsbState::Failed, std::memory_order_release);
        directUsbOutput_->stop();
        cleanupEngineState();
        return false;

    }
    return true;
}

bool AudioEngine::openDirectUsbDevice(int fd) {
    std::lock_guard<std::mutex> lifecycleCallLock(publicLifecycleMutex_);
    waitForDirectUsbCleanup();
    if (fd < 0 || directUsbSession_.load(std::memory_order_acquire) ||
        isRunning_.load(std::memory_order_acquire) ||
        directUsbState_.load(std::memory_order_acquire) != DirectUsbState::Stopped) {
        return false;
    }
    return directUsbOutput_ && directUsbOutput_->open(fd);
}

void AudioEngine::cleanupWorkerLoop() {
    for (;;) {
        std::unique_lock<std::mutex> lock(lifecycleMutex_);
        lifecycleCv_.wait(lock, [this] {
            return cleanupRequested_.load(std::memory_order_acquire) ||
                   cleanupWorkerStop_.load(std::memory_order_acquire);
        });
        if (cleanupWorkerStop_.load(std::memory_order_acquire) &&
            !cleanupRequested_.load(std::memory_order_acquire)) return;
        if (!cleanupRequested_.exchange(false, std::memory_order_acq_rel)) continue;
        lock.unlock();
        finishDirectUsbCleanup();
        cleanupDoneCv_.notify_all();
    }
}

void AudioEngine::requestDirectUsbCleanup(int32_t failureCode) noexcept {
    if (failureCode != 0) directUsbFailureCode_.store(failureCode, std::memory_order_release);
    cleanupRequested_.store(true, std::memory_order_release);
    lifecycleCv_.notify_one();
}
bool AudioEngine::waitForDirectUsbCleanup() {
    std::unique_lock<std::mutex> lock(lifecycleMutex_);
    const bool pending = directUsbRenderThread_.joinable() ||
                         cleanupInProgress_ ||
                         cleanupRequested_.load(std::memory_order_acquire);
    if (!pending) return false;
    cleanupRequested_.store(true, std::memory_order_release);
    lifecycleCv_.notify_one();
    cleanupDoneCv_.wait(lock, [this] {
        return !cleanupInProgress_ &&
               !cleanupRequested_.load(std::memory_order_acquire) &&
               !directUsbRenderThread_.joinable();
    });
    return true;
}

void AudioEngine::finishDirectUsbCleanup() {
    std::lock_guard<std::mutex> guard(lifecycleMutex_);
    if (cleanupInProgress_) return;
    cleanupInProgress_ = true;
    directUsbState_.store(DirectUsbState::Stopping, std::memory_order_release);
    directUsbSession_.store(false, std::memory_order_release);
    isRunning_.store(false, std::memory_order_release);
    if (directUsbOutput_) directUsbOutput_->requestStop();
    if (directUsbRenderThread_.joinable()) directUsbRenderThread_.join();
    if (directUsbOutput_) directUsbOutput_->stop();
    cleanupEngineState();
    publishedLatencyMs_.store(0.0, std::memory_order_release);
    publishedXRunCount_.store(0, std::memory_order_release);
    publishedCallbackFrameCount_.store(0, std::memory_order_release);
    directCaptureRingFrames_.store(0, std::memory_order_release);
    directPlaybackRingFrames_.store(0, std::memory_order_release);
    directQueuedOutFrames_.store(0, std::memory_order_release);
    directCaptureTransferFrames_.store(0, std::memory_order_release);
    directCaptureOverruns_.store(0, std::memory_order_release);
    directCaptureUnderruns_.store(0, std::memory_order_release);
    directPlaybackXruns_.store(0, std::memory_order_release);
    directUsbPerformanceHintActive_.store(false, std::memory_order_release);
    const int32_t failure = directUsbFailureCode_.load(std::memory_order_acquire);
    directUsbState_.store(failure ? DirectUsbState::Failed : DirectUsbState::Stopped,
                          std::memory_order_release);
    cleanupInProgress_ = false;
}

void AudioEngine::cleanupEngineState() {
    if (cleanupStarted_.exchange(true, std::memory_order_acq_rel)) return;
    if (recorder_.isRecording()) recorder_.stopRecording();
    rackGraph_.pauseAndResetTransport();
    rackGraph_.deactivate();
}

AudioEngine::DirectUsbRuntimeStats AudioEngine::getDirectUsbRuntimeStats() const noexcept {
    DirectUsbRuntimeStats out;
    out.state = directUsbState_.load(std::memory_order_acquire);
    out.sessionId = directUsbSessionId_.load(std::memory_order_acquire);
    out.failureCode = directUsbFailureCode_.load(std::memory_order_acquire);
    out.effectiveQuantum = directUsbEffectiveQuantum_.load(std::memory_order_acquire);
    out.requestedPeriodMultiplier = directUsbPeriodMultiplier_.load(std::memory_order_acquire);
    out.startupPrimeFrames = directUsbPrimeFrames_.load(std::memory_order_acquire);
    out.steadyTargetFrames = directUsbSteadyTargetFrames_.load(std::memory_order_acquire);
    out.lastDspNanoseconds = directUsbLastDspNs_.load(std::memory_order_relaxed);
    out.peakDspNanoseconds = directUsbPeakDspNs_.load(std::memory_order_relaxed);
    out.lastCycleNanoseconds = directUsbLastCycleNs_.load(std::memory_order_relaxed);
    out.peakCycleNanoseconds = directUsbPeakCycleNs_.load(std::memory_order_relaxed);
    out.deadlineBudgetNanoseconds = directUsbDeadlineBudgetNs_.load(std::memory_order_relaxed);
    out.deadlineMisses = directUsbDeadlineMisses_.load(std::memory_order_relaxed);
    out.captureWaitTimeouts = directUsbCaptureWaitTimeouts_.load(std::memory_order_relaxed);
    out.writeWaitTimeouts = directUsbWriteWaitTimeouts_.load(std::memory_order_relaxed);
    out.captureRingFrames = directCaptureRingFrames_.load(std::memory_order_acquire);
    out.playbackRingFrames = directPlaybackRingFrames_.load(std::memory_order_acquire);
    out.queuedOutFrames = directQueuedOutFrames_.load(std::memory_order_acquire);
    out.captureTransferFrames = directCaptureTransferFrames_.load(std::memory_order_acquire);
    out.captureOverruns = directCaptureOverruns_.load(std::memory_order_acquire);
    out.captureUnderruns = directCaptureUnderruns_.load(std::memory_order_acquire);
    out.playbackXruns = directPlaybackXruns_.load(std::memory_order_acquire);
    out.performanceHintActive =
        directUsbPerformanceHintActive_.load(std::memory_order_acquire);
    return out;
}

void AudioEngine::stop() {
    std::lock_guard<std::mutex> lifecycleCallLock(publicLifecycleMutex_);
    LOGI("stop() entered tid=%ld isRunning_=%d", getTid(),
         isRunning_ ? 1 : 0);
    const DirectUsbState usbState =
        directUsbState_.load(std::memory_order_acquire);
    const bool needsUsb = directUsbSession_.load(std::memory_order_acquire) ||
                          usbState == DirectUsbState::Starting ||
                          usbState == DirectUsbState::Running ||
                          usbState == DirectUsbState::Stopping;
    if (needsUsb) {
        directUsbState_.store(DirectUsbState::Stopping, std::memory_order_release);
        directUsbSession_.store(false, std::memory_order_release);
        isRunning_.store(false, std::memory_order_release);
        requestDirectUsbCleanup(0);
        waitForDirectUsbCleanup();
        directUsbFailureCode_.store(0, std::memory_order_release);
        directUsbState_.store(DirectUsbState::Stopped, std::memory_order_release);
        return;
    }
    isRunning_.store(false, std::memory_order_release);
    cleanupEngineState();
    directUsbState_.store(DirectUsbState::Stopped, std::memory_order_release);
}

bool AudioEngine::isRunning() const {
    return isRunning_;
}

double AudioEngine::getLatencyMs() const {
    return publishedLatencyMs_.load(std::memory_order_acquire);
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
    return publishedXRunCount_.load(std::memory_order_acquire);
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
        rackGraph_.advanceTransport(numFrames);
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
    const int32_t startupBlocks = directUsbStartupBlocks_;
    const auto period = std::chrono::duration<double>(
        static_cast<double>(frames) / static_cast<double>(sampleRate_));
    const int writeTimeoutMs = std::max(
        2, static_cast<int>(std::ceil(period.count() * 2000.0)));
    const int captureTimeoutMs = std::min(
        1000, std::max(20, static_cast<int>(std::ceil(period.count() * 4000.0))));
    const int64_t targetWorkDurationNs = std::max<int64_t>(
        1, std::chrono::duration_cast<std::chrono::nanoseconds>(period).count());
    PerformanceHintSession performanceHint(targetWorkDurationNs);
    directUsbPerformanceHintActive_.store(
        performanceHint.active(), std::memory_order_release);
    int captureMissStreak = 0;
    int failureCode = 0;

    const auto canContinue = [this]() noexcept {
        return directUsbSession_.load(std::memory_order_acquire) &&
            directUsbOutput_ && directUsbOutput_->isStreaming();
    };
    const auto waitForCapture = [this, &canContinue, &captureMissStreak,
                                 &failureCode, captureTimeoutMs](int32_t required) noexcept {
        if (directUsbOutput_ &&
            directUsbOutput_->waitForCaptureFrames(required, captureTimeoutMs)) {
            captureMissStreak = 0;
            return true;
        }
        if (canContinue()) {
            directUsbCaptureWaitTimeouts_.fetch_add(1, std::memory_order_relaxed);
            if (++captureMissStreak >= 3) {
                failureCode = usbFailureCode(
                    monotrypt::usb::StartError::TransportStoppedUnexpectedly);
            }
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
        const uint64_t dspNs = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count());
        directUsbLastDspNs_.store(dspNs, std::memory_order_relaxed);
        const auto transport = directUsbOutput_->transportStats();
        directCaptureRingFrames_.store(static_cast<uint32_t>(std::min<uint64_t>(
            transport.captureRingFrames, std::numeric_limits<uint32_t>::max())), std::memory_order_relaxed);
        directPlaybackRingFrames_.store(static_cast<uint32_t>(std::min<uint64_t>(
            transport.ringFrames, std::numeric_limits<uint32_t>::max())), std::memory_order_relaxed);
        directQueuedOutFrames_.store(static_cast<uint32_t>(std::min<uint64_t>(
            directUsbOutput_->queuedOutFrames(), std::numeric_limits<uint32_t>::max())), std::memory_order_relaxed);
        directCaptureTransferFrames_.store(static_cast<uint32_t>(
            std::max(0, directUsbOutput_->captureTransferFrames())), std::memory_order_relaxed);
        const auto captures = directUsbOutput_->captureStats();
        directCaptureOverruns_.store(captures.overruns, std::memory_order_relaxed);
        directCaptureUnderruns_.store(captures.underruns, std::memory_order_relaxed);
        directPlaybackXruns_.store(directUsbOutput_->xrunCount(), std::memory_order_relaxed);
        const uint64_t latencyFrames = std::max<uint64_t>(
            directUsbEffectiveQuantum_.load(std::memory_order_relaxed),
            std::max<uint64_t>(transport.captureRingFrames,
                               directCaptureTransferFrames_.load(std::memory_order_relaxed)))
            + transport.ringFrames + directUsbOutput_->queuedOutFrames();
        publishedLatencyMs_.store(
            static_cast<double>(latencyFrames) / publishedSampleRate_.load(std::memory_order_relaxed) * 1000.0,
            std::memory_order_relaxed);
        const uint64_t totalXruns = directUsbOutput_->xrunCount() +
                                    directUsbOutput_->captureXRunCount();
        publishedXRunCount_.store(static_cast<int32_t>(std::min<uint64_t>(
            totalXruns, static_cast<uint64_t>(std::numeric_limits<int32_t>::max()))),
            std::memory_order_relaxed);
        uint64_t peak = directUsbPeakDspNs_.load(std::memory_order_relaxed);
        while (peak < dspNs &&
               !directUsbPeakDspNs_.compare_exchange_weak(
                   peak, dspNs, std::memory_order_relaxed)) {}
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
        const float peakDecay = meterDecayForBlock(frames, sampleRate_);
        inputPeakHold_ = std::max(inputPeak, inputPeakHold_ * peakDecay);
        outputPeakHold_ = std::max(outputPeak, outputPeakHold_ * peakDecay);
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
        int misses = 0;
        constexpr int kMaxMisses = 8;
        while (submittedFrames < frames && canContinue()) {
            const int32_t remainingFrames = frames - submittedFrames;
            if (!directUsbOutput_->waitForWritableFrames(
                    remainingFrames, writeTimeoutMs)) {
                directUsbWriteWaitTimeouts_.fetch_add(1, std::memory_order_relaxed);
                if (++misses >= kMaxMisses) return false;
                continue;
            }
            const int written = directUsbOutput_->writeStereo(
                left + submittedFrames, right + submittedFrames, remainingFrames);
            if (written <= 0) {
                directUsbWriteWaitTimeouts_.fetch_add(1, std::memory_order_relaxed);
                if (++misses >= kMaxMisses) return false;
                continue;
            }
            submittedFrames += written;
            misses = 0;
        }
        return submittedFrames == frames;
    };
    bool outputPrimed = false;
    while (directUsbSession_.load(std::memory_order_acquire)) {
        if (!outputPrimed) {
            bool startupOk = true;
            for (int32_t block = 0; block < startupBlocks; ++block) {
                if (!waitForCapture(frames)) {
                    if (failureCode != 0 || !canContinue()) {
                        startupOk = false;
                        break;
                    }
                    --block;
                    continue;
                }
                renderBlock();
                std::memcpy(directUsbStartupLeft_.data() +
                                static_cast<size_t>(block) * frames,
                            directUsbOutputLeft_.data(),
                            static_cast<size_t>(frames) * sizeof(float));
                std::memcpy(directUsbStartupRight_.data() +
                                static_cast<size_t>(block) * frames,
                            directUsbOutputRight_.data(),
                            static_cast<size_t>(frames) * sizeof(float));
            }
            for (int32_t block = 0; block < startupBlocks; ++block) {
                if (!submitBlock(directUsbStartupLeft_.data() +
                                     static_cast<size_t>(block) * frames,
                                 directUsbStartupRight_.data() +
                                     static_cast<size_t>(block) * frames)) {
                    if (directUsbSession_.load(std::memory_order_acquire)) {
                        failureCode = usbFailureCode(
                            monotrypt::usb::StartError::TransportStoppedUnexpectedly);
                    }
                    startupOk = false;
                    break;
                }
            }
            if (!startupOk) break;
            if (!directUsbOutput_->startPlayback()) {
                const int32_t driverFailure = directUsbOutput_->lastErrorCode();
                failureCode =
                    driverFailure != usbFailureCode(monotrypt::usb::StartError::Ok)
                        ? driverFailure
                        : usbFailureCode(
                              monotrypt::usb::StartError::IsoPumpSubmitFailed);
                const std::string detail = directUsbOutput_->lastErrorDetail();
                LOGE("Direct USB playback arm failed: %s", detail.c_str());
                break;
            }
            outputPrimed = true;
            isRunning_.store(true, std::memory_order_release);
            directUsbState_.store(DirectUsbState::Running, std::memory_order_release);
            continue;
        }
        const auto cycleBegan = std::chrono::steady_clock::now();
        const auto transportBeforeWait = directUsbOutput_->transportStats();
        const uint64_t queuedUsbFrames = directUsbOutput_->queuedOutFrames();
        const uint64_t playbackFrames =
            transportBeforeWait.ringFrames >
                    std::numeric_limits<uint64_t>::max() - queuedUsbFrames
                ? std::numeric_limits<uint64_t>::max()
                : transportBeforeWait.ringFrames + queuedUsbFrames;
        const uint32_t cycleSampleRate = static_cast<uint32_t>(
            std::max(0.0f, publishedSampleRate_.load(std::memory_order_relaxed)));
        const uint64_t cycleDeadlineBudgetNs =
            monotrypt::usb::playbackRunwayNanoseconds(
                playbackFrames, cycleSampleRate);
        directUsbDeadlineBudgetNs_.store(
            cycleDeadlineBudgetNs, std::memory_order_relaxed);
        if (!waitForCapture(frames)) {
            const uint64_t elapsedNs = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - cycleBegan).count());
            if (cycleDeadlineBudgetNs != 0 &&
                elapsedNs > cycleDeadlineBudgetNs) {
                directUsbDeadlineMisses_.fetch_add(1, std::memory_order_relaxed);
            }
            if (failureCode != 0 || !canContinue()) break;
            continue;
        }
        renderBlock();
        // A full playback ring can make submitBlock wait safely for capacity.
        // Deadline risk ends once the next block is rendered; do not classify
        // intentional producer backpressure as an audio deadline miss.
        const uint64_t renderReadyNs = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - cycleBegan).count());
        const bool submitted = submitBlock(
            directUsbOutputLeft_.data(), directUsbOutputRight_.data());
        const uint64_t cycleNs = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - cycleBegan).count());
        performanceHint.reportActualWorkDuration(cycleNs);
        if (submitted) {
            if (directUsbState_.load(std::memory_order_acquire) == DirectUsbState::Running) {
                uint64_t peak = directUsbPeakCycleNs_.load(std::memory_order_relaxed);
                while (peak < cycleNs && !directUsbPeakCycleNs_.compare_exchange_weak(
                    peak, cycleNs, std::memory_order_relaxed)) {}
                directUsbLastCycleNs_.store(cycleNs, std::memory_order_release);
                if (cycleDeadlineBudgetNs != 0 &&
                    renderReadyNs > cycleDeadlineBudgetNs) {
                    directUsbDeadlineMisses_.fetch_add(1, std::memory_order_relaxed);
                }
            }
            continue;
        }
        if (cycleDeadlineBudgetNs != 0 &&
            renderReadyNs > cycleDeadlineBudgetNs) {
            directUsbDeadlineMisses_.fetch_add(1, std::memory_order_relaxed);
        }
        if (directUsbSession_.load(std::memory_order_acquire)) {
            failureCode = usbFailureCode(
                monotrypt::usb::StartError::TransportStoppedUnexpectedly);
        }
        break;
    }
    directUsbPerformanceHintActive_.store(false, std::memory_order_release);
    const bool sessionWasActive =
        directUsbSession_.exchange(false, std::memory_order_acq_rel);
    if (failureCode == 0 && sessionWasActive) {
        failureCode = usbFailureCode(
            monotrypt::usb::StartError::TransportStoppedUnexpectedly);
    }
    isRunning_.store(false, std::memory_order_release);
    requestDirectUsbCleanup(failureCode);
}


} // namespace guitarrackcraft
