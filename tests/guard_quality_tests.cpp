#include "../src/dsp.hpp"
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <limits>
#include <vector>

namespace {
using namespace sv;
constexpr double pi2=6.283185307179586476925286766559;
int assertions=0;
void require(bool pass,const char* message) {
    ++assertions;
    if (!pass) { std::cerr<<"FAIL: "<<message<<'\n'; std::exit(1); }
}
Values isolated(bool adaptive,double delay=0,double reduction=8.56) {
    auto settings=defaults();
    settings[Strength]=0;
    settings[Guard]=1;
    settings[AdaptiveGuard]=adaptive ? 1 : 0;
    settings[Ceiling]=normalized(Ceiling,-1);
    settings[Trim]=normalized(Trim,float(20*std::log10(dbGain(-1)/.6)+reduction));
    settings[Lookahead]=normalized(Lookahead,float(delay));
    return settings;
}
std::vector<float> tone(int count,double rate,double frequency,double amplitude=.6) {
    std::vector<float> input(size_t(count),0);
    for (int i=0;i<count;++i) input[size_t(i)]=float(amplitude*std::sin(pi2*frequency*i/rate));
    return input;
}
std::vector<float> process(const std::vector<float>& input,const Values& settings,
                           double rate=48000,int block=127,bool stereo=false) {
    Leveler processor; processor.prepare(rate,settings);
    std::vector<float> output(input.size()),right(input.size());
    for (size_t start=0;start<input.size();start+=size_t(block)) {
        const int count=int(std::min(size_t(block),input.size()-start));
        processor.process<float>(input.data()+start,stereo ? input.data()+start : nullptr,
                          output.data()+start,stereo ? right.data()+start : nullptr,count);
    }
    if (stereo) require(output==right,"Identical stereo input must remain identical with adaptive guard");
    return output;
}
double peak(const std::vector<float>& input) {
    double result=0;
    for (const auto sample:input) result=std::max(result,std::abs(double(sample)));
    return result;
}
// Fit the fundamental, then measure all residual energy. This includes
// harmonics and any modulation rather than just a handpicked set of bins.
double toneResidual(const std::vector<float>& output,size_t first,size_t count,
                    double rate,double frequency) {
    double real=0,imaginary=0;
    for (size_t i=0;i<count;++i) {
        const double angle=pi2*frequency*double(i)/rate;
        real+=double(output[first+i])*std::cos(angle);
        imaginary+=double(output[first+i])*std::sin(angle);
    }
    real*=2.0/count; imaginary*=2.0/count;
    double residual=0;
    for (size_t i=0;i<count;++i) {
        const double angle=pi2*frequency*double(i)/rate;
        const double difference=double(output[first+i])-real*std::cos(angle)-imaginary*std::sin(angle);
        residual+=difference*difference;
    }
    return std::sqrt((residual/count)/((real*real+imaginary*imaginary)*.5));
}
std::vector<float> irregular(int count,int rate) {
    std::vector<float> input(size_t(count),0);
    uint32_t seed=9871;
    for (int i=0;i<count;++i) {
        seed=1664525*seed+1013904223;
        const double random=double(int32_t(seed))/2147483648.0;
        const double amplitude=(i/(rate/25))%7==0 ? 1.4 : .06;
        input[size_t(i)]=float(amplitude*(.5*std::sin(pi2*73*i/rate)+
                              .3*std::sin(pi2*997*i/rate)+.2*random));
        if (i%(rate/20)==0) input[size_t(i)]=i%2 ? -1.9f : 1.9f;
    }
    return input;
}
}

