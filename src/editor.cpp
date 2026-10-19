#include "editor.hpp"
#ifdef _WIN32
#include <windowsx.h>
#include <cstring>
#include <cstdlib>
#include <cctype>

namespace sv {
namespace {
// The complete custom palette is #6667ab blended only with black or white.
constexpr COLORREF motif=RGB(102,103,171);
constexpr COLORREF shade(unsigned amount) {
    return RGB((102*amount+50)/100,(103*amount+50)/100,(171*amount+50)/100);
}
constexpr COLORREF tint(unsigned amount) {
    return RGB(102+(153*amount+50)/100,103+(152*amount+50)/100,171+(84*amount+50)/100);
}
constexpr COLORREF background=shade(24), surface=shade(34), track=shade(58);
constexpr COLORREF ink=tint(85), muted=tint(38), accent=tint(60), emphasis=tint(78);
constexpr int order[]={Target,Boost,Cut,Lift,Reduce,Strength,Floor,Hold,DetectorHPF,Ceiling,Trim};
constexpr int advancedOrder[]={RmsSoftest,RmsSoft,RmsMedian,RmsLoud,RmsLoudest,MedianRange,Lookahead,Adaptive,AdaptiveGuard,TruePeak};
constexpr int tabIds[]={2001,2002};
constexpr const char* tabNames[]={"Main","RMS / Quality"};
constexpr int mainTabFocus=-2, advancedTabFocus=-3;
constexpr const char* editorClass="SteadyVoiceEditor_v1_5";
void fill(HDC dc,RECT r,COLORREF c) {
    HBRUSH b=CreateSolidBrush(c); FillRect(dc,&r,b); DeleteObject(b);
}
HFONT makeFont(int size,int weight=FW_NORMAL) {
    return CreateFontA(-size,0,0,0,weight,FALSE,FALSE,FALSE,DEFAULT_CHARSET,
                       OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,
                       DEFAULT_PITCH|FF_DONTCARE,"Segoe UI");
}
bool inside(RECT r,int x,int y) { return x>=r.left && x<r.right && y>=r.top && y<r.bottom; }
bool isButton(int index) { return isToggle(index) || index==TruePeak; }
}
Editor::~Editor() { close(); }
bool Editor::open(void* parent) {
    if (window_) return true;
    if (!IsWindow(static_cast<HWND>(parent))) return false;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                      reinterpret_cast<LPCSTR>(&windowProc),&module_);
    WNDCLASSEXA cls{}; cls.cbSize=sizeof(cls); cls.style=CS_DBLCLKS;
    cls.lpfnWndProc=windowProc; cls.hInstance=module_; cls.hCursor=LoadCursor(nullptr,IDC_ARROW);
    cls.lpszClassName=editorClass;
    if (!RegisterClassExA(&cls) && GetLastError()!=ERROR_CLASS_ALREADY_EXISTS) return false;
    title_=makeFont(30,FW_SEMIBOLD); body_=makeFont(14,FW_SEMIBOLD);
    small_=makeFont(12); numberFont_=makeFont(21,FW_SEMIBOLD);
    window_=CreateWindowExA(0,editorClass,"SteadyVoice",WS_CHILD|WS_VISIBLE|WS_CLIPCHILDREN|WS_TABSTOP,
                           0,0,900,720,static_cast<HWND>(parent),nullptr,module_,this);
    if (!window_) { close(); return false; }
    HDC dc=GetDC(window_); buffer_=CreateCompatibleDC(dc);
    bitmap_=CreateCompatibleBitmap(dc,900,720); ReleaseDC(window_,dc);
    if (!buffer_ || !bitmap_) { close(); return false; }
    oldBitmap_=static_cast<HBITMAP>(SelectObject(buffer_,bitmap_));
    const RECT tabRects[]={{365,27,442,64},{452,27,633,64}};
    for (int i=0;i<2;++i) {
        const RECT r=tabRects[i];
        tabs_[i]=CreateWindowExA(0,"BUTTON",tabNames[i],WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_OWNERDRAW,
                                r.left,r.top,r.right-r.left,r.bottom-r.top,window_,
                                reinterpret_cast<HMENU>(static_cast<INT_PTR>(tabIds[i])),module_,nullptr);
        if (!tabs_[i]) { close(); return false; }
        SendMessageA(tabs_[i],WM_SETFONT,reinterpret_cast<WPARAM>(body_),TRUE);
        SetWindowLongPtrA(tabs_[i],GWLP_USERDATA,reinterpret_cast<LONG_PTR>(this));
        const auto original=reinterpret_cast<WNDPROC>(SetWindowLongPtrA(tabs_[i],GWLP_WNDPROC,reinterpret_cast<LONG_PTR>(tabProc)));
        if (!oldTabProc_) oldTabProc_=original;
    }
    dirty_=true; drawnValues_.fill(-1); drawnLatencyMs_=drawnSampleRate_=-1;
    SetTimer(window_,1,50,nullptr);
    InvalidateRect(window_,nullptr,FALSE); UpdateWindow(window_);
    return true;
}
void Editor::close() noexcept {
    if (number_) finishNumber(false);
    if (window_) {
        if (dragging_>=0) { plugin_.endEdit(dragging_); dragging_=-1; }
        KillTimer(window_,1); DestroyWindow(window_); window_=nullptr;
    }
    tabs_[0]=tabs_[1]=nullptr; oldTabProc_=nullptr;
    if (buffer_) { if (oldBitmap_) SelectObject(buffer_,oldBitmap_); DeleteDC(buffer_); buffer_=nullptr; }
    if (bitmap_) { DeleteObject(bitmap_); bitmap_=nullptr; oldBitmap_=nullptr; }
    for (HFONT f : {title_,body_,small_,numberFont_}) if (f) DeleteObject(f);
    title_=body_=small_=numberFont_=nullptr;
    if (module_) UnregisterClassA(editorClass,module_); // Fails harmlessly while another instance is open.
}
void Editor::refresh() noexcept {
    if (!window_) return;
    for (int i=0;i<ParamCount;++i) {
        const float value=plugin_.parameter(i);
        if (drawnValues_[i]!=value) { drawnValues_[i]=value; dirty_=true; }
    }
    const float latency=plugin_.latencyMs();
    if (drawnLatencyMs_!=latency) { drawnLatencyMs_=latency; dirty_=true; }
    const float sampleRate=plugin_.sampleRate();
    if (drawnSampleRate_!=sampleRate) { drawnSampleRate_=sampleRate; dirty_=true; }
    RECT meters{24,90,876,212};
    InvalidateRect(window_,dirty_?nullptr:&meters,FALSE);
}
RECT Editor::card(int index) const noexcept {
    int slot=-1;
    if (tab_==0) {
        for (int s=0;s<11;++s) if (order[s]==index) { slot=s; break; }
    } else {
        for (int s=0;s<10;++s) if (advancedOrder[s]==index) { slot=s; break; }
    }
    if (slot<0) return {0,0,0,0};
    const int x=24+(slot%3)*288, y=228+(slot/3)*106;
    return {x,y,x+276,y+96};
}
bool Editor::visible(int index) const noexcept {
    if (tab_==0) return index>=Target && index<=Bypass;
    return index>=RmsSoftest && index<=TruePeak;
}
int Editor::hit(int x,int y) const noexcept {
    if (tab_==0) {
        for (int i : order) if (inside(card(i),x,y)) return i;
        if (inside({616,582,728,620},x,y)) return Guard;
        if (inside({740,582,860,620},x,y)) return Bypass;
    } else {
        for (int i : advancedOrder) if (inside(card(i),x,y)) return i;
    }
    return -1;
}
void Editor::changeTab(int tab) {
    if (number_) finishNumber(true);
    if (dragging_>=0) { plugin_.endEdit(dragging_); dragging_=-1; ReleaseCapture(); }
    tab_=std::clamp(tab,0,1);
    if (!visible(focus_)) focus_=tab_==0?Target:RmsSoftest;
    dirty_=true; InvalidateRect(window_,nullptr,FALSE);
    for (HWND button : tabs_) if (button) InvalidateRect(button,nullptr,FALSE);
}
void Editor::advanceFocus(int direction) {
    int sequence[15]{mainTabFocus,advancedTabFocus};
    int count=2;
    if (tab_==0) {
        for (int i : order) sequence[count++]=i;
        sequence[count++]=Guard; sequence[count++]=Bypass;
    } else for (int i : advancedOrder) sequence[count++]=i;
    int current=focus_;
    if (GetFocus()==tabs_[0]) current=mainTabFocus;
    if (GetFocus()==tabs_[1]) current=advancedTabFocus;
    int position=0;
    for (int i=0;i<count;++i) if (sequence[i]==current) { position=i; break; }
    current=sequence[(position+direction+count)%count];
    if (current==mainTabFocus || current==advancedTabFocus) SetFocus(tabs_[current==mainTabFocus?0:1]);
    else { focus_=current; SetFocus(window_); }
    dirty_=true; InvalidateRect(window_,nullptr,FALSE);
}
void Editor::showPresets() {
    if (number_) finishNumber(true);
    HMENU menu=CreatePopupMenu();
    for (int p=0;p<PresetCount;++p) AppendMenuA(menu,MF_STRING|(p==plugin_.currentPreset()?MF_CHECKED:0),p+1,presetNames[p]);
    POINT point{647,65}; ClientToScreen(window_,&point);
    const int choice=int(TrackPopupMenu(menu,TPM_RETURNCMD|TPM_LEFTALIGN,point.x,point.y,0,window_,nullptr));
    DestroyMenu(menu); if (choice) { plugin_.choosePreset(choice-1,true); dirty_=true; }
    InvalidateRect(window_,nullptr,FALSE);
}
void Editor::text(HDC dc,int x,int y,int w,int h,const char* s,COLORREF c,HFONT f,UINT flags) {
    const auto old=SelectObject(dc,f); SetTextColor(dc,c); SetBkMode(dc,TRANSPARENT);
    RECT r{x,y,x+w,y+h}; DrawTextA(dc,s,-1,&r,flags); SelectObject(dc,old);
}
void Editor::panel(HDC dc,RECT r,COLORREF c,int radius) {
    HBRUSH b=CreateSolidBrush(c); auto oldB=SelectObject(dc,b); auto oldP=SelectObject(dc,GetStockObject(NULL_PEN));
    RoundRect(dc,r.left,r.top,r.right,r.bottom,radius,radius);
    SelectObject(dc,oldP); SelectObject(dc,oldB); DeleteObject(b);
}
void Editor::paint(HDC destination) {
    if (!buffer_) return;
    HDC dc=buffer_;
    if (dirty_) {
        fill(dc,{0,0,900,720},background);
        fill(dc,{0,0,900,5},motif);
        text(dc,24,19,330,40,"SteadyVoice",ink,title_);
        char latencyLabel[96];
        const float latency=plugin_.latencyMs();
        if (latency>0) std::snprintf(latencyLabel,sizeof(latencyLabel),"MIC LEVELER  /  ADDED LATENCY: %.3f ms",double(latency));
        else std::snprintf(latencyLabel,sizeof(latencyLabel),"MIC LEVELER  /  ZERO ADDED LATENCY");
        text(dc,25,59,560,22,latencyLabel,accent,small_);
        panel(dc,{647,25,876,65},surface);
        char presetLabel[96]; std::snprintf(presetLabel,sizeof(presetLabel),"Preset: %s   v",presetNames[plugin_.currentPreset()]);
        text(dc,662,26,202,38,presetLabel,ink,body_);
        for (int i=0;i<ParamCount;++i) {
            if (!visible(i) || isButton(i)) continue;
            const RECT r=card(i); panel(dc,r,surface);
            text(dc,r.left+16,r.top+10,140,23,params[i].name,ink,body_);
            char value[64]; display(i,plugin_.parameter(i),value,sizeof(value));
            text(dc,r.right-122,r.top+8,106,27,value,accent,body_,DT_RIGHT|DT_SINGLELINE|DT_VCENTER);
            const char* hint=i==Lookahead && plugin_.parameter(TruePeak)>0
                ? "4x / 8x modes use at least 5 ms" : params[i].hint;
            text(dc,r.left+16,r.top+36,244,20,hint,muted,small_);
            const int x=r.left+16, y=r.top+70, w=244;
            panel(dc,{x,y,x+w,y+5},track,5);
            int n=int(plugin_.parameter(i)*w);
            if (n>0) panel(dc,{x,y,x+n,y+5},motif,5);
            HBRUSH b=CreateSolidBrush(ink); const auto old=SelectObject(dc,b); const auto pen=SelectObject(dc,GetStockObject(NULL_PEN));
            Ellipse(dc,x+n-5,y-3,x+n+5,y+8); SelectObject(dc,pen); SelectObject(dc,old); DeleteObject(b);
            if (focus_==i && GetFocus()==window_) {
                RECT focus{r.left+3,r.top+3,r.right-3,r.bottom-3}; DrawFocusRect(dc,&focus);
            }
        }
        auto drawToggle=[&](int i,RECT button,const char* label) {
            const bool on=plugin_.parameter(i)>=.5f;
            panel(dc,button,on?motif:track,8);
            char caption[64]; std::snprintf(caption,sizeof(caption),"%s: %s",label,on?"On":"Off");
            text(dc,button.left,button.top,button.right-button.left,button.bottom-button.top,caption,ink,body_,DT_CENTER|DT_SINGLELINE|DT_VCENTER);
            if (focus_==i && GetFocus()==window_) { InflateRect(&button,-3,-3); DrawFocusRect(dc,&button); }
        };
        if (tab_==0) {
            panel(dc,{600,546,876,642},surface);
            text(dc,616,555,244,23,"Final stage",ink,body_);
            drawToggle(Guard,{616,582,728,620},"Guard");
            drawToggle(Bypass,{740,582,860,620},"Bypass");
        } else {
            for (int i : {Adaptive,AdaptiveGuard}) {
                const RECT r=card(i);
                panel(dc,r,surface);
                const char* heading=i==Adaptive ? "Loudness-dependent detector" : "Guard recovery";
                const char* hint=i==Adaptive ? "Off: use the original RMS detector" : "Smoother bass and plosive recovery";
                text(dc,r.left+16,r.top+10,244,23,heading,ink,body_);
                drawToggle(i,{r.left+16,r.top+36,r.right-16,r.top+74},params[i].name);
                text(dc,r.left+16,r.top+75,244,19,hint,muted,small_);
            }
            const RECT peak=card(TruePeak);
            panel(dc,peak,surface);
            text(dc,peak.left+16,peak.top+10,244,23,"Intersample peak protection",ink,body_);
            RECT button{peak.left+16,peak.top+36,peak.right-16,peak.top+74};
            panel(dc,button,plugin_.parameter(TruePeak)>0?motif:track,8);
            char mode[64],modeLabel[96]; display(TruePeak,plugin_.parameter(TruePeak),mode,sizeof(mode));
            std::snprintf(modeLabel,sizeof(modeLabel),"Peak detection: %s",mode);
            text(dc,button.left,button.top,button.right-button.left,button.bottom-button.top,modeLabel,ink,body_,DT_CENTER|DT_SINGLELINE|DT_VCENTER);
            if (focus_==TruePeak && GetFocus()==window_) { InflateRect(&button,-3,-3); DrawFocusRect(dc,&button); }
            text(dc,peak.left+16,peak.top+75,244,19,"Click / Space: Sample > 4x > 8x",muted,small_);
            panel(dc,{312,546,876,642},surface);
            const float target=physical(Target,plugin_.parameter(Target));
            const float half=physical(MedianRange,plugin_.parameter(MedianRange))*.5f;
            char band[160];
            std::snprintf(band,sizeof(band),"Median input band: %.1f to %.1f dBFS (Target +/- half the range)",double(target-half),double(target+half));
            text(dc,328,553,532,21,band,ink,small_);
            const float upper=target+half;
            const float highStep=upper<0?std::min(12.0f,-upper*.5f):12.0f;
            char anchors[160];
            std::snprintf(anchors,sizeof(anchors),"Anchors: Softest %.1f | Soft %.1f | Loud %.1f | Loudest %.1f dBFS",double(target-half-24),double(target-half-12),double(upper+highStep),double(upper+2*highStep));
            text(dc,328,576,532,20,anchors,muted,small_);
            text(dc,328,597,532,19,"APO editor may show its analysis rate; Windows sets the capture format.",muted,small_);
            text(dc,328,616,532,19,plugin_.parameter(TruePeak)>0
                ? "4x / 8x use at least 5 ms lookahead; total delay appears above."
                : "Begin with 5 ms lookahead and Guard on; total delay appears above.",muted,small_);
        }
        fill(dc,{24,660,876,661},track);
        text(dc,24,670,852,21,"Drag / scroll  |  Shift: fine  |  Click a value: type  |  Ctrl + click: reset  |  Ctrl + Tab: switch page",muted,small_);
        char footer[192],peakMode[32]; display(TruePeak,plugin_.parameter(TruePeak),peakMode,sizeof(peakMode));
        std::snprintf(footer,sizeof(footer),"v1.5.0  /  Instance rate: %.3g kHz  /  Linked stereo  /  Target and floor: detector RMS  /  Peak detection: %s",double(plugin_.sampleRate())*.001,peakMode);
        text(dc,24,691,852,19,footer,muted,small_);
        dirty_=false;
    }
    panel(dc,{24,90,876,212},surface);
    const auto m=plugin_.meterSnapshot();
    auto drawMeter=[&](int x,const char* label,float rms,float peak) {
        text(dc,x,101,120,25,label,ink,body_);
        char v[80]; std::snprintf(v,sizeof(v),"%+.1f dBFS",double(rms));
        text(dc,x+114,101,130,25,v,ink,body_,DT_RIGHT|DT_SINGLELINE|DT_VCENTER);
        panel(dc,{x,134,x+244,144},track,6);
        int n=int(std::clamp((rms+60)/60,0.0f,1.0f)*244);
        if(n>0) panel(dc,{x,134,x+n,144},rms>-9?accent:motif,6);
        int p=int(std::clamp((peak+60)/60,0.0f,1.0f)*244);
        fill(dc,{x+p-1,132,x+p+1,146},peak>=0?emphasis:ink);
        std::snprintf(v,sizeof(v),"Sample peak %+.1f dBFS",double(peak));
        text(dc,x,149,244,22,v,muted,small_);
    };
    drawMeter(40,"Input RMS",m.inputRms,m.inputPeak);
    drawMeter(334,"Output RMS",m.outputRms,m.outputPeak);
    text(dc,628,101,225,24,"Leveling gain",muted,body_);
    char gain[64]; std::snprintf(gain,sizeof(gain),"%+.1f dB",double(m.levelingGain));
    text(dc,628,126,225,32,gain,m.levelingGain>=0?accent:emphasis,numberFont_);
    std::snprintf(gain,sizeof(gain),"Peak guard: %.1f dB reduction",double(m.guardReduction));
    text(dc,628,157,225,22,gain,m.guardReduction>=3?emphasis:muted,small_);
    const bool bypass=plugin_.parameter(Bypass)>=.5f;
    text(dc,40,181,520,21,bypass?"BYPASSED":(m.speech?"VOICE ACTIVE":"PAUSE / NOISE FLOOR PROTECTION"),bypass?emphasis:(m.speech?accent:muted),small_);
    if(m.invalidSamples) text(dc,628,181,225,21,"Invalid input samples discarded",emphasis,small_);
    BitBlt(destination,0,0,900,720,dc,0,0,SRCCOPY);
}
void Editor::drag(int x) noexcept {
    if (dragging_<0) return;
    const RECT r=card(dragging_);
    plugin_.editParameter(dragging_,std::clamp(float(x-r.left-16)/244.0f,0.0f,1.0f));
    dirty_=true; InvalidateRect(window_,nullptr,FALSE);
}
void Editor::step(int index,int direction,bool fine) noexcept {
    if (index<0 || index>=ParamCount) return;
    plugin_.beginEdit(index);
    if(isToggle(index)) plugin_.editParameter(index,plugin_.parameter(index)>=.5f?0.0f:1.0f);
    else if(index==TruePeak) {
        const int mode=(int(physical(index,plugin_.parameter(index)))+direction+3)%3;
        plugin_.editParameter(index,normalized(index,float(mode)));
    }
    else {
        const float v=physical(index,plugin_.parameter(index)) + direction*params[index].step*(fine?.1f:1.0f);
        plugin_.editParameter(index,normalized(index,v));
    }
    plugin_.endEdit(index); dirty_=true; InvalidateRect(window_,nullptr,FALSE);
}
void Editor::editNumber(int i) {
    if (i<0 || i>=ParamCount || isButton(i) || !visible(i)) return;
    if (number_) finishNumber(true);
    const RECT r=card(i);
    char number[64]; display(i,plugin_.parameter(i),number,sizeof(number),false);
    editing_=i;
    number_=CreateWindowExA(0,"EDIT",number,WS_CHILD|WS_VISIBLE|ES_RIGHT|ES_AUTOHSCROLL,
                          r.right-122,r.top+8,106,28,window_,nullptr,module_,nullptr);
    if (!number_) { editing_=-1; return; }
    SendMessageA(number_,WM_SETFONT,reinterpret_cast<WPARAM>(body_),TRUE);
    SendMessageA(number_,EM_SETLIMITTEXT,24,0);
    SetWindowLongPtrA(number_,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(this));
    oldEditProc_=reinterpret_cast<WNDPROC>(SetWindowLongPtrA(number_,GWLP_WNDPROC,reinterpret_cast<LONG_PTR>(editProc)));
    SetFocus(number_); SendMessageA(number_,EM_SETSEL,0,-1);
}
void Editor::finishNumber(bool commit) noexcept {
    if (!number_ || finishing_) return;
    finishing_=true;
    const HWND n=number_;
    if (commit) {
        char buffer[64]; GetWindowTextA(n,buffer,sizeof(buffer));
        char* end=nullptr; const float v=std::strtof(buffer,&end);
        if (end!=buffer) {
            while (*end && std::isspace(static_cast<unsigned char>(*end))) ++end;
            if (!*end && std::isfinite(v)) {
                plugin_.beginEdit(editing_); plugin_.editParameter(editing_,normalized(editing_,v)); plugin_.endEdit(editing_);
            }
        }
    }
    number_=nullptr; editing_=-1;
    SetWindowLongPtrA(n,GWLP_WNDPROC,reinterpret_cast<LONG_PTR>(oldEditProc_));
    DestroyWindow(n); SetFocus(window_);
    finishing_=false; dirty_=true; InvalidateRect(window_,nullptr,FALSE);
}
LRESULT CALLBACK Editor::editProc(HWND h,UINT message,WPARAM w,LPARAM l) {
    auto* e=reinterpret_cast<Editor*>(GetWindowLongPtrA(h,GWLP_USERDATA));
    if (!e) return DefWindowProcA(h,message,w,l);
    if (message==WM_GETDLGCODE) return CallWindowProcA(e->oldEditProc_,h,message,w,l)|DLGC_WANTTAB;
    if (message==WM_KEYDOWN && (w==VK_RETURN || w==VK_ESCAPE)) { e->finishNumber(w==VK_RETURN); return 0; }
    if (message==WM_KEYDOWN && w==VK_TAB) {
        const bool control=(GetKeyState(VK_CONTROL)&0x8000)!=0;
        const bool backward=(GetKeyState(VK_SHIFT)&0x8000)!=0;
        e->finishNumber(true);
        if (control) e->changeTab(1-e->tab_);
        else e->advanceFocus(backward?-1:1);
        return 0;
    }
    if (message==WM_KILLFOCUS) { e->finishNumber(true); return 0; }
    return CallWindowProcA(e->oldEditProc_,h,message,w,l);
}
LRESULT CALLBACK Editor::tabProc(HWND h,UINT message,WPARAM w,LPARAM l) {
    auto* e=reinterpret_cast<Editor*>(GetWindowLongPtrA(h,GWLP_USERDATA));
    if (!e) return DefWindowProcA(h,message,w,l);
    if (message==WM_GETDLGCODE) return CallWindowProcA(e->oldTabProc_,h,message,w,l)|DLGC_WANTTAB;
    if (message==WM_KEYDOWN && w==VK_TAB) {
        if (GetKeyState(VK_CONTROL)&0x8000) e->changeTab(1-e->tab_);
        else e->advanceFocus((GetKeyState(VK_SHIFT)&0x8000)?-1:1);
        return 0;
    }
    if (message==WM_KEYDOWN && w==VK_RETURN) {
        e->changeTab(h==e->tabs_[0]?0:1); return 0;
    }
    return CallWindowProcA(e->oldTabProc_,h,message,w,l);
}
LRESULT CALLBACK Editor::windowProc(HWND h,UINT message,WPARAM w,LPARAM l) {
    Editor* e=reinterpret_cast<Editor*>(GetWindowLongPtrA(h,GWLP_USERDATA));
    if (message==WM_NCCREATE) {
        e=static_cast<Editor*>(reinterpret_cast<CREATESTRUCTA*>(l)->lpCreateParams);
        SetWindowLongPtrA(h,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(e)); e->window_=h;
    }
    if (!e) return DefWindowProcA(h,message,w,l);
    if (message==WM_NCDESTROY) { SetWindowLongPtrA(h,GWLP_USERDATA,0); e->window_=nullptr; return DefWindowProcA(h,message,w,l); }
    return e->handle(message,w,l);
}
LRESULT Editor::handle(UINT message,WPARAM w,LPARAM l) {
    switch(message) {
    case WM_ERASEBKGND: return 1;
    case WM_COMMAND:
        if (HIWORD(w)==BN_CLICKED) for (int i=0;i<2;++i) if (LOWORD(w)==tabIds[i]) { changeTab(i); return 0; }
        return 0;
    case WM_DRAWITEM: {
        const auto* item=reinterpret_cast<DRAWITEMSTRUCT*>(l);
        for (int i=0;i<2;++i) if (item && item->CtlID==static_cast<UINT>(tabIds[i])) {
            fill(item->hDC,item->rcItem,background);
            panel(item->hDC,item->rcItem,tab_==i?motif:track,8);
            const RECT r=item->rcItem;
            text(item->hDC,r.left,r.top,r.right-r.left,r.bottom-r.top,tabNames[i],ink,body_,DT_CENTER|DT_SINGLELINE|DT_VCENTER);
            if (item->itemState&ODS_FOCUS) { RECT focus=r; InflateRect(&focus,-3,-3); DrawFocusRect(item->hDC,&focus); }
            return TRUE;
        }
        return FALSE;
    }
    case WM_CTLCOLOREDIT: {
        HDC dc=reinterpret_cast<HDC>(w);
        SetTextColor(dc,ink); SetBkColor(dc,surface); SetDCBrushColor(dc,surface);
        return reinterpret_cast<LRESULT>(GetStockObject(DC_BRUSH));
    }
    case WM_PAINT: { PAINTSTRUCT p{}; HDC dc=BeginPaint(window_,&p); paint(dc); EndPaint(window_,&p); return 0; }
    case WM_TIMER: refresh(); return 0;
    case WM_SETFOCUS: case WM_KILLFOCUS: dirty_=true; InvalidateRect(window_,nullptr,FALSE); return 0;
    case WM_GETDLGCODE: return DLGC_WANTARROWS|DLGC_WANTTAB|DLGC_WANTCHARS;
    case WM_LBUTTONDOWN: {
        const int x=GET_X_LPARAM(l), y=GET_Y_LPARAM(l);
        if (inside({647,25,876,65},x,y)) {
            showPresets(); return 0;
        }
        const int i=hit(x,y);
        if (i<0) { SetFocus(window_); return 0; }
        focus_=i; dirty_=true;
        if (number_) finishNumber(true);
        SetFocus(window_);
        if (GetKeyState(VK_CONTROL)&0x8000) {
            plugin_.beginEdit(i); plugin_.editParameter(i,normalized(i,params[i].initial)); plugin_.endEdit(i);
        } else if(isButton(i)) step(i,1,false);
        else {
            const RECT r=card(i);
            if (x>=r.right-122 && y<r.top+36) { editNumber(i); return 0; }
            dragging_=i; plugin_.beginEdit(i); SetCapture(window_); drag(x);
        }
        InvalidateRect(window_,nullptr,FALSE); return 0;
    }
    case WM_LBUTTONDBLCLK:
        if (dragging_>=0) { plugin_.endEdit(dragging_); dragging_=-1; ReleaseCapture(); }
        editNumber(hit(GET_X_LPARAM(l),GET_Y_LPARAM(l))); return 0;
    case WM_MOUSEMOVE: if (dragging_>=0) drag(GET_X_LPARAM(l)); return 0;
    case WM_LBUTTONUP:
        if(dragging_>=0) { plugin_.endEdit(dragging_); dragging_=-1; ReleaseCapture(); } return 0;
    case WM_CAPTURECHANGED:
        if(dragging_>=0) { plugin_.endEdit(dragging_); dragging_=-1; } return 0;
    case WM_MOUSEWHEEL: {
        POINT p{GET_X_LPARAM(l),GET_Y_LPARAM(l)}; ScreenToClient(window_,&p);
        const int i=hit(p.x,p.y); if(i>=0) { focus_=i; step(i,GET_WHEEL_DELTA_WPARAM(w)>0?1:-1,(GetKeyState(VK_SHIFT)&0x8000)!=0); }
        return 0;
    }
    case WM_KEYDOWN:
        if(w==VK_TAB) {
            if (GetKeyState(VK_CONTROL)&0x8000) changeTab(1-tab_);
            else advanceFocus((GetKeyState(VK_SHIFT)&0x8000)?-1:1);
            return 0;
        }
        if(w=='P') { showPresets(); return 0; }
        if(w==VK_LEFT || w==VK_DOWN || w==VK_RIGHT || w==VK_UP) { step(focus_,(w==VK_LEFT || w==VK_DOWN)?-1:1,(GetKeyState(VK_SHIFT)&0x8000)!=0); return 0; }
        if(w==VK_RETURN || w==VK_SPACE) { if(isButton(focus_)) step(focus_,1,false); else editNumber(focus_); return 0; }
        if(w==VK_HOME || w==VK_END) {
            plugin_.beginEdit(focus_); plugin_.editParameter(focus_,w==VK_HOME?0.0f:1.0f); plugin_.endEdit(focus_);
            dirty_=true; InvalidateRect(window_,nullptr,FALSE); return 0;
        }
        return 0;
    default: return DefWindowProcA(window_,message,w,l);
    }
}
}
#endif
