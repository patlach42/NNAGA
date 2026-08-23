#ifndef GUITARRACKCRAFT_RACK_GRAPH_H
#define GUITARRACKCRAFT_RACK_GRAPH_H

#include "PluginChain.h"
#include "ClipTempoAdapter.h"

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
enum class TrackInputTap : uint8_t { PreFader = 0, PostFader = 1 };
struct TrackInputSource {
    enum class Kind : uint8_t { HardwarePair = 0, TrackOutput = 1 };
    Kind kind = Kind::HardwarePair;
    int32_t firstChannel = 0;
    RackPathId trackId = 0;
    TrackInputTap tap = TrackInputTap::PreFader;
};
enum class LaunchQuantization : uint8_t { Bar, Quarter, Eighth, Sixteenth, None };
struct WavClip {
    std::vector<float> left;
    std::vector<float> right;
    uint32_t sampleRate = 0;
    double sourceBpm = 120.0;
    std::string displayName;
};
struct MidiTimedEvent {
    uint64_t microseconds = 0;
    MidiEvent event{};
};
struct MidiClip {
    std::vector<MidiTimedEvent> events;
    uint64_t durationMicroseconds = 0;
    double sourceBpm = 120.0;
    std::string displayName;
};

struct MidiNoteInfo { uint64_t startMicroseconds{}; uint64_t durationMicroseconds{}; int32_t pitch{}; int32_t velocity{}; };
struct TrackClipSlotInfo {
    RackPathId trackId{};
    uint32_t slot{};
    bool wavLoaded{};
    bool midiLoaded{};
    std::string displayName;
    double durationSec{};
    bool active{};
    bool playing{};
    bool looping{};
    double positionSec{};
    uint64_t transportFrame{};
    double loopLengthBars{1.0};
    bool enterOnPunch{};
    double sourceBpm{};
    int tempoMode{0};
    double defaultLoopLengthBars{1.0};
    bool launchPending{};
    double musicalQuarterNotes{};
    double sampleRate{};
    uint64_t capturedAtMonotonicNanos{};
    double loopStartQuarterNotes{};
    double loopLengthQuarterNotes{1.0};
};
struct TrackSnapshot {
    RackPathId id;
    float volume;
    bool inputArmed;
    bool inputArmLocked;
    bool wavLoaded;
    std::string wavDisplayName;
    double wavDurationSec;
    bool playing;
    bool looping;
    double positionSec;
    uint64_t transportFrame;
    bool recordPending;
    uint8_t inputSourceKind{0};
    RackPathId inputSourceTrackId{0};
    uint8_t inputTap{0};
    int32_t inputSourceFirstChannel{0};
    bool recording;
    bool punchArmed;
    bool midiLoaded;
    bool midiPlaying;
    int32_t activeSlot{-1};
    uint32_t selectedSlot{};
    double defaultLoopLengthBars{1.0};
    double musicalQuarterNotes{};
    double sampleRate{};
    uint64_t capturedAtMonotonicNanos{};
};
struct TransportSnapshot { bool playing; double positionSec; double beatsPerMinute; uint64_t samplePosition; uint64_t transportFrame; double musicalQuarterNotes{0.0}; double sampleRate{0.0}; uint64_t capturedAtMonotonicNanos{0}; };

