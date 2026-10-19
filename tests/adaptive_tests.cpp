#include "../src/dsp.hpp"
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <vector>

using namespace sv;

namespace {
int assertions=0;
constexpr double pi2=6.2831853071795864769;

void require(bool pass,const char* message) {
    ++assertions;
    if (!pass) { std::cerr<<"FAIL: "<<message<<'\n'; std::exit(1); }
}

template<class T> std::vector<T> tone(int samples,double sr,double rmsDb,double frequency=997) {
    std::vector<T> input(size_t(samples),T(0));
    const double amplitude=std::sqrt(2.0)*dbGain(rmsDb);
    for (int i=0;i<samples;++i) input[size_t(i)]=T(amplitude*std::sin(pi2*frequency*i/sr));
    return input;
}

template<class T> std::vector<T> run(const std::vector<T>& input,const Values& settings,
                                    double sr=48000,int block=128) {
    Leveler processor; processor.prepare(sr,settings);
    std::vector<T> output(input.size());
    for (size_t offset=0;offset<input.size();offset+=size_t(block)) {
        const int count=int(std::min(size_t(block),input.size()-offset));
        processor.process<T>(input.data()+offset,nullptr,output.data()+offset,nullptr,count);
    }
    return output;
}

template<class T> double rms(const std::vector<T>& samples,size_t begin,size_t end) {
    double sum=0;
    for (size_t i=begin;i<end;++i) sum+=double(samples[i])*double(samples[i]);
    return std::sqrt(sum/double(end-begin));
}

template<class T> double peak(const std::vector<T>& samples) {
    double value=0;
    for (T sample:samples) value=std::max(value,std::abs(double(sample)));
    return value;
}

template<class T> bool finite(const std::vector<T>& samples) {
    return std::all_of(samples.begin(),samples.end(),[](T sample){return std::isfinite(sample);});
}

// Coherent 1 kHz / 48 kHz measurements keep leakage out of the harmonic
// bins. Float samples exercise the actual host path and its quantization.
double toneThdDb(const std::vector<float>& samples,size_t begin,size_t count) {
    double fundamentalPower=0,harmonicPower=0;
    for (int harmonic=1;harmonic<=23;++harmonic) {
        double real=0,imaginary=0;
        for (size_t i=0;i<count;++i) {
            const double phase=pi2*harmonic*double(i)/48;
            real+=double(samples[begin+i])*std::cos(phase);
            imaginary+=double(samples[begin+i])*std::sin(phase);
        }
        const double power=real*real+imaginary*imaginary;
        if (harmonic==1) fundamentalPower=power;
        else harmonicPower+=power;
    }
    return 10*std::log10(std::max(harmonicPower/fundamentalPower,1e-30));
}

double adjacentGainStep(const std::vector<float>& input,const std::vector<float>& output,size_t delay) {
    double largest=0,previous=20*std::log10(std::abs(double(output[delay])/input[0]));
    for (size_t i=delay+1;i<output.size();++i) {
        const double gain=20*std::log10(std::abs(double(output[i])/input[i-delay]));
        largest=std::max(largest,std::abs(gain-previous));
        previous=gain;
    }
    return largest;
}

// DC isolates the full-band classifier from both carrier ripple and the
// high-pass detector. Audio output is immaterial to these selector checks.
double classifiedDuration(double level,const Values& settings) {
    Leveler processor; processor.prepare(48000,settings);
    std::vector<double> input(14400,dbGain(level)),output(input.size());
    processor.process<double>(input.data(),nullptr,output.data(),nullptr,int(input.size()));
    return processor.adaptiveRmsMs();
}

std::vector<float> alternating(double sr) {
    // Warm up on quiet speech, then alternate abrupt and brief loud passages.
    // The isolated peaks challenge held boost without clipping the input.
    const int quietWarm=int(sr*2), cycle=int(sr*.35), cycles=8, tail=int(sr*.15);
    std::vector<float> input(size_t(quietWarm+cycle*cycles+tail),0);
    for (int i=0;i<quietWarm+cycle*cycles;++i) {
        const int position=i<quietWarm ? -1 : (i-quietWarm)%cycle;
        const bool loud=position>=0 && position<int(sr*.07);
        const double level=loud ? -7.0 : -38.0;
        input[size_t(i)]=float(std::sqrt(2.0)*dbGain(level)*std::sin(pi2*997*i/sr));
        if (position==0) input[size_t(i)]=.98f;
        if (position==int(sr*.008)) input[size_t(i)]=-.94f;
    }
    return input;
}
}

