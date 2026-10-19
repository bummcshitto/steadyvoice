#include "../src/dsp.hpp"
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {
using namespace sv;
constexpr double pi=3.1415926535897932384626433832795;
constexpr double pi2=2*pi;
int assertions=0,failures=0;
void require(bool pass,const std::string& message) {
    ++assertions;
    if (!pass) {
        ++failures;
        if (failures<=20) std::cerr<<"FAIL: "<<message<<'\n';
    }
}
Values isolated(int mode,double lookahead=5) {
    auto p=defaults();
    p[Strength]=0;
    p[Guard]=1;
    p[AdaptiveGuard]=1;
    p[TruePeak]=normalized(TruePeak,float(mode));
    p[Ceiling]=normalized(Ceiling,-1);
    p[Lookahead]=normalized(Lookahead,float(lookahead));
    return p;
}
double samplePeak(const std::vector<float>& audio) {
    double p=0;
    for (float x:audio) p=std::max(p,std::abs(double(x)));
    return p;
}
struct Render {std::vector<float> audio; int latency=0;};
Render render(const std::vector<float>& input,const Values& p,double rate=48000,
              int block=127,bool inPlace=false,bool duplicate=false,bool silentRight=false) {
    Leveler d; d.prepare(rate,p);
    Render r; r.audio=inPlace ? input : std::vector<float>(input.size());
    r.latency=d.latencySamples();
    std::vector<float> right(duplicate || silentRight ? input.size() : 0);
    std::vector<float> silence(silentRight ? input.size() : 0);
    for (size_t first=0;first<input.size();first+=size_t(block)) {
        const int n=int(std::min(size_t(block),input.size()-first));
        const float* left=inPlace ? r.audio.data()+first : input.data()+first;
        d.process<float>(left,duplicate ? input.data()+first : silentRight ? silence.data()+first : nullptr,
                         r.audio.data()+first,duplicate || silentRight ? right.data()+first : nullptr,n);
    }
    if (duplicate) require(r.audio==right,"Duplicated channels must remain identical");
    if (silentRight) require(samplePeak(right)==0,"A silent right channel must remain exactly silent");
    return r;
}

// Independent offline reconstruction: 16 phases, 512 taps, a Blackman window,
// and forward-centred dot products. The production detector uses a shorter
// causal Kaiser polyphase bank. This is a stronger test oracle, not proof of
// an exact ideal-sinc or arbitrary downstream true-peak bound.
class ReferencePeak {
    static constexpr int Phases=16,Taps=512,Half=Taps/2;
    std::array<std::array<double,Taps>,Phases> coefficients_{};
public:
    ReferencePeak() {
        for (int phase=1;phase<Phases;++phase) {
            double sum=0;
            for (int k=0;k<Taps;++k) {
                const double distance=k-Half-double(phase)/Phases;
                const double u=distance/Half;
                const double window=std::abs(u)<=1 ? .42+.5*std::cos(pi*u)+.08*std::cos(pi2*u) : 0;
                const double sinc=distance==0 ? 1 : std::sin(pi*distance)/(pi*distance);
                coefficients_[phase][k]=sinc*window;
                sum+=coefficients_[phase][k];
            }
            for (double& c:coefficients_[phase]) c/=sum;
        }
    }
    double peak(const std::vector<float>& audio,size_t first=0,size_t count=0) const {
        if (count==0) count=audio.size()-first;
        const size_t end=std::min(audio.size(),first+count);
        double result=0;
        // Explicit padding lets the inner convolution contain no boundary
        // condition and also includes the reconstructed finite-signal tails.
        std::vector<double> padded(audio.size()+Taps*2,0);
        for (size_t i=0;i<audio.size();++i) padded[i+Taps]=audio[i];
        const int begin=int(first)-(first==0 ? Half : 0);
        const int finish=int(end)+(end==audio.size() ? Half : 0);
        for (int n=begin;n<finish;++n) {
            result=std::max(result,std::abs(padded[size_t(n+Taps)]));
            const double* x=padded.data()+n+Taps-Half;
            for (int phase=1;phase<Phases;++phase) {
                double y=0;
                for (int k=0;k<Taps;++k) y+=x[k]*coefficients_[phase][k];
                result=std::max(result,std::abs(y));
            }
        }
        return result;
    }
};

struct Worst {
    double excess=-1e10;
    std::string fixture;
    void add(double peak,double ceiling,const std::string& name) {
        const double db=20*std::log10(std::max(peak,1e-20)/ceiling);
        if (db>excess) {excess=db;fixture=name;}
    }
    void check(const std::string& group,int mode,double lookahead) const {
        std::cout<<group<<" mode="<<mode<<" lookahead_ms="<<lookahead
                 <<" maximum_reference_excess_db="<<excess<<" fixture="<<fixture<<'\n';
        require(excess<=.05,group+" independently reconstructed peak exceeds ceiling by "+std::to_string(excess)+" dB: "+fixture);
    }
};
std::vector<float> burst(double frequency,double phase,double width,size_t size=1536) {
    std::vector<float> input(size,0);
    for (size_t i=0;i<size;++i) {
        const double distance=(double(i)-384)/width;
        input[i]=float(2*std::cos(pi2*frequency*i+phase)*std::exp(-.5*distance*distance));
    }
    return input;
}
std::string description(double frequency,double phase,double width) {
    std::ostringstream s;s<<"f="<<frequency<<" phase="<<phase<<" width="<<width;return s.str();
}
double fittedAmplitude(const std::vector<float>& y,size_t first,size_t count,double frequency,
                       double* residual=nullptr) {
    double yc=0,ys=0,cc=0,ss=0,cs=0;
    for (size_t i=0;i<count;++i) {
        const double c=std::cos(pi2*frequency*i),s=std::sin(pi2*frequency*i);
        yc+=y[first+i]*c;ys+=y[first+i]*s;cc+=c*c;ss+=s*s;cs+=c*s;
    }
    const double determinant=cc*ss-cs*cs;
    const double a=(yc*ss-ys*cs)/determinant,b=(ys*cc-yc*cs)/determinant;
    if (residual) {
        double energy=0;
        for (size_t i=0;i<count;++i) {
            const double e=y[first+i]-a*std::cos(pi2*frequency*i)-b*std::sin(pi2*frequency*i);
            energy+=e*e;
        }
        *residual=std::sqrt(energy/count)/std::hypot(a,b);
    }
    return std::hypot(a,b);
}
}