int main() {
    std::cout<<std::fixed<<std::setprecision(8)<<"SteadyVoice adaptive guard quality tests\n";
    require(physical(AdaptiveGuard,defaults()[AdaptiveGuard])==0,
            "Adaptive guard must remain disabled in defaults for compatibility");

    constexpr int rate=48000;
    double legacy20=0,adaptive20=0;
    for (const double reduction:{.1,1.0,8.56}) {
        for (const double delay:{0.0,5.0,10.0}) {
            const int latency=int(std::lround(rate*delay*.001));
            for (const int frequency:{20,30,40,80,1000}) {
                const auto input=tone(rate*2+latency,rate,frequency);
                const auto legacy=process(input,isolated(false,delay,reduction));
                const auto adaptive=process(input,isolated(true,delay,reduction));
                const double legacyResidual=toneResidual(legacy,size_t(rate+latency),rate,rate,frequency);
                const double adaptiveResidual=toneResidual(adaptive,size_t(rate+latency),rate,rate,frequency);
                std::cout<<"frequency_hz="<<frequency<<" delay_ms="<<delay
                         <<" reduction_db="<<reduction<<" legacy_residual_pct="<<legacyResidual*100
                         <<" adaptive_residual_pct="<<adaptiveResidual*100<<'\n';
                require(adaptiveResidual<1e-5,"Adaptive guard must avoid steady-tone gain modulation");
                require(adaptiveResidual<=legacyResidual+1e-7,
                        "Adaptive guard must not add measurable distortion relative to legacy guard");
                require(peak(adaptive)<=dbGain(-1)+1e-7,
                        "Adaptive guard must preserve the native sample ceiling");
                if (frequency==20 && reduction==8.56 && delay==0) {
                    legacy20=legacyResidual; adaptive20=adaptiveResidual;
                }
            }
        }
    }
    require(legacy20>.01 && adaptive20<legacy20*.001,
            "The regression fixture must expose and remove legacy low-frequency modulation");

    for (const double delay:{0.0,5.0,10.0}) {
        const auto quiet=tone(rate,rate,73,.03);
        require(process(quiet,isolated(false,delay))==process(quiet,isolated(true,delay)),
                "Adaptive guard must leave audio below the ceiling unchanged");
        auto disabled=isolated(true,delay); disabled[Guard]=0;
        auto legacyDisabled=disabled; legacyDisabled[AdaptiveGuard]=0;
        const auto loud=irregular(rate/2,rate);
        require(process(loud,disabled)==process(loud,legacyDisabled),
                "An inactive guard must be independent of the adaptive recovery setting");
    }

    // Real microphones add noise to recurring waveform peaks. A crest/valley
    // tracker and small refresh tolerance must avoid turning that jitter into
    // gain modulation. The residual should contain essentially the input's
    // noise alone for these modest noise levels.
    for (const double noise:{.0001,.001,.01}) {
        for (const int frequency:{20,40}) {
            for (const double delay:{0.0,5.0,10.0}) {
                const int latency=int(std::lround(rate*delay*.001));
                auto input=tone(rate*3+latency,rate,frequency);
                uint32_t seed=12;
                for (auto& sample:input) {
                    seed=1664525*seed+1013904223;
                    sample+=float(noise*double(int32_t(seed))/2147483648.0);
                }
                const auto output=process(input,isolated(true,delay));
                const double source=toneResidual(input,size_t(rate*2),rate,rate,frequency);
                const double result=toneResidual(output,size_t(rate*2+latency),rate,rate,frequency);
                require(result<=source*1.03,
                        "Small peak jitter must not create measurable adaptive guard modulation");
            }
        }
    }

    for (const int sampleRate:{8000,44100,48000,96000,192000,384000}) {
        for (const double delay:{0.0,5.0,10.0,20.0}) {
            auto input=irregular(sampleRate/2,sampleRate);
            auto settings=isolated(true,delay);
            const auto small=process(input,settings,sampleRate,1);
            const auto large=process(input,settings,sampleRate,511,true);
            require(small==large,"Adaptive guard must be independent of host block boundaries and channel duplication");
            require(peak(small)<=dbGain(-1)+1e-7,
                    "Irregular audio must remain below the native sample ceiling at every rate");
            require(std::all_of(small.begin(),small.end(),[](float x){return std::isfinite(x);}),
                    "Irregular audio must remain finite at every rate");
        }
    }

    // A sustained low note followed by a quiet phrase exercises the longest
    // typical learned hold. Recovery must not stick after the note ends.
    for (const double delay:{0.0,5.0,10.0}) {
        const int latency=int(std::lround(rate*delay*.001));
        auto input=tone(rate*2+latency,rate,20);
        for (int i=rate;i<int(input.size());++i)
            input[size_t(i)]=float(.003*std::sin(pi2*1000*i/rate));
        const auto settings=isolated(true,delay);
        const auto output=process(input,settings);
        const double trim=dbGain(physical(Trim,settings[Trim]));
        const size_t point=size_t(rate+int(rate*.65)+latency+12);
        require(std::abs(output[point]/(input[point-size_t(latency)]*trim)-1)<.002,
                "Guard must recover to within 0.2 percent of unity after a quiet tail");

        auto plosive=tone(rate+latency,rate,1000,.003);
        for (int i=0;i<rate/200;++i)
            plosive[size_t(i)]=float(.95*std::cos(pi2*50*i/rate)*std::exp(-double(i)/(rate*.004)));
        const auto recovered=process(plosive,settings);
        const size_t after=size_t(int(rate*.65)+latency+12);
        require(std::abs(recovered[after]/(plosive[after-size_t(latency)]*trim)-1)<.002,
                "An isolated plosive must not retain attenuation on a subsequent quiet phrase");
    }

    // Reset clears recurrence and held reduction. Disabled guard must also
    // clear adaptive history, while still applying the leveler's trim.
    for (const double delay:{0.0,5.0}) {
        auto settings=isolated(true,delay);
        Leveler warmed,fresh; warmed.prepare(rate,settings); fresh.prepare(rate,settings);
        auto note=tone(rate,rate,20),warmOutput=note;
        warmed.process<float>(note.data(),nullptr,warmOutput.data(),nullptr,rate);
        warmed.reset();
        auto probe=irregular(rate/5,rate),a=probe,b=probe;
        warmed.process<float>(probe.data(),nullptr,a.data(),nullptr,int(probe.size()));
        fresh.process<float>(probe.data(),nullptr,b.data(),nullptr,int(probe.size()));
        require(a==b,"Reset must remove all adaptive guard history");
        settings[Guard]=0; warmed.configure(settings);
        auto quiet=tone(rate/2,rate,997,.02),unguarded=quiet;
        warmed.process<float>(quiet.data(),nullptr,unguarded.data(),nullptr,int(quiet.size()));
        require(warmed.meters().guardReduction==0,"Disabled guard must not retain guard reduction");
        settings[Guard]=1; warmed.configure(settings);
        warmed.reset(); fresh.prepare(rate,settings);
        warmed.process<float>(probe.data(),nullptr,a.data(),nullptr,int(probe.size()));
        fresh.process<float>(probe.data(),nullptr,b.data(),nullptr,int(probe.size()));
        require(a==b,"Re-enabling a reset guard must match a fresh processor");
    }

    // Change the ceiling and delay while limiting: existing queued forecasts
    // cannot be trusted, so the same-sample safety cap must still apply.
    auto settings=isolated(true,5);
    Leveler changed; changed.prepare(rate,settings);
    auto loud=irregular(rate/2,rate),out=loud;
    changed.process<float>(loud.data(),nullptr,out.data(),nullptr,int(loud.size()));
    settings[Ceiling]=normalized(Ceiling,-9); changed.configure(settings);
    changed.process<float>(loud.data(),nullptr,out.data(),nullptr,int(loud.size()));
    require(peak(out)<=dbGain(-9)+1e-7,"A lower ceiling must immediately protect queued audio");
    settings[Lookahead]=normalized(Lookahead,10); changed.configure(settings);
    changed.process<float>(loud.data(),nullptr,out.data(),nullptr,int(loud.size()));
    require(peak(out)<=dbGain(-9)+1e-7,"A changed lookahead must preserve the ceiling");

    std::cout<<"PASS assertions="<<assertions<<'\n';
    return 0;
}
