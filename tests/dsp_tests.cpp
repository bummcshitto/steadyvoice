#include "../src/dsp.hpp"
#include <vector>
#include <iostream>
#include <chrono>
#include <iomanip>
#include <cstdlib>
#include <limits>
#include <cstring>
#include <cfenv>

using namespace sv;
int assertions=0;
void require(bool pass,const char* message) {
    ++assertions;
    if(!pass) { std::cerr<<"FAIL: "<<message<<"\n"; std::exit(1); }
}
std::vector<float> tone(int length,double sr,double rmsDb,double frequency=1000) {
    std::vector<float> v(length);
    const double amplitude=dbGain(rmsDb)*std::sqrt(2.0);
    for(int i=0;i<length;++i) v[i]=float(amplitude*std::sin(6.283185307179586*frequency*i/sr));
    return v;
}
double rms(const std::vector<float>& v,int start=0) {
    double e=0; for(size_t i=size_t(start);i<v.size();++i) e+=double(v[i])*v[i];
    return std::sqrt(e/(v.size()-start));
}
std::vector<float> run(const std::vector<float>& a,const Values& p,double sr=48000,int block=128) {
    Leveler d; d.prepare(sr,p);
    std::vector<float> out(a.size());
    for(size_t i=0;i<a.size();i+=size_t(block)) {
        int n=int(std::min(size_t(block),a.size()-i));
        d.process<float>(a.data()+i,a.data()+i,out.data()+i,nullptr,n);
    }
    return out;
}
double thd(const std::vector<float>& a,int start,double sr,double frequency) {
    auto energy=[&](int harmonic) {
        double x=0,y=0;
        for(size_t i=size_t(start);i<a.size();++i) {
            const double t=6.283185307179586*frequency*harmonic*double(i)/sr;
            x+=a[i]*std::sin(t); y+=a[i]*std::cos(t);
        }
        return x*x+y*y;
    };
    double distortion=0; for(int h=2;h<=10;++h) distortion+=energy(h);
    return 10*std::log10(std::max(distortion/energy(1),1e-30));
}
int main() {
    auto values=defaults();
    Leveler d; d.prepare(48000,values);
    std::vector<float> silence(48000*4), zeros(silence.size(),99);
    d.process<float>(silence.data(),silence.data(),zeros.data(),nullptr,int(zeros.size()));
    require(std::all_of(zeros.begin(),zeros.end(),[](float x){return x==0;}),"Silence must stay exactly silent");
    require(d.meters().levelingGain==0,"Silence must not build gain");
    require(!d.meters().speech,"Silence must remain below the speech threshold");

    std::cout<<std::fixed<<std::setprecision(3);
    std::cout<<"SteadyVoice DSP tests\n";
    std::vector<double> levels;
    for(double level : {-38.0,-26.0,-10.0}) {
        auto input=tone(48000*6,48000,level);
        auto output=run(input,values);
        const double out=amplitudeDb(rms(output,48000*4)); levels.push_back(out);
        std::cout<<"steady_input_db="<<level<<" output_rms_db="<<out<<"\n";
        require(out>-30 && out<-18,"Quiet and loud inputs must converge to a useful voice level");
    }
    const double spread=*std::max_element(levels.begin(),levels.end())-*std::min_element(levels.begin(),levels.end());
    require(spread<7,"28 dB input difference must shrink to less than 7 dB");
    std::cout<<"output_spread_db="<<spread<<" from_input_spread_db=28\n";

    auto noise=tone(48000*8,48000,-65);
    auto quiet=run(noise,values);
    require(std::abs(amplitudeDb(rms(quiet,48000*4))+65)<.05,"Below-floor audio must not be boosted at rest");
    auto whisper=tone(48000*6,48000,-42);
    auto lifted=run(whisper,values);
    require(amplitudeDb(rms(lifted,48000*4))<-29.8,"Boost must respect the 12 dB cap");
    require(amplitudeDb(rms(lifted,48000*4))>-30.2,"Quiet speech must reach the available boost");
    Leveler pauses; pauses.prepare(48000,values);
    std::vector<float> speechOut(whisper.size());
    pauses.process<float>(whisper.data(),whisper.data(),speechOut.data(),nullptr,int(whisper.size()));
    const float gainBefore=pauses.meters().levelingGain;
    std::vector<float> pauseOut(48000*5);
    pauses.process<float>(noise.data(),noise.data(),pauseOut.data(),nullptr,int(pauseOut.size()));
    require(pauses.meters().levelingGain<.25,"Gain must return close to unity after a pause");
    require(pauses.meters().levelingGain<gainBefore,"Pauses must not accumulate extra boost");

    Values transparent=defaults(); transparent[Strength]=0; transparent[Guard]=0;
    auto impulse=std::vector<float>(512,0); impulse[0]=.25f;
    auto response=run(impulse,transparent);
    require(response[0]==.25f,"Impulse must emerge at sample zero (zero added latency)");
    require(std::all_of(response.begin()+1,response.end(),[](float x){return x==0;}),"No delayed impulse or audio tail");

    std::vector<float> complex(48000*3);
    uint32_t seed=1234567;
    for(size_t i=0;i<complex.size();++i) { seed=1664525*seed+1013904223; complex[i]=float((int32_t(seed)/2147483648.0)*.35); }
    auto one=run(complex,values,48000,1);
    auto wide=run(complex,values,48000,511);
    require(one==wide,"Audio must be identical across arbitrary host block boundaries");
    auto regular=run(complex,values);
    Leveler inplace; inplace.prepare(48000,values);
    auto inPlace=complex;
    inplace.process<float>(inPlace.data(),inPlace.data(),inPlace.data(),nullptr,int(inPlace.size()));
    require(inPlace==regular,"In-place processing must match separate buffers");

    Leveler stereo,mono; stereo.prepare(48000,values); mono.prepare(48000,values);
    auto reference=tone(48000*3,48000,-30), right=reference;
    for(auto& x:right) x*=.35f;
    std::vector<float> l(reference.size()),r(reference.size()),m(reference.size());
    stereo.process<float>(reference.data(),right.data(),l.data(),r.data(),int(l.size()));
    mono.process<float>(reference.data(),nullptr,m.data(),nullptr,int(m.size()));
    require(l==m,"A silent right channel must not alter the mono detector gain");
    double ratioError=0;
    for(size_t i=0;i<l.size();++i) ratioError=std::max(ratioError,std::abs(double(r[i])-.35*double(l[i])));
    require(ratioError<1e-7,"Linked processing must preserve stereo ratios");

    Values guard=defaults(); guard[Strength]=0; guard[Trim]=normalized(Trim,6);
    for(double sr : {8000.0,16000.0,44100.0,48000.0,96000.0,192000.0,384000.0}) {
        auto hot=tone(int(sr*.5),sr,3,997);
        auto capped=run(hot,guard,sr,127);
        double peak=0;
        require(std::all_of(capped.begin(),capped.end(),[](float x){return std::isfinite(x);}),"Output must remain finite at supported sample rates");
        for(float x:capped) peak=std::max(peak,std::abs(double(x)));
        require(peak<=dbGain(-3)+1e-7,"Peak guard must catch every sample including the first transient");
    }
    auto stable=tone(48000*3,48000,-30);
    auto linear=run(stable,values);
    const double harmonic=thd(linear,48000*2,48000,1000);
    std::cout<<"settled_1khz_thd_db="<<harmonic<<"\n";
    require(harmonic<-85,"Settled level control must introduce negligible harmonic distortion");
    auto hot=tone(48000*3,48000,-1);
    auto limited=run(hot,guard);
    const double limiterThd=thd(limited,48000*2,48000,1000);
    std::cout<<"settled_guard_1khz_thd_db="<<limiterThd<<"\n";
    require(limiterThd<-85,"Settled peak limiting must not continuously clip a sine wave");

    std::vector<float> corrupted{std::numeric_limits<float>::quiet_NaN(),std::numeric_limits<float>::infinity(),
                                  -std::numeric_limits<float>::infinity(),1e30f,-1e30f,1e-40f,0};
    auto recovered=run(corrupted,values);
    require(std::all_of(recovered.begin(),recovered.end(),[](float x){return std::isfinite(x)&&std::abs(x)<.708f;}),"Invalid/extreme samples must not corrupt the stream");

    Leveler bypass; bypass.prepare(48000,values);
    std::vector<float> bypassOut(stable.size());
    bypass.process<float>(stable.data(),stable.data(),bypassOut.data(),nullptr,48000);
    auto off=values; off[Bypass]=1; bypass.configure(off);
    bypass.process<float>(stable.data(),stable.data(),bypassOut.data(),nullptr,48000);
    require(std::equal(bypassOut.begin()+2000,bypassOut.begin()+48000,stable.begin()+2000),"Bypass must settle to exact unity");

    // Test all control extremes together, rather than mirroring the controller.
    for(int p=0;p<4;++p) for(int extreme=0;extreme<2;++extreme) {
        auto settings=preset(p);
        for(int i=0;i<Guard;++i) settings[i]=float(extreme);
        auto out=run(complex,settings,44100,17);
        require(std::all_of(out.begin(),out.end(),[](float x){return std::isfinite(x);}),"Every parameter extreme must remain stable");
    }

    // Measure the actual output, not just the parameter mapping, at the new
    // gain limits. The guard is intentionally off for these isolated checks.
    Values expanded=defaults();
    expanded[Target]=1; expanded[Boost]=1; expanded[Ceiling]=1;
    expanded[Strength]=1; expanded[Lift]=normalized(Lift,100); expanded[Floor]=0;
    expanded[DetectorHPF]=0; expanded[Guard]=0;
    auto veryQuiet=tone(48000*4,48000,-60);
    for(double boost : {24.0,32.0,40.0}) {
        auto settings=expanded; settings[Boost]=normalized(Boost,float(boost));
        auto out=run(veryQuiet,settings,48000,71);
        const double measuredGain=amplitudeDb(rms(out,48000*3))+60;
        std::cout<<"requested_boost_db="<<boost<<" measured_boost_db="<<measuredGain<<"\n";
        require(std::abs(measuredGain-boost)<.03,"Quiet speech must reach and respect every expanded boost cap");
    }
    auto targetInput=tone(48000*4,48000,-10);
    auto targetOutput=run(targetInput,expanded);
    const double targetRms=amplitudeDb(rms(targetOutput,48000*3));
    std::cout<<"zero_target_output_rms_db="<<targetRms<<"\n";
    require(std::abs(targetRms)<.03,"The 0 dBFS RMS target must be reachable with the guard disabled");
    require(*std::max_element(targetOutput.begin()+48000*3,targetOutput.end())>1.4f,"RMS and sample peaks must not be confused at the new target");

    expanded[Guard]=1;
    Leveler maximumSilence; maximumSilence.prepare(48000,expanded);
    maximumSilence.process<float>(silence.data(),nullptr,zeros.data(),nullptr,int(silence.size()));
    require(maximumSilence.meters().levelingGain==0,"The 40 dB boost setting must not build gain during silence");
    require(std::all_of(zeros.begin(),zeros.end(),[](float x){return x==0;}),"Silence must remain silent with all new endpoints selected");
    for(double sr : {8000.0,16000.0,44100.0,48000.0,96000.0,192000.0,384000.0}) {
        auto settings=expanded; settings[Trim]=1;
        Leveler transition; transition.prepare(sr,settings);
        auto buildup=tone(int(sr*2),sr,-60,997);
        std::vector<float> buildupOut(buildup.size());
        transition.process<float>(buildup.data(),nullptr,buildupOut.data(),nullptr,int(buildup.size()));
        require(transition.meters().levelingGain>39.9f,"Quiet speech must build the full 40 dB gain before the loud-onset test");
        auto loud=tone(int(sr*.25),sr,3,997); loud[0]=2;
        auto loudRight=loud; for(auto& x:loudRight) x*=-.35f;
        std::vector<float> outLeft(loud.size()),outRight(loud.size());
        transition.process<float>(loud.data(),loudRight.data(),outLeft.data(),outRight.data(),int(loud.size()));
        require(std::all_of(outLeft.begin(),outLeft.end(),[](float x){return std::isfinite(x)&&std::abs(x)<=1;}),"0 dBFS guard must catch an immediate shout after 40 dB boost at every sample rate");
        require(std::all_of(outRight.begin(),outRight.end(),[](float x){return std::isfinite(x)&&std::abs(x)<=1;}),"0 dBFS guard must protect both stereo channels");
        double error=0;
        for(size_t i=0;i<outLeft.size();++i) error=std::max(error,std::abs(double(outRight[i])+.35*double(outLeft[i])));
        require(error<1e-6,"Expanded gain and peak protection must retain linked stereo ratios");
        require(outLeft[0]>0.9999f,"The first full-scale protected sample must emerge immediately without delay");
        std::vector<float> resting(int(sr*5)),restOut(resting.size());
        transition.process<float>(resting.data(),nullptr,restOut.data(),nullptr,int(resting.size()));
        require(std::abs(transition.meters().levelingGain)<.3f,"Expanded gain must still return toward unity after speech ends");
        // Check corrupted buffers separately; their extreme detector energy
        // should not be confused with the recovery time after ordinary speech.
        std::vector<double> doubleLoud{2,-2,1e30,-1e30,std::numeric_limits<double>::infinity(),std::numeric_limits<double>::quiet_NaN()},doubleOut(doubleLoud.size());
        transition.process<double>(doubleLoud.data(),nullptr,doubleOut.data(),nullptr,int(doubleLoud.size()));
        require(std::all_of(doubleOut.begin(),doubleOut.end(),[](double x){return std::isfinite(x)&&std::abs(x)<=1;}),"The new 0 dBFS ceiling must also protect double-precision and corrupted inputs");
    }
    auto cappedTarget=run(targetInput,expanded);
    require(amplitudeDb(rms(cappedTarget,48000*3))<-2.99,"A 0 dBFS sample ceiling must take priority over a 0 dBFS RMS target");
    auto expandedOne=run(complex,expanded,48000,1);
    auto expandedWide=run(complex,expanded,48000,511);
    require(expandedOne==expandedWide,"Expanded endpoints must remain invariant across host block sizes");
    expanded[Bypass]=1;
    auto maximumBypass=run(complex,expanded);
    require(maximumBypass==complex,"Bypass must remain exact at the expanded control endpoints");

    for(int i : {Lift,Reduce}) {
        require(physical(i,0)==0 && normalized(i,0)==0,"Time controls must map exact zero in both directions");
        require(physical(i,1)==params[i].high && normalized(i,params[i].high)==1,"Time controls must retain exact upper endpoints");
        bool monotonic=true; float previous=-1;
        for(int n=0;n<=1000;++n) {
            const float current=physical(i,n/1000.0f);
            monotonic=monotonic && std::isfinite(current) && current>=previous;
            previous=current;
        }
        require(monotonic,"Zero-inclusive logarithmic time scales must remain finite and monotonic");
        for(float milliseconds : {.001f,.1f,1.0f,10.0f,100.0f,500.0f}) {
            const float roundTrip=physical(i,normalized(i,milliseconds));
            require(std::abs(roundTrip-milliseconds)<1e-4f*(1+milliseconds),"Sub-millisecond and positive time entries must round-trip accurately");
        }
    }

    auto highLevel=tone(48000*4,48000,12);
    Values fast=defaults(); fast[Target]=0; fast[Strength]=1; fast[Floor]=0;
    fast[Lift]=fast[Reduce]=0; fast[DetectorHPF]=0; fast[Guard]=0;
    for(float cut : {24.0f,36.0f,40.0f}) {
        fast[Cut]=normalized(Cut,cut);
        const auto out=run(highLevel,fast);
        const double measuredCut=12-amplitudeDb(rms(out,48000*3));
        std::cout<<"requested_cut_db="<<cut<<" measured_cut_db="<<measuredCut<<"\n";
        require(std::abs(measuredCut-cut)<.03,"Sustained loud input must reach and respect every cut cap up to 40 dB");
    }

    // Black-box zero-time checks at an aligned controller tick: the very first
    // processed sample must receive the new gain, rather than a hidden ramp.
    std::feclearexcept(FE_DIVBYZERO|FE_INVALID);
    auto zeroTimes=fast; zeroTimes[Target]=1; zeroTimes[Boost]=1; zeroTimes[Strength]=0;
    Leveler immediate; immediate.prepare(48000,zeroTimes);
    auto warming=tone(48000,48000,-60);
    std::vector<float> warmingOut(warming.size());
    immediate.process<float>(warming.data(),nullptr,warmingOut.data(),nullptr,int(warming.size()));
    zeroTimes[Strength]=1; immediate.configure(zeroTimes);
    float liftProbe=.001f,liftOut=0;
    immediate.process<float>(&liftProbe,nullptr,&liftOut,nullptr,1);
    const double firstLift=liftOut/liftProbe;
    std::cout<<"zero_ms_first_tick_lift_db="<<amplitudeDb(firstLift)<<"\n";
    require(std::abs(firstLift-100)<1e-4,"Zero lift time must apply the full available boost on the first current-tick sample");
    std::array<float,95> between{},betweenOut{}; between.fill(.001f);
    immediate.process<float>(between.data(),nullptr,betweenOut.data(),nullptr,int(between.size()));
    zeroTimes[Target]=0; zeroTimes[Boost]=0; immediate.configure(zeroTimes);
    float reduceProbe=1000,reduceOut=0;
    immediate.process<float>(&reduceProbe,nullptr,&reduceOut,nullptr,1);
    const double firstCut=reduceOut/reduceProbe;
    std::cout<<"zero_ms_first_tick_cut_db="<<amplitudeDb(firstCut)<<"\n";
    require(std::abs(firstCut-.01)<1e-7,"Zero reduce time must apply the full 40 dB cut on the first current-tick sample");
    require((std::fetestexcept(FE_DIVBYZERO|FE_INVALID))==0,"Zero time must not trigger divide-by-zero or invalid floating-point calculations");

    zeroTimes[Target]=1; zeroTimes[Boost]=zeroTimes[Cut]=zeroTimes[Ceiling]=zeroTimes[Guard]=1;
    for(double sr : {8000.0,16000.0,44100.0,48000.0,96000.0,192000.0,384000.0}) {
        for(const auto times : {std::array<float,2>{0,0},{0,100},{600,0},{.001f,.001f}}) {
            auto settings=zeroTimes;
            settings[Lift]=normalized(Lift,times[0]); settings[Reduce]=normalized(Reduce,times[1]);
            const auto out=run(complex,settings,sr,71);
            require(std::all_of(out.begin(),out.end(),[](float x){return std::isfinite(x)&&std::abs(x)<=1;}),"Zero and near-zero time combinations must stay finite and peak-protected at every sample rate");
        }
    }
    const auto zeroOne=run(complex,zeroTimes,48000,1);
    const auto zeroWide=run(complex,zeroTimes,48000,511);
    require(zeroOne==zeroWide,"Zero-time processing must remain invariant across host block boundaries");
    Leveler zeroInplace; zeroInplace.prepare(48000,zeroTimes);
    auto zeroBuffer=complex;
    zeroInplace.process<float>(zeroBuffer.data(),nullptr,zeroBuffer.data(),nullptr,int(zeroBuffer.size()));
    require(zeroBuffer==zeroOne,"Zero-time in-place processing must match separate buffers");
    Leveler zeroSilence; zeroSilence.prepare(48000,zeroTimes);
    zeroSilence.process<float>(silence.data(),nullptr,zeros.data(),nullptr,int(silence.size()));
    require(zeroSilence.meters().levelingGain==0 && std::all_of(zeros.begin(),zeros.end(),[](float x){return x==0;}),"Zero-time settings must not boost silence");

    Leveler benchmark; benchmark.prepare(48000,values);
    constexpr int blocks=37500; // 100 seconds of stereo at 128 samples.
    auto block=tone(128,48000,-24); float outA[128]{},outB[128]{};
    const auto start=std::chrono::steady_clock::now();
    for(int i=0;i<blocks;++i) benchmark.process<float>(block.data(),block.data(),outA,outB,128);
    const auto elapsed=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
    volatile float consume=outA[127]; (void)consume;
    std::cout<<"benchmark_audio_seconds=100 benchmark_wall_seconds="<<elapsed
             <<" one_core_percent="<<elapsed<<"\n";
    std::cout<<"PASS assertions="<<assertions<<"\n";
}
