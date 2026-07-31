// SPDX-License-Identifier: GPL-3.0-or-later
// USB Audio Class 2.0 direct-output driver — libusb-backed PCM sink.
#include <array>
#include <algorithm>
//
// Owns:
//   - the libusb context (one per process, lazily created)
//   - the device handle wrapping a Java-supplied UsbDeviceConnection fd
//   - the streaming-interface alt setting + claim
//   - a fixed pool of preallocated isochronous transfers
//   - an SPSC ring buffer the audio thread writes into
//   - an event-handling thread driving libusb_handle_events
//
// The audio thread (Media3's playback worker, via JNI) calls write();
// the iso completion callback drains from the same ring on the event
// thread. Single producer, single consumer — no locks on the hot path.

#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <mutex>
#include <vector>

#include <libusb.h>
#include "UsbScheduling.h"

namespace guitarrackcraft {
class DirectUsbOutput;
}

namespace monotrypt::usb {

// Categorised reason the most recent start() returned false. Surfaced
// to Kotlin so the Settings screen can show actionable text instead of
// the prior "kernel still owns it" boilerplate which was wrong about
// half the time (rate negotiation failures, clock STALLs, alloc
// failures all looked the same to the user). Order matches Kotlin's
// LibusbUacDriver.StartError enum.
enum class StartError : int {
    Ok = 0,
    NoDevice,                  // start() before open()
    NoMatchingAlt,             // selectAltSetting found nothing
    ClaimInterfaceFailed,      // libusb_claim_interface (most common
                               // — kernel UAC driver owns the iface)
    SetAltFailed,              // libusb_set_interface_alt_setting
    SetSampleRateFailed,       // SET_CUR/GET_CUR all fell through
    IsoPumpAllocFailed,        // libusb_alloc_transfer returned null
    IsoPumpSubmitFailed,       // initial libusb_submit_transfer
    TransportStoppedUnexpectedly,
    Unknown,
};

// One subrange entry returned by GET_RANGE on a clock entity. UAC2
// §5.2.1 RANGE attribute — wNumSubRanges followed by N triples of
// (dMIN, dMAX, dRES) each 4-byte LE. Most DACs report each supported
// discrete rate as its own subrange with min==max; some (like USB
// audio interfaces with a continuous PLL) report a single subrange
// covering a range. We surface both honestly.
struct ClockRateRange {
    uint8_t clockId = 0;
    uint32_t minHz = 0;
    uint32_t maxHz = 0;
    uint32_t resHz = 0;
};
// Advertised PCM Type-I playback alternative.
struct UsbFormatCandidate {
    int sampleRateHz = 0;
    int bitsPerSample = 0;
    int bytesPerSample = 0;
    int channels = 0;
};



// Negotiated PCM stream parameters (output of UAC2 enumeration).
struct StreamFormat {
    int sampleRateHz = 0;
    int bitsPerSample = 0;     // 16, 24, or 32
    int bytesPerSample = 0;    // bSubslotSize from AS_FORMAT_TYPE
    int channels = 0;
    uint8_t interfaceNumber = 0;
    uint8_t altSetting = 0;
    uint8_t endpointAddress = 0;
    uint8_t syncEndpointAddress = 0;
    uint8_t terminalLink = 0;
    uint16_t maxPacketSize = 0;
    uint8_t clockSourceId = 0;
    uint8_t controlInterfaceNum = 0;
    bool isHighSpeed = true;   // affects packet timing (125us vs 1ms)
    // Iso transfer interval. For HS: 2^(bInterval-1) microframes
    // between packets — 1 = every 125us, 4 = every 1ms. For FS:
    // bInterval value in milliseconds. Without honoring this, our
    // pump computes frames/packet against the wrong clock and
    // either underflows the device's FIFO (silence) or — worse —
    // crams too many frames into too-small packets, which the
    // device interprets at its own clock rate and renders as a
    // pitch-shifted distorted stream.
    uint8_t bInterval = 1;
    // UAC version, decoded from bcdADC in the AudioControl Header
    // class-specific descriptor (UAC1 = 0x0100, UAC2 = 0x0200).
    // Different rate-set wire format and different AS_FORMAT_TYPE
    // layout — Focal Bathys is UAC1; most newer hi-res DACs are
    // UAC2.
    uint16_t uacVersion = 0x0200;
    // Async iso feedback IN endpoint (UAC2 §3.16.2.2). Zero if the
    // device is adaptive/sync — most quality DACs (incl. Focal Bathys)
    // are async and expose one. The host reads a 16.16 fixed-point
    // "samples per (micro)frame" value the device wants the host to
    // pace at; without honoring it we'd drift and the device's
    // rate-matching FIFO would eventually over- or under-flow.
    uint8_t feedbackEndpointAddress = 0;
    uint16_t feedbackMaxPacketSize = 0;
    uint8_t feedbackInterval = 0;
    // An IN AS endpoint with usage type implicit-feedback clocks the
    // paired asynchronous playback endpoint from its packet cadence.
    bool implicitFeedback = false;
    // Every clock entity ID we found in the AudioControl topology,
    // in walk order. Used as a fall-through search list when SET_CUR
    // / GET_CUR on the topology-resolved clockSourceId fails — some
    // devices' Selector / Multiplier resolution is non-obvious and
    // it's cheaper to just try them all than misparse the graph.
    std::vector<uint8_t> candidateClockIds;
};
// Duplex capture format (PCM Type-I IN stream).
using CaptureFormat = StreamFormat;

struct CaptureStats {
    uint64_t overruns = 0;
    uint64_t underruns = 0;
    uint64_t sequence = 0;
};
struct ImplicitFeedbackStats {
    uint64_t fifoDepth = 0;
    uint64_t deferredTransfers = 0;
    uint64_t metadataFifoOverruns = 0;
    uint64_t captureTransferErrors = 0;
    uint64_t playbackTransferErrors = 0;
    uint64_t ringFrames = 0;
    uint64_t captureRingFrames = 0;
    uint64_t lifecycleFailures = 0;
    bool transportFailed = false;
    bool eventThreadUrgentAudio = false;
    uint64_t pendingDepth = 0;
    uint64_t pendingHighWater = 0;
    uint64_t zeroRunwayEvents = 0;
    uint64_t maxPendingAgeNs = 0;
};


class LibusbUacDriver {
    friend struct UsbDriverTestAccess;
public:
    LibusbUacDriver();
    ~LibusbUacDriver();
    LibusbUacDriver(const LibusbUacDriver&) = delete;
    LibusbUacDriver& operator=(const LibusbUacDriver&) = delete;