int main() {
    static_assert(Guard==11 && Bypass==12 && RmsSoftest==13 && Adaptive==20 && AdaptiveGuard==21 && TruePeak==22 && ParamCount==23,
                  "Advanced parameters must preserve all original parameter indices");
    std::cout<<std::fixed<<std::setprecision(6)<<"SteadyVoice adaptive RMS and lookahead tests\n";

    const auto legacy=defaults();
    require(physical(Adaptive,legacy[Adaptive])==0,"Defaults must retain legacy RMS behavior");
    require(physical(Lookahead,legacy[Lookahead])==0,"Defaults must retain zero added latency");
    Leveler defaultProcessor; defaultProcessor.prepare(48000,legacy);
    require(defaultProcessor.latencySamples()==0,"Legacy default latency must be exactly zero");
    auto inactiveDurations=legacy;
    for (int parameter=RmsSoftest;parameter<=RmsLoudest;++parameter) inactiveDurations[parameter]=1;
    inactiveDurations[MedianRange]=1;
    auto reference=tone<float>(96000,48000,-30);
    require(run(reference,legacy)==run(reference,inactiveDurations),
            "Disabled adaptive controls must leave legacy audio unchanged");

    auto smooth=preset(4);
    require(physical(Adaptive,smooth[Adaptive])==1,"Smooth boost preset must enable adaptive RMS");
    require(std::abs(physical(Target,smooth[Target])+16)<.001,"Smooth boost must use a practical RMS target");
    require(std::abs(physical(Lookahead,smooth[Lookahead])-5)<.001,"Smooth boost must provide 5 ms lookahead");
    const double target=physical(Target,smooth[Target]);
    const double halfBand=physical(MedianRange,smooth[MedianRange])*.5;
    const double upperBand=target+halfBand;
    const double highStep=upperBand<0 ? std::min(12.0,-upperBand*.5) : 12.0;
    const int rmsParameters[]={RmsSoftest,RmsSoft,RmsMedian,RmsLoud,RmsLoudest};
    const double anchorLevels[]={target-halfBand-24,target-halfBand-12,target,
                                 upperBand+highStep,upperBand+2*highStep};
    for (int i=0;i<5;++i) {
        const double actual=classifiedDuration(anchorLevels[i],smooth);
        const double expected=physical(rmsParameters[i],smooth[rmsParameters[i]]);
        std::cout<<"rms_anchor_input_db="<<anchorLevels[i]<<" duration_ms="<<actual<<'\n';
        require(std::abs(actual-expected)<.03,"Each adaptive band must reach its configured RMS time constant");
    }
    const double median=physical(RmsMedian,smooth[RmsMedian]);
    for (double offset:{-halfBand,-halfBand*.5,0.0,halfBand*.5,halfBand}) {
        require(std::abs(classifiedDuration(target+offset,smooth)-median)<.03,
                "The entire median range must use the median RMS time constant");
    }
    double previous=std::numeric_limits<double>::infinity();
    for (int step=0;step<=80;++step) {
        const double duration=classifiedDuration(target-30+step*.75,smooth);
        require(std::isfinite(duration) && duration>0,"Adaptive RMS duration must remain positive and finite");
        require(duration<=previous+.001,"The preset must transition smoothly from longer quiet to shorter loud RMS");
        previous=duration;
    }
    for (double boundary:{target-halfBand-24,target-halfBand-12,target-halfBand,
                          upperBand,upperBand+highStep,upperBand+2*highStep}) {
        require(std::abs(classifiedDuration(boundary-.01,smooth)-classifiedDuration(boundary+.01,smooth))<.15,
                "Crossing an adaptive band boundary must not jump the RMS duration");
    }
    auto wideMedian=smooth; wideMedian[MedianRange]=normalized(MedianRange,12);
    require(std::abs(classifiedDuration(target-5,wideMedian)-median)<.03 &&
            std::abs(classifiedDuration(target+5,wideMedian)-median)<.03,
            "Widening the median range must move both classifier boundaries");
    // User durations need not follow the preset's ordering.
    auto reversed=smooth; reversed[RmsSoftest]=normalized(RmsSoftest,8);
    reversed[RmsLoudest]=normalized(RmsLoudest,200);
    require(std::abs(classifiedDuration(anchorLevels[0],reversed)-8)<.03 &&
            std::abs(classifiedDuration(anchorLevels[4],reversed)-200)<.03,
            "Independent RMS duration controls must support either ordering");

    // Hold the primary leveler at unity so these checks isolate the guard.
    // The quiet background makes pre-peak attenuation visible, while the
    // +12 dB trim ensures that an otherwise legal isolated peak overloads it.
    auto guardOnly=legacy;
    guardOnly[Strength]=0; guardOnly[Trim]=normalized(Trim,12);
    guardOnly[Ceiling]=normalized(Ceiling,-1); guardOnly[Guard]=1;
    std::vector<float> isolated(4800,.1f); constexpr size_t isolatedPeak=2400;
    isolated[isolatedPeak]=.9f;
    const auto hot=tone<float>(96000,48000,20*std::log10(.9/std::sqrt(2.0)),1000);
    std::array<double,3> guardSteps{};
    const int delaysMs[]={0,5,20};
    for (int i=0;i<3;++i) {
        auto settings=guardOnly; settings[Lookahead]=normalized(Lookahead,float(delaysMs[i]));
        const size_t delay=size_t(delaysMs[i]*48);
        const auto guarded=run(isolated,settings);
        guardSteps[size_t(i)]=adjacentGainStep(isolated,guarded,delay);
        require(peak(guarded)<=dbGain(-1)+1e-7,"The isolated-peak fixture must obey the guard ceiling");
        if (delay>0)
            require(guarded[isolatedPeak+delay-1]<.2f,
                    "Lookahead must lower gain before the isolated peak reaches the output");
        const auto cold=run(std::vector<float>(1200,.9f),settings);
        require(std::all_of(cold.begin(),cold.begin()+delay,[](float sample){return sample==0;}) &&
                std::abs(double(cold[delay])-dbGain(-1))<1e-7,
                "A hot cold start must reach the ceiling without a stale cap from silent delay frames");
        const auto settled=run(hot,settings);
        const size_t measurementBegin=72000+delay;
        const double thd=toneThdDb(settled,measurementBegin,4800);
        double settledPeak=0;
        for (size_t sample=measurementBegin;sample<measurementBegin+4800;++sample)
            settledPeak=std::max(settledPeak,std::abs(double(settled[sample])));
        std::cout<<"guard_lookahead_ms="<<delaysMs[i]<<" isolated_peak_gain_step_db="<<guardSteps[size_t(i)]
                 <<" settled_1khz_peak_db="<<amplitudeDb(settledPeak)<<" settled_1khz_thd_db="<<thd<<'\n';
        require(peak(settled)<=dbGain(-1)+1e-7 && settledPeak>dbGain(-1)*.99,
                "The steady-tone fixture must engage peak protection without exceeding its ceiling");
        require(thd<-85,"Settled guard processing must not modulate a sustained 1 kHz waveform");
    }
    require(guardSteps[0]>10,"The zero-lookahead fixture must expose an abrupt same-sample guard attack");
    require(guardSteps[1]<guardSteps[0]*.05 && guardSteps[1]<.2,
            "Five milliseconds of lookahead must materially smooth the isolated-peak guard attack");
    require(guardSteps[2]<guardSteps[1]*.3,
            "Twenty milliseconds of lookahead must further smooth the guard attack");

    const auto input=alternating(48000);
    const auto output=run(input,smooth,48000,1);
    require(output==run(input,smooth,48000,511),"Adaptive/lookahead output must be identical across host block sizes");
    require(finite(output),"Rapid quiet/loud alternation must produce finite samples");
    const double ceiling=dbGain(physical(Ceiling,smooth[Ceiling]));
    require(peak(output)<=ceiling+1e-7,"Lookahead guard must catch every quiet-to-loud onset and isolated peak");
    Leveler delayProbe; delayProbe.prepare(48000,smooth);
    const size_t latency=size_t(delayProbe.latencySamples());
    require(latency==240,"Five milliseconds at 48 kHz must report 240 samples of latency");
    const size_t quietBegin=48000+latency,quietEnd=90000+latency;
    const double softGain=20*std::log10(rms(output,quietBegin,quietEnd)/rms(input,48000,90000));
    require(softGain>20 && softGain<24.1,"Peak protection must retain useful gain on sustained quiet speech");
    const double loudRms=amplitudeDb(rms(output,96000+latency,99360+latency));
    require(loudRms>-24,"Peak protection must retain audible loud passages instead of muting transitions");
    auto unguarded=smooth; unguarded[Guard]=0;
    require(peak(run(input,unguarded))>1.2,"The onset fixture must challenge genuine held-boost overloads");
    double largestGainStep=0,previousGain=0; bool previousValid=false;
    for (size_t i=latency;i<output.size();++i) {
        const double original=input[i-latency];
        if (std::abs(original)<1e-5 || output[i]==0) { previousValid=false; continue; }
        const double gain=20*std::log10(std::abs(double(output[i])/original));
        if (previousValid) largestGainStep=std::max(largestGainStep,std::abs(gain-previousGain));
        previousGain=gain; previousValid=true;
    }
    std::cout<<"alternating_peak_db="<<amplitudeDb(peak(output))<<" sustained_soft_gain_db="<<softGain
             <<" first_loud_passage_rms_db="<<loudRms
             <<" largest_adjacent_gain_step_db="<<largestGainStep<<'\n';

    std::vector<double> doubleInput(input.begin(),input.end());
    const auto doubleOutput=run(doubleInput,smooth,48000,127);
    double precisionDifference=0;
    for (size_t i=0;i<output.size();++i)
        precisionDifference=std::max(precisionDifference,std::abs(double(output[i])-doubleOutput[i]));
    require(precisionDifference<1e-6,"Float and double processing must agree for the same input samples");
    require(peak(doubleOutput)<=ceiling+1e-12,"Double output must obey the configured sample ceiling");
    auto transparent=smooth; transparent[Strength]=0;
    const auto unmodified=run(reference,transparent);
    require(std::all_of(unmodified.begin(),unmodified.begin()+latency,[](float x){return x==0;}) &&
            std::equal(unmodified.begin()+latency,unmodified.end(),reference.begin()),
            "Lookahead guard must be exactly transparent when audio stays below the ceiling");
    Leveler inPlace; inPlace.prepare(48000,smooth);
    auto inPlaceOutput=input;
    inPlace.process<float>(inPlaceOutput.data(),nullptr,inPlaceOutput.data(),nullptr,int(inPlaceOutput.size()));
    require(inPlaceOutput==output,"Adaptive lookahead must support in-place host buffers");

    for (double sr:{8000.0,16000.0,44100.0,48000.0,96000.0,192000.0,384000.0}) {
        auto bypass=smooth; bypass[Bypass]=1;
        Leveler dry; dry.prepare(sr,bypass);
        const int delay=dry.latencySamples();
        require(delay==int(std::lround(sr*.005)),"Lookahead latency must follow the actual sample rate");
        auto dryInput=tone<float>(int(sr*.08)+delay,sr,-12);
        dryInput[0]=.25f;
        std::vector<float> dryOutput(dryInput.size());
        dry.process<float>(dryInput.data(),nullptr,dryOutput.data(),nullptr,int(dryInput.size()));
        require(std::all_of(dryOutput.begin(),dryOutput.begin()+delay,[](float x){return x==0;}),
                "Lookahead bypass must begin with exactly the reported delay");
        require(std::equal(dryOutput.begin()+delay,dryOutput.end(),dryInput.begin()),
                "Bypass must preserve exact input samples after its reported delay");

        auto burstInput=alternating(sr);
        auto burstOutput=run(burstInput,smooth,sr,73);
        require(finite(burstOutput) && peak(burstOutput)<=ceiling+1e-7,
                "Adaptive lookahead must protect abrupt speech at every supported sample rate");
        Leveler stereo; stereo.prepare(sr,smooth);
        auto right=burstInput; for (auto& sample:right) sample*=-.35f;
        std::vector<float> leftOut(right.size()),rightOut(right.size());
        stereo.process<float>(burstInput.data(),right.data(),leftOut.data(),rightOut.data(),int(right.size()));
        double ratioError=0;
        for (size_t i=0;i<right.size();++i)
            ratioError=std::max(ratioError,std::abs(double(rightOut[i])+.35*double(leftOut[i])));
        require(ratioError<2e-7,"Lookahead peak protection must preserve linked stereo ratios");

        // Changing settings during a live stream must not poison detector or
        // delay state, including arbitrary user ordering of RMS durations.
        Leveler changing; changing.prepare(sr,smooth);
        auto chunk=tone<float>(int(sr*.04),sr,-9);
        std::vector<float> chunkOut(chunk.size());
        for (int pass=0;pass<12;++pass) {
            auto settings=smooth;
            settings[Lookahead]=normalized(Lookahead,pass%3==0 ? 0.f : (pass%3==1 ? 5.f : 20.f));
            settings[Adaptive]=float(pass%2);
            settings[MedianRange]=float(pass%2);
            for (int parameter=RmsSoftest;parameter<=RmsLoudest;++parameter)
                settings[parameter]=float((pass+parameter)%2);
            changing.configure(settings);
            changing.process<float>(chunk.data(),nullptr,chunkOut.data(),nullptr,int(chunk.size()));
            require(finite(chunkOut) && peak(chunkOut)<=ceiling+1e-7,
                    "Live lookahead and adaptive parameter changes must remain finite and peak-protected");
        }
    }

    // Very soft audible speech should reach its available boost, even though
    // the target is unreachable at that cap. Silence must not add more gain.
    auto whisper=tone<float>(192000,48000,-43);
    const auto whisperOut=run(whisper,smooth);
    const double whisperGain=20*std::log10(rms(whisperOut,144000,191000)/rms(whisper,144000-latency,191000-latency));
    require(std::abs(whisperGain-24)<.1,"Adaptive RMS must retain the requested maximum gain for very soft speech");
    std::cout<<"very_soft_available_gain_db="<<whisperGain<<'\n';
    Leveler pauses; pauses.prepare(48000,smooth);
    std::vector<float> speechOut(whisper.size());
    pauses.process<float>(whisper.data(),nullptr,speechOut.data(),nullptr,int(whisper.size()));
    const double gainBefore=pauses.meters().levelingGain;
    std::vector<float> silence(960),silenceOut(silence.size());
    bool observedInactive=false; double previousInactiveGain=0;
    for (int block=0;block<300;++block) {
        pauses.process<float>(silence.data(),nullptr,silenceOut.data(),nullptr,int(silence.size()));
        const auto meters=pauses.meters();
        require(meters.levelingGain<=gainBefore+.05,"Silence must not build gain above the preceding available boost");
        if (!meters.speech) {
            if (observedInactive)
                require(meters.levelingGain<=previousInactiveGain+.001,"Gain must not grow after the pause hold ends");
            previousInactiveGain=meters.levelingGain; observedInactive=true;
        }
        if (block>0)
            require(std::all_of(silenceOut.begin(),silenceOut.end(),[](float x){return x==0;}),
                    "Silence must be exact after delayed speech drains");
    }
    require(observedInactive && std::abs(pauses.meters().levelingGain)<.3,
            "Adaptive gain must return toward unity after speech ends");
    Leveler silentStart; silentStart.prepare(48000,smooth);
    silentStart.process<float>(silence.data(),nullptr,silenceOut.data(),nullptr,int(silence.size()));
    require(silentStart.meters().levelingGain==0 &&
            std::all_of(silenceOut.begin(),silenceOut.end(),[](float x){return x==0;}),
            "Adaptive lookahead must not boost initial silence");

    std::vector<double> corrupt(4096,0),corruptOut(corrupt.size());
    corrupt[0]=std::numeric_limits<double>::quiet_NaN();
    corrupt[1]=std::numeric_limits<double>::infinity();
    corrupt[2]=-std::numeric_limits<double>::infinity();
    corrupt[3]=1e30; corrupt[4]=-1e30; corrupt[5]=1e-300;
    Leveler repaired; repaired.prepare(48000,smooth);
    repaired.process<double>(corrupt.data(),nullptr,corruptOut.data(),nullptr,int(corrupt.size()));
    require(finite(corruptOut) && peak(corruptOut)<=ceiling+1e-12,
            "Corrupted input must not poison the lookahead buffer or adaptive detectors");

    auto noDelay=smooth; noDelay[Lookahead]=0; noDelay[Bypass]=1;
    auto impulse=std::vector<float>(64,0); impulse[0]=.25f;
    require(run(impulse,noDelay)==impulse,"Zero-lookahead bypass must remain exactly immediate");
    std::cout<<"PASS assertions="<<assertions<<'\n';
}
