#include "RackGraph.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

namespace guitarrackcraft {
namespace {
constexpr uint32_t kChannels = 2;
void silence(float* const* out, uint32_t frames) noexcept { if (!out) return; for (uint32_t c=0;c<kChannels;++c) if (out[c]) std::memset(out[c], 0, frames*sizeof(float)); }
void copyScaled(float* l,float* r,const float* inL,const float* inR,uint32_t n,float gain) noexcept { for(uint32_t i=0;i<n;++i){l[i]=inL[i]*gain;r[i]=inR[i]*gain;} }
void mixScaled(float* l,float* r,const float* inL,const float* inR,uint32_t n,float gain) noexcept { for(uint32_t i=0;i<n;++i){l[i]+=inL[i]*gain;r[i]+=inR[i]*gain;} }
}

RackGraph::RackGraph() : master_(std::make_shared<PluginChain>()) {
    auto first=std::make_shared<TrackNode>(); first->id=nextTrackId_++; first->chain=std::make_shared<PluginChain>(); tracks_.push_back(first); clips_.push_back(nullptr); recordingClips_.push_back(nullptr);
    activeOwner_=buildSnapshotLocked(tracks_, clips_, recordingClips_); activeSnapshot_.store(activeOwner_.get(),std::memory_order_release); reclaimerThread_=std::thread(&RackGraph::reclaimerLoop,this);
}
RackGraph::~RackGraph(){ hazardSnapshot_.store(nullptr,std::memory_order_seq_cst); {std::lock_guard lock(reclaimerMutex_);reclaimerStop_=true;} reclaimerWake_.notify_one(); if(reclaimerThread_.joinable()) reclaimerThread_.join(); reclaimRetired(); }
double RackGraph::clipDuration(const WavClip& clip){return clip.sampleRate && !clip.left.empty()?static_cast<double>(clip.left.size())/clip.sampleRate:0.;}
std::unique_ptr<RackGraph::GraphSnapshot> RackGraph::buildSnapshotLocked(const std::vector<std::shared_ptr<TrackNode>>& nodes,const std::vector<std::shared_ptr<const WavClip>>& clips,const std::vector<std::shared_ptr<WavClip>>& recordings) const {
    if(nodes.size()!=clips.size()) return nullptr; auto snapshot=std::make_unique<GraphSnapshot>(); snapshot->master=master_; snapshot->capacity=bufferSize_; snapshot->mixLeft.resize(bufferSize_); snapshot->mixRight.resize(bufferSize_); snapshot->tracks.reserve(nodes.size());
    for(size_t i=0;i<nodes.size();++i){ if(!nodes[i]||!nodes[i]->chain||nodes[i]->sourceLeft.size()<bufferSize_||nodes[i]->sourceRight.size()<bufferSize_||nodes[i]->outputLeft.size()<bufferSize_||nodes[i]->outputRight.size()<bufferSize_) return nullptr; snapshot->tracks.push_back({nodes[i],clips[i],recordings.size()==nodes.size()?recordings[i]:nullptr}); }
    return snapshot;
}
bool RackGraph::publishSnapshotLocked(std::unique_ptr<GraphSnapshot> next){ if(!next)return false; std::unique_ptr<RetiredSnapshot> retired; if(activeOwner_){retired=std::make_unique<RetiredSnapshot>();retired->owner=std::move(activeOwner_);} auto* raw=next.get(); activeOwner_=std::move(next); activeSnapshot_.exchange(raw,std::memory_order_release); if(retired){std::lock_guard lock(reclaimerMutex_);retired->next=retired_;retired_=retired.release();reclaimerWake_.notify_one();} return true; }
RackPathId RackGraph::addTrack(){std::lock_guard lock(controlMutex_);try{auto node=std::make_shared<TrackNode>();node->id=nextTrackId_;node->chain=std::make_shared<PluginChain>();node->sourceLeft.resize(bufferSize_);node->sourceRight.resize(bufferSize_);node->outputLeft.resize(bufferSize_);node->outputRight.resize(bufferSize_);if(sampleRate_.load()>0)node->chain->setSampleRate(sampleRate_.load(),bufferSize_);auto nodes=tracks_;auto clips=clips_;auto recs=recordingClips_;nodes.push_back(node);clips.push_back(nullptr);recs.push_back(nullptr);if(!publishSnapshotLocked(buildSnapshotLocked(nodes,clips,recs)))return 0;tracks_=std::move(nodes);clips_=std::move(clips);recordingClips_=std::move(recs);return nextTrackId_++;}catch(...){return 0;}}
bool RackGraph::removeTrack(RackPathId id){std::lock_guard lock(controlMutex_);if(id==kMasterPathId||tracks_.size()<=1)return false;auto it=std::find_if(tracks_.begin(),tracks_.end(),[&](auto& n){return n->id==id;});if(it==tracks_.end())return false;auto nodes=tracks_;auto clips=clips_;auto recs=recordingClips_;auto i=static_cast<size_t>(it-tracks_.begin());nodes.erase(nodes.begin()+i);clips.erase(clips.begin()+i);recs.erase(recs.begin()+i);if(!publishSnapshotLocked(buildSnapshotLocked(nodes,clips,recs)))return false;tracks_=std::move(nodes);clips_=std::move(clips);recordingClips_=std::move(recs);return true;}
std::vector<TrackSnapshot> RackGraph::getTracks() const {std::lock_guard lock(controlMutex_);std::vector<TrackSnapshot> result;result.reserve(tracks_.size());auto rate=sampleRate_.load();if(rate<=0)rate=48000;for(size_t i=0;i<tracks_.size();++i){auto& n=*tracks_[i];auto& c=clips_[i];auto frame=n.statusFrame.load();bool loaded=static_cast<bool>(c)||n.recordComplete.load();result.push_back({n.id,n.volume.load(),n.inputArmed.load(),loaded,c?c->displayName:(recordingClips_[i]?recordingClips_[i]->displayName:std::string()),c?clipDuration(*c):(recordingClips_[i]?clipDuration(*recordingClips_[i]):0.),n.statusPlaying.load(),n.statusLooping.load(),static_cast<double>(frame)/rate,frame,n.recordPending.load(),n.recording.load(),n.punchArmed.load(),n.inputChannel.load()});}return result;}
bool RackGraph::setTrackVolume(RackPathId id,float value){std::lock_guard lock(controlMutex_);auto it=std::find_if(tracks_.begin(),tracks_.end(),[&](auto& n){return n->id==id;});if(it==tracks_.end())return false;(*it)->volume.store(std::clamp(value,0.f,1.f));return true;}
bool RackGraph::setTrackInputArmed(RackPathId id,bool armed){std::lock_guard lock(controlMutex_);auto it=std::find_if(tracks_.begin(),tracks_.end(),[&](auto& n){return n->id==id;});if(it==tracks_.end())return false;(*it)->inputArmed.store(armed);return true;}
bool RackGraph::setTrackInputChannel(RackPathId id, int32_t channel) {
    constexpr int32_t kMaxInputChannels = 8;
    std::lock_guard lock(controlMutex_);
    auto it = std::find_if(tracks_.begin(), tracks_.end(),
                           [&](const auto& node) { return node->id == id; });
    const int32_t count = availableInputChannelCount_.load(std::memory_order_acquire);
    if (it == tracks_.end() || channel < 0 || channel >= kMaxInputChannels ||
        (count > 0 && channel >= count)) return false;
    (*it)->inputChannel.store(channel, std::memory_order_release);
    return true;
}
void RackGraph::setAvailableInputChannelCount(int32_t count) noexcept {
    count = std::max<int32_t>(0, count);
    std::lock_guard lock(controlMutex_);
    availableInputChannelCount_.store(count, std::memory_order_release);
    for (const auto& node : tracks_) {
        const int32_t channel = node->inputChannel.load(std::memory_order_relaxed);
        node->inputChannel.store(count > 0 ? std::min(channel, count - 1) : 0,
                                 std::memory_order_relaxed);
    }
}
bool RackGraph::attachTrackWav(RackPathId id,std::shared_ptr<const WavClip> clip){if(!clip||clip->left.empty()||!clip->sampleRate||(!clip->right.empty()&&clip->right.size()!=clip->left.size()))return false;std::lock_guard lock(controlMutex_);auto it=std::find_if(tracks_.begin(),tracks_.end(),[&](auto& n){return n->id==id;});if(it==tracks_.end())return false;auto copy=clips_;copy[it-tracks_.begin()]=std::move(clip);auto recs=recordingClips_;recs[it-tracks_.begin()].reset();if(!publishSnapshotLocked(buildSnapshotLocked(tracks_,copy,recs)))return false;clips_=std::move(copy);recordingClips_=std::move(recs);(*it)->recordPending.store(false);(*it)->recording.store(false);(*it)->punchArmed.store(false);(*it)->recordComplete.store(false);return true;}
bool RackGraph::unloadTrackWav(RackPathId id){std::lock_guard lock(controlMutex_);auto it=std::find_if(tracks_.begin(),tracks_.end(),[&](auto& n){return n->id==id;});if(it==tracks_.end())return false;auto copy=clips_;auto recs=recordingClips_;auto i=static_cast<size_t>(it-tracks_.begin());copy[i].reset();recs[i].reset();(*it)->recordPending.store(false);(*it)->recording.store(false);(*it)->punchArmed.store(false);(*it)->recordComplete.store(false);if(!publishSnapshotLocked(buildSnapshotLocked(tracks_,copy,recs)))return false;clips_=std::move(copy);recordingClips_=std::move(recs);return true;}
bool RackGraph::clearTrackWavs(){std::lock_guard lock(controlMutex_);auto copy=clips_;auto recs=recordingClips_;for(size_t i=0;i<copy.size();++i){copy[i].reset();recs[i].reset();tracks_[i]->recordPending.store(false);tracks_[i]->recording.store(false);tracks_[i]->punchArmed.store(false);tracks_[i]->recordComplete.store(false);}if(!publishSnapshotLocked(buildSnapshotLocked(tracks_,copy,recs)))return false;clips_=std::move(copy);recordingClips_=std::move(recs);return true;}
bool RackGraph::startTrackLoopRecording(RackPathId id,double bars,LaunchQuantization q,bool enterOnPunch){if(!(bars==.25||bars==1||bars==2||bars==4||bars==8||bars==16))return false;std::lock_guard lock(controlMutex_);auto it=std::find_if(tracks_.begin(),tracks_.end(),[&](auto& n){return n->id==id;});if(it==tracks_.end())return false;auto& n=**it;size_t i=static_cast<size_t>(it-tracks_.begin());if(!n.inputArmed.load()||!n.desiredLooping.load()||clips_[i]||recordingClips_[i])return false;double rate=sampleRate_.load(),bpm=mailbox_.desiredBpm.load();if(rate<=0||bpm<=0)return false;if(enterOnPunch && statusPlaying_.load(std::memory_order_acquire))return false;uint32_t length=static_cast<uint32_t>(std::llround(bars*4.*60./bpm*rate));if(!length)return false;auto rec=std::make_shared<WavClip>();rec->sampleRate=static_cast<uint32_t>(rate);rec->left.resize(length);rec->right.resize(length);rec->displayName="Recorded loop";auto recs=recordingClips_;recs[i]=rec;n.recordLength=length;n.recordQuantization=static_cast<uint8_t>(q);n.recordFrame.store(0);n.recordComplete.store(false);n.recording.store(false);n.recordPending.store(!enterOnPunch);n.punchArmed.store(enterOnPunch);const uint32_t calibrationFrames=enterOnPunch?std::max<uint32_t>(1,static_cast<uint32_t>(std::ceil(rate*0.01))):0;n.punchCalibrationFrames.store(calibrationFrames);n.punchCalibrationRemaining.store(calibrationFrames);n.punchNoiseSum.store(0.0f);n.punchThreshold.store(0.02f);n.recordStartFrame.store(enterOnPunch?std::numeric_limits<uint64_t>::max():nextBoundary(statusTransportFrame_.load(std::memory_order_acquire),rate,bpm,q));if(!publishSnapshotLocked(buildSnapshotLocked(tracks_,clips_,recs)))return false;recordingClips_=std::move(recs);return true;}
bool RackGraph::cancelTrackLoopRecording(RackPathId id){std::lock_guard lock(controlMutex_);auto it=std::find_if(tracks_.begin(),tracks_.end(),[&](auto& n){return n->id==id;});if(it==tracks_.end())return false;size_t i=static_cast<size_t>(it-tracks_.begin());auto& n=**it;if(!recordingClips_[i]||n.recordComplete.load())return false;auto recs=recordingClips_;recs[i].reset();n.recordPending.store(false);n.recording.store(false);n.punchArmed.store(false);n.recordComplete.store(false);if(!publishSnapshotLocked(buildSnapshotLocked(tracks_,clips_,recs)))return false;recordingClips_=std::move(recs);return true;}
void RackGraph::writeMailboxLocked(bool changePlay,bool playing,bool reset,bool changeBpm,double bpm){mailbox_.sequence.fetch_add(1);if(changePlay){mailbox_.desiredPlaying.store(playing);mailbox_.playSerial.fetch_add(1);}if(changeBpm){mailbox_.desiredBpm.store(std::clamp(std::isfinite(bpm)?bpm:120.,20.,400.));mailbox_.bpmSerial.fetch_add(1);}if(reset)mailbox_.resetSerial.fetch_add(1);mailbox_.sequence.fetch_add(1,std::memory_order_release);}
bool RackGraph::setTransportPlaying(bool playing){std::lock_guard lock(controlMutex_);if(playing){auto recs=recordingClips_;bool changed=false;for(size_t i=0;i<recs.size();++i)if(tracks_[i]->punchArmed.load()){recs[i].reset();tracks_[i]->punchArmed.store(false);tracks_[i]->recordPending.store(false);tracks_[i]->recording.store(false);tracks_[i]->recordComplete.store(false);changed=true;}if(changed){publishSnapshotLocked(buildSnapshotLocked(tracks_,clips_,recs));recordingClips_=std::move(recs);}}else{auto recs=recordingClips_;bool changed=false;for(size_t i=0;i<recs.size();++i)if(recs[i] && !clips_[i] && !tracks_[i]->recordComplete.load(std::memory_order_acquire)){recs[i].reset();tracks_[i]->recordPending.store(false);tracks_[i]->recording.store(false);tracks_[i]->punchArmed.store(false);tracks_[i]->recordComplete.store(false);changed=true;}if(changed){publishSnapshotLocked(buildSnapshotLocked(tracks_,clips_,recs));recordingClips_=std::move(recs);}}writeMailboxLocked(true,playing,false);return true;}bool RackGraph::restartTransport(){std::lock_guard lock(controlMutex_);writeMailboxLocked(true,true,true);return true;}void RackGraph::setBeatsPerMinute(double bpm){std::lock_guard lock(controlMutex_);writeMailboxLocked(false,false,false,true,bpm);}
bool RackGraph::setTrackTransportPlaying(RackPathId id,bool playing,LaunchQuantization quantization){std::lock_guard lock(controlMutex_);auto it=std::find_if(tracks_.begin(),tracks_.end(),[&](auto& n){return n->id==id;});if(it==tracks_.end())return false;auto& n=**it;n.desiredQuantization.store(static_cast<uint8_t>(quantization));n.desiredPlaying.store(playing);n.commandSerial.fetch_add(1,std::memory_order_release);return true;}
bool RackGraph::setTrackTransportLooping(RackPathId id,bool looping){std::lock_guard lock(controlMutex_);auto it=std::find_if(tracks_.begin(),tracks_.end(),[&](auto& n){return n->id==id;});if(it==tracks_.end())return false;(*it)->desiredLooping.store(looping);return true;}
TransportSnapshot RackGraph::getTransportSnapshot() const {TransportSnapshot r{};for(;;){auto before=statusSequence_.load(std::memory_order_acquire);if(before&1)continue;r={statusPlaying_.load(),statusPositionSec_.load(),statusBpm_.load(),statusSamplePosition_.load(),statusTransportFrame_.load()};if(before==statusSequence_.load(std::memory_order_acquire))return r;}}
std::shared_ptr<PluginChain> RackGraph::getChain(RackPathId id) const{std::lock_guard lock(controlMutex_);if(id==kMasterPathId)return master_;auto it=std::find_if(tracks_.begin(),tracks_.end(),[&](auto& n){return n->id==id;});return it==tracks_.end()?nullptr:(*it)->chain;}
void RackGraph::setSampleRate(float rate,uint32_t buffer){std::lock_guard lock(controlMutex_);sampleRate_.store(rate);bufferSize_=buffer;for(auto& n:tracks_){n->sourceLeft.resize(buffer);n->sourceRight.resize(buffer);n->outputLeft.resize(buffer);n->outputRight.resize(buffer);n->chain->setSampleRate(rate,buffer);}master_->setSampleRate(rate,buffer);(void)publishSnapshotLocked(buildSnapshotLocked(tracks_,clips_,recordingClips_));}
void RackGraph::activate(){std::lock_guard lock(controlMutex_);for(auto& n:tracks_)n->chain->activate();master_->activate();}void RackGraph::deactivate(){std::lock_guard lock(controlMutex_);for(auto& n:tracks_)n->chain->deactivate();master_->deactivate();}void RackGraph::pauseAndResetTransport(){std::lock_guard lock(controlMutex_);writeMailboxLocked(true,false,true);}
RackGraph::State RackGraph::saveState(){std::lock_guard lock(controlMutex_);State s;for(auto& n:tracks_)s.tracks.push_back({n->volume.load(),n->inputArmed.load(),n->inputChannel.load(),n->chain->saveChainState()});s.master=master_->saveChainState();return s;}
void RackGraph::applyGlobalMailbox() noexcept {
    const auto sequence = mailbox_.sequence.load(std::memory_order_acquire);
    if (sequence & 1U) return;
    const auto play = mailbox_.playSerial.load();
    const auto reset = mailbox_.resetSerial.load();
    const auto bpm = mailbox_.bpmSerial.load();
    const bool desiredPlaying = mailbox_.desiredPlaying.load();
    const double desiredBpm = mailbox_.desiredBpm.load();
    if (mailbox_.sequence.load(std::memory_order_acquire) != sequence) return;
    if (play != appliedPlaySerial_) { audioPlaying_ = desiredPlaying; appliedPlaySerial_ = play; }
    if (reset != appliedResetSerial_) { audioTransportFrame_ = 0; appliedResetSerial_ = reset; }
    if (bpm != appliedBpmSerial_) { audioBpm_ = desiredBpm; appliedBpmSerial_ = bpm; }
}

void RackGraph::publishGlobalStatus(double rate) noexcept {statusSequence_.fetch_add(1);statusPlaying_.store(audioPlaying_);statusBpm_.store(audioBpm_);statusSamplePosition_.store(audioSamplePosition_);statusTransportFrame_.store(audioTransportFrame_);statusPositionSec_.store(static_cast<double>(audioTransportFrame_)/rate);statusSequence_.fetch_add(1,std::memory_order_release);}
void RackGraph::advanceTransport(uint32_t frames) noexcept {applyGlobalMailbox();double rate=sampleRate_.load();if(rate<=0)rate=48000.;audioSamplePosition_+=frames;if(audioPlaying_)audioTransportFrame_+=frames;publishGlobalStatus(rate);}
uint64_t RackGraph::nextBoundary(uint64_t frame,double rate,double bpm,LaunchQuantization q) noexcept {const double beats=q==LaunchQuantization::Bar?4.:q==LaunchQuantization::Quarter?1.:q==LaunchQuantization::Eighth?.5:.25;const uint64_t step=std::max<uint64_t>(1,static_cast<uint64_t>(std::llround(beats*rate*60./bpm)));return (frame/step+1)*step;}
void RackGraph::process(
        const float* const* inputs, int inputChannelCount,
        float* const* outputs, uint32_t frames) noexcept {
    GraphSnapshot* snapshot = activeSnapshot_.load(std::memory_order_acquire);
    do {
        hazardSnapshot_.store(snapshot, std::memory_order_seq_cst);
        if (snapshot == activeSnapshot_.load(std::memory_order_acquire)) break;
        snapshot = activeSnapshot_.load(std::memory_order_acquire);
    } while (true);
    if (!snapshot || frames > snapshot->capacity || !outputs ||
        !outputs[0] || !outputs[1]) {
        silence(outputs, frames);
        hazardSnapshot_.store(nullptr, std::memory_order_seq_cst);
        return;
    }

    applyGlobalMailbox();
    double rate = sampleRate_.load(std::memory_order_relaxed);
    if (rate <= 0.0) rate = 48000.0;
    const double bpm = std::clamp(audioBpm_, 20.0, 400.0);
    const double beatPosition =
        static_cast<double>(audioTransportFrame_) * bpm / (rate * 60.0);
    const int64_t bar = static_cast<int64_t>(std::floor(beatPosition / 4.0));
    const AudioProcessContext context{
        audioSamplePosition_, audioTransportFrame_, 0, rate, bpm,
        audioPlaying_, false, beatPosition, bar,
        beatPosition - static_cast<double>(bar) * 4.0};

    const bool masterEmpty = snapshot->master->isEmptyForAudio();
    const bool directSingleTrack = masterEmpty && snapshot->tracks.size() == 1;
    bool mixHasData = false;

    for (const auto& view : snapshot->tracks) {
        auto& node = *view.node;
        const int selectedChannel = inputChannelCount > 0
            ? std::clamp(node.inputChannel.load(std::memory_order_relaxed),
                         0, inputChannelCount - 1)
            : 0;
        const float* selectedInput = inputs && inputChannelCount > 0
            ? inputs[selectedChannel] : nullptr;
        const uint64_t commandSerial = node.commandSerial.load(std::memory_order_acquire);
        if (commandSerial != node.appliedCommandSerial) {
            node.appliedCommandSerial = commandSerial;
            if (!node.desiredPlaying.load(std::memory_order_relaxed)) {
                node.localPlaying = false;
                node.pendingLaunchFrame = std::numeric_limits<uint64_t>::max();
            } else if (audioPlaying_) {
                node.pendingLaunchFrame = nextBoundary(
                    audioTransportFrame_, rate, bpm,
                    static_cast<LaunchQuantization>(
                        node.desiredQuantization.load(std::memory_order_relaxed)));
            }
        }
        node.localLooping = node.desiredLooping.load(std::memory_order_relaxed);
        if (!audioPlaying_) {
            node.localPlaying = false;
            node.pendingLaunchFrame = std::numeric_limits<uint64_t>::max();
            if (!node.punchArmed.load(std::memory_order_relaxed)) {
                node.recordPending.store(false, std::memory_order_relaxed);
                node.recording.store(false, std::memory_order_relaxed);
            }
        }

        float* source[2] = {node.sourceLeft.data(), node.sourceRight.data()};
        for (uint32_t frame = 0; frame < frames; ++frame) {
            const uint64_t globalFrame = audioTransportFrame_ + frame;
            const float liveInput = selectedInput ? selectedInput[frame] : 0.0f;
            if (node.pendingLaunchFrame != std::numeric_limits<uint64_t>::max() &&
                audioPlaying_ && globalFrame >= node.pendingLaunchFrame) {
                node.pendingLaunchFrame = std::numeric_limits<uint64_t>::max();
                node.localPlaying = true;
                node.localFrame = 0;
            }

            if (view.recordingClip &&
                node.punchArmed.load(std::memory_order_relaxed)) {
                const float magnitude = std::fabs(liveInput);
                uint32_t calibrationRemaining =
                    node.punchCalibrationRemaining.load(std::memory_order_relaxed);
                if (calibrationRemaining > 0) {
                    node.punchNoiseSum.store(
                        node.punchNoiseSum.load(std::memory_order_relaxed) + magnitude,
                        std::memory_order_relaxed);
                    --calibrationRemaining;
                    node.punchCalibrationRemaining.store(
                        calibrationRemaining, std::memory_order_relaxed);
                    if (calibrationRemaining == 0) {
                        const uint32_t calibrationFrames = std::max<uint32_t>(
                            1, node.punchCalibrationFrames.load(std::memory_order_relaxed));
                        const float noiseAverage =
                            node.punchNoiseSum.load(std::memory_order_relaxed) /
                            static_cast<float>(calibrationFrames);
                        node.punchThreshold.store(
                            std::max(0.02f, noiseAverage * 2.0f),
                            std::memory_order_relaxed);
                    }
                } else if (magnitude >=
                           node.punchThreshold.load(std::memory_order_relaxed)) {
                    node.punchArmed.store(false, std::memory_order_relaxed);
                    node.recording.store(true, std::memory_order_relaxed);
                    node.recordFrame.store(0, std::memory_order_relaxed);
                    audioPlaying_ = true;
                    mailbox_.desiredPlaying.store(true, std::memory_order_relaxed);
                    statusPlaying_.store(true, std::memory_order_relaxed);
                }
            }
            if (view.recordingClip &&
                node.recordPending.load(std::memory_order_relaxed) &&
                audioPlaying_ &&
                globalFrame >= node.recordStartFrame.load(std::memory_order_relaxed)) {
                node.recordPending.store(false, std::memory_order_relaxed);
                node.recording.store(true, std::memory_order_relaxed);
                node.recordFrame.store(0, std::memory_order_relaxed);
            }

            if (view.recordingClip && node.recording.load(std::memory_order_relaxed)) {
                uint64_t recordFrame = node.recordFrame.load(std::memory_order_relaxed);
                if (recordFrame < view.recordingClip->left.size()) {
                    view.recordingClip->left[recordFrame] = liveInput;
                    view.recordingClip->right[recordFrame] = liveInput;
                }
                source[0][frame] = liveInput;
                source[1][frame] = liveInput;
                ++recordFrame;
                node.recordFrame.store(recordFrame, std::memory_order_relaxed);
                if (recordFrame >= node.recordLength) {
                    node.recording.store(false, std::memory_order_relaxed);
                    node.recordComplete.store(true, std::memory_order_release);
                    node.localPlaying = true;
                    node.localLooping = true;
                    node.localFrame = 0;
                }
                continue;
            }

            const WavClip* clip = view.clip.get();
            if (!clip && view.recordingClip &&
                node.recordComplete.load(std::memory_order_acquire)) {
                clip = view.recordingClip.get();
            }
            if (clip && node.localPlaying && !clip->left.empty() && clip->sampleRate > 0) {
                const uint64_t clipBoundary = std::max<uint64_t>(
                    1, static_cast<uint64_t>(std::ceil(
                        static_cast<double>(clip->left.size()) * rate /
                        static_cast<double>(clip->sampleRate))));
                if (node.localFrame >= clipBoundary) {
                    if (node.localLooping) node.localFrame %= clipBoundary;
                    else { node.localFrame = clipBoundary; node.localPlaying = false; }
                }
                if (node.localPlaying) {
                    const double sampleFrame = static_cast<double>(node.localFrame) *
                        static_cast<double>(clip->sampleRate) / rate;
                    const size_t first = std::min(static_cast<size_t>(sampleFrame),
                                                  clip->left.size() - 1);
                    const size_t second = std::min(first + 1, clip->left.size() - 1);
                    const float fraction = static_cast<float>(sampleFrame - first);
                    source[0][frame] = clip->left[first] +
                        (clip->left[second] - clip->left[first]) * fraction;
                    source[1][frame] = clip->right.empty() ? source[0][frame]
                        : clip->right[first] +
                            (clip->right[second] - clip->right[first]) * fraction;
                    ++node.localFrame;
                    if (node.localFrame >= clipBoundary) {
                        if (node.localLooping) node.localFrame %= clipBoundary;
                        else { node.localFrame = clipBoundary; node.localPlaying = false; }
                    }
                    continue;
                }
            }

            if (!view.clip && !view.recordingClip &&
                node.inputArmed.load(std::memory_order_relaxed) && selectedInput) {
                source[0][frame] = liveInput;
                source[1][frame] = liveInput;
            } else {
                source[0][frame] = 0.0f;
                source[1][frame] = 0.0f;
            }
        }

        node.statusPlaying.store(node.localPlaying, std::memory_order_relaxed);
        node.statusLooping.store(node.localLooping, std::memory_order_relaxed);
        node.statusFrame.store(node.localFrame, std::memory_order_relaxed);
        const float* trackSignal[2] = {source[0], source[1]};
        if (!node.chain->isEmptyForAudio()) {
            float* trackOutput[2] = {
                directSingleTrack ? outputs[0] : node.outputLeft.data(),
                directSingleTrack ? outputs[1] : node.outputRight.data()};
            node.chain->process(source, trackOutput, frames, context);
            trackSignal[0] = trackOutput[0];
            trackSignal[1] = trackOutput[1];
        }
        const float volume = node.volume.load(std::memory_order_relaxed);
        if (directSingleTrack) {
            copyScaled(outputs[0], outputs[1], trackSignal[0], trackSignal[1],
                       frames, volume);
        } else if (!mixHasData) {
            copyScaled(snapshot->mixLeft.data(), snapshot->mixRight.data(),
                       trackSignal[0], trackSignal[1], frames, volume);
            mixHasData = true;
        } else {
            mixScaled(snapshot->mixLeft.data(), snapshot->mixRight.data(),
                      trackSignal[0], trackSignal[1], frames, volume);
        }
    }

    if (!directSingleTrack) {
        if (!mixHasData) {
            std::memset(snapshot->mixLeft.data(), 0, frames * sizeof(float));
            std::memset(snapshot->mixRight.data(), 0, frames * sizeof(float));
        }
        const float* mix[2] = {snapshot->mixLeft.data(), snapshot->mixRight.data()};
        if (masterEmpty) copyScaled(outputs[0], outputs[1], mix[0], mix[1], frames, 1.0f);
        else snapshot->master->process(mix, outputs, frames, context);
    }
    audioSamplePosition_ += frames;
    if (audioPlaying_) audioTransportFrame_ += frames;
    publishGlobalStatus(rate);
    hazardSnapshot_.store(nullptr, std::memory_order_seq_cst);
}
void RackGraph::reclaimerLoop(){std::unique_lock lock(reclaimerMutex_);while(!reclaimerStop_){reclaimerWake_.wait_for(lock,std::chrono::milliseconds(10));lock.unlock();reclaimRetired();lock.lock();}}
void RackGraph::reclaimRetired(){std::lock_guard lock(reclaimerMutex_);auto* hazard=hazardSnapshot_.load(std::memory_order_seq_cst);RetiredSnapshot** cursor=&retired_;while(*cursor){auto* item=*cursor;if(item->owner.get()!=hazard){*cursor=item->next;delete item;}else cursor=&item->next;}}
} // namespace guitarrackcraft
