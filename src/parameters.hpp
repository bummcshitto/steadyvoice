#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>

namespace sv {
enum Param { Target, Boost, Cut, Lift, Reduce, Floor, Hold, Ceiling, Strength,
             DetectorHPF, Trim, Guard, Bypass,
             RmsSoftest, RmsSoft, RmsMedian, RmsLoud, RmsLoudest,
             MedianRange, Lookahead, Adaptive, AdaptiveGuard, TruePeak, ParamCount };
inline constexpr bool isToggle(int index) noexcept {
    return index == Guard || index == Bypass || index == Adaptive
        || index == AdaptiveGuard;
}
inline constexpr bool isRmsTime(int index) noexcept {
    return index >= RmsSoftest && index <= RmsLoudest;
}
struct ParamInfo {
    const char* name;
    const char* shortName;
    const char* unit;
    const char* hint;
    float low, high, initial, step;
    bool logarithmic;
};
inline constexpr std::array<ParamInfo, ParamCount> params {{
    {"Target level", "Target", "dBFS", "Average voice level, not a peak target", -36, 0, -22, 0.5f, false},
    {"Maximum boost", "Boost", "dB", "How far quiet speech can be lifted", 0, 40, 12, 0.5f, false},
    {"Maximum cut", "Cut", "dB", "Reduction capacity; peaks use guard", 0, 40, 24, 0.5f, false},
    {"Lift time", "Lift", "ms", "0 ms = no gain-increase smoothing", 0, 4000, 600, 25, true},
    {"Reduce time", "Reduce", "ms", "0 ms: immediate; loud catch adapts", 0, 1000, 100, 5, true},
    {"Noise floor", "Floor", "dBFS", "Set above room noise, below soft speech", -80, -24, -52, 1, false},
    {"Pause hold", "Hold", "ms", "Holds gain briefly between phrases", 0, 2000, 450, 25, false},
    {"Peak ceiling", "Ceiling", "dBFS", "Peak protection when guard is on", -12, 0, -3, 0.2f, false},
    {"Leveling strength", "Amount", "%", "Lower = more natural volume variation", 0, 100, 90, 1, false},
    {"Detector high-pass", "Det HPF", "Hz", "Rumble filter for slow voice detector", 20, 250, 80, 5, true},
    {"Output trim", "Trim", "dB", "Applied before the peak guard", -12, 12, 0, 0.5f, false},
    {"Peak guard", "Guard", "", "Stereo-linked peak protection", 0, 1, 1, 1, false},
    {"Bypass", "Bypass", "", "Smooth transition to unprocessed audio", 0, 1, 0, 1, false},
    {"Softest RMS", "Softest", "ms", "24 dB below the target band", 1, 1000, 200, 1, true},
    {"Soft RMS", "SoftRMS", "ms", "12 dB below the target band", 1, 1000, 120, 1, true},
    {"Median RMS", "Mid RMS", "ms", "Within the target-centered band", 1, 1000, 80, 1, true},
    {"Loud RMS", "LoudRMS", "ms", "Upper input levels; see ranges below", 1, 1000, 24, 1, true},
    {"Loudest RMS", "Loudest", "ms", "Highest input levels; see ranges below", 1, 1000, 8, 1, true},
    {"Median range", "MidBand", "dB", "Full width of the target-centered band", 0, 24, 6, 0.5f, false},
    {"Lookahead", "Lookahd", "ms", "Audio delay for smoother peak protection", 0, 20, 0, 0.1f, false},
    {"Adaptive RMS", "Adapt", "", "Use five loudness-dependent RMS detectors", 0, 1, 0, 1, false},
    {"Adaptive guard", "AdGuard", "", "Signal-aware guard hold and recovery", 0, 1, 0, 1, false},
    {"Peak detection", "PkMode", "", "Sample peaks / 4x / 8x oversampling", 0, 2, 0, 1, false}
}};
inline float physical(int index, float normalized) noexcept {
    const auto& p = params[index];
    const float v = std::clamp(normalized, 0.0f, 1.0f);
    if (isToggle(index)) return v >= 0.5f ? 1.0f : 0.0f;
    if (index==TruePeak) return std::round(v*2.0f);
    if (p.logarithmic && p.low==0) {
        if (v==0) return 0;
        if (v==1) return p.high;
        // A 1 ms offset retains fine control near zero without log(0).
        return std::expm1(v*std::log1p(p.high));
    }
    return p.logarithmic ? p.low * std::pow(p.high / p.low, v) : p.low + v * (p.high - p.low);
}
inline float normalized(int index, float value) noexcept {
    const auto& p = params[index];
    value = std::clamp(value, p.low, p.high);
    if (isToggle(index)) return value >= 0.5f ? 1.0f : 0.0f;
    if (index==TruePeak) return std::round(value)*.5f;
    if (p.logarithmic && p.low==0) return std::log1p(value)/std::log1p(p.high);
    return p.logarithmic ? std::log(value / p.low) / std::log(p.high / p.low) : (value - p.low) / (p.high - p.low);
}
using Values = std::array<float, ParamCount>;
inline constexpr int TruePeak4xLatencySamples=63,TruePeak8xLatencySamples=63;
inline constexpr float TruePeakMinimumLookaheadMs=5.0f;
inline int guardLookaheadSamples(double sampleRate,const Values& values) noexcept {
    const float requested=physical(Lookahead,values[Lookahead]);
    const float effective=physical(TruePeak,values[TruePeak])>0
        ? std::max(requested,TruePeakMinimumLookaheadMs) : requested;
    return int(std::lround(sampleRate*effective*.001));
}
inline int processingLatencySamples(double sampleRate,const Values& values) noexcept {
    const int mode=int(physical(TruePeak,values[TruePeak]));
    return guardLookaheadSamples(sampleRate,values)
        + (mode==1 ? TruePeak4xLatencySamples : (mode==2 ? TruePeak8xLatencySamples : 0));
}
inline Values defaults() noexcept {
    Values a{};
    for (int i=0; i<ParamCount; ++i) a[i] = normalized(i, params[i].initial);
    return a;
}
inline constexpr int PresetCount = 6;
inline constexpr const char* presetNames[PresetCount] = {"Broadcast", "Natural", "Wide range", "Singing", "Smooth boost", "Quality voice"};
inline Values preset(int index) noexcept {
    Values a = defaults();
    auto put = [&a](int i, float v) { a[i] = normalized(i, v); };
    if (index == 1) { put(Target,-24); put(Boost,8); put(Cut,18); put(Lift,1200); put(Reduce,220); put(Strength,70); }
    if (index == 2) { put(Target,-22); put(Boost,18); put(Cut,30); put(Lift,450); put(Reduce,60); put(Hold,600); put(Strength,95); }
    if (index == 3) { put(Target,-24); put(Boost,10); put(Cut,24); put(Lift,1400); put(Reduce,160); put(Hold,900); put(Strength,70); }
    if (index == 4 || index == 5) {
        put(Target,-16); put(Boost,24); put(Cut,40); put(Lift,150); put(Reduce,20);
        put(Floor,-50); put(Hold,600); put(Ceiling,-1); put(Strength,100);
        put(DetectorHPF,40); put(Trim,0); put(Guard,1); put(Adaptive,1); put(Lookahead,5);
    }
    if (index == 5) { put(AdaptiveGuard,1); put(TruePeak,2); }
    return a;
}
inline void display(int i, float v, char* destination, size_t capacity, bool withUnit=true) noexcept {
    if (isToggle(i)) { std::snprintf(destination, capacity, "%s", physical(i,v) > 0 ? "On" : "Off"); return; }
    if (i==TruePeak) {
        constexpr const char* modes[]={"Sample","4x","8x"};
        std::snprintf(destination,capacity,"%s",modes[int(physical(i,v))]); return;
    }
    float x = physical(i,v);
    if ((i==Lift || i==Reduce || i==Lookahead || isRmsTime(i)) && x>0 && x<10) {
        // Positive sub-millisecond times must never be rounded to "0 ms".
        // Both formats also fit the host's seven-character display payload.
        std::snprintf(destination,capacity,x<.001f ? "%.0e%s%s" : "%.3g%s%s",double(x),
                      withUnit ? " " : "",withUnit ? params[i].unit : "");
        return;
    }
    if (i==Lookahead) {
        std::snprintf(destination,capacity,"%.2f%s%s",double(x),withUnit ? " " : "",withUnit ? params[i].unit : "");
        return;
    }
    if (isRmsTime(i)) {
        std::snprintf(destination,capacity,"%.4g%s%s",double(x),withUnit ? " " : "",withUnit ? params[i].unit : "");
        return;
    }
    const bool decimal = (i==Target || i==Boost || i==Cut || i==Ceiling || i==Trim || i==MedianRange);
    std::snprintf(destination, capacity, decimal ? "%.1f%s%s" : "%.0f%s%s", double(x),
                  withUnit ? " " : "", withUnit ? params[i].unit : "");
}
}
