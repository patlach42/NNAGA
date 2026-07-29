#include "RackGraph.h"

#include <chrono>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace guitarrackcraft {
namespace {
constexpr uint32_t kStereoChannels = 2;

void silence(float* const* outputs, uint32_t frames) noexcept {
    if (!outputs) return;
    for (uint32_t channel = 0; channel < kStereoChannels; ++channel) {
        if (outputs[channel]) std::memset(outputs[channel], 0, sizeof(float) * frames);
    }
}
} // namespace

RackGraph::RackGraph() : master_(std::make_shared<PluginChain>()) {
    auto first = std::make_shared<TrackNode>();
    first->id = nextTrackId_++;
    first->inputArmed.store(true, std::memory_order_relaxed);
    first->chain = std::make_shared<PluginChain>();
    tracks_.push_back(first);
    clips_.push_back(nullptr);
    activeOwner_ = buildSnapshotLocked(tracks_, clips_);
    activeSnapshot_.store(activeOwner_.get(), std::memory_order_release);
    publishStaticTransportLocked();
    reclaimerThread_ = std::thread(&RackGraph::reclaimerLoop, this);
}

RackGraph::~RackGraph() {
    hazardSnapshot_.store(nullptr, std::memory_order_seq_cst);
    {
        std::lock_guard lock(reclaimerMutex_);
        reclaimerStop_ = true;
    }
    reclaimerWake_.notify_one();
    if (reclaimerThread_.joinable()) reclaimerThread_.join();
    reclaimRetired();
}

double RackGraph::clipDuration(const WavClip& clip) {
    if (clip.sampleRate == 0 || clip.left.empty()) return 0.0;
    return static_cast<double>(clip.left.size()) / clip.sampleRate;
}

std::unique_ptr<RackGraph::GraphSnapshot> RackGraph::buildSnapshotLocked(
    const std::vector<std::shared_ptr<TrackNode>>& nodes,
    const std::vector<std::shared_ptr<const WavClip>>& clips) const {
    if (nodes.size() != clips.size()) return nullptr;
    auto snapshot = std::make_unique<GraphSnapshot>();
    snapshot->master = master_;
    snapshot->capacity = bufferSize_;
    snapshot->mixLeft.resize(bufferSize_);
    snapshot->mixRight.resize(bufferSize_);
    snapshot->tracks.reserve(nodes.size());
    for (size_t index = 0; index < nodes.size(); ++index) {
        if (!nodes[index] || !nodes[index]->chain) return nullptr;
        if (nodes[index]->sourceLeft.size() < bufferSize_ ||
            nodes[index]->sourceRight.size() < bufferSize_ ||
            nodes[index]->outputLeft.size() < bufferSize_ ||
            nodes[index]->outputRight.size() < bufferSize_) return nullptr;
        snapshot->tracks.push_back({nodes[index], clips[index]});
    }
    return snapshot;
}

void RackGraph::writeMailboxLocked(bool changePlay, bool playing, bool changeLoop, bool looping, bool reset) {
    mailbox_.sequence.fetch_add(1, std::memory_order_acq_rel);
    if (changePlay) {
        mailbox_.desiredPlaying.store(playing, std::memory_order_relaxed);
        mailbox_.playSerial.fetch_add(1, std::memory_order_relaxed);
    }
    if (changeLoop) {
        mailbox_.desiredLooping.store(looping, std::memory_order_relaxed);
        mailbox_.loopSerial.fetch_add(1, std::memory_order_relaxed);
    }
    if (reset) mailbox_.resetSerial.fetch_add(1, std::memory_order_relaxed);
    mailbox_.sequence.fetch_add(1, std::memory_order_release);
}

