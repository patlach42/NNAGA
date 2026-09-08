#include <atomic>
#include <array>
#include <cstdio>
#include <ctime>
#include <cstdint>
static inline uint64_t ns(){ timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec*1000000000ull+t.tv_nsec; }
static const int K=256;
static std::array<std::atomic<bool>,K> dirty{};
static std::array<std::atomic<float>,K> pending{};
static std::array<std::atomic<uint64_t>,4> mask{};
volatile float sink;
int main(){
    const int N=200000;
    // current: 256 atomic exchange (ldaxrb/stlxrb) per block
    uint64_t a=ns();
    for(int b=0;b<N;b++)
        for(int i=0;i<K;i++)
            if(dirty[i].exchange(false,std::memory_order_acq_rel))
                sink=pending[i].load(std::memory_order_relaxed);
    uint64_t t1=ns()-a;
    // proposed: 4 atomic word exchanges, bit-scan only what changed
    a=ns();
    for(int b=0;b<N;b++)
        for(int g=0;g<4;g++){
            uint64_t m=mask[g].load(std::memory_order_acquire);
            if(!m) continue;
            m=mask[g].exchange(0,std::memory_order_acq_rel);
            while(m){ int i=__builtin_ctzll(m); m&=m-1; sink=pending[(g<<6)+i].load(std::memory_order_relaxed); }
        }
    uint64_t t2=ns()-a;
    printf("per-block slider scan:  current(256 atomic xchg) = %6.3f us   proposed(4 word loads) = %6.3f us  -> %.0fx\n",
        (double)t1/N/1000.0,(double)t2/N/1000.0,(double)t1/t2);
    return 0;
}