    bool ensureContext();
    bool open(int fileDescriptor, int driverCode = 0);
    // Enumerates PCM Type-I playback alternatives with at least two
    // channels. This is a control-thread operation and may issue
    // descriptor/control reads.
    std::vector<UsbFormatCandidate> enumerateFormats();
    // Maximum selectable mono channels advertised by valid PCM Type-I IN
    // streaming alternate settings. Control-thread enumeration.
    int captureChannelCount() const;

    void close();
    bool isOpen() const { return device_ != nullptr; }
    bool waitForCaptureFrames(int frames, int timeoutMs) const;

    // Negotiates UAC2 alt setting matching the requested format,
    // claims the streaming interface, sets the clock source rate via
    // a class-specific control transfer, and starts the iso pump.
    // Starts capture and prepares playback storage; OUT is submitted by startPlayback().
    bool startDuplex(int sampleRateHz, int bitsPerSample, int channels,
                     int bytesPerSample = 0);
    // Consumes the initial ring prime and submits feedback/OUT transfers.
    bool startPlayback() noexcept;
    // Non-blocking stop request; final resource teardown remains stop().
    void requestStop() noexcept;
    int startupPrimeFrames() const noexcept;
    int readCapturePcm(uint8_t* dst, int frames);
    int captureAvailableFrames() const;
    const CaptureFormat& currentCaptureFormat() const { return captureFormat_; }
    CaptureStats captureStats() const {
        return {captureOverruns_.load(std::memory_order_acquire),
                captureUnderruns_.load(std::memory_order_acquire),
                captureSequence_.load(std::memory_order_acquire)};
    }
    int discardCaptureFrames(int maxFrames) noexcept;
    ImplicitFeedbackStats implicitFeedbackStats() const noexcept {
        const size_t implicitWrite =
            implicitWrite_.load(std::memory_order_acquire);
        const size_t implicitRead =
            implicitRead_.load(std::memory_order_acquire);
        const size_t ringHead = ringHead_.load(std::memory_order_acquire);
        const size_t ringTail = ringTail_.load(std::memory_order_acquire);
        const size_t captureHead =
            captureHead_.load(std::memory_order_acquire);
        const size_t captureTail =
            captureTail_.load(std::memory_order_acquire);
        const int playbackStride =
            std::max(1, playbackFrameStride_.load(std::memory_order_acquire));
        const int captureStride =
            std::max(1, captureFrameStride_.load(std::memory_order_acquire));
        return {
            implicitWrite >= implicitRead ? implicitWrite - implicitRead : 0,
            deferredTransfers_.load(std::memory_order_acquire),
            metadataFifoOverruns_.load(std::memory_order_acquire),
            captureTransferErrors_.load(std::memory_order_acquire),
            playbackTransferErrors_.load(std::memory_order_acquire),
            (ringHead >= ringTail ? ringHead - ringTail : 0) /
                static_cast<size_t>(playbackStride),
            (captureHead >= captureTail ? captureHead - captureTail : 0) /
                static_cast<size_t>(captureStride),
            lifecycleFailures_.load(std::memory_order_acquire),
            transportFailed_.load(std::memory_order_acquire),
            eventThreadUrgentAudio_.load(std::memory_order_acquire),
            pendingDepth_.load(std::memory_order_acquire),
            pendingHighWater_.load(std::memory_order_acquire),
            zeroRunwayEvents_.load(std::memory_order_acquire),
            maxPendingAgeNs_.load(std::memory_order_acquire)
        };
    }
    int32_t eventThreadTid() const noexcept {
        return eventThreadTid_.load(std::memory_order_acquire);
    }
    uint64_t captureSequence() const {
        return captureSequence_.load(std::memory_order_acquire);
    }
    // Returns false if any step fails (reasons logged with TAG
    // "LibusbUacDriver" — most often `LIBUSB_ERROR_BUSY` from
    // libusb_claim_interface, meaning the kernel UAC driver still
    // owns the interface and the user needs to enable Developer
    // Options → "Disable USB audio routing").
    bool start(int sampleRateHz, int bitsPerSample, int channels,
               int bytesPerSample = 0);

