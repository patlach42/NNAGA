#include "RackGraph.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <functional>
#include <unordered_map>
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
double RackGraph::clipDuration(const WavClip& clip) {
    return clip.sampleRate && !clip.left.empty()
        ? static_cast<double>(clip.left.size()) / static_cast<double>(clip.sampleRate)
        : 0.0;
}
std::unique_ptr<RackGraph::GraphSnapshot> RackGraph::buildSnapshotLocked(const std::vector<std::shared_ptr<TrackNode>>& nodes,const std::vector<std::shared_ptr<const WavClip>>& clips,const std::vector<std::shared_ptr<WavClip>>& recordings) const {
    if (nodes.size() != clips.size() || (recordings.size() && recordings.size() != nodes.size())) return nullptr;
    auto snapshot = std::make_unique<GraphSnapshot>();
    snapshot->master = master_;
    snapshot->capacity = bufferSize_;
    snapshot->mixLeft.resize(bufferSize_);
    snapshot->mixRight.resize(bufferSize_);
    snapshot->tracks.reserve(nodes.size());
    std::unordered_map<RackPathId, uint32_t> index;
    index.reserve(nodes.size());
    for (uint32_t i = 0; i < nodes.size(); ++i) {
        if (!nodes[i] || !nodes[i]->chain ||
            nodes[i]->sourceLeft.size() < bufferSize_ ||
            nodes[i]->sourceRight.size() < bufferSize_ ||
            nodes[i]->outputLeft.size() < bufferSize_ ||
            nodes[i]->outputRight.size() < bufferSize_) return nullptr;
        index.emplace(nodes[i]->id, i);
        auto view = GraphSnapshot::TrackView{};
        view.node = nodes[i];
        view.inputSource = nodes[i]->inputSource;
        view.clipRuntime = nodes[i]->clipRuntime;
        view.slotConfig = nodes[i]->slotConfig;
        view.clip = clips[i];
        view.recordingClip = recordings.size() == nodes.size() ? recordings[i] : nullptr;
        if (i < wavSlots_.size()) view.wavSlots = wavSlots_[i];
        if (i < midiSlots_.size()) view.midiSlots = midiSlots_[i];
        view.selectedSlot = nodes[i]->selectedSlot.load(std::memory_order_relaxed);
        view.recordingSlot = nodes[i]->recordingSlot;
        view.recordLength = nodes[i]->recordLength;
        view.recordingGeneration = nodes[i]->recordingGeneration;
        const auto s = view.selectedSlot;
        if (s < view.wavSlots.size()) view.clip = view.wavSlots[s];
        if (s < view.midiSlots.size()) view.midi = view.midiSlots[s];
        snapshot->tracks.push_back(std::move(view));
    }
    std::vector<uint8_t> state(nodes.size(), 0);
    std::function<bool(uint32_t)> visit = [&](uint32_t i) {
        if (state[i] == 1) return false;
        if (state[i] == 2) return true;
        state[i] = 1;
        const auto& source = snapshot->tracks[i].inputSource;
        if (source.kind == TrackInputSource::Kind::TrackOutput) {
            auto it = index.find(source.trackId);
            if (it == index.end() || it->second == i || !visit(it->second)) return false;
            snapshot->tracks[i].routeIndex = static_cast<int32_t>(it->second);
        }
        state[i] = 2;
        snapshot->topoOrder.push_back(i);
        return true;
    };
    for (uint32_t i = 0; i < nodes.size(); ++i) if (!visit(i)) return nullptr;
    return snapshot;
}
bool RackGraph::publishSnapshotLocked(std::unique_ptr<GraphSnapshot> next){ if(!next)return false; std::unique_ptr<RetiredSnapshot> retired; if(activeOwner_){retired=std::make_unique<RetiredSnapshot>();retired->owner=std::move(activeOwner_);} auto* raw=next.get(); activeOwner_=std::move(next); activeSnapshot_.exchange(raw,std::memory_order_release); if(retired){std::lock_guard lock(reclaimerMutex_);retired->next=retired_;retired_=retired.release();reclaimerWake_.notify_one();} return true; }
RackPathId RackGraph::addTrack(){std::lock_guard lock(controlMutex_);try{auto node=std::make_shared<TrackNode>();node->id=nextTrackId_;node->chain=std::make_shared<PluginChain>();node->sourceLeft.resize(bufferSize_);node->sourceRight.resize(bufferSize_);node->outputLeft.resize(bufferSize_);node->outputRight.resize(bufferSize_);if(sampleRate_.load()>0)node->chain->setSampleRate(sampleRate_.load(),bufferSize_);auto nodes=tracks_;auto clips=clips_;auto recs=recordingClips_;auto slots=wavSlots_;auto midiSlots=midiSlots_;auto labels=clipLabelOverrides_;nodes.push_back(node);clips.push_back(nullptr);recs.push_back(nullptr);slots.push_back({});midiSlots.push_back({});labels.push_back({});if(!publishSnapshotLocked(buildSnapshotLocked(nodes,clips,recs)))return 0;tracks_=std::move(nodes);clips_=std::move(clips);recordingClips_=std::move(recs);wavSlots_=std::move(slots);midiSlots_=std::move(midiSlots);clipLabelOverrides_=std::move(labels);midiClips_.push_back(nullptr);return nextTrackId_++;}catch(...){return 0;}}
bool RackGraph::removeTrack(RackPathId id){std::lock_guard lock(controlMutex_);if(id==kMasterPathId||tracks_.size()<=1)return false;auto it=std::find_if(tracks_.begin(),tracks_.end(),[&](auto& n){return n->id==id;});if(it==tracks_.end())return false;auto nodes=tracks_;auto clips=clips_;auto recs=recordingClips_;auto mids=midiClips_;auto ws=wavSlots_;auto ms=midiSlots_;auto labels=clipLabelOverrides_;auto i=static_cast<size_t>(it-tracks_.begin());nodes.erase(nodes.begin()+i);clips.erase(clips.begin()+i);recs.erase(recs.begin()+i);mids.erase(mids.begin()+i);ws.erase(ws.begin()+i);ms.erase(ms.begin()+i);labels.erase(labels.begin()+i);if(!publishSnapshotLocked(buildSnapshotLocked(nodes,clips,recs)))return false;tracks_=std::move(nodes);clips_=std::move(clips);recordingClips_=std::move(recs);midiClips_=std::move(mids);wavSlots_=std::move(ws);midiSlots_=std::move(ms);clipLabelOverrides_=std::move(labels);return true;}
std::vector<TrackSnapshot> RackGraph::getTracks() const {
    std::lock_guard lock(controlMutex_);
    std::vector<TrackSnapshot> result;
    result.reserve(tracks_.size());
    for (size_t i = 0; i < tracks_.size(); ++i) {
        const auto& node = *tracks_[i];
        const uint32_t selected = node.selectedSlot.load(std::memory_order_relaxed);
        const int32_t activeIndex = node.activeSlot.load(std::memory_order_relaxed);
        const auto recordingPhase = node.recordingPhase.load(std::memory_order_acquire);
        const bool hasActive = activeIndex >= 0;
        const uint32_t mediaSlot = hasActive ? static_cast<uint32_t>(activeIndex) : selected;
        const auto& wavSlots = wavSlots_[i];
        const auto& midiSlots = midiSlots_[i];
        const auto wav = mediaSlot < wavSlots.size() ? wavSlots[mediaSlot] : nullptr;
        const auto midi = mediaSlot < midiSlots.size() ? midiSlots[mediaSlot] : nullptr;
        const auto rt = hasActive && mediaSlot < node.clipRuntime.size() ? node.clipRuntime[mediaSlot] : nullptr;
        uint64_t frame = 0;
        bool playing = false;
        bool looping = false;
        double localQn = 0.0;
        double rate = 48000.0;
        uint64_t captured = 0;
        for (;;) {
            const auto before = statusSequence_.load(std::memory_order_acquire);
            if (before & 1U) continue;
            rate = statusSampleRate_.load(std::memory_order_relaxed);
            if (rate <= 0) rate = 48000.0;
            frame = rt ? rt->statusFrame.load(std::memory_order_relaxed) : 0;
            playing = rt && rt->statusPlaying.load(std::memory_order_relaxed);
            looping = rt && rt->looping.load(std::memory_order_relaxed);
            localQn = rt ? rt->localQuarterNotes.load(std::memory_order_acquire) : 0.0;
            captured = statusCapturedAtNanos_.load(std::memory_order_relaxed);
            if (before == statusSequence_.load(std::memory_order_acquire)) break;
        }
        const std::string name = (i < clipLabelOverrides_.size() && mediaSlot < clipLabelOverrides_[i].size() &&
            !clipLabelOverrides_[i][mediaSlot].empty()) ? clipLabelOverrides_[i][mediaSlot] :
            (midi ? midi->displayName : (wav ? wav->displayName : std::string()));
        const double duration = midi ? static_cast<double>(midi->durationMicroseconds) / 1'000'000.0 :
            (wav ? clipDuration(*wav) : 0.0);
        result.push_back({node.id, node.volume.load(), node.inputArmed.load(), node.inputArmLocked.load(),
            static_cast<bool>(wav), name, duration, playing, looping, static_cast<double>(frame) / rate,
            frame, node.recordPending.load(),
            static_cast<uint8_t>(node.inputSource.kind),
            node.inputSource.trackId, static_cast<uint8_t>(node.inputSource.tap),
            node.inputSource.firstChannel,
            node.recording.load(), node.punchArmed.load(),
            static_cast<bool>(midi), playing && static_cast<bool>(midi),
            activeIndex, selected, node.defaultLoopLengthBars.load(), localQn, rate, captured,
            (recordingPhase == RecordingPhase::Armed ||
             recordingPhase == RecordingPhase::Pending ||
             recordingPhase == RecordingPhase::Recording)
                ? static_cast<int32_t>(node.recordingSlot)
                : -1});
    }
    return result;
}
std::vector<TrackClipSlotInfo> RackGraph::getTrackClipSlots(RackPathId id) const {
    std::lock_guard lock(controlMutex_);
    auto it=std::find_if(tracks_.begin(),tracks_.end(),[&](auto& n){return n->id==id;});
    if(it==tracks_.end()) return {};
    const size_t i=static_cast<size_t>(it-tracks_.begin());
    const auto& wav=wavSlots_[i]; const auto& midi=midiSlots_[i];
    const auto& n=**it;
    const size_t count=std::max({wav.size(),midi.size(),n.clipRuntime.size(),n.slotConfig.size()});
    std::vector<TrackClipSlotInfo> out; out.reserve(count);
    for(size_t s=0;s<count;++s){
        auto w=s<wav.size()?wav[s]:nullptr; auto m=s<midi.size()?midi[s]:nullptr;
        const std::string name=(i<clipLabelOverrides_.size()&&s<clipLabelOverrides_[i].size()&&!clipLabelOverrides_[i][s].empty())?clipLabelOverrides_[i][s]:(m?m->displayName:(w?w->displayName:std::string()));
        const bool audibleActive=n.activeSlot.load(std::memory_order_relaxed)==static_cast<int32_t>(s);
        const auto rt=s<n.clipRuntime.size()?n.clipRuntime[s]:nullptr;
        uint64_t frame = 0;
        double localQn = 0.0;
        double rate = 48000.0;
        uint64_t captured = 0;
        for (;;) {
            const auto before = statusSequence_.load(std::memory_order_acquire);
            if (before & 1U) continue;
            rate = statusSampleRate_.load(std::memory_order_relaxed);
            if (rate <= 0) rate = 48000.0;
            frame = rt ? rt->statusFrame.load(std::memory_order_relaxed) : 0;
            localQn = rt ? rt->localQuarterNotes.load(std::memory_order_acquire) : 0.0;
            captured = statusCapturedAtNanos_.load(std::memory_order_relaxed);
            if (before == statusSequence_.load(std::memory_order_acquire)) break;
        }
        const double defaultLoopLength = s<n.slotConfig.size()&&n.slotConfig[s]?n.slotConfig[s]->defaultLoopLengthBars.load():n.defaultLoopLengthBars.load();
        out.push_back({id,static_cast<uint32_t>(s),static_cast<bool>(w),static_cast<bool>(m),name,m?static_cast<double>(m->durationMicroseconds)/1'000'000.0:(w?clipDuration(*w):0.0),n.selectedSlot.load(std::memory_order_relaxed)==s,audibleActive&&rt&&rt->statusPlaying.load(),rt?rt->looping.load():false,static_cast<double>(frame)/rate,frame,rt?rt->loopLengthBars.load():defaultLoopLength,s<n.slotConfig.size()&&n.slotConfig[s]&&n.slotConfig[s]->enterOnPunch.load(),((w||m)&&rt)?rt->sourceBpm.load():0.0,rt?rt->tempoMode.load():0,defaultLoopLength,n.pendingSwitchSlot.load(std::memory_order_relaxed)==static_cast<int32_t>(s),localQn,rate,captured,rt?rt->loopStartQuarterNotes.load():0.0,rt?rt->loopLengthQuarterNotes.load():defaultLoopLength*4.0});
    }
    return out;
}
std::vector<MidiNoteInfo> RackGraph::getTrackClipMidiNotes(RackPathId id,uint32_t slot) const { std::lock_guard lock(controlMutex_); auto it=std::find_if(tracks_.begin(),tracks_.end(),[&](auto& n){return n->id==id;}); if(it==tracks_.end())return {}; size_t i=static_cast<size_t>(it-tracks_.begin()); std::vector<MidiNoteInfo> out; if(i>=midiSlots_.size()||slot>=midiSlots_[i].size()||!midiSlots_[i][slot])return out; for(const auto& e:midiSlots_[i][slot]->events){ if(e.event.status>=0x90 && e.event.status<0xa0 && e.event.data2>0) out.push_back({e.microseconds,0,e.event.data1,e.event.data2}); } return out; }
bool RackGraph::selectTrackClipSlot(RackPathId id,uint32_t slot){std::lock_guard lock(controlMutex_);auto it=std::find_if(tracks_.begin(),tracks_.end(),[&](auto& n){return n->id==id;});if(it==tracks_.end())return false;const size_t i=static_cast<size_t>(it-tracks_.begin());if(i>=wavSlots_.size()||i>=midiSlots_.size())return false;(*it)->selectedSlot.store(slot);return publishSnapshotLocked(buildSnapshotLocked(tracks_,clips_,recordingClips_));}
bool RackGraph::attachTrackWavSlot(RackPathId id,uint32_t slot,std::shared_ptr<const WavClip> c){
    if(!c || c->sampleRate == 0 || c->left.empty() || !std::isfinite(c->sourceBpm) || c->sourceBpm < 20.0 || c->sourceBpm > 400.0)return false; std::lock_guard lock(controlMutex_);
    auto it=std::find_if(tracks_.begin(),tracks_.end(),[&](auto& n){return n->id==id;});
    if(it==tracks_.end())return false;
    if(!ensureClipRuntimeLocked(**it,slot))return false;
    auto ws=wavSlots_; const size_t i=static_cast<size_t>(it-tracks_.begin());
    if(i>=ws.size())return false; if(slot>=ws[i].size())ws[i].resize(static_cast<size_t>(slot)+1);
    const double importedBpm = c->sourceBpm;
    auto old=wavSlots_; auto oldRuntime=(*it)->clipRuntime;
    ws[i][slot]=std::move(c); wavSlots_=ws;
    (*it)->clipRuntime[slot]=std::make_shared<ClipRuntime>();
    (*it)->clipRuntime[slot]->loopLengthBars.store(slot<(*it)->slotConfig.size()&&(*it)->slotConfig[slot] ? (*it)->slotConfig[slot]->defaultLoopLengthBars.load() : (*it)->defaultLoopLengthBars.load());
    (*it)->clipRuntime[slot]->loopLengthQuarterNotes.store(static_cast<double>(ws[i][slot]->left.size()) / static_cast<double>(ws[i][slot]->sampleRate) * importedBpm / 60.0);
    (*it)->clipRuntime[slot]->sourceBpm.store(importedBpm, std::memory_order_release);
    if(i>=clipLabelOverrides_.size())clipLabelOverrides_.resize(i+1);
    if(slot>=clipLabelOverrides_[i].size())clipLabelOverrides_[i].resize(static_cast<size_t>(slot)+1);
    clipLabelOverrides_[i][slot].clear();
    if(!publishSnapshotLocked(buildSnapshotLocked(tracks_,clips_,recordingClips_))){wavSlots_=std::move(old);(*it)->clipRuntime=std::move(oldRuntime);return false;} return true;
}
bool RackGraph::unloadTrackWavSlot(RackPathId id,uint32_t slot){
    std::lock_guard lock(controlMutex_); auto it=std::find_if(tracks_.begin(),tracks_.end(),[&](auto& n){return n->id==id;});
    if(it==tracks_.end())return false; const size_t i=static_cast<size_t>(it-tracks_.begin());
    auto ws=wavSlots_; if(i>=ws.size()||slot>=ws[i].size())return false; ws[i][slot].reset();
    if(i<clipLabelOverrides_.size()&&slot<clipLabelOverrides_[i].size())clipLabelOverrides_[i][slot].clear();
    auto old=wavSlots_; auto oldRuntime=(*it)->clipRuntime; wavSlots_=ws;
    const bool hasOther=slot<midiSlots_[i].size()&&midiSlots_[i][slot];
    if(hasOther){
        (*it)->clipRuntime[slot]->loopLengthQuarterNotes.store((*it)->clipRuntime[slot]->loopLengthBars.load() * 4.0);
    } else if(slot<(*it)->clipRuntime.size()) (*it)->clipRuntime[slot].reset();
    if((*it)->activeSlot.load()==static_cast<int32_t>(slot))(*it)->activeSlot.store(-1);
    if((*it)->pendingSwitchSlot.load()==static_cast<int32_t>(slot)){(*it)->pendingSwitchSlot.store(-1);(*it)->pendingSwitchFrame=std::numeric_limits<uint64_t>::max();}
    if(!publishSnapshotLocked(buildSnapshotLocked(tracks_,clips_,recordingClips_))){wavSlots_=std::move(old);(*it)->clipRuntime=std::move(oldRuntime);return false;} return true;
}
bool RackGraph::attachTrackMidiSlot(RackPathId id,uint32_t slot,std::shared_ptr<const MidiClip> c){
    if(!c || c->durationMicroseconds == 0 || c->events.empty())return false; std::lock_guard lock(controlMutex_);
    auto it=std::find_if(tracks_.begin(),tracks_.end(),[&](auto& n){return n->id==id;});
    if(it==tracks_.end()||!ensureClipRuntimeLocked(**it,slot))return false;
    auto ms=midiSlots_; const size_t i=static_cast<size_t>(it-tracks_.begin());
    if(i>=ms.size())return false; if(slot>=ms[i].size())ms[i].resize(static_cast<size_t>(slot)+1);
    ms[i][slot]=std::move(c);
    constexpr size_t kMaxMidiEventsPerBlock = 4096;
    const size_t eventCount = ms[i][slot]->events.size();
    (*it)->midiScratch.resize(std::min(kMaxMidiEventsPerBlock, eventCount * 64u));
    auto old=midiSlots_; auto oldRuntime=(*it)->clipRuntime; midiSlots_=ms;
    (*it)->clipRuntime[slot]=std::make_shared<ClipRuntime>();
    (*it)->clipRuntime[slot]->loopLengthBars.store(slot<(*it)->slotConfig.size()&&(*it)->slotConfig[slot] ? (*it)->slotConfig[slot]->defaultLoopLengthBars.load() : (*it)->defaultLoopLengthBars.load());
    (*it)->clipRuntime[slot]->loopLengthQuarterNotes.store(static_cast<double>(ms[i][slot]->durationMicroseconds) * ms[i][slot]->sourceBpm / 60'000'000.0);
    (*it)->clipRuntime[slot]->sourceBpm.store(ms[i][slot]->sourceBpm, std::memory_order_release);
    if(!publishSnapshotLocked(buildSnapshotLocked(tracks_,clips_,recordingClips_))){midiSlots_=std::move(old);(*it)->clipRuntime=std::move(oldRuntime);return false;} return true;
}
bool RackGraph::unloadTrackMidiSlot(RackPathId id,uint32_t slot){
    std::lock_guard lock(controlMutex_);
    auto it=std::find_if(tracks_.begin(),tracks_.end(),[&](auto& n){return n->id==id;});
    if(it==tracks_.end())return false; const size_t i=static_cast<size_t>(it-tracks_.begin());
    auto ms=midiSlots_; if(i>=ms.size()||slot>=ms[i].size())return false; ms[i][slot].reset();
    if(i<clipLabelOverrides_.size()&&slot<clipLabelOverrides_[i].size())clipLabelOverrides_[i][slot].clear();
    auto old=midiSlots_; auto oldRuntime=(*it)->clipRuntime; midiSlots_=ms;
    const bool hasOther=slot<wavSlots_[i].size()&&wavSlots_[i][slot];
    if(hasOther){
        (*it)->clipRuntime[slot]->loopLengthQuarterNotes.store((*it)->clipRuntime[slot]->loopLengthBars.load() * 4.0);
    } else if(slot<(*it)->clipRuntime.size()) (*it)->clipRuntime[slot].reset();
    if((*it)->activeSlot.load()==static_cast<int32_t>(slot))(*it)->activeSlot.store(-1);
    if((*it)->pendingSwitchSlot.load()==static_cast<int32_t>(slot)){(*it)->pendingSwitchSlot.store(-1);(*it)->pendingSwitchFrame=std::numeric_limits<uint64_t>::max();}
    if(!publishSnapshotLocked(buildSnapshotLocked(tracks_,clips_,recordingClips_))){midiSlots_=std::move(old);(*it)->clipRuntime=std::move(oldRuntime);return false;}
    return true;
}
bool RackGraph::renameTrackClip(RackPathId id,int32_t slot,const std::string& displayName){if(slot<0||displayName.empty()||displayName.find_first_not_of(" \t\n\r") == std::string::npos)return false;std::lock_guard lock(controlMutex_);auto it=std::find_if(tracks_.begin(),tracks_.end(),[&](auto& n){return n->id==id;});if(it==tracks_.end())return false;const size_t i=static_cast<size_t>(it-tracks_.begin());const size_t s=static_cast<size_t>(slot);if(i>=wavSlots_.size()||i>=midiSlots_.size()||(s>=wavSlots_[i].size()||!wavSlots_[i][s])&&(s>=midiSlots_[i].size()||!midiSlots_[i][s]))return false;if(i>=clipLabelOverrides_.size())clipLabelOverrides_.resize(i+1);if(s>=clipLabelOverrides_[i].size())clipLabelOverrides_[i].resize(s+1);clipLabelOverrides_[i][s]=displayName;return true;}
bool RackGraph::setTrackVolume(RackPathId id,float value){std::lock_guard lock(controlMutex_);auto it=std::find_if(tracks_.begin(),tracks_.end(),[&](auto& n){return n->id==id;});if(it==tracks_.end())return false;(*it)->volume.store(std::clamp(value,0.f,1.f));return true;}
bool RackGraph::setTrackInputArmed(RackPathId id,bool armed){std::lock_guard lock(controlMutex_);auto it=std::find_if(tracks_.begin(),tracks_.end(),[&](auto& n){return n->id==id;});if(it==tracks_.end())return false;(*it)->inputArmed.store(armed);return true;}
bool RackGraph::setTrackInputArmLocked(RackPathId id,bool locked){std::lock_guard lock(controlMutex_);auto it=std::find_if(tracks_.begin(),tracks_.end(),[&](auto& n){return n->id==id;});if(it==tracks_.end())return false;(*it)->inputArmLocked.store(locked);return true;}
bool RackGraph::setTrackInputArmedExclusive(RackPathId id){std::lock_guard lock(controlMutex_);auto it=std::find_if(tracks_.begin(),tracks_.end(),[&](auto& n){return n->id==id;});if(it==tracks_.end())return false;for(auto& node:tracks_)if(node.get()==it->get())node->inputArmed.store(true,std::memory_order_release);else if(!node->inputArmLocked.load(std::memory_order_acquire))node->inputArmed.store(false,std::memory_order_release);return true;}
bool RackGraph::setTrackInputSource(RackPathId id, const TrackInputSource& source) {
    std::lock_guard lock(controlMutex_);
    auto it = std::find_if(tracks_.begin(), tracks_.end(), [&](const auto& n) { return n->id == id; });
    if (it == tracks_.end()) return false;
    if (source.kind == TrackInputSource::Kind::HardwarePair &&
        (source.firstChannel < 0 || (source.firstChannel & 1) != 0)) return false;
    if (source.kind == TrackInputSource::Kind::HardwareMono) {
        const auto channelCount = availableInputChannelCount_.load(std::memory_order_acquire);
        if (source.firstChannel < 0 || source.firstChannel > 7 ||
            (channelCount > 0 && source.firstChannel >= channelCount)) return false;
    }
    const auto old = (*it)->inputSource;
    (*it)->inputSource = source;
    if (!publishSnapshotLocked(buildSnapshotLocked(tracks_, clips_, recordingClips_))) {
        (*it)->inputSource = old;
        return false;
    }
    return true;
}
bool RackGraph::setTrackInputHardwarePair(RackPathId id, int32_t firstChannel) {
    TrackInputSource source;
    source.kind = TrackInputSource::Kind::HardwarePair;
    source.firstChannel = firstChannel;
    return setTrackInputSource(id, source);
}
bool RackGraph::setTrackInputHardwareMono(RackPathId id, int32_t channel) {
    TrackInputSource source;
    source.kind = TrackInputSource::Kind::HardwareMono;
    source.firstChannel = channel;
    return setTrackInputSource(id, source);
}
bool RackGraph::setTrackInputTrack(RackPathId id, RackPathId sourceId, TrackInputTap tap) {
    TrackInputSource source;
    source.kind = TrackInputSource::Kind::TrackOutput;
    source.trackId = sourceId;
    source.tap = tap;
    return setTrackInputSource(id, source);
}
TrackInputSource RackGraph::getTrackInputSource(RackPathId id) const {
    std::lock_guard lock(controlMutex_);
    auto it = std::find_if(tracks_.begin(), tracks_.end(), [&](const auto& n) { return n->id == id; });
    return it == tracks_.end() ? TrackInputSource{} : (*it)->inputSource;
}
void RackGraph::setAvailableInputChannelCount(int32_t count) noexcept {
    availableInputChannelCount_.store(std::max<int32_t>(0, count), std::memory_order_release);
}
bool RackGraph::attachTrackWav(RackPathId id,std::shared_ptr<const WavClip> clip){return attachTrackWavSlot(id,0,std::move(clip));}
bool RackGraph::attachTrackMidi(RackPathId id,std::shared_ptr<const MidiClip> clip){return attachTrackMidiSlot(id,0,std::move(clip));}
bool RackGraph::unloadTrackMidi(RackPathId id){return unloadTrackMidiSlot(id,0);}
bool RackGraph::unloadTrackWav(RackPathId id){return unloadTrackWavSlot(id,0);}
bool RackGraph::clearTrackWavs(){
    std::lock_guard lock(controlMutex_);
    std::vector<size_t> reserved;
    reserved.reserve(tracks_.size());
    for (size_t i = 0; i < tracks_.size(); ++i) {
        for (;;) {
            RecordingPhase original = RecordingPhase::Idle;
            if (reserveIncompleteRecordingLocked(i, original)) {
                if (original == RecordingPhase::Armed ||
                    original == RecordingPhase::Pending ||
                    original == RecordingPhase::Recording) {
                    reserved.push_back(i);
                }
                break;
            }
            if (original != RecordingPhase::Completing &&
                original != RecordingPhase::Cancelling) {
                return false;
            }
            std::this_thread::yield();
        }
    }
    for (size_t i : reserved) {
        (void)clearReservedRecordingLocked(i);
    }
    auto copy = clips_;
    auto recs = recordingClips_;
    for (size_t i = 0; i < copy.size(); ++i) {
        copy[i].reset();
        recs[i].reset();
        const bool wasReserved = std::find(reserved.begin(), reserved.end(), i) != reserved.end();
        if (wasReserved) continue;
        auto& node = *tracks_[i];
        ++node.recordingGeneration;
        node.recordingGenerationActive.store(node.recordingGeneration, std::memory_order_release);
        node.recordingSlot = std::numeric_limits<uint32_t>::max();
        node.recordingSlotAtomic.store(std::numeric_limits<uint32_t>::max(), std::memory_order_release);
        node.recordPending.store(false, std::memory_order_relaxed);
        node.recording.store(false, std::memory_order_relaxed);
        node.punchArmed.store(false, std::memory_order_relaxed);
        node.recordComplete.store(false, std::memory_order_relaxed);
        node.recordingPhase.store(RecordingPhase::Idle, std::memory_order_release);
    }
    if (!publishSnapshotLocked(buildSnapshotLocked(tracks_, copy, recs))) return false;
    clips_ = std::move(copy);
    recordingClips_ = std::move(recs);
    return true;
}
bool RackGraph::startTrackRecordingLocked(RackPathId id,uint32_t slot,double bars,LaunchQuantization q,bool enterOnPunch){
    if(!(bars==.25||bars==1||bars==2||bars==4||bars==8||bars==16)) return false;
    auto it=std::find_if(tracks_.begin(),tracks_.end(),[&](auto& n){return n->id==id;});
    if(it==tracks_.end()) return false;
    auto& n=**it; const size_t i=static_cast<size_t>(it-tracks_.begin()); auto oldRuntime=n.clipRuntime;
    const auto phase = n.recordingPhase.load(std::memory_order_acquire);
    if (recordingClips_[i] && phase != RecordingPhase::Complete) return false;
    if(!n.inputArmed.load() || clips_[i]) return false;
    if(i>=wavSlots_.size() || i>=midiSlots_.size()) return false;
    if(slot<wavSlots_[i].size() && wavSlots_[i][slot]) return false;
    if(slot<midiSlots_[i].size() && midiSlots_[i][slot]) return false;
    if(!ensureClipRuntimeLocked(n,slot)) return false;
    const double rate=sampleRate_.load(), bpm=mailbox_.desiredBpm.load();
    if(rate<=0 || bpm<=0) return false;
    const uint32_t length=static_cast<uint32_t>(std::llround(bars*4.*60./bpm*rate)); if(!length) return false;
    auto rec=std::make_shared<WavClip>(); rec->sampleRate=static_cast<uint32_t>(rate); rec->sourceBpm=bpm; rec->left.resize(length); rec->right.resize(length); rec->displayName="Recorded loop";
    auto recs=recordingClips_; auto slots=wavSlots_; auto oldSlots=wavSlots_; if(slot>=slots[i].size()) slots[i].resize(static_cast<size_t>(slot)+1);
    recs[i]=rec; slots[i][slot]=rec;
    const auto oldSelected=n.selectedSlot.load();
    const auto oldSlot=n.recordingSlot; const auto oldLength=n.recordLength; const auto oldQuant=n.recordQuantization.load();
    const auto oldPending=n.recordPending.load(); const auto oldRecording=n.recording.load(); const auto oldComplete=n.recordComplete.load(); const auto oldPunch=n.punchArmed.load(); const auto oldFrame=n.recordFrame.load(); const auto oldStart=n.recordStartFrame.load(); const auto oldPhase=n.recordingPhase.load(std::memory_order_acquire); const auto oldGeneration=n.recordingGeneration;
    n.selectedSlot.store(slot); n.clipRuntime[slot]=std::make_shared<ClipRuntime>(); n.clipRuntime[slot]->looping.store(true); n.clipRuntime[slot]->loopLengthBars.store(bars); n.recordingSlot=slot; n.recordingSlotAtomic.store(slot, std::memory_order_release); n.recordLength=length; n.recordQuantization.store(static_cast<uint8_t>(q));
    ++n.recordingGeneration;
    n.recordFrame.store(0);
    const uint32_t calibrationFrames=enterOnPunch?std::max<uint32_t>(1,static_cast<uint32_t>(std::ceil(rate*0.01))):0;
    n.punchCalibrationFrames.store(calibrationFrames); n.punchCalibrationRemaining.store(calibrationFrames); n.punchNoiseSum.store(0.0f); n.punchThreshold.store(0.02f);
    const auto transport = getTransportSnapshot();
    const bool running = transport.playing;
    const uint64_t currentFrame = transport.transportFrame;
    const double currentQn = transport.musicalQuarterNotes;
    const uint64_t boundary = running
        ? nextBoundary(currentFrame, currentQn, rate, bpm, q)
        : currentFrame;
    n.recordStartFrame.store(enterOnPunch ? std::numeric_limits<uint64_t>::max() : boundary);
    wavSlots_=slots;
    if(!publishSnapshotLocked(buildSnapshotLocked(tracks_,clips_,recs))){
        wavSlots_=std::move(oldSlots); n.clipRuntime=std::move(oldRuntime);
        n.selectedSlot.store(oldSelected); n.recordingSlot=oldSlot; n.recordingSlotAtomic.store(oldSlot, std::memory_order_release); n.recordLength=oldLength; n.recordQuantization.store(oldQuant);
        n.recordingGeneration=oldGeneration; n.recordPending.store(oldPending); n.recording.store(oldRecording); n.recordingPhase.store(oldPhase, std::memory_order_release); n.punchArmed.store(oldPunch); n.recordComplete.store(oldComplete); n.recordFrame.store(oldFrame); n.recordStartFrame.store(oldStart); return false;
    }
    recordingClips_=std::move(recs);
    n.recordingGenerationActive.store(n.recordingGeneration, std::memory_order_release);
    n.recordComplete.store(false, std::memory_order_relaxed);
    n.recording.store(false, std::memory_order_relaxed);
    n.recordPending.store(!enterOnPunch, std::memory_order_relaxed);
    n.punchArmed.store(enterOnPunch, std::memory_order_relaxed);
    n.recordingPhase.store(
        enterOnPunch ? RecordingPhase::Armed : RecordingPhase::Pending,
        std::memory_order_release);
    return true;
}
bool RackGraph::startTrackClipRecording(RackPathId id,uint32_t slot,LaunchQuantization q){
    std::lock_guard lock(controlMutex_);
    auto it=std::find_if(tracks_.begin(),tracks_.end(),[&](auto& n){return n->id==id;});
    if(it==tracks_.end() || !ensureSlotConfigLocked(*(*it),slot)) return false;
    const double bars=(*it)->slotConfig[slot]->defaultLoopLengthBars.load();
    const bool punch=(*it)->slotConfig[slot]->enterOnPunch.load();
    return startTrackRecordingLocked(id,slot,bars,punch ? static_cast<LaunchQuantization>((*it)->slotConfig[slot]->punchQuantization.load(std::memory_order_relaxed)) : q,punch);
}
bool RackGraph::ensureClipRuntimeLocked(TrackNode& n,uint32_t slot){
    try {
        if (slot >= n.clipRuntime.size()) n.clipRuntime.resize(static_cast<size_t>(slot)+1);
        if (!n.clipRuntime[slot]) {
            n.clipRuntime[slot]=std::make_shared<ClipRuntime>();
            const double bars = slot<n.slotConfig.size()&&n.slotConfig[slot] ? n.slotConfig[slot]->defaultLoopLengthBars.load() : n.defaultLoopLengthBars.load();
            n.clipRuntime[slot]->loopLengthBars.store(bars);
            n.clipRuntime[slot]->loopLengthQuarterNotes.store(bars * 4.0);
        }
        return true;
    } catch(...) { return false; }
}
bool RackGraph::ensureSlotConfigLocked(TrackNode& n,uint32_t slot){
    if(slot<n.slotConfig.size()) return static_cast<bool>(n.slotConfig[slot]);
    try {
        const size_t old=n.slotConfig.size(); n.slotConfig.resize(static_cast<size_t>(slot)+1);
        for(size_t i=old;i<n.slotConfig.size();++i){ n.slotConfig[i]=std::make_shared<SlotConfig>(); n.slotConfig[i]->defaultLoopLengthBars.store(n.defaultLoopLengthBars.load()); }
        return true;
    } catch(...) { return false; }
}
bool RackGraph::setSlotDefaultLoopLength(RackPathId id, uint32_t slot, double bars) {
    if (!(bars == .25 || bars == 1.0 || bars == 2.0 || bars == 4.0 || bars == 8.0 || bars == 16.0)) return false;
    std::lock_guard lock(controlMutex_);
    auto it = std::find_if(tracks_.begin(), tracks_.end(), [&](auto& n) { return n->id == id; });
    if (it == tracks_.end() || !ensureSlotConfigLocked(**it, slot)) return false;
    auto& config = *(*it)->slotConfig[slot];
    const double old = config.defaultLoopLengthBars.load();
    config.defaultLoopLengthBars.store(bars);
    if (publishSnapshotLocked(buildSnapshotLocked(tracks_, clips_, recordingClips_))) return true;
    config.defaultLoopLengthBars.store(old);
    return false;
}
bool RackGraph::setTrackDefaultLoopLength(RackPathId id, double bars) {
    if (!(bars == .25 || bars == 1.0 || bars == 2.0 || bars == 4.0 || bars == 8.0 || bars == 16.0)) return false;
    std::lock_guard lock(controlMutex_);
    auto it = std::find_if(tracks_.begin(), tracks_.end(), [&](auto& n) { return n->id == id; });
    if (it == tracks_.end()) return false;
    auto& node = **it;
    const double old = node.defaultLoopLengthBars.load();
    node.defaultLoopLengthBars.store(bars);
    if (publishSnapshotLocked(buildSnapshotLocked(tracks_, clips_, recordingClips_))) return true;
    node.defaultLoopLengthBars.store(old);
    return false;
}
bool RackGraph::setClipLoopLength(RackPathId id,uint32_t slot,double bars){
    if(!(bars==.25||bars==1||bars==2||bars==4||bars==8||bars==16)) return false;
    std::lock_guard lock(controlMutex_);
    auto it=std::find_if(tracks_.begin(),tracks_.end(),[&](auto& n){return n->id==id;});
    if(it==tracks_.end()) return false;
    auto& n=**it; const size_t i=static_cast<size_t>(it-tracks_.begin());
    if(i>=wavSlots_.size() || i>=midiSlots_.size() || (slot>=wavSlots_[i].size() || !wavSlots_[i][slot]) && (slot>=midiSlots_[i].size() || !midiSlots_[i][slot])) return false;
    if(!ensureClipRuntimeLocked(n,slot)) return false;
    n.clipRuntime[slot]->loopLengthBars.store(bars);
    n.clipRuntime[slot]->loopLengthQuarterNotes.store(bars * 4.0);
    return publishSnapshotLocked(buildSnapshotLocked(tracks_,clips_,recordingClips_));
}
bool RackGraph::setClipLoopStartQuarterNotes(RackPathId id,uint32_t slot,double value){
    if(!std::isfinite(value) || value < 0.0) return false;
    std::lock_guard lock(controlMutex_);
    auto it=std::find_if(tracks_.begin(),tracks_.end(),[&](auto& n){return n->id==id;});
    if(it==tracks_.end()) return false;
    auto& n=**it; const size_t i=static_cast<size_t>(it-tracks_.begin());
    if(i>=wavSlots_.size() || i>=midiSlots_.size() ||
       (slot>=wavSlots_[i].size() || !wavSlots_[i][slot]) &&
       (slot>=midiSlots_[i].size() || !midiSlots_[i][slot])) return false;
    if(!ensureClipRuntimeLocked(n,slot)) return false;
    const double sourceBpm = n.clipRuntime[slot]->sourceBpm.load(std::memory_order_relaxed);
    const double duration = (slot<wavSlots_[i].size()&&wavSlots_[i][slot])
        ? clipDuration(*wavSlots_[i][slot])*sourceBpm/60.0
        : (slot<midiSlots_[i].size()&&midiSlots_[i][slot]
            ? static_cast<double>(midiSlots_[i][slot]->durationMicroseconds)*sourceBpm/60'000'000.0 : 0.0);
    if(value > duration + 1e-9) return false;
    n.clipRuntime[slot]->loopStartQuarterNotes.store(value);
    return publishSnapshotLocked(buildSnapshotLocked(tracks_,clips_,recordingClips_));
}
bool RackGraph::setClipLoopLengthQuarterNotes(RackPathId id,uint32_t slot,double value){
    if(!std::isfinite(value) || value < 0.25) return false;
    std::lock_guard lock(controlMutex_);
    auto it=std::find_if(tracks_.begin(),tracks_.end(),[&](auto& n){return n->id==id;});
    if(it==tracks_.end()) return false;
    auto& n=**it; const size_t i=static_cast<size_t>(it-tracks_.begin());
    if(i>=wavSlots_.size() || i>=midiSlots_.size() ||
       (slot>=wavSlots_[i].size() || !wavSlots_[i][slot]) &&
       (slot>=midiSlots_[i].size() || !midiSlots_[i][slot])) return false;
    if(!ensureClipRuntimeLocked(n,slot)) return false;
    n.clipRuntime[slot]->loopLengthQuarterNotes.store(value);
    return publishSnapshotLocked(buildSnapshotLocked(tracks_,clips_,recordingClips_));
}
bool RackGraph::setSlotEnterOnPunch(RackPathId id,uint32_t slot,bool armed,LaunchQuantization q){
    std::lock_guard lock(controlMutex_);
    auto it=std::find_if(tracks_.begin(),tracks_.end(),[&](auto& n){return n->id==id;});
    if(it==tracks_.end()) return false;
    auto& n=**it; const size_t i=static_cast<size_t>(it-tracks_.begin());
    if(!ensureSlotConfigLocked(n,slot)) return false;
    if(armed){
        if(slot<wavSlots_[i].size() && wavSlots_[i][slot]) return false;
        const auto phase = n.recordingPhase.load(std::memory_order_acquire);
        if (n.recordingSlot != slot &&
            phase != RecordingPhase::Idle &&
            phase != RecordingPhase::Complete) {
            const uint32_t old=n.recordingSlot;
            if (!clearIncompleteRecordingLocked(i)) return false;
            if(old<n.slotConfig.size() && n.slotConfig[old]) n.slotConfig[old]->enterOnPunch.store(false);
        }
        for(size_t s=0;s<n.slotConfig.size();++s) {
            if(n.slotConfig[s]) n.slotConfig[s]->enterOnPunch.store(s==slot);
        }
        n.recordingSlot=slot; n.recordingSlotAtomic.store(slot, std::memory_order_release); n.recordQuantization.store(static_cast<uint8_t>(q)); n.slotConfig[slot]->punchQuantization.store(static_cast<uint8_t>(q),std::memory_order_relaxed);
    } else n.slotConfig[slot]->enterOnPunch.store(false);
    return publishSnapshotLocked(buildSnapshotLocked(tracks_,clips_,recordingClips_));
}
bool RackGraph::setClipLooping(RackPathId id,uint32_t slot,bool looping) {
    std::lock_guard lock(controlMutex_);
    auto it=std::find_if(tracks_.begin(),tracks_.end(),[&](auto& n){return n->id==id;});
    if(it==tracks_.end()) return false;
    const size_t i=static_cast<size_t>(it-tracks_.begin());
    if(i>=wavSlots_.size() || i>=midiSlots_.size() ||
       (slot>=wavSlots_[i].size() || !wavSlots_[i][slot]) &&
       (slot>=midiSlots_[i].size() || !midiSlots_[i][slot])) return false;
    auto& n=**it;
    if(!ensureClipRuntimeLocked(n,slot)) return false;
    auto& rt=*n.clipRuntime[slot];
    if(looping) {
        const double start=rt.loopStartQuarterNotes.load(std::memory_order_relaxed);
        const double sourceBpm=rt.sourceBpm.load(std::memory_order_relaxed);
        const double duration=(slot<wavSlots_[i].size()&&wavSlots_[i][slot])
            ? clipDuration(*wavSlots_[i][slot])*sourceBpm/60.0
            : (slot<midiSlots_[i].size()&&midiSlots_[i][slot]
                ? static_cast<double>(midiSlots_[i][slot]->durationMicroseconds)*sourceBpm/60'000'000.0 : 0.0);
        if(!std::isfinite(start) || start < 0.0 || start > duration + 1e-9) return false;
    }
    rt.looping.store(looping);
    return publishSnapshotLocked(buildSnapshotLocked(tracks_,clips_,recordingClips_));
}
bool RackGraph::setClipTransportPlaying(RackPathId id,uint32_t slot,bool playing,LaunchQuantization q){std::lock_guard lock(controlMutex_);auto it=std::find_if(tracks_.begin(),tracks_.end(),[&](auto& n){return n->id==id;});if(it==tracks_.end())return false;const size_t i=static_cast<size_t>(it-tracks_.begin());if(i>=wavSlots_.size()||i>=midiSlots_.size()||(slot>=wavSlots_[i].size()||!wavSlots_[i][slot])&&(slot>=midiSlots_[i].size()||!midiSlots_[i][slot]))return false;auto& n=**it;if(!ensureClipRuntimeLocked(n,slot))return false;auto& r=*n.clipRuntime[slot];r.desiredPlaying.store(playing);r.desiredQuantization.store(static_cast<uint8_t>(q));if (playing) {
        n.pendingSwitchSlot.store(static_cast<int32_t>(slot), std::memory_order_release);
    } else {
        int32_t expected = static_cast<int32_t>(slot);
        n.pendingSwitchSlot.compare_exchange_strong(expected, -1, std::memory_order_acq_rel,
                                                     std::memory_order_relaxed);
    }
    r.commandSerial.fetch_add(1, std::memory_order_release);return publishSnapshotLocked(buildSnapshotLocked(tracks_,clips_,recordingClips_));}
