#pragma once

#include <cstdint>
#include <string>
#include <thread>
#include <atomic>

extern "C" {
#include "../../../../external/shared_layout.h"
}

// Host-side accessor for the mmap'd VstpocShared region exchanged with the
// x86_64 guest. Creates and zeroes the backing file on construction; unmaps
// and closes (but does not delete) on destruction.
class SharedRing {
public:
    explicit SharedRing(const std::string& path);
    ~SharedRing();

    SharedRing(const SharedRing&) = delete;
    SharedRing& operator=(const SharedRing&) = delete;

    bool valid() const { return data_ != nullptr; }
    VstpocShared* raw() { return data_; }
    const VstpocShared* raw() const { return data_; }

    // Host (audio thread): pull up to maxFrames stereo samples from the
    // audio ring. Returns frames actually drained (may be < maxFrames on
    // underrun). Non-blocking, lock-free, RT-safe.
    int32_t pullAudio(float* outL, float* outR, int32_t maxFrames);

    bool publishTransport(uint64_t samplePosition, uint64_t transportFrame,
                          uint64_t loopEndFrame, double sampleRate,
                          double beatsPerMinute, bool playing, bool looping,
                          uint32_t blockFrames);

    // Host (UI thread): enqueue a parameter change for the guest.
    // Drops the message silently if the param ring is full.
    void pushParam(int32_t index, float value);

    // Host (RT input thread): push planar stereo samples.
    bool inputWritable(uint32_t frames) const;
    // Returns frames actually pushed; RT-safe and lock-free.
    int32_t pushInput(const float* left, const float* right, int32_t numFrames);

    // Host: declare that the input ring is being fed by a live mic stream.
    // Guest checks this flag — if set, it reads from audio_in; otherwise
    // it generates a test signal (sawtooth). Cleared on stop.
    void setMicActive(bool active);

    // Host: ask the guest to exit its loop. Sets the shared stop_flag.
    void signalStop();

    bool guestReady() const;
    uint64_t guestFramesProduced() const;
    void notifyGuest();
private:
    void notifyWake() const;
    void acceptLoop();
    int fd_ = -1;
    VstpocShared* data_ = nullptr;
    int wakeListener_ = -1;
    mutable std::atomic<int> wakeClient_{-1};
    std::atomic<bool> wakeStop_{false};
    std::thread wakeThread_;
    std::string wakePath_;
};
