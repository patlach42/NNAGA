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

#ifndef GUITARRACKCRAFT_AUDIO_ENGINE_H
#define GUITARRACKCRAFT_AUDIO_ENGINE_H

#include <cstdint>
#include <condition_variable>
#include <mutex>
#include <atomic>
#include <thread>
#include <string>
#include <vector>
#include "plugin/PluginChain.h"
#include "DirectUsbOutput.h"

#include "../plugin/RackGraph.h"
namespace guitarrackcraft {

/**
 * Audio engine using the direct USB UAC transport for audio I/O.
 * Processes audio through the plugin graph in real-time.
 */
class AudioEngine {
public:

    enum class DirectUsbState : uint8_t { Stopped, Starting, Running, Failed, Stopping };
    struct DirectUsbRuntimeStats {
        DirectUsbState state = DirectUsbState::Stopped;
        uint64_t sessionId = 0;
        int32_t failureCode = 0;
        uint32_t effectiveQuantum = 0;
        int32_t requestedPeriodMultiplier = 0;
        uint32_t startupPrimeFrames = 0;
        uint32_t steadyTargetFrames = 0;
        uint32_t captureRingFrames = 0;
        uint32_t playbackRingFrames = 0;
        uint32_t queuedOutFrames = 0;
        uint32_t captureTransferFrames = 0;
        uint64_t lastDspNanoseconds = 0;
        uint64_t peakDspNanoseconds = 0;
        uint64_t lastCycleNanoseconds = 0;
        uint64_t peakCycleNanoseconds = 0;
        uint64_t deadlineBudgetNanoseconds = 0;
        uint64_t deadlineMisses = 0;
        uint64_t captureWaitTimeouts = 0;
        uint64_t writeWaitTimeouts = 0;
        uint64_t captureOverruns = 0;
        uint64_t captureUnderruns = 0;
        uint64_t playbackXruns = 0;
        bool performanceHintActive = false;
    };
    DirectUsbRuntimeStats getDirectUsbRuntimeStats() const noexcept;
    AudioEngine();
    ~AudioEngine();


    /**
     * Stop audio processing.
     */
    /**
     * Start a playback-only USB session. No Android audio streams are opened;
     * the native render thread supplies silent (or WAV) mono input to the
     * plugin chain and writes processed stereo frames to DirectUsbOutput.
     */
    bool startDirectUsbSession(float sampleRate, int32_t bitsPerSample,
                               int32_t subslotBytes, int32_t channels,
                               int32_t outputPair,
                               int32_t bufferFrames, int32_t periodMultiplier,
                               int32_t watermarkFrames);

    void stop();
    bool openDirectUsbDevice(int fd);

    /**
     * Check if engine is running.
     */
    bool isRunning() const;

    /** Get the parallel rack graph for track and master operations. */
    RackGraph& getRackGraph() { return rackGraph_; }
    const RackGraph& getRackGraph() const { return rackGraph_; }

    /**
     * Get current sample rate.
     */
    float getSampleRate() const { return publishedSampleRate_.load(std::memory_order_acquire); }

    /**
     * Get actual callback frame count (buffer size used by the audio callback).
     */
    uint32_t getCallbackFrameCount() const {
        return publishedCallbackFrameCount_.load(std::memory_order_acquire);
    }


    /**
     * Get current latency in milliseconds.
     */
    double getLatencyMs() const;

    /**
     * Get input peak level (0.0–1.0).
     */
    float getInputLevel() const;

    /**
     * Get output peak level (0.0–1.0).
     */
    float getOutputLevel() const;

    /**
     * Get CPU load (0.0–1.0) from processing time vs buffer duration.
     */
    float getCpuLoad() const;

    /**
     * Get cumulative actual render discontinuities from the direct-USB
     * playback/capture transport.
     */
    int32_t getXRunCount() const;

    /**
     * True if input has clipped (peak >= 0.99).
     */
    bool isInputClipping() const;

    /**
     * True if output has clipped (peak >= 0.99).
     */
    bool isOutputClipping() const;

    /**
     * Clear clipping indicators (call when user taps to reset).
     */
    void resetClipping();


    // Attach the duplex direct USB transport. Lifetime is owned by NativeContext.
    void setDirectUsbOutput(DirectUsbOutput* output) { directUsbOutput_ = output; }
    bool isDirectUsbRenderUrgentAudio() const noexcept {
        return directUsbRenderUrgentAudio_.load(std::memory_order_acquire);
    }
    uint64_t directUsbCaptureWaitTimeouts() const noexcept {
        return directUsbCaptureWaitTimeouts_.load(std::memory_order_acquire);
    }
    uint64_t directUsbWriteWaitTimeouts() const noexcept {
        return directUsbWriteWaitTimeouts_.load(std::memory_order_acquire);
    }

