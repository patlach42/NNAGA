#ifndef GUITARRACKCRAFT_RACK_GRAPH_H
#define GUITARRACKCRAFT_RACK_GRAPH_H

#include "PluginChain.h"

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

enum class LaunchQuantization : uint8_t { Bar, Quarter, Eighth, Sixteenth };

struct WavClip { std::vector<float> left; std::vector<float> right; uint32_t sampleRate = 0; std::string displayName; };
struct TrackSnapshot { RackPathId id; float volume; bool inputArmed; bool wavLoaded; std::string wavDisplayName; double wavDurationSec; bool playing; bool looping; double positionSec; uint64_t transportFrame; };
struct TransportSnapshot { bool playing; double positionSec; double beatsPerMinute; uint64_t samplePosition; uint64_t transportFrame; };

class RackGraph {
public:
    struct State { struct Track { float volume; bool inputArmed; PluginChain::ChainState chain; }; std::vector<Track> tracks; PluginChain::ChainState master; };
    RackGraph(); ~RackGraph();
    RackGraph(const RackGraph&) = delete; RackGraph& operator=(const RackGraph&) = delete;
    RackPathId addTrack(); bool removeTrack(RackPathId); std::vector<TrackSnapshot> getTracks() const;
    bool setTrackVolume(RackPathId, float); bool setTrackInputArmed(RackPathId, bool);
    bool attachTrackWav(RackPathId, std::shared_ptr<const WavClip>); bool unloadTrackWav(RackPathId); bool clearTrackWavs();
    bool setTransportPlaying(bool); bool restartTransport(); void setBeatsPerMinute(double); TransportSnapshot getTransportSnapshot() const;
    bool setTrackTransportPlaying(RackPathId, bool, LaunchQuantization); bool setTrackTransportLooping(RackPathId, bool);
    std::shared_ptr<PluginChain> getChain(RackPathId) const;
    void setSampleRate(float, uint32_t); void activate(); void deactivate(); void pauseAndResetTransport();
    void process(const float* const*, float* const*, uint32_t) noexcept; void advanceTransport(uint32_t) noexcept; State saveState();
private:
    struct TrackNode {
        RackPathId id{}; std::atomic<float> volume{1.f}; std::atomic<bool> inputArmed{false}; std::shared_ptr<PluginChain> chain;
        std::vector<float> sourceLeft, sourceRight, outputLeft, outputRight;
        std::atomic<bool> desiredPlaying{false}, desiredLooping{false}; std::atomic<uint8_t> desiredQuantization{0}; std::atomic<uint64_t> commandSerial{0};
        std::atomic<bool> statusPlaying{false}, statusLooping{false}; std::atomic<uint64_t> statusFrame{0};
        uint64_t appliedCommandSerial=0, pendingLaunchFrame=std::numeric_limits<uint64_t>::max(), localFrame=0; bool localPlaying=false, localLooping=false;
    };
    struct GraphSnapshot { struct TrackView { std::shared_ptr<TrackNode> node; std::shared_ptr<const WavClip> clip; }; std::vector<TrackView> tracks; std::shared_ptr<PluginChain> master; std::vector<float> mixLeft,mixRight; uint32_t capacity=0; };
    struct RetiredSnapshot { std::unique_ptr<GraphSnapshot> owner; RetiredSnapshot* next=nullptr; };
    struct Mailbox { std::atomic<uint64_t> sequence{0}, playSerial{0}, resetSerial{0}, bpmSerial{0}; std::atomic<bool> desiredPlaying{false}; std::atomic<double> desiredBpm{120.0}; };
    std::unique_ptr<GraphSnapshot> activeOwner_; alignas(64) std::atomic<GraphSnapshot*> activeSnapshot_{nullptr}; alignas(64) std::atomic<GraphSnapshot*> hazardSnapshot_{nullptr}; RetiredSnapshot* retired_=nullptr;
    std::thread reclaimerThread_; std::condition_variable reclaimerWake_; std::mutex reclaimerMutex_; bool reclaimerStop_=false; mutable std::mutex controlMutex_;
    std::vector<std::shared_ptr<TrackNode>> tracks_; std::vector<std::shared_ptr<const WavClip>> clips_; std::shared_ptr<PluginChain> master_; RackPathId nextTrackId_=1; std::atomic<float> sampleRate_{0}; uint32_t bufferSize_=0;
    Mailbox mailbox_; std::atomic<bool> statusPlaying_{false}; std::atomic<double> statusPositionSec_{0}, statusBpm_{120}; std::atomic<uint64_t> statusSamplePosition_{0}, statusTransportFrame_{0}, statusSequence_{0};
    uint64_t audioSamplePosition_=0, audioTransportFrame_=0, appliedPlaySerial_=0, appliedResetSerial_=0, appliedBpmSerial_=0; bool audioPlaying_=false; double audioBpm_=120;
    void writeMailboxLocked(bool, bool, bool, bool=false, double=120); std::unique_ptr<GraphSnapshot> buildSnapshotLocked(const std::vector<std::shared_ptr<TrackNode>>&, const std::vector<std::shared_ptr<const WavClip>>&) const; bool publishSnapshotLocked(std::unique_ptr<GraphSnapshot>); static double clipDuration(const WavClip&); void reclaimerLoop(); void reclaimRetired();
    void applyGlobalMailbox() noexcept; void publishGlobalStatus(double) noexcept; static uint64_t nextBoundary(uint64_t, double, double, LaunchQuantization) noexcept;
};
} // namespace guitarrackcraft
#endif