    // Requests cancellation, performs a bounded callback drain, and releases
    // interfaces only after every transfer retires. Safe to call repeatedly
    // from control threads.
    void stop();

    bool isStreaming() const {
        return streaming_.load(std::memory_order_acquire);
    }

    // Discards any PCM still in the ring without tearing down the iso
    // pump. Use between tracks so the Android kernel cannot reclaim the
    // streaming interface between same-device format changes.
    void flushRing();

    // Current queued playback frames, used to enforce bounded latency.
    uint64_t queuedOutFrames() const noexcept {
        return queuedOutFrames_.load(std::memory_order_acquire);
    }
    int captureTransferFrames() const noexcept {
        return captureTransferFrames_.load(std::memory_order_acquire);
    }
    int playbackTargetFrames() const noexcept {
        return playbackTargetFrames_.load(std::memory_order_acquire);
    }
    int bufferedFrames() const;
    // Set graph quantum and playback watermark. A positive watermarkFrames is
    // an explicit userspace target; zero selects the device-derived policy.
    void setGraphQuantum(
        int frames,
        int periodMultiplier = kDefaultPeriodMultiplier,
        int watermarkFrames = 0);

    // Returns true when the iso pump is already streaming a stream
    // matching [sampleRate]/[bitsPerSample]/[channels]. Used by
    // LibusbAudioSink.configure() to skip a redundant stop/start when
    // the next track is the same format as the last (the common case
    // for an album with consistent encoding).
    bool isStreamingFormat(int sampleRate, int bitsPerSample, int channels) const;

    // Writes [frames] frames of interleaved PCM into the ring. Returns
    // the actual number written (may be less than requested if the
    // ring is nearly full). Caller will retry on the next audio-thread
    // tick. Buffer must be in the negotiated subslot size — write()
    // does not convert.
    int writePcm(const uint8_t* data, int frames);

    // How many frames can be written right now without blocking.
    int writableFrames() const;
    // Waits until [frames] fit inside the bounded playback watermark.
    // Uses the shared transport eventfd; no polling or spin sleeps.
    bool waitForWritableFrames(int frames, int timeoutMs) const;
    // Silence padding reaches the DAC and is an audible playback xrun.
    uint64_t playbackXRunCount() const {
        return playbackUnderruns_.load(std::memory_order_acquire);
    }
    // Bounded-watermark short writes are retried by the producer without
    // dropping PCM. Track them as scheduling pressure, not audible xruns.
    uint64_t playbackBackpressureCount() const {
        return playbackOverruns_.load(std::memory_order_acquire);
    }
    // Exact silence inserted into submitted ISO packets. Unlike the xrun
    // transition count, these counters reveal sustained starvation.
    uint64_t playbackSilentPacketCount() const noexcept {
        return playbackSilentPackets_.load(std::memory_order_acquire);
    }
    uint64_t playbackSilentFrameCount() const noexcept {
        return playbackSilentFrames_.load(std::memory_order_acquire);
    }

