#include "plugin.hpp"
#include "editor.hpp"
#include <cstring>
#include <cstdlib>
#include <new>
#if defined(__SSE2__) || defined(_M_X64)
#include <xmmintrin.h>
#endif

namespace sv {
static_assert(std::atomic<float>::is_always_lock_free, "Audio parameters must be lock-free");
static_assert(std::atomic<uint32_t>::is_always_lock_free, "Audio meters must be lock-free");
static intptr_t VST_FUNCTION_INTERFACE dispatcher(vst_effect_t* e,int32_t op,int32_t i,intptr_t v,void* p,float f) {
    if (!e || !e->effect_internal) return 0;
    auto* plugin=static_cast<Plugin*>(e->effect_internal);
    if (op==VST_EFFECT_OPCODE_DESTROY) { e->effect_internal=nullptr; delete plugin; return 1; }
    return plugin->dispatch(op,i,v,p,f);
}
static void VST_FUNCTION_INTERFACE set(vst_effect_t* e,uint32_t i,float value) {
    if (e && e->effect_internal) static_cast<Plugin*>(e->effect_internal)->setParameter(int(i),value);
}
static float VST_FUNCTION_INTERFACE get(vst_effect_t* e,uint32_t i) {
    return e && e->effect_internal ? static_cast<Plugin*>(e->effect_internal)->parameter(int(i)) : 0.0f;
}
static void VST_FUNCTION_INTERFACE replacing(vst_effect_t* e,const float* const* in,float** out,int32_t n) {
    if (e && e->effect_internal) static_cast<Plugin*>(e->effect_internal)->render(in,out,n);
}
static void VST_FUNCTION_INTERFACE adding(vst_effect_t* e,const float* const* in,float** out,int32_t n) {
    if (e && e->effect_internal) static_cast<Plugin*>(e->effect_internal)->render(in,out,n,true);
}
static void VST_FUNCTION_INTERFACE doubles(vst_effect_t* e,const double* const* in,double** out,int32_t n) {
    if (e && e->effect_internal) static_cast<Plugin*>(e->effect_internal)->render(in,out,n);
}
static intptr_t copyText(void* destination,size_t size,const char* text) noexcept {
    if (!destination) return 0;
    const size_t length=std::min(size-1,std::strlen(text));
    std::memcpy(destination,text,length);
    static_cast<char*>(destination)[length]='\0';
    return 1;
}
Plugin::Plugin(vst_host_callback_t callback) noexcept : host(callback) {
    const Values v=defaults();
    for (int i=0;i<ParamCount;++i) parameters_[i].store(v[i],std::memory_order_relaxed);
    dsp_.prepare(48000,v);
    effect.magic_number=VST_FOURCC('V','s','t','P');
    effect.control=dispatcher; effect.process=adding; effect.set_parameter=set; effect.get_parameter=get;
    effect.num_programs=PresetCount; effect.num_params=ParamCount; effect.num_inputs=2; effect.num_outputs=2;
    effect.flags=VST_EFFECT_FLAG_CHUNKS|VST_EFFECT_FLAG_SUPPORTS_FLOAT|VST_EFFECT_FLAG_SUPPORTS_DOUBLE;
#ifdef _WIN32
    effect.flags|=VST_EFFECT_FLAG_EDITOR;
#endif
    effect.delay=dsp_.latencySamples(); reportedLatency_.store(effect.delay,std::memory_order_relaxed);
    effect.input_output_ratio=1; effect.effect_internal=this;
    effect.unique_id=VST_FOURCC('S','v','L','1'); effect.version=10500;
    effect.process_float=replacing; effect.process_double=doubles;
    arrangement_.type=VST_SPEAKER_ARRANGEMENT_TYPE_STEREO; arrangement_.channels=2;
    arrangement_.speakers[0].type=VST_SPEAKER_TYPE_LEFT;
    arrangement_.speakers[1].type=VST_SPEAKER_TYPE_RIGHT;
    std::snprintf(arrangement_.speakers[0].name,64,"Left");
    std::snprintf(arrangement_.speakers[1].name,64,"Right");
    for (int i=0;i<4;++i) meters_[i].store(-120);
}
Plugin::~Plugin() { delete editor_; }
float Plugin::parameter(int i) const noexcept {
    return i>=0 && i<ParamCount ? parameters_[i].load(std::memory_order_relaxed) : 0;
}
void Plugin::setParameter(int i,float v) noexcept {
    if (i<0 || i>=ParamCount || !std::isfinite(v)) return;
    v=std::clamp(v,0.0f,1.0f);
    if (isToggle(i)) v=v>=.5f ? 1.0f : 0.0f;
    if (i==TruePeak) v=normalized(i,physical(i,v));
    parameters_[i].store(v,std::memory_order_relaxed);
    if (i==Lookahead || i==TruePeak) updateLatency();
}
void Plugin::updateLatency() noexcept {
    // Control-side reporting uses the same sample rounding as the DSP, without
    // touching its audio-thread-owned state or calling the host from render.
    Values values{};
    values[Lookahead]=parameter(Lookahead); values[TruePeak]=parameter(TruePeak);
    const int latency=processingLatencySamples(rate_.load(std::memory_order_relaxed),values);
    if (effect.delay==latency) return;
    effect.delay=latency;
    reportedLatency_.store(latency,std::memory_order_relaxed);
    if (host) host(&effect,VST_HOST_OPCODE_IO_MODIFIED,0,0,nullptr,0);
}
void Plugin::beginEdit(int i) noexcept {
    if (host) host(&effect,VST_HOST_OPCODE_PARAM_START_EDIT,i,0,nullptr,0);
}
void Plugin::endEdit(int i) noexcept {
    if (host) host(&effect,VST_HOST_OPCODE_PARAM_STOP_EDIT,i,0,nullptr,0);
}
void Plugin::editParameter(int i,float value) noexcept {
    setParameter(i,value);
    if (host) host(&effect,VST_HOST_OPCODE_PARAM_UPDATE,i,0,nullptr,parameter(i));
}
void Plugin::choosePreset(int i,bool notify) noexcept {
    if (i<0 || i>=PresetCount) return;
    const Values values=preset(i);
    program_.store(i,std::memory_order_relaxed);
    for (int p=0;p<ParamCount;++p) {
        if (notify) { beginEdit(p); editParameter(p,values[p]); endEdit(p); }
        else setParameter(p,values[p]);
    }
    if (notify && host) host(&effect,VST_HOST_OPCODE_EDITOR_UPDATE,0,0,nullptr,0);
}
MeterState Plugin::meterSnapshot() const noexcept {
    return {meters_[0].load(std::memory_order_relaxed),meters_[1].load(std::memory_order_relaxed),
            meters_[2].load(std::memory_order_relaxed),meters_[3].load(std::memory_order_relaxed),
            meters_[4].load(std::memory_order_relaxed),meters_[5].load(std::memory_order_relaxed),
            active_.load(std::memory_order_relaxed),invalid_.load(std::memory_order_relaxed)};
}
template<class T> void Plugin::render(const T* const* input,T** output,int frames,bool accumulating) noexcept {
    if (frames<=0 || !output) return;
#if defined(__SSE2__) || defined(_M_X64)
    const unsigned savedCSR=_mm_getcsr();
    _mm_setcsr(savedCSR|0x8040u); // Flush denormals; restore the host's mode below.
#endif
    Values v{};
    for (int i=0;i<ParamCount;++i) v[i]=parameters_[i].load(std::memory_order_relaxed);
    const double rate=rate_.load(std::memory_order_relaxed);
    if (rate!=dsp_.sampleRate()) dsp_.prepare(rate,v);
    else dsp_.configure(v);
    if (resetRequested_.exchange(false,std::memory_order_relaxed)) dsp_.reset();
    dsp_.process(input?input[0]:nullptr,input?input[1]:nullptr,output[0],output[1],frames,accumulating);
    const MeterState m=dsp_.meters();
    meters_[0].store(m.inputRms,std::memory_order_relaxed); meters_[1].store(m.outputRms,std::memory_order_relaxed);
    meters_[2].store(m.inputPeak,std::memory_order_relaxed); meters_[3].store(m.outputPeak,std::memory_order_relaxed);
    meters_[4].store(m.levelingGain,std::memory_order_relaxed); meters_[5].store(m.guardReduction,std::memory_order_relaxed);
    active_.store(m.speech,std::memory_order_relaxed); invalid_.store(m.invalidSamples,std::memory_order_relaxed);
#if defined(__SSE2__) || defined(_M_X64)
    _mm_setcsr(savedCSR);
#endif
}
template void Plugin::render<float>(const float* const*,float**,int,bool) noexcept;
template void Plugin::render<double>(const double* const*,double**,int,bool) noexcept;

intptr_t Plugin::dispatch(int op,int index,intptr_t value,void* ptr,float opt) {
    switch (op) {
    case VST_EFFECT_OPCODE_CREATE: return 1;
    case VST_EFFECT_OPCODE_PROGRAM_SET:
        if (value<0 || value>=PresetCount) return 0;
        choosePreset(int(value),false); return 1;
    case VST_EFFECT_OPCODE_PROGRAM_GET: return currentPreset();
    case VST_EFFECT_OPCODE_PROGRAM_GET_NAME: return copyText(ptr,24,presetNames[currentPreset()]);
    case 29: return index>=0 && index<PresetCount ? copyText(ptr,24,presetNames[index]) : 0; // effGetProgramNameIndexed
    case VST_EFFECT_OPCODE_PARAM_LABEL:
        return index>=0 && index<ParamCount ? copyText(ptr,8,params[index].unit) : 0;
    case VST_EFFECT_OPCODE_PARAM_NAME:
        return index>=0 && index<ParamCount ? copyText(ptr,8,params[index].shortName) : 0;
    case VST_EFFECT_OPCODE_PARAM_VALUE: {
        if (!ptr || index<0 || index>=ParamCount) return 0;
        char buffer[64]; display(index,parameter(index),buffer,sizeof(buffer),false);
        return copyText(ptr,8,buffer);
    }
    case VST_EFFECT_OPCODE_SET_SAMPLE_RATE:
        if (std::isfinite(opt) && opt>=8000 && opt<=384000) {
            rate_.store(opt,std::memory_order_relaxed); updateLatency(); return 1;
        }
        return 0;
    case VST_EFFECT_OPCODE_SET_BLOCK_SIZE: return value>0 ? 1 : 0;
    case VST_EFFECT_OPCODE_SUSPEND_RESUME:
        if (value) resetRequested_.store(true,std::memory_order_relaxed);
        updateLatency(); return 1;
    case VST_EFFECT_OPCODE_EDITOR_GET_RECT:
        if (!ptr) return 0;
        *static_cast<vst_rect_t**>(ptr)=&rect_; return 1;
    case VST_EFFECT_OPCODE_EDITOR_OPEN:
        if (!ptr) return 0;
        if (!editor_) editor_=new(std::nothrow) Editor(*this);
        return editor_ && editor_->open(ptr);
    case VST_EFFECT_OPCODE_EDITOR_CLOSE:
        if (editor_) editor_->close();
        return 1;
    case VST_EFFECT_OPCODE_EDITOR_KEEP_ALIVE:
        if (editor_) editor_->refresh();
        return 1;
    case VST_EFFECT_OPCODE_GET_CHUNK_DATA:
        if (!ptr) return 0;
        chunk_.magic=VST_FOURCC('S','V','0','1'); chunk_.version=5; chunk_.count=ParamCount; chunk_.program=uint32_t(currentPreset());
        for (int i=0;i<ParamCount;++i) chunk_.values[i]=parameter(i);
        *static_cast<void**>(ptr)=&chunk_; return sizeof(chunk_);
    case VST_EFFECT_OPCODE_SET_CHUNK_DATA: {
        constexpr uint32_t legacyCount=13;
        struct Header { uint32_t magic,version,count,program; } header{};
        if (!ptr || value<intptr_t(sizeof(Header))) return 0;
        std::memcpy(&header,ptr,sizeof(header));
        if (header.magic!=VST_FOURCC('S','V','0','1') || header.version<1 || header.version>5) return 0;
        const bool legacy=header.version<=3;
        const uint32_t expectedCount=legacy ? legacyCount : (header.version==4 ? 21u : uint32_t(ParamCount));
        const uint32_t expectedPrograms=legacy ? 4u : (header.version==4 ? 5u : uint32_t(PresetCount));
        if (header.count!=expectedCount || header.program>=expectedPrograms
            || value!=intptr_t(sizeof(Header)+expectedCount*sizeof(float))) return 0;
        Values restored=defaults();
        std::memcpy(restored.data(),static_cast<const unsigned char*>(ptr)+sizeof(Header),expectedCount*sizeof(float));
        for (float v:restored) if (!std::isfinite(v) || v<0 || v>1) return 0;
        if (header.version==1) {
            // V1 chunks use the old normalized ranges. Migrate through their
            // physical dB values before publishing any of the restored state.
            restored[Target]=normalized(Target,-36.0f+24.0f*restored[Target]);
            restored[Boost]=normalized(Boost,24.0f*restored[Boost]);
            restored[Ceiling]=normalized(Ceiling,-12.0f+11.8f*restored[Ceiling]);
        }
        if (header.version<=2) {
            restored[Cut]=normalized(Cut,36.0f*restored[Cut]);
            restored[Lift]=normalized(Lift,100.0f*std::pow(40.0f,restored[Lift]));
            restored[Reduce]=normalized(Reduce,10.0f*std::pow(100.0f,restored[Reduce]));
        }
        program_.store(int(header.program),std::memory_order_relaxed);
        for (int i=0;i<ParamCount;++i) setParameter(i,restored[i]);
        return 1;
    }
    case VST_EFFECT_OPCODE_PARAM_IS_AUTOMATABLE: return index>=0 && index<ParamCount;
    case VST_EFFECT_OPCODE_PARAM_VALUE_FROM_STRING: {
        if (!ptr || index<0 || index>=ParamCount) return 0;
        if (index==TruePeak) {
            const auto* text=static_cast<const char*>(ptr);
            if (!std::strcmp(text,"Sample")) { setParameter(index,0); return 1; }
            if (!std::strcmp(text,"4x")) { setParameter(index,.5f); return 1; }
            if (!std::strcmp(text,"8x")) { setParameter(index,1); return 1; }
        }
        char* end=nullptr;
        const float f=std::strtof(static_cast<const char*>(ptr),&end);
        if (end==ptr || !std::isfinite(f)) return 0;
        setParameter(index,normalized(index,f)); return 1;
    }
    case VST_EFFECT_OPCODE_INPUT_GET_PROPERTIES:
    case VST_EFFECT_OPCODE_OUTPUT_GET_PROPERTIES: {
        if (!ptr || index<0 || index>=2) return 0;
        auto* p=static_cast<vst_stream_properties_t*>(ptr); std::memset(p,0,sizeof(*p));
        std::snprintf(p->name,sizeof(p->name),"SteadyVoice %s",index==0?"Left":"Right");
        std::snprintf(p->label,sizeof(p->label),"%s",index==0?"Left":"Right");
        p->flags=1|(index==0?VST_STREAM_FLAG_STEREO:0); p->type=VST_SPEAKER_ARRANGEMENT_TYPE_STEREO;
        return 1;
    }
    case VST_EFFECT_OPCODE_SET_SPEAKER_ARRANGEMENT: {
        // Fixed linked stereo topology; hosts may pass a silent second channel.
        auto* in=reinterpret_cast<vst_speaker_arrangement_t*>(value);
        auto* out=static_cast<vst_speaker_arrangement_t*>(ptr);
        return in && out && in->channels==2 && out->channels==2;
    }
    case VST_EFFECT_OPCODE_GET_SPEAKER_ARRANGEMENT:
        if (value) *reinterpret_cast<vst_speaker_arrangement_t**>(value)=&arrangement_;
        if (ptr) *static_cast<vst_speaker_arrangement_t**>(ptr)=&arrangement_;
        return 1;
    case VST_EFFECT_OPCODE_CATEGORY: return VST_EFFECT_CATEGORY_EFFECT;
    case VST_EFFECT_OPCODE_BYPASS: setParameter(Bypass,value?1.0f:0.0f); return 1;
    case VST_EFFECT_OPCODE_EFFECT_NAME: return copyText(ptr,32,"SteadyVoice");
    case VST_EFFECT_OPCODE_VENDOR_NAME: return copyText(ptr,64,"SteadyVoice");
    case VST_EFFECT_OPCODE_PRODUCT_NAME: return copyText(ptr,64,"SteadyVoice Leveler");
    case VST_EFFECT_OPCODE_VENDOR_VERSION: return 10500;
    case VST_EFFECT_OPCODE_SUPPORTS:
        if (!ptr) return 0;
        if (!std::strcmp(static_cast<char*>(ptr),"plugAsChannelInsert") || !std::strcmp(static_cast<char*>(ptr),"plugAsSend") || !std::strcmp(static_cast<char*>(ptr),"bypass")) return 1;
        if (!std::strcmp(static_cast<char*>(ptr),"receiveVstEvents") || !std::strcmp(static_cast<char*>(ptr),"receiveVstMidiEvent")) return -1;
        return 0;
    case VST_EFFECT_OPCODE_TAIL_SAMPLES:
        // VST reserves 1 for "no tail", so a one-sample delay needs 2 here.
        return effect.delay>0 ? std::max(2,effect.delay) : 1;
    case VST_EFFECT_OPCODE_PARAM_PROPERTIES: {
        if (!ptr || index<0 || index>=ParamCount) return 0;
        auto* p=static_cast<vst_parameter_properties_t*>(ptr); std::memset(p,0,sizeof(*p));
        std::snprintf(p->name,sizeof(p->name),"%s",params[index].name);
        std::snprintf(p->label,sizeof(p->label),"%s",params[index].shortName);
        p->flags=isToggle(index)?VST_PARAMETER_FLAG_SWITCH:VST_PARAMETER_FLAG_STEP_FLOAT;
        p->step_f32=.005f; p->step_small_f32=.001f; p->step_large_f32=.05f;
        if (index==TruePeak) {
            p->flags=VST_PARAMETER_FLAG_INTEGER_LIMITS|VST_PARAMETER_FLAG_STEP_INT|VST_PARAMETER_FLAG_STEP_FLOAT;
            p->min_value_i32=0; p->max_value_i32=2; p->step_i32=1;
            p->step_f32=p->step_small_f32=p->step_large_f32=.5f;
        }
        p->index=uint16_t(index); return 1;
    }
    case VST_EFFECT_OPCODE_VST_VERSION: return 2400;
    case VST_EFFECT_OPCODE_PROCESS_BEGIN: return 1;
    case VST_EFFECT_OPCODE_PROCESS_END: return 1;
    case 77: return value==0 || value==1; // effSetProcessPrecision: 32- or 64-bit.
    default: return 0;
    }
}
}

#ifdef _WIN32
#define SV_EXPORT __declspec(dllexport)
#else
#define SV_EXPORT __attribute__((visibility("default")))
#endif
extern "C" SV_EXPORT vst_effect_t* VST_FUNCTION_INTERFACE VSTPluginMain(vst_host_callback_t host) {
    if (!host || host(nullptr,VST_HOST_OPCODE_VST_VERSION,0,0,nullptr,0)==0) return nullptr;
    auto* plugin=new(std::nothrow) sv::Plugin(host);
    return plugin ? &plugin->effect : nullptr;
}
