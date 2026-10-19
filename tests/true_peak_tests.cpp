#include "../src/true_peak.hpp"
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <random>
#include <vector>

namespace {
constexpr double Pi=3.1415926535897932384626433832795;
int assertions=0;
void require(bool pass,const char* what) {
    ++assertions;
    if (!pass) { std::cerr<<"FAIL: "<<what<<'\n'; std::exit(1); }
}
double db(double amplitude) { return 20*std::log10(std::max(amplitude,1e-30)); }

// Independent offline reference: 16x, 257 taps, Blackman window. Production
// uses 4x/8x, 128 taps, a Kaiser window, and streaming duplicated ring history.
// Analytical sinusoid checks below are independent of FIR reconstruction.
double referencePeak(const std::vector<double>& x) {
    constexpr int Delay=128,Taps=257,Factor=16;
    std::array<std::array<double,Taps>,Factor-1> c{};
    for (int p=1;p<Factor;++p) {
        double sum=0;
        for (int k=0;k<Taps;++k) {
            const double d=k-Delay+double(p)/Factor;
            const double w=std::abs(d)<=Delay
                ? .42+.5*std::cos(Pi*d/Delay)+.08*std::cos(2*Pi*d/Delay) : 0;
            c[p-1][k]=std::sin(Pi*d)/(Pi*d)*w;
            sum+=c[p-1][k];
        }
        for (double& a:c[p-1]) a/=sum;
    }
    double peak=0;
    for (double a:x) peak=std::max(peak,std::abs(a));
    for (int i=-Delay;i<int(x.size())+Delay;++i) {
        for (int p=1;p<Factor;++p) {
            double y=0;
            for (int k=0;k<Taps;++k) {
                const int source=i-k+Delay;
                if (source>=0 && source<int(x.size())) y+=c[p-1][k]*x[source];
            }
            peak=std::max(peak,std::abs(y));
        }
    }
    return peak;
}
double estimatedPeak(const std::vector<double>& x,int factor) {
    sv::TruePeakDetector d; d.setFactor(factor);
    double peak=0;
    for (int i=0;i<int(x.size())+sv::TruePeakDetector::Taps;++i) {
        const double v=i<int(x.size()) ? x[i] : 0;
        peak=std::max(peak,d.process(v,0));
    }
    return peak;
}
double analyticIntervalPeak(double phase,double normalizedFrequency) {
    const double end=phase+2*Pi*normalizedFrequency;
    double peak=std::max(std::abs(std::cos(phase)),std::abs(std::cos(end)));
    if (std::ceil(phase/Pi)*Pi<=end) peak=1;
    return peak;
}
double sinusoidWorstError(int factor,double highestFrequency) {
    double worst=0;
    for (int f=1;f<=100;++f) {
        const double frequency=highestFrequency*f/100;
        for (int p=0;p<8;++p) {
            const double phase=p*Pi/8;
            sv::TruePeakDetector d; d.setFactor(factor);
            for (int n=0;n<384;++n) {
                const double sample=std::cos(2*Pi*frequency*n+phase);
                const double estimate=d.process(sample,0);
                if (n>=256) {
                    const double reference=analyticIntervalPeak(
                        2*Pi*frequency*(n-sv::TruePeakDetector::Latency)+phase,frequency);
                    worst=std::max(worst,db(reference/estimate));
                }
            }
        }
    }
    return worst;
}
std::vector<double> burst(double frequency,double phase,double width) {
    std::vector<double> x(384);
    for (int i=0;i<int(x.size());++i) {
        const double offset=(i-192)/width;
        x[i]=std::cos(2*Pi*frequency*i+phase)*std::exp(-.5*offset*offset);
    }
    return x;
}
} // namespace

