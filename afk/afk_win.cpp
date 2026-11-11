// afk_win.cpp -- see afk_win.h
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include "afk_win.h"
#include "sw_lock.h"
#include <mmsystem.h>

static std::atomic<bool> g_displayChanged{false};
static std::atomic<bool> g_displayOff{false};
// GUID_CONSOLE_DISPLAY_STATE {6FE69556-704A-47A0-8F24-C28D936FDA47}: spelled out so no import library has to define it.
static const GUID kGuidConsoleDisplayState={0x6fe69556,0x704a,0x47a0,{0x8f,0x24,0xc2,0x8d,0x93,0x6f,0xda,0x47}};
static std::atomic<int>  g_classState{0};          // 0 not registered yet, 1 registered
static std::atomic<bool> g_threadAware{false};     // some per-thread DPI awareness could be set

// ------------------------------------------------------------------------------------------------ DPI
typedef HANDLE (WINAPI *PFN_SetThreadDpiAwarenessContext)(HANDLE);
typedef HRESULT (WINAPI *PFN_GetDpiForMonitor)(HMONITOR,int,UINT*,UINT*);

void afkThreadDpiAware(){
  HMODULE u=GetModuleHandleW(L"user32.dll"); if(!u) return;
  PFN_SetThreadDpiAwarenessContext f=(PFN_SetThreadDpiAwarenessContext)GetProcAddress(u,"SetThreadDpiAwarenessContext");
  if(!f){ logLine(L"[AFK] this Windows has no per-thread DPI awareness -- the screen uses the (scaled) desktop size"); return; }
  const INT_PTR ctx[3]={-4,-3,-2};                  // per-monitor v2, per-monitor, system
  const wchar_t* nm[3]={L"per-monitor v2",L"per-monitor",L"system"};
  for(int i=0;i<3;i++) if(f((HANDLE)ctx[i])){ g_threadAware.store(true); logLine(std::wstring(L"[AFK] render thread DPI awareness: ")+nm[i]); return; }
  logLine(L"[AFK] could not set the render thread's DPI awareness -- the screen uses the (scaled) desktop size");
}

// ------------------------------------------------------------------------------------------------ monitors
static bool fillMonitor(HMONITOR mon,AfkMonitor& m){
  MONITORINFOEXW mi; ZeroMemory(&mi,sizeof(mi)); mi.cbSize=sizeof(mi);
  if(!mon||!GetMonitorInfoW(mon,(MONITORINFO*)&mi)) return false;
  m.rc=mi.rcMonitor; m.hz=60; m.dpiScale=1.0f;
  DEVMODEW dm; ZeroMemory(&dm,sizeof(dm)); dm.dmSize=sizeof(dm);
  if(EnumDisplaySettingsW(mi.szDevice,ENUM_CURRENT_SETTINGS,&dm)&&dm.dmDisplayFrequency>1&&dm.dmDisplayFrequency<1000) m.hz=(int)dm.dmDisplayFrequency;
  if(g_threadAware.load()){
    HMODULE sh=GetModuleHandleW(L"shcore.dll"); if(!sh) sh=LoadLibraryW(L"shcore.dll");
    PFN_GetDpiForMonitor f=sh?(PFN_GetDpiForMonitor)GetProcAddress(sh,"GetDpiForMonitor"):nullptr;
    UINT x=96,y=96; if(f&&SUCCEEDED(f(mon,0,&x,&y))&&x>=48&&x<=960) m.dpiScale=(float)x/96.0f; }
  return m.width()>0&&m.height()>0;
}
bool afkMonitorFromCursor(AfkMonitor& m){
  POINT p={0,0}; GetCursorPos(&p);
  if(fillMonitor(MonitorFromPoint(p,MONITOR_DEFAULTTOPRIMARY),m)) return true;
  return afkMonitorPrimary(m);
}
bool afkMonitorPrimary(AfkMonitor& m){
  POINT o={0,0};
  if(fillMonitor(MonitorFromPoint(o,MONITOR_DEFAULTTOPRIMARY),m)) return true;
  m.rc.left=0; m.rc.top=0; m.rc.right=GetSystemMetrics(SM_CXSCREEN); m.rc.bottom=GetSystemMetrics(SM_CYSCREEN); m.hz=60; m.dpiScale=1.0f;
  return m.width()>0&&m.height()>0;
}

