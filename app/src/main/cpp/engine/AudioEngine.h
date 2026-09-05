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

#ifndef GUITARRACKCRAFT_AUDIO_ENGINE_H
#define GUITARRACKCRAFT_AUDIO_ENGINE_H


#include <condition_variable>
#include <mutex>
#include <atomic>
#include <thread>
#include <string>
#include <vector>
#include "plugin/PluginChain.h"
#include <liblowlatencyaudio/DirectUsbOutput.h>
#include "AndroidOboeBackend.h"
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
        uint32_t captureTargetFrames = 0;
        uint32_t captureHeadroomFrames = 0;
        uint32_t captureDeadlineSlackFrames = 0;
        uint64_t lastDspNanoseconds = 0;
        uint64_t peakDspNanoseconds = 0;
        uint64_t lastCycleNanoseconds = 0;
        uint64_t peakCycleNanoseconds = 0;
        uint64_t deadlineBudgetNanoseconds = 0;
        uint64_t deadlineMisses = 0;
        uint64_t captureWaitTimeouts = 0;
        uint64_t writeWaitTimeouts = 0;
        uint64_t playbackQuantumDrops = 0;
        uint64_t capturePacketDrops = 0;
        uint64_t captureOverruns = 0;
        uint64_t captureUnderruns = 0;
        uint64_t playbackXruns = 0;
        uint64_t schedulerDeadlineMisses = 0;
        uint64_t maxSchedulerLatenessNanoseconds = 0;
        bool performanceHintActive = false;
        bool thermalSafetyEnabled = false;
        bool thermalSafetyActive = false;
    };
    DirectUsbRuntimeStats getDirectUsbRuntimeStats() const noexcept;
    struct RealtimeStatsSnapshot {
        uint64_t callbackCount = 0;
        uint64_t callbackFrames = 0;
        uint64_t frameCapacityViolations = 0;
        uint64_t inputUnderflowFrames = 0;
        uint64_t inputOverflowFrames = 0;
        uint64_t midiEventDrops = 0;
        uint64_t planPublishDeferrals = 0;
        uint64_t vstInputStarvations = 0;
        uint64_t vstOutputUnderrunFrames = 0;
        uint64_t vstGuestDeadlineMisses = 0;
        uint64_t xRunCount = 0;
        int32_t audioApi = 0;
        int32_t sampleRateHz = 0;
        int32_t framesPerBurst = 0;
        int32_t bufferSize = 0;
        int32_t performanceMode = 0;
        int32_t sharingMode = 0;
        int32_t callbackFramesPerBurst = 0;
        int32_t activatedCapacity = 0;
        int32_t deviceId = 0;
        int32_t inputChannels = 0;
        uint64_t lastCallbackNanoseconds = 0;
        uint64_t peakCallbackNanoseconds = 0;
        uint64_t callbackDeadlineBudgetNanoseconds = 0;
        uint64_t callbackDeadlineMisses = 0;
    };
    RealtimeStatsSnapshot getRealtimeStatsSnapshot() const noexcept;
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
                               const monotrypt::usb::UserspaceBufferConfig& bufferConfig,
                               bool thermalSafetyEnabled);

    void stop();
    // Blocking control-thread measurement; render thread only observes the atomic request.
    // Slots 0..4 are latency frames, latency ms, correlation, input peak and
    // output peak. Slot 5 is how many arrivals the loopback contained and
    // 6..11 are up to three of them as offset/correlation pairs, strongest
    // first. More than one arrival means the path sums the signal with a
    // delayed copy of itself, which no discontinuity check can see.
    static constexpr int kRoundTripResultSlots = 12;
    static constexpr int kRoundTripReportedPeaks = 3;
    bool measureDirectUsbRoundTrip(int32_t timeoutMs,
                                   double result[kRoundTripResultSlots],
                                   std::string& error) noexcept;

    bool startAndroidOboeSession(int32_t inputDeviceId, int32_t outputDeviceId, int32_t bufferFrames);
    bool openDirectUsbDevice(int fd, int driverCode = 0);
    void closeDirectUsbDevice();

    /**
     * Check if engine is running.
     */
    bool isRunning() const;

    /**
     * Check if the active Android Oboe backend reported a device/route error.
     * The flag is retained after cleanup until the next successful start.
     */
    bool hasError() const;
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
    // Zero when nothing was lost. Reported once, at the warmup boundary, off
    // the render thread.
    void getDirectUsbFirstLoss(int32_t* ring, int32_t* queued, int32_t* hadRoom,
                               int64_t* credit) const {
        if (ring) *ring = directUsbFirstLossRing_.load(std::memory_order_relaxed);
        if (queued) *queued = directUsbFirstLossQueued_.load(std::memory_order_relaxed);
        if (hadRoom) *hadRoom = directUsbFirstLossHadRoom_.load(std::memory_order_relaxed);
        if (credit) *credit = directUsbFirstLossCredit_.load(std::memory_order_relaxed);
    }
    void injectRenderStallUs(int microseconds) {
        directUsbRenderStallUs_.store(microseconds, std::memory_order_relaxed);
    }
    uint64_t getDirectUsbRenderStallsFired() const {
        return directUsbRenderStallsFired_.load(std::memory_order_relaxed);
    }
    uint64_t getDirectUsbWorkDeadlineMisses() const {
        return directUsbWorkDeadlineMisses_.load(std::memory_order_relaxed);
    }
    uint64_t getDirectUsbHeldQuanta() const {
        return directUsbHeldQuanta_.load(std::memory_order_relaxed);
    }
    uint64_t getDirectUsbLostQuanta() const {
        return directUsbLostQuanta_.load(std::memory_order_relaxed);
    }
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
    void setDirectUsbInputMeterChannel(int32_t channel) {
        directUsbInputMeterChannel_.store(channel < 0 ? 0 : channel,
                                          std::memory_order_relaxed);
    }
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
    bool loadTrackClipWav(RackPathId trackId, uint32_t slot, const std::string& path, const std::string& displayName, double sourceBpm);
    bool unloadTrackClipWav(RackPathId trackId, uint32_t slot);
    bool unloadTrackClipMidi(RackPathId trackId, uint32_t slot);
    bool selectTrackClipSlot(RackPathId trackId, uint32_t slot);