int main() {
    std::cout<<std::fixed<<std::setprecision(6);
    require(sv::TruePeakDetector::Latency==63,"FIR interval timestamp must remain 63 samples behind input");
    sv::TruePeakDetector a,b;
    for (int i=0;i<100;++i) require(a.process(0,0)==0,"Silent estimator must be exactly zero");
    for (int i=0;i<200;++i) a.process(.125,-.25);
    require(std::abs(a.process(.125,-.25)-.25)<1e-14,"DC normalization must preserve either channel peak");
    a.reset();
    for (int i=0;i<100;++i) require(a.process(0,0)==0,"Reset must clear the entire reconstruction history");

    std::mt19937 rng(4215);
    std::uniform_real_distribution<double> uniform(-2,2);
    for (int i=0;i<1000;++i) {
        const double l=uniform(rng),r=uniform(rng);
        require(a.process(l,r)==b.process(r,l),"Linked peak estimate must be channel-order independent");
    }
    a.setFactor(4);require(a.factor()==4,"Fourfold estimator must select every other fractional phase");
    a.reset();require(a.factor()==4,"Reset must preserve configured oversampling");
    a.setFactor(8);require(a.factor()==8,"Eightfold estimator must restore every fractional phase");

    // Force the stereo convolution in a control instance using an inaudible
    // nonzero other channel, and compare against the silent-channel shortcut.
    // Different mute/unmute points exercise a complete history flush.
    for (int factor:{4,8}) {
        sv::TruePeakDetector shortcut,control;
        shortcut.setFactor(factor);control.setFactor(factor);
        for (int i=0;i<1400;++i) {
            const bool active=(i>=200 && i<550) || (i>=850 && i<1000);
            const double signal=active ? .3*std::cos(i*.172)+.1*std::sin(i*.593) : 0;
            const double fast=shortcut.process(signal,0);
            const double full=control.process(signal,1e-25);
            require(fast==full || (fast==0 && full<1e-24),
                    "Silent-channel shortcut must preserve exact active-channel interpolation");
            if (i>=1000+sv::TruePeakDetector::Taps)
                require(fast==0,"Muted channel must become exactly zero after complete FIR history flush");
        }
    }

    std::vector<double> quarter(512);
    double samplePeak=0;
    for (int i=0;i<int(quarter.size());++i) {
        quarter[i]=std::cos(Pi*.5*i+Pi*.25);
        samplePeak=std::max(samplePeak,std::abs(quarter[i]));
    }
    require(db(samplePeak)<-3.0,"Adversarial quarter-rate phase must hide a 3 dB sample-peak error");
    require(estimatedPeak(quarter,8)>.995,"Eightfold detector must recover quarter-rate intersample peaks");
    require(estimatedPeak(quarter,4)>.995,"Fourfold detector must recover quarter-rate intersample peaks");

    const double voice4=sinusoidWorstError(4,10000./48000);
    const double voice8=sinusoidWorstError(8,10000./48000);
    const double broad4=sinusoidWorstError(4,.45);
    const double broad8=sinusoidWorstError(8,.45);
    std::cout<<"analytic_voice_10k_underread_db 4x="<<voice4<<" 8x="<<voice8<<'\n'
             <<"analytic_to_0.45fs_underread_db 4x="<<broad4<<" 8x="<<broad8<<'\n';
    require(voice4<.15,"Fourfold speech-band estimate must remain within .15 dB analytically");
    require(voice8<.05,"Eightfold speech-band estimate must remain within .05 dB analytically");
    require(broad4<.8,"Fourfold analytic error through .45 Fs must fit the documented margin");
    require(broad8<.3,"Eightfold analytic error through .45 Fs must fit the documented margin");

    double voiceBurst4=0,voiceBurst8=0,hfBurst4=0,hfBurst8=0;
    for (double frequency:{.05,.10,.20,.30,.40,.49}) {
        for (double width:{2.,4.,8.,12.,16.,32.}) {
            for (double phase:{0.,Pi/4,Pi/2,5.105088062083414}) {
                const auto x=burst(frequency,phase,width);
                const double reference=referencePeak(x);
                const double error4=db(reference/estimatedPeak(x,4));
                const double error8=db(reference/estimatedPeak(x,8));
                if (frequency<=.40) {
                    voiceBurst4=std::max(voiceBurst4,error4);
                    voiceBurst8=std::max(voiceBurst8,error8);
                } else {
                    hfBurst4=std::max(hfBurst4,error4);
                    hfBurst8=std::max(hfBurst8,error8);
                }
            }
        }
    }
    std::cout<<"gaussian_burst_to_0.40fs_vs16x_db 4x="<<voiceBurst4<<" 8x="<<voiceBurst8<<'\n'
             <<"known_near_nyquist_burst_underread_db 4x="<<hfBurst4<<" 8x="<<hfBurst8<<'\n';
    require(voiceBurst4<.8,"Fourfold below-.4Fs burst coverage must fit nominal margin");
    require(voiceBurst8<.3,"Eightfold below-.4Fs burst coverage must fit nominal margin");

    double multi4=0,multi8=0;
    for (int trial=0;trial<16;++trial) {
        std::vector<double> x(384);
        for (int harmonic=0;harmonic<12;++harmonic) {
            const double frequency=(harmonic+1)*.4/12;
            const double phase=uniform(rng)*Pi;
            for (int i=0;i<int(x.size());++i) {
                const double t=(i-192)/40.;
                x[i]+=std::cos(2*Pi*frequency*i+phase)*std::exp(-.5*t*t)/(harmonic+1);
            }
        }
        const double reference=referencePeak(x);
        multi4=std::max(multi4,db(reference/estimatedPeak(x,4)));
        multi8=std::max(multi8,db(reference/estimatedPeak(x,8)));
    }
    std::cout<<"voiceband_multitone_underread_db 4x="<<multi4<<" 8x="<<multi8<<'\n';
    require(multi4<.8 && multi8<.3,"Band-limited multitone peak estimation must fit nominal margins");
    std::cout<<"true_peak_assertions="<<assertions<<'\n';
}
