#include "../src/dsp.hpp"
#include "fixtures/leveler_1_2.hpp"
#include <vector>
#include <iostream>
#include <iomanip>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <cstdio>

using namespace sv;
int assertions=0;
void require(bool pass,const char* reason) {
    ++assertions;
    if(!pass) { std::cerr<<"FAIL: "<<reason<<"\n"; std::exit(1); }
}
std::vector<float> speech(int samples,double rate) {
    std::vector<float> voice(samples);
    double power=0;
    for(int i=0;i<samples;++i) {
        const double t=i/rate;
        voice[i]=float(std::sin(6.283185307179586*157*t)
                     +.42*std::sin(6.283185307179586*314*t+.3)
                     +.27*std::sin(6.283185307179586*471*t+.7)
                     +.13*std::sin(6.283185307179586*2355*t+1.1));
        power+=double(voice[i])*voice[i];
    }
    const double scale=1/std::sqrt(power/samples);
    for(auto& x:voice)x=float(x*scale);
    return voice;
}
struct Trace {
    std::vector<float> audio,gain,guard;
};
template<class Processor>
Trace render(const std::vector<float>& input,const Values& p,double rate=48000,int block=48) {
    Processor d;d.prepare(rate,p);
    Trace result;result.audio.resize(input.size());result.gain.resize(input.size());result.guard.resize(input.size());
    for(size_t i=0;i<input.size();i+=size_t(block)) {
        const int n=int(std::min(size_t(block),input.size()-i));
        d.template process<float>(input.data()+i,nullptr,result.audio.data()+i,nullptr,n);
        const auto m=d.meters();
        std::fill(result.gain.begin()+i,result.gain.begin()+i+n,m.levelingGain);
        std::fill(result.guard.begin()+i,result.guard.begin()+i+n,m.guardReduction);
    }
    return result;
}
double rmsDb(const std::vector<float>& v,int start,int count) {
    double power=0;
    for(int i=start;i<start+count;++i)power+=double(v[i])*v[i];
    return 10*std::log10(std::max(power/count,1e-20));
}
double peakDb(const std::vector<float>& v,int start,int end) {
    double peak=0;for(int i=start;i<end;++i)peak=std::max(peak,std::abs(double(v[i])));
    return amplitudeDb(peak);
}
int overloads(const std::vector<float>& v,int start,int end) {
    int count=0;for(int i=start;i<end;++i)if(std::abs(v[i])>1)++count;
    return count;
}
bool saveFloatWave(const std::filesystem::path& path,const std::vector<float>& audio) {
    FILE* f=std::fopen(path.string().c_str(),"wb");if(!f)return false;
    const uint32_t dataBytes=uint32_t(audio.size()*sizeof(float)),riffBytes=36+dataBytes;
    const uint32_t formatBytes=16,rate=48000,bytesPerSecond=rate*4;
    const uint16_t format=3,channels=1,alignment=4,bits=32;
    bool ok=true;
    auto write=[&](const void* p,size_t count){ok=ok && std::fwrite(p,1,count,f)==count;};
    write("RIFF",4);write(&riffBytes,4);write("WAVEfmt ",8);write(&formatBytes,4);
    write(&format,2);write(&channels,2);write(&rate,4);write(&bytesPerSecond,4);
    write(&alignment,2);write(&bits,2);write("data",4);write(&dataBytes,4);
    write(audio.data(),dataBytes);ok=std::fclose(f)==0 && ok;return ok;
}
int main(int argc,char** argv) {
    constexpr int rate=48000,onset=rate*3;
    auto input=speech(rate*5,rate);
    for(int i=0;i<int(input.size());++i)input[i]*=float(dbGain(i<onset?-44:-8));
    auto p=defaults();p[Target]=normalized(Target,-10);p[Boost]=p[Cut]=p[Strength]=1;
    p[Guard]=0;p[DetectorHPF]=0;
    const auto old=render<sv120::Leveler>(input,p);
    const auto revised=render<Leveler>(input,p);
    const int begin=onset+rate*30/1000,end=onset+rate*200/1000;
    const int oldClips=overloads(old.audio,begin,end),newClips=overloads(revised.audio,begin,end);
    std::cout<<std::fixed<<std::setprecision(3);
    std::cout<<"quiet_to_loud target=-10 strength=100 boost=40 cut=40 reduce=100 guard=off\n";
    std::cout<<"baseline_overrange_samples_30_to_200ms="<<oldClips
             <<" revised_overrange_samples_30_to_200ms="<<newClips<<"\n";
    std::cout<<"baseline_peak_30_to_200ms_db="<<peakDb(old.audio,begin,end)
             <<" revised_peak_30_to_200ms_db="<<peakDb(revised.audio,begin,end)<<"\n";
    std::cout<<"baseline_level_gain_at_50ms_db="<<old.gain[onset+rate/20]
             <<" revised_level_gain_at_50ms_db="<<revised.gain[onset+rate/20]<<"\n";
    p[Guard]=1;
    const auto oldGuarded=render<sv120::Leveler>(input,p);
    const auto newGuarded=render<Leveler>(input,p);
    std::cout<<"baseline_guarded_peak_db="<<peakDb(oldGuarded.audio,0,int(input.size()))
             <<" revised_guarded_peak_db="<<peakDb(newGuarded.audio,0,int(input.size()))<<"\n";
    std::cout<<"baseline_output_rms_50_to_150ms_db="<<rmsDb(oldGuarded.audio,onset+rate/20,rate/10)
             <<" revised_output_rms_50_to_150ms_db="<<rmsDb(newGuarded.audio,onset+rate/20,rate/10)<<"\n";
    if(argc>1 && std::strcmp(argv[1],"baseline")==0)return 0;
    require(oldClips>100,"Regression reference must reproduce unguarded quiet-to-loud overload");
    require(newClips<oldClips/20,"Revised loud catch must remove at least 95 percent of later overrange samples in this transition");
    require(revised.gain[onset+rate/20]<old.gain[onset+rate/20]-12,"The revised leveler must shed held boost substantially faster");
    require(peakDb(oldGuarded.audio,0,int(input.size()))<=-3+1e-5,"Original guard must also be checked for actual sample ceiling violations");
    require(peakDb(newGuarded.audio,0,int(input.size()))<=-3+1e-5,"Revised guard must preserve the sample ceiling");

    // Full-band loud control must catch energy rejected by the slow detector
    // HPF. Otherwise a deep voice can be raised far beyond the RMS target.
    std::vector<float> bass(rate*6);
    for(size_t i=0;i<bass.size();++i)bass[i]=float(dbGain(-18)*std::sqrt(2.0)*std::sin(6.283185307179586*50*i/rate));
    auto bassSettings=p;bassSettings[Guard]=0;bassSettings[DetectorHPF]=1;
    const auto oldBass=render<sv120::Leveler>(bass,bassSettings);
    const auto newBass=render<Leveler>(bass,bassSettings);
    const double oldBassRms=rmsDb(oldBass.audio,rate*5,rate),newBassRms=rmsDb(newBass.audio,rate*5,rate);
    std::cout<<"50hz_input_rms=-18 target=-10 hpf=250 guard=off baseline_output_rms_db="<<oldBassRms
             <<" revised_output_rms_db="<<newBassRms<<"\n";
    require(oldBassRms>0,"Regression reference must reproduce high-pass-related overboost of a low-frequency signal");
    require(newBassRms<-9 && newBassRms>-12,"Full-band loud catch must keep deep-voice energy near the actual RMS target");
    require(peakDb(newBass.audio,rate*5,int(bass.size()))<-4,"Deep-voice test must regain sample-peak headroom without relying on the guard");

    // A guard's held cap must not compound a primary gain reduction and cause
    // an extra volume sag as the leveler or trim takes over from protection.
    auto protection=defaults();protection[Strength]=0;
    Leveler guarded,reference;guarded.prepare(rate,protection);
    auto dry=protection;dry[Guard]=0;reference.prepare(rate,dry);
    float value=1,protectedOut=0,referenceOut=0;
    guarded.process<float>(&value,nullptr,&protectedOut,nullptr,1);
    reference.process<float>(&value,nullptr,&referenceOut,nullptr,1);
    protection[Trim]=normalized(Trim,-12);dry[Trim]=protection[Trim];
    guarded.configure(protection);reference.configure(dry);
    bool noExtraSag=true;
    for(int i=0;i<rate/10;++i) {
        guarded.process<float>(&value,nullptr,&protectedOut,nullptr,1);
        reference.process<float>(&value,nullptr,&referenceOut,nullptr,1);
        noExtraSag=noExtraSag && std::abs(protectedOut-std::min(referenceOut,float(dbGain(-3))))<2e-7;
    }
    require(noExtraSag,"Peak guard must not apply stale relative attenuation on top of falling primary gain");
    protection[Guard]=0;guarded.configure(protection);
    guarded.process<float>(&value,nullptr,&protectedOut,nullptr,1);
    reference.process<float>(&value,nullptr,&referenceOut,nullptr,1);
    require(std::abs(protectedOut-referenceOut)<2e-7,"Turning off the guard must immediately remove its held attenuation");
    auto switched=defaults();switched[Strength]=0;
    Leveler activeSwitch;activeSwitch.prepare(rate,switched);
    activeSwitch.process<float>(&value,nullptr,&protectedOut,nullptr,1);
    require(protectedOut<.71f,"Guard-switch test must begin with actively applied attenuation");
    switched[Guard]=0;activeSwitch.configure(switched);
    activeSwitch.process<float>(&value,nullptr,&protectedOut,nullptr,1);
    require(protectedOut==value,"Guard-off must remove active attenuation on the very next sample");
    switched[Guard]=1;activeSwitch.configure(switched);
    activeSwitch.process<float>(&value,nullptr,&protectedOut,nullptr,1);
    require(std::abs(protectedOut)<=dbGain(-3)+1e-7,"Guard re-enable must catch the current sample without stale state");

    for(float target : {-10.0f,-6.0f}) for(float ceiling : {-3.0f,0.0f})
    for(float reduction : {0.0f,100.0f,1000.0f}) for(float boost : {12.0f,40.0f}) {
        auto settings=p;
        settings[Target]=normalized(Target,target);settings[Ceiling]=normalized(Ceiling,ceiling);
        settings[Reduce]=normalized(Reduce,reduction);settings[Boost]=normalized(Boost,boost);
        settings[Trim]=normalized(Trim,12);
        const auto result=render<Leveler>(input,settings,rate,127);
        require(std::all_of(result.audio.begin(),result.audio.end(),[&](float x){return std::isfinite(x)&&std::abs(x)<=dbGain(ceiling)+1e-7;}),"Guard must hold its ceiling across near-full-scale targets, timing and boost settings, and +12 dB trim");
    }
    for(double sampleRate : {8000.0,16000.0,44100.0,48000.0,96000.0,192000.0,384000.0}) {
        const int frames=int(sampleRate/2);
        auto shortVoice=speech(frames,sampleRate);
        for(int i=0;i<frames;++i)shortVoice[i]*=float(dbGain(i<frames/2?-40:0));
        auto settings=p;settings[Lift]=settings[Reduce]=0;settings[Ceiling]=1;settings[Trim]=1;
        const auto protectedTrace=render<Leveler>(shortVoice,settings,sampleRate,71);
        require(std::all_of(protectedTrace.audio.begin(),protectedTrace.audio.end(),[](float x){return std::isfinite(x)&&std::abs(x)<=1;}),"Revised fast catch and absolute guard must remain stable at every supported test rate");
    }
    const auto sampleBlocks=render<Leveler>(input,p,rate,1);
    const auto irregularBlocks=render<Leveler>(input,p,rate,511);
    require(sampleBlocks.audio==irregularBlocks.audio,"Loud-transition audio must not depend on host block size");
    Leveler inPlace;inPlace.prepare(rate,p);
    auto aliased=input;inPlace.process<float>(aliased.data(),nullptr,aliased.data(),nullptr,int(aliased.size()));
    require(aliased==sampleBlocks.audio,"Loud-transition in-place buffers must retain the exact output");
    if(argc>2 && std::strcmp(argv[1],"--export")==0) {
        const std::filesystem::path directory(argv[2]);std::filesystem::create_directories(directory);
        for(float ceiling : {-3.0f,0.0f}) {
            auto settings=p;settings[Ceiling]=normalized(Ceiling,ceiling);
            const auto encodedSource=render<Leveler>(input,settings);
            const auto path=directory/(ceiling<0?"guard-ceiling-minus3.wav":"guard-ceiling-zero.wav");
            require(saveFloatWave(path,encodedSource.audio),"Encoding probe WAV must be written without quantizing the float output");
        }
    }
    std::cout<<"PASS loudness_assertions="<<assertions<<"\n";
}