private:
    void directUsbRenderLoop();
    void directUsbThermalPolicyLoop();
    void stopDirectUsbThermalPolicy() noexcept;
    std::thread directUsbThermalPolicyThread_;
    std::mutex directUsbThermalPolicyMutex_;
    std::condition_variable directUsbThermalPolicyCv_;
    std::atomic<bool> directUsbThermalPolicyStop_{false};
    std::atomic<bool> directUsbThermalSafetyEnabled_{false};
    int32_t directUsbConfiguredWatermarkFrames_ = 0;
    int32_t directUsbConfiguredMultiplier_ = 0;
    std::atomic<bool> directUsbThermalSafetyActive_{false};
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
    std::atomic<int32_t> directUsbFailureRequest_{0};
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
    std::atomic<uint64_t> directUsbSchedulerDeadlineMisses_{0};
    // Cycles that overran the period by more than they spent blocked - that
    // is, overran while actually working. The counter above includes waiting
    // for the device, which is the stream's clock rather than a fault.
    std::atomic<uint64_t> directUsbWorkDeadlineMisses_{0};
    std::atomic<uint64_t> directUsbMaxSchedulerLatenessNs_{0};
    // Quantum periods in which the device granted no playback credit.
    std::atomic<uint64_t> directUsbCreditTimeouts_{0};
    // Rendered blocks that never reached the ring because a wait ran out of
    // deadline. Frame loss, and gated as such.
    std::atomic<uint64_t> directUsbLostQuanta_{0};
    // A rendered quantum that could not be published this cycle, kept so the
    // next cycle can publish it instead of discarding it. Depth is one by
    // design: a second undeliverable block means the device is not consuming,
    // which is a transport fault rather than something to queue deeper.
    // Holding it converts a loss into latency, and only for as long as the
    // stall lasts.
    std::vector<float> directUsbHeldLeft_;
    std::vector<float> directUsbHeldRight_;
    bool directUsbHoldingBlock_ = false;
    std::atomic<uint64_t> directUsbHeldQuanta_{0};
    // A deliberate stall in the render thread, in microseconds, fired once
    // when armed. Distinct from a service stall: this one delays the producer
    // while USB keeps draining, which is the opposite disturbance and should
    // produce the opposite symptom.
    std::atomic<int> directUsbRenderStallUs_{0};
    std::atomic<uint64_t> directUsbRenderStallsFired_{0};
    // The pipeline as it stood when the first quantum was lost. Captured with
    // plain atomic stores rather than through the flight recorder, which is
    // armed by a caller and so cannot be relied on to have been listening.
    std::atomic<uint64_t> directUsbFirstLossNs_{0};
    std::atomic<int32_t> directUsbFirstLossRing_{-1};
    std::atomic<int32_t> directUsbFirstLossQueued_{-1};
    std::atomic<int32_t> directUsbFirstLossHadRoom_{-1};
    std::atomic<int64_t> directUsbFirstLossCredit_{0};
    std::atomic<int32_t> directUsbOutputPair_{0};
    // Which capture channel the input meter follows. Zero unless a caller
    // points it elsewhere, which a loopback returning on another pair needs.
    std::atomic<int32_t> directUsbInputMeterChannel_{0};
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
    std::atomic<uint64_t> directUsbPlaybackQuantumDrops_{0};
    std::atomic<uint64_t> directUsbWriteWaitTimeouts_{0};
    std::vector<float> directUsbOutputRight_;

    RackGraph rackGraph_;
    std::unique_ptr<AndroidOboeBackend> androidOboeBackend_;
    float sampleRate_;
    uint32_t callbackFrameCount_ = 0;  // Power-of-2 frames per audio callback
    mutable std::atomic<bool> isRunning_;
    std::atomic<bool> androidOboeSession_{false};

    std::atomic<uint64_t> realtimeCallbackCount_{0};
    std::atomic<uint64_t> realtimeCallbackFrames_{0};
    
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
    std::atomic<uint32_t> directCaptureTransferFrames_{0};
    float inputPeakHold_{0.0f};
    float outputPeakHold_{0.0f};


    static constexpr float kClippingThreshold = 0.99f;



    struct RoundTripMeasurement {
        // 0 idle, 1 armed, 2 complete, 3 cancel requested,
        // 4 render thread quiesced, 5 control thread preparing.
        std::atomic<int32_t> state{0};
        std::atomic<int32_t> processedFrames{0};
        std::atomic<int32_t> capturedFrames{0};
        std::atomic<float> inputPeak{0.0f};
        std::atomic<float> outputPeak{0.0f};
        int32_t sampleRate = 0;
        int32_t preRollFrames = 0;
        std::vector<float> probe;
        std::vector<float> capture;
    };
    RoundTripMeasurement roundTripMeasurement_;
    std::atomic<RoundTripMeasurement*> activeRoundTripMeasurement_{nullptr};

    DirectUsbOutput* directUsbOutput_ = nullptr; // non-owning, NativeContext-owned
    void processRackBlock(const float* const* liveInputs, int32_t inputChannelCount,
                          float* const* outputs, uint32_t numFrames) noexcept;
    void cleanupEngineState();
};

} // namespace guitarrackcraft

#endif // GUITARRACKCRAFT_AUDIO_ENGINE_H