// ------------------------------------------------------------------------------------------------ the window
static LRESULT CALLBACK ScreenProc(HWND h,UINT m,WPARAM w,LPARAM l){
  switch(m){
  case WM_ERASEBKGND: return 1;                                   // never flash a background colour: the back-end owns every pixel
  case WM_SETCURSOR:  SetCursor(nullptr); return TRUE;            // no mouse pointer on the screen
  case WM_PAINT:      { PAINTSTRUCT ps; BeginPaint(h,&ps); EndPaint(h,&ps); return 0; }   // validate only; the next frame repaints everything
  case WM_CLOSE:      return 0;                                   // the screen ends when the user does something, not on Alt+F4
  case WM_DISPLAYCHANGE: g_displayChanged.store(true); return 0;
  case WM_POWERBROADCAST:                                         // the monitor was switched off / on / dimmed (registered in afkCreateScreenWindow)
    if(w==PBT_POWERSETTINGCHANGE&&l){ const POWERBROADCAST_SETTING* ps=(const POWERBROADCAST_SETTING*)l;
      if(IsEqualGUID(ps->PowerSetting,kGuidConsoleDisplayState)&&ps->DataLength>=sizeof(DWORD)){ DWORD v=1; memcpy(&v,ps->Data,sizeof(v)); g_displayOff.store(v==0); } }
    return TRUE;
  case WM_QUERYENDSESSION: return TRUE;
  case WM_ENDSESSION: if(w) afkMarkerClear(); return 0;           // the system ends the process right after this: a shutdown is not a crash
  case WM_SYSCOMMAND: { UINT c=(UINT)(w&0xFFF0); if(c==SC_SCREENSAVE||c==SC_CLOSE) return 0; break; }   // no second (system) screensaver on top
  }
  return DefWindowProcW(h,m,w,l);
}
static bool registerClass(){
  if(g_classState.load()==1) return true;
  WNDCLASSEXW wc; ZeroMemory(&wc,sizeof(wc)); wc.cbSize=sizeof(wc); wc.lpfnWndProc=ScreenProc; wc.hInstance=g_hInst; wc.lpszClassName=CLS_AFKSCR;
  if(!RegisterClassExW(&wc) && GetLastError()!=ERROR_CLASS_ALREADY_EXISTS){ logLine(L"[AFK] RegisterClassEx failed, error "+std::to_wstring(GetLastError())); return false; }
  g_classState.store(1); return true;
}
// The registrations of the power-setting notification, so that a destroyed window gives its registration back.
struct PnEntry{ HWND h; HANDLE pn; };
static SwCs g_pnCs; static std::vector<PnEntry> g_pn;
typedef HANDLE (WINAPI *PFN_RegPower)(HANDLE,const GUID*,DWORD);
typedef BOOL   (WINAPI *PFN_UnregPower)(HANDLE);

HWND afkCreateScreenWindow(const RECT& rc){
  if(!registerClass()) return nullptr;
  HWND h=CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_TOPMOST,CLS_AFKSCR,L"Sentinel AFK",WS_POPUP,rc.left,rc.top,rc.right-rc.left,rc.bottom-rc.top,nullptr,nullptr,g_hInst,nullptr);
  if(!h){ logLine(L"[AFK] CreateWindowEx failed, error "+std::to_wstring(GetLastError())); return h; }
  HMODULE u=GetModuleHandleW(L"user32.dll");
  PFN_RegPower reg=u?(PFN_RegPower)(void*)GetProcAddress(u,"RegisterPowerSettingNotification"):nullptr;
  if(reg){ HANDLE pn=reg(h,&kGuidConsoleDisplayState,0/*DEVICE_NOTIFY_WINDOW_HANDLE*/);
    if(pn){ SwGuard g(g_pnCs); PnEntry e={h,pn}; g_pn.push_back(e); } }
  return h;
}
void afkDestroyScreenWindow(HWND h){
  if(!h) return;
  HANDLE pn=nullptr; { SwGuard g(g_pnCs); for(size_t i=0;i<g_pn.size();i++) if(g_pn[i].h==h){ pn=g_pn[i].pn; g_pn.erase(g_pn.begin()+(long)i); break; } }
  if(pn){ HMODULE u=GetModuleHandleW(L"user32.dll"); PFN_UnregPower un=u?(PFN_UnregPower)(void*)GetProcAddress(u,"UnregisterPowerSettingNotification"):nullptr; if(un) un(pn); }
  DestroyWindow(h);
}
void afkShowScreenWindow(HWND h,const RECT& rc){
  if(!h) return;
  SetWindowPos(h,HWND_TOPMOST,rc.left,rc.top,rc.right-rc.left,rc.bottom-rc.top,SWP_SHOWWINDOW|SWP_NOACTIVATE);
}
void afkHideScreenWindow(HWND h){ if(h) ShowWindow(h,SW_HIDE); }
void afkCoverWithBlack(HWND h){
  if(!h) return; HDC dc=GetDC(h); if(!dc) return;
  RECT rc; GetClientRect(h,&rc); FillRect(dc,&rc,(HBRUSH)GetStockObject(BLACK_BRUSH)); ReleaseDC(h,dc);
}
// A background process may not simply take the foreground. Attaching this thread's input queue to the foreground thread's makes
// Windows treat the request as coming from it; SwitchToThisWindow (what Alt+Tab uses) is the second try. (No synthetic input is sent: that
// would reset the idle timer that ends the screen.) The result is checked: without focus the first key press would also reach the app underneath.
bool afkForceForeground(HWND h){
  if(!h) return false;
  HWND fg=GetForegroundWindow(); DWORD me=GetCurrentThreadId(), ft=fg?GetWindowThreadProcessId(fg,nullptr):0; bool att=false;
  if(ft&&ft!=me) att=AttachThreadInput(me,ft,TRUE)!=FALSE;
  BringWindowToTop(h); SetForegroundWindow(h); SetActiveWindow(h); SetFocus(h);
  if(att) AttachThreadInput(me,ft,FALSE);
  if(GetForegroundWindow()==h) return true;
  SwitchToThisWindow(h,TRUE);
  return GetForegroundWindow()==h;
}
bool afkTakeDisplayChanged(){ return g_displayChanged.exchange(false); }
bool afkDisplayOff(){ return g_displayOff.load(); }
bool afkInputDesktopLocked(){
  HDESK d=OpenInputDesktop(0,FALSE,DESKTOP_SWITCHDESKTOP);       // refused (access denied) while the lock screen / a UAC prompt owns the input desktop
  if(!d) return GetLastError()==ERROR_ACCESS_DENIED;              // any other failure says nothing about the desktop: not "locked"
  CloseDesktop(d); return false;
}
DWORD afkLastInputTick(){ LASTINPUTINFO li; li.cbSize=sizeof(li); li.dwTime=0; return GetLastInputInfo(&li)?li.dwTime:0; }
void afkPumpMessages(){ MSG m; while(PeekMessageW(&m,nullptr,0,0,PM_REMOVE)){ if(m.message==WM_QUIT) continue; TranslateMessage(&m); DispatchMessageW(&m); } }

