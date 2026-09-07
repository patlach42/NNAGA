#ifndef VSTPOC_VST3_MIDI_CONVERSION_H
#define VSTPOC_VST3_MIDI_CONVERSION_H
#include <cstddef>
#include <cstdint>
#include <cstring>
namespace vstpoc::vst3 {
constexpr std::size_t kMaxEvents=128,kMaxPayload=65536;
struct Message{uint32_t frame=0,size=0;const uint8_t*bytes=nullptr;};
inline bool isChannelVoice(const uint8_t*b,std::size_t n)noexcept{if(!b||n<2||b[0]<0x80||b[0]>=0xF0)return false;const uint8_t h=b[0]&0xF0;return n==((h==0xC0||h==0xD0)?2u:3u);}
inline bool isSysEx(const uint8_t*b,std::size_t n)noexcept{return b&&n>=2&&b[0]==0xF0&&b[n-1]==0xF7;}
inline bool isSupportedInput(const uint8_t*b,std::size_t n)noexcept{return isChannelVoice(b,n)||isSysEx(b,n);}
class MessageBuffer final{
public:
 void clear()noexcept{count_=payloadSize_=0;rejected_=0;}
 bool append(uint32_t f,const uint8_t*b,std::size_t n)noexcept{if(!b||!n||n>kMaxPayload||count_==kMaxEvents||payloadSize_>kMaxPayload-n){++rejected_;return false;}std::memcpy(payload_+payloadSize_,b,n);messages_[count_++]={f,(uint32_t)n,payload_+payloadSize_};payloadSize_+=n;return true;}
 bool append(const Message&m)noexcept{return append(m.frame,m.bytes,m.size);}
 void copyFrom(const MessageBuffer&src)noexcept{clear();for(std::size_t i=0;i<src.count_;++i)append(src.messages_[i]);rejected_=src.rejected_;}
 void mergeInto(MessageBuffer&out,const MessageBuffer&other)const noexcept{out.clear();std::size_t a=0,b=0;while(a<count_||b<other.count_){const bool takeA=b==other.count_||(a<count_&&messages_[a].frame<=other.messages_[b].frame);out.append(takeA?messages_[a++]:other.messages_[b++]);}out.rejected_=rejected_+other.rejected_+out.rejected_;}
 std::size_t size()const noexcept{return count_;}const Message&at(std::size_t i)const noexcept{return messages_[i];}uint64_t rejected()const noexcept{return rejected_;}void recordRejected()noexcept{++rejected_;}
private:Message messages_[kMaxEvents]{};uint8_t payload_[kMaxPayload]{};std::size_t count_=0,payloadSize_=0;uint64_t rejected_=0;
};}
#endif