void RackGraph::publishStaticTransportLocked() {
    double duration = 0.0;
    uint32_t count = 0;
    if (activeOwner_) {
        for (const auto& view : activeOwner_->tracks) {
            if (view.clip) {
                ++count;
                duration = std::max(duration, clipDuration(*view.clip));
            }
        }
    }
    statusDurationSec_.store(duration, std::memory_order_release);
    statusLoadedTrackCount_.store(count, std::memory_order_release);
    if (count == 0) {
        statusPlaying_.store(false, std::memory_order_release);
        statusPositionSec_.store(0.0, std::memory_order_release);
    }
}

bool RackGraph::publishSnapshotLocked(std::unique_ptr<GraphSnapshot> next, bool resetTransport) {
    if (!next) return false;
    std::unique_ptr<RetiredSnapshot> retired;
    if (activeOwner_) {
        retired = std::make_unique<RetiredSnapshot>();
        retired->owner = std::move(activeOwner_);
    }
    if (resetTransport) writeMailboxLocked(true, false, false, false, true);
    GraphSnapshot* nextRaw = next.get();
    activeOwner_ = std::move(next);
    activeSnapshot_.exchange(nextRaw, std::memory_order_release);
    if (retired) {
        {
            std::lock_guard reclaimLock(reclaimerMutex_);
            retired->next = retired_;
            retired_ = retired.release();
        }
        reclaimerWake_.notify_one();
    }
    publishStaticTransportLocked();
    return true;
}

RackPathId RackGraph::addTrack() {
    std::lock_guard lock(controlMutex_);
    try {
        auto node = std::make_shared<TrackNode>();
        node->id = nextTrackId_;
        node->chain = std::make_shared<PluginChain>();
        node->sourceLeft.resize(bufferSize_);
        node->sourceRight.resize(bufferSize_);
        node->outputLeft.resize(bufferSize_);
        node->outputRight.resize(bufferSize_);
        if (sampleRate_.load(std::memory_order_acquire) > 0.0f) {
            node->chain->setSampleRate(sampleRate_.load(std::memory_order_acquire), bufferSize_);
        }
        auto nodes = tracks_;
        auto clips = clips_;
        nodes.push_back(node);
        clips.push_back(nullptr);
        auto next = buildSnapshotLocked(nodes, clips);
        if (!publishSnapshotLocked(std::move(next), false)) return 0;
        tracks_ = std::move(nodes);
        clips_ = std::move(clips);
        return nextTrackId_++;
    } catch (const std::exception&) {
        return 0;
    }
}

