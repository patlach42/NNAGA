#ifndef GUITARRACKCRAFT_RACK_GRAPH_H
#define GUITARRACKCRAFT_RACK_GRAPH_H

#include "PluginChain.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace guitarrackcraft {

using RackPathId = uint64_t;
constexpr RackPathId kMasterPathId = 0;

struct WavClip {
    std::vector<float> left;
    std::vector<float> right;
    uint32_t sampleRate = 0;
    std::string displayName;
};

struct TrackSnapshot {
    RackPathId id;
    float volume;
    bool inputArmed;
    bool wavLoaded;
    std::string wavDisplayName;
    double wavDurationSec;
};

struct TransportSnapshot {
    bool playing;
    bool looping;
    double positionSec;
    double durationSec;
    uint32_t loadedTrackCount;
};

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

    RackGraph();
    ~RackGraph();
    RackGraph(const RackGraph&) = delete;
    RackGraph& operator=(const RackGraph&) = delete;

    RackPathId addTrack();
    bool removeTrack(RackPathId trackId);
    std::vector<TrackSnapshot> getTracks() const;
    bool setTrackVolume(RackPathId trackId, float volume);
    bool setTrackInputArmed(RackPathId trackId, bool armed);
    bool attachTrackWav(RackPathId trackId, std::shared_ptr<const WavClip> clip);
    bool unloadTrackWav(RackPathId trackId);
    bool clearTrackWavs();

    bool setTransportPlaying(bool playing);
    bool restartTransport();
    void setTransportLooping(bool looping);
    TransportSnapshot getTransportSnapshot() const;

    std::shared_ptr<PluginChain> getChain(RackPathId pathId) const;
    void setSampleRate(float sampleRate, uint32_t bufferSize);
    void activate();
    void deactivate();
    void pauseAndResetTransport();
    void process(const float* const* liveInputs, float* const* outputs, uint32_t numFrames) noexcept;
    State saveState();

private:
    struct TrackNode {
        RackPathId id;
        std::atomic<float> volume{1.0f};
        std::atomic<bool> inputArmed{false};
        std::shared_ptr<PluginChain> chain;
        std::vector<float> sourceLeft;
        std::vector<float> sourceRight;
        std::vector<float> outputLeft;
        std::vector<float> outputRight;
    };
    struct GraphSnapshot {
        struct TrackView {
            std::shared_ptr<TrackNode> node;
            std::shared_ptr<const WavClip> clip;
        };
        std::vector<TrackView> tracks;
        std::shared_ptr<PluginChain> master;
        std::vector<float> mixLeft;
        std::vector<float> mixRight;
        uint32_t capacity = 0;
    };
    struct RetiredSnapshot {
        std::unique_ptr<GraphSnapshot> owner;
        RetiredSnapshot* next = nullptr;
    };
    struct Mailbox {
        std::atomic<uint64_t> sequence{0};
        std::atomic<uint64_t> playSerial{0};
        std::atomic<uint64_t> loopSerial{0};
        std::atomic<uint64_t> resetSerial{0};
        std::atomic<bool> desiredPlaying{false};
        std::atomic<bool> desiredLooping{false};
    };

    std::unique_ptr<GraphSnapshot> activeOwner_;
    std::atomic<GraphSnapshot*> activeSnapshot_{nullptr};
    std::atomic<GraphSnapshot*> hazardSnapshot_{nullptr};
    RetiredSnapshot* retired_ = nullptr;
    std::thread reclaimerThread_;
    std::condition_variable reclaimerWake_;
    std::mutex reclaimerMutex_;
    bool reclaimerStop_ = false;

    mutable std::mutex controlMutex_;
    std::vector<std::shared_ptr<TrackNode>> tracks_;
    std::vector<std::shared_ptr<const WavClip>> clips_;
    std::shared_ptr<PluginChain> master_;
    RackPathId nextTrackId_ = 1;
    float sampleRate_ = 0.0f;
    uint32_t bufferSize_ = 0;

    Mailbox mailbox_;
    std::atomic<bool> statusPlaying_{false};
    std::atomic<bool> statusLooping_{false};
    std::atomic<double> statusPositionSec_{0.0};
    std::atomic<double> statusDurationSec_{0.0};
    std::atomic<uint32_t> statusLoadedTrackCount_{0};

    std::unique_ptr<GraphSnapshot> buildSnapshotLocked(
        const std::vector<std::shared_ptr<TrackNode>>& nodes,
        const std::vector<std::shared_ptr<const WavClip>>& clips) const;
    bool publishSnapshotLocked(std::unique_ptr<GraphSnapshot> next, bool resetTransport);
    void reclaimerLoop();
    void reclaimRetired();
    void writeMailboxLocked(bool changePlay, bool playing, bool changeLoop, bool looping, bool reset);
    void publishStaticTransportLocked();
    static double clipDuration(const WavClip& clip);
};

} // namespace guitarrackcraft

#endif // GUITARRACKCRAFT_RACK_GRAPH_H
