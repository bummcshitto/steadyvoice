#include "../src/abi.hpp"
#include "../src/parameters.hpp"
#include <vector>
#include <array>
#include <iostream>
#include <atomic>
#include <thread>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <limits>
#include <new>
#include <utility>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#include <xmmintrin.h>
#endif

using namespace sv;
std::atomic<int> updates{0},begins{0},ends{0};
std::atomic<int> ioChanges{0},notifiedDelay{-1},renderHostCalls{0};
thread_local bool insideAudioCallback=false;
std::atomic<bool> trackAllocations{false};
std::atomic<size_t> allocations{0};
#ifndef _WIN32
void* operator new(size_t n) {
    if(trackAllocations.load(std::memory_order_relaxed)) allocations.fetch_add(1,std::memory_order_relaxed);
    if(void* p=std::malloc(n?n:1)) return p;
    throw std::bad_alloc();
}
void* operator new[](size_t n) { return ::operator new(n); }
// Cover both allocation forms so sanitizer runs use one malloc/free family.
void* operator new(size_t n,const std::nothrow_t&) noexcept {
    try { return ::operator new(n); } catch(...) { return nullptr; }
}
void* operator new[](size_t n,const std::nothrow_t& tag) noexcept { return ::operator new(n,tag); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p,size_t) noexcept { std::free(p); }
void operator delete[](void* p,size_t) noexcept { std::free(p); }
void operator delete(void* p,const std::nothrow_t&) noexcept { std::free(p); }
void operator delete[](void* p,const std::nothrow_t&) noexcept { std::free(p); }
#endif
int assertions=0;
void require(bool pass,const char* reason) {
    ++assertions;
    if(!pass) { std::cerr<<"FAIL: "<<reason<<"\n"; std::exit(1); }
}
intptr_t VST_FUNCTION_INTERFACE callback(vst_effect_t* effect,int32_t opcode,int32_t,int64_t,const char*,float) {
    if(insideAudioCallback) ++renderHostCalls;
    switch(opcode) {
        case VST_HOST_OPCODE_VST_VERSION: return 2400;
        case VST_HOST_OPCODE_GET_SAMPLE_RATE: return 48000;
        case VST_HOST_OPCODE_GET_BLOCK_SIZE: return 128;
        case VST_HOST_OPCODE_PARAM_UPDATE: ++updates; return 1;
        case VST_HOST_OPCODE_PARAM_START_EDIT: ++begins; return 1;
        case VST_HOST_OPCODE_PARAM_STOP_EDIT: ++ends; return 1;
        case VST_HOST_OPCODE_EDITOR_UPDATE: return 1;
        case VST_HOST_OPCODE_IO_MODIFIED: ++ioChanges; notifiedDelay=effect?effect->delay:-1; return 1;
        default: return 0;
    }
}
using Entry=vst_effect_t* (VST_FUNCTION_INTERFACE*)(vst_host_callback_t);
intptr_t dispatch(vst_effect_t* e,int op,int index=0,intptr_t value=0,void* ptr=nullptr,float option=0) {
    return e->control(e,op,index,value,ptr,option);
}
void reset(vst_effect_t* e,float rate=48000) {
    dispatch(e,VST_EFFECT_OPCODE_SET_SAMPLE_RATE,0,0,nullptr,rate);
    dispatch(e,VST_EFFECT_OPCODE_SUSPEND_RESUME,0,1);
}
void preset(vst_effect_t* e,int i=0) { dispatch(e,VST_EFFECT_OPCODE_PROGRAM_SET,0,i); reset(e); }
void pump() {
#ifdef _WIN32
    MSG message;
    while(PeekMessageA(&message,nullptr,0,0,PM_REMOVE)) { TranslateMessage(&message); DispatchMessageA(&message); }
#endif
}
#ifdef _WIN32
bool saveBitmap(HWND window,const char* filename) {
    HDC source=GetDC(window), dc=CreateCompatibleDC(source);
    HBITMAP bitmap=CreateCompatibleBitmap(source,900,720);
    auto old=SelectObject(dc,bitmap); BitBlt(dc,0,0,900,720,source,0,0,SRCCOPY);
    BITMAPINFO info{}; info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth=900; info.bmiHeader.biHeight=-720;
    info.bmiHeader.biPlanes=1; info.bmiHeader.biBitCount=32; info.bmiHeader.biCompression=BI_RGB;
    std::vector<unsigned char> pixels(900*720*4);
    SelectObject(dc,old);
    const bool read=GetDIBits(source,bitmap,0,720,pixels.data(),&info,DIB_RGB_COLORS)!=0;
    BITMAPFILEHEADER header{}; header.bfType=0x4d42;
    header.bfOffBits=sizeof(header)+sizeof(BITMAPINFOHEADER); header.bfSize=header.bfOffBits+DWORD(pixels.size());
    FILE* file=std::fopen(filename,"wb");
    bool saved=false;
    if(file) {
        saved=std::fwrite(&header,sizeof(header),1,file)==1 && std::fwrite(&info.bmiHeader,sizeof(info.bmiHeader),1,file)==1
               && std::fwrite(pixels.data(),pixels.size(),1,file)==1;
        std::fclose(file);
    }
    DeleteObject(bitmap); DeleteDC(dc); ReleaseDC(window,source);
    return read&&saved;
}
#endif
int main(int argc,char** argv) {
    require(argc>1,"Pass a path to the built plugin");
#ifdef _WIN32
    HMODULE module=LoadLibraryA(argv[1]);
    if(!module) std::cerr<<"LoadLibrary error="<<GetLastError()<<"\n";
    require(module!=nullptr,"The Windows DLL must load without extra runtimes");
    Entry entry=nullptr;
    const FARPROC symbol=GetProcAddress(module,"VSTPluginMain");
    static_assert(sizeof(entry)==sizeof(symbol));
    std::memcpy(&entry,&symbol,sizeof(entry));
#else
    void* module=dlopen(argv[1],RTLD_NOW|RTLD_LOCAL);
    if(!module) std::cerr<<dlerror()<<"\n";
    require(module!=nullptr,"The plugin must load");
    Entry entry=nullptr; void* symbol=dlsym(module,"VSTPluginMain"); std::memcpy(&entry,&symbol,sizeof(entry));
#endif
    require(entry!=nullptr,"VSTPluginMain must be exported");
    require(entry(nullptr)==nullptr,"An invalid host must be rejected");
    vst_effect_t* e=entry(callback);
    vst_effect_t* other=entry(callback);
    require(e&&other&&e!=other,"Multiple independent instances must load");
    require(e->magic_number==int32_t(VST_FOURCC('V','s','t','P')),"Correct AEffect magic");
    require(e->num_inputs==2&&e->num_outputs==2,"Two-channel host-compatible topology");
    require(e->delay==0,"The host must see zero latency by default");
    require(e->num_params==23 && e->num_programs==PresetCount && PresetCount==6,"The quality upgrade must append two parameters and one preset");
    require(dispatch(e,VST_EFFECT_OPCODE_VST_VERSION)==2400,"VST 2.4 interface");
    require(e->version==10500 && dispatch(e,VST_EFFECT_OPCODE_VENDOR_VERSION)==10500,"Both host version fields must identify 1.5.0");
    require(e->unique_id==int32_t(VST_FOURCC('S','v','L','1')),"The upgrade must retain the plugin identity");
    require(dispatch(e,VST_EFFECT_OPCODE_TAIL_SAMPLES)==1,"Explicit no-tail sentinel");
    dispatch(e,VST_EFFECT_OPCODE_CREATE); dispatch(other,VST_EFFECT_OPCODE_CREATE);
    dispatch(e,VST_EFFECT_OPCODE_SET_BLOCK_SIZE,0,128); preset(e);
    char name[32]{}; dispatch(e,VST_EFFECT_OPCODE_EFFECT_NAME,0,0,name);
    require(std::strcmp(name,"SteadyVoice")==0,"Effect metadata");
    for(int i=0;i<ParamCount;++i) {
        require(std::abs(e->get_parameter(e,i)-defaults()[i])<1e-6,"Default parameter round trip");
        char shortName[10]; std::memset(shortName,0x7f,sizeof(shortName));
        dispatch(e,VST_EFFECT_OPCODE_PARAM_NAME,i,0,shortName);
        require(shortName[7]==0 || std::strlen(shortName)<8,"Short labels must terminate within 8 bytes");
        require(shortName[8]==char(0x7f)&&shortName[9]==char(0x7f),"Labels must not overwrite host buffers");
        vst_parameter_properties_t properties{};
        require(dispatch(e,VST_EFFECT_OPCODE_PARAM_PROPERTIES,i,0,&properties)==1,"Complete parameter properties");
        require(bool(properties.flags&VST_PARAMETER_FLAG_SWITCH)==isToggle(i),"Only actual toggles must be exposed as switches");
        if(i==TruePeak) require((properties.flags&VST_PARAMETER_FLAG_INTEGER_LIMITS) && properties.min_value_i32==0 && properties.max_value_i32==2 && properties.step_i32==1,"Peak detection must expose its three discrete modes to the host");
        e->set_parameter(e,i,.37f);
        require(e->get_parameter(e,i)==(isToggle(i)?0.0f:(i==TruePeak?.5f:.37f)),"Continuous controls must retain their values and discrete controls must canonicalize");
        e->set_parameter(e,i,defaults()[i]);
    }
    for(const auto& mode : {std::pair<const char*,float>{"Sample",0}, {"4x",.5f}, {"8x",1}}) {
        require(dispatch(e,VST_EFFECT_OPCODE_PARAM_VALUE_FROM_STRING,TruePeak,0,const_cast<char*>(mode.first))==1,"Host string entry must understand every peak-detection label");
        require(e->get_parameter(e,TruePeak)==mode.second,"Host string entry must select the intended peak-detection mode");
        char shown[10]; std::memset(shown,0x7f,sizeof(shown));
        require(dispatch(e,VST_EFFECT_OPCODE_PARAM_VALUE,TruePeak,0,shown)==1 && std::strcmp(shown,mode.first)==0,"Each peak mode must display its actual oversampling quality");
        require(shown[8]==char(0x7f) && shown[9]==char(0x7f),"Peak mode labels must fit host buffers");
    }
    e->set_parameter(e,Target,.37f);
    require(std::abs(other->get_parameter(other,Target)-defaults()[Target])<1e-6,"Instances must not share parameters");
    e->set_parameter(e,Target,std::numeric_limits<float>::quiet_NaN());
    require(e->get_parameter(e,Target)==.37f,"Invalid parameter writes must be ignored");
    e->set_parameter(e,Target,9); require(e->get_parameter(e,Target)==1,"Parameter values must clamp");
    e->set_parameter(e,Target,-9); require(e->get_parameter(e,Target)==0,"Negative parameter values must clamp");
    require(e->get_parameter(e,999)==0,"Invalid parameter indices must be harmless");
    for(int i : {Target,Boost,Cut,Ceiling}) {
        char text[]="999";
        require(dispatch(e,VST_EFFECT_OPCODE_PARAM_VALUE_FROM_STRING,i,0,text)==1,"Host numeric entry must accept the expanded controls");
        require(e->get_parameter(e,i)==1,"Host numeric entry must clamp to the new upper endpoint");
        char shown[10]; std::memset(shown,0x7f,sizeof(shown));
        require(dispatch(e,VST_EFFECT_OPCODE_PARAM_VALUE,i,0,shown)==1,"New endpoints must have host displays");
        require(std::strcmp(shown,(i==Boost || i==Cut)?"40.0":"0.0")==0,"Host must display the correct upper endpoint");
        require(shown[8]==char(0x7f) && shown[9]==char(0x7f),"Expanded endpoint displays must not overwrite host buffers");
    }
    for(int i : {Lift,Reduce}) {
        for(const char* text : {"0","-999"}) {
            require(dispatch(e,VST_EFFECT_OPCODE_PARAM_VALUE_FROM_STRING,i,0,const_cast<char*>(text))==1,"Host numeric entry must accept zero time and clamp negative times");
            require(e->get_parameter(e,i)==0 && physical(i,e->get_parameter(e,i))==0,"Both time controls must expose an exact zero-millisecond endpoint");
            char shown[10]; std::memset(shown,0x7f,sizeof(shown));
            require(dispatch(e,VST_EFFECT_OPCODE_PARAM_VALUE,i,0,shown)==1 && std::strcmp(shown,"0")==0,"Zero time must display correctly to the host");
            require(shown[8]==char(0x7f) && shown[9]==char(0x7f),"Time endpoint displays must stay within host buffers");
        }
        for(const char* text : {"0.5","0.001","0.00001"}) {
            require(dispatch(e,VST_EFFECT_OPCODE_PARAM_VALUE_FROM_STRING,i,0,const_cast<char*>(text))==1,"Sub-millisecond host entries must be accepted");
            char shown[10]; std::memset(shown,0x7f,sizeof(shown));
            require(dispatch(e,VST_EFFECT_OPCODE_PARAM_VALUE,i,0,shown)==1 && std::strtof(shown,nullptr)>0,"Positive sub-millisecond settings must not be displayed as zero");
            require(shown[8]==char(0x7f) && shown[9]==char(0x7f),"Sub-millisecond displays must stay within host buffers");
        }
    }
    preset(e,2); e->set_parameter(e,Target,1); e->set_parameter(e,Boost,1); e->set_parameter(e,Ceiling,1);
    e->set_parameter(e,Cut,1); e->set_parameter(e,Lift,0); e->set_parameter(e,Reduce,0);
    for(int i=RmsSoftest;i<=MedianRange;++i) e->set_parameter(e,i,.37f);
    e->set_parameter(e,Lookahead,normalized(Lookahead,5)); e->set_parameter(e,Adaptive,1);
    e->set_parameter(e,AdaptiveGuard,1);
    void* chunk=nullptr;
    const intptr_t bytes=dispatch(e,VST_EFFECT_OPCODE_GET_CHUNK_DATA,0,0,&chunk);
    require(bytes==16+ParamCount*4&&chunk,"Settings must serialize");
    std::vector<unsigned char> state(size_t(bytes),0); std::memcpy(state.data(),chunk,size_t(bytes));
    uint32_t stateVersion=0; std::memcpy(&stateVersion,state.data()+4,sizeof(stateVersion));
    require(stateVersion==5,"Quality controls must save a version-5 state chunk");
    uint32_t stateCount=0; std::memcpy(&stateCount,state.data()+8,sizeof(stateCount));
    require(stateCount==23 && bytes==108,"Version-5 state must have exactly 23 controls in 108 bytes");
    auto saved=std::array<float,ParamCount>{};
    for(int i=0;i<ParamCount;++i) saved[i]=e->get_parameter(e,i);
    preset(e,0);
    require(dispatch(e,VST_EFFECT_OPCODE_SET_CHUNK_DATA,0,bytes,state.data())==1,"Settings must restore");
    for(int i=0;i<ParamCount;++i) require(e->get_parameter(e,i)==saved[i],"All controls must survive save/load");
    require(e->delay==240 && notifiedDelay==240,"State restoration must report the restored lookahead latency");
    require(dispatch(e,VST_EFFECT_OPCODE_PROGRAM_GET)==2,"Preset identity must survive save/load");
    require(dispatch(e,VST_EFFECT_OPCODE_SET_CHUNK_DATA,0,bytes-1,state.data())==0,"Truncated chunks must be rejected");
    state[0]^=0xff;
    require(dispatch(e,VST_EFFECT_OPCODE_SET_CHUNK_DATA,0,bytes,state.data())==0,"Corrupt chunks must be rejected");
    require(e->get_parameter(e,Target)==saved[Target],"A rejected chunk must not change controls");

    constexpr int legacyCount=13;
    struct LegacyChunk { uint32_t magic,version,count,program; float values[legacyCount]; };
    static_assert(sizeof(LegacyChunk)==68,"Legacy state must retain its genuine 13-parameter layout");
    const float factoryTargets[]{-22,-24,-22,-24},factoryBoosts[]{12,8,18,10};
    const float factoryCuts[]{24,18,30,24},factoryLifts[]{600,1200,450,1400},factoryReductions[]{100,220,60,160};
    auto factoryChunk=[&](int p,uint32_t version) {
        LegacyChunk factory{VST_FOURCC('S','V','0','1'),version,legacyCount,uint32_t(p),{}};
        const auto expected=sv::preset(p);
        for(int i=0;i<legacyCount;++i) factory.values[i]=expected[i];
        if(version<=2) {
            factory.values[Cut]=factoryCuts[p]/36;
            factory.values[Lift]=std::log(factoryLifts[p]/100)/std::log(40.0f);
            factory.values[Reduce]=std::log(factoryReductions[p]/10)/std::log(100.0f);
        }
        if(version==1) {
            factory.values[Target]=(factoryTargets[p]+36)/24;
            factory.values[Boost]=factoryBoosts[p]/24;
            factory.values[Ceiling]=9/11.8f;
        }
        return factory;
    };
    for(uint32_t version : {1u,2u,3u}) for(int p=0;p<4;++p) for(float position : {0.0f,.37f,1.0f}) {
        auto legacy=factoryChunk(p,version);
        const auto expected=sv::preset(p);
        if(version==1) legacy.values[Target]=legacy.values[Boost]=legacy.values[Ceiling]=position;
        legacy.values[Cut]=legacy.values[Lift]=legacy.values[Reduce]=position;
        e->set_parameter(e,Lookahead,1); e->set_parameter(e,Adaptive,1);
        e->set_parameter(e,AdaptiveGuard,1); e->set_parameter(e,TruePeak,1);
        require(dispatch(e,VST_EFFECT_OPCODE_SET_CHUNK_DATA,0,sizeof(legacy),&legacy)==1,"Genuine 68-byte version-1 through version-3 chunks must load");
        if(version==1) {
            require(std::abs(physical(Target,e->get_parameter(e,Target))-(-36.0f+24.0f*position))<1e-5,"Legacy target must retain its physical dBFS value");
            require(std::abs(physical(Boost,e->get_parameter(e,Boost))-24.0f*position)<1e-5,"Legacy boost must retain its physical dB value");
            require(std::abs(physical(Ceiling,e->get_parameter(e,Ceiling))-(-12.0f+11.8f*position))<1e-5,"Legacy ceiling must retain its physical dBFS value");
        }
        if(version<=2) {
            require(std::abs(physical(Cut,e->get_parameter(e,Cut))-36*position)<1e-5,"Legacy cut must retain its physical dB value");
            require(std::abs(physical(Lift,e->get_parameter(e,Lift))-100*std::pow(40.0f,position))<.003f,"Legacy lift must retain its physical milliseconds");
            require(std::abs(physical(Reduce,e->get_parameter(e,Reduce))-10*std::pow(100.0f,position))<.001f,"Legacy reduction time must retain its physical milliseconds");
        } else {
            for(int i : {Cut,Lift,Reduce}) require(e->get_parameter(e,i)==position,"Version-3 continuous controls must retain their exact normalized values");
        }
        require(e->get_parameter(e,Lookahead)==0 && e->get_parameter(e,Adaptive)==0 && e->delay==0,"Legacy migration must retain zero latency and disable adaptive behavior");
        require(e->get_parameter(e,AdaptiveGuard)==0 && e->get_parameter(e,TruePeak)==0,"Legacy migration must disable both quality switches");
        require(dispatch(e,VST_EFFECT_OPCODE_PROGRAM_GET)==p,"Migration must preserve preset identity");
        for(int i=0;i<ParamCount;++i) if(i!=Cut && i!=Lift && i!=Reduce && (version!=1 || (i!=Target && i!=Boost && i!=Ceiling)))
            require(e->get_parameter(e,i)==expected[i],"Migration must not change other controls");
        const intptr_t migratedBytes=dispatch(e,VST_EFFECT_OPCODE_GET_CHUNK_DATA,0,0,&chunk);
        std::vector<unsigned char> migrated(size_t(migratedBytes),0); std::memcpy(migrated.data(),chunk,size_t(migratedBytes));
        auto migratedValues=std::array<float,ParamCount>{};
        for(int i=0;i<ParamCount;++i) migratedValues[i]=e->get_parameter(e,i);
        preset(e);
        require(dispatch(e,VST_EFFECT_OPCODE_SET_CHUNK_DATA,0,migratedBytes,migrated.data())==1,"Migrated state must save and reload as version 5");
        for(int i=0;i<ParamCount;++i) require(e->get_parameter(e,i)==migratedValues[i],"Migration must occur once without repeated remapping");
    }

    // Version 4 has exactly 21 parameters. Its final two controls must not be
    // read from beyond the buffer or silently inherited from a quality preset.
    struct PreviousChunk { uint32_t magic,version,count,program; float values[21]; };
    static_assert(sizeof(PreviousChunk)==100,"Version-4 chunks retain their original physical layout");
    for(int p=0;p<5;++p) {
        PreviousChunk previous{VST_FOURCC('S','V','0','1'),4,21,uint32_t(p),{}};
        const auto expected=sv::preset(p);
        for(int i=0;i<21;++i) previous.values[i]=expected[i];
        previous.values[RmsSoft]=.37f; previous.values[MedianRange]=.63f;
        previous.values[Lookahead]=normalized(Lookahead,7.5f); previous.values[Adaptive]=1;
        preset(e,5);
        require(dispatch(e,VST_EFFECT_OPCODE_SET_CHUNK_DATA,0,sizeof(previous),&previous)==1,"Genuine 100-byte version-4 chunks must restore");
        require(dispatch(e,VST_EFFECT_OPCODE_PROGRAM_GET)==p,"Version-4 loading must preserve each of its five preset identities");
        for(int i=0;i<21;++i) require(e->get_parameter(e,i)==previous.values[i],"Version-4 loading must retain all original normalized values exactly");
        require(e->get_parameter(e,AdaptiveGuard)==0 && e->get_parameter(e,TruePeak)==0,"Version-4 loading must disable both new quality switches");
        require(e->delay==360 && notifiedDelay==360,"Version-4 state must preserve its 7.5 ms delay without adding quality latency");
        require(dispatch(e,VST_EFFECT_OPCODE_GET_CHUNK_DATA,0,0,&chunk)==108,"Version-4 states must save into the complete version-5 layout");
        std::array<unsigned char,108> upgraded{}; std::memcpy(upgraded.data(),chunk,upgraded.size());
        preset(e,5);
        require(dispatch(e,VST_EFFECT_OPCODE_SET_CHUNK_DATA,0,upgraded.size(),upgraded.data())==1,"Upgraded version-4 states must reload as version 5");
        for(int i=0;i<21;++i) require(e->get_parameter(e,i)==previous.values[i],"Upgraded state must not remap version-4 values");
        require(e->get_parameter(e,AdaptiveGuard)==0 && e->get_parameter(e,TruePeak)==0,"Upgraded state must retain disabled quality switches");
    }
    LegacyChunk invalid{VST_FOURCC('S','V','0','1'),1,legacyCount,3,{}};
    for(int i=0;i<legacyCount;++i) invalid.values[i]=defaults()[i];
    auto unchanged=std::array<float,ParamCount>{};
    for(int i=0;i<ParamCount;++i) unchanged[i]=e->get_parameter(e,i);
    const intptr_t programBefore=dispatch(e,VST_EFFECT_OPCODE_PROGRAM_GET);
    for(uint32_t version : {1u,2u,3u}) for(float bad : {-1.0f,2.0f,std::numeric_limits<float>::quiet_NaN(),std::numeric_limits<float>::infinity()}) {
        invalid.version=version; invalid.values[Boost]=bad;
        require(dispatch(e,VST_EFFECT_OPCODE_SET_CHUNK_DATA,0,sizeof(invalid),&invalid)==0,"Invalid old and new chunks must be rejected before migration");
        require(dispatch(e,VST_EFFECT_OPCODE_PROGRAM_GET)==programBefore,"Invalid chunks must not change preset identity");
        for(int i=0;i<ParamCount;++i) require(e->get_parameter(e,i)==unchanged[i],"Rejected chunks must leave all controls unchanged");
    }
    invalid.version=6; invalid.values[Boost]=defaults()[Boost];
    require(dispatch(e,VST_EFFECT_OPCODE_SET_CHUNK_DATA,0,sizeof(invalid),&invalid)==0,"Unknown state versions must be rejected even when values are valid");
    for(int p=0;p<4;++p) {
        preset(e,p);
        require(std::abs(physical(Target,e->get_parameter(e,Target))-factoryTargets[p])<1e-5,"Factory target levels must retain their original dBFS values");
        require(std::abs(physical(Boost,e->get_parameter(e,Boost))-factoryBoosts[p])<1e-5,"Factory boost values must retain their original dB values");
        require(std::abs(physical(Ceiling,e->get_parameter(e,Ceiling))+3)<1e-5,"Factory peak ceilings must retain their original headroom");
        require(std::abs(physical(Cut,e->get_parameter(e,Cut))-factoryCuts[p])<1e-5,"Factory cuts must retain their original dB values");
        require(std::abs(physical(Lift,e->get_parameter(e,Lift))-factoryLifts[p])<.003f,"Factory lift times must retain their original milliseconds");
        require(std::abs(physical(Reduce,e->get_parameter(e,Reduce))-factoryReductions[p])<.001f,"Factory reduce times must retain their original milliseconds");
        for(uint32_t version : {1u,2u,3u}) {
            auto factory=factoryChunk(p,version);
            preset(e);
            require(dispatch(e,VST_EFFECT_OPCODE_SET_CHUNK_DATA,0,sizeof(factory),&factory)==1,"Both earlier factory state formats must migrate successfully");
            const auto expected=sv::preset(p);
            for(int i=0;i<ParamCount;++i) require(std::abs(e->get_parameter(e,i)-expected[i])<1e-6,"Migrated factory states must match their new factory presets");
        }
    }

    preset(e,4);
    require(dispatch(e,VST_EFFECT_OPCODE_PROGRAM_GET)==4,"The fifth preset must be selectable through the host");
    std::memset(name,0,sizeof(name)); dispatch(e,29,4,0,name);
    require(std::strcmp(name,"Smooth boost")==0,"The host must enumerate the new preset name");
    require(e->get_parameter(e,Adaptive)==1 && std::abs(physical(Lookahead,e->get_parameter(e,Lookahead))-5)<1e-6 && e->delay==240,"Smooth boost must enable adaptive leveling with 5 ms lookahead");
    require(e->get_parameter(e,AdaptiveGuard)==0 && e->get_parameter(e,TruePeak)==0,"Smooth boost must retain its version-4 guard behavior");
    require(dispatch(e,VST_EFFECT_OPCODE_PROGRAM_SET,0,PresetCount)==0 && dispatch(e,VST_EFFECT_OPCODE_PROGRAM_GET)==4,"Out-of-range presets must leave the program unchanged");
    for(intptr_t invalidProgram : {intptr_t(-1),intptr_t(1)<<32,-(intptr_t(1)<<32)})
        require(dispatch(e,VST_EFFECT_OPCODE_PROGRAM_SET,0,invalidProgram)==0 && dispatch(e,VST_EFFECT_OPCODE_PROGRAM_GET)==4,"Invalid 64-bit program values must be rejected before integer narrowing");
    struct ModernChunk { uint32_t magic,version,count,program; float values[ParamCount]; };
    static_assert(sizeof(ModernChunk)==108);
    require(dispatch(e,VST_EFFECT_OPCODE_GET_CHUNK_DATA,0,0,&chunk)==sizeof(ModernChunk),"Modern state has a fixed layout");
    ModernChunk modern{}; std::memcpy(&modern,chunk,sizeof(modern));
    for(int i=0;i<ParamCount;++i) unchanged[i]=e->get_parameter(e,i);
    const int latencyBefore=e->delay,changesBefore=ioChanges.load();
    auto reject=[&](const void* data,intptr_t size) {
        require(dispatch(e,VST_EFFECT_OPCODE_SET_CHUNK_DATA,0,size,const_cast<void*>(data))==0,"Malformed chunks must be rejected");
        require(dispatch(e,VST_EFFECT_OPCODE_PROGRAM_GET)==4 && e->delay==latencyBefore && ioChanges==changesBefore,"Rejected chunks must not change preset, latency, or host notifications");
        for(int i=0;i<ParamCount;++i) require(e->get_parameter(e,i)==unchanged[i],"Chunk rejection must preserve every parameter atomically");
    };
    for(float bad : {-1.0f,2.0f,std::numeric_limits<float>::quiet_NaN(),std::numeric_limits<float>::infinity()}) for(int p : {Target,Lookahead,Adaptive,AdaptiveGuard,TruePeak}) {
        ModernChunk malformed=modern; malformed.program=0; malformed.values[p]=bad;
        reject(&malformed,sizeof(malformed));
    }
    reject(nullptr,sizeof(modern)); reject(&modern,0); reject(&modern,15); reject(&modern,sizeof(modern)-1); reject(&modern,sizeof(modern)+1);
    for(uint32_t version : {0u,1u,2u,3u,4u,6u,0xffffffffu}) {
        ModernChunk malformed=modern; malformed.version=version;
        reject(&malformed,sizeof(malformed));
    }
    for(uint32_t count : {0u,13u,20u,21u,22u,24u,0xffffffffu}) {
        ModernChunk malformed=modern; malformed.count=count;
        reject(&malformed,sizeof(malformed));
    }
    ModernChunk malformed=modern; malformed.program=PresetCount;
    reject(&malformed,sizeof(malformed));
    invalid=factoryChunk(0,3); invalid.program=4; reject(&invalid,sizeof(invalid));
    invalid.program=0; invalid.count=14; reject(&invalid,sizeof(invalid));
    invalid.count=13; invalid.version=4; reject(&invalid,sizeof(invalid));
    invalid.version=3; reject(&invalid,sizeof(invalid)-1); reject(&invalid,sizeof(invalid)+1);
    std::array<unsigned char,sizeof(ModernChunk)+1> unaligned{};
    std::memcpy(unaligned.data()+1,&modern,sizeof(modern));
    require(dispatch(e,VST_EFFECT_OPCODE_SET_CHUNK_DATA,0,sizeof(modern),unaligned.data()+1)==1,"Valid state must load safely from an unaligned host buffer");

    preset(e,5);
    std::memset(name,0,sizeof(name)); dispatch(e,29,5,0,name);
    require(std::strcmp(name,"Quality voice")==0 && dispatch(e,VST_EFFECT_OPCODE_PROGRAM_GET)==5,"The host must enumerate and select Quality voice");
    require(e->get_parameter(e,AdaptiveGuard)==1 && e->get_parameter(e,TruePeak)==1 && e->get_parameter(e,Adaptive)==1,"Quality voice must enable all three quality controls");
    require(e->delay==303,"Quality voice must report 5 ms lookahead plus 63 samples for 8x detection at 48 kHz");
    for(int i=0;i<AdaptiveGuard;++i) require(e->get_parameter(e,i)==sv::preset(4)[i],"Quality voice must retain the Smooth boost leveling values");
    require(dispatch(e,VST_EFFECT_OPCODE_GET_CHUNK_DATA,0,0,&chunk)==sizeof(modern),"Quality preset must save a complete modern state");
    std::memcpy(&modern,chunk,sizeof(modern));
    preset(e);
    require(dispatch(e,VST_EFFECT_OPCODE_SET_CHUNK_DATA,0,sizeof(modern),&modern)==1,"Quality preset must reload from its modern state");
    require(e->get_parameter(e,AdaptiveGuard)==1 && e->get_parameter(e,TruePeak)==1 && dispatch(e,VST_EFFECT_OPCODE_PROGRAM_GET)==5,"Both quality controls and preset identity must survive a host state round trip");
    for(float mode : {0.0f,.5f,1.0f}) {
        e->set_parameter(e,TruePeak,mode);
        require(dispatch(e,VST_EFFECT_OPCODE_GET_CHUNK_DATA,0,0,&chunk)==sizeof(modern),"Every peak mode must save in the same version-5 layout");
        std::memcpy(&modern,chunk,sizeof(modern)); preset(e);
        require(dispatch(e,VST_EFFECT_OPCODE_SET_CHUNK_DATA,0,sizeof(modern),&modern)==1 && e->get_parameter(e,TruePeak)==mode,"Sample, 4x and 8x peak modes must each survive a state round trip");
        const int extra=mode>0?63:0;
        require(e->delay==240+extra && notifiedDelay==e->delay,"Restoring any peak mode must report its actual total delay");
    }

    // Report latency immediately on controls, then prove the actual audio path
    // has that exact delay with arbitrary host block boundaries and bypass.
    for(float sampleRate : {8000.0f,44100.0f,48000.0f,96000.0f,192000.0f,384000.0f}) {
        for(float lookMs : {0.0f,.1f,.5f,1.0f,5.0f,20.0f}) for(int mode : {0,1,2}) for(bool bypass : {false,true}) {
            preset(e);
            e->set_parameter(e,Strength,0); e->set_parameter(e,Guard,0);
            e->set_parameter(e,Bypass,bypass?1.0f:0.0f);
            e->set_parameter(e,Lookahead,normalized(Lookahead,lookMs));
            const int changesAtMode=ioChanges.load(),delayBeforeMode=e->delay;
            e->set_parameter(e,TruePeak,normalized(TruePeak,float(mode)));
            const int modeExtra=mode>0?63:0;
            const float requestedMs=physical(Lookahead,e->get_parameter(e,Lookahead));
            const float effectiveMs=mode>0 ? std::max(requestedMs,5.0f) : requestedMs;
            const int modeDelay=int(std::lround(48000.0*effectiveMs*.001))+modeExtra;
            require(e->delay==modeDelay && ioChanges==changesAtMode+(delayBeforeMode!=modeDelay),"Peak mode changes must immediately report and notify their native filter delay");
            const int changesAtRate=ioChanges.load(),oldDelay=e->delay;
            require(dispatch(e,VST_EFFECT_OPCODE_SET_SAMPLE_RATE,0,0,nullptr,sampleRate)==1,"Valid host sample rates must be accepted");
            const int expected=int(std::lround(double(sampleRate)*effectiveMs*.001))+modeExtra;
            require(e->delay==expected,"Host latency must use exact sample-rate rounding");
            require(ioChanges==changesAtRate+(oldDelay!=expected) && (oldDelay==expected || notifiedDelay==expected),"Sample-rate changes must notify the host exactly when latency changes");
            const int stableChanges=ioChanges.load();
            e->set_parameter(e,Lookahead,e->get_parameter(e,Lookahead));
            e->set_parameter(e,TruePeak,e->get_parameter(e,TruePeak));
            require(ioChanges==stableChanges,"Setting unchanged lookahead and detection mode must not issue duplicate latency notifications");
            dispatch(e,VST_EFFECT_OPCODE_SUSPEND_RESUME,0,1);
            require(e->delay==expected && dispatch(e,VST_EFFECT_OPCODE_TAIL_SAMPLES)==(expected>0?std::max(2,expected):1),"Resume must preserve declared latency and the delayed tail length");
            const size_t frames=size_t(expected)+129;
            std::vector<float> left(frames),right(frames),renderedLeft(frames,-9),renderedRight(frames,-9);
            left[0]=.25f; right[0]=-.375f; left[128]=-.125f; right[128]=.5f;
            size_t position=0; int blockIndex=0;
            constexpr int blockSizes[]{1,17,128,31,257};
            insideAudioCallback=true;
            while(position<frames) {
                const int n=int(std::min(frames-position,size_t(blockSizes[blockIndex++%5])));
                const float* inputs[]{left.data()+position,right.data()+position};
                float* outputs[]{renderedLeft.data()+position,renderedRight.data()+position};
                e->process_float(e,inputs,outputs,n); position+=size_t(n);
            }
            insideAudioCallback=false;
            for(size_t i=0;i<frames;++i) {
                const float expectedLeft=i>=size_t(expected)?left[i-size_t(expected)]:0;
                const float expectedRight=i>=size_t(expected)?right[i-size_t(expected)]:0;
                require(renderedLeft[i]==expectedLeft && renderedRight[i]==expectedRight,"Lookahead, peak mode, Guard off and bypass must preserve the exact unsmeared stereo delay");
            }
        }
    }
    require(renderHostCalls==0,"Latency synchronization must never call the host inside an audio callback");

    // Requested values below the protection minimum remain automatable, but
    // changing them must neither issue latency notifications nor flush audio.
    for(vst_effect_t* instance : {e,other}) {
        preset(instance,5);
        instance->set_parameter(instance,Strength,0); instance->set_parameter(instance,Guard,0);
        instance->set_parameter(instance,Lookahead,0); reset(instance);
    }
    const int minimumChanges=ioChanges.load();
    for(float requested : {0.0f,1.0f,4.0f}) {
        e->set_parameter(e,Lookahead,normalized(Lookahead,requested));
        require(std::abs(physical(Lookahead,e->get_parameter(e,Lookahead))-requested)<1e-5,"The protection minimum must not rewrite the requested lookahead parameter");
        require(e->delay==303 && ioChanges==minimumChanges,"Lookahead changes below 5 ms with oversampling must preserve total latency without duplicate notifications");
    }
    e->set_parameter(e,Lookahead,0);
    std::array<float,128> continuousLeft{},continuousRight{},continuousOutLeft{},continuousOutRight{},referenceLeft{},referenceRight{};
    float* continuousOutputs[]{continuousOutLeft.data(),continuousOutRight.data()};
    float* referenceOutputs[]{referenceLeft.data(),referenceRight.data()};
    const float* continuousInputs[]{continuousLeft.data(),continuousRight.data()};
    for(int block=0;block<12;++block) {
        for(int i=0;i<128;++i) {
            continuousLeft[size_t(i)]=float(.1*std::sin((block*128+i)*.13));
            continuousRight[size_t(i)]=float(.07*std::cos((block*128+i)*.17));
        }
        constexpr float requests[]{0,1,4};
        e->set_parameter(e,Lookahead,normalized(Lookahead,requests[block%3]));
        e->process_float(e,continuousInputs,continuousOutputs,128);
        other->process_float(other,continuousInputs,referenceOutputs,128);
        require(continuousOutLeft==referenceLeft && continuousOutRight==referenceRight,"Changing requested lookahead below the minimum must preserve continuous delayed audio without clearing the buffers");
    }
    require(ioChanges==minimumChanges,"Sub-minimum lookahead automation must not notify during continuous processing");
    preset(other);

    float in0[128]{},in1[128]{},out0[128]{},out1[128]{};
    const float* input[2]{in0,in1}; float* output[2]{out0,out1};
    preset(e); e->set_parameter(e,Strength,0); e->set_parameter(e,Guard,0); reset(e);
    in0[0]=in1[0]=.25f; e->process_float(e,input,output,128);
    require(out0[0]==.25f&&out1[0]==.25f,"Actual plugin must deliver an impulse at sample zero");
    require(out0[1]==0&&out1[127]==0,"Actual plugin must not delay or smear the impulse");
    double doubleIn[128]{},doubleOut[128]{}; doubleIn[0]=.123456789123;
    const double* doubleInputs[2]{doubleIn,doubleIn}; double* doubleOutputs[2]{doubleOut,nullptr};
    reset(e); e->process_double(e,doubleInputs,doubleOutputs,128);
    require(doubleOut[0]==doubleIn[0],"64-bit replacing must retain double precision");
    e->set_parameter(e,Lookahead,normalized(Lookahead,.5f));
    for(bool bypass : {false,true}) {
        e->set_parameter(e,Bypass,bypass?1.0f:0.0f); reset(e,44100);
        insideAudioCallback=true; e->process_double(e,doubleInputs,doubleOutputs,128); insideAudioCallback=false;
        require(e->delay==22 && doubleOut[22]==doubleIn[0],"64-bit processing and bypass must preserve exact precision after the declared lookahead");
        for(int i=0;i<128;++i) if(i!=22) require(doubleOut[i]==0,"64-bit lookahead must not smear the delayed impulse");
    }
    e->set_parameter(e,Lookahead,0); e->set_parameter(e,Bypass,0); reset(e);
    e->process_float(e,input,output,0); e->process_float(e,input,output,-1);
    require(out0[0]==.25f,"Empty callbacks must not change the buffers");
    reset(e); std::fill(std::begin(out0),std::end(out0),.1f); std::fill(std::begin(out1),std::end(out1),.1f);
    e->process(e,input,output,128);
    require(std::abs(out0[0]-.35f)<1e-7&&out0[1]==.1f,"Legacy accumulating callback must add to its buffers");

    preset(e); for(int i=0;i<128;++i) in0[i]=in1[i]=float(.07*std::sin(6.283185307179586*1000*i/48000));
#ifndef _WIN32
    const unsigned csr=_mm_getcsr();
    e->process_float(e,input,output,128);
    require(_mm_getcsr()==csr,"The plugin must restore the host's floating-point mode");
    allocations=0; trackAllocations=true; insideAudioCallback=true;
    for(int i=0;i<1000;++i) e->process_float(e,input,output,128);
    insideAudioCallback=false; trackAllocations=false;
    require(allocations==0,"Audio callbacks must perform zero C++ heap allocations");
    for(int i : {Target,Boost,Cut,Ceiling}) e->set_parameter(e,i,1);
    e->set_parameter(e,Lift,0); e->set_parameter(e,Reduce,0);
    allocations=0; trackAllocations=true; insideAudioCallback=true;
    for(int i=0;i<1000;++i) e->process_float(e,input,output,128);
    insideAudioCallback=false; trackAllocations=false;
    require(allocations==0,"Expanded-range audio callbacks must also allocate nothing");
    require(std::all_of(std::begin(out0),std::end(out0),[](float x){return std::isfinite(x)&&std::abs(x)<=1;}),"The actual plugin must honor the 0 dBFS ceiling at expanded settings");
    preset(e,5); e->set_parameter(e,Lookahead,1); reset(e,384000);
    allocations=0; trackAllocations=true; insideAudioCallback=true;
    for(int i=0;i<1000;++i) { e->process_float(e,input,output,128); e->process_double(e,doubleInputs,doubleOutputs,128); }
    insideAudioCallback=false; trackAllocations=false;
    require(allocations==0,"The first callback, maximum lookahead, all quality switches, and both precisions must allocate nothing");
    allocations=0; trackAllocations=true;
    for(int i=0;i<24;++i) {
        e->set_parameter(e,Lookahead,normalized(Lookahead,float(i%21)));
        e->set_parameter(e,TruePeak,normalized(TruePeak,float(i%3)));
        dispatch(e,VST_EFFECT_OPCODE_SET_SAMPLE_RATE,0,0,nullptr,i%2?44100.0f:384000.0f);
        dispatch(e,VST_EFFECT_OPCODE_SUSPEND_RESUME,0,1);
        insideAudioCallback=true; e->process_float(e,input,output,128); insideAudioCallback=false;
    }
    trackAllocations=false;
    require(allocations==0,"Rate changes and live lookahead reconfiguration must remain allocation-free");
    require(renderHostCalls==0,"Audio callbacks must not issue latency or other host notifications");
#endif
    // Real host-driven concurrent automation while the audio thread processes.
    std::atomic<bool> stop{false},ready{false};
    std::thread control([&]{
        ready=true; uint32_t seed=991;
        while(!stop.load(std::memory_order_relaxed)) {
            seed=1664525*seed+1013904223;
            e->set_parameter(e,(seed>>24)%Guard,(seed&7)==0?0.0f:float(seed&0xffff)/65535.0f);
        }
    });
    while(!ready.load()) std::this_thread::yield();
    bool finite=true;
    for(int block=0;block<3000;++block) {
        e->process_float(e,input,output,128);
        for(float x:out0) finite=finite&&std::isfinite(x)&&std::abs(x)<=1;
    }
    stop=true; control.join(); require(finite,"Concurrent automation must keep the output stable");

    preset(e);
    const auto before=std::chrono::steady_clock::now();
    for(int i=0;i<22500;++i) e->process_float(e,input,output,128); // 60 seconds.
    const auto elapsed=std::chrono::duration<double>(std::chrono::steady_clock::now()-before).count();
    std::cout<<"actual_plugin_audio_seconds=60 wall_seconds="<<elapsed
             <<" one_core_percent="<<elapsed/60*100<<"\n";

#ifdef _WIN32
    const DWORD gdiBefore=GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS);
    HWND parent=CreateWindowExA(0,"STATIC","SteadyVoice test host",WS_OVERLAPPEDWINDOW|WS_VISIBLE,
                               30,30,930,780,nullptr,nullptr,GetModuleHandle(nullptr),nullptr);
    HWND parent2=CreateWindowExA(0,"STATIC","Second plugin instance",WS_OVERLAPPEDWINDOW|WS_VISIBLE,
                                1000,30,930,780,nullptr,nullptr,GetModuleHandle(nullptr),nullptr);
    require(parent&&parent2,"Windows UI host must open");
    vst_rect_t* rect=nullptr;
    require(dispatch(e,VST_EFFECT_OPCODE_EDITOR_GET_RECT,0,0,&rect)==1&&rect&&rect->right==900&&rect->bottom==720,"Editor must report its exact size");
    require(dispatch(e,VST_EFFECT_OPCODE_EDITOR_OPEN,0,0,parent)==1,"The actual DLL editor must open");
    require(dispatch(other,VST_EFFECT_OPCODE_EDITOR_OPEN,0,0,parent2)==1,"Multiple editors must open together");
    HWND editor=FindWindowExA(parent,nullptr,"SteadyVoiceEditor_v1_5",nullptr);
    HWND editor2=FindWindowExA(parent2,nullptr,"SteadyVoiceEditor_v1_5",nullptr);
    require(editor&&editor2,"Editor must create native child windows");
    pump(); UpdateWindow(editor);
    HDC editorDC=GetDC(editor);
    require(GetPixel(editorDC,450,2)==RGB(102,103,171),"The rendered main motif must exactly match #6667ab");
    ReleaseDC(editor,editorDC);
    const float targetBefore=e->get_parameter(e,Target);
    SendMessageA(editor,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(220,300));
    SendMessageA(editor,WM_LBUTTONUP,0,MAKELPARAM(220,300));
    require(e->get_parameter(e,Target)!=targetBefore,"Editor slider must change the real audio parameter");
    require(updates>0&&begins>0&&ends>0,"Editor must notify the host about automation");
    SendMessageA(editor,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(245,249));
    HWND number=FindWindowExA(editor,nullptr,"EDIT",nullptr);
    require(number!=nullptr,"Clicking a displayed value must open numeric entry");
    SetWindowTextA(number,"-23.5"); SendMessageA(number,WM_KEYDOWN,VK_RETURN,0);
    require(std::abs(physical(Target,e->get_parameter(e,Target))+23.5f)<.001f,"Numeric entry must reach the DSP parameter");
    SendMessageA(editor,WM_KEYDOWN,VK_RIGHT,0);
    require(std::abs(physical(Target,e->get_parameter(e,Target))+23.0f)<.001f,"Keyboard adjustment must work");
    struct EndpointControl { int index,x,y; const char* value; float high; };
    for(const auto& control : {EndpointControl{Target,245,249,"0",0},EndpointControl{Boost,533,249,"40",40},EndpointControl{Cut,821,249,"40",40},EndpointControl{Ceiling,245,567,"0",0}}) {
        SendMessageA(editor,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(control.x,control.y));
        HWND entryBox=FindWindowExA(editor,nullptr,"EDIT",nullptr);
        require(entryBox!=nullptr,"Every expanded control must expose numeric entry");
        SetWindowTextA(entryBox,control.value); SendMessageA(entryBox,WM_KEYDOWN,VK_RETURN,0);
        require(physical(control.index,e->get_parameter(e,control.index))==control.high,"Numeric entry must reach each new endpoint");
        SendMessageA(editor,WM_KEYDOWN,VK_HOME,0);
        require(physical(control.index,e->get_parameter(e,control.index))==params[control.index].low,"Home must select the correct lower endpoint");
        SendMessageA(editor,WM_KEYDOWN,VK_END,0);
        require(physical(control.index,e->get_parameter(e,control.index))==control.high,"End must select each expanded upper endpoint");
        SendMessageA(editor,WM_KEYDOWN,VK_RIGHT,0);
        require(physical(control.index,e->get_parameter(e,control.index))==control.high,"Keyboard adjustments must not exceed the expanded endpoints");
        SendMessageA(editor,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(control.x+39,control.y+51));
        SendMessageA(editor,WM_LBUTTONUP,0,MAKELPARAM(control.x+39,control.y+51));
        require(physical(control.index,e->get_parameter(e,control.index))==control.high,"Dragging must reach the expanded upper endpoints");
    }
    for(const auto& control : {EndpointControl{Lift,245,355,"0",4000},EndpointControl{Reduce,533,355,"0",1000}}) {
        SendMessageA(editor,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(control.x,control.y));
        HWND entryBox=FindWindowExA(editor,nullptr,"EDIT",nullptr);
        require(entryBox!=nullptr,"Both zero-time controls must expose numeric entry");
        SetWindowTextA(entryBox,"0"); SendMessageA(entryBox,WM_KEYDOWN,VK_RETURN,0);
        require(e->get_parameter(e,control.index)==0,"Typing zero milliseconds must reach the exact minimum");
        SendMessageA(editor,WM_KEYDOWN,VK_LEFT,0);
        require(e->get_parameter(e,control.index)==0,"Keyboard decrement must stay at zero without producing an invalid value");
        SendMessageA(editor,WM_KEYDOWN,VK_END,0);
        require(physical(control.index,e->get_parameter(e,control.index))==control.high,"End must retain the original maximum time");
        SendMessageA(editor,WM_KEYDOWN,VK_HOME,0);
        require(e->get_parameter(e,control.index)==0,"Home must select zero milliseconds");
        const int sliderLeft=control.x-205;
        SendMessageA(editor,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(sliderLeft,control.y+51));
        SendMessageA(editor,WM_LBUTTONUP,0,MAKELPARAM(sliderLeft,control.y+51));
        require(e->get_parameter(e,control.index)==0,"Dragging to the left endpoint must select exact zero milliseconds");
    }
    SendMessageA(editor,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(650,600));
    require(e->get_parameter(e,Guard)==0,"Peak guard switch must work");
    SendMessageA(editor,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(785,600));
    require(e->get_parameter(e,Bypass)==1,"Bypass switch must work");

    HWND mainTab=GetDlgItem(editor,2001),advancedTab=GetDlgItem(editor,2002);
    require(mainTab&&advancedTab,"The editor must expose both native, keyboard-focusable tab buttons");
    SendMessageA(advancedTab,BM_CLICK,0,0);
    const float targetOnMain=e->get_parameter(e,Target);
    auto enterAdvanced=[&](int parameterIndex,int x,int y,const char* value) {
        SendMessageA(editor,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(x,y));
        HWND entryBox=FindWindowExA(editor,nullptr,"EDIT",nullptr);
        require(entryBox!=nullptr,"Each advanced continuous control must offer numeric entry");
        SetWindowTextA(entryBox,value); SendMessageA(entryBox,WM_KEYDOWN,VK_RETURN,0);
        const float expected=std::strtof(value,nullptr);
        require(std::abs(physical(parameterIndex,e->get_parameter(e,parameterIndex))-expected)<.002f,"Advanced numeric entry must reach the intended parameter");
        require(e->get_parameter(e,Target)==targetOnMain,"Advanced controls must preserve the main page target");
    };
    for(const auto& control : {EndpointControl{RmsSoftest,245,249,"350",1000},EndpointControl{RmsSoft,533,249,"140",1000},EndpointControl{RmsMedian,821,249,"65",1000},EndpointControl{RmsLoud,245,355,"15",1000},EndpointControl{RmsLoudest,533,355,"4",1000}})
        enterAdvanced(control.index,control.x,control.y,control.value);
    enterAdvanced(MedianRange,821,355,"6"); enterAdvanced(MedianRange,821,355,"12");
    for(const char* text : {"5","20","0.1"}) {
        const int oldLatency=e->delay,changes=ioChanges.load();
        enterAdvanced(Lookahead,245,461,text);
        const int expected=int(std::lround(48000.0*physical(Lookahead,e->get_parameter(e,Lookahead))*.001));
        require(e->delay==expected && ioChanges==changes+(oldLatency!=expected) && notifiedDelay==expected,"Typed lookahead must immediately update and notify the host's sample latency");
    }
    const float adaptiveBefore=e->get_parameter(e,Adaptive);
    SendMessageA(editor,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(450,490));
    require(e->get_parameter(e,Adaptive)==1-adaptiveBefore,"The advanced adaptive switch must respond to clicks");
    SendMessageA(editor,WM_KEYDOWN,VK_SPACE,0);
    require(e->get_parameter(e,Adaptive)==adaptiveBefore,"The focused adaptive switch must respond to Space");
    SendMessageA(editor,WM_KEYDOWN,VK_RETURN,0);
    require(e->get_parameter(e,Adaptive)==1-adaptiveBefore,"The focused adaptive switch must respond to Enter");

    SendMessageA(editor,WM_KEYDOWN,VK_TAB,0);
    const float recoveryBefore=e->get_parameter(e,AdaptiveGuard);
    SendMessageA(editor,WM_KEYDOWN,VK_SPACE,0);
    require(e->get_parameter(e,AdaptiveGuard)==1-recoveryBefore,"Tab after adaptive RMS must reach adaptive guard");
    SendMessageA(editor,WM_KEYDOWN,VK_TAB,0);
    const float truePeakBefore=e->get_parameter(e,TruePeak);
    SendMessageA(editor,WM_KEYDOWN,VK_SPACE,0);
    require(e->get_parameter(e,TruePeak)==normalized(TruePeak,float((int(physical(TruePeak,truePeakBefore))+1)%3)),"Tab after adaptive guard must reach peak detection and cycle to its next mode");
    SendMessageA(editor,WM_KEYDOWN,VK_TAB,0);
    require(GetFocus()==mainTab,"Tab after the final advanced control must reach Main");
    SendMessageA(mainTab,WM_KEYDOWN,VK_TAB,0);
    require(GetFocus()==advancedTab,"Keyboard traversal must reach the RMS / Quality tab");
    SendMessageA(advancedTab,WM_KEYDOWN,VK_RETURN,0);
    SendMessageA(advancedTab,WM_KEYDOWN,VK_TAB,0);
    require(GetFocus()==editor,"Tab from the active page button must reach its first control");
    SendMessageA(editor,WM_KEYDOWN,VK_RETURN,0);
    number=FindWindowExA(editor,nullptr,"EDIT",nullptr);
    require(number!=nullptr,"Keyboard focus must allow editing the first RMS control");
    SetWindowTextA(number,"321"); SendMessageA(number,WM_KEYDOWN,VK_RETURN,0);
    require(std::abs(physical(RmsSoftest,e->get_parameter(e,RmsSoftest))-321)<.002f,"Keyboard traversal must focus Softest RMS");
    for(int parameterIndex : {RmsSoft,RmsMedian,RmsLoud,RmsLoudest,MedianRange,Lookahead}) {
        SendMessageA(editor,WM_KEYDOWN,VK_TAB,0);
        SendMessageA(editor,WM_KEYDOWN,VK_HOME,0);
        require(e->get_parameter(e,parameterIndex)==0,"Tab must reach every advanced continuous control in visual order");
    }
    SendMessageA(editor,WM_KEYDOWN,VK_TAB,0);
    const float keyboardAdaptive=e->get_parameter(e,Adaptive);
    SendMessageA(editor,WM_KEYDOWN,VK_SPACE,0);
    require(e->get_parameter(e,Adaptive)==1-keyboardAdaptive,"Keyboard traversal must also reach the adaptive switch");
    for(int parameterIndex : {AdaptiveGuard,TruePeak}) {
        SendMessageA(editor,WM_KEYDOWN,VK_TAB,0);
        const float previous=e->get_parameter(e,parameterIndex);
        SendMessageA(editor,WM_KEYDOWN,VK_SPACE,0);
        const float expected=parameterIndex==TruePeak
            ? normalized(TruePeak,float((int(physical(TruePeak,previous))+1)%3)) : 1-previous;
        require(e->get_parameter(e,parameterIndex)==expected,"Keyboard traversal must reach both quality controls in visual order");
    }
    for(const auto& control : {EndpointControl{AdaptiveGuard,738,490,"",1}}) {
        const float previous=e->get_parameter(e,control.index);
        SendMessageA(editor,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(control.x,control.y));
        require(e->get_parameter(e,control.index)==1-previous,"Each quality switch must respond to clicks at its own card");
        SendMessageA(editor,WM_KEYDOWN,VK_RETURN,0);
        require(e->get_parameter(e,control.index)==previous,"Each focused quality switch must respond to Enter");
    }
    const float peakModeBefore=e->get_parameter(e,TruePeak);
    SendMessageA(editor,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(162,596));
    require(e->get_parameter(e,TruePeak)==normalized(TruePeak,float((int(physical(TruePeak,peakModeBefore))+1)%3)),"Clicking peak detection must advance Sample, 4x and 8x cyclically");
    SendMessageA(editor,WM_KEYDOWN,VK_RETURN,0);
    require(e->get_parameter(e,TruePeak)==normalized(TruePeak,float((int(physical(TruePeak,peakModeBefore))+2)%3)),"Enter must advance the focused peak detection mode");
    SendMessageA(editor,WM_KEYDOWN,VK_SPACE,0);
    require(e->get_parameter(e,TruePeak)==peakModeBefore,"Three peak-mode actions must return to the original mode");
    SendMessageA(editor,WM_KEYDOWN,VK_HOME,0);
    require(e->get_parameter(e,TruePeak)==0,"Home must select sample-peak detection");
    SendMessageA(editor,WM_KEYDOWN,VK_END,0);
    require(e->get_parameter(e,TruePeak)==1,"End must select 8x peak detection");

    auto controlTab=[&](HWND target) {
        BYTE previous[256]{},modified[256]{};
        require(GetKeyboardState(previous)!=FALSE,"The native test host must capture keyboard state");
        std::memcpy(modified,previous,sizeof(modified));
        modified[VK_CONTROL]|=0x80; modified[VK_LCONTROL]|=0x80;
        require(SetKeyboardState(modified)!=FALSE,"The native test host must set the Ctrl modifier");
        SendMessageA(target,WM_KEYDOWN,VK_TAB,0);
        require(SetKeyboardState(previous)!=FALSE,"The native test host must restore keyboard state");
    };
    controlTab(editor);
    SendMessageA(editor,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(245,249));
    number=FindWindowExA(editor,nullptr,"EDIT",nullptr);
    require(number!=nullptr,"Ctrl + Tab must return to an editable Main page");
    SetWindowTextA(number,"-21.5"); SendMessageA(number,WM_KEYDOWN,VK_RETURN,0);
    require(std::abs(physical(Target,e->get_parameter(e,Target))+21.5f)<.002f,"Ctrl + Tab must select Main rather than a stale advanced control");
    controlTab(editor);
    SendMessageA(editor,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(245,249));
    number=FindWindowExA(editor,nullptr,"EDIT",nullptr);
    require(number!=nullptr,"Ctrl + Tab must return to RMS numeric entry");
    SetWindowTextA(number,"333"); SendMessageA(number,WM_KEYDOWN,VK_RETURN,0);
    require(std::abs(physical(RmsSoftest,e->get_parameter(e,RmsSoftest))-333)<.002f,"Ctrl + Tab must select RMS / Quality rather than a stale main control");
    // Finish on useful advanced values and prove closing the native editor does
    // not lose any parameter or affect another instance's editor resources.
    for(const auto& control : {EndpointControl{RmsSoftest,245,249,"350",1000},EndpointControl{RmsSoft,533,249,"140",1000},EndpointControl{RmsMedian,821,249,"65",1000},EndpointControl{RmsLoud,245,355,"15",1000},EndpointControl{RmsLoudest,533,355,"4",1000}}) {
        SendMessageA(editor,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(control.x,control.y));
        number=FindWindowExA(editor,nullptr,"EDIT",nullptr);
        require(number!=nullptr,"Advanced numeric entry must survive keyboard page changes");
        SetWindowTextA(number,control.value); SendMessageA(number,WM_KEYDOWN,VK_RETURN,0);
    }
    e->set_parameter(e,MedianRange,normalized(MedianRange,12)); e->set_parameter(e,Lookahead,normalized(Lookahead,5)); e->set_parameter(e,Adaptive,1);
    e->set_parameter(e,AdaptiveGuard,1); e->set_parameter(e,TruePeak,1);
    auto beforeReopen=std::array<float,ParamCount>{};
    for(int i=0;i<ParamCount;++i) beforeReopen[i]=e->get_parameter(e,i);
    dispatch(e,VST_EFFECT_OPCODE_EDITOR_KEEP_ALIVE); pump(); UpdateWindow(editor);
    if(argc>3) require(saveBitmap(editor,argv[3]),"The rendered advanced page must be capturable");
    dispatch(e,VST_EFFECT_OPCODE_EDITOR_CLOSE);
    require(IsWindow(editor2),"Closing the advanced editor must preserve the other instance");
    require(dispatch(e,VST_EFFECT_OPCODE_EDITOR_OPEN,0,0,parent)==1,"The advanced editor must reopen cleanly");
    editor=FindWindowExA(parent,nullptr,"SteadyVoiceEditor_v1_5",nullptr);
    require(editor!=nullptr,"Reopening must recreate the native editor");
    for(int i=0;i<ParamCount;++i) require(e->get_parameter(e,i)==beforeReopen[i],"Closing and reopening the editor must preserve all advanced controls");
    require(e->delay==303,"Reopening must preserve lookahead plus oversampled peak-detection latency");
    mainTab=GetDlgItem(editor,2001); advancedTab=GetDlgItem(editor,2002);
    require(mainTab&&advancedTab,"Reopening must recreate both tab controls");
    SendMessageA(mainTab,BM_CLICK,0,0);
    preset(e);
    for(int i=0;i<128;++i) in0[i]=in1[i]=float(.04*std::sin(6.283185307179586*1000*i/48000));
    for(int i=0;i<2250;++i) e->process_float(e,input,output,128);
    dispatch(e,VST_EFFECT_OPCODE_EDITOR_KEEP_ALIVE); pump(); UpdateWindow(editor);
    require(saveBitmap(editor,argc>2?argv[2]:"editor.bmp"),"Actual rendered editor must be capturable");
    dispatch(e,VST_EFFECT_OPCODE_EDITOR_CLOSE);
    require(IsWindow(editor2),"Closing one instance must not destroy another editor");
    for(int i=0;i<4;++i) {
        require(dispatch(e,VST_EFFECT_OPCODE_EDITOR_OPEN,0,0,parent)==1,"Editor must reopen cleanly");
        dispatch(e,VST_EFFECT_OPCODE_EDITOR_CLOSE);
    }
    dispatch(other,VST_EFFECT_OPCODE_EDITOR_CLOSE); DestroyWindow(parent); DestroyWindow(parent2); pump();
    const DWORD gdiAfter=GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS);
    std::cout<<"gdi_objects_before="<<gdiBefore<<" after="<<gdiAfter<<"\n";
    require(gdiAfter<=gdiBefore+2,"Repeated editor opening must not leak GDI objects");
#endif
    dispatch(e,VST_EFFECT_OPCODE_DESTROY); dispatch(other,VST_EFFECT_OPCODE_DESTROY);
#ifdef _WIN32
    FreeLibrary(module);
#else
    dlclose(module);
#endif
    std::cout<<"PASS host_assertions="<<assertions<<"\n";
}
