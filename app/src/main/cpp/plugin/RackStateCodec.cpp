/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * This file is part of NNAGA. NNAGA is free software under GPLv3+.
 */
#include "RackStateCodec.h"
#include <cstring>
#include <limits>
#include <cmath>
#include <unordered_set>
namespace guitarrackcraft {
namespace {
constexpr uint32_t kMaxItems=4096, kMaxString=1u<<20, kMaxProperty=8u<<20;
struct W { std::vector<uint8_t> b; bool ok=true; void u8(uint8_t x){b.push_back(x);} void u32(uint32_t x){for(int i=0;i<4;i++)u8(uint8_t(x>>(i*8)));} void u64(uint64_t x){for(int i=0;i<8;i++)u8(uint8_t(x>>(i*8)));} void f(float x){uint32_t v;std::memcpy(&v,&x,4);u32(v);} void d(double x){uint64_t v;std::memcpy(&v,&x,8);u64(v);} void bytes(const std::vector<uint8_t>& x){u32(uint32_t(x.size()));b.insert(b.end(),x.begin(),x.end());} void str(const std::string& x){u32(uint32_t(x.size()));b.insert(b.end(),x.begin(),x.end());}};
struct R {const uint8_t*p;size_t n=0,o=0;bool ok=true; uint8_t u8(){if(o>=n){ok=false;return 0;}return p[o++];} uint32_t u32(){uint32_t x=0;for(int i=0;i<4;i++)x|=uint32_t(u8())<<(i*8);return x;} uint64_t u64(){uint64_t x=0;for(int i=0;i<8;i++)x|=uint64_t(u8())<<(i*8);return x;} float f(){uint32_t v=u32();float x;std::memcpy(&x,&v,4);return x;} double d(){uint64_t v=u64();double x;std::memcpy(&x,&v,8);return x;} bool bytes(std::vector<uint8_t>& x,uint32_t max=kMaxProperty){auto z=u32();if(!ok||z>max||z>n-o){ok=false;return false;}x.assign(p+o,p+o+z);o+=z;return true;} bool str(std::string& x){auto z=u32();if(!ok||z>kMaxString||z>n-o){ok=false;return false;}x.assign(reinterpret_cast<const char*>(p+o),z);o+=z;return validUtf8(x); } static bool validUtf8(const std::string&s){for(size_t i=0;i<s.size();){uint8_t c=s[i++];size_t k=(c<0x80?0:(c>=0xc2&&c<=0xdf?1:c>=0xe0&&c<=0xef?2:c>=0xf0&&c<=0xf4?3:9));if(k==9||i+k>s.size())return false;for(size_t j=0;j<k;j++)if((uint8_t(s[i+j])&0xc0)!=0x80)return false;if((k==2&&c==0xe0&&(uint8_t)s[i]<0xa0)||(k==2&&c==0xed&&(uint8_t)s[i]>=0xa0)||(k==3&&c==0xf0&&(uint8_t)s[i]<0x90)||(k==3&&c==0xf4&&(uint8_t)s[i]>=0x90))return false;i+=k;}return true;}};
uint32_t crc(const uint8_t*p,size_t n){uint32_t c=0xffffffff;for(size_t i=0;i<n;i++){c^=p[i];for(int j=0;j<8;j++)c=(c>>1)^(0xedb88320u&-(c&1));}return ~c;}
void chainW(W&w,const PluginChain::ChainState&c){w.u32(c.plugins.size());for(auto&p:c.plugins){w.str(p.format);w.str(p.pluginUri);w.u32(p.controlPortValues.size());for(auto&q:p.controlPortValues){w.u32(q.first);w.f(q.second);}w.u32(p.properties.size());for(auto&q:p.properties){w.str(q.keyUri);w.str(q.typeUri);w.u32(q.flags);w.bytes(q.value);}w.u32(p.manualLatencyFrames);}}
bool chainR(R& reader, PluginChain::ChainState& chain, bool hasManualLatency) {
    const auto pluginCount = reader.u32();
    if (!reader.ok || pluginCount > kMaxItems) return false;
    chain.plugins.clear();
    chain.plugins.reserve(pluginCount);
    for (uint32_t pluginIndex = 0; pluginIndex < pluginCount; ++pluginIndex) {
        PluginState plugin;
        if (!reader.str(plugin.format) || !reader.str(plugin.pluginUri)) return false;
        const auto controlCount = reader.u32();
        if (!reader.ok || controlCount > kMaxItems) return false;
        plugin.controlPortValues.reserve(controlCount);
        for (uint32_t controlIndex = 0; controlIndex < controlCount; ++controlIndex) {
            const uint32_t port = reader.u32();
            const float value = reader.f();
            if (!reader.ok || !std::isfinite(value)) return false;
            plugin.controlPortValues.emplace_back(port, value);
        }
        const auto propertyCount = reader.u32();
        if (!reader.ok || propertyCount > kMaxItems) return false;
        plugin.properties.reserve(propertyCount);
        for (uint32_t propertyIndex = 0; propertyIndex < propertyCount; ++propertyIndex) {
            StateProperty property;
            if (!reader.str(property.keyUri) || !reader.str(property.typeUri)) return false;
            property.flags = reader.u32();
            if (!reader.bytes(property.value)) return false;
            plugin.properties.push_back(std::move(property));
        }
        if (hasManualLatency) {
            plugin.manualLatencyFrames = reader.u32();
            if (!reader.ok ||
                plugin.manualLatencyFrames > PluginChain::kMaxSupportedPdcFrames)
                return false;
        }
        chain.plugins.push_back(std::move(plugin));
    }
    return reader.ok;
}
}
std::vector<uint8_t> RackStateCodec::encode(const RackGraph::State&s,std::string*e){W w;w.b.insert(w.b.end(),{'N','N','G','S'});w.u32(2);w.u32(s.tracks.size());for(auto&t:s.tracks){w.u64(t.id);w.f(t.volume);w.u8(t.inputArmed);w.u8(t.inputArmLocked);w.u8(uint8_t(t.inputSource.kind));w.u8(uint8_t(t.inputSource.tap));w.u32(uint32_t(t.inputSource.firstChannel));w.u64(t.inputSource.trackId);chainW(w,t.chain);}chainW(w,s.master);w.d(s.beatsPerMinute);w.u8(s.transportPlaying);w.u64(s.transportFrame);w.u64(s.samplePosition);w.d(s.musicalQuarterNotes);if(w.b.size()>kMaxBlobBytes-4){if(e)*e="state-too-large";return{};}w.u32(crc(w.b.data(),w.b.size()));return std::move(w.b);}
bool RackStateCodec::decode(const uint8_t*d,size_t z,RackGraph::State&s,std::string&e){if(!d||z<12||z>kMaxBlobBytes){e="invalid-size";return false;}if(std::memcmp(d,"NNGS",4)||(d[4] != 1 && d[4] != 2)||d[5]||d[6]||d[7]){e="unsupported-header";return false;}uint32_t got=uint32_t(d[z-4])|uint32_t(d[z-3])<<8|uint32_t(d[z-2])<<16|uint32_t(d[z-1])<<24;if(crc(d,z-4)!=got){e="crc-mismatch";return false;}R r{d,z-4,0,true};r.o=8;auto n=r.u32();if(!r.ok||n>kMaxItems){e="invalid-track-count";return false;}RackGraph::State out;for(uint32_t i=0;i<n;i++){RackGraph::State::Track t;t.id=r.u64();t.volume=r.f();t.inputArmed=r.u8();t.inputArmLocked=r.u8();t.inputSource.kind=TrackInputSource::Kind(r.u8());t.inputSource.tap=TrackInputTap(r.u8());t.inputSource.firstChannel=int32_t(r.u32());t.inputSource.trackId=r.u64();if(uint8_t(t.inputSource.kind)>2||uint8_t(t.inputSource.tap)>1||!std::isfinite(t.volume)||!chainR(r, t.chain, d[4] == 2)){e="invalid-track";return false;}out.tracks.push_back(std::move(t));}if(!chainR(r, out.master, d[4] == 2)){e="invalid-master";return false;}out.beatsPerMinute=r.d();out.transportPlaying=r.u8();out.transportFrame=r.u64();out.samplePosition=r.u64();out.musicalQuarterNotes=r.d();if(!r.ok||r.o!=z-4||!std::isfinite(out.beatsPerMinute)||out.beatsPerMinute<=0||out.beatsPerMinute>1000||!std::isfinite(out.musicalQuarterNotes)){e="truncated-or-invalid";return false;}s=std::move(out);return true;}
std::vector<uint8_t> RackStateCodec::encodeDeviceChain(
        RackPathId pathId, const PluginChain::ChainState& chain, std::string* error) {
    RackGraph::State state;
    RackGraph::State::Track scoped;
    scoped.id = pathId;
    scoped.chain = chain;
    state.tracks.push_back(std::move(scoped));
    return encode(state, error);
}

bool RackStateCodec::decodeDeviceChain(
        const uint8_t* data, size_t size, RackPathId& pathId,
        PluginChain::ChainState& chain, std::string& error) {
    RackGraph::State state;
    if (!decode(data, size, state, error)) return false;
    if (state.tracks.size() != 1 || !state.master.plugins.empty()) {
        error = "not-device-chain";
        return false;
    }
    pathId = state.tracks.front().id;
    chain = std::move(state.tracks.front().chain);
    error.clear();
    return true;
}
}
