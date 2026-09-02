/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * This file is part of NNAGA.
 *
 * NNAGA is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * NNAGA is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with NNAGA. If not, see <https://www.gnu.org/licenses/>.
 */

#include "AudioEngine.h"
#include "RoundTripCorrelation.h"
#include "utils/WavIO.h"
#include <liblowlatencyaudio/ThreadUtils.h>
#include <android/log.h>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <unistd.h>
#include <thread>
#include <limits>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <pthread.h>
#if defined(__aarch64__)
#include <arm_neon.h>
#endif

#define LOG_TAG "AudioEngine"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace guitarrackcraft {
namespace {
constexpr int32_t usbFailureCode(monotrypt::usb::StartError error) noexcept {
    return static_cast<int32_t>(error);
}
constexpr float kMeterReleaseSeconds = 0.15f;

void measureStereoPeaks(
        const float* __restrict input,
        const float* __restrict outputLeft,
        const float* __restrict outputRight,
        int32_t frames, float& inputPeak, float& outputPeak) noexcept {
    int32_t frame = 0;
#if defined(__aarch64__)
    float32x4_t inputMaximum = vdupq_n_f32(0.0f);
    float32x4_t outputMaximum = vdupq_n_f32(0.0f);
    for (; frame + 4 <= frames; frame += 4) {
        inputMaximum = vmaxnmq_f32(
            inputMaximum, vabsq_f32(vld1q_f32(input + frame)));
        outputMaximum = vmaxnmq_f32(
            outputMaximum,
            vmaxnmq_f32(vabsq_f32(vld1q_f32(outputLeft + frame)),
                        vabsq_f32(vld1q_f32(outputRight + frame))));
    }
    inputPeak = vmaxnmvq_f32(inputMaximum);
    outputPeak = vmaxnmvq_f32(outputMaximum);
#else
    inputPeak = 0.0f;
    outputPeak = 0.0f;
#endif
    for (; frame < frames; ++frame) {
        inputPeak = std::max(inputPeak, std::fabs(input[frame]));
        outputPeak = std::max(
            outputPeak,
            std::max(std::fabs(outputLeft[frame]), std::fabs(outputRight[frame])));
    }
}

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
    , androidOboeBackend_(std::make_unique<AndroidOboeBackend>(rackGraph_))
{
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

void AudioEngine::stopDirectUsbThermalPolicy() noexcept {
    directUsbThermalPolicyStop_.store(true, std::memory_order_release);
    directUsbThermalPolicyCv_.notify_one();
    if (directUsbThermalPolicyThread_.joinable()) {
        directUsbThermalPolicyThread_.join();
    }
    directUsbThermalSafetyActive_ = false;
}

void AudioEngine::directUsbThermalPolicyLoop() {
    (void)pthread_setname_np(pthread_self(), "UsbThermalPolicy");
#if defined(__ANDROID__) || defined(__linux__)
    (void)setpriority(PRIO_PROCESS,
                      static_cast<id_t>(syscall(SYS_gettid)), 5);
#endif
    int32_t initialRenderTid = 0;
    int32_t initialEventTid = 0;
    {
        std::unique_lock<std::mutex> lock(directUsbThermalPolicyMutex_);
        while (!directUsbThermalPolicyStop_.load(std::memory_order_acquire)) {
            initialRenderTid =
                directUsbRenderTid_.load(std::memory_order_acquire);
            initialEventTid =
                directUsbOutput_ ? directUsbOutput_->eventThreadTid() : 0;
            if (initialRenderTid > 0 && initialEventTid > 0 &&
                initialEventTid != initialRenderTid) {
                break;
            }
            directUsbThermalPolicyCv_.wait_for(
                lock, std::chrono::milliseconds(1));
        }
    }
    if (directUsbThermalPolicyStop_.load(std::memory_order_acquire))
        return;
    const int32_t initialThreadIds[] = {
        initialRenderTid, initialEventTid
    };
    constexpr size_t initialThreadCount = 2;
    const int64_t targetWorkDurationNs = std::max<int64_t>(
        1, static_cast<int64_t>(
            1'000'000'000.0 * static_cast<double>(callbackFrameCount_) /
            static_cast<double>(sampleRate_)));
    PerformanceHintSession performanceHint(
        targetWorkDurationNs, initialThreadIds, initialThreadCount);
    ThermalHeadroomMonitor monitor;
    bool safetyActive = false;
    int32_t configuredRenderTid = initialRenderTid;
    int32_t configuredEventTid = initialEventTid;


    directUsbPerformanceHintActive_.store(
        performanceHint.active(), std::memory_order_release);
    directUsbPerformanceHintSession_.store(
        performanceHint.active()
            ? static_cast<void*>(&performanceHint)
            : nullptr,
        std::memory_order_release);

    for (;;) {
        std::unique_lock<std::mutex> lock(directUsbThermalPolicyMutex_);
        if (directUsbThermalPolicyCv_.wait_for(
                lock, std::chrono::seconds(1), [this] {
                    return directUsbThermalPolicyStop_.load(std::memory_order_acquire);
                })) {
            break;
        }
        lock.unlock();
        if (!directUsbSession_.load(std::memory_order_acquire)) continue;
        const float headroom = monitor.sample(5);
        if (!directUsbThermalSafetyEnabled_.load(std::memory_order_acquire)) {
            if (safetyActive && directUsbOutput_) {
                directUsbOutput_->setGraphQuantum(
                    directUsbEffectiveQuantum_.load(std::memory_order_acquire),
                    directUsbConfiguredMultiplier_,
                    directUsbConfiguredWatermarkFrames_);
                directUsbSteadyTargetFrames_.store(
                    static_cast<uint32_t>(std::max(0, directUsbOutput_->playbackTargetFrames())),
                    std::memory_order_release);
            }
            safetyActive = false;
            directUsbThermalSafetyActive_ = false;
            continue;
        }
        if (!std::isfinite(headroom)) continue;
        if (!safetyActive && headroom >= 0.85f) {
            const int32_t quantum = static_cast<int32_t>(directUsbEffectiveQuantum_.load(std::memory_order_acquire));
            const int32_t currentTarget = static_cast<int32_t>(directUsbSteadyTargetFrames_.load(std::memory_order_acquire));
            if (quantum > 0 && currentTarget > 0 && directUsbOutput_) {
                directUsbOutput_->setGraphQuantum(quantum, directUsbConfiguredMultiplier_, currentTarget + 2 * quantum);
                directUsbSteadyTargetFrames_.store(static_cast<uint32_t>(std::max(0, directUsbOutput_->playbackTargetFrames())), std::memory_order_release);
                safetyActive = true;
                directUsbThermalSafetyActive_ = true;
            }
        } else if (safetyActive && headroom <= 0.65f) {
            const int32_t quantum = static_cast<int32_t>(directUsbEffectiveQuantum_.load(std::memory_order_acquire));
            if (quantum > 0 && directUsbOutput_) {
                directUsbOutput_->setGraphQuantum(quantum, directUsbConfiguredMultiplier_, directUsbConfiguredWatermarkFrames_);
                directUsbSteadyTargetFrames_.store(static_cast<uint32_t>(std::max(0, directUsbOutput_->playbackTargetFrames())), std::memory_order_release);
            }
            safetyActive = false;
            directUsbThermalSafetyActive_ = false;
        }
    }
    directUsbThermalSafetyActive_.store(false, std::memory_order_release);
    directUsbPerformanceHintSession_.store(nullptr, std::memory_order_release);
    directUsbPerformanceHintActive_.store(false, std::memory_order_release);
}


bool AudioEngine::startDirectUsbSession(
        float sampleRate, int32_t bitsPerSample, int32_t subslotBytes,
        int32_t channels, int32_t outputPair, int32_t bufferFrames,
        int32_t periodMultiplier,
        const monotrypt::usb::UserspaceBufferConfig& bufferConfig,
        bool thermalSafetyEnabled) {
    directUsbThermalSafetyEnabled_.store(thermalSafetyEnabled, std::memory_order_release);
    std::lock_guard<std::mutex> lifecycleCallLock(publicLifecycleMutex_);
    // An Oboe route error is reported asynchronously. Reconcile that failed
    // session here, before Direct USB examines the shared running flag, so a
    // stale Oboe identity cannot mask the USB backend or hijack stop().
    if (androidOboeSession_.load(std::memory_order_acquire) &&
        androidOboeBackend_ &&
        (!androidOboeBackend_->isRunning() || androidOboeBackend_->hasError())) {
        isRunning_.store(false, std::memory_order_release);
        androidOboeBackend_->stop();
        androidOboeSession_.store(false, std::memory_order_release);
        cleanupEngineState();
        publishedCallbackFrameCount_.store(0, std::memory_order_release);
    }
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
        outputPair < 0 || outputPair * 2 + 1 >= channels ||
        bufferConfig.playbackTargetFrames < 0 ||
        bufferConfig.startupPrimeFrames < 0 ||
        bufferConfig.writeHeadroomFrames < 0 ||
        bufferConfig.captureLimitFrames < 0 ||
        bufferConfig.captureTargetFrames < 0 ||
        bufferConfig.captureHeadroomFrames < 0 ||
        bufferConfig.captureDeadlineSlackFrames < 0) {
        directUsbFailureCode_.store(
            usbFailureCode(monotrypt::usb::StartError::Unknown),
            std::memory_order_release);
        directUsbState_.store(DirectUsbState::Failed, std::memory_order_release);
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
    directUsbFailureRequest_.store(0, std::memory_order_relaxed);

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
    directUsbSchedulerDeadlineMisses_.store(0, std::memory_order_relaxed);
    directUsbMaxSchedulerLatenessNs_.store(0, std::memory_order_relaxed);
    if (!directUsbOutput_->configureUserspaceBuffers(bufferConfig)) {
        directUsbFailureCode_.store(
            usbFailureCode(monotrypt::usb::StartError::Unknown),
            std::memory_order_release);
        directUsbState_.store(DirectUsbState::Failed, std::memory_order_release);
        return false;
    }
    if (!directUsbOutput_->start(static_cast<int>(sampleRate), bitsPerSample,
                                 subslotBytes, channels, outputPair)) {
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
    directUsbOutput_->setUserspaceBufferConfig(
        renderFrames, bufferConfig, multiplier);
    if (directUsbOutput_->playbackTargetFrames() <= 0 ||
        directUsbOutput_->startupPrimeFrames() <= 0) {
        directUsbOutput_->requestStop();
        directUsbOutput_->stop();
        directUsbFailureCode_.store(
            usbFailureCode(monotrypt::usb::StartError::Unknown),
            std::memory_order_release);
        directUsbState_.store(DirectUsbState::Failed, std::memory_order_release);
        return false;
    }
    const int32_t primeFrames = std::max(0, directUsbOutput_->startupPrimeFrames());
    const int32_t startupBlocks =
        (primeFrames + renderFrames - 1) / renderFrames;
    const int32_t captureTransferFrames =
        std::max(1, directUsbOutput_->captureTransferFrames());
    directCaptureTransferFrames_.store(
        static_cast<uint32_t>(captureTransferFrames), std::memory_order_release);
    directUsbPrimeFrames_.store(static_cast<uint32_t>(primeFrames), std::memory_order_release);
    directUsbSteadyTargetFrames_.store(
        static_cast<uint32_t>(std::max(0, directUsbOutput_->playbackTargetFrames())),
        std::memory_order_release);
    directUsbConfiguredWatermarkFrames_ = bufferConfig.playbackTargetFrames;
    directUsbConfiguredMultiplier_ = multiplier;
    directUsbThermalSafetyActive_ = false;
    directUsbThermalPolicyStop_.store(false, std::memory_order_release);
    directUsbInputChannelCount_ = std::clamp(
        directUsbOutput_->captureChannelCount(), 0,
        DirectUsbOutput::kMaxDeviceChannels);
    if (directUsbInputChannelCount_ <= 0) {
        directUsbOutput_->requestStop();
        directUsbFailureCode_.store(
            usbFailureCode(monotrypt::usb::StartError::Unknown),
            std::memory_order_release);
        directUsbState_.store(DirectUsbState::Failed, std::memory_order_release);
        directUsbOutput_->stop();
        return false;
    }
    try {
        directUsbInputBuffer_.assign(
            static_cast<size_t>(directUsbInputChannelCount_) * renderFrames, 0.0f);
        directUsbInputPlanes_.resize(static_cast<size_t>(directUsbInputChannelCount_));
        for (int32_t channel = 0; channel < directUsbInputChannelCount_; ++channel) {
            directUsbInputPlanes_[static_cast<size_t>(channel)] =
                directUsbInputBuffer_.data() + static_cast<size_t>(channel) * renderFrames;
        }
        directUsbOutputLeft_.assign(static_cast<size_t>(renderFrames), 0.0f);
        directUsbOutputRight_.assign(static_cast<size_t>(renderFrames), 0.0f);
    } catch (const std::bad_alloc&) {
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
    directUsbOutputPair_.store(outputPair, std::memory_order_release);
    directUsbStartupBlocks_ = startupBlocks;
    directUsbCaptureWaitTimeouts_.store(0, std::memory_order_relaxed);
    directUsbWriteWaitTimeouts_.store(0, std::memory_order_relaxed);
    rackGraph_.setSampleRate(sampleRate_, callbackFrameCount_);
    rackGraph_.setAvailableInputChannelCount(directUsbInputChannelCount_);
    rackGraph_.activate();
    rackGraph_.pauseAndResetTransport();
    cleanupStarted_.store(false, std::memory_order_release);
    directUsbRenderUrgentAudio_.store(false, std::memory_order_relaxed);
    directUsbPlaybackQuantumDrops_.store(0, std::memory_order_relaxed);
    for (int32_t block = 0; block < startupBlocks; ++block) {
        if (!directUsbOutput_->submitWholeQuantum(
                directUsbOutputLeft_.data(), directUsbOutputRight_.data(), renderFrames)) {
            directUsbOutput_->requestStop();
            directUsbOutput_->stop();
            directUsbFailureCode_.store(
                usbFailureCode(monotrypt::usb::StartError::IsoPumpSubmitFailed),
                std::memory_order_release);
            directUsbState_.store(DirectUsbState::Failed, std::memory_order_release);
            cleanupEngineState();
            return false;
        }
    }
    const int32_t captureCapacityFrames =
        std::max(renderFrames, directUsbOutput_->captureCapacityFrames());
    const int32_t captureTargetFrames =
        std::max(0, directUsbOutput_->captureTargetFrames());
    const int32_t captureHeadroomFrames =
        std::max(0, directUsbOutput_->captureHeadroomFrames());
    if (captureTargetFrames >
        captureCapacityFrames - renderFrames - captureHeadroomFrames) {
        directUsbOutput_->requestStop();
        directUsbOutput_->stop();
        directUsbFailureCode_.store(
            usbFailureCode(monotrypt::usb::StartError::Unknown),
            std::memory_order_release);
        directUsbState_.store(DirectUsbState::Failed, std::memory_order_release);
        cleanupEngineState();
        return false;
    }
    const int32_t capturePrimeFrames = renderFrames + captureTargetFrames;
    const auto capturePrimeDeadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
    if (!directUsbOutput_->waitForCaptureUntil(
            capturePrimeFrames, capturePrimeDeadline)) {
        directUsbOutput_->requestStop();
        directUsbOutput_->stop();
        directUsbFailureCode_.store(
            usbFailureCode(
                monotrypt::usb::StartError::TransportStoppedUnexpectedly),
            std::memory_order_release);
        directUsbState_.store(DirectUsbState::Failed, std::memory_order_release);
        cleanupEngineState();
        return false;
    }
    if (!directUsbOutput_->startPlayback()) {
        directUsbOutput_->requestStop();
        directUsbOutput_->stop();
        directUsbFailureCode_.store(
            usbFailureCode(monotrypt::usb::StartError::IsoPumpSubmitFailed),
            std::memory_order_release);
        directUsbState_.store(DirectUsbState::Failed, std::memory_order_release);
        cleanupEngineState();
        return false;
    }
    directUsbOutput_->resetRealtimeCounters();
    directUsbSession_.store(true, std::memory_order_release);
    isRunning_.store(true, std::memory_order_release);
    directUsbState_.store(DirectUsbState::Running, std::memory_order_release);
    try {
        directUsbThermalPolicyThread_ =
            std::thread(&AudioEngine::directUsbThermalPolicyLoop, this);
    } catch (...) {
        LOGE("Direct USB thermal/ADPF policy thread unavailable");
        directUsbThermalPolicyStop_.store(true, std::memory_order_release);
    }
    try {
        directUsbRenderThread_ = std::thread(&AudioEngine::directUsbRenderLoop, this);
    } catch (...) {
        directUsbSession_.store(false, std::memory_order_release);
        isRunning_.store(false, std::memory_order_release);
        stopDirectUsbThermalPolicy();
        directUsbOutput_->requestStop();
        directUsbFailureCode_.store(
            usbFailureCode(monotrypt::usb::StartError::Unknown),
            std::memory_order_release);
        directUsbState_.store(DirectUsbState::Failed, std::memory_order_release);
        directUsbOutput_->stop();
        cleanupEngineState();
        return false;
    }
    directUsbThermalPolicyCv_.notify_one();
    return true;
}

bool AudioEngine::openDirectUsbDevice(int fd, int driverCode) {
    std::lock_guard<std::mutex> lifecycleCallLock(publicLifecycleMutex_);
    waitForDirectUsbCleanup();
    if (fd < 0 || directUsbSession_.load(std::memory_order_acquire) ||
        isRunning_.load(std::memory_order_acquire) ||
        directUsbState_.load(std::memory_order_acquire) != DirectUsbState::Stopped) return false;
    return directUsbOutput_ && directUsbOutput_->open(fd, driverCode);
}

void AudioEngine::closeDirectUsbDevice() {
    std::lock_guard<std::mutex> lifecycleCallLock(publicLifecycleMutex_);
    waitForDirectUsbCleanup();
    if (directUsbSession_.load(std::memory_order_acquire) ||
        isRunning_.load(std::memory_order_acquire)) return;
    if (directUsbOutput_) directUsbOutput_->close();
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
    if (failureCode != 0) {
        directUsbFailureRequest_.store(failureCode, std::memory_order_release);
        directUsbFailureCode_.store(failureCode, std::memory_order_release);
    }
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
    stopDirectUsbThermalPolicy();
    if (directUsbOutput_) directUsbOutput_->stop();
    cleanupEngineState();
    publishedLatencyMs_.store(0.0, std::memory_order_release);
    publishedXRunCount_.store(0, std::memory_order_release);
    publishedCallbackFrameCount_.store(0, std::memory_order_release);
    directCaptureTransferFrames_.store(0, std::memory_order_release);
    directUsbPerformanceHintActive_.store(false, std::memory_order_release);
    const int32_t failure = directUsbFailureCode_.load(std::memory_order_acquire);
    directUsbState_.store(failure ? DirectUsbState::Failed : DirectUsbState::Stopped,
                          std::memory_order_release);
    cleanupInProgress_ = false;
}

void AudioEngine::cleanupEngineState() {
    rackGraph_.setAvailableInputChannelCount(0);
    if (cleanupStarted_.exchange(true, std::memory_order_acq_rel)) return;
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
    out.captureTransferFrames = directCaptureTransferFrames_.load(
        std::memory_order_acquire);
    if (directUsbOutput_) {
        out.captureRingFrames = static_cast<uint32_t>(
            std::max(0, directUsbOutput_->captureAvailableFrames()));
        out.playbackRingFrames = static_cast<uint32_t>(
            std::max(0, directUsbOutput_->bufferedFrames()));
        out.queuedOutFrames = static_cast<uint32_t>(
            std::min<uint64_t>(
                std::numeric_limits<uint32_t>::max(),
                directUsbOutput_->queuedOutFrames()));
        out.captureTargetFrames = static_cast<uint32_t>(
            std::max(0, directUsbOutput_->captureTargetFrames()));
        out.captureHeadroomFrames = static_cast<uint32_t>(
            std::max(0, directUsbOutput_->captureHeadroomFrames()));
        out.captureDeadlineSlackFrames = static_cast<uint32_t>(
            std::max(0, directUsbOutput_->captureDeadlineSlackFrames()));
        const auto capture = directUsbOutput_->captureStats();
        out.captureOverruns = capture.overruns;
        out.captureUnderruns = capture.underruns;
        out.capturePacketDrops = directUsbOutput_->capturePacketDropCount();
        out.playbackXruns = directUsbOutput_->xrunCount();
        out.playbackQuantumDrops = directUsbOutput_->playbackQuantumDrops();
    }
    out.schedulerDeadlineMisses =
        directUsbSchedulerDeadlineMisses_.load(std::memory_order_relaxed);
    out.maxSchedulerLatenessNanoseconds =
        directUsbMaxSchedulerLatenessNs_.load(std::memory_order_relaxed);
    out.performanceHintActive =
        directUsbPerformanceHintActive_.load(std::memory_order_acquire);
    out.thermalSafetyEnabled =
        directUsbThermalSafetyEnabled_.load(std::memory_order_acquire);
    out.thermalSafetyActive =
        directUsbThermalSafetyActive_.load(std::memory_order_acquire);
    return out;
}

bool AudioEngine::startAndroidOboeSession(int32_t inputDeviceId, int32_t outputDeviceId, int32_t bufferFrames) {
    std::lock_guard<std::mutex> lock(publicLifecycleMutex_);
    if (!androidOboeBackend_) androidOboeBackend_ = std::make_unique<AndroidOboeBackend>(rackGraph_);
    const bool started = androidOboeBackend_->start(
        static_cast<int32_t>(sampleRate_), inputDeviceId, outputDeviceId, bufferFrames);
    if (!started) {
        // start() stops any prior streams before reporting failure. Do not
        // leave the old session identity visible after that transition.
        androidOboeSession_.store(false, std::memory_order_release);
        isRunning_.store(false, std::memory_order_release);
        cleanupEngineState();
        publishedCallbackFrameCount_.store(0, std::memory_order_release);
        return false;
    }
    sampleRate_ = static_cast<float>(androidOboeBackend_->actualSampleRate());
    callbackFrameCount_ = static_cast<uint32_t>(
        androidOboeBackend_->actualFramesPerDataCallback());
    publishedSampleRate_.store(sampleRate_, std::memory_order_release);
    publishedCallbackFrameCount_.store(callbackFrameCount_, std::memory_order_release);
    rackGraph_.setAvailableInputChannelCount(androidOboeBackend_->inputChannelCount());
    cleanupStarted_.store(false, std::memory_order_release);
    androidOboeSession_.store(true, std::memory_order_release);
    isRunning_.store(true, std::memory_order_release);
    return true;
}
void AudioEngine::stop() {
    std::lock_guard<std::mutex> lifecycleCallLock(publicLifecycleMutex_);
    if (androidOboeSession_.load(std::memory_order_acquire)) {
        // Oboe closes streams only after callbacks have quiesced. Keep all
        // graph/plugin teardown on this lifecycle thread, never in Oboe's
        // error callback.
        isRunning_.store(false, std::memory_order_release);
        if (androidOboeBackend_) androidOboeBackend_->stop();
        androidOboeSession_.store(false, std::memory_order_release);
        cleanupEngineState();
        publishedCallbackFrameCount_.store(0, std::memory_order_release);
        return;
    }
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
    // Error callbacks only publish atomic state. Lifecycle reconciliation is
    // performed by start/stop under publicLifecycleMutex_.
    if (androidOboeSession_.load(std::memory_order_acquire) &&
        androidOboeBackend_ &&
        (!androidOboeBackend_->isRunning() || androidOboeBackend_->hasError())) {
        return false;
    }
    return isRunning_.load(std::memory_order_acquire);
}

bool AudioEngine::hasError() const {
    return androidOboeBackend_ && androidOboeBackend_->hasError();
}

double AudioEngine::getLatencyMs() const {
    if (directUsbSession_.load(std::memory_order_acquire) && directUsbOutput_) {
        const auto stats = getDirectUsbRuntimeStats();
        const uint64_t frames =
            static_cast<uint64_t>(stats.captureRingFrames) +
            static_cast<uint64_t>(stats.playbackRingFrames) +
            static_cast<uint64_t>(stats.queuedOutFrames);
        const float rate = publishedSampleRate_.load(std::memory_order_acquire);
        return rate > 0.0f
            ? static_cast<double>(frames) * 1000.0 / rate
            : 0.0;
    }
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
    if (directUsbSession_.load(std::memory_order_acquire) && directUsbOutput_) {
        const auto stats = getDirectUsbRuntimeStats();
        const uint64_t total =
            stats.captureOverruns + stats.captureUnderruns +
            stats.capturePacketDrops + stats.playbackXruns +
            stats.playbackQuantumDrops;
        return static_cast<int32_t>(std::min<uint64_t>(
            total, static_cast<uint64_t>(
                std::numeric_limits<int32_t>::max())));
    }
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
bool AudioEngine::loadTrackClipWav(RackPathId id,uint32_t slot,const std::string& path,const std::string& name,double sourceBpm){try{if(!std::isfinite(sourceBpm)||sourceBpm<20.0||sourceBpm>400.0)return false;std::vector<float> s;uint32_t r=0,c=0;if(!readWavFile(path,s,r,c)||s.empty()||r==0||(c!=1&&c!=2)||s.size()%c)return false;auto clip=std::make_shared<WavClip>();clip->sampleRate=r;clip->sourceBpm=sourceBpm;clip->displayName=name;size_t f=s.size()/c;clip->left.resize(f);if(c==1)clip->left=std::move(s);else{clip->right.resize(f);for(size_t i=0;i<f;++i){clip->left[i]=s[i*2];clip->right[i]=s[i*2+1];}}return rackGraph_.attachTrackWavSlot(id,slot,std::move(clip));}catch(...){return false;}}
bool AudioEngine::unloadTrackClipWav(RackPathId id,uint32_t slot){return rackGraph_.unloadTrackWavSlot(id,slot);}
bool AudioEngine::unloadTrackClipMidi(RackPathId id,uint32_t slot){return rackGraph_.unloadTrackMidiSlot(id,slot);}
bool AudioEngine::selectTrackClipSlot(RackPathId id,uint32_t slot){return rackGraph_.selectTrackClipSlot(id,slot);}

bool AudioEngine::unloadTrackWav(RackPathId trackId) {
    return rackGraph_.unloadTrackWav(trackId);
}

void AudioEngine::processRackBlock(const float* const* liveInputs,
                                   int32_t inputChannelCount,
                                   float* const* outputs,
                                   uint32_t numFrames) noexcept {
    realtimeCallbackCount_.fetch_add(1, std::memory_order_relaxed);
    realtimeCallbackFrames_.fetch_add(numFrames, std::memory_order_relaxed);
    rackGraph_.process(liveInputs, inputChannelCount, outputs, numFrames);
}
AudioEngine::RealtimeStatsSnapshot AudioEngine::getRealtimeStatsSnapshot() const noexcept {
    RealtimeStatsSnapshot out;
    if (androidOboeSession_.load(std::memory_order_acquire) && androidOboeBackend_) {
        out.callbackCount = androidOboeBackend_->callbackCount();
        out.callbackFrames = androidOboeBackend_->callbackFrames();
        out.frameCapacityViolations = androidOboeBackend_->capacityViolationCount();
        out.inputOverflowFrames = androidOboeBackend_->inputOverflowCount();
        out.inputUnderflowFrames = androidOboeBackend_->inputUnderflowCount();
        out.audioApi = androidOboeBackend_->actualAudioApi();
        out.sampleRateHz = androidOboeBackend_->actualSampleRate();
        out.framesPerBurst = androidOboeBackend_->actualFramesPerBurst();
        out.bufferSize = androidOboeBackend_->actualBufferSize();
        out.performanceMode = androidOboeBackend_->actualPerformanceMode();
        out.sharingMode = androidOboeBackend_->actualSharingMode();
        out.callbackFramesPerBurst = androidOboeBackend_->actualFramesPerDataCallback();
        out.activatedCapacity = androidOboeBackend_->preparedCapacity();
        out.deviceId = androidOboeBackend_->actualDeviceId();
        out.inputChannels = androidOboeBackend_->inputChannelCount();
        out.lastCallbackNanoseconds = androidOboeBackend_->lastCallbackNanoseconds();
        out.peakCallbackNanoseconds = androidOboeBackend_->peakCallbackNanoseconds();
        out.callbackDeadlineBudgetNanoseconds =
            androidOboeBackend_->callbackDeadlineBudgetNanoseconds();
        out.callbackDeadlineMisses = androidOboeBackend_->callbackDeadlineMisses();
    } else {
        out.callbackCount = realtimeCallbackCount_.load(std::memory_order_relaxed);
        out.callbackFrames = realtimeCallbackFrames_.load(std::memory_order_relaxed);
        out.sampleRateHz = static_cast<int32_t>(publishedSampleRate_.load(std::memory_order_relaxed));
        out.callbackFramesPerBurst = static_cast<int32_t>(publishedCallbackFrameCount_.load(std::memory_order_relaxed));
        out.activatedCapacity = out.callbackFramesPerBurst;
        out.lastCallbackNanoseconds =
            directUsbLastCycleNs_.load(std::memory_order_relaxed);
        out.peakCallbackNanoseconds =
            directUsbPeakCycleNs_.load(std::memory_order_relaxed);
        out.callbackDeadlineBudgetNanoseconds =
            directUsbDeadlineBudgetNs_.load(std::memory_order_relaxed);
        out.callbackDeadlineMisses =
            directUsbSchedulerDeadlineMisses_.load(std::memory_order_relaxed);
    }
    const auto pluginStats = rackGraph_.getRealtimeCounters();
    out.vstInputStarvations = pluginStats.inputStarvations;
    out.vstOutputUnderrunFrames = pluginStats.outputUnderrunFrames;
    out.vstGuestDeadlineMisses = pluginStats.guestDeadlineMisses;
    out.midiEventDrops = rackGraph_.getMidiEventDrops();
    out.planPublishDeferrals = rackGraph_.getPlanPublishDeferrals();
    out.xRunCount = static_cast<uint64_t>(std::max(0, getXRunCount()));
    return out;
}
bool AudioEngine::measureDirectUsbRoundTrip(
        int32_t timeoutMs, double result[5], std::string& error) noexcept {
    for (int i = 0; i < 5; ++i) result[i] = 0.0;
    if (timeoutMs <= 0) {
        error = "invalid timeout";
        return false;
    }
    if (!directUsbSession_.load(std::memory_order_acquire) ||
        !isRunning_.load(std::memory_order_acquire)) {
        error = "Direct USB must be running";
        return false;
    }
    if (directUsbOutputPair_.load(std::memory_order_acquire) != 0) {
        error = "Select USB outputs 1-2 before measuring output 1 to input 1";
        return false;
    }

    RoundTripMeasurement& measurement = roundTripMeasurement_;
    int32_t quiesced = 4;
    if (measurement.state.compare_exchange_strong(
            quiesced, 0, std::memory_order_acq_rel)) {
        activeRoundTripMeasurement_.store(
            nullptr, std::memory_order_release);
    }
    int32_t expected = 0;
    if (!measurement.state.compare_exchange_strong(
            expected, 5, std::memory_order_acq_rel)) {
        error = "A round-trip measurement is already running";
        return false;
    }

    const int32_t rate = std::max(
        1, static_cast<int32_t>(std::lround(sampleRate_)));
    const int32_t preRollFrames = std::max(1, rate / 10);
    const int32_t probeFrames = std::clamp(rate / 50, 1024, 2048);
    const int32_t postRollFrames = std::max(1, rate / 4);
    const int32_t captureFrames =
        preRollFrames + probeFrames + postRollFrames;
    try {
        measurement.probe.assign(
            static_cast<size_t>(probeFrames), 0.0f);
        measurement.capture.assign(
            static_cast<size_t>(captureFrames), 0.0f);
    } catch (const std::bad_alloc&) {
        measurement.state.store(0, std::memory_order_release);
        error = "Unable to allocate round-trip buffers";
        return false;
    }

    uint32_t random = 0x9e3779b9u;
    for (float& sample : measurement.probe) {
        random ^= random << 13u;
        random ^= random >> 17u;
        random ^= random << 5u;
        sample = (random & 1u) != 0u ? 0.25f : -0.25f;
    }
    measurement.sampleRate = rate;
    measurement.preRollFrames = preRollFrames;
    measurement.processedFrames.store(0, std::memory_order_relaxed);
    measurement.capturedFrames.store(0, std::memory_order_relaxed);
    measurement.inputPeak.store(0.0f, std::memory_order_relaxed);
    measurement.outputPeak.store(0.0f, std::memory_order_relaxed);
    activeRoundTripMeasurement_.store(
        &measurement, std::memory_order_release);
    int32_t preparing = 5;
    if (!measurement.state.compare_exchange_strong(
            preparing, 1, std::memory_order_acq_rel)) {
        activeRoundTripMeasurement_.store(
            nullptr, std::memory_order_release);
        measurement.state.store(0, std::memory_order_release);
        error = "Direct USB stopped before round-trip measurement started";
        return false;
    }

    const auto deadline =
        std::chrono::steady_clock::now() +
        std::chrono::milliseconds(timeoutMs);
    while (measurement.state.load(std::memory_order_acquire) == 1 &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    bool timedOut = false;
    if (measurement.state.load(std::memory_order_acquire) == 1) {
        int32_t armed = 1;
        if (measurement.state.compare_exchange_strong(
                armed, 3, std::memory_order_acq_rel)) {
            timedOut = true;
            const auto quiesceDeadline =
                std::chrono::steady_clock::now() +
                std::chrono::milliseconds(100);
            while (measurement.state.load(
                       std::memory_order_acquire) == 3 &&
                   std::chrono::steady_clock::now() <
                       quiesceDeadline) {
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(1));
            }
        } else if (armed != 2) {
            timedOut = true;
        }
    }
    if (timedOut) {
        if (measurement.state.load(
                std::memory_order_acquire) == 4) {
            activeRoundTripMeasurement_.store(
                nullptr, std::memory_order_release);
            measurement.state.store(
                0, std::memory_order_release);
        }
        error = "Round-trip measurement timed out";
        return false;
    }

    activeRoundTripMeasurement_.store(nullptr, std::memory_order_release);
    if (measurement.state.load(std::memory_order_acquire) != 2) {
        measurement.state.store(0, std::memory_order_release);
        error = "Direct USB stopped during round-trip measurement";
        return false;
    }

    const int32_t capturedFrames = std::min<int32_t>(
        measurement.capturedFrames.load(std::memory_order_acquire),
        static_cast<int32_t>(measurement.capture.size()));
    const auto correlation = analyzeRoundTripCorrelation(
        measurement.probe.data(),
        static_cast<int32_t>(measurement.probe.size()),
        measurement.capture.data(),
        capturedFrames,
        measurement.preRollFrames);
    const int32_t latencyFrames = correlation.latencyFrames;
    if (latencyFrames < 0 || correlation.correlation < 0.2) {
        measurement.state.store(0, std::memory_order_release);
        error = "Probe correlation is too weak; check the output 1 to input 1 cable and gain";
        return false;
    }

    result[0] = static_cast<double>(latencyFrames);
    result[1] =
        static_cast<double>(latencyFrames) * 1000.0 /
        static_cast<double>(measurement.sampleRate);
    result[2] = correlation.correlation;
    result[3] = measurement.inputPeak.load(std::memory_order_acquire);
    result[4] = measurement.outputPeak.load(std::memory_order_acquire);
    measurement.state.store(0, std::memory_order_release);
    error.clear();
    return true;
}


void AudioEngine::directUsbRenderLoop() {
    directUsbRenderTid_.store(static_cast<int32_t>(getTid()), std::memory_order_release);
    directUsbRenderUrgentAudio_.store(
        setCurrentThreadUrgentAudio("UsbAudioRender"), std::memory_order_release);
    const int32_t frames = static_cast<int32_t>(callbackFrameCount_);
    const auto period = std::chrono::duration<double>(
        static_cast<double>(frames) / static_cast<double>(sampleRate_));
    const auto quantumPeriod =
        std::chrono::duration_cast<std::chrono::steady_clock::duration>(period);
    const uint32_t captureTransferFrames = std::max<uint32_t>(
        1, directCaptureTransferFrames_.load(std::memory_order_acquire));
    const uint32_t captureTargetFrames = static_cast<uint32_t>(
        std::max(0, directUsbOutput_->captureTargetFrames()));
    const uint32_t captureDeadlineSlackFrames = static_cast<uint32_t>(
        std::max(0, directUsbOutput_->captureDeadlineSlackFrames()));
    const uint32_t captureRequiredFrames =
        static_cast<uint32_t>(frames) + captureTargetFrames;
    const float peakDecay = meterDecayForBlock(frames, sampleRate_);
    const float* const* renderInputPtrs = directUsbInputPlanes_.data();
    float* const renderOutputPtrs[2] = {
        directUsbOutputLeft_.data(), directUsbOutputRight_.data()};
    int32_t failureCode = 0;


    while (directUsbSession_.load(std::memory_order_acquire)) {
        const auto began = std::chrono::steady_clock::now();
        const uint32_t captureAvailableFrames = static_cast<uint32_t>(
            std::max(0, directUsbOutput_->captureAvailableFrames()));
        const uint64_t captureMissingFrames =
            captureRequiredFrames > captureAvailableFrames
                ? static_cast<uint64_t>(
                    captureRequiredFrames - captureAvailableFrames)
                : 0;
        const uint64_t roundedMissingFrames =
            ((captureMissingFrames + captureTransferFrames - 1U) /
             captureTransferFrames) * captureTransferFrames;
        const uint64_t captureDeadlineFrames =
            roundedMissingFrames + captureDeadlineSlackFrames;
        const auto captureWaitPeriod =
            std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                std::chrono::duration<double>(
                    static_cast<double>(captureDeadlineFrames) /
                    static_cast<double>(sampleRate_)));
        const auto deadline = began + captureWaitPeriod;
        directUsbDeadlineBudgetNs_.store(
            static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                quantumPeriod).count()), std::memory_order_relaxed);
        const bool captureTargetReady = directUsbOutput_ &&
            directUsbOutput_->waitForCaptureUntil(
                static_cast<int>(captureRequiredFrames), deadline);
        if (!captureTargetReady) {
            directUsbCaptureWaitTimeouts_.fetch_add(
                1, std::memory_order_relaxed);
            const int available = directUsbOutput_
                ? directUsbOutput_->captureAvailableFrames()
                : 0;
            if (!directUsbOutput_ ||
                !directUsbOutput_->driverStreaming()) {
                failureCode = usbFailureCode(
                    monotrypt::usb::StartError::
                        TransportStoppedUnexpectedly);
                break;
            }
            if (!monotrypt::usb::isCompleteCaptureQuantum(
                    available, frames)) {
                directUsbDeadlineMisses_.fetch_add(
                    1, std::memory_order_relaxed);
                // Preserve the capture timeline. The working main path
                // skipped incomplete reads; rendering here would inject a
                // zero-filled tail into guitar/NAM input.
                // A stalled capture must still acknowledge a timed-out
                // round-trip request; otherwise the measurement stays armed
                // until some future complete quantum arrives.
                RoundTripMeasurement* pendingMeasurement =
                    activeRoundTripMeasurement_.load(std::memory_order_acquire);
                if (pendingMeasurement &&
                    pendingMeasurement->state.load(std::memory_order_acquire) == 3) {
                    RoundTripMeasurement* expected = pendingMeasurement;
                    activeRoundTripMeasurement_.compare_exchange_strong(
                        expected, nullptr, std::memory_order_acq_rel);
                    pendingMeasurement->state.store(4, std::memory_order_release);
                }
                continue;
            }
        }
        directUsbOutput_->readInputChannels(
            directUsbInputPlanes_.data(),
            directUsbInputChannelCount_,
            frames);

        const auto dspBegan = std::chrono::steady_clock::now();
        processRackBlock(
            renderInputPtrs,
            directUsbInputChannelCount_,
            renderOutputPtrs,
            frames);

        RoundTripMeasurement* measurement =
            activeRoundTripMeasurement_.load(
                std::memory_order_acquire);
        if (measurement) {
            const int32_t measurementState =
                measurement->state.load(
                    std::memory_order_acquire);
            if (measurementState == 3) {
                RoundTripMeasurement* expectedMeasurement =
                    measurement;
                activeRoundTripMeasurement_.compare_exchange_strong(
                    expectedMeasurement,
                    nullptr,
                    std::memory_order_acq_rel);
                measurement->state.store(
                    4, std::memory_order_release);
            } else if (measurementState == 1) {
                const int32_t blockStart =
                    measurement->processedFrames.fetch_add(
                        frames, std::memory_order_relaxed);
                const int32_t captured =
                    measurement->capturedFrames.load(
                        std::memory_order_relaxed);
                const int32_t copyFrames = std::max(
                    0,
                    std::min(
                        frames,
                        static_cast<int32_t>(
                            measurement->capture.size()) -
                            captured));
                float measuredInputPeak =
                    measurement->inputPeak.load(
                        std::memory_order_relaxed);
                for (int32_t i = 0; i < copyFrames; ++i) {
                    const float input =
                        renderInputPtrs[0][i];
                    measurement->capture[
                        static_cast<size_t>(captured + i)] =
                        input;
                    measuredInputPeak = std::max(
                        measuredInputPeak,
                        std::fabs(input));
                }
                measurement->inputPeak.store(
                    measuredInputPeak,
                    std::memory_order_relaxed);
                measurement->capturedFrames.store(
                    captured + copyFrames,
                    std::memory_order_release);

                bool emittedProbe = false;
                for (int32_t i = 0; i < frames; ++i) {
                    const int32_t probeFrame =
                        blockStart + i -
                        measurement->preRollFrames;
                    const bool inProbe =
                        probeFrame >= 0 &&
                        probeFrame <
                            static_cast<int32_t>(
                                measurement->probe.size());
                    directUsbOutputLeft_[
                        static_cast<size_t>(i)] =
                        inProbe
                            ? measurement->probe[
                                static_cast<size_t>(
                                    probeFrame)]
                            : 0.0f;
                    directUsbOutputRight_[
                        static_cast<size_t>(i)] = 0.0f;
                    emittedProbe = emittedProbe || inProbe;
                }
                if (emittedProbe) {
                    measurement->outputPeak.store(
                        0.25f, std::memory_order_relaxed);
                }
                if (captured + copyFrames >=
                    static_cast<int32_t>(
                        measurement->capture.size())) {
                    int32_t armed = 1;
                    if (!measurement->state.compare_exchange_strong(
                            armed, 2,
                            std::memory_order_acq_rel) &&
                        armed == 3) {
                        RoundTripMeasurement* expectedMeasurement =
                            measurement;
                        activeRoundTripMeasurement_.compare_exchange_strong(
                            expectedMeasurement,
                            nullptr,
                            std::memory_order_acq_rel);
                        measurement->state.store(
                            4, std::memory_order_release);
                    }
                }
            }
        }
        const uint64_t dspNs = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - dspBegan).count());
        directUsbLastDspNs_.store(dspNs, std::memory_order_relaxed);
        uint64_t peak = directUsbPeakDspNs_.load(std::memory_order_relaxed);
        while (peak < dspNs && !directUsbPeakDspNs_.compare_exchange_weak(
                   peak, dspNs, std::memory_order_relaxed)) {}
        cpuLoad_.store(std::min(1.0f, static_cast<float>(
            (dspNs * 1e-9) / period.count())), std::memory_order_relaxed);

        float inputPeak = 0.0f;
        float outputPeak = 0.0f;
        measureStereoPeaks(renderInputPtrs[0], renderOutputPtrs[0],
                           renderOutputPtrs[1], frames, inputPeak, outputPeak);
        inputPeakHold_ = std::max(inputPeak, inputPeakHold_ * peakDecay);
        outputPeakHold_ = std::max(outputPeak, outputPeakHold_ * peakDecay);
        inputPeakLevel_.store(inputPeakHold_, std::memory_order_relaxed);
        outputPeakLevel_.store(outputPeakHold_, std::memory_order_relaxed);
        if (inputPeak >= kClippingThreshold) inputClipping_.store(true, std::memory_order_relaxed);
        if (outputPeak >= kClippingThreshold) outputClipping_.store(true, std::memory_order_relaxed);

        const bool submitted = directUsbOutput_->submitWholeQuantum(
            directUsbOutputLeft_.data(), directUsbOutputRight_.data(), frames);
        if (!submitted) {
            directUsbWriteWaitTimeouts_.fetch_add(1, std::memory_order_relaxed);
            if (!directUsbOutput_->driverStreaming()) {
                failureCode = usbFailureCode(
                    monotrypt::usb::StartError::TransportStoppedUnexpectedly);
                break;
            }
        }
        const auto finished = std::chrono::steady_clock::now();
        const uint64_t cycleNs = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                finished - began).count());
        directUsbLastCycleNs_.store(cycleNs, std::memory_order_relaxed);
        peak = directUsbPeakCycleNs_.load(std::memory_order_relaxed);
        while (peak < cycleNs && !directUsbPeakCycleNs_.compare_exchange_weak(
                   peak, cycleNs, std::memory_order_relaxed)) {}
        const auto cycleDeadline = began + quantumPeriod;
        if (finished > cycleDeadline) {
            directUsbSchedulerDeadlineMisses_.fetch_add(
                1, std::memory_order_relaxed);
            const uint64_t latenessNs = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    finished - cycleDeadline).count());
            uint64_t maxLateness = directUsbMaxSchedulerLatenessNs_.load(
                std::memory_order_relaxed);
            while (maxLateness < latenessNs &&
                   !directUsbMaxSchedulerLatenessNs_.compare_exchange_weak(
                       maxLateness, latenessNs, std::memory_order_relaxed)) {}
        }
    }
    if (RoundTripMeasurement* measurement =
            activeRoundTripMeasurement_.exchange(
                nullptr, std::memory_order_acq_rel)) {
        const int32_t state =
            measurement->state.load(std::memory_order_acquire);
        if (state == 1 || state == 3 || state == 5) {
            measurement->state.store(
                4, std::memory_order_release);
        }
    }
    directUsbRenderTid_.store(0, std::memory_order_release);
    const bool sessionWasActive =
        directUsbSession_.exchange(false, std::memory_order_acq_rel);
    if (failureCode == 0 && sessionWasActive) {
        failureCode = usbFailureCode(
            monotrypt::usb::StartError::TransportStoppedUnexpectedly);
    }
    directUsbPlaybackQuantumDrops_.store(
        directUsbOutput_->playbackQuantumDrops(), std::memory_order_relaxed);
    isRunning_.store(false, std::memory_order_release);
    requestDirectUsbCleanup(failureCode);
}


} // namespace guitarrackcraft