    // Total PCM frames the iso pump has drained from the ring since
    // [start] — i.e. the frames the device has actually been told to
    // play. Used by LibusbAudioSink for getCurrentPositionUs / hasPendingData
    // / isEnded; reporting framesWritten there instead caused ExoPlayer
    // to think a track had finished within seconds (the renderer fills
    // the ring much faster than realtime), which manifested as 5-second
    // playbacks followed by an early skip to the next track.
    long playedFrames() const {
        return playedFrames_.load(std::memory_order_acquire);
    }

    // Total PCM frames the host has pushed into the ring since [start].
    long writtenFrames() const {
        return writtenFrames_.load(std::memory_order_acquire);
    }

    const StreamFormat& currentFormat() const { return format_; }

    // Reason the most recent start() returned false, or Ok if it
    // succeeded / hasn't been attempted. Reset to Ok on the next
    // successful start.
    StartError lastError() const {
        return lastError_.load(std::memory_order_acquire);
    }

    // Best-effort detail string accompanying [lastError]. May be empty.
    // Holds the libusb_strerror text or a contextual line written at
    // the failure site.
    std::string lastErrorDetail() const;

    // Snapshot of the GET_RANGE table reported by the device's clock
    // entities. Populated during start(); empty before any start
    // attempt or if the device returned nothing readable. Used by the
    // Settings UI to show what rates the DAC actually supports —
    // people want to know whether their hi-res 24/192 file is going
    // bit-perfect or quietly being downsampled to 48k.
    std::vector<ClockRateRange> supportedRates() const;

private:
    friend class guitarrackcraft::DirectUsbOutput;

    struct PlaybackWriteRegion {
        uint8_t* first = nullptr;
        size_t firstBytes = 0;
        uint8_t* second = nullptr;
        size_t secondBytes = 0;
        size_t producerCursor = 0;
        int frames = 0;
        int frameStride = 0;
    };

    struct CaptureReadRegion {
        const uint8_t* first = nullptr;
        size_t firstBytes = 0;
        const uint8_t* second = nullptr;
        size_t secondBytes = 0;
        size_t consumerCursor = 0;
        int frames = 0;
        int frameStride = 0;
    };

    // Single-producer/two-phase playback write. The caller fills both spans
    // before commit publishes the producer cursor to the USB event thread.
    PlaybackWriteRegion preparePlaybackWrite(int requestedFrames) noexcept;
    void commitPlaybackWrite(const PlaybackWriteRegion& region) noexcept;

    // Single-consumer/two-phase capture read. The selected bytes remain owned
    // by the render thread until commit publishes the consumer cursor.
    CaptureReadRegion prepareCaptureRead(int requestedFrames) noexcept;
    void commitCaptureRead(const CaptureReadRegion& region) noexcept;
    // Walks the active config, finds an Audio Streaming alt-setting
    // whose AS_GENERAL/AS_FORMAT_TYPE descriptors match the request,
    // and fills out_fmt. Returns false if no match.
    bool selectAltSetting(int sampleRateHz, int bitsPerSample,
                          int channels, int bytesPerSample,
                          StreamFormat* out_fmt);
    // Sends a UAC2 SET_CUR(CS_SAM_FREQ_CONTROL) class-specific
    // control transfer to the clock source entity. UAC2 puts the rate
    // on a clock-source unit, not on the endpoint like UAC1 did.
    bool setSampleRate(uint32_t hz);

    // Issues GET_RANGE on the given clock entity and appends decoded
    bool selectCaptureAltSetting(const StreamFormat& playback,
                                 StreamFormat* out_fmt);
    bool startCapturePump();
    bool stopCapturePump();
    static void LIBUSB_CALL onCaptureTrampoline(libusb_transfer* xfr);
    void onCapture(libusb_transfer* xfr);
    // (min, max, res) subranges into supportedRates_. No-op if the
    // device returns less than the 2-byte wNumSubRanges header.
    void captureRangeForClock(uint8_t clockId);