bool RackGraph::setClipTempoMode(RackPathId id,uint32_t slot,ClipTempoMode mode){
    std::lock_guard lock(controlMutex_);
    auto it=std::find_if(tracks_.begin(),tracks_.end(),[&](auto& n){return n->id==id;});
    if(it==tracks_.end()) return false;
    const size_t i=static_cast<size_t>(it-tracks_.begin());
    if(i>=wavSlots_.size()||i>=midiSlots_.size()||(slot>=wavSlots_[i].size()||!wavSlots_[i][slot])&&(slot>=midiSlots_[i].size()||!midiSlots_[i][slot])) return false;
    if(!ensureClipRuntimeLocked(**it,slot)) return false;
    (*it)->clipRuntime[slot]->tempoMode.store(static_cast<int>(mode),std::memory_order_release);
    return publishSnapshotLocked(buildSnapshotLocked(tracks_,clips_,recordingClips_));
}
bool RackGraph::setClipSourceBpm(RackPathId id, uint32_t slot, double sourceBpm){
    if(!std::isfinite(sourceBpm) || sourceBpm < 20.0 || sourceBpm > 400.0) return false;
    std::lock_guard lock(controlMutex_);
    auto it=std::find_if(tracks_.begin(),tracks_.end(),[&](auto& n){return n->id==id;});
    if(it==tracks_.end()) return false;
    const size_t i=static_cast<size_t>(it-tracks_.begin());
    if(i>=wavSlots_.size() || slot>=wavSlots_[i].size() || !wavSlots_[i][slot]) return false;
    if(!ensureClipRuntimeLocked(**it,slot)) return false;
    (*it)->clipRuntime[slot]->sourceBpm.store(sourceBpm,std::memory_order_release);
    return true;
}
bool RackGraph::reserveIncompleteRecordingLocked(size_t i, RecordingPhase& original) noexcept {
    if (i >= tracks_.size() || i >= recordingClips_.size() || i >= wavSlots_.size()) return false;
    auto& phase = tracks_[i]->recordingPhase;
    original = phase.load(std::memory_order_acquire);
    while (original == RecordingPhase::Armed ||
           original == RecordingPhase::Pending ||
           original == RecordingPhase::Recording) {
        if (phase.compare_exchange_weak(
                original, RecordingPhase::Cancelling,
                std::memory_order_acq_rel, std::memory_order_acquire)) {
            return true;
        }
    }
    return original == RecordingPhase::Idle || original == RecordingPhase::Complete;
}
bool RackGraph::clearReservedRecordingLocked(size_t i) noexcept {
    if (i >= tracks_.size() || i >= recordingClips_.size() || i >= wavSlots_.size()) return false;
    auto& n = *tracks_[i];
    if (n.recordingPhase.load(std::memory_order_acquire) != RecordingPhase::Cancelling) return false;
    ++n.recordingGeneration;
    n.recordingGenerationActive.store(n.recordingGeneration, std::memory_order_release);
    const auto slot = n.recordingSlot;
    if (slot < wavSlots_[i].size() && recordingClips_[i] &&
        wavSlots_[i][slot].get() == recordingClips_[i].get()) {
        wavSlots_[i][slot].reset();
    }
    recordingClips_[i].reset();
    n.recordingSlot = std::numeric_limits<uint32_t>::max();
    n.recordingSlotAtomic.store(std::numeric_limits<uint32_t>::max(), std::memory_order_release);
    n.recordPending.store(false, std::memory_order_relaxed);
    n.recording.store(false, std::memory_order_relaxed);
    n.punchArmed.store(false, std::memory_order_relaxed);
    n.recordComplete.store(false, std::memory_order_release);
    n.recordStartFrame.store(std::numeric_limits<uint64_t>::max(), std::memory_order_relaxed);
    n.recordFrame.store(0, std::memory_order_relaxed);
    n.recordingPhase.store(RecordingPhase::Idle, std::memory_order_release);
    return true;
}
bool RackGraph::clearIncompleteRecordingLocked(size_t i) noexcept {
    RecordingPhase original = RecordingPhase::Idle;
    if (!reserveIncompleteRecordingLocked(i, original) ||
        (original != RecordingPhase::Armed &&
         original != RecordingPhase::Pending &&
         original != RecordingPhase::Recording)) {
        return false;
    }
    return clearReservedRecordingLocked(i);
}
bool RackGraph::cancelTrackLoopRecording(RackPathId id){
    std::lock_guard lock(controlMutex_);
    auto it=std::find_if(tracks_.begin(),tracks_.end(),[&](auto& n){return n->id==id;});
    if(it==tracks_.end()) return false;
    const size_t i=static_cast<size_t>(it-tracks_.begin());
    if (!clearIncompleteRecordingLocked(i)) return false;
    return publishSnapshotLocked(buildSnapshotLocked(tracks_,clips_,recordingClips_));
}
void RackGraph::writeMailboxLocked(bool changePlay,bool playing,bool reset,bool changeBpm,double bpm){mailbox_.sequence.fetch_add(1);if(changePlay){mailbox_.desiredPlaying.store(playing);mailbox_.playSerial.fetch_add(1);}if(changeBpm){mailbox_.desiredBpm.store(std::clamp(std::isfinite(bpm)?bpm:120.,20.,400.));mailbox_.bpmSerial.fetch_add(1);}if(reset)mailbox_.resetSerial.fetch_add(1);mailbox_.sequence.fetch_add(1,std::memory_order_release);}
bool RackGraph::setTransportPlaying(bool playing){
    std::lock_guard lock(controlMutex_);
    bool changed=false;
    if (!playing) {
        for (size_t i=0;i<tracks_.size();++i) {
            if (clearIncompleteRecordingLocked(i)) changed=true;
        }
    }
    if (changed) (void)publishSnapshotLocked(buildSnapshotLocked(tracks_,clips_,recordingClips_));
    writeMailboxLocked(true,playing,false);
    return true;
}
bool RackGraph::restartTransport(){std::lock_guard lock(controlMutex_);writeMailboxLocked(true,true,true);return true;}
void RackGraph::setBeatsPerMinute(double bpm){std::lock_guard lock(controlMutex_);writeMailboxLocked(false,false,false,true,bpm);}
std::vector<float> RackGraph::getTrackWaveformPeaks(RackPathId id, uint32_t maxBuckets) const {
    std::lock_guard lock(controlMutex_);
    constexpr uint32_t kMaxBuckets = 512;
    if (maxBuckets == 0) return {};
    const auto it = std::find_if(tracks_.begin(), tracks_.end(),
                                 [&](const auto& node) { return node->id == id; });
    if (it == tracks_.end()) return {};
    const size_t trackIndex = static_cast<size_t>(it - tracks_.begin());
    const auto phase = (*it)->recordingPhase.load(std::memory_order_acquire);
    if (phase != RecordingPhase::Idle && phase != RecordingPhase::Complete) return {};
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
TransportSnapshot RackGraph::getTransportSnapshot() const {
    TransportSnapshot r{};
    for (;;) {
        const auto before=statusSequence_.load(std::memory_order_acquire);
        if(before&1) continue;
        r.playing=statusPlaying_.load(std::memory_order_relaxed);
        r.positionSec=statusPositionSec_.load(std::memory_order_relaxed);
        r.beatsPerMinute=statusBpm_.load(std::memory_order_relaxed);
        if(!r.playing) r.beatsPerMinute=mailbox_.desiredBpm.load(std::memory_order_relaxed);
        r.samplePosition=statusSamplePosition_.load(std::memory_order_relaxed);
        r.transportFrame=statusTransportFrame_.load(std::memory_order_relaxed);
        r.musicalQuarterNotes=statusMusicalQuarterNotes_.load(std::memory_order_relaxed);
        r.sampleRate=statusSampleRate_.load(std::memory_order_relaxed);
        r.capturedAtMonotonicNanos=statusCapturedAtNanos_.load(std::memory_order_relaxed);
        if(before==statusSequence_.load(std::memory_order_acquire)) return r;
    }
}
std::shared_ptr<PluginChain> RackGraph::getChain(RackPathId id) const{std::lock_guard lock(controlMutex_);if(id==kMasterPathId)return master_;auto it=std::find_if(tracks_.begin(),tracks_.end(),[&](auto& n){return n->id==id;});return it==tracks_.end()?nullptr:(*it)->chain;}
void RackGraph::setSampleRate(float rate,uint32_t buffer){std::lock_guard lock(controlMutex_);sampleRate_.store(rate);bufferSize_=buffer;for(auto& n:tracks_){n->sourceLeft.resize(buffer);n->sourceRight.resize(buffer);n->outputLeft.resize(buffer);n->outputRight.resize(buffer);n->chain->setSampleRate(rate,buffer);}master_->setSampleRate(rate,buffer);(void)publishSnapshotLocked(buildSnapshotLocked(tracks_,clips_,recordingClips_));}
void RackGraph::activate(){std::lock_guard lock(controlMutex_);for(auto& n:tracks_)n->chain->activate();master_->activate();}void RackGraph::deactivate(){std::lock_guard lock(controlMutex_);for(auto& n:tracks_)n->chain->deactivate();master_->deactivate();}void RackGraph::pauseAndResetTransport(){std::lock_guard lock(controlMutex_);writeMailboxLocked(true,false,true);}
RackGraph::State RackGraph::saveState(){std::lock_guard lock(controlMutex_);State s;for(auto& n:tracks_)s.tracks.push_back({n->volume.load(),n->inputArmed.load(),n->chain->saveChainState()});s.master=master_->saveChainState();return s;}
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
    if (reset != appliedResetSerial_) { audioTransportFrame_ = 0; audioElapsedSeconds_ = 0.0; audioMusicalQuarterNotes_ = 0.0; appliedResetSerial_ = reset; }
    if (bpm != appliedBpmSerial_) { audioBpm_ = desiredBpm; appliedBpmSerial_ = bpm; }
}