bool RackGraph::removeTrack(RackPathId trackId) {
    std::lock_guard lock(controlMutex_);
    if (trackId == kMasterPathId || tracks_.size() <= 1) return false;
    auto it = std::find_if(tracks_.begin(), tracks_.end(), [trackId](const auto& node) { return node->id == trackId; });
    if (it == tracks_.end()) return false;
    try {
        const size_t index = static_cast<size_t>(std::distance(tracks_.begin(), it));
        auto nodes = tracks_;
        auto clips = clips_;
        const bool reset = static_cast<bool>(clips[index]);
        nodes.erase(nodes.begin() + index);
        clips.erase(clips.begin() + index);
        auto next = buildSnapshotLocked(nodes, clips);
        if (!publishSnapshotLocked(std::move(next), reset)) return false;
        tracks_ = std::move(nodes);
        clips_ = std::move(clips);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

std::vector<TrackSnapshot> RackGraph::getTracks() const {
    std::lock_guard lock(controlMutex_);
    std::vector<TrackSnapshot> result;
    result.reserve(tracks_.size());
    for (size_t index = 0; index < tracks_.size(); ++index) {
        const auto& clip = clips_[index];
        result.push_back({tracks_[index]->id, tracks_[index]->volume.load(std::memory_order_relaxed),
            tracks_[index]->inputArmed.load(std::memory_order_relaxed), static_cast<bool>(clip),
            clip ? clip->displayName : std::string(), clip ? clipDuration(*clip) : 0.0});
    }
    return result;
}

bool RackGraph::setTrackVolume(RackPathId trackId, float volume) {
    std::lock_guard lock(controlMutex_);
    auto it = std::find_if(tracks_.begin(), tracks_.end(), [trackId](const auto& node) { return node->id == trackId; });
    if (it == tracks_.end()) return false;
    (*it)->volume.store(std::clamp(volume, 0.0f, 1.0f), std::memory_order_release);
    return true;
}

bool RackGraph::setTrackInputArmed(RackPathId trackId, bool armed) {
    std::lock_guard lock(controlMutex_);
    auto it = std::find_if(tracks_.begin(), tracks_.end(), [trackId](const auto& node) { return node->id == trackId; });
    if (it == tracks_.end()) return false;
    (*it)->inputArmed.store(armed, std::memory_order_release);
    return true;
}

bool RackGraph::attachTrackWav(RackPathId trackId, std::shared_ptr<const WavClip> clip) {
    if (!clip || clip->left.empty() || clip->sampleRate == 0 || (!clip->right.empty() && clip->right.size() != clip->left.size())) return false;
    std::lock_guard lock(controlMutex_);
    auto it = std::find_if(tracks_.begin(), tracks_.end(), [trackId](const auto& node) { return node->id == trackId; });
    if (it == tracks_.end()) return false;
    try {
        auto newClips = clips_;
        newClips[static_cast<size_t>(std::distance(tracks_.begin(), it))] = std::move(clip);
        auto next = buildSnapshotLocked(tracks_, newClips);
        if (!publishSnapshotLocked(std::move(next), true)) return false;
        clips_ = std::move(newClips);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

bool RackGraph::unloadTrackWav(RackPathId trackId) {
    std::lock_guard lock(controlMutex_);
    auto it = std::find_if(tracks_.begin(), tracks_.end(), [trackId](const auto& node) { return node->id == trackId; });
    if (it == tracks_.end()) return false;
    const size_t index = static_cast<size_t>(std::distance(tracks_.begin(), it));
    if (!clips_[index]) return true;
    try {
        auto newClips = clips_;
        newClips[index].reset();
        auto next = buildSnapshotLocked(tracks_, newClips);
        if (!publishSnapshotLocked(std::move(next), true)) return false;
        clips_ = std::move(newClips);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

bool RackGraph::clearTrackWavs() {
    std::lock_guard lock(controlMutex_);
    if (std::none_of(clips_.begin(), clips_.end(), [](const auto& clip) { return static_cast<bool>(clip); })) return true;
    try {
        auto newClips = clips_;
        for (auto& clip : newClips) clip.reset();
        auto next = buildSnapshotLocked(tracks_, newClips);
        if (!publishSnapshotLocked(std::move(next), true)) return false;
        clips_ = std::move(newClips);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

bool RackGraph::setTransportPlaying(bool playing) {
    std::lock_guard lock(controlMutex_);
    if (playing && statusLoadedTrackCount_.load(std::memory_order_acquire) == 0) return false;
    writeMailboxLocked(true, playing, false, false, false);
    return true;
}

bool RackGraph::restartTransport() {
    std::lock_guard lock(controlMutex_);
    if (statusLoadedTrackCount_.load(std::memory_order_acquire) == 0) return false;
    writeMailboxLocked(true, true, false, false, true);
    return true;
}

void RackGraph::setTransportLooping(bool looping) {
    std::lock_guard lock(controlMutex_);
    writeMailboxLocked(false, false, true, looping, false);
}

TransportSnapshot RackGraph::getTransportSnapshot() const {
    return {statusPlaying_.load(std::memory_order_acquire), statusLooping_.load(std::memory_order_acquire),
        statusPositionSec_.load(std::memory_order_acquire), statusDurationSec_.load(std::memory_order_acquire),
        statusLoadedTrackCount_.load(std::memory_order_acquire)};
}

std::shared_ptr<PluginChain> RackGraph::getChain(RackPathId pathId) const {
    std::lock_guard lock(controlMutex_);
    if (pathId == kMasterPathId) return master_;
    auto it = std::find_if(tracks_.begin(), tracks_.end(), [pathId](const auto& node) { return node->id == pathId; });
    return it == tracks_.end() ? nullptr : (*it)->chain;
}

void RackGraph::setSampleRate(float sampleRate, uint32_t bufferSize) {
    std::lock_guard lock(controlMutex_);
    sampleRate_.store(sampleRate, std::memory_order_release);
    bufferSize_ = bufferSize;
    for (const auto& node : tracks_) {
        node->sourceLeft.resize(bufferSize);
        node->sourceRight.resize(bufferSize);
        node->outputLeft.resize(bufferSize);
        node->outputRight.resize(bufferSize);
        node->chain->setSampleRate(sampleRate, bufferSize);
    }
    master_->setSampleRate(sampleRate, bufferSize);
    try {
        auto next = buildSnapshotLocked(tracks_, clips_);
        (void)publishSnapshotLocked(std::move(next), false);
    } catch (const std::exception&) {}
}

void RackGraph::activate() { std::lock_guard lock(controlMutex_); for (const auto& node : tracks_) node->chain->activate(); master_->activate(); }
void RackGraph::deactivate() { std::lock_guard lock(controlMutex_); for (const auto& node : tracks_) node->chain->deactivate(); master_->deactivate(); }
void RackGraph::pauseAndResetTransport() { std::lock_guard lock(controlMutex_); writeMailboxLocked(true, false, false, false, true); }

RackGraph::State RackGraph::saveState() {
    std::lock_guard lock(controlMutex_);
    State state;
    state.tracks.reserve(tracks_.size());
    for (const auto& node : tracks_) state.tracks.push_back({node->volume.load(), node->inputArmed.load(), node->chain->saveChainState()});
    state.master = master_->saveChainState();
    return state;
}

void RackGraph::process(const float* const* liveInputs, float* const* outputs, uint32_t frames) noexcept {
    GraphSnapshot* snapshot = activeSnapshot_.load(std::memory_order_acquire);
    do {
        hazardSnapshot_.store(snapshot, std::memory_order_seq_cst);
        if (snapshot == activeSnapshot_.load(std::memory_order_acquire)) break;
        snapshot = activeSnapshot_.load(std::memory_order_acquire);
    } while (true);
    if (!snapshot || frames > snapshot->capacity || !outputs || !outputs[0] || !outputs[1]) { silence(outputs, frames); hazardSnapshot_.store(nullptr, std::memory_order_seq_cst); return; }

    uint64_t sequence = mailbox_.sequence.load(std::memory_order_acquire);
    uint64_t playSerial = 0, loopSerial = 0, resetSerial = 0;
    bool desiredPlaying = false, desiredLooping = false;
    if ((sequence & 1U) == 0) {
        playSerial = mailbox_.playSerial.load(std::memory_order_relaxed);
        loopSerial = mailbox_.loopSerial.load(std::memory_order_relaxed);
        resetSerial = mailbox_.resetSerial.load(std::memory_order_relaxed);
        desiredPlaying = mailbox_.desiredPlaying.load(std::memory_order_relaxed);
        desiredLooping = mailbox_.desiredLooping.load(std::memory_order_relaxed);
        if (sequence != mailbox_.sequence.load(std::memory_order_acquire)) sequence = 1;
    }
    static thread_local uint64_t appliedPlay = 0, appliedLoop = 0, appliedReset = 0;
    static thread_local bool playing = false, looping = false;
    static thread_local double position = 0.0;
    if ((sequence & 1U) == 0) {
        if (playSerial != appliedPlay) { playing = desiredPlaying; appliedPlay = playSerial; }
        if (loopSerial != appliedLoop) { looping = desiredLooping; appliedLoop = loopSerial; }
        if (resetSerial != appliedReset) { position = 0.0; appliedReset = resetSerial; }
    }
    const double duration = statusDurationSec_.load(std::memory_order_acquire);
    std::fill(snapshot->mixLeft.begin(), snapshot->mixLeft.begin() + frames, 0.0f);
    std::fill(snapshot->mixRight.begin(), snapshot->mixRight.begin() + frames, 0.0f);
    const float configuredRate = sampleRate_.load(std::memory_order_acquire);
    const double rate = configuredRate > 0.0f ? configuredRate : 48000.0;
    for (const auto& view : snapshot->tracks) {
        auto& node = *view.node;
        float* source[2] = {node.sourceLeft.data(), node.sourceRight.data()};
        if (view.clip) {
            const auto& clip = *view.clip;
            for (uint32_t frame = 0; frame < frames; ++frame) {
                double p = position + static_cast<double>(frame) / rate;
                if (!playing) {
                    source[0][frame] = source[1][frame] = 0.0f;
                    continue;
                }
                if (p >= clipDuration(clip)) {
                    if (looping && duration > 0.0) {
                        p = std::fmod(p, duration);
                    } else {
                        source[0][frame] = source[1][frame] = 0.0f;
                        continue;
                    }
                }
                const double clipFrame = p * clip.sampleRate;
                const size_t first = static_cast<size_t>(clipFrame);
                if (first >= clip.left.size()) {
                    source[0][frame] = source[1][frame] = 0.0f;
                    continue;
                }
                const size_t second = std::min(first + 1, clip.left.size() - 1);
                const float fraction = static_cast<float>(clipFrame - first);
                source[0][frame] = clip.left[first] + (clip.left[second] - clip.left[first]) * fraction;
                source[1][frame] = clip.right.empty() ? source[0][frame] :
                    clip.right[first] + (clip.right[second] - clip.right[first]) * fraction;
            }
        } else if (node.inputArmed.load(std::memory_order_acquire) && liveInputs &&
                   liveInputs[0] && liveInputs[1]) {
            std::memcpy(source[0], liveInputs[0], frames * sizeof(float));
            std::memcpy(source[1], liveInputs[1], frames * sizeof(float));
        } else {
            std::memset(source[0], 0, frames * sizeof(float));
            std::memset(source[1], 0, frames * sizeof(float));
        }
        float* trackOutput[2] = {node.outputLeft.data(), node.outputRight.data()};
        node.chain->process(source, trackOutput, frames);
        const float volume = node.volume.load(std::memory_order_acquire);
        for (uint32_t frame = 0; frame < frames; ++frame) { snapshot->mixLeft[frame] += trackOutput[0][frame] * volume; snapshot->mixRight[frame] += trackOutput[1][frame] * volume; }
    }
    const float* mix[2] = {snapshot->mixLeft.data(), snapshot->mixRight.data()};
    snapshot->master->process(mix, outputs, frames);
    if (playing && duration > 0.0) { position += static_cast<double>(frames) / rate; if (position >= duration) { if (looping) position = std::fmod(position, duration); else { position = duration; playing = false; } } }
    statusPlaying_.store(playing, std::memory_order_release);
    statusLooping_.store(looping, std::memory_order_release);
    statusPositionSec_.store(position, std::memory_order_release);
    hazardSnapshot_.store(nullptr, std::memory_order_seq_cst);
}

void RackGraph::reclaimerLoop() {
    std::unique_lock lock(reclaimerMutex_);
    while (!reclaimerStop_) { reclaimerWake_.wait_for(lock, std::chrono::milliseconds(10)); lock.unlock(); reclaimRetired(); lock.lock(); }
}

void RackGraph::reclaimRetired() {
    std::lock_guard lock(reclaimerMutex_);
    const GraphSnapshot* hazard = hazardSnapshot_.load(std::memory_order_seq_cst);
    RetiredSnapshot** cursor = &retired_;
    while (*cursor) {
        RetiredSnapshot* item = *cursor;
        if (item->owner.get() != hazard) {
            *cursor = item->next;
            delete item;
        } else {
            cursor = &item->next;
        }
    }
}

} // namespace guitarrackcraft