    bool loadTrackWav(RackPathId trackId, const std::string& path,
                      const std::string& displayName);
    bool unloadTrackWav(RackPathId trackId);


private:
    void directUsbRenderLoop();
    void directUsbThermalPolicyLoop();
    void stopDirectUsbThermalPolicy() noexcept;
    std::thread directUsbThermalPolicyThread_;
    std::mutex directUsbThermalPolicyMutex_;
    std::condition_variable directUsbThermalPolicyCv_;
    std::atomic<bool> directUsbThermalPolicyStop_{false};
    int32_t directUsbConfiguredWatermarkFrames_ = 0;
    int32_t directUsbConfiguredMultiplier_ = 0;
    bool directUsbThermalSafetyActive_ = false;
    std::atomic<int32_t> directUsbRenderTid_{0};
    std::atomic<void*> directUsbPerformanceHintSession_{nullptr};
    std::thread directUsbRenderThread_;
    std::atomic<bool> directUsbSession_{false};
    void cleanupWorkerLoop();
    void requestDirectUsbCleanup(int32_t failureCode) noexcept;
    void finishDirectUsbCleanup();
    bool waitForDirectUsbCleanup();
    std::thread cleanupWorker_;
    mutable std::mutex lifecycleMutex_;
    std::mutex publicLifecycleMutex_;
    std::condition_variable lifecycleCv_;
    std::condition_variable cleanupDoneCv_;
    std::atomic<bool> cleanupRequested_{false};
    std::atomic<bool> cleanupWorkerStop_{false};
    bool cleanupInProgress_ = false;
    std::atomic<DirectUsbState> directUsbState_{DirectUsbState::Stopped};
    std::atomic<uint64_t> directUsbSessionId_{0};
    std::atomic<int32_t> directUsbFailureCode_{0};
    std::atomic<uint32_t> directUsbEffectiveQuantum_{0};
    std::atomic<int32_t> directUsbPeriodMultiplier_{0};
    std::atomic<uint32_t> directUsbPrimeFrames_{0};
    std::atomic<uint32_t> directUsbSteadyTargetFrames_{0};
    std::atomic<uint64_t> directUsbLastDspNs_{0};
    std::atomic<uint64_t> directUsbPeakDspNs_{0};
    std::atomic<uint64_t> directUsbLastCycleNs_{0};
    std::atomic<uint64_t> directUsbPeakCycleNs_{0};
    std::atomic<uint64_t> directUsbDeadlineBudgetNs_{0};
    std::atomic<uint64_t> directUsbDeadlineMisses_{0};
    std::atomic<bool> directUsbRenderUrgentAudio_{false};
    std::atomic<bool> directUsbPerformanceHintActive_{false};
    std::atomic<bool> cleanupStarted_{true};
    int32_t directUsbBits_ = 0;
    int32_t directUsbSubslotBytes_ = 0;
    int32_t directUsbChannels_ = 0;
    std::vector<float> directUsbInputBuffer_;
    std::vector<float*> directUsbInputPlanes_;
    int32_t directUsbInputChannelCount_ = 0;
    std::vector<float> directUsbOutputLeft_;
    std::atomic<uint64_t> directUsbCaptureWaitTimeouts_{0};
    std::vector<float> directUsbStartupLeft_;
    std::vector<float> directUsbStartupRight_;
    int32_t directUsbStartupBlocks_ = 0;
    std::atomic<uint64_t> directUsbWriteWaitTimeouts_{0};
    std::vector<float> directUsbOutputRight_;


    RackGraph rackGraph_;
    float sampleRate_;
    uint32_t callbackFrameCount_ = 0;  // Power-of-2 frames per audio callback
    std::atomic<bool> isRunning_;

    
    // Temporary buffers for plugin chain
    const float* inputPtrs_[2];
    float* outputPtrs_[2];

    // Level metering and CPU (written from audio thread, read from UI)
    std::atomic<float> inputPeakLevel_{0.0f};
    std::atomic<float> outputPeakLevel_{0.0f};
    std::atomic<float> cpuLoad_{0.0f};
    std::atomic<bool> inputClipping_{false};
    std::atomic<bool> outputClipping_{false};
    // Immutable UI telemetry snapshots. Writers are lifecycle/audio threads; getters never touch streams.
    std::atomic<float> publishedSampleRate_{48000.0f};
    std::atomic<uint32_t> publishedCallbackFrameCount_{0};
    std::atomic<double> publishedLatencyMs_{0.0};
    std::atomic<int32_t> publishedXRunCount_{0};
    std::atomic<uint32_t> directCaptureRingFrames_{0};
    std::atomic<uint32_t> directPlaybackRingFrames_{0};
    std::atomic<uint32_t> directQueuedOutFrames_{0};
    std::atomic<uint32_t> directCaptureTransferFrames_{0};
    std::atomic<uint64_t> directCaptureOverruns_{0};
    std::atomic<uint64_t> directCaptureUnderruns_{0};
    std::atomic<uint64_t> directPlaybackXruns_{0};
    float inputPeakHold_{0.0f};
    float outputPeakHold_{0.0f};


    static constexpr float kClippingThreshold = 0.99f;




    DirectUsbOutput* directUsbOutput_ = nullptr; // non-owning, NativeContext-owned
    void processRackBlock(const float* const* liveInputs, int32_t inputChannelCount,
                          float* const* outputs, uint32_t numFrames) noexcept;
    void cleanupEngineState();
};

} // namespace guitarrackcraft

#endif // GUITARRACKCRAFT_AUDIO_ENGINE_H