int main(int argc,char** argv) {
    std::cout<<std::fixed<<std::setprecision(8)<<"SteadyVoice peak guard integration tests\n";
    ReferencePeak reference;
    constexpr double ceiling=.8912509381337456; // -1 dBFS, independent of dbGain.

    // The known phase-shifted fs/4 sine has intersample peaks that are larger
    // than its sample peaks. Ensure the independent oracle exposes this.
    std::vector<float> witness(2048);
    for (size_t i=0;i<witness.size();++i) witness[i]=float(std::cos(pi2*.25*i+pi/4));
    require(reference.peak(witness,512,1024)>samplePeak(witness)*1.40,
            "Reference must expose a genuine intersample-peak fixture");

    if (argc>1 && std::string(argv[1])=="--attack-probe") {
        const std::vector<double> requested=argc>2 ? std::vector<double>{std::atof(argv[2])}
                                                 : std::vector<double>{.25,.5,1.0,2.0};
        for (int mode=1;mode<=2;++mode) for (const double lookahead:requested) {
            Worst worst;
            for (const double frequency:{.25,.40,.45,.475,.49}) {
                for (const double width:{2.0,8.0,16.0}) for (const double phase:{0.0,pi/2,5.105088062083414}) {
                    const auto output=render(burst(frequency,phase,width),isolated(mode,lookahead));
                    require(samplePeak(output.audio)<=ceiling+1e-7,"Attack probe must retain native sample ceiling");
                    worst.add(reference.peak(output.audio),ceiling,description(frequency,phase,width));
                }
            }
            worst.check("attack_probe",mode,lookahead);
        }
        std::cout<<(failures ? "FAIL" : "PASS")<<" assertions="<<assertions<<" failures="<<failures<<'\n';
        return failures ? 1 : 0;
    }

    // Guard Off and initial bypass preserve exact dry samples and their
    // advertised latency. A peak-quality mode does not silently filter audio.
    for (const double rate:{8000.0,44100.0,48000.0,96000.0,192000.0,384000.0}) {
        for (const double lookahead:{0.0,5.0,20.0}) for (int mode=0;mode<=2;++mode) {
            auto p=isolated(mode,lookahead);
            const double effectiveLookahead=mode>0 ? std::max(5.0,lookahead) : lookahead;
            const int expected=int(std::lround(rate*effectiveLookahead*.001))
                +(mode==1 ? TruePeak4xLatencySamples : mode==2 ? TruePeak8xLatencySamples : 0);
            require(processingLatencySamples(rate,p)==expected,"Published latency helper must reflect rate, lookahead and detector lag");
            std::vector<float> input(size_t(expected+512),0);
            for (size_t i=0;i<256;++i) input[i]=float(((int(i)*37)%101-50)*.03);
            for (int bypass=0;bypass<=1;++bypass) {
                p[Guard]=bypass ? 1 : 0;p[Bypass]=float(bypass);
                const auto output=render(input,p,rate,113);
                require(output.latency==expected,"DSP must report exactly the latency it implements");
                bool exact=true;
                for (size_t i=0;i<input.size();++i) {
                    const float desired=i<size_t(expected) ? 0 : input[i-size_t(expected)];
                    exact=exact && output.audio[i]==desired;
                }
                require(exact,"Guard Off/bypass must preserve delayed dry audio exactly at every supported rate");
            }
        }
    }

    for (int mode=0;mode<=2;++mode) for (const double lookahead:{0.0,5.0,20.0}) {
        auto p=isolated(mode,lookahead);
        auto input=burst(.45,pi/4,12,3072);
        for (size_t i=0;i<input.size();++i) input[i]+=float(.025*std::sin(pi2*.023*i));
        const auto samples=render(input,p,48000,1);
        const auto blocks=render(input,p,48000,511,false,true);
        const auto inPlace=render(input,p,48000,73,true);
        const auto actualStereo=render(input,p,48000,193,false,false,true);
        require(samples.audio==blocks.audio,"Guard audio must be block-independent and mono/stereo-independent");
        require(samples.audio==inPlace.audio,"Guard must support exact in-place processing");
        require(samples.audio==actualStereo.audio,"A null or actual silent right channel must have identical gain behavior");
        require(samplePeak(samples.audio)<=ceiling+1e-7,"Every peak mode must retain the native sample ceiling");
    }

    // Analytic reconstruction of a settled sine has amplitude hypot(a,b).
    // This check avoids using the production FIR as its own correctness oracle.
    for (int mode=1;mode<=2;++mode) for (const double lookahead:{0.0,5.0}) {
        Worst worst;
        for (const double frequency:{1000.0/48000,10000.0/48000,.25,.375,.45,.475,.49}) {
            for (const double phase:{0.0,pi/4,5.105088062083414}) {
                auto p=isolated(mode,lookahead);
                std::vector<float> input(24000,0);
                for (size_t i=0;i<input.size();++i) input[i]=float(2*std::cos(pi2*frequency*i+phase));
                const auto output=render(input,p);
                double residual=0;
                const double amplitude=fittedAmplitude(output.audio,16000,4096,frequency,&residual);
                require(residual<1e-5,"Settled high-frequency limiter must preserve a pure sine waveform");
                require(samplePeak(output.audio)<=ceiling+1e-7,"Settled tones must retain the sample ceiling");
                worst.add(amplitude,ceiling,description(frequency,phase,0));
            }
        }
        worst.check("settled_sine",mode,lookahead);
    }

    // Gaussian bursts include the finite-FIR near-Nyquist failure class.
    // Zero padding and complete tails are measured so delayed onset or
    // reconstruction ringing cannot disappear from the oracle's search.
    for (int mode=1;mode<=2;++mode) for (const double lookahead:{0.0,5.0}) {
        Worst worst;
        for (const double frequency:{.25,.40,.45,.475,.49}) {
            for (const double width:{2.0,4.0,8.0,12.0,16.0,32.0}) for (const double phase:{0.0,pi/4,pi/2,5.105088062083414}) {
                const auto input=burst(frequency,phase,width);
                const auto output=render(input,isolated(mode,lookahead));
                require(samplePeak(output.audio)<=ceiling+1e-7,"Burst audio must retain the native sample ceiling");
                worst.add(reference.peak(output.audio),ceiling,description(frequency,phase,width));
            }
        }
        worst.check("gaussian_burst",mode,lookahead);
    }

    for (int mode=1;mode<=2;++mode) for (const double lookahead:{0.0,5.0}) {
        Worst worst;
        for (int phase=0;phase<5;++phase) {
            std::vector<float> input(2048,0);
            for (size_t i=0;i<input.size();++i) {
                const double envelope=i<256 || i>1792 ? 0 : 1;
                input[i]=float(envelope*(.7*std::cos(pi2*.137*i+phase*.37)
                          +.8*std::cos(pi2*.371*i+phase*.61)
                          +.8*std::cos(pi2*.489*i+phase*.83)));
            }
            const auto output=render(input,isolated(mode,lookahead));
            require(samplePeak(output.audio)<=ceiling+1e-7,"Multitone audio must retain the native sample ceiling");
            worst.add(reference.peak(output.audio),ceiling,"phase="+std::to_string(phase));
        }
        worst.check("multitone",mode,lookahead);
    }

    // The FIR sees nominal leveled samples, so a loud onset while the leveler
    // holds quiet-speech boost exercises detector/gain/delay alignment.
    for (int mode=1;mode<=2;++mode) for (const double lookahead:{0.0,5.0}) {
        Worst worst;
        for (const float reduce:{0.0f,20.0f}) {
            auto p=isolated(mode,lookahead);
            p[Strength]=1;p[Boost]=1;p[Cut]=1;
            p[Target]=normalized(Target,-10);p[Lift]=normalized(Lift,100);
            p[Reduce]=normalized(Reduce,reduce);
            p[Adaptive]=1;
            constexpr size_t onset=18000;
            std::vector<float> input(onset+4096,0);
            for (size_t i=0;i<input.size();++i) {
                const double amplitude=i<onset ? .002 : .8;
                input[i]=float(amplitude*(std::sin(pi2*157*i/48000)
                          +.3*std::sin(pi2*10937*i/48000)+.2*std::sin(pi2*22999*i/48000)));
            }
            const auto output=render(input,p);
            require(samplePeak(output.audio)<=ceiling+1e-7,"Gain transitions must retain the native sample ceiling");
            const size_t first=onset+size_t(output.latency)-128;
            worst.add(reference.peak(output.audio,first,2048),ceiling,"reduce_ms="+std::to_string(reduce));
        }
        worst.check("quiet_to_loud",mode,lookahead);
    }

    // Muting/unmuting one native channel must retain its FIR history for
    // linked detection without placing interpolation ringing in audio itself.
    for (int mode=1;mode<=2;++mode) {
        auto p=isolated(mode,5);
        Leveler d;d.prepare(48000,p);
        constexpr size_t size=4096;
        std::vector<float> left(size),right(size,0),outLeft(size),outRight(size);
        for (size_t i=0;i<size;++i) left[i]=float(.03*std::sin(pi2*997*i/48000));
        for (size_t centre:{size_t(512),size_t(1536),size_t(2560)}) {
            for (size_t i=centre-48;i<centre+48;++i) {
                const double distance=(double(i)-centre)/8;
                right[i]=float(2*std::cos(pi2*.475*i+5.105088062083414)*std::exp(-.5*distance*distance));
            }
        }
        d.process<float>(left.data(),right.data(),outLeft.data(),outRight.data(),int(size));
        require(std::max(samplePeak(outLeft),samplePeak(outRight))<=ceiling+1e-7,
                "Stereo mute/unmute history must retain the native sample ceiling");
        require(std::max(reference.peak(outLeft),reference.peak(outRight))<=ceiling*std::pow(10,.05/20),
                "Stereo mute/unmute history must retain the independently reconstructed peak ceiling");
        bool noAudioRinging=true;
        for (size_t i=size_t(d.latencySamples());i<size;++i)
            if (right[i-size_t(d.latencySamples())]==0) noAudioRinging=noAudioRinging && outRight[i]==0;
        require(noAudioRinging,"The detector must never insert FIR ringing into the dry audio path");
    }

    // Changing peak quality or the ceiling must leave no unsafe stale cap,
    // allocation-dependent state, or mismatched public latency.
    for (int mode=1;mode<=2;++mode) for (const double lookahead:{0.0,5.0,10.0}) {
        auto p=isolated(mode,lookahead);p[Ceiling]=normalized(Ceiling,0);
        Leveler d;d.prepare(48000,p);
        std::vector<float> input(4096),warm(4096),output(4096);
        for (size_t i=0;i<input.size();++i) input[i]=float(.8*std::cos(pi2*.25*i+pi/4));
        d.process<float>(input.data(),nullptr,warm.data(),nullptr,int(input.size()));
        p[Ceiling]=normalized(Ceiling,-9);d.configure(p);
        d.process<float>(input.data(),nullptr,output.data(),nullptr,int(input.size()));
        require(samplePeak(output)<=dbGain(-9)+1e-7,"Lowering the live ceiling must protect queued native samples immediately");
        // Reconstruct only newly emitted audio, not the already-emitted louder
        // block. Search early enough to cover the still-queued old forecasts,
        // while skipping the immediate cut boundary and the final tail.
        const double actual=reference.peak(output,64,1024);
        require(actual<=dbGain(-9)*std::pow(10,.05/20),
                "A lowered ceiling must protect queued true-peak intervals, not only their endpoint samples");
    }
    {
        auto p=isolated(0,5);
        Leveler d;d.prepare(48000,p);
        auto input=burst(.475,5.105088062083414,8,1536),output=input;
        for (int mode:{1,2,0,2,1}) {
            p[TruePeak]=normalized(TruePeak,float(mode));
            p[Ceiling]=normalized(Ceiling,mode==1 ? -9 : -1);
            d.configure(p);
            d.process<float>(input.data(),nullptr,output.data(),nullptr,int(input.size()));
            require(d.latencySamples()==processingLatencySamples(48000,p),
                    "Changing the FIR mode must update public latency coherently");
            require(samplePeak(output)<=dbGain(physical(Ceiling,p[Ceiling]))+1e-7,
                    "A changed FIR mode or ceiling must preserve the native sample ceiling immediately");
            require(std::all_of(output.begin(),output.end(),[](float x){return std::isfinite(x);}),
                    "Changing FIR mode must not create invalid samples");
            d.reset();
            Leveler fresh;fresh.prepare(48000,p);
            auto expected=input;
            d.process<float>(input.data(),nullptr,output.data(),nullptr,int(input.size()));
            fresh.process<float>(input.data(),nullptr,expected.data(),nullptr,int(input.size()));
            require(output==expected,"Reset must clear FIR and delay history after mode changes");
        }
    }

    std::cout<<(failures ? "FAIL" : "PASS")<<" assertions="<<assertions<<" failures="<<failures<<'\n';
    return failures ? 1 : 0;
}
