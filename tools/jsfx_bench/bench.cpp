#include <ysfx.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <vector>
#include <algorithm>
#include <cstdint>
#include <string>
extern "C" void ysfx_prealloc_ram(ysfx_t*);

static inline uint64_t ns(){ timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec*1000000000ull+t.tv_nsec; }
static inline uint64_t fpcr_rd(){ uint64_t v; __asm__ __volatile__("mrs %0, fpcr":"=r"(v)); return v; }
static inline void fpcr_wr(uint64_t v){ __asm__ __volatile__("msr fpcr, %0"::"r"(v)); }

// spin the core so DVFS is at a steady high point before every measurement
static void dvfs_warm(int ms){
    uint64_t end=ns()+(uint64_t)ms*1000000ull; volatile double x=1.0;
    while(ns()<end) for(int i=0;i<1000;i++) x=x*1.0000001+0.5;
}

struct Stats { double p50,p95,p999,max,mean; };
static Stats stats(std::vector<double> v){
    std::sort(v.begin(),v.end());
    double s=0; for(double x:v) s+=x;
    return { v[v.size()/2], v[(size_t)(v.size()*0.95)], v[(size_t)(v.size()*0.999)], v.back(), s/v.size() };
}
static void report(const char* tag,std::vector<double>& t,int block){
    Stats s=stats(t); double budget=1e6*block/48000.0;
    printf("  %-34s p50=%7.2f p95=%7.2f p99.9=%8.2f max=%9.2f mean=%7.2f us  (%5.2f%% of %.0fus)\n",
        tag,s.p50,s.p95,s.p999,s.max,s.mean,100.0*s.mean/budget,budget);
}

struct Rig {
    ysfx_config_t* cfg; ysfx_t* fx; int block;
    std::vector<float> inL,inR,outL,outR;
    const float* ins[2]; float* outs[2];
    Rig(const char* path,int b):block(b),inL(b),inR(b),outL(b),outR(b){
        cfg=ysfx_config_new(); fx=ysfx_new(cfg);
        if(!ysfx_load_file(fx,path,0)||!ysfx_compile(fx,0)){ printf("compile failed: %s\n",path); exit(1); }
        ysfx_set_sample_rate(fx,48000); ysfx_set_block_size(fx,block);
        ysfx_set_midi_capacity(fx,65536,false);
        ysfx_init(fx);
        for(int i=0;i<block;i++){ inL[i]=0.01f*((i%17)-8); inR[i]=inL[i]; }
        ins[0]=inL.data(); ins[1]=inR.data(); outs[0]=outL.data(); outs[1]=outR.data();
    }
    ~Rig(){ ysfx_free(fx); ysfx_config_free(cfg); }
    inline void run(){ ysfx_process_float(fx,ins,outs,2,2,block); }
};

int main(int argc,char** argv){
    const char* only = argc>3? argv[3] : "ABCDE";
#define WANT(c) (strchr(only,c)!=nullptr)
    const char* path = argc>1? argv[1] : "fx/reverb.jsfx";
    int block = argc>2? atoi(argv[2]) : 64;
    const int N=20000, WARM=8000;

    printf("== %s  block=%d  sr=48000 ==\n",path,block);
    printf("  process default FPCR=0x%llx (FZ=%d)\n",(unsigned long long)fpcr_rd(),(int)((fpcr_rd()>>24)&1));

    if(WANT('A'))
    // ---- A: steady state, FZ clear (what ships today) ----
    {
        Rig r(path,block);
        for(int i=0;i<WARM;i++) r.run();       // RAM resident + caches warm
        dvfs_warm(300);
        uint64_t sv=fpcr_rd(); fpcr_wr(sv&~(uint64_t)(1u<<24));
        std::vector<double> t; t.reserve(N);
        for(int i=0;i<N;i++){ uint64_t a=ns(); r.run(); t.push_back((ns()-a)/1000.0); }
        fpcr_wr(sv);
        report("A steady, FPCR.FZ=0 (current)",t,block);
    }
    if(WANT('B'))
    // ---- B: steady state, FZ pre-set by host ----
    {
        Rig r(path,block);
        for(int i=0;i<WARM;i++) r.run();
        dvfs_warm(300);
        uint64_t sv=fpcr_rd(); fpcr_wr(sv|(1ull<<24));
        std::vector<double> t; t.reserve(N);
        for(int i=0;i<N;i++){ uint64_t a=ns(); r.run(); t.push_back((ns()-a)/1000.0); }
        fpcr_wr(sv);
        report("B steady, FPCR.FZ=1 (proposed)",t,block);
    }
    if(WANT('C'))
    // ---- C: cold start, no prealloc -- first 3000 blocks of a fresh instance ----
    {
        dvfs_warm(300);
        Rig r(path,block);
        std::vector<double> t; t.reserve(3000);
        for(int i=0;i<3000;i++){ uint64_t a=ns(); r.run(); t.push_back((ns()-a)/1000.0); }
        report("C cold start, lazy EEL RAM",t,block);
    }
    if(WANT('D'))
    // ---- D: cold start, RAM preallocated off-thread ----
    {
        dvfs_warm(300);
        Rig r(path,block);
        ysfx_prealloc_ram(r.fx);               // added below
        std::vector<double> t; t.reserve(3000);
        for(int i=0;i<3000;i++){ uint64_t a=ns(); r.run(); t.push_back((ns()-a)/1000.0); }
        report("D cold start, RAM preallocated",t,block);
    }
    if(WANT('E'))
    // ---- E: transport start mid-stream (must_compute_init -> @init on RT thread) ----
    {
        dvfs_warm(300);
        Rig r(path,block);
        for(int i=0;i<WARM;i++) r.run();
        ysfx_time_info_t ti{}; ti.tempo=120; ti.time_signature[0]=4; ti.time_signature[1]=4;
        std::vector<double> t; t.reserve(2000);
        for(int i=0;i<2000;i++){
            // toggle transport stopped->playing every 200 blocks, like the app's looper
            ti.playback_state = ((i/200)%2)? ysfx_playback_playing : ysfx_playback_stopped;
            ysfx_set_time_info(r.fx,&ti);
            uint64_t a=ns(); r.run(); t.push_back((ns()-a)/1000.0);
        }
        report("E transport toggling (init on RT)",t,block);
    }

    if(WANT('P'))
    // ---- P: paired A/B, interleaved in one process (DVFS-neutral) ----
    {
        Rig r(path,block);
        for(int i=0;i<WARM;i++) r.run();
        dvfs_warm(500);
        std::vector<double> ta,tb; const int BURST=500, REPS=40;
        uint64_t sv=fpcr_rd();
        for(int rep=0;rep<REPS;rep++){
            for(int phase=0;phase<2;phase++){
                fpcr_wr(phase? (sv|(1ull<<24)) : (sv&~(uint64_t)(1u<<24)));
                for(int i=0;i<BURST;i++){
                    uint64_t a=ns(); r.run(); double d=(ns()-a)/1000.0;
                    (phase?tb:ta).push_back(d);
                }
            }
        }
        fpcr_wr(sv);
        report("P paired FZ=0",ta,block);
        report("P paired FZ=1",tb,block);
        Stats sa=stats(ta), sb=stats(tb);
        printf("  %-34s p50 %.2f -> %.2f us  = %.2fx   (%.1f ns/sample saved)\n",
            "P speedup",sa.p50,sb.p50,sa.p50/sb.p50,(sa.p50-sb.p50)*1000.0/block);
    }
    return 0;
}