    bool startIsoPump(bool submit = true);
    bool ensureEventThread();
    bool line6SelectFormat(StreamFormat* playback, StreamFormat* capture);
    bool line6VendorSetup();
    bool line6StartDuplex();
    // Number of frames in all initially prepared OUT packets.
    int exactInitialPacketFrames_ = 0;
    bool submitIsoPump();
    bool stopIsoPump();

    // libusb iso completion callback — static trampoline into onIso().
    static void LIBUSB_CALL onIsoTrampoline(libusb_transfer* xfr);
    void onIso(libusb_transfer* xfr);
    bool prepareImplicitTransfer(libusb_transfer* xfr);
    void submitPendingImplicitTransfers();
    void markTransportFailed() noexcept;
    int captureFrameLimit() const noexcept;

    // Drains [bytes] bytes from the ring into [dst]. Returns bytes
    // actually drained; pads remainder with silence so iso packets
    // ship even on underrun (better a glitch than a dropped URB).
    int drainRing(uint8_t* dst, int bytes);

    // SPSC ring buffer. Power-of-two size, atomic head/tail. Producer
    // is the audio thread (writePcm); consumer is the event thread
    // via onIso → drainRing.
    std::vector<uint8_t> ring_;
    size_t ringMask_ = 0;
    std::atomic<size_t> ringHead_{0};  // producer cursor (writePcm)
    std::atomic<size_t> ringTail_{0};  // consumer cursor (onIso)
    // Frame-based latency budget, configured by the graph quantum.
    std::atomic<int> graphQuantum_{64};
    std::atomic<int> playbackTargetFrames_{128};
    std::atomic<int> startupPrimeFrames_{128};
    std::atomic<uint64_t> queuedOutFrames_{0};
    mutable std::mutex mutex_;          // guards open/start/stop only
    mutable std::recursive_mutex sessionMutex_; // serializes start/duplex/stop
    libusb_context* ctx_ = nullptr;
    libusb_device_handle* device_ = nullptr;
    int driverCode_ = 0;
    bool line6Profile_ = false;
    int fd_ = -1;
    std::atomic<bool> contextReady_{false};

    StreamFormat format_;
    std::atomic<int> playbackFrameStride_{1};
    std::atomic<bool> streaming_{false};
    std::atomic<bool> stopRequested_{false};
    // True iff we currently hold libusb_claim_interface on
    // format_.interfaceNumber. Tracked separately from streaming_
    // because we keep the claim alive across format changes (e.g.
    // 16-bit → 24-bit between tracks of the same album) so the
    // Android kernel can't briefly re-attach snd-usb-audio in the
    // gap and bounce us with LIBUSB_ERROR_BUSY on the next claim.
    bool interfaceClaimed_ = false;
    // Same idea for the AudioControl interface. We need to own it
    // for class-specific control transfers (SET_CUR / GET_CUR /
    // GET_RANGE on clock entities) to reach the device — otherwise
    // the kernel's snd-usb-audio still has AC claimed and silently
    // returns LIBUSB_ERROR_IO on every control transfer with
    // wIndex pointing at the AC interface.
    bool controlInterfaceClaimed_ = false;
    uint8_t claimedControlIface_ = 0xFF;
    // Capture SPSC ring and persistent IN transfer storage.
    std::vector<uint8_t> captureRing_;
    size_t captureRingMask_ = 0;
    std::atomic<size_t> captureHead_{0};
    std::atomic<size_t> captureTail_{0};
    CaptureFormat captureFormat_{};
    std::atomic<int> captureFrameStride_{1};
    std::atomic<bool> captureActive_{false};
    int captureWakeFd_ = -1;
    std::vector<libusb_transfer*> captureTransfers_;
    std::vector<std::vector<uint8_t>> captureTransferBuffers_;
    bool captureInterfaceClaimed_ = false;
    uint8_t claimedCaptureIface_ = 0xFF;
    std::atomic<uint64_t> captureOverruns_{0};
    std::atomic<uint64_t> captureUnderruns_{0};
    std::atomic<uint64_t> captureSequence_{0};
    std::atomic<int> captureInflight_{0};
    static constexpr size_t kImplicitFifoCapacity = 256;
    static constexpr size_t kMaxTransferCount = 8;
    std::array<std::atomic<uint32_t>, kImplicitFifoCapacity> implicitFrames_{};
    std::atomic<size_t> implicitRead_{0};
    bool lowLatencyProfile_ = false;
    int transferCount_ = 4;
    int playbackPacketsPerTransfer_ = 1;
    int capturePacketsPerTransfer_ = 1;
    std::atomic<int> captureTransferFrames_{0};
    std::atomic<size_t> implicitWrite_{0};
    std::atomic<uint64_t> deferredTransfers_{0};
    std::atomic<uint64_t> metadataFifoOverruns_{0};
    std::atomic<uint64_t> captureTransferErrors_{0};
    std::atomic<uint64_t> playbackTransferErrors_{0};
    std::atomic<uint64_t> lifecycleFailures_{0};
    std::atomic<bool> transportFailed_{false};
    int captureMaxFramesPerPacket_ = 0;
    std::atomic<bool> eventThreadUrgentAudio_{false};
    std::atomic<int32_t> eventThreadTid_{0};

