#include "RackGraph.h"

#include <algorithm>
#include <array>
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
    auto first=std::make_shared<TrackNode>(); first->id=nextTrackId_++; first->chain=std::make_shared<PluginChain>(); tracks_.push_back(first); clips_.push_back(nullptr); recordingClips_.push_back(nullptr); midiClips_.push_back(nullptr); wavSlots_.push_back({}); midiSlots_.push_back({});
    activeOwner_=buildSnapshotLocked(tracks_, clips_, recordingClips_); activeSnapshot_.store(activeOwner_.get(),std::memory_order_release); reclaimerThread_=std::thread(&RackGraph::reclaimerLoop,this);
}
RackGraph::~RackGraph(){ hazardSnapshot_.store(nullptr,std::memory_order_seq_cst); {std::lock_guard lock(reclaimerMutex_);reclaimerStop_=true;} reclaimerWake_.notify_one(); if(reclaimerThread_.joinable()) reclaimerThread_.join(); reclaimRetired(); }
double RackGraph::clipDuration(const WavClip& clip){return clip.sampleRate && !clip.left.empty()?static_cast<double>(clip.left.size())/clip.sampleRate:0.;}
std::unique_ptr<RackGraph::GraphSnapshot> RackGraph::buildSnapshotLocked(const std::vector<std::shared_ptr<TrackNode>>& nodes,const std::vector<std::shared_ptr<const WavClip>>& clips,const std::vector<std::shared_ptr<WavClip>>& recordings) const {
    if(nodes.size()!=clips.size()) return nullptr; auto snapshot=std::make_unique<GraphSnapshot>(); snapshot->master=master_; snapshot->capacity=bufferSize_; snapshot->mixLeft.resize(bufferSize_); snapshot->mixRight.resize(bufferSize_); snapshot->tracks.reserve(nodes.size());
    for(size_t i=0;i<nodes.size();++i){ if(!nodes[i]||!nodes[i]->chain||nodes[i]->sourceLeft.size()<bufferSize_||nodes[i]->sourceRight.size()<bufferSize_||nodes[i]->outputLeft.size()<bufferSize_||nodes[i]->outputRight.size()<bufferSize_) return nullptr; auto view=GraphSnapshot::TrackView{}; view.node=nodes[i]; view.clip=clips[i]; view.recordingClip=recordings.size()==nodes.size()?recordings[i]:nullptr; if(i<wavSlots_.size()) view.wavSlots=wavSlots_[i]; if(i<midiSlots_.size()) view.midiSlots=midiSlots_[i]; auto s=nodes[i]->selectedSlot.load(std::memory_order_relaxed); if(s<view.wavSlots.size()){view.clip=view.wavSlots[s];} if(s<view.midiSlots.size()){view.midi=view.midiSlots[s];} snapshot->tracks.push_back(std::move(view)); }
    return snapshot;
}
bool RackGraph::publishSnapshotLocked(std::unique_ptr<GraphSnapshot> next){ if(!next)return false; std::unique_ptr<RetiredSnapshot> retired; if(activeOwner_){retired=std::make_unique<RetiredSnapshot>();retired->owner=std::move(activeOwner_);} auto* raw=next.get(); activeOwner_=std::move(next); activeSnapshot_.exchange(raw,std::memory_order_release); if(retired){std::lock_guard lock(reclaimerMutex_);retired->next=retired_;retired_=retired.release();reclaimerWake_.notify_one();} return true; }
RackPathId RackGraph::addTrack(){std::lock_guard lock(controlMutex_);try{auto node=std::make_shared<TrackNode>();node->id=nextTrackId_;node->chain=std::make_shared<PluginChain>();node->sourceLeft.resize(bufferSize_);node->sourceRight.resize(bufferSize_);node->outputLeft.resize(bufferSize_);node->outputRight.resize(bufferSize_);if(sampleRate_.load()>0)node->chain->setSampleRate(sampleRate_.load(),bufferSize_);auto nodes=tracks_;auto clips=clips_;auto recs=recordingClips_;auto slots=wavSlots_;auto midiSlots=midiSlots_;nodes.push_back(node);clips.push_back(nullptr);recs.push_back(nullptr);slots.push_back({});midiSlots.push_back({});if(!publishSnapshotLocked(buildSnapshotLocked(nodes,clips,recs)))return 0;tracks_=std::move(nodes);clips_=std::move(clips);recordingClips_=std::move(recs);wavSlots_=std::move(slots);midiSlots_=std::move(midiSlots);midiClips_.push_back(nullptr);return nextTrackId_++;}catch(...){return 0;}}
bool RackGraph::removeTrack(RackPathId id){std::lock_guard lock(controlMutex_);if(id==kMasterPathId||tracks_.size()<=1)return false;auto it=std::find_if(tracks_.begin(),tracks_.end(),[&](auto& n){return n->id==id;});if(it==tracks_.end())return false;auto nodes=tracks_;auto clips=clips_;auto recs=recordingClips_;auto mids=midiClips_;auto ws=wavSlots_;auto ms=midiSlots_;auto i=static_cast<size_t>(it-tracks_.begin());nodes.erase(nodes.begin()+i);clips.erase(clips.begin()+i);recs.erase(recs.begin()+i);mids.erase(mids.begin()+i);ws.erase(ws.begin()+i);ms.erase(ms.begin()+i);if(!publishSnapshotLocked(buildSnapshotLocked(nodes,clips,recs)))return false;tracks_=std::move(nodes);clips_=std::move(clips);recordingClips_=std::move(recs);midiClips_=std::move(mids);wavSlots_=std::move(ws);midiSlots_=std::move(ms);return true;}
std::vector<TrackSnapshot> RackGraph::getTracks() const {
    std::lock_guard lock(controlMutex_);
    std::vector<TrackSnapshot> result;
    result.reserve(tracks_.size());
    auto rate = sampleRate_.load();
    if (rate <= 0) rate = 48000;
    for (size_t i = 0; i < tracks_.size(); ++i) {
        const auto& node = *tracks_[i];
        const uint32_t slot = node.selectedSlot.load(std::memory_order_relaxed);
        const auto& wavSlots = wavSlots_[i];
        const auto& midiSlots = midiSlots_[i];
        const auto wav = slot < wavSlots.size() ? wavSlots[slot] : nullptr;
        const auto midi = slot < midiSlots.size() ? midiSlots[slot] : nullptr;
        const auto frame = node.statusFrame.load();
        const auto& recorded = recordingClips_[i];
        const bool wavLoaded = static_cast<bool>(wav) || node.recordComplete.load();
        const bool midiLoaded = static_cast<bool>(midi);
        const std::string name = midi ? midi->displayName :
            (wav ? wav->displayName : (recorded ? recorded->displayName : std::string()));
        const double duration = midi ? static_cast<double>(midi->durationFrames) / rate :
            (wav ? clipDuration(*wav) : (recorded ? clipDuration(*recorded) : 0.0));
        result.push_back({node.id, node.volume.load(), node.inputArmed.load(), wavLoaded,
            name, duration, node.statusPlaying.load(), node.statusLooping.load(),
            static_cast<double>(frame) / rate, frame, node.recordPending.load(),
            node.recording.load(), node.punchArmed.load(), node.inputChannel.load(),
            midiLoaded, node.statusPlaying.load() && midiLoaded});
    }
    return result;
}
std::vector<TrackClipSlotInfo> RackGraph::getTrackClipSlots(RackPathId id) const { std::lock_guard lock(controlMutex_); auto it=std::find_if(tracks_.begin(),tracks_.end(),[&](auto& n){return n->id==id;}); if(it==tracks_.end()) return {}; size_t i=static_cast<size_t>(it-tracks_.begin()); auto rate=sampleRate_.load(); if(rate<=0) rate=48000; const auto& wav=wavSlots_[i]; const auto& midi=midiSlots_[i]; const size_t count=std::max(wav.size(),midi.size()); std::vector<TrackClipSlotInfo> out; out.reserve(count); for(size_t s=0;s<count;++s){auto w=s<wav.size()?wav[s]:nullptr; auto m=s<midi.size()?midi[s]:nullptr; out.push_back({id,static_cast<uint32_t>(s),static_cast<bool>(w),static_cast<bool>(m),m?m->displayName:(w?w->displayName:std::string()),m?static_cast<double>(m->durationFrames)/rate:(w?clipDuration(*w):0.),it.operator*()->selectedSlot.load()==s});} return out; }
std::vector<MidiNoteInfo> RackGraph::getTrackClipMidiNotes(RackPathId id,uint32_t slot) const { std::lock_guard lock(controlMutex_); auto it=std::find_if(tracks_.begin(),tracks_.end(),[&](auto& n){return n->id==id;}); if(it==tracks_.end())return {}; size_t i=static_cast<size_t>(it-tracks_.begin()); std::vector<MidiNoteInfo> out; if(i>=midiSlots_.size()||slot>=midiSlots_[i].size()||!midiSlots_[i][slot])return out; for(const auto& e:midiSlots_[i][slot]->events){ if(e.event.status>=0x90 && e.event.status<0xa0 && e.event.data2>0) out.push_back({e.frame,0,e.event.data1,e.event.data2}); } return out; }
bool RackGraph::selectTrackClipSlot(RackPathId id,uint32_t slot){std::lock_guard lock(controlMutex_);auto it=std::find_if(tracks_.begin(),tracks_.end(),[&](auto& n){return n->id==id;});if(it==tracks_.end())return false;const size_t i=static_cast<size_t>(it-tracks_.begin());if(i>=wavSlots_.size()||i>=midiSlots_.size())return false;(*it)->selectedSlot.store(slot);return publishSnapshotLocked(buildSnapshotLocked(tracks_,clips_,recordingClips_));}
bool RackGraph::attachTrackWavSlot(RackPathId id,uint32_t slot,std::shared_ptr<const WavClip> c){if(!c)return false;std::lock_guard lock(controlMutex_);auto it=std::find_if(tracks_.begin(),tracks_.end(),[&](auto& n){return n->id==id;});if(it==tracks_.end())return false;auto ws=wavSlots_;const size_t i=static_cast<size_t>(it-tracks_.begin());if(i>=ws.size())return false;if(slot>=ws[i].size())ws[i].resize(static_cast<size_t>(slot)+1);ws[i][slot]=std::move(c);auto old=wavSlots_;wavSlots_=ws;if(!publishSnapshotLocked(buildSnapshotLocked(tracks_,clips_,recordingClips_))){wavSlots_=std::move(old);return false;}return true;}
bool RackGraph::unloadTrackWavSlot(RackPathId id,uint32_t slot){std::lock_guard lock(controlMutex_);auto it=std::find_if(tracks_.begin(),tracks_.end(),[&](auto& n){return n->id==id;});if(it==tracks_.end())return false;auto ws=wavSlots_;const size_t i=static_cast<size_t>(it-tracks_.begin());if(i>=ws.size()||slot>=ws[i].size())return false;ws[i][slot].reset();auto old=wavSlots_;wavSlots_=ws;if(!publishSnapshotLocked(buildSnapshotLocked(tracks_,clips_,recordingClips_))){wavSlots_=std::move(old);return false;}return true;}
bool RackGraph::attachTrackMidiSlot(RackPathId id,uint32_t slot,std::shared_ptr<const MidiClip> c){if(!c)return false;std::lock_guard lock(controlMutex_);auto it=std::find_if(tracks_.begin(),tracks_.end(),[&](auto& n){return n->id==id;});if(it==tracks_.end())return false;auto ms=midiSlots_;const size_t i=static_cast<size_t>(it-tracks_.begin());if(i>=ms.size())return false;if(slot>=ms[i].size())ms[i].resize(static_cast<size_t>(slot)+1);ms[i][slot]=std::move(c);(*it)->midiScratch.resize(ms[i][slot]->events.size());auto old=midiSlots_;midiSlots_=ms;if(!publishSnapshotLocked(buildSnapshotLocked(tracks_,clips_,recordingClips_))){midiSlots_=std::move(old);return false;}return true;}
bool RackGraph::unloadTrackMidiSlot(RackPathId id,uint32_t slot){std::lock_guard lock(controlMutex_);auto it=std::find_if(tracks_.begin(),tracks_.end(),[&](auto& n){return n->id==id;});if(it==tracks_.end())return false;auto ms=midiSlots_;const size_t i=static_cast<size_t>(it-tracks_.begin());if(i>=ms.size()||slot>=ms[i].size())return false;ms[i][slot].reset();auto old=midiSlots_;midiSlots_=ms;if(!publishSnapshotLocked(buildSnapshotLocked(tracks_,clips_,recordingClips_))){midiSlots_=std::move(old);return false;}return true;}
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
}
bool RackGraph::attachTrackWav(RackPathId id,std::shared_ptr<const WavClip> clip){return attachTrackWavSlot(id,0,std::move(clip));}
bool RackGraph::attachTrackMidi(RackPathId id,std::shared_ptr<const MidiClip> clip){return attachTrackMidiSlot(id,0,std::move(clip));}
bool RackGraph::unloadTrackMidi(RackPathId id){return unloadTrackMidiSlot(id,0);}
bool RackGraph::unloadTrackWav(RackPathId id){return unloadTrackWavSlot(id,0);}
bool RackGraph::clearTrackWavs(){std::lock_guard lock(controlMutex_);auto copy=clips_;auto recs=recordingClips_;for(size_t i=0;i<copy.size();++i){copy[i].reset();recs[i].reset();tracks_[i]->recordPending.store(false);tracks_[i]->recording.store(false);tracks_[i]->punchArmed.store(false);tracks_[i]->recordComplete.store(false);}if(!publishSnapshotLocked(buildSnapshotLocked(tracks_,copy,recs)))return false;clips_=std::move(copy);recordingClips_=std::move(recs);return true;}
bool RackGraph::startTrackLoopRecording(RackPathId id,double bars,LaunchQuantization q,bool enterOnPunch){if(!(bars==.25||bars==1||bars==2||bars==4||bars==8||bars==16))return false;std::lock_guard lock(controlMutex_);auto it=std::find_if(tracks_.begin(),tracks_.end(),[&](auto& n){return n->id==id;});if(it==tracks_.end())return false;auto& n=**it;size_t i=static_cast<size_t>(it-tracks_.begin());if(!n.inputArmed.load()||!n.desiredLooping.load()||clips_[i]||recordingClips_[i])return false;double rate=sampleRate_.load(),bpm=mailbox_.desiredBpm.load();if(rate<=0||bpm<=0)return false;if(enterOnPunch && q!=LaunchQuantization::None && statusPlaying_.load(std::memory_order_acquire))return false;uint32_t length=static_cast<uint32_t>(std::llround(bars*4.*60./bpm*rate));if(!length)return false;auto rec=std::make_shared<WavClip>();rec->sampleRate=static_cast<uint32_t>(rate);rec->left.resize(length);rec->right.resize(length);rec->displayName="Recorded loop";auto recs=recordingClips_;recs[i]=rec;n.recordLength=length;n.recordQuantization=static_cast<uint8_t>(q);n.recordFrame.store(0);n.recordComplete.store(false);n.recording.store(false);n.recordPending.store(!enterOnPunch);n.punchArmed.store(enterOnPunch);const uint32_t calibrationFrames=enterOnPunch?std::max<uint32_t>(1,static_cast<uint32_t>(std::ceil(rate*0.01))):0;n.punchCalibrationFrames.store(calibrationFrames);n.punchCalibrationRemaining.store(calibrationFrames);n.punchNoiseSum.store(0.0f);n.punchThreshold.store(0.02f);n.recordStartFrame.store(enterOnPunch?std::numeric_limits<uint64_t>::max():nextBoundary(statusTransportFrame_.load(std::memory_order_acquire),rate,bpm,q));if(!publishSnapshotLocked(buildSnapshotLocked(tracks_,clips_,recs)))return false;recordingClips_=std::move(recs);return true;}
bool RackGraph::cancelTrackLoopRecording(RackPathId id){std::lock_guard lock(controlMutex_);auto it=std::find_if(tracks_.begin(),tracks_.end(),[&](auto& n){return n->id==id;});if(it==tracks_.end())return false;size_t i=static_cast<size_t>(it-tracks_.begin());auto& n=**it;if(!recordingClips_[i]||n.recordComplete.load())return false;auto recs=recordingClips_;recs[i].reset();n.recordPending.store(false);n.recording.store(false);n.punchArmed.store(false);n.recordComplete.store(false);if(!publishSnapshotLocked(buildSnapshotLocked(tracks_,clips_,recs)))return false;recordingClips_=std::move(recs);return true;}
void RackGraph::writeMailboxLocked(bool changePlay,bool playing,bool reset,bool changeBpm,double bpm){mailbox_.sequence.fetch_add(1);if(changePlay){mailbox_.desiredPlaying.store(playing);mailbox_.playSerial.fetch_add(1);}if(changeBpm){mailbox_.desiredBpm.store(std::clamp(std::isfinite(bpm)?bpm:120.,20.,400.));mailbox_.bpmSerial.fetch_add(1);}if(reset)mailbox_.resetSerial.fetch_add(1);mailbox_.sequence.fetch_add(1,std::memory_order_release);}
bool RackGraph::setTransportPlaying(bool playing){std::lock_guard lock(controlMutex_);if(playing){auto recs=recordingClips_;bool changed=false;for(size_t i=0;i<recs.size();++i)if(tracks_[i]->punchArmed.load()){recs[i].reset();tracks_[i]->punchArmed.store(false);tracks_[i]->recordPending.store(false);tracks_[i]->recording.store(false);tracks_[i]->recordComplete.store(false);changed=true;}if(changed){publishSnapshotLocked(buildSnapshotLocked(tracks_,clips_,recs));recordingClips_=std::move(recs);}}else{auto recs=recordingClips_;bool changed=false;for(size_t i=0;i<recs.size();++i)if(recs[i] && !clips_[i] && !tracks_[i]->recordComplete.load(std::memory_order_acquire)){recs[i].reset();tracks_[i]->recordPending.store(false);tracks_[i]->recording.store(false);tracks_[i]->punchArmed.store(false);tracks_[i]->recordComplete.store(false);changed=true;}if(changed){publishSnapshotLocked(buildSnapshotLocked(tracks_,clips_,recs));recordingClips_=std::move(recs);}}writeMailboxLocked(true,playing,false);return true;}bool RackGraph::restartTransport(){std::lock_guard lock(controlMutex_);writeMailboxLocked(true,true,true);return true;}void RackGraph::setBeatsPerMinute(double bpm){std::lock_guard lock(controlMutex_);writeMailboxLocked(false,false,false,true,bpm);}
bool RackGraph::setTrackTransportPlaying(RackPathId id,bool playing,LaunchQuantization quantization){std::lock_guard lock(controlMutex_);auto it=std::find_if(tracks_.begin(),tracks_.end(),[&](auto& n){return n->id==id;});if(it==tracks_.end())return false;auto& n=**it;n.desiredQuantization.store(static_cast<uint8_t>(quantization));n.desiredPlaying.store(playing);n.commandSerial.fetch_add(1,std::memory_order_release);return true;}
bool RackGraph::setTrackTransportLooping(RackPathId id,bool looping){std::lock_guard lock(controlMutex_);auto it=std::find_if(tracks_.begin(),tracks_.end(),[&](auto& n){return n->id==id;});if(it==tracks_.end())return false;(*it)->desiredLooping.store(looping);return true;}
std::vector<float> RackGraph::getTrackWaveformPeaks(RackPathId id, uint32_t maxBuckets) const {
    constexpr uint32_t kMaxBuckets = 512;
    if (maxBuckets == 0) return {};
    std::lock_guard lock(controlMutex_);
    const auto it = std::find_if(tracks_.begin(), tracks_.end(),
                                 [&](const auto& node) { return node->id == id; });
    if (it == tracks_.end()) return {};
    const size_t trackIndex = static_cast<size_t>(it - tracks_.begin());
    const auto& slots = wavSlots_[trackIndex];
    const uint32_t selectedSlot = (*it)->selectedSlot.load(std::memory_order_relaxed);
    const auto clip = selectedSlot < slots.size() ? slots[selectedSlot] : nullptr;
    if (!clip || clip->left.empty()) return {};
    const uint32_t bucketCount = std::min<uint32_t>(
        {maxBuckets, kMaxBuckets, static_cast<uint32_t>(clip->left.size())});
    std::vector<float> peaks(bucketCount);
    for (uint32_t bucket = 0; bucket < bucketCount; ++bucket) {
        const size_t begin = clip->left.size() * bucket / bucketCount;
        const size_t end = clip->left.size() * (bucket + 1) / bucketCount;
        float peak = 0.0f;
        for (size_t frame = begin; frame < end; ++frame) {
            peak = std::max(peak, std::fabs(clip->left[frame]));
            if (!clip->right.empty()) peak = std::max(peak, std::fabs(clip->right[frame]));
        }
        peaks[bucket] = peak;
    }
    return peaks;
}
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
uint64_t RackGraph::nextBoundary(uint64_t frame,double rate,double bpm,LaunchQuantization q) noexcept {if(q==LaunchQuantization::None)return frame;const double beats=q==LaunchQuantization::Bar?4.:q==LaunchQuantization::Quarter?1.:q==LaunchQuantization::Eighth?.5:.25;const uint64_t step=std::max<uint64_t>(1,static_cast<uint64_t>(std::llround(beats*rate*60./bpm)));return (frame/step+1)*step;}
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
        bool midiWasPlaying = false;
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

            if (view.midi && !view.clip && !view.recordingClip && node.localPlaying) {
                midiWasPlaying = true;
                const uint64_t midiBoundary = std::max<uint64_t>(
                    1, static_cast<uint64_t>(std::ceil(
                        static_cast<double>(view.midi->durationFrames) * rate / 48000.0)));
                ++node.localFrame;
                if (node.localFrame >= midiBoundary) {
                    if (node.localLooping) node.localFrame %= midiBoundary;
                    else {
                        node.localFrame = midiBoundary;
                        node.localPlaying = false;
                    }
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
        uint32_t midiCount = 0;
        const uint64_t midiStart = node.localFrame >= frames ? node.localFrame - frames : 0;
        if (view.midi && midiWasPlaying && audioPlaying_) {
            const uint64_t midiEnd = node.localFrame;
            for (const auto& timed : view.midi->events) {
                const uint64_t eventFrame = static_cast<uint64_t>(static_cast<double>(timed.frame) * rate / 48000.0);
                if (eventFrame >= midiStart && eventFrame < midiEnd && midiCount < node.midiScratch.size()) {
                    auto event = timed.event;
                    event.frameOffset = static_cast<uint32_t>(eventFrame - midiStart);
                    node.midiScratch[midiCount++] = event;
                }
            }
        }
        node.statusLooping.store(node.localLooping, std::memory_order_relaxed);
        node.statusFrame.store(node.localFrame, std::memory_order_relaxed);
        const float* trackSignal[2] = {source[0], source[1]};
        if (!node.chain->isEmptyForAudio()) {
            float* trackOutput[2] = {
                directSingleTrack ? outputs[0] : node.outputLeft.data(),
                directSingleTrack ? outputs[1] : node.outputRight.data()};
            std::array<MidiEvent, 128> midiOutputScratch{};
            node.chain->process(source, trackOutput, frames, context,
                                node.midiScratch.data(), midiCount,
                                midiOutputScratch.data(),
                                static_cast<uint32_t>(midiOutputScratch.size()));
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
        std::array<MidiEvent, 128> masterMidiOutput{};
        if (masterEmpty) copyScaled(outputs[0], outputs[1], mix[0], mix[1], frames, 1.0f);
        else snapshot->master->process(mix, outputs, frames, context, nullptr, 0,
                                       masterMidiOutput.data(), masterMidiOutput.size());
    }
    audioSamplePosition_ += frames;
    if (audioPlaying_) audioTransportFrame_ += frames;
    publishGlobalStatus(rate);
    hazardSnapshot_.store(nullptr, std::memory_order_seq_cst);
}
void RackGraph::reclaimerLoop(){std::unique_lock lock(reclaimerMutex_);while(!reclaimerStop_){reclaimerWake_.wait_for(lock,std::chrono::milliseconds(10));lock.unlock();reclaimRetired();lock.lock();}}
void RackGraph::reclaimRetired(){std::lock_guard lock(reclaimerMutex_);auto* hazard=hazardSnapshot_.load(std::memory_order_seq_cst);RetiredSnapshot** cursor=&retired_;while(*cursor){auto* item=*cursor;if(item->owner.get()!=hazard){*cursor=item->next;delete item;}else cursor=&item->next;}}
} // namespace guitarrackcraft
