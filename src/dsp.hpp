#pragma once
#include "parameters.hpp"
#include "true_peak.hpp"
#include <cstdint>
#include <limits>

namespace sv {
inline double dbGain(double db) noexcept { return std::exp(db * 0.1151292546497022842); }
inline float powerDb(double power) noexcept { return float(10.0 * std::log10(std::max(power, 1e-12))); }
inline float amplitudeDb(double a) noexcept { return float(20.0 * std::log10(std::max(a, 1e-6))); }
struct MeterState {
    float inputRms=-120, outputRms=-120, inputPeak=-120, outputPeak=-120;
    float levelingGain=0, guardReduction=0;
    bool speech=false;
    uint32_t invalidSamples=0;
};

// Feed-forward RMS controller with an optional delayed peak stage. Zero
// lookahead retains the original causal path. Control math runs at approximately
// 500 Hz, independently of host blocks; delay/peak scheduling is per sample.
class Leveler {
public:
    static_assert(TruePeakDetector::Latency==TruePeak4xLatencySamples
                  && TruePeakDetector::Latency==TruePeak8xLatencySamples,
                  "Detector delay and host latency must agree");
    void prepare(double rate, const Values& values) noexcept {
        sr_ = std::clamp(rate, 8000.0, 384000.0);
        quantum_ = std::max(1, int(std::lround(sr_ / 500.0)));
        dt_ = quantum_ / sr_;
        rmsCoeff_ = 1.0 - std::exp(-1.0 / (sr_ * .080));
        fastCoeff_ = 1.0 - std::exp(-1.0 / (sr_ * .012));
        limiterRelease_ = 1.0 - std::exp(-1.0 / (sr_ * .080));
        meterDecay_ = std::exp(-1.0 / (sr_ * .300));
        trimCoeff_ = 1.0 - std::exp(-1.0 / (sr_ * .020));
        bypassStep_ = 1.0 / (sr_ * .020);
        cached_.fill(-1);
        configure(values);
        reset();
    }
    void configure(const Values& values) noexcept {
        if (values == cached_) return;
        cached_ = values;
        target_ = physical(Target,values[Target]);
        maxBoost_ = physical(Boost,values[Boost]);
        maxCut_ = physical(Cut,values[Cut]);
        liftTime_ = physical(Lift,values[Lift]) * .001;
        reduceTime_ = physical(Reduce,values[Reduce]) * .001;
        liftCoeff_ = liftTime_>0 ? 1.0-std::exp(-dt_/liftTime_) : 1.0;
        reduceCoeff_ = reduceTime_>0 ? 1.0-std::exp(-dt_/reduceTime_) : 1.0;
        idleCoeff_ = 1.0 - std::exp(-dt_/std::max(1.0,liftTime_));
        floor_ = physical(Floor,values[Floor]);
        holdTime_ = physical(Hold,values[Hold]) * .001;
        ceiling_ = dbGain(physical(Ceiling,values[Ceiling]));
        strength_ = physical(Strength,values[Strength]) * .01;
        hpCoeff_ = std::exp(-6.283185307179586 * physical(DetectorHPF,values[DetectorHPF])/sr_);
        trimTarget_ = dbGain(physical(Trim,values[Trim]));
        const bool nextGuard=physical(Guard,values[Guard]) > .5f;
        const bool nextAdaptiveGuard=physical(AdaptiveGuard,values[AdaptiveGuard]) > .5f;
        if (nextGuard!=guardEnabled_ || nextAdaptiveGuard!=adaptiveGuard_) {
            resetAdaptiveGuard();
            if (!nextAdaptiveGuard) limiterHold_=std::min(limiterHold_,int(sr_*.010));
        }
        guardEnabled_=nextGuard;
        adaptiveGuard_=nextAdaptiveGuard;
        bypassTarget_ = physical(Bypass,values[Bypass]) > .5f ? 0.0 : 1.0;
        adaptive_ = physical(Adaptive,values[Adaptive]) > .5f;
        medianHalf_ = physical(MedianRange,values[MedianRange])*.5;
        const double upper=target_+medianHalf_;
        loudSpacing_=upper<0 ? std::min(12.0,-upper*.5) : 12.0;
        for (int i=0;i<5;++i) {
            rmsTimes_[i]=physical(RmsSoftest+i,values[RmsSoftest+i]);
            bankCoeff_[i]=1.0-std::exp(-1.0/(sr_*rmsTimes_[i]*.001));
        }
        const int mode=int(physical(TruePeak,values[TruePeak]));
        const int delay=guardLookaheadSamples(sr_,values);
        if (delay!=delaySamples_ || mode!=peakMode_) {
            delaySamples_=std::clamp(delay,0,MaxDelay);
            peakMode_=mode;
            peakDelaySamples_=mode>0 ? TruePeakDetector::Latency : 0;
            peakDetector_.setFactor(mode==1 ? 4 : 8);
            // A finite reconstruction and oversampling grid estimate peaks.
            // This reserve covers the validated detector fixtures; it is not
            // an arbitrary-waveform or downstream encoded-peak guarantee.
            peakReserve_=dbGain(mode==1 ? .9 : .8);
            resetDelay();
        }
    }
    void reset() noexcept {
        hpState_[0]=hpState_[1]=0;
        rmsPower_=fastPower_=rawFastPower_=outputPower_=0;
        gainDb_=0; gain_=nextGain_=1; gainStep_=0; countdown_=0;
        trim_=trimTarget_; wet_=bypassTarget_; limiterGain_=1; limiterCap_=1; limiterHold_=0; limiterActive_=false;
        inputPeak_=outputPeak_=0; activityHold_=0; active_=false; invalid_=0;
        bankPower_.fill(0); rawBankPower_.fill(0);
        selectedPower_=selectedRawPower_=0; selectedMs_=rmsTimes_[2];
        resetDelay();
    }
    template<class T>
    void process(const T* left, const T* right, T* outLeft, T* outRight,
                 int frames, bool accumulating=false) noexcept {
        if (frames <= 0) return;
        // Two-channel fixed topology. A missing channel stays silent; it never
        // causes a 3 dB error in mono level detection. In-place buffers work.
        for (int i=0; i<frames; ++i) {
            const double x0 = clean(left ? double(left[i]) : 0.0);
            const double x1 = clean(right ? double(right[i]) : 0.0);
            hpState_[0] = hpCoeff_*hpState_[0] + (1.0-hpCoeff_)*x0;
            hpState_[1] = hpCoeff_*hpState_[1] + (1.0-hpCoeff_)*x1;
            const double d0 = x0-hpState_[0], d1=x1-hpState_[1];
            const double energy = std::max(d0*d0,d1*d1);
            rmsPower_ += rmsCoeff_*(energy-rmsPower_);
            fastPower_ += fastCoeff_*(energy-fastPower_);
            const double rawEnergy=std::max(x0*x0,x1*x1);
            rawFastPower_ += fastCoeff_*(rawEnergy-rawFastPower_);
            // Keep every envelope warm, even while adaptive mode is disabled.
            // Selecting a different range never starts a new detector at zero.
            for (int b=0;b<5;++b) {
                bankPower_[b]+=bankCoeff_[b]*(energy-bankPower_[b]);
                rawBankPower_[b]+=bankCoeff_[b]*(rawEnergy-rawBankPower_[b]);
            }
            if (countdown_ == 0) updateController();
            --countdown_;
            gain_ += gainStep_;
            trim_ += trimCoeff_*(trimTarget_-trim_);
            double levelGain = gain_*trim_;
            double audio0=x0,audio1=x1;
            const double rawPeak = std::max(std::abs(x0),std::abs(x1));
            double guardRawPeak=rawPeak;
            double processedPeak=rawPeak*levelGain;
            if (peakMode_>0) {
                const double estimate=peakDetector_.process(x0*levelGain,x1*levelGain);
                peakLine_[peakWrite_]={x0,x1,levelGain};
                peakWrite_=(peakWrite_+1)%(peakDelaySamples_+1);
                const auto frame=peakLine_[peakWrite_];
                audio0=frame.left; audio1=frame.right; levelGain=frame.gain;
                guardRawPeak=std::max(std::abs(audio0),std::abs(audio1));
                ++peakClock_;
                processedPeak=guardRawPeak*levelGain;
                if (peakClock_>peakDelaySamples_)
                    processedPeak=std::max(processedPeak,estimate*peakReserve_);
            }
            // Hold an absolute total-gain cap, rather than stale attenuation
            // relative to the leveler. A primary reduction can then take over
            // without being reduced a second time by the guard's old envelope.
            if (delaySamples_>0) {
                // Schedule a finite linear attack from unity toward each known
                // future sample's required guard ratio. The minimum of these
                // lines is maintained with an allocation-free monotonic deque.
                // At the sample's output time its bound equals its exact cap.
                const double ratio=processedPeak>ceiling_ ? ceiling_/processedPeak : 1.0;
                const double bound=forecastBound(ratio);
                delayLine_[delayWrite_]={audio0,audio1,levelGain,processedPeak};
                delayWrite_=(delayWrite_+1)%(delaySamples_+1);
                const auto frame=delayLine_[delayWrite_];
                audio0=frame.left; audio1=frame.right; levelGain=frame.gain;
                if (guardEnabled_ && adaptiveGuard_)
                    trackGuardPeaks(std::max(std::abs(audio0),std::abs(audio1)));
                if (!guardEnabled_ || delayClock_<=delaySamples_+peakDelaySamples_) {
                    // Empty startup frames carry no real gain. Building an
                    // absolute cap from their placeholder gain would retain
                    // spurious attenuation once actual audio emerges.
                    limiterActive_=false; limiterHold_=0;
                } else {
                    const double predicted=levelGain*bound;
                    if (bound<1.0 && (!limiterActive_ || predicted<=limiterCap_*guardRefreshTolerance())) {
                        limiterCap_=limiterActive_ ? std::min(limiterCap_,predicted) : predicted;
                        limiterActive_=true; limiterHold_=guardHoldSamples();
                    } else if (limiterActive_) {
                        // Hold across individual waveform peaks. Releasing at
                        // every zero crossing would modulate a sustained tone.
                        if (limiterHold_>0) --limiterHold_;
                        else if (levelGain<=limiterCap_) limiterActive_=false;
                        else limiterCap_+=limiterRelease_*(levelGain-limiterCap_);
                    }
                    // Also protects a ceiling change before the queued forecast
                    // has been rebuilt, and the few ulps lost in line subtraction.
                    const double headPeak=std::max(std::abs(audio0),std::abs(audio1));
                    const double headGuardPeak=peakMode_>0 ? frame.guardPeak : headPeak*levelGain;
                    if (headGuardPeak>ceiling_) {
                        const double cap=peakMode_>0
                            ? levelGain*(ceiling_/headGuardPeak) : ceiling_/headPeak;
                        if (!limiterActive_ || limiterCap_>cap) {
                            limiterCap_=cap; limiterActive_=true;
                            limiterHold_=guardHoldSamples();
                        }
                    }
                }
            } else if (!guardEnabled_) {
                limiterActive_=false; limiterHold_=0;
            } else {
                if (adaptiveGuard_) trackGuardPeaks(guardRawPeak);
                const bool overload=processedPeak>ceiling_;
                const double required=overload
                    ? (peakMode_>0 ? levelGain*(ceiling_/processedPeak) : ceiling_/rawPeak)
                    : levelGain;
                if (overload && (!limiterActive_ || required<=limiterCap_*guardRefreshTolerance())) {
                    limiterCap_=limiterActive_ ? std::min(limiterCap_,required) : required;
                    limiterActive_=true; limiterHold_=guardHoldSamples();
                } else if (limiterActive_) {
                    if (limiterHold_>0) --limiterHold_;
                    else if (levelGain<=limiterCap_) limiterActive_=false;
                    else limiterCap_+=limiterRelease_*(levelGain-limiterCap_);
                }
                // Same-sample protection also applies during envelope release.
                if (overload && limiterActive_ && limiterCap_>required) {
                    limiterCap_=required; limiterHold_=guardHoldSamples();
                }
            }
            const double protectedGain=guardEnabled_ && limiterActive_
                ? std::min(levelGain,limiterCap_) : levelGain;
            limiterGain_=protectedGain/levelGain; // Actual extra guard reduction for the meter.
            if (wet_ < bypassTarget_) wet_=std::min(bypassTarget_,wet_+bypassStep_);
            else if (wet_ > bypassTarget_) wet_=std::max(bypassTarget_,wet_-bypassStep_);
            const double multiplier = 1.0 + wet_*(protectedGain-1.0);
            double y0=audio0*multiplier, y1=audio1*multiplier;
            // Correct rounding by a few ulps only. This cannot add a delay.
            if (guardEnabled_ && wet_==1.0) {
                y0=std::clamp(y0,-ceiling_,ceiling_);
                y1=std::clamp(y1,-ceiling_,ceiling_);
            }
            if (outLeft) {
                if (accumulating) outLeft[i]+=T(y0); else outLeft[i]=T(y0);
            }
            if (outRight && outRight!=outLeft) {
                if (accumulating) outRight[i]+=T(y1); else outRight[i]=T(y1);
            }
            const double peak=std::max(std::abs(y0),std::abs(y1));
            inputPeak_=std::max(rawPeak,inputPeak_*meterDecay_);
            outputPeak_=std::max(peak,outputPeak_*meterDecay_);
            outputPower_ += rmsCoeff_*(std::max(y0*y0,y1*y1)-outputPower_);
        }
    }
    MeterState meters() const noexcept {
        return {powerDb(rmsPower_), powerDb(outputPower_), amplitudeDb(inputPeak_),
                amplitudeDb(outputPeak_), float(gainDb_), std::max(0.0f,-amplitudeDb(limiterGain_)),
                active_, invalid_};
    }
    double sampleRate() const noexcept { return sr_; }
    int latencySamples() const noexcept { return delaySamples_+peakDelaySamples_; }
    double adaptiveRmsMs() const noexcept { return selectedMs_; }
private:
    static constexpr int MaxDelay=7680; // 20 ms at the maximum supported 384 kHz.
    struct DelayFrame { double left=0,right=0,gain=1,guardPeak=0; };
    struct Forecast { int64_t sample=0; double bound=1; };
    static constexpr int DelayCapacity=MaxDelay+2;
    void resetDelay() noexcept {
        delayLine_.fill(DelayFrame{});
        peakLine_.fill(DelayFrame{});
        peakDetector_.reset(); peakWrite_=0; peakClock_=0;
        delayWrite_=queueHead_=queueTail_=0; delayClock_=0;
        limiterCap_=limiterGain_=1; limiterActive_=false; limiterHold_=0;
        resetAdaptiveGuard();
    }
    int guardHoldSamples() const noexcept {
        return adaptiveGuard_ ? adaptiveHold_ : int(sr_*.010);
    }
    double guardRefreshTolerance() const noexcept {
        // Near-identical recurring peaks should refresh the learned hold even
        // with a little microphone noise. The retained cap can then be at most
        // 0.1 dB stronger than the new peak requires, rather than periodically
        // releasing and making a fresh cut. Legacy mode keeps its exact test.
        return adaptiveGuard_ ? 1.0115794542598986 : 1.0+1e-5;
    }
    void resetAdaptiveGuard() noexcept {
        guardClock_=lastGuardPeak_=guardCrestClock_=0;
        guardCrest_=guardPeakHeight_=guardValley_=guardInterval_=0;
        guardSeekingPeak_=true; guardHasPeak_=false;
        adaptiveHold_=int(sr_*.010);
    }
    void trackGuardPeaks(double peak) noexcept {
        ++guardClock_;
        // Measure meaningful absolute-waveform peaks on the actual delayed
        // input, not on the forecast envelope. A 20 Hz tone has peaks every
        // 25 ms; the legacy 10 ms hold otherwise releases between them.
        // Confirm a crest only after a 20% fall. Starting the next crest also
        // requires a substantial rise. Tiny noise ripples around a bass peak
        // therefore cannot be mistaken for new, closely spaced waveform peaks.
        // A plosive must not leave a stale long hold for the next phrase.
        // History expires after 100 ms without a qualifying peak; held gain
        // retains the normal 80 ms recovery following a hold bounded by 50 ms.
        if (guardHasPeak_ && guardClock_-lastGuardPeak_>sr_*.100) {
            guardHasPeak_=false; guardInterval_=0; adaptiveHold_=int(sr_*.010);
            guardSeekingPeak_=true; guardCrest_=peak; guardCrestClock_=guardClock_;
        }
        if (guardSeekingPeak_) {
            if (peak>guardCrest_) { guardCrest_=peak; guardCrestClock_=guardClock_; }
            if (guardCrest_>1e-9 && peak<guardCrest_*.8) {
                if (guardHasPeak_) {
                    const double interval=double(guardCrestClock_-lastGuardPeak_);
                    if (interval>=sr_*.001 && interval<=sr_*.050) {
                        // Follow a longer recurrence immediately. Relax the
                        // estimate gradually when closely spaced peaks return.
                        guardInterval_=interval>guardInterval_ ? interval :
                            guardInterval_+.2*(interval-guardInterval_);
                        adaptiveHold_=std::clamp(int(std::lround(guardInterval_*1.25)),
                                                int(sr_*.010),int(sr_*.050));
                    } else {
                        guardInterval_=0; adaptiveHold_=int(sr_*.010);
                    }
                }
                guardPeakHeight_=guardCrest_; guardValley_=peak;
                lastGuardPeak_=guardCrestClock_; guardHasPeak_=true;
                guardSeekingPeak_=false;
            }
        } else {
            guardValley_=std::min(guardValley_,peak);
            if (peak>=guardPeakHeight_*.5 && peak>=guardValley_*1.25) {
                guardSeekingPeak_=true; guardCrest_=peak; guardCrestClock_=guardClock_;
            }
        }
    }
    double forecastBound(double ratio) noexcept {
        const int64_t outputSample=delayClock_-delaySamples_;
        while (queueHead_!=queueTail_ && forecast_[queueHead_].sample<outputSample)
            queueHead_=(queueHead_+1)%DelayCapacity;
        const double key=ratio+double(delayClock_)/delaySamples_;
        while (queueHead_!=queueTail_) {
            const int back=(queueTail_+DelayCapacity-1)%DelayCapacity;
            if (forecast_[back].bound<key) break;
            queueTail_=back;
        }
        forecast_[queueTail_]={delayClock_,key};
        queueTail_=(queueTail_+1)%DelayCapacity;
        const double result=std::clamp(forecast_[queueHead_].bound-double(outputSample)/delaySamples_,0.0,1.0);
        ++delayClock_;
        return result;
    }
    void selectDetector() noexcept {
        const double offset=powerDb(rawFastPower_)-target_;
        const double distance=std::max(0.0,std::abs(offset)-medianHalf_);
        // Near full scale, fixed +12/+24 dB loud anchors would be unreachable.
        // Fit the louder two anchors below 0 dBFS when the median edge is below
        // it; an over-full-scale median retains the normal 12 dB spacing.
        const double spacing=offset<0 ? 12.0 : loudSpacing_;
        int a=2,b=2; double mix=0;
        if (distance>0) {
            if (offset<0) { a=distance<spacing ? 2 : 1; b=distance<spacing ? 1 : 0; }
            else { a=distance<spacing ? 2 : 3; b=distance<spacing ? 3 : 4; }
            mix=std::clamp((distance-(distance<spacing ? 0 : spacing))/spacing,0.0,1.0);
            mix=mix*mix*(3.0-2.0*mix); // Zero slope at each range boundary.
        }
        selectedPower_=bankPower_[a]+mix*(bankPower_[b]-bankPower_[a]);
        selectedRawPower_=rawBankPower_[a]+mix*(rawBankPower_[b]-rawBankPower_[a]);
        selectedMs_=std::exp(std::log(rmsTimes_[a])+mix*(std::log(rmsTimes_[b])-std::log(rmsTimes_[a])));
    }
    double clean(double x) noexcept {
        if (!std::isfinite(x)) { ++invalid_; return 0.0; }
        // Corrupted/extreme host buffers cannot overflow the detector. Normal
        // float audio, including deliberate levels above 0 dBFS, passes intact.
        return std::clamp(x,-1e6,1e6);
    }
    void updateController() noexcept {
        countdown_=quantum_;
        gain_=nextGain_; // Avoid cumulative interpolation drift.
        selectDetector();
        const double detectorPower=adaptive_ ? selectedPower_ : rmsPower_;
        const double rmsDb=powerDb(detectorPower);
        const double fastDb=powerDb(fastPower_);
        const bool audible=fastDb >= floor_+3.0;
        if (audible) activityHold_=holdTime_;
        else activityHold_=std::max(0.0,activityHold_-dt_);
        active_=audible || activityHold_>0;
        // Keep the slow, high-passed detector for gentle lifting. If actual
        // full-band audio plus held gain is above target, a separate 12 ms RMS
        // detector can demand a faster reduction. Bass must not hide behind
        // the rumble filter and then receive excessive boost.
        double loudDb=powerDb(adaptive_ ? selectedRawPower_ : rawFastPower_);
        // A new loud onset can arrive while a long quiet-range envelope still
        // reads low. Retain an independent full-band catch for >6 dB rises.
        if (adaptive_ && rawFastPower_>selectedRawPower_*4.0)
            loudDb=std::max(loudDb,double(powerDb(rawFastPower_)));
        double request=(target_-rmsDb)*strength_;
        if (loudDb+gainDb_>target_) request=std::min(request,(target_-loudDb)*strength_);
        double desired=std::clamp(request,-maxCut_,maxBoost_);
        // Prevent the decaying detector from adding gain after speech stops.
        const bool fading = fastPower_ < detectorPower*.16;
        if (!audible || fading) desired=std::min(desired,gainDb_);
        double coefficient=desired<gainDb_ ? reduceCoeff_ : liftCoeff_;
        bool immediate=desired<gainDb_ ? reduceTime_==0 : liftTime_==0;
        if (active_ && desired<gainDb_ && reduceTime_>0) {
            // As held boost carries a loud onset above target, progressively
            // accelerate the cut instead of waiting out the normal time.
            const double overshoot=std::max(0.0,loudDb+gainDb_-target_);
            const double urgency=overshoot/3.0;
            const double catchTime=reduceTime_/(1.0+urgency*urgency);
            coefficient=1.0-std::exp(-dt_/catchTime);
        }
        if (!active_) {
            desired=0;
            coefficient=idleCoeff_;
            immediate=false; // Preserve smooth recovery to unity during pauses.
        }
        gainDb_ += coefficient*(desired-gainDb_);
        nextGain_=dbGain(gainDb_);
        if (immediate) { gain_=nextGain_; gainStep_=0; }
        else gainStep_=(nextGain_-gain_)/quantum_;
    }
    std::array<double,5> bankCoeff_{},bankPower_{},rawBankPower_{},rmsTimes_{{200,120,80,24,8}};
    double selectedPower_=0,selectedRawPower_=0,selectedMs_=80,medianHalf_=3,loudSpacing_=9.5;
    bool adaptive_=false;
    bool adaptiveGuard_=false,guardSeekingPeak_=true,guardHasPeak_=false;
    int adaptiveHold_=480;
    int64_t guardClock_=0,lastGuardPeak_=0,guardCrestClock_=0;
    double guardCrest_=0,guardPeakHeight_=0,guardValley_=0,guardInterval_=0;
    std::array<DelayFrame,DelayCapacity> delayLine_{};
    std::array<DelayFrame,64> peakLine_{};
    TruePeakDetector peakDetector_{};
    int peakMode_=0,peakDelaySamples_=0,peakWrite_=0;
    int64_t peakClock_=0;
    double peakReserve_=1;
    std::array<Forecast,DelayCapacity> forecast_{};
    int delaySamples_=0,delayWrite_=0,queueHead_=0,queueTail_=0;
    int64_t delayClock_=0;
    Values cached_{};
    double sr_=48000, dt_=.002;
    int quantum_=96, countdown_=0, limiterHold_=0;
    double rmsCoeff_=0, fastCoeff_=0, limiterRelease_=0, meterDecay_=0;
    double trimCoeff_=0, bypassStep_=0;
    double target_=-22, maxBoost_=12, maxCut_=24, strength_=.9;
    double floor_=-52, holdTime_=.45, liftTime_=.6, reduceTime_=.1;
    double liftCoeff_=0, reduceCoeff_=0, idleCoeff_=0;
    double ceiling_=.707945784, hpCoeff_=0, trimTarget_=1, bypassTarget_=1;
    bool guardEnabled_=true, active_=false, limiterActive_=false;
    double hpState_[2]{}, rmsPower_=0, fastPower_=0, rawFastPower_=0, outputPower_=0;
    double gainDb_=0, gain_=1, nextGain_=1, gainStep_=0, trim_=1, wet_=1;
    double limiterGain_=1, limiterCap_=1, inputPeak_=0, outputPeak_=0, activityHold_=0;
    uint32_t invalid_=0;
};
}
