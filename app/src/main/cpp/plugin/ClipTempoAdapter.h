#ifndef GUITARRACKCRAFT_CLIP_TEMPO_ADAPTER_H
#define GUITARRACKCRAFT_CLIP_TEMPO_ADAPTER_H
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
namespace guitarrackcraft {
enum class ClipTempoMode : uint8_t { Original=0, Stretch=1, Repitch=2 };
class ClipTempoAdapter final {
public:
 static constexpr uint32_t kHopFrames=512;
 void configure(ClipTempoMode m,double src,double dst,uint32_t sr,uint32_t orate) noexcept { mode_=m; sourceBpm_=(std::isfinite(src)&&src>0)?src:120.; targetBpm_=(std::isfinite(dst)&&dst>0)?dst:sourceBpm_; sourceRate_=sr?sr:orate; outputRate_=orate?orate:sourceRate_; ratio_=targetBpm_/sourceBpm_; if(!(ratio_>0)||!std::isfinite(ratio_))ratio_=1.; }
 double adaptedLengthFrames(double sourceFrames) const noexcept { if(!(sourceFrames>0))return 0.; const double base=sourceFrames*outputRate_/sourceRate_; return mode_==ClipTempoMode::Original?base:base/ratio_; }
 void renderStereo(const float* l,const float* r,size_t count,uint64_t outputFrame,float& ol,float& orr) const noexcept { ol=orr=0.; if(!l||!count)return; const double unit=double(sourceRate_)/outputRate_; double p=double(outputFrame)*unit; if(mode_==ClipTempoMode::Repitch)p*=ratio_; if(mode_==ClipTempoMode::Stretch){const uint64_t grain=outputFrame/kHopFrames; const uint32_t local=outputFrame%kHopFrames; const double cur=double(grain)*kHopFrames*ratio_*unit+double(local)*unit; if(grain){const double prev=double(grain-1)*kHopFrames*ratio_*unit+double(kHopFrames+local)*unit; float aL,aR,bL,bR; sample(l,r,count,prev,aL,aR);sample(l,r,count,cur,bL,bR);const float w=float(local)/kHopFrames;ol=aL*(1-w)+bL*w;orr=aR*(1-w)+bR*w;return;} p=cur;} sample(l,r,count,p,ol,orr); }
 ClipTempoMode mode() const noexcept{return mode_;} double ratio() const noexcept{return ratio_;}
private:
 static void sample(const float* l,const float* r,size_t n,double p,float& ol,float& orr) noexcept { ol=orr=0.0f; if(p<0||p>=double(n))return;size_t i=std::min(size_t(p),n-1),j=std::min(i+1,n-1);float f=float(p-i);ol=l[i]+(l[j]-l[i])*f;orr=r?r[i]+(r[j]-r[i])*f:ol; }
 ClipTempoMode mode_{ClipTempoMode::Original}; double sourceBpm_{120},targetBpm_{120},ratio_{1}; uint32_t sourceRate_{48000},outputRate_{48000};
};
}
#endif
