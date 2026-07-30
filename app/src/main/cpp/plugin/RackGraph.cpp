#include "RackGraph.h"

#include <chrono>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#if defined(__aarch64__)
#include <arm_neon.h>
#endif

namespace guitarrackcraft {
namespace {
constexpr uint32_t kStereoChannels = 2;

void silence(float* const* outputs, uint32_t frames) noexcept {
    if (!outputs) return;
    for (uint32_t channel = 0; channel < kStereoChannels; ++channel) {
        if (outputs[channel]) std::memset(outputs[channel], 0, sizeof(float) * frames);
    }
}

void mixScaledStereo(
        float* __restrict mixLeft, float* __restrict mixRight,
        const float* __restrict inputLeft, const float* __restrict inputRight,
        uint32_t frames, float gain) noexcept {
    uint32_t frame = 0;
#if defined(__aarch64__)
    for (; frame + 4 <= frames; frame += 4) {
        const float32x4_t left = vld1q_f32(inputLeft + frame);
        const float32x4_t right = vld1q_f32(inputRight + frame);
        vst1q_f32(mixLeft + frame,
                  vmlaq_n_f32(vld1q_f32(mixLeft + frame), left, gain));
        vst1q_f32(mixRight + frame,
                  vmlaq_n_f32(vld1q_f32(mixRight + frame), right, gain));
    }
#endif
    for (; frame < frames; ++frame) {
        mixLeft[frame] += inputLeft[frame] * gain;
        mixRight[frame] += inputRight[frame] * gain;
    }
}

void copyScaledStereo(
        float* outputLeft, float* outputRight,
        const float* inputLeft, const float* inputRight,
        uint32_t frames, float gain) noexcept {
    if (gain == 1.0f) {
        if (outputLeft != inputLeft) {
            std::memcpy(outputLeft, inputLeft, sizeof(float) * frames);
        }
        if (outputRight != inputRight) {
            std::memcpy(outputRight, inputRight, sizeof(float) * frames);
        }
        return;
    }
    uint32_t frame = 0;
#if defined(__aarch64__)
    for (; frame + 4 <= frames; frame += 4) {
        vst1q_f32(
            outputLeft + frame,
            vmulq_n_f32(vld1q_f32(inputLeft + frame), gain));
        vst1q_f32(
            outputRight + frame,
            vmulq_n_f32(vld1q_f32(inputRight + frame), gain));
    }
#endif
    for (; frame < frames; ++frame) {
        outputLeft[frame] = inputLeft[frame] * gain;
        outputRight[frame] = inputRight[frame] * gain;
    }
}
} // namespace

RackGraph::RackGraph() : master_(std::make_shared<PluginChain>()) {
    auto first = std::make_shared<TrackNode>();
    first->id = nextTrackId_++;
    first->inputArmed.store(false, std::memory_order_relaxed);
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

void RackGraph::writeMailboxLocked(bool changePlay, bool playing, bool changeLoop, bool looping,
                                    bool reset, bool changeBpm, double bpm) {
    mailbox_.sequence.fetch_add(1, std::memory_order_acq_rel);
    if (changePlay) {
        mailbox_.desiredPlaying.store(playing, std::memory_order_relaxed);
        mailbox_.playSerial.fetch_add(1, std::memory_order_relaxed);
    }
    if (changeLoop) {
        mailbox_.desiredLooping.store(looping, std::memory_order_relaxed);
        mailbox_.loopSerial.fetch_add(1, std::memory_order_relaxed);
    }
    if (changeBpm) {
        mailbox_.desiredBpm.store(std::clamp(std::isfinite(bpm) ? bpm : 120.0, 20.0, 400.0),
                                  std::memory_order_relaxed);
        mailbox_.bpmSerial.fetch_add(1, std::memory_order_relaxed);
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
    staticStatusSequence_.fetch_add(1, std::memory_order_acq_rel);
    statusDurationSec_.store(duration, std::memory_order_relaxed);
    statusLoadedTrackCount_.store(count, std::memory_order_relaxed);
    staticStatusSequence_.fetch_add(1, std::memory_order_release);
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
    writeMailboxLocked(true, playing, false, false, false);
    return true;
}

bool RackGraph::restartTransport() {
    std::lock_guard lock(controlMutex_);
    writeMailboxLocked(true, true, false, false, true);
    return true;
}

void RackGraph::setTransportLooping(bool looping) {
    std::lock_guard lock(controlMutex_);
    writeMailboxLocked(false, false, true, looping, false);
}

void RackGraph::setBeatsPerMinute(double bpm) {
    std::lock_guard lock(controlMutex_);
    writeMailboxLocked(false, false, false, false, false, true, bpm);
}

TransportSnapshot RackGraph::getTransportSnapshot() const {
    TransportSnapshot result{};
    for (;;) {
        const uint64_t before = statusSequence_.load(std::memory_order_acquire);
        if (before & 1U) continue;
        result.playing = statusPlaying_.load(std::memory_order_relaxed);
        result.looping = statusLooping_.load(std::memory_order_relaxed);
        result.positionSec = statusPositionSec_.load(std::memory_order_relaxed);
        result.beatsPerMinute = statusBpm_.load(std::memory_order_relaxed);
        result.samplePosition = statusSamplePosition_.load(std::memory_order_relaxed);
        result.transportFrame = statusTransportFrame_.load(std::memory_order_relaxed);
        if (before == statusSequence_.load(std::memory_order_acquire)) break;
    }
    for (;;) {
        const uint64_t before = staticStatusSequence_.load(std::memory_order_acquire);
        if (before & 1U) continue;
        result.durationSec = statusDurationSec_.load(std::memory_order_relaxed);
        result.loadedTrackCount = statusLoadedTrackCount_.load(std::memory_order_relaxed);
        if (before == staticStatusSequence_.load(std::memory_order_acquire)) break;
    }
    return result;
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

void RackGraph::advanceTransport(uint32_t frames) noexcept {
    const uint64_t sequence = mailbox_.sequence.load(std::memory_order_acquire);
    if ((sequence & 1U) == 0) {
        const uint64_t playSerial = mailbox_.playSerial.load(std::memory_order_relaxed);
        const uint64_t loopSerial = mailbox_.loopSerial.load(std::memory_order_relaxed);
        const uint64_t resetSerial = mailbox_.resetSerial.load(std::memory_order_relaxed);
        const uint64_t bpmSerial = mailbox_.bpmSerial.load(std::memory_order_relaxed);
        const bool desiredPlaying = mailbox_.desiredPlaying.load(std::memory_order_relaxed);
        const bool desiredLooping = mailbox_.desiredLooping.load(std::memory_order_relaxed);
        const double desiredBpm = mailbox_.desiredBpm.load(std::memory_order_relaxed);
        if (mailbox_.sequence.load(std::memory_order_acquire) == sequence) {
            if (playSerial != appliedPlaySerial_) { audioPlaying_ = desiredPlaying; appliedPlaySerial_ = playSerial; }
            if (loopSerial != appliedLoopSerial_) { audioLooping_ = desiredLooping; appliedLoopSerial_ = loopSerial; }
            if (bpmSerial != appliedBpmSerial_) { audioBpm_ = desiredBpm; appliedBpmSerial_ = bpmSerial; }
            if (resetSerial != appliedResetSerial_) { audioTransportFrame_ = 0; appliedResetSerial_ = resetSerial; }
        }
    }
    const double rate = sampleRate_.load(std::memory_order_relaxed) > 0.0f
        ? sampleRate_.load(std::memory_order_relaxed) : 48000.0;
    const double duration = statusDurationSec_.load(std::memory_order_acquire);
    const double exact = duration * rate;
    const uint64_t boundary =
        exact >= static_cast<double>(std::numeric_limits<uint64_t>::max())
            ? std::numeric_limits<uint64_t>::max()
            : static_cast<uint64_t>(std::ceil(exact));
    if (audioLooping_ && boundary > 0) {
        audioTransportFrame_ %= boundary;
    }
    audioSamplePosition_ += frames;
    if (audioPlaying_) {
        audioTransportFrame_ += frames;
        if (boundary > 0 && audioLooping_) {
            audioTransportFrame_ %= boundary;
        } else if (boundary > 0 && audioTransportFrame_ >= boundary) {
            audioTransportFrame_ = boundary;
            audioPlaying_ = false;
        }
    }
    statusSequence_.fetch_add(1, std::memory_order_acq_rel);
    statusSamplePosition_.store(audioSamplePosition_, std::memory_order_relaxed);
    statusTransportFrame_.store(audioTransportFrame_, std::memory_order_relaxed);
    statusPlaying_.store(audioPlaying_, std::memory_order_relaxed);
    statusLooping_.store(audioLooping_, std::memory_order_relaxed);
    statusBpm_.store(audioBpm_, std::memory_order_relaxed);
    statusPositionSec_.store(static_cast<double>(audioTransportFrame_) / rate, std::memory_order_relaxed);
    statusSequence_.fetch_add(1, std::memory_order_release);
}

void RackGraph::process(const float* const* liveInputs, float* const* outputs, uint32_t frames) noexcept {
    GraphSnapshot* snapshot = activeSnapshot_.load(std::memory_order_acquire);
    do {
        hazardSnapshot_.store(snapshot, std::memory_order_seq_cst);
        if (snapshot == activeSnapshot_.load(std::memory_order_acquire)) break;
        snapshot = activeSnapshot_.load(std::memory_order_acquire);
    } while (true);
    if (!snapshot || frames > snapshot->capacity || !outputs || !outputs[0] || !outputs[1]) {
        silence(outputs, frames);
        hazardSnapshot_.store(nullptr, std::memory_order_seq_cst);
        return;
    }
    const uint64_t sequence = mailbox_.sequence.load(std::memory_order_acquire);
    if ((sequence & 1U) == 0) {
        const uint64_t playSerial = mailbox_.playSerial.load(std::memory_order_relaxed);
        const uint64_t loopSerial = mailbox_.loopSerial.load(std::memory_order_relaxed);
        const uint64_t resetSerial = mailbox_.resetSerial.load(std::memory_order_relaxed);
        const uint64_t bpmSerial = mailbox_.bpmSerial.load(std::memory_order_relaxed);
        const bool desiredPlaying = mailbox_.desiredPlaying.load(std::memory_order_relaxed);
        const bool desiredLooping = mailbox_.desiredLooping.load(std::memory_order_relaxed);
        const double desiredBpm = mailbox_.desiredBpm.load(std::memory_order_relaxed);
        const uint64_t confirm = mailbox_.sequence.load(std::memory_order_acquire);
        if (confirm == sequence) {
            if (playSerial != appliedPlaySerial_) { audioPlaying_ = desiredPlaying; appliedPlaySerial_ = playSerial; }
            if (loopSerial != appliedLoopSerial_) { audioLooping_ = desiredLooping; appliedLoopSerial_ = loopSerial; }
            if (bpmSerial != appliedBpmSerial_) { audioBpm_ = desiredBpm; appliedBpmSerial_ = bpmSerial; }
            if (resetSerial != appliedResetSerial_) { audioTransportFrame_ = 0; appliedResetSerial_ = resetSerial; }
        }
    }
    const double duration = statusDurationSec_.load(std::memory_order_acquire);
    const double rate = sampleRate_.load(std::memory_order_acquire) > 0.0f
        ? sampleRate_.load(std::memory_order_relaxed) : 48000.0;
    const double durationFramesExact = duration * rate;
    const uint64_t durationFrames =
        durationFramesExact >= static_cast<double>(std::numeric_limits<uint64_t>::max())
            ? std::numeric_limits<uint64_t>::max()
            : static_cast<uint64_t>(std::ceil(durationFramesExact));
    if (audioLooping_ && durationFrames > 0) {
        audioTransportFrame_ %= durationFrames;
    }
    const double bpm = std::isfinite(audioBpm_)
        ? std::clamp(audioBpm_, 20.0, 400.0) : 120.0;
    const double beatPosition =
        static_cast<double>(audioTransportFrame_) * bpm / (rate * 60.0);
    const int64_t bar =
        static_cast<int64_t>(std::floor(beatPosition * 0.25));
    const AudioProcessContext context{
        audioSamplePosition_, audioTransportFrame_,
        audioLooping_ ? durationFrames : 0, rate, bpm,
        audioPlaying_, audioLooping_, beatPosition, bar,
        beatPosition - static_cast<double>(bar) * 4.0};
    const bool masterEmpty = snapshot->master->isEmptyForAudio();
    const bool directSingleTrack =
        masterEmpty && snapshot->tracks.size() == 1;
    bool mixHasData = false;
    for (const auto& view : snapshot->tracks) {
        auto& node = *view.node;
        float* source[2] = {node.sourceLeft.data(), node.sourceRight.data()};
        const float* chainInput[2] = {source[0], source[1]};
        if (view.clip) {
            const auto& clip = *view.clip;
            const bool equalRate = static_cast<double>(clip.sampleRate) == rate;
            if (equalRate) {
                const size_t clipFrames = clip.left.size();
                if (!audioPlaying_ || clipFrames == 0) {
                    std::memset(source[0], 0, frames * sizeof(float));
                    std::memset(source[1], 0, frames * sizeof(float));
                } else if (!audioLooping_) {
                    const uint64_t hostFrame = audioTransportFrame_;
                    if (hostFrame >= clipFrames) {
                        std::memset(source[0], 0, frames * sizeof(float));
                        std::memset(source[1], 0, frames * sizeof(float));
                    } else {
                        const size_t available = clipFrames - static_cast<size_t>(hostFrame);
                        const size_t copied = std::min<size_t>(frames, available);
                        std::memcpy(source[0], clip.left.data() + hostFrame, copied * sizeof(float));
                        if (clip.right.empty()) {
                            std::memcpy(source[1], clip.left.data() + hostFrame, copied * sizeof(float));
                        } else {
                            std::memcpy(source[1], clip.right.data() + hostFrame, copied * sizeof(float));
                        }
                        if (copied < frames) {
                            std::memset(source[0] + copied, 0, (frames - copied) * sizeof(float));
                            std::memset(source[1] + copied, 0, (frames - copied) * sizeof(float));
                        }
                    }
                } else {
                    size_t destination = 0;
                    const size_t start = static_cast<size_t>(audioTransportFrame_ % clipFrames);
                    size_t sourceFrame = start;
                    while (destination < frames) {
                        const size_t run = std::min<size_t>(frames - destination, clipFrames - sourceFrame);
                        std::memcpy(source[0] + destination, clip.left.data() + sourceFrame, run * sizeof(float));
                        if (clip.right.empty()) {
                            std::memcpy(source[1] + destination, clip.left.data() + sourceFrame, run * sizeof(float));
                        } else {
                            std::memcpy(source[1] + destination, clip.right.data() + sourceFrame, run * sizeof(float));
                        }
                        destination += run;
                        sourceFrame = 0;
                    }
                }
            } else {
                const double step = static_cast<double>(clip.sampleRate) / rate;
                const double clipFrames = static_cast<double>(clip.left.size());
                const double exactBoundary = (clipFrames / static_cast<double>(clip.sampleRate)) * rate;
                const uint64_t hostBoundary = exactBoundary >= static_cast<double>(std::numeric_limits<uint64_t>::max())
                    ? std::numeric_limits<uint64_t>::max() : static_cast<uint64_t>(std::ceil(exactBoundary));
                for (uint32_t frame = 0; frame < frames; ++frame) {
                    if (!audioPlaying_) { source[0][frame] = source[1][frame] = 0.0f; continue; }
                    const uint64_t hostFrame = audioTransportFrame_ + frame;
                    if (hostBoundary == 0 || (!audioLooping_ && hostFrame >= hostBoundary)) {
                        source[0][frame] = source[1][frame] = 0.0f; continue;
                    }
                    const double sampleFrame = static_cast<double>(audioLooping_ ? (hostFrame % hostBoundary) : hostFrame) * step;
                    if (sampleFrame >= clipFrames) {
                        source[0][frame] = source[1][frame] = 0.0f;
                        continue;
                    }
                    const size_t first = static_cast<size_t>(sampleFrame);
                    const size_t second = std::min(first + 1, clip.left.size() - 1);
                    const float fraction = static_cast<float>(sampleFrame - first);
                    source[0][frame] = clip.left[first] + (clip.left[second] - clip.left[first]) * fraction;
                    source[1][frame] = clip.right.empty() ? source[0][frame] : clip.right[first] + (clip.right[second] - clip.right[first]) * fraction;
                }
            }
        } else if (node.inputArmed.load(std::memory_order_acquire) &&
                   liveInputs && liveInputs[0] && liveInputs[1]) {
            chainInput[0] = liveInputs[0];
            chainInput[1] = liveInputs[1];
        } else {
            std::memset(source[0], 0, frames * sizeof(float));
            std::memset(source[1], 0, frames * sizeof(float));
        }
        const bool trackChainEmpty = node.chain->isEmptyForAudio();
        const float* trackSignal[2] = {chainInput[0], chainInput[1]};
        if (!trackChainEmpty) {
            float* trackOutput[2] = {
                directSingleTrack ? outputs[0] : node.outputLeft.data(),
                directSingleTrack ? outputs[1] : node.outputRight.data()};
            node.chain->process(chainInput, trackOutput, frames, context);
            trackSignal[0] = trackOutput[0];
            trackSignal[1] = trackOutput[1];
        }
        const float volume = node.volume.load(std::memory_order_acquire);
        if (directSingleTrack) {
            copyScaledStereo(
                outputs[0], outputs[1],
                trackSignal[0], trackSignal[1], frames, volume);
            continue;
        }
        if (!mixHasData) {
            copyScaledStereo(
                snapshot->mixLeft.data(), snapshot->mixRight.data(),
                trackSignal[0], trackSignal[1], frames, volume);
            mixHasData = true;
        } else {
            mixScaledStereo(
                snapshot->mixLeft.data(), snapshot->mixRight.data(),
                trackSignal[0], trackSignal[1], frames, volume);
        }
    }
    if (!directSingleTrack) {
        if (!mixHasData) {
            std::memset(
                snapshot->mixLeft.data(), 0, frames * sizeof(float));
            std::memset(
                snapshot->mixRight.data(), 0, frames * sizeof(float));
        }
        const float* mix[2] = {
            snapshot->mixLeft.data(), snapshot->mixRight.data()};
        if (masterEmpty) {
            copyScaledStereo(
                outputs[0], outputs[1], mix[0], mix[1], frames, 1.0f);
        } else {
            snapshot->master->process(mix, outputs, frames, context);
        }
    }
    audioSamplePosition_ += frames;
    if (audioPlaying_) {
        audioTransportFrame_ += frames;
        if (durationFrames > 0 && !audioLooping_ &&
            audioTransportFrame_ >= durationFrames) {
            audioTransportFrame_ = durationFrames;
            audioPlaying_ = false;
        } else if (durationFrames > 0 && audioLooping_) {
            audioTransportFrame_ %= durationFrames;
        }
    }
    statusSequence_.fetch_add(1, std::memory_order_acq_rel);
    statusSamplePosition_.store(audioSamplePosition_, std::memory_order_relaxed);
    statusTransportFrame_.store(audioTransportFrame_, std::memory_order_relaxed);
    statusPlaying_.store(audioPlaying_, std::memory_order_relaxed);
    statusLooping_.store(audioLooping_, std::memory_order_relaxed);
    statusBpm_.store(audioBpm_, std::memory_order_relaxed);
    statusPositionSec_.store(static_cast<double>(audioTransportFrame_) / rate, std::memory_order_relaxed);
    statusSequence_.fetch_add(1, std::memory_order_release);
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