    // Iso pump state — touched only from the event thread once
    // streaming_ is true.
    std::vector<libusb_transfer*> transfers_;
    std::vector<std::vector<uint8_t>> transferBuffers_;
    std::vector<libusb_transfer*> feedbackTransfers_;
    std::vector<std::vector<uint8_t>> feedbackBuffers_;
    std::array<libusb_transfer*, kMaxTransferCount> pendingImplicitTransfers_{};
    std::array<uint64_t, kMaxTransferCount> pendingImplicitSinceNs_{};
    size_t pendingImplicitCount_ = 0;
    std::atomic<uint64_t> pendingDepth_{0};
    std::atomic<uint64_t> pendingHighWater_{0};
    std::atomic<uint64_t> zeroRunwayEvents_{0};
    std::atomic<uint64_t> maxPendingAgeNs_{0};
    std::atomic<bool> implicitZeroRunwayActive_{false};
    std::atomic<int> inflight_{0};     // active transfers (data + fb)
    // Cumulative frame counters used for honest position reporting
    // (see playedFrames() / writtenFrames()). Reset on start().
    std::atomic<long> writtenFrames_{0};
    std::atomic<long> playedFrames_{0};
    // Count state transitions, rather than every silent packet or short
    // write: a sustained starvation/overflow is one audible xrun.
    std::atomic<uint64_t> playbackOverruns_{0};
    std::atomic<uint64_t> playbackUnderruns_{0};
    std::atomic<bool> playbackOverrunActive_{false};
    std::atomic<bool> playbackUnderrunActive_{false};
    std::atomic<uint64_t> playbackSilentPackets_{0};
    std::atomic<uint64_t> playbackSilentFrames_{0};
    std::atomic<bool> playbackStarted_{false};
    std::thread eventThread_;
    std::atomic<bool> deferOutputStart_{false};

    // Q16 frames per USB packet. Without explicit feedback it is seeded
    // from the selected sample rate and packet cadence. Explicit feedback
    // reports frames per microframe/frame, so onFeedback multiplies by the
    // endpoint interval before publishing the per-packet value consumed by
    // onIso. The fractional accumulator retains sub-frame remainder.
    monotrypt::usb::RationalPacketScheduler nominalScheduler_;
    std::atomic<uint32_t> framesPerUframe_q16_{0};
    int microframesPerSec_ = 8000;
    int maxFramesPerPacket_ = 0;
    int packetIntervalUframes_ = 1;
    uint32_t fracAccumulator_q16_ = 0;  // event-thread-only state


    static void LIBUSB_CALL onFeedbackTrampoline(libusb_transfer* xfr);
    void onFeedback(libusb_transfer* xfr);

    // Persist the most recent failure category + a human-readable
    // detail line. Mutated only from start()'s call stack (under
    // mutex_) and from setSampleRate; read from any thread via
    // lastError() / lastErrorDetail() so we don't have to plumb a
    // result code back through several layers. Reset to Ok at the
    // top of every start() call.
    std::atomic<StartError> lastError_{StartError::Ok};
    mutable std::mutex errorMutex_;
    std::string lastErrorDetail_;

    // Filled at start() time by setSampleRate's GET_RANGE pass. Read
    // by the JNI getter to surface to the UI. Guarded by the same
    // errorMutex_ — readers and the start() writer don't race on the
    // hot path.
    std::vector<ClockRateRange> supportedRates_;
};

} // namespace monotrypt::usb
