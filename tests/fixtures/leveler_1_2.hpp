// SteadyVoice 1.2.0 DSP regression reference; test-only, never linked into the plugin.
#pragma once
#include "../../src/parameters.hpp"
#include <cstdint>
#include <limits>

namespace sv120 {
using namespace sv;
inline double dbGain(double db) noexcept { return std::exp(db * 0.1151292546497022842); }
inline float powerDb(double power) noexcept { return float(10.0 * std::log10(std::max(power, 1e-12))); }
inline float amplitudeDb(double a) noexcept { return float(20.0 * std::log10(std::max(a, 1e-6))); }
struct MeterState {
    float inputRms=-120, outputRms=-120, inputPeak=-120, outputPeak=-120;
    float levelingGain=0, guardReduction=0;
    bool speech=false;
    uint32_t invalidSamples=0;
};

// A causal feed-forward controller. Audio never enters a delay line: detectors
// only control multiplication of the current sample. All expensive control math
// runs at approximately 500 Hz, independently of host block boundaries.
class Leveler {
public:
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
        guardEnabled_ = physical(Guard,values[Guard]) > .5f;
        bypassTarget_ = physical(Bypass,values[Bypass]) > .5f ? 0.0 : 1.0;
    }
    void reset() noexcept {
        hpState_[0]=hpState_[1]=0;
        rmsPower_=fastPower_=outputPower_=0;
        gainDb_=0; gain_=nextGain_=1; gainStep_=0; countdown_=0;
        trim_=trimTarget_; wet_=bypassTarget_; limiterGain_=1; limiterHold_=0;
        inputPeak_=outputPeak_=0; activityHold_=0; active_=false; invalid_=0;
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
            if (countdown_ == 0) updateController();
            --countdown_;
            gain_ += gainStep_;
            trim_ += trimCoeff_*(trimTarget_-trim_);
            const double levelGain = gain_*trim_;
            const double rawPeak = std::max(std::abs(x0),std::abs(x1));
            const double processedPeak = rawPeak*levelGain;
            // Same-sample attack with a 10 ms hold and a smooth 80 ms release.
            // This is a sample-peak guard, not an oversampled true-peak limiter.
            const double required = guardEnabled_ && processedPeak>ceiling_
                ? ceiling_/processedPeak : 1.0;
            if (required<1.0 && required<=limiterGain_*(1.0+1e-5)) {
                limiterGain_ = std::min(limiterGain_,required);
                // Retrigger at the same peak too: otherwise a steady tone can
                // spuriously release and re-limit on alternate waveform peaks.
                limiterHold_ = int(sr_*.010);
            } else if (limiterHold_ > 0) {
                --limiterHold_;
            } else {
                limiterGain_ += limiterRelease_*(1.0-limiterGain_);
            }
            // Release must never overshoot today's required gain.
            if (guardEnabled_ && limiterGain_>required) {
                limiterGain_=required;
                limiterHold_=int(sr_*.010);
            }
            if (wet_ < bypassTarget_) wet_=std::min(bypassTarget_,wet_+bypassStep_);
            else if (wet_ > bypassTarget_) wet_=std::max(bypassTarget_,wet_-bypassStep_);
            const double multiplier = 1.0 + wet_*(levelGain*limiterGain_-1.0);
            double y0=x0*multiplier, y1=x1*multiplier;
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
                amplitudeDb(outputPeak_), float(gainDb_), -amplitudeDb(limiterGain_),
                active_, invalid_};
    }
    double sampleRate() const noexcept { return sr_; }
private:
    double clean(double x) noexcept {
        if (!std::isfinite(x)) { ++invalid_; return 0.0; }
        // Corrupted/extreme host buffers cannot overflow the detector. Normal
        // float audio, including deliberate levels above 0 dBFS, passes intact.
        return std::clamp(x,-1e6,1e6);
    }
    void updateController() noexcept {
        countdown_=quantum_;
        gain_=nextGain_; // Avoid cumulative interpolation drift.
        const double rmsDb=powerDb(rmsPower_);
        const double fastDb=powerDb(fastPower_);
        const bool audible=fastDb >= floor_+3.0;
        if (audible) activityHold_=holdTime_;
        else activityHold_=std::max(0.0,activityHold_-dt_);
        active_=audible || activityHold_>0;
        double desired=std::clamp((target_-rmsDb)*strength_,-maxCut_,maxBoost_);
        // Prevent the decaying detector from adding gain after speech stops.
        const bool fading = fastPower_ < rmsPower_*.16;
        if (!audible || fading) desired=std::min(desired,gainDb_);
        double coefficient=desired<gainDb_ ? reduceCoeff_ : liftCoeff_;
        bool immediate=desired<gainDb_ ? reduceTime_==0 : liftTime_==0;
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
    Values cached_{};
    double sr_=48000, dt_=.002;
    int quantum_=96, countdown_=0, limiterHold_=0;
    double rmsCoeff_=0, fastCoeff_=0, limiterRelease_=0, meterDecay_=0;
    double trimCoeff_=0, bypassStep_=0;
    double target_=-22, maxBoost_=12, maxCut_=24, strength_=.9;
    double floor_=-52, holdTime_=.45, liftTime_=.6, reduceTime_=.1;
    double liftCoeff_=0, reduceCoeff_=0, idleCoeff_=0;
    double ceiling_=.707945784, hpCoeff_=0, trimTarget_=1, bypassTarget_=1;
    bool guardEnabled_=true, active_=false;
    double hpState_[2]{}, rmsPower_=0, fastPower_=0, outputPower_=0;
    double gainDb_=0, gain_=1, nextGain_=1, gainStep_=0, trim_=1, wet_=1;
    double limiterGain_=1, inputPeak_=0, outputPeak_=0, activityHold_=0;
    uint32_t invalid_=0;
};
}
