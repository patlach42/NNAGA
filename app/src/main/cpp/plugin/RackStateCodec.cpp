#include "RackStateCodec.h"
#include <cstring>
#include <cmath>
#include <algorithm>
namespace guitarrackcraft { namespace {
constexpr uint32_t M=4096,S=1u<<20,P=8u<<20;
struct W{std::vector<uint8_t>b;void u8(uint8_t x){b.push_back(x);}void u32(uint32_t x){for(int i=0;i<4;i++)u8(x>>(i*8));}void u64(uint64_t x){for(int i=0;i<8;i++)u8(x>>(i*8));}void f(float x){uint32_t v;memcpy(&v,&x,4);u32(v);}void d(double x){uint64_t v;memcpy(&v,&x,8);u64(v);}void str(const std::string&x){u32(x.size());b.insert(b.end(),x.begin(),x.end());}void bytes(const std::vector<uint8_t>&x){u32(x.size());b.insert(b.end(),x.begin(),x.end());}};
struct R{const uint8_t*p;size_t n,o=0;bool ok=1;uint8_t u8(){if(o>=n){ok=0;return 0;}return p[o++];}uint32_t u32(){uint32_t x=0;for(int i=0;i<4;i++)x|=uint32_t(u8())<<(i*8);return x;}uint64_t u64(){uint64_t x=0;for(int i=0;i<8;i++)x|=uint64_t(u8())<<(i*8);return x;}float f(){uint32_t v=u32();float x;memcpy(&x,&v,4);return x;}double d(){uint64_t v=u64();double x;memcpy(&x,&v,8);return x;}bool str(std::string&x){auto z=u32();if(!ok||z>S||z>n-o){ok=0;return 0;}x.assign((const char*)p+o,z);o+=z;return 1;}bool bytes(std::vector<uint8_t>&x){auto z=u32();if(!ok||z>P||z>n-o){ok=0;return 0;}x.assign(p+o,p+o+z);o+=z;return 1;}};
uint32_t crc(const uint8_t*p,size_t n){uint32_t c=~0u;for(size_t i=0;i<n;i++){c^=p[i];for(int j=0;j<8;j++)c=(c>>1)^(0xedb88320u&-(c&1));}return ~c;}
void cw(W&w,const PluginChain::ChainState&c){w.u32(c.plugins.size());for(auto&p:c.plugins){w.str(p.format);w.str(p.pluginUri);w.u32(p.controlPortValues.size());for(auto&q:p.controlPortValues){w.u32(q.first);w.f(q.second);}w.u32(p.properties.size());for(auto&q:p.properties){w.str(q.keyUri);w.str(q.typeUri);w.u32(q.flags);w.bytes(q.value);}w.u32(p.manualLatencyFrames);}}
bool cr(R&r,PluginChain::ChainState&c,bool lat){auto n=r.u32();if(!r.ok||n>M)return 0;c.plugins.clear();for(uint32_t i=0;i<n;i++){PluginState p;if(!r.str(p.format)||!r.str(p.pluginUri))return 0;auto q=r.u32();if(!r.ok||q>M)return 0;for(uint32_t j=0;j<q;j++){auto k=r.u32();auto v=r.f();if(!r.ok||!std::isfinite(v))return 0;p.controlPortValues.emplace_back(k,v);}q=r.u32();if(!r.ok||q>M)return 0;for(uint32_t j=0;j<q;j++){StateProperty x;if(!r.str(x.keyUri)||!r.str(x.typeUri))return 0;x.flags=r.u32();if(!r.bytes(x.value))return 0;p.properties.push_back(std::move(x));}if(lat){p.manualLatencyFrames=r.u32();if(!r.ok||p.manualLatencyFrames>PluginChain::kMaxSupportedPdcFrames)return 0;}c.plugins.push_back(std::move(p));}return r.ok;}
}
std::vector<uint8_t> RackStateCodec::encode(const RackGraph::State&s,std::string*e){W w;w.b.insert(w.b.end(),{'N','N','G','S'});w.u32(4);w.u32(s.tracks.size());for(auto&t:s.tracks){w.u64(t.id);w.f(t.volume);w.u8(t.inputArmed);w.u8(t.inputArmLocked);w.u8((uint8_t)t.inputSource.kind);w.u8((uint8_t)t.inputSource.tap);w.u32(t.inputSource.firstChannel);w.u64(t.inputSource.trackId);w.u32(t.selectedSlot);w.d(t.defaultLoopLengthBars);w.str(t.name);w.u32(t.colorArgb);w.u32(t.clipSlots.size());for(auto&c:t.clipSlots){w.u32(c.slot);w.u8(c.wav);w.u8(c.midi);w.str(c.assetId);w.str(c.midiAssetId);w.str(c.displayName);w.d(c.sourceBpm);w.u32(c.tempoMode);w.u8(c.looping);w.d(c.loopLengthBars);w.d(c.defaultLoopLengthBars);w.d(c.loopStartQuarterNotes);w.d(c.loopLengthQuarterNotes);w.u8(c.enterOnPunch);w.u8((uint8_t)c.launchQuantization);}cw(w,t.chain);}cw(w,s.master);w.d(s.beatsPerMinute);w.u8(s.transportPlaying);w.u64(s.transportFrame);w.u64(s.samplePosition);w.d(s.musicalQuarterNotes);if(w.b.size()>kMaxBlobBytes-4){if(e)*e="state-too-large";return{};}w.u32(crc(w.b.data(),w.b.size()));return std::move(w.b);}
bool RackStateCodec::decode(const uint8_t*d,size_t z,RackGraph::State&s,std::string&e){
    if(!d||z<12||z>kMaxBlobBytes||memcmp(d,"NNGS",4)||d[5]||d[6]||d[7]||(d[4]!=1&&d[4]!=2&&d[4]!=3&&d[4]!=4)){e="unsupported-header";return 0;}
    uint32_t got=d[z-4]|d[z-3]<<8|d[z-2]<<16|d[z-1]<<24;
    if(crc(d,z-4)!=got){e="crc-mismatch";return 0;}
    R r{d,z-4};r.o=8;auto n=r.u32();
    if(!r.ok||n>M){e="invalid-track-count";return 0;}
    RackGraph::State o;
    for(uint32_t i=0;i<n;i++){
        RackGraph::State::Track t;
        t.id=r.u64();t.volume=r.f();t.inputArmed=r.u8();t.inputArmLocked=r.u8();
        t.inputSource.kind=TrackInputSource::Kind(r.u8());t.inputSource.tap=TrackInputTap(r.u8());
        t.inputSource.firstChannel=r.u32();t.inputSource.trackId=r.u64();
        if(d[4]>=3){t.selectedSlot=r.u32();t.defaultLoopLengthBars=r.d();}
        if(d[4]>=4){
            if(!r.str(t.name)){e="invalid-track";return 0;}
            if(!isValidTrackName(t.name)){e="invalid-track-name";return 0;}
            t.colorArgb=r.u32();
        }
        if(d[4]>=3){
            auto q=r.u32();if(!r.ok||q>M){e="invalid-clips";return 0;}
            for(uint32_t j=0;j<q;j++){
                RackGraph::State::ClipSlot c;c.slot=r.u32();c.wav=r.u8();c.midi=r.u8();
                if(!r.str(c.assetId)||!r.str(c.midiAssetId)||!r.str(c.displayName))return 0;
                c.sourceBpm=r.d();c.tempoMode=r.u32();c.looping=r.u8();c.loopLengthBars=r.d();
                c.defaultLoopLengthBars=r.d();c.loopStartQuarterNotes=r.d();c.loopLengthQuarterNotes=r.d();
                c.enterOnPunch=r.u8();c.launchQuantization=LaunchQuantization(r.u8());t.clipSlots.push_back(std::move(c));
            }
        }
        if(uint8_t(t.inputSource.kind)>2||uint8_t(t.inputSource.tap)>1||!std::isfinite(t.volume)||!cr(r,t.chain,d[4]>=2)){e="invalid-track";return 0;}
        o.tracks.push_back(std::move(t));
    }
    if(!cr(r,o.master,d[4]>=2)){e="invalid-master";return 0;}
    o.beatsPerMinute=r.d();o.transportPlaying=r.u8();o.transportFrame=r.u64();o.samplePosition=r.u64();o.musicalQuarterNotes=r.d();
    if(!r.ok||r.o!=z-4||!std::isfinite(o.beatsPerMinute)||o.beatsPerMinute<=0||o.beatsPerMinute>1000||!std::isfinite(o.musicalQuarterNotes)){e="truncated-or-invalid";return 0;}
    s=std::move(o);return 1;
}
std::vector<uint8_t> RackStateCodec::encodeDeviceChain(RackPathId id,const PluginChain::ChainState&c,std::string*e){RackGraph::State s;RackGraph::State::Track t;t.id=id;t.chain=c;s.tracks.push_back(std::move(t));return encode(s,e);}
bool RackStateCodec::decodeDeviceChain(const uint8_t*d,size_t z,RackPathId&id,PluginChain::ChainState&c,std::string&e){
    RackGraph::State s;
    if(!decode(d,z,s,e)) return 0;
    if(s.tracks.size()!=1||!s.master.plugins.empty()||!s.tracks[0].clipSlots.empty()){e="not-device-chain";return 0;}
    id=s.tracks[0].id;c=std::move(s.tracks[0].chain);return 1;
}
}