void RackGraph::publishGlobalStatus(double rate) noexcept { const auto now = static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()); statusPlaying_.store(audioPlaying_, std::memory_order_relaxed); statusBpm_.store(audioBpm_, std::memory_order_relaxed); statusSamplePosition_.store(audioSamplePosition_, std::memory_order_relaxed); statusTransportFrame_.store(audioTransportFrame_, std::memory_order_relaxed); statusMusicalQuarterNotes_.store(audioMusicalQuarterNotes_, std::memory_order_relaxed); statusSampleRate_.store(rate, std::memory_order_relaxed); statusCapturedAtNanos_.store(now, std::memory_order_relaxed); statusPositionSec_.store(audioElapsedSeconds_, std::memory_order_relaxed); statusSequence_.fetch_add(1, std::memory_order_release); }
void RackGraph::advanceTransport(uint32_t frames) noexcept { statusSequence_.fetch_add(1, std::memory_order_acq_rel); applyGlobalMailbox(); double rate=sampleRate_.load(std::memory_order_relaxed); if(rate<=0) rate=48000.; audioSamplePosition_+=frames; if(audioPlaying_){ audioTransportFrame_+=frames; audioElapsedSeconds_ += static_cast<double>(frames) / rate; audioMusicalQuarterNotes_ += static_cast<double>(frames) * audioBpm_ / (rate * 60.0); } publishGlobalStatus(rate); }
uint64_t RackGraph::nextBoundary(uint64_t frame,double quarterNotes,double rate,double bpm,LaunchQuantization q) noexcept {
    if(q==LaunchQuantization::None)return frame;
    const double quantum=q==LaunchQuantization::Bar?4.:q==LaunchQuantization::Quarter?1.:q==LaunchQuantization::Eighth?.5:.25;
    const double position=std::max(0.0,quarterNotes);
    double phase=std::fmod(position,quantum);
    if(phase<0.0)phase+=quantum;
    double remaining=quantum-phase;
    if(remaining<=1e-9)remaining=quantum;
    const uint64_t delta=std::max<uint64_t>(1,static_cast<uint64_t>(std::llround(remaining*rate*60./bpm)));
    return frame+delta;
}
void RackGraph::process(
        const float* const* inputs, int inputChannelCount,
        float* const* outputs, uint32_t frames) noexcept {
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
    statusSequence_.fetch_add(1, std::memory_order_acq_rel);
    uint32_t transportStartOffset = frames;
    applyGlobalMailbox();
    const bool transportPlayingAtBufferStart = audioPlaying_;
    double rate = sampleRate_.load(std::memory_order_relaxed);
    if (rate <= 0.0) rate = 48000.0;
    const double bpm = std::clamp(audioBpm_, 20.0, 400.0);
    const double beatPosition = audioMusicalQuarterNotes_;
    const int64_t bar = static_cast<int64_t>(std::floor(beatPosition / 4.0));
    const AudioProcessContext context{audioSamplePosition_, audioTransportFrame_, 0, rate, bpm,
        audioPlaying_, false, beatPosition, bar, beatPosition - static_cast<double>(bar) * 4.0, beatPosition};
    const bool masterEmpty = snapshot->master->isEmptyForAudio();
    const bool directSingleTrack = masterEmpty && snapshot->tracks.size() == 1;
    bool mixHasData = false;
    for (const uint32_t topoIndex : snapshot->topoOrder) {
        const auto& view = snapshot->tracks[topoIndex];
        auto& node = *view.node;
        const uint32_t slotCount = static_cast<uint32_t>(view.clipRuntime.size());
        int32_t active = node.activeSlot.load(std::memory_order_relaxed);
        int32_t pending = node.pendingSwitchSlot.load(std::memory_order_relaxed);
        uint64_t pendingFrame = node.pendingSwitchFrame;
        if (active < 0 || static_cast<uint32_t>(active) >= slotCount || !view.clipRuntime[static_cast<uint32_t>(active)]) active = -1;
        if (pending < 0 || static_cast<uint32_t>(pending) >= slotCount || !view.clipRuntime[static_cast<uint32_t>(pending)]) {
            pending = -1;
            pendingFrame = std::numeric_limits<uint64_t>::max();
        }
        /* Commands are consumed for every slot, not just the UI selection. */
        for (uint32_t s = 0; s < slotCount; ++s) {
            auto rtPtr = view.clipRuntime[s];
            if (!rtPtr) continue;
            auto& rt = *rtPtr;
            const uint64_t serial = rt.commandSerial.load(std::memory_order_acquire);
            if (serial == rt.appliedCommandSerial) continue;
            rt.appliedCommandSerial = serial;
            const bool want = rt.desiredPlaying.load(std::memory_order_relaxed);
            if (!want) {
                if (active == static_cast<int32_t>(s)) {
                    active = -1;
                    // runtime state is owned by the slot runtime
                }
                if (pending == static_cast<int32_t>(s)) {
                    pending = -1;
                    pendingFrame = std::numeric_limits<uint64_t>::max();
                }
                rt.pendingLaunchFrame = std::numeric_limits<uint64_t>::max();
                rt.statusPlaying.store(false, std::memory_order_relaxed);
                continue;
            }
            const auto q = static_cast<LaunchQuantization>(
                rt.desiredQuantization.load(std::memory_order_relaxed));
            const uint64_t boundary = audioPlaying_ ? nextBoundary(audioTransportFrame_, audioMusicalQuarterNotes_, rate, bpm, q)
                                                     : std::numeric_limits<uint64_t>::max();
            pending = static_cast<int32_t>(s);
            pendingFrame = boundary;
            rt.pendingLaunchFrame = boundary;
            if (active != static_cast<int32_t>(s))
                rt.statusPlaying.store(false, std::memory_order_relaxed);
        }
        if (!audioPlaying_) {
            active = -1;
            pending = -1;
            pendingFrame = std::numeric_limits<uint64_t>::max();
            for (auto& rtPtr : view.clipRuntime) {
                if (!rtPtr) continue;
                auto& rt = *rtPtr;
                rt.pendingLaunchFrame = std::numeric_limits<uint64_t>::max();
                rt.statusPlaying.store(false, std::memory_order_relaxed);
                rt.restartOnLaunch.store(true, std::memory_order_relaxed);
            }
        }
        if (pending >= 0 && pendingFrame != std::numeric_limits<uint64_t>::max() &&
            audioPlaying_ && audioTransportFrame_ >= pendingFrame) {
            if (active >= 0 && static_cast<uint32_t>(active) < slotCount && view.clipRuntime[static_cast<uint32_t>(active)]) {
                auto& old = *view.clipRuntime[static_cast<uint32_t>(active)];
                old.localPlaying = false;
                old.statusPlaying.store(false, std::memory_order_relaxed);
            }
            active = pending;
            pending = -1;
            pendingFrame = std::numeric_limits<uint64_t>::max();
            if (static_cast<uint32_t>(active) < slotCount && view.clipRuntime[static_cast<uint32_t>(active)]) {
                auto& rt = *view.clipRuntime[static_cast<uint32_t>(active)];
                rt.localFrame = 0; // cross-slot replacement always starts at frame zero
                rt.localQuarterNotes.store(0.0, std::memory_order_relaxed);
                rt.localPlaying = true;
                rt.restartOnLaunch.store(false, std::memory_order_relaxed);
                rt.pendingLaunchFrame = std::numeric_limits<uint64_t>::max();
            } else {
                active = -1;
            }
        }
        node.midiScratch.clear();
        float* source[2] = {node.sourceLeft.data(), node.sourceRight.data()};
        const float* routedLeft = nullptr;
        const float* routedRight = nullptr;
        float routedGain = 1.0f;
        if (view.routeIndex >= 0) {
            const auto& sourceNode = *snapshot->tracks[static_cast<uint32_t>(view.routeIndex)].node;
            routedLeft = sourceNode.outputLeft.data();
            routedRight = sourceNode.outputRight.data();
            if (view.inputSource.tap == TrackInputTap::PostFader)
                routedGain = sourceNode.volume.load(std::memory_order_relaxed);
        }
        ClipTempoAdapter adapter;
        const WavClip* wav = nullptr;
        const MidiClip* midi = nullptr;
        uint32_t activeRenderedFrames = 0;
        ClipRuntime* runtime = nullptr;
        if (active >= 0 && static_cast<uint32_t>(active) < slotCount && view.clipRuntime[static_cast<uint32_t>(active)]) {
            const uint32_t s = static_cast<uint32_t>(active);
            runtime = view.clipRuntime[s].get();
            wav = s < view.wavSlots.size() ? view.wavSlots[s].get() : nullptr;
            midi = s < view.midiSlots.size() ? view.midiSlots[s].get() : nullptr;
            if (!wav && view.recordingClip && view.recordingSlot == s &&
                node.recordComplete.load(std::memory_order_acquire)) wav = view.recordingClip.get();
            if (wav && runtime) {
                adapter.configure(static_cast<ClipTempoMode>(runtime->tempoMode.load(std::memory_order_relaxed)),
                    runtime->sourceBpm.load(std::memory_order_relaxed), bpm, wav->sampleRate, static_cast<uint32_t>(rate));
            }
        }
        for (uint32_t frame = 0; frame < frames; ++frame) {
            const uint64_t globalFrame = audioTransportFrame_ + frame;
            if (pending >= 0 && pendingFrame != std::numeric_limits<uint64_t>::max() &&
                audioPlaying_ && globalFrame >= pendingFrame) {
                if (active >= 0 && static_cast<uint32_t>(active) < slotCount && view.clipRuntime[static_cast<uint32_t>(active)]) {
                    auto& old = *view.clipRuntime[static_cast<uint32_t>(active)];
                    old.localPlaying = false;
                    old.statusPlaying.store(false, std::memory_order_relaxed);
                }
                active = pending;
                pending = -1;
                pendingFrame = std::numeric_limits<uint64_t>::max();
                node.activeSlot.store(active, std::memory_order_relaxed);
                if (static_cast<uint32_t>(active) < slotCount && view.clipRuntime[static_cast<uint32_t>(active)]) {
                    runtime = view.clipRuntime[static_cast<uint32_t>(active)].get();
                    runtime->localFrame = 0;
                    runtime->localQuarterNotes.store(0.0, std::memory_order_relaxed);
                    runtime->localPlaying = true;
                    runtime->restartOnLaunch.store(false, std::memory_order_relaxed);
                    runtime->pendingLaunchFrame = std::numeric_limits<uint64_t>::max();
                    wav = static_cast<uint32_t>(active) < view.wavSlots.size()
                        ? view.wavSlots[static_cast<uint32_t>(active)].get() : nullptr;
                    midi = static_cast<uint32_t>(active) < view.midiSlots.size()
                        ? view.midiSlots[static_cast<uint32_t>(active)].get() : nullptr;
                    if (!wav && view.recordingClip && view.recordingSlot == static_cast<uint32_t>(active) &&
                        node.recordComplete.load(std::memory_order_acquire)) wav = view.recordingClip.get();
                    if (wav) adapter.configure(static_cast<ClipTempoMode>(
                        runtime->tempoMode.load(std::memory_order_relaxed)), runtime->sourceBpm.load(std::memory_order_relaxed), bpm,
                        wav->sampleRate, static_cast<uint32_t>(rate));
                } else {
                    active = -1;
                    runtime = nullptr;
                    wav = nullptr;
                    midi = nullptr;
                }
            }
            const bool hardwarePair = !routedLeft && inputs &&
                view.inputSource.kind == TrackInputSource::Kind::HardwarePair;
            const bool hardwareMono = !routedLeft && inputs &&
                view.inputSource.kind == TrackInputSource::Kind::HardwareMono;
            const int32_t firstChannel = view.inputSource.firstChannel;
            const float* hardwareLeft = (hardwarePair || hardwareMono) && firstChannel >= 0 &&
                firstChannel < inputChannelCount ? inputs[firstChannel] : nullptr;
            const float* hardwareRight = hardwarePair && firstChannel >= 0 &&
                firstChannel + 1 < inputChannelCount ? inputs[firstChannel + 1] : nullptr;
            const float liveLeft = routedLeft ? routedLeft[frame] * routedGain :
                (hardwareLeft ? hardwareLeft[frame] : 0.0f);
            const float liveRight = routedRight ? routedRight[frame] * routedGain :
                (hardwareMono ? (hardwareLeft ? hardwareLeft[frame] : 0.0f) :
                    (hardwareRight ? hardwareRight[frame] : 0.0f));
            source[0][frame] = source[1][frame] = 0.0f;
            const bool currentRecordingGeneration =
                view.recordingGeneration ==
                node.recordingGenerationActive.load(std::memory_order_acquire);
            if (currentRecordingGeneration && view.recordingClip &&
                node.punchArmed.load(std::memory_order_relaxed)) {
                const float magnitude = std::max(std::fabs(liveLeft), std::fabs(liveRight));
                auto remaining = node.punchCalibrationRemaining.load(std::memory_order_relaxed);
                if (remaining > 0) {
                    node.punchNoiseSum.store(node.punchNoiseSum.load(std::memory_order_relaxed) + magnitude, std::memory_order_relaxed);
                    --remaining;
                    node.punchCalibrationRemaining.store(remaining, std::memory_order_relaxed);
                    if (remaining == 0) {
                        const auto calibration = std::max<uint32_t>(1, node.punchCalibrationFrames.load(std::memory_order_relaxed));
                        node.punchThreshold.store(std::max(0.02f, node.punchNoiseSum.load(std::memory_order_relaxed) / calibration * 2.0f), std::memory_order_relaxed);
                    }
                } else if (magnitude >= node.punchThreshold.load(std::memory_order_relaxed)) {
                    RecordingPhase expected = RecordingPhase::Armed;
                    if (node.recordingPhase.compare_exchange_strong(expected,
                            (transportPlayingAtBufferStart &&
                             node.recordQuantization.load(std::memory_order_relaxed) !=
                                 static_cast<uint8_t>(LaunchQuantization::None))
                                ? RecordingPhase::Pending : RecordingPhase::Recording,
                            std::memory_order_acq_rel, std::memory_order_relaxed)) {
                        node.punchArmed.store(false, std::memory_order_relaxed);
                        const auto q = static_cast<LaunchQuantization>(node.recordQuantization.load(std::memory_order_relaxed));
                        if (transportPlayingAtBufferStart && q != LaunchQuantization::None) {
                            const double qn = audioMusicalQuarterNotes_ +
                                static_cast<double>(globalFrame-audioTransportFrame_) * bpm/(rate*60.0);
                            node.recordPending.store(true, std::memory_order_relaxed);
                            node.recordStartFrame.store(
                                nextBoundary(globalFrame, qn, rate, bpm, q),
                                std::memory_order_relaxed);
                        } else {
                            node.recordPending.store(false, std::memory_order_relaxed);
                            node.recording.store(true, std::memory_order_relaxed);
                            node.recordFrame.store(0, std::memory_order_relaxed);
                            if (!transportPlayingAtBufferStart) transportStartOffset = std::min<uint32_t>(transportStartOffset, frame);
                            audioPlaying_ = true;
                        }
                    }
            }
            }
            if (currentRecordingGeneration && view.recordingClip &&
                node.recordPending.load(std::memory_order_relaxed) &&
                globalFrame >= node.recordStartFrame.load(std::memory_order_relaxed)) {
                RecordingPhase expected = RecordingPhase::Pending;
                if (node.recordingPhase.compare_exchange_strong(expected, RecordingPhase::Recording,
                        std::memory_order_acq_rel, std::memory_order_relaxed)) {
                    node.recordPending.store(false, std::memory_order_relaxed);
                    node.recording.store(true, std::memory_order_relaxed);
                    node.recordFrame.store(0, std::memory_order_relaxed);
                    if (!audioPlaying_) {
                        transportStartOffset = std::min<uint32_t>(transportStartOffset, frame);
                        audioPlaying_ = true;
                    }
                }
            }
            if (currentRecordingGeneration && view.recordingClip &&
                node.recordingPhase.load(std::memory_order_relaxed) == RecordingPhase::Recording) {
                const uint64_t rf = node.recordFrame.load(std::memory_order_relaxed);
                if (rf < view.recordingClip->left.size()) {
                    view.recordingClip->left[rf] = liveLeft;
                    if (rf < view.recordingClip->right.size()) view.recordingClip->right[rf] = liveRight;
                }
                source[0][frame] = liveLeft;
                source[1][frame] = liveRight;
                node.recordFrame.store(rf + 1, std::memory_order_relaxed);
                if (rf + 1 >= view.recordLength) {
                    RecordingPhase expected = RecordingPhase::Recording;
                    if (node.recordingPhase.compare_exchange_strong(expected, RecordingPhase::Completing,
                            std::memory_order_acq_rel, std::memory_order_relaxed)) {
                        node.recording.store(false, std::memory_order_relaxed);
                        if (view.recordingSlot < slotCount && view.clipRuntime[view.recordingSlot]) {
                            auto& rr = *view.clipRuntime[view.recordingSlot];
                            rr.localPlaying = true; rr.localFrame = 0; rr.statusPlaying.store(true, std::memory_order_relaxed);
                            active = static_cast<int32_t>(view.recordingSlot); pending = -1;
                            node.activeSlot.store(active, std::memory_order_relaxed);
                            runtime = &rr; wav = view.recordingClip.get(); midi = nullptr;
                            if (wav) {
                                rr.sourceBpm.store(wav->sourceBpm, std::memory_order_relaxed);
                                rr.loopLengthQuarterNotes.store(static_cast<double>(view.recordLength) * wav->sourceBpm / (wav->sampleRate * 60.0), std::memory_order_relaxed);
                                adapter.configure(static_cast<ClipTempoMode>(rr.tempoMode.load(std::memory_order_relaxed)), rr.sourceBpm.load(std::memory_order_relaxed), bpm, wav->sampleRate, static_cast<uint32_t>(rate));
                            }
                        }
                        node.recordComplete.store(true, std::memory_order_relaxed);
                        node.recordingPhase.store(RecordingPhase::Complete, std::memory_order_release);
                    }
                }
                continue;
            }
            if (runtime && runtime->localPlaying && wav && !wav->left.empty()) {
                const double startQn = runtime->loopStartQuarterNotes.load(std::memory_order_relaxed);
                const double lengthQn = runtime->loopLengthQuarterNotes.load(std::memory_order_relaxed);
                const bool looping = runtime->looping.load(std::memory_order_relaxed);
                const double sourceBpm = runtime->sourceBpm.load(std::memory_order_relaxed);
                const double timelineBpm = runtime->tempoMode.load(std::memory_order_relaxed) == static_cast<int>(ClipTempoMode::Original)
                    ? sourceBpm : bpm;
                const uint64_t startFrame = looping
                    ? static_cast<uint64_t>(std::llround(startQn * 60.0 * rate / timelineBpm))
                    : 0;
                const uint64_t length = looping
                    ? std::max<uint64_t>(1, static_cast<uint64_t>(std::llround(lengthQn * 60.0 * rate / bpm)))
                    : std::max<uint64_t>(1, static_cast<uint64_t>(std::ceil(
                        adapter.adaptedLengthFrames(static_cast<double>(wav->left.size())))));
                if (runtime->localFrame >= length) {
                    if (looping) runtime->localFrame %= length;
                    else { runtime->localPlaying = false; runtime->statusPlaying.store(false, std::memory_order_relaxed); }
                }
                if (runtime->localPlaying) {
                    adapter.renderStereo(wav->left.data(), wav->right.empty() ? nullptr : wav->right.data(),
                        wav->left.size(), runtime->localFrame + startFrame, source[0][frame], source[1][frame]);
                    ++runtime->localFrame;
                    ++activeRenderedFrames;
                    if (runtime->localFrame >= length && looping) runtime->localFrame %= length;
                }
            } else if (runtime && runtime->localPlaying && midi) {
                const double startQn = runtime->loopStartQuarterNotes.load(std::memory_order_relaxed);
                const double lengthQn = runtime->loopLengthQuarterNotes.load(std::memory_order_relaxed);
                const bool looping = runtime->looping.load(std::memory_order_relaxed);
                const double sourceBpm = runtime->sourceBpm.load(std::memory_order_relaxed);
                const double timelineBpm = runtime->tempoMode.load(std::memory_order_relaxed) == static_cast<int>(ClipTempoMode::Original)
                    ? sourceBpm : bpm;
                const uint64_t startFrame = looping
                    ? static_cast<uint64_t>(std::llround(startQn * 60.0 * rate / timelineBpm)) : 0;
                const uint64_t length = looping
                    ? std::max<uint64_t>(1, static_cast<uint64_t>(std::llround(lengthQn * 60.0 * rate / bpm)))
                    : std::max<uint64_t>(1, static_cast<uint64_t>(std::ceil(
                        static_cast<double>(midi->durationMicroseconds) * sourceBpm / timelineBpm * rate / 1'000'000.0)));
                const uint64_t phase = runtime->localFrame + startFrame;
                for (const auto& timed : midi->events) {
                    const uint64_t eventFrame = static_cast<uint64_t>(
                        static_cast<double>(timed.microseconds) * sourceBpm / timelineBpm * rate / 1'000'000.0);
                    if (eventFrame == phase && node.midiScratch.size() < node.midiScratch.capacity()) {
                        auto event = timed.event;
                        event.frameOffset = frame;
                        node.midiScratch.push_back(event);
                    }
                }
                ++runtime->localFrame;
                ++activeRenderedFrames;
                if (runtime->localFrame >= length) {
                    if (looping) runtime->localFrame %= length;
                    else runtime->localPlaying = false;
                }
            } else if (node.inputArmed.load(std::memory_order_relaxed) &&
                       (hardwarePair || hardwareMono || routedLeft)) {
                source[0][frame] = liveLeft;
                source[1][frame] = liveRight;
            }
        }
        node.activeSlot.store(active, std::memory_order_relaxed);
        node.pendingSwitchSlot.store(pending, std::memory_order_relaxed);
        node.pendingSwitchFrame = pendingFrame;
        for (uint32_t s = 0; s < slotCount; ++s) {
            auto rtPtr = view.clipRuntime[s];
            if (!rtPtr) continue;
            auto& rt = *rtPtr;
            const bool isActive = active == static_cast<int32_t>(s);
            rt.statusPlaying.store(isActive && rt.localPlaying, std::memory_order_relaxed);
            if (isActive && rt.localPlaying) {
                const double deltaQn = static_cast<double>(activeRenderedFrames) * bpm / (rate * 60.0);
                rt.localQuarterNotes.store(
                    rt.localQuarterNotes.load(std::memory_order_relaxed) + deltaQn,
                    std::memory_order_relaxed);
            } else if (!isActive || !rt.localPlaying) {
                rt.localQuarterNotes.store(0.0, std::memory_order_relaxed);
            }
            rt.statusFrame.store(rt.localFrame, std::memory_order_relaxed);
        }
        const float* trackSignal[2] = {source[0], source[1]};
        if (!node.chain->isEmptyForAudio()) {
            float* trackOutput[2] = {directSingleTrack ? outputs[0] : node.outputLeft.data(),
                directSingleTrack ? outputs[1] : node.outputRight.data()};
            std::array<MidiEvent, 128> midiOutputScratch{};
            node.chain->process(source, trackOutput, frames, context, node.midiScratch.data(),
                static_cast<uint32_t>(node.midiScratch.size()), midiOutputScratch.data(),
                static_cast<uint32_t>(midiOutputScratch.size()));
            trackSignal[0] = trackOutput[0];
            trackSignal[1] = trackOutput[1];
        } else if (!directSingleTrack) {
            std::memcpy(node.outputLeft.data(), source[0], frames * sizeof(float));
            std::memcpy(node.outputRight.data(), source[1], frames * sizeof(float));
        }
        const float volume = node.volume.load(std::memory_order_relaxed);
        if (directSingleTrack) copyScaled(outputs[0], outputs[1], trackSignal[0], trackSignal[1], frames, volume);
        else if (!mixHasData) { copyScaled(snapshot->mixLeft.data(), snapshot->mixRight.data(), trackSignal[0], trackSignal[1], frames, volume); mixHasData = true; }
        else mixScaled(snapshot->mixLeft.data(), snapshot->mixRight.data(), trackSignal[0], trackSignal[1], frames, volume);
    }
    if (!directSingleTrack) {
        if (!mixHasData) {
            std::memset(snapshot->mixLeft.data(), 0, frames * sizeof(float));
            std::memset(snapshot->mixRight.data(), 0, frames * sizeof(float));
        }
        const float* mix[2] = {snapshot->mixLeft.data(), snapshot->mixRight.data()};
        std::array<MidiEvent, 128> masterMidiOutput{};
        if (masterEmpty) copyScaled(outputs[0], outputs[1], mix[0], mix[1], frames, 1.0f);
        else snapshot->master->process(
            mix, outputs, frames, context, nullptr, 0,
            masterMidiOutput.data(), masterMidiOutput.size());
    }
    audioSamplePosition_ += frames;
    const uint32_t transportFrames = transportPlayingAtBufferStart ? frames :
        (transportStartOffset < frames ? frames - transportStartOffset : 0);
    if (audioPlaying_) {
        audioTransportFrame_ += transportFrames;
        audioElapsedSeconds_ += static_cast<double>(transportFrames) / rate;
        audioMusicalQuarterNotes_ += static_cast<double>(transportFrames) * bpm / (rate * 60.0);
    }
    publishGlobalStatus(rate);
    hazardSnapshot_.store(nullptr, std::memory_order_seq_cst);
    }
void RackGraph::reclaimerLoop(){std::unique_lock lock(reclaimerMutex_);while(!reclaimerStop_){reclaimerWake_.wait_for(lock,std::chrono::milliseconds(10));lock.unlock();reclaimRetired();lock.lock();}}
void RackGraph::reclaimRetired(){std::lock_guard lock(reclaimerMutex_);auto* hazard=hazardSnapshot_.load(std::memory_order_seq_cst);RetiredSnapshot** cursor=&retired_;while(*cursor){auto* item=*cursor;if(item->owner.get()!=hazard){*cursor=item->next;delete item;}else cursor=&item->next;}}
} // namespace guitarrackcraft
