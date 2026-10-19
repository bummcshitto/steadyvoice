#include "plugin.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <sched.h>
#include <string>
#include <time.h>
#include <vector>

// Compile this file separately against unchanged 1.4 and 1.5 source. The
// SV_BENCH_V15 macro only enables indices appended in 1.5 and its quality modes.
static double cpuSeconds() {
    timespec t{};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}
static double wallSeconds() {
    timespec t{};
    clock_gettime(CLOCK_MONOTONIC_RAW, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}
static void set(sv::Values& p, int i, float value) { p[i]=sv::normalized(i,value); }

static sv::Values settings(int qualityMode, bool heavy, int lookMs) {
    auto p=sv::preset(4); // Smooth boost is identical in both source versions.
    set(p, sv::Lookahead, float(lookMs));
#ifdef SV_BENCH_V15
    set(p, sv::AdaptiveGuard, qualityMode>=0 ? 1.f : 0.f);
    set(p, sv::TruePeak, qualityMode>=0 ? float(qualityMode) : 0.f);
#else
    (void)qualityMode;
#endif
    if (heavy) {
        set(p,sv::Target,-8); set(p,sv::Boost,24); set(p,sv::Lift,4);
        set(p,sv::Reduce,4); set(p,sv::Trim,12); set(p,sv::Ceiling,-1);
    }
    return p;
}
static std::unique_ptr<sv::Plugin> pluginFor(int rate, const sv::Values& p) {
    auto plugin=std::make_unique<sv::Plugin>(nullptr);
    for(int i=0;i<sv::ParamCount;++i) plugin->setParameter(i,p[i]);
    plugin->dispatch(VST_EFFECT_OPCODE_SET_SAMPLE_RATE,0,0,nullptr,float(rate));
    return plugin;
}
static uint64_t hashFloat(uint64_t h,float value) {
    uint32_t bits;
    std::memcpy(&bits,&value,sizeof(bits));
    for(int i=0;i<4;++i) { h^=(bits>>(8*i))&255u; h*=1099511628211ull; }
    return h;
}

int main(int argc,char** argv) {
    bool focus=false;
    bool stereoFocus=false;
    bool deadlinesFocus=false;
    bool smokeFocus=false;
    bool offFocus=false;
    for(int i=1;i<argc;++i) {
        if(std::string(argv[i])=="--focus") focus=true;
        if(std::string(argv[i])=="--stereo-focus") stereoFocus=focus=true;
        if(std::string(argv[i])=="--deadlines-focus") deadlinesFocus=true;
        if(std::string(argv[i])=="--smoke-focus") smokeFocus=focus=true;
        if(std::string(argv[i])=="--off-focus") offFocus=focus=true;
    }
    cpu_set_t eligible;
    CPU_ZERO(&eligible);
    int pinned=-1;
    if(sched_getaffinity(0,sizeof(eligible),&eligible)==0) {
        for(int i=0;i<CPU_SETSIZE;++i) if(CPU_ISSET(i,&eligible)) {
            cpu_set_t selected; CPU_ZERO(&selected); CPU_SET(i,&selected);
            if(sched_setaffinity(0,sizeof(selected),&selected)==0) pinned=i;
            break;
        }
    }
#ifdef SV_BENCH_V15
    const char* version="1.5";
    const std::vector<int> qualityModes={-1,0,1,2};
#else
    const char* version="1.4";
    const std::vector<int> qualityModes={-1};
#endif
    std::cerr<<"Version "<<version<<", pinned logical CPU "<<pinned<<", three repeats, target 25 ms thread CPU per repeat\n";
    std::cout<<"version,rate,block_ms,block_frames,fixture,profile,peak_mode,lookahead_ms,latency_samples,ns_per_frame_median,ns_per_frame_min,ns_per_frame_max,one_cpu_realtime_percent,mean_cpu_us_per_block,measured_frames_per_repeat,output_hash,probe_max_output_peak_db,probe_max_guard_reduction_db,probe_guard_active_blocks,probe_blocks,invalid_samples,routing,probe_p99_cpu_us_per_block,probe_max_cpu_us_per_block,probe_max_wall_us_per_block\n";
    constexpr double pi=3.14159265358979323846;
    for(int rate:{48000,96000,192000}) {
        if((focus || deadlinesFocus) && rate==96000) continue;
        for(int blockMs:{2,5,10}) {
            if(deadlinesFocus ? blockMs==5 : (focus && blockMs!=5)) continue;
            const int block=int(std::lround(rate*blockMs*.001));
            const size_t ringFrames=((size_t(rate)*2+block-1)/block)*block;
            for(bool heavy:{false,true}) {
                if(deadlinesFocus && !heavy) continue;
                std::vector<float> left(ringFrames),right(ringFrames,0),outL(block),outR(block);
                uint32_t rng=0x53565135u;
                for(size_t i=0;i<ringFrames;++i) {
                    const double t=double(i)/rate;
                    constexpr std::array<double,10> amplitude{{.006,.030,.120,.500,.012,.004,.260,.050,.015,.400}};
                    const double a=amplitude[std::min(size_t(t/.2),size_t(9))];
                    rng=rng*1664525u+1013904223u;
                    const double noise=double(rng)/4294967296.0*2-1;
                    const double voice=.58*std::sin(2*pi*173*t)+.25*std::sin(2*pi*719*t)+.08*std::sin(2*pi*4311*t);
                    const double syllable=.7+.3*std::sin(2*pi*4.2*t);
                    const double bass=(heavy ? .055 : .008)*std::sin(2*pi*59*t);
                    const double plosive=i%size_t(rate/20)<size_t(std::max(1,rate/16000)) ? .18 : 0;
                    left[i]=float(a*voice*syllable+bass+(heavy?.001:.0003)*noise+plosive);
                    if(stereoFocus) right[i]=float(.61*a*(.55*std::sin(2*pi*229*t)+.23*std::sin(2*pi*1009*t))+.0005*noise);
                }
                for(int lookMs:{5,10}) for(int mode:qualityModes) {
                    if((stereoFocus || deadlinesFocus) && lookMs!=5) continue;
                    if(deadlinesFocus && mode!=2) continue;
                    if(smokeFocus && (mode!=2 || lookMs!=5)) continue;
                    if(offFocus && mode!=-1) continue;
                    auto p=settings(mode,heavy,lookMs);
                    auto timed=pluginFor(rate,p);
                    float* outputs[2]={outL.data(),outR.data()};
                    size_t offset=0;
                    double checksum=0;
                    auto run=[&](size_t blocks) {
                        for(size_t n=0;n<blocks;++n) {
                            const float* input[2]={left.data()+offset,right.data()+offset};
                            timed->render(input,outputs,block);
                            checksum+=outL[block/3]+outR[block/2];
                            offset+=block;
                            if(offset==ringFrames) offset=0;
                        }
                    };
                    run(ringFrames/block);
                    constexpr size_t pilotBlocks=64;
                    const double pilotStart=cpuSeconds();
                    run(pilotBlocks);
                    const double pilotSeconds=std::max(cpuSeconds()-pilotStart,1e-8);
                    const size_t measuredBlocks=std::clamp(size_t(std::ceil(.025/pilotSeconds*pilotBlocks)),size_t(32),size_t(65536));
                    std::array<double,3> elapsed{};
                    for(int rep=0;rep<3;++rep) {
                        const double start=cpuSeconds(); run(measuredBlocks);
                        elapsed[rep]=(cpuSeconds()-start)*1e9/(measuredBlocks*block);
                    }
                    std::sort(elapsed.begin(),elapsed.end());

                    // A separate fresh instance makes the untimed output hash
                    // independent of timing calibration and repeat lengths.
                    auto probe=pluginFor(rate,p);
                    for(size_t at=0;at<ringFrames;at+=block) {
                        const float* input[2]={left.data()+at,right.data()+at};
                        probe->render(input,outputs,block);
                    }
                    uint64_t hash=14695981039346656037ull;
                    double maxPeak=-120,maxReduction=0;
                    size_t activeBlocks=0;
                    std::vector<double> probeCpuTimes;
                    probeCpuTimes.reserve(ringFrames/block);
                    double maxWall=0;
                    for(size_t at=0;at<ringFrames;at+=block) {
                        const float* input[2]={left.data()+at,right.data()+at};
                        const double wallStart=wallSeconds();
                        const double cpuStart=cpuSeconds();
                        probe->render(input,outputs,block);
                        const double cpuElapsed=(cpuSeconds()-cpuStart)*1e6;
                        const double wallElapsed=(wallSeconds()-wallStart)*1e6;
                        probeCpuTimes.push_back(cpuElapsed);
                        maxWall=std::max(maxWall,wallElapsed);
                        for(int i=0;i<block;++i) { hash=hashFloat(hash,outL[i]); hash=hashFloat(hash,outR[i]); }
                        const auto m=probe->meterSnapshot();
                        maxPeak=std::max(maxPeak,double(m.outputPeak));
                        maxReduction=std::max(maxReduction,double(m.guardReduction));
                        if(m.guardReduction>.01f) ++activeBlocks;
                    }
                    if(!std::isfinite(checksum)) return 3;
                    const auto endMeters=probe->meterSnapshot();
                    std::sort(probeCpuTimes.begin(),probeCpuTimes.end());
                    const double p99=probeCpuTimes[size_t(std::ceil(.99*(probeCpuTimes.size()-1)))];
                    std::cout<<version<<','<<rate<<','<<blockMs<<','<<block<<','<<(heavy?"noisy_bass_heavy":"typical")<<','
                        <<(mode<0?"legacy_off":"quality_voice")<<','<<(mode<0||mode==0?"sample":(mode==1?"4x":"8x"))<<','<<lookMs<<','
                        <<probe->effect.delay<<','<<std::fixed<<std::setprecision(4)<<elapsed[1]<<','<<elapsed[0]<<','<<elapsed[2]<<','
                        <<elapsed[1]*rate/1e7<<','<<elapsed[1]*block/1000<<','<<measuredBlocks*block<<','<<std::hex<<hash<<std::dec<<','
                        <<maxPeak<<','<<maxReduction<<','<<activeBlocks<<','<<ringFrames/block<<','<<endMeters.invalidSamples<<','
                        <<(stereoFocus?"active_stereo":"left_plus_silence")<<','<<p99<<','<<probeCpuTimes.back()<<','<<maxWall<<'\n';
                }
            }
        }
    }
}