// ------------------------------------------------------------------------------------------------ priority boost
void AfkBoost::on(){
  if(m_on) return; m_on=true;
  timeBeginPeriod(1);
  HANDLE th=GetCurrentThread(); m_prio=GetThreadPriority(th); if(m_prio==THREAD_PRIORITY_ERROR_RETURN) m_prio=THREAD_PRIORITY_NORMAL;
  SetThreadPriority(th,THREAD_PRIORITY_ABOVE_NORMAL);
  m_avrt=LoadLibraryW(L"avrt.dll");
  if(m_avrt){ typedef HANDLE (WINAPI *PFN)(LPCWSTR,LPDWORD); PFN f=(PFN)GetProcAddress(m_avrt,"AvSetMmThreadCharacteristicsW");
    if(f){ DWORD idx=0; m_mm=f(L"Playback",&idx); } }
}
void AfkBoost::off(){
  if(!m_on) return; m_on=false;
  if(m_mm&&m_avrt){ typedef BOOL (WINAPI *PFN)(HANDLE); PFN f=(PFN)GetProcAddress(m_avrt,"AvRevertMmThreadCharacteristics"); if(f) f(m_mm); }
  m_mm=nullptr; if(m_avrt){ FreeLibrary(m_avrt); m_avrt=nullptr; }
  SetThreadPriority(GetCurrentThread(),m_prio);
  timeEndPeriod(1);
}

// ------------------------------------------------------------------------------------------------ crash marker
static std::wstring markerPath(){ return appDir()+L"\\afk_active.flag"; }
// Content: "<tier> <build id>", e.g. "1 2026.10.17-gpu1".
void afkMarkerWrite(int tier){
  HANDLE f=CreateFileW(markerPath().c_str(),GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_DELETE,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
  if(f==INVALID_HANDLE_VALUE) return;
  std::string t(1,(char)('0'+(tier<0?0:(tier>9?9:tier)))); t+=' '; t+=SW_BUILD_ID;
  DWORD n=0; WriteFile(f,t.data(),(DWORD)t.size(),&n,nullptr); CloseHandle(f);
}
void afkMarkerClear(){ DeleteFileW(markerPath().c_str()); }
int afkMarkerRead(){
  HANDLE f=CreateFileW(markerPath().c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
  if(f==INVALID_HANDLE_VALUE) return -1;
  char b[96]={0}; DWORD n=0; ReadFile(f,b,sizeof(b)-1,&n,nullptr); CloseHandle(f);
  if(n<1||b[0]<'0'||b[0]>'2') return 0;             // present but unreadable: it was written by a GPU tier, so count it as the top one
  if(std::string(b+1,n-1)!=std::string(" ")+SW_BUILD_ID) return -1;   // another build's crash (or the old format): it says nothing about this build
  return b[0]-'0';
}