class RackGraph {
public:
    struct State {
        struct Track {
            float volume;
            bool inputArmed;
            PluginChain::ChainState chain;
        };
        std::vector<Track> tracks;
        PluginChain::ChainState master;
    };
    RackGraph(); ~RackGraph();
    RackPathId addTrack(); bool removeTrack(RackPathId); std::vector<TrackSnapshot> getTracks() const; std::vector<TrackClipSlotInfo> getTrackClipSlots(RackPathId) const; std::vector<MidiNoteInfo> getTrackClipMidiNotes(RackPathId,uint32_t) const;
    bool setTrackVolume(RackPathId, float); bool setTrackInputArmed(RackPathId, bool); bool setTrackInputArmLocked(RackPathId, bool); void setAvailableInputChannelCount(int32_t) noexcept;
    bool setTrackInputSource(RackPathId, const TrackInputSource&);
    bool setTrackInputHardwarePair(RackPathId, int32_t);
    bool setTrackInputTrack(RackPathId, RackPathId, TrackInputTap);
    TrackInputSource getTrackInputSource(RackPathId) const;
    bool setTrackInputArmedExclusive(RackPathId);
    bool attachTrackWav(RackPathId, std::shared_ptr<const WavClip>); bool unloadTrackWav(RackPathId); bool clearTrackWavs();
    bool attachTrackWavSlot(RackPathId, uint32_t, std::shared_ptr<const WavClip>); bool unloadTrackWavSlot(RackPathId, uint32_t);
    bool attachTrackMidi(RackPathId, std::shared_ptr<const MidiClip>); bool unloadTrackMidi(RackPathId);
    bool attachTrackMidiSlot(RackPathId, uint32_t, std::shared_ptr<const MidiClip>); bool unloadTrackMidiSlot(RackPathId, uint32_t); bool selectTrackClipSlot(RackPathId, uint32_t); bool renameTrackClip(RackPathId, int32_t, const std::string&);
    bool startTrackClipRecording(RackPathId, uint32_t slot, LaunchQuantization);
    bool cancelTrackLoopRecording(RackPathId);
    bool setSlotDefaultLoopLength(RackPathId, uint32_t, double);
    bool setTrackDefaultLoopLength(RackPathId, double);
    bool setClipLoopLength(RackPathId, uint32_t, double);
    bool setClipLoopStartQuarterNotes(RackPathId, uint32_t, double);
    bool setClipLoopLengthQuarterNotes(RackPathId, uint32_t, double);
    bool setClipLooping(RackPathId, uint32_t, bool);
    bool setSlotEnterOnPunch(RackPathId, uint32_t, bool, LaunchQuantization);
    bool setClipTransportPlaying(RackPathId, uint32_t, bool, LaunchQuantization);
    bool setClipTempoMode(RackPathId, uint32_t, ClipTempoMode);
    bool setClipSourceBpm(RackPathId, uint32_t, double);
    bool setTransportPlaying(bool); bool restartTransport(); void setBeatsPerMinute(double); TransportSnapshot getTransportSnapshot() const;
    std::vector<float> getTrackWaveformPeaks(RackPathId, uint32_t maxBuckets) const;
    std::shared_ptr<PluginChain> getChain(RackPathId) const;
    void setSampleRate(float, uint32_t); void activate(); void deactivate(); void pauseAndResetTransport();
    void process(const float* const*, int, float* const*, uint32_t) noexcept; void advanceTransport(uint32_t) noexcept; State saveState();
    struct SlotConfig {
        std::atomic<double> defaultLoopLengthBars{1.0};
        std::atomic<bool> enterOnPunch{false};
        std::atomic<uint8_t> punchQuantization{0};
    };
    struct ClipRuntime {
        std::atomic<double> sourceBpm{120.0};
        std::atomic<double> loopLengthBars{1.0};
        std::atomic<double> loopStartQuarterNotes{0.0};
        std::atomic<double> loopLengthQuarterNotes{4.0};
        std::atomic<bool> looping{false}, desiredPlaying{false};
        std::atomic<uint8_t> desiredQuantization{0};
        std::atomic<int> tempoMode{0};
        std::atomic<uint64_t> commandSerial{0}, statusFrame{0};
        std::atomic<double> localQuarterNotes{0.0};
        std::atomic<bool> statusPlaying{false}, restartOnLaunch{false};
        uint64_t appliedCommandSerial=0, pendingLaunchFrame=std::numeric_limits<uint64_t>::max(), localFrame=0;
        bool localPlaying=false;
        ClipRuntime() = default;
    };
    struct TrackNode {
        RackPathId id{};
        std::atomic<float> volume{1.0f};
        std::atomic<bool> inputArmed{false};
        std::atomic<bool> inputArmLocked{false};
        std::vector<std::shared_ptr<SlotConfig>> slotConfig;
        std::vector<std::shared_ptr<ClipRuntime>> clipRuntime;
        std::atomic<int32_t> activeSlot{-1};
        std::atomic<int32_t> pendingSwitchSlot{-1};
        uint64_t pendingSwitchFrame=std::numeric_limits<uint64_t>::max();
        std::atomic<uint32_t> selectedSlot{0};
        std::atomic<double> defaultLoopLengthBars{1.0};
        std::shared_ptr<PluginChain> chain;
        TrackInputSource inputSource{};
        std::vector<float> sourceLeft, sourceRight, outputLeft, outputRight;
        std::vector<MidiEvent> midiScratch;
        std::atomic<uint32_t> punchCalibrationRemaining{0}, punchCalibrationFrames{0};
        std::atomic<float> punchNoiseSum{0.0f}, punchThreshold{0.02f};
        std::atomic<bool> recordPending{false}, recording{false}, recordComplete{false}, punchArmed{false};
        uint32_t recordingSlot=std::numeric_limits<uint32_t>::max();
        std::atomic<uint64_t> recordStartFrame{std::numeric_limits<uint64_t>::max()}, recordFrame{0};
        uint32_t recordLength=0; uint8_t recordQuantization=0;
        TrackNode() = default;
    };
    struct GraphSnapshot { struct TrackView { std::shared_ptr<TrackNode> node; std::shared_ptr<const WavClip> clip; std::shared_ptr<WavClip> recordingClip; std::shared_ptr<const MidiClip> midi; std::vector<std::shared_ptr<const WavClip>> wavSlots; std::vector<std::shared_ptr<const MidiClip>> midiSlots; std::vector<std::shared_ptr<ClipRuntime>> clipRuntime; std::vector<std::shared_ptr<SlotConfig>> slotConfig; uint32_t selectedSlot{0}; uint32_t recordingSlot{std::numeric_limits<uint32_t>::max()}; uint32_t recordLength{0}; TrackInputSource inputSource{}; int32_t routeIndex{-1}; }; std::vector<TrackView> tracks; std::vector<uint32_t> topoOrder; std::shared_ptr<PluginChain> master; std::vector<float> mixLeft,mixRight; uint32_t capacity=0; };
    struct RetiredSnapshot { std::unique_ptr<GraphSnapshot> owner; RetiredSnapshot* next=nullptr; };
    struct Mailbox { std::atomic<uint64_t> sequence{0}, playSerial{0}, resetSerial{0}, bpmSerial{0}; std::atomic<bool> desiredPlaying{false}; std::atomic<double> desiredBpm{120.0}; };
    std::unique_ptr<GraphSnapshot> activeOwner_; alignas(64) std::atomic<GraphSnapshot*> activeSnapshot_{nullptr}; alignas(64) std::atomic<GraphSnapshot*> hazardSnapshot_{nullptr}; RetiredSnapshot* retired_=nullptr;
    std::thread reclaimerThread_; std::condition_variable reclaimerWake_; std::mutex reclaimerMutex_; bool reclaimerStop_=false; mutable std::mutex controlMutex_;
    uint64_t audioSamplePosition_=0, audioTransportFrame_=0, appliedPlaySerial_=0, appliedResetSerial_=0, appliedBpmSerial_=0; bool audioPlaying_=false; double audioElapsedSeconds_=0.0, audioBpm_=120; double audioMusicalQuarterNotes_=0.0;
    std::vector<std::shared_ptr<TrackNode>> tracks_; std::vector<std::shared_ptr<const WavClip>> clips_; std::vector<std::shared_ptr<WavClip>> recordingClips_; std::vector<std::shared_ptr<const MidiClip>> midiClips_; std::vector<std::vector<std::shared_ptr<const WavClip>>> wavSlots_; std::vector<std::vector<std::shared_ptr<const MidiClip>>> midiSlots_; std::vector<std::vector<std::string>> clipLabelOverrides_; std::shared_ptr<PluginChain> master_; RackPathId nextTrackId_=1; std::atomic<float> sampleRate_{0}; uint32_t bufferSize_=0;
    Mailbox mailbox_; std::atomic<bool> statusPlaying_{false}; std::atomic<double> statusPositionSec_{0}, statusBpm_{120}, statusMusicalQuarterNotes_{0}; std::atomic<uint64_t> statusSamplePosition_{0}, statusTransportFrame_{0}, statusSampleRate_{0}, statusCapturedAtNanos_{0}, statusSequence_{0};
    void writeMailboxLocked(bool, bool, bool, bool=false, double=120); std::unique_ptr<GraphSnapshot> buildSnapshotLocked(const std::vector<std::shared_ptr<TrackNode>>&, const std::vector<std::shared_ptr<const WavClip>>&, const std::vector<std::shared_ptr<WavClip>>& = {}) const; bool publishSnapshotLocked(std::unique_ptr<GraphSnapshot>); bool startTrackRecordingLocked(RackPathId, uint32_t, double, LaunchQuantization, bool); static double clipDuration(const WavClip&); void reclaimerLoop(); void reclaimRetired();
    void applyGlobalMailbox() noexcept; void publishGlobalStatus(double) noexcept; static uint64_t nextBoundary(uint64_t, double, double, LaunchQuantization) noexcept;
    std::atomic<int32_t> availableInputChannelCount_{0};
    bool ensureClipRuntimeLocked(TrackNode&, uint32_t);
    bool ensureSlotConfigLocked(TrackNode&, uint32_t);
};
} // namespace guitarrackcraft
#endif
