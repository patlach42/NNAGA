#ifndef GUITARRACKCRAFT_RACK_GRAPH_H
#define GUITARRACKCRAFT_RACK_GRAPH_H

#include "PluginChain.h"

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace guitarrackcraft {

using RackPathId = uint64_t;
constexpr RackPathId kMasterPathId = 0;

enum class LaunchQuantization : uint8_t { Bar, Quarter, Eighth, Sixteenth, None };
struct WavClip {
    std::vector<float> left;
    std::vector<float> right;
    uint32_t sampleRate = 0;
    std::string displayName;
};
struct MidiTimedEvent {
    uint64_t frame = 0;
    MidiEvent event{};
};
struct MidiClip {
    std::vector<MidiTimedEvent> events;
    uint64_t durationFrames = 0;
    std::string displayName;
};

struct MidiNoteInfo { uint64_t startFrame{}; uint64_t durationFrames{}; int32_t pitch{}; int32_t velocity{}; };
struct TrackClipSlotInfo { RackPathId trackId{}; uint32_t slot{}; bool wavLoaded{}; bool midiLoaded{}; std::string displayName; double durationSec{}; bool active{}; };
struct TrackSnapshot { RackPathId id; float volume; bool inputArmed; bool wavLoaded; std::string wavDisplayName; double wavDurationSec; bool playing; bool looping; double positionSec; uint64_t transportFrame; bool recordPending; bool recording; bool punchArmed; int32_t inputChannel; bool midiLoaded; bool midiPlaying; };
struct TransportSnapshot { bool playing; double positionSec; double beatsPerMinute; uint64_t samplePosition; uint64_t transportFrame; };

class RackGraph {
public:
    struct State {
        struct Track {
            float volume;
            bool inputArmed;
            int32_t inputChannel;
            PluginChain::ChainState chain;
        };
        std::vector<Track> tracks;
        PluginChain::ChainState master;
    };
    RackGraph(); ~RackGraph();
    RackPathId addTrack(); bool removeTrack(RackPathId); std::vector<TrackSnapshot> getTracks() const; std::vector<TrackClipSlotInfo> getTrackClipSlots(RackPathId) const; std::vector<MidiNoteInfo> getTrackClipMidiNotes(RackPathId,uint32_t) const;
    bool setTrackVolume(RackPathId, float); bool setTrackInputArmed(RackPathId, bool); bool setTrackInputChannel(RackPathId, int32_t); void setAvailableInputChannelCount(int32_t) noexcept;
    bool attachTrackWav(RackPathId, std::shared_ptr<const WavClip>); bool unloadTrackWav(RackPathId); bool clearTrackWavs();
    bool attachTrackWavSlot(RackPathId, uint32_t, std::shared_ptr<const WavClip>); bool unloadTrackWavSlot(RackPathId, uint32_t);
    bool attachTrackMidi(RackPathId, std::shared_ptr<const MidiClip>); bool unloadTrackMidi(RackPathId);
    bool attachTrackMidiSlot(RackPathId, uint32_t, std::shared_ptr<const MidiClip>); bool unloadTrackMidiSlot(RackPathId, uint32_t); bool selectTrackClipSlot(RackPathId, uint32_t);
    bool startTrackLoopRecording(RackPathId, double bars, LaunchQuantization, bool enterOnPunch);
    bool cancelTrackLoopRecording(RackPathId);
    bool setTransportPlaying(bool); bool restartTransport(); void setBeatsPerMinute(double); TransportSnapshot getTransportSnapshot() const;
    bool setTrackTransportPlaying(RackPathId, bool, LaunchQuantization); bool setTrackTransportLooping(RackPathId, bool);
    std::vector<float> getTrackWaveformPeaks(RackPathId, uint32_t maxBuckets) const;
    std::shared_ptr<PluginChain> getChain(RackPathId) const;
    void setSampleRate(float, uint32_t); void activate(); void deactivate(); void pauseAndResetTransport();
    void process(const float* const*, int, float* const*, uint32_t) noexcept; void advanceTransport(uint32_t) noexcept; State saveState();
private:
    struct TrackNode {
        RackPathId id{};
        std::atomic<float> volume{1.0f};
        std::atomic<bool> inputArmed{false};
        std::atomic<int32_t> inputChannel{0};
        std::shared_ptr<PluginChain> chain;
        std::vector<float> sourceLeft, sourceRight, outputLeft, outputRight;
        std::vector<MidiEvent> midiScratch;
        std::atomic<uint32_t> selectedSlot{0};
        std::atomic<uint8_t> desiredQuantization{0};
        std::atomic<bool> desiredPlaying{false}, desiredLooping{false};
        std::atomic<uint64_t> commandSerial{0}, statusFrame{0};
        std::atomic<bool> statusPlaying{false}, statusLooping{false};
        std::atomic<uint32_t> punchCalibrationRemaining{0}, punchCalibrationFrames{0};
        std::atomic<float> punchNoiseSum{0.0f}, punchThreshold{0.02f};
        std::atomic<bool> recordPending{false}, recording{false}, recordComplete{false}, punchArmed{false};
        std::atomic<uint64_t> recordStartFrame{std::numeric_limits<uint64_t>::max()}, recordFrame{0};
        uint32_t recordLength = 0;
        uint8_t recordQuantization = 0;
        uint64_t appliedCommandSerial = 0, pendingLaunchFrame = std::numeric_limits<uint64_t>::max(), localFrame = 0;
        bool localPlaying = false, localLooping = false;
    };
    struct GraphSnapshot { struct TrackView { std::shared_ptr<TrackNode> node; std::shared_ptr<const WavClip> clip; std::shared_ptr<WavClip> recordingClip; std::shared_ptr<const MidiClip> midi; std::vector<std::shared_ptr<const WavClip>> wavSlots; std::vector<std::shared_ptr<const MidiClip>> midiSlots; }; std::vector<TrackView> tracks; std::shared_ptr<PluginChain> master; std::vector<float> mixLeft,mixRight; uint32_t capacity=0; };
    struct RetiredSnapshot { std::unique_ptr<GraphSnapshot> owner; RetiredSnapshot* next=nullptr; };
    struct Mailbox { std::atomic<uint64_t> sequence{0}, playSerial{0}, resetSerial{0}, bpmSerial{0}; std::atomic<bool> desiredPlaying{false}; std::atomic<double> desiredBpm{120.0}; };
    std::unique_ptr<GraphSnapshot> activeOwner_; alignas(64) std::atomic<GraphSnapshot*> activeSnapshot_{nullptr}; alignas(64) std::atomic<GraphSnapshot*> hazardSnapshot_{nullptr}; RetiredSnapshot* retired_=nullptr;
    std::thread reclaimerThread_; std::condition_variable reclaimerWake_; std::mutex reclaimerMutex_; bool reclaimerStop_=false; mutable std::mutex controlMutex_;
    uint64_t audioSamplePosition_=0, audioTransportFrame_=0, appliedPlaySerial_=0, appliedResetSerial_=0, appliedBpmSerial_=0; bool audioPlaying_=false; double audioBpm_=120;
    std::vector<std::shared_ptr<TrackNode>> tracks_; std::vector<std::shared_ptr<const WavClip>> clips_; std::vector<std::shared_ptr<WavClip>> recordingClips_; std::vector<std::shared_ptr<const MidiClip>> midiClips_; std::vector<std::vector<std::shared_ptr<const WavClip>>> wavSlots_; std::vector<std::vector<std::shared_ptr<const MidiClip>>> midiSlots_; std::shared_ptr<PluginChain> master_; RackPathId nextTrackId_=1; std::atomic<float> sampleRate_{0}; uint32_t bufferSize_=0;
    Mailbox mailbox_; std::atomic<bool> statusPlaying_{false}; std::atomic<double> statusPositionSec_{0}, statusBpm_{120}; std::atomic<uint64_t> statusSamplePosition_{0}, statusTransportFrame_{0}, statusSequence_{0};
    void writeMailboxLocked(bool, bool, bool, bool=false, double=120); std::unique_ptr<GraphSnapshot> buildSnapshotLocked(const std::vector<std::shared_ptr<TrackNode>>&, const std::vector<std::shared_ptr<const WavClip>>&, const std::vector<std::shared_ptr<WavClip>>& = {}) const; bool publishSnapshotLocked(std::unique_ptr<GraphSnapshot>); static double clipDuration(const WavClip&); void reclaimerLoop(); void reclaimRetired();
    void applyGlobalMailbox() noexcept; void publishGlobalStatus(double) noexcept; static uint64_t nextBoundary(uint64_t, double, double, LaunchQuantization) noexcept;
    std::atomic<int32_t> availableInputChannelCount_{0};
};
} // namespace guitarrackcraft
#endif
