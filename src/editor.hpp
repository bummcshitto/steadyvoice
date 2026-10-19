#pragma once
#include "plugin.hpp"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace sv {
class Editor {
public:
    explicit Editor(Plugin& plugin) noexcept : plugin_(plugin) {}
    ~Editor();
    bool open(void* parent);
    void close() noexcept;
    void refresh() noexcept;
private:
    static LRESULT CALLBACK windowProc(HWND,UINT,WPARAM,LPARAM);
    static LRESULT CALLBACK editProc(HWND,UINT,WPARAM,LPARAM);
    static LRESULT CALLBACK tabProc(HWND,UINT,WPARAM,LPARAM);
    LRESULT handle(UINT,WPARAM,LPARAM);
    void paint(HDC);
    void text(HDC,int,int,int,int,const char*,COLORREF,HFONT,UINT=DT_LEFT|DT_SINGLELINE|DT_VCENTER);
    void panel(HDC,RECT,COLORREF,int=10);
    RECT card(int index) const noexcept;
    int hit(int x,int y) const noexcept;
    bool visible(int index) const noexcept;
    void changeTab(int tab);
    void advanceFocus(int direction);
    void showPresets();
    void drag(int x) noexcept;
    void step(int index,int direction,bool fine) noexcept;
    void editNumber(int index);
    void finishNumber(bool commit) noexcept;
    Plugin& plugin_;
    HWND window_=nullptr, number_=nullptr, tabs_[2]{nullptr,nullptr};
    HINSTANCE module_=nullptr;
    WNDPROC oldEditProc_=nullptr, oldTabProc_=nullptr;
    HDC buffer_=nullptr;
    HBITMAP bitmap_=nullptr, oldBitmap_=nullptr;
    HFONT title_=nullptr, body_=nullptr, small_=nullptr, numberFont_=nullptr;
    int focus_=0, dragging_=-1, editing_=-1, tab_=0;
    bool finishing_=false;
    bool dirty_=true;
    float drawnLatencyMs_=-1;
    float drawnSampleRate_=-1;
    Values drawnValues_{};
};
}
#else
namespace sv {
class Editor {
public:
    explicit Editor(Plugin&) {}
    bool open(void*) { return false; }
    void close() noexcept {}
    void refresh() noexcept {}
};
}
#endif
