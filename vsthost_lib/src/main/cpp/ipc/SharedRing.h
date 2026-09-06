#pragma once
#include "../../../../../app/src/main/cpp/plugin/IPlugin.h"
#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <sys/types.h>
extern "C" {
#include "../../../../external/shared_layout.h"
}
class SharedRing {
public:
    explicit SharedRing(const std::string& path, int reservedFd = -1);
    ~SharedRing();
    SharedRing(const SharedRing&) = delete;
    SharedRing& operator=(const SharedRing&) = delete;
    bool valid() const { return data_ != nullptr; }
    VstpocShared* raw() { return data_; }
    const VstpocShared* raw() const { return data_; }
    int32_t pullAudio(float* outL, float* outR, int32_t maxFrames);
    int32_t pullAudioBlock(float* outL, float* outR, int32_t maxFrames,
                           guitarrackcraft::MidiEvent* midi, uint32_t midiCapacity,
                           uint32_t* midiCount, bool oldest);
    bool publishTransport(uint64_t samplePosition, uint64_t transportFrame, uint64_t loopEndFrame, double sampleRate, double beatsPerMinute, bool playing, bool looping, uint32_t blockFrames, const guitarrackcraft::MidiEvent* midiEvents, uint32_t midiEventCount);
    uint32_t readMidiOutput(guitarrackcraft::MidiEvent* outputEvents, uint32_t outputCapacity) const;
    void pushParam(int32_t index, float value);
    bool inputWritable(uint32_t frames) const;
    int32_t pushInput(const float* left, const float* right, int32_t numFrames);
    void setMicActive(bool active);
    void signalStop();
    bool guestReady() const;
    uint64_t guestFramesProduced() const;
    uint64_t starvationCount() const noexcept;
    uint64_t outputDropCount() const noexcept;
    uint64_t guestDeadlineNs() const noexcept;
    uint64_t deadlineMissCount() const noexcept;
    void notifyGuest();
    void setExpectedWakePeer(pid_t pid) noexcept { expected_wake_peer_.store(pid, std::memory_order_release); }
    bool wakeReady() const noexcept { return wake_listener_fd_ >= 0; }
    const std::string& wakePath() const noexcept { return wake_path_; }
private:
    void wakeAcceptLoop();
    void signalWake() noexcept;
    int fd_ = -1;
    VstpocShared* data_ = nullptr;
    int wake_listener_fd_ = -1;
    std::atomic<int> wake_connection_fd_{-1};
    std::atomic<bool> wake_running_{false};
    std::atomic<pid_t> expected_wake_peer_{-1};
    std::thread wake_thread_;
    std::string wake_path_;
};