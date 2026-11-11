// main.cpp -- process entry point: hidden message window (tray icon, timers), watcher start-up, message loop.
#include "sw_common.h"
#include "settings.h"
#include "events.h"
#include "watchers.h"
#include "gfx.h"
#include "toast.h"
#include "dashboard.h"
#include "content.h"
#include "afk_screen.h"
#include "autostart.h"
#include <shellapi.h>
#include <fstream>
#include <cstdlib>

#ifdef SW_TRACE
static void TR(const char* m){ HANDLE f=CreateFileA("C:\\sw_trace.log",FILE_APPEND_DATA,FILE_SHARE_READ|FILE_SHARE_WRITE,0,OPEN_ALWAYS,0,0);
  if(f!=INVALID_HANDLE_VALUE){ DWORD n; WriteFile(f,m,(DWORD)strlen(m),&n,0); WriteFile(f,"\r\n",2,&n,0); CloseHandle(f);} }
#else
#define TR(x) ((void)0)
#endif
static NOTIFYICONDATAW g_nid={};
static ULONG_PTR g_gdip=0;
static HICON g_moon=nullptr;
static UINT g_afkTimerMs=1000;    // slow idle poll; quickens while the AFK screen is up
static HANDLE g_wmi=nullptr;
static const wchar_t* const CLS_END=L"SentinelEnd";

static HICON makeMoon(){
  Bitmap bmp(32,32,PixelFormat32bppPARGB); Graphics g(&bmp);
  g.SetSmoothingMode(SmoothingModeAntiAlias); g.Clear(Color(0,0,0,0));
  SolidBrush glow(Color(60,150,124,246)); g.FillEllipse(&glow,2,2,29,29);
  SolidBrush moon(Color(255,150,205,255)); g.FillEllipse(&moon,4,3,25,25);
  g.SetCompositingMode(CompositingModeSourceCopy);
  SolidBrush hole(Color(0,0,0,0)); g.FillEllipse(&hole,12,1,25,25);
  HICON ic=nullptr; bmp.GetHICON(&ic); return ic;
}

// A message-only window is not told when the session ends; a hidden top-level one is. Signing out, shutting down or restarting while the power-saving plan is on has to put
// the user's plan back first (the active plan outlives the session).
static LRESULT CALLBACK EndProc(HWND h,UINT m,WPARAM w,LPARAM l){
  if(m==WM_ENDSESSION&&w) ecoStop();
  return DefWindowProcW(h,m,w,l); }

static LRESULT CALLBACK MsgProc(HWND h,UINT m,WPARAM w,LPARAM l){
  switch(m){
  case WM_TRAY:
    if(LOWORD(l)==WM_LBUTTONDBLCLK) openDash();
    else if(LOWORD(l)==WM_RBUTTONUP||LOWORD(l)==WM_CONTEXTMENU){
      POINT p; GetCursorPos(&p); HMENU mn=CreatePopupMenu();
      AppendMenuW(mn,MF_STRING,ID_OPEN,L"Open dashboard");
      AppendMenuW(mn,MF_STRING,ID_SCAN,L"Quick scan");
      AppendMenuW(mn,MF_STRING|(g_tNotify.load()?MF_CHECKED:0),ID_NOTIFY,L"Notifications");
      AppendMenuW(mn,MF_SEPARATOR,0,0);
      AppendMenuW(mn,MF_STRING,ID_EXIT,L"Exit");
      SetForegroundWindow(h); TrackPopupMenu(mn,TPM_RIGHTBUTTON,p.x,p.y,0,h,0); DestroyMenu(mn);
    }
    return 0;
  case WM_COMMAND:
    if(LOWORD(w)==ID_OPEN) openDash();
    else if(LOWORD(w)==ID_SCAN){ doScan(true); openDash(); }
    else if(LOWORD(w)==ID_NOTIFY){ g_tNotify.store(!g_tNotify.load()); saveSettings(); }
    else if(LOWORD(w)==ID_EXIT) DestroyWindow(h);
    return 0;
  case WM_TIMER: if(w==TMR_AUTOSCAN) doScan(false);
    else if(w==TMR_AFK){
      updateQuoteState();                 // AFK detection + quote state machine; publishes to the AFK render thread
      afkScreenWatch();                   // supervisor: replaces an AFK thread that is stuck inside a GPU driver call
      ecoTick();                          // power saving after 30 min without input, undone at the first input (core/eco.cpp; looks at the clock itself: once a second)
      cpuReport();                        // once a minute: a [CPU] line in events.log if the app used 3 % or more
      // The render thread owns smoothness while the AFK screen is up, so this only has to keep the
      // quote state machine ticking (50 ms is plenty); otherwise a cheap 1 s idle poll.
      UINT want=(g_afkNow||afkScreenVisible())?50u:1000u;
      if(g_afkTimerMs!=want){ g_afkTimerMs=want; SetTimer(h,TMR_AFK,want,0); }
    }
    return 0;
  case WM_SHOWTOAST: toastDisplay((Toast*)l); return 0;
  case WM_DESTROY: Shell_NotifyIconW(NIM_DELETE,&g_nid); PostQuitMessage(0); return 0;
  }
  return DefWindowProcW(h,m,w,l);
}

int WINAPI wWinMain(HINSTANCE hi,HINSTANCE,PWSTR cmd,int){
  const bool background=cmd&&wcsstr(cmd,AUTOSTART_FLAG)!=nullptr;     // started by "Start with Windows": hidden in the tray, no dashboard
  g_hInst=hi; g_start=GetTickCount64(); srand((unsigned)g_start); cpuNameThread(L"ui");
  TR("before cacheDir();");
  cacheDir();                     // create %LOCALAPPDATA%\\SentinelWatch\\cache up front
  TR("before logRotate(); eventsInit();");
  logRotate(); eventsInit();
  TR("before CoInitializeEx(0,COINIT_APARTM");
  CoInitializeEx(0,COINIT_APARTMENTTHREADED);
  TR("before CoInitializeSecurity(");
  CoInitializeSecurity(0,-1,0,0,RPC_C_AUTHN_LEVEL_DEFAULT,RPC_C_IMP_LEVEL_IMPERSONATE,0,EOAC_NONE,0);
  TR("before GdiplusStartupInput gsi;");
  GdiplusStartupInput gsi; GdiplusStartup(&g_gdip,&gsi,0);
  TR("before gfxInit(); g_moon=makeMoon();");
  gfxInit(); g_moon=makeMoon();

  TR("before HANDLE mtx=CreateMutexW");
  HANDLE mtx=CreateMutexW(0,TRUE,L"Sentinel_Single");
  if(GetLastError()==ERROR_ALREADY_EXISTS){ if(mtx)CloseHandle(mtx);
    if(!background){ HWND other=FindWindowExW(HWND_MESSAGE,nullptr,CLS_MSG,nullptr); if(other) PostMessageW(other,WM_COMMAND,ID_OPEN,0); }   // started twice by hand: show the one that is running (it may be hidden in the tray)
    return 0; }

  TR("before WNDCLASSW wcm={}");
  WNDCLASSW wcm={}; wcm.lpfnWndProc=MsgProc; wcm.hInstance=hi; wcm.lpszClassName=CLS_MSG; wcm.hIcon=g_moon; RegisterClassW(&wcm);
  WNDCLASSW wcd={}; wcd.lpfnWndProc=DashProc; wcd.hInstance=hi; wcd.lpszClassName=CLS_DASH;
  wcd.hCursor=LoadCursorW(0,IDC_ARROW); wcd.hIcon=g_moon; RegisterClassW(&wcd);
  WNDCLASSW wco={}; wco.lpfnWndProc=OvlProc; wco.hInstance=hi; wco.lpszClassName=CLS_OVL;
  wco.hCursor=LoadCursorW(0,IDC_ARROW); RegisterClassW(&wco);
  WNDCLASSW wce={}; wce.lpfnWndProc=EndProc; wce.hInstance=hi; wce.lpszClassName=CLS_END; RegisterClassW(&wce);

  TR("before g_msg=CreateWindowExW");
  g_msg=CreateWindowExW(0,CLS_MSG,L"",0,0,0,0,0,HWND_MESSAGE,0,hi,0);
  TR("before g_ovl=CreateWindowExW");
  g_ovl=CreateWindowExW(WS_EX_LAYERED|WS_EX_TOPMOST|WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE,
    CLS_OVL,L"",WS_POPUP,0,0,400,150,0,0,hi,0);
  SetLayeredWindowAttributes(g_ovl,0,240,LWA_ALPHA);
  CreateWindowExW(WS_EX_TOOLWINDOW,CLS_END,L"",WS_POPUP,0,0,0,0,0,0,hi,0);       // never shown: it only hears WM_ENDSESSION (EndProc)
  SetTimer(g_msg,TMR_AFK,g_afkTimerMs,0);   // idle-poll timer; quickens on its own once AFK shows

  g_nid.cbSize=sizeof(g_nid); g_nid.hWnd=g_msg; g_nid.uID=1;
  g_nid.uFlags=NIF_ICON|NIF_MESSAGE|NIF_TIP; g_nid.uCallbackMessage=WM_TRAY;
  g_nid.hIcon=g_moon?g_moon:LoadIconW(0,IDI_APPLICATION);
  lstrcpynW(g_nid.szTip,L"Sentinel Watch Suite",ARRAYSIZE(g_nid.szTip));
  TR("before Shell_NotifyIconW(NIM_ADD");
  Shell_NotifyIconW(NIM_ADD,&g_nid);

  TR("before loadSettings(); saveSettings()");
  loadSettings(); saveSettings();   // persist toggle state as JSON in the app folder
  autostartApply();                 // the Run value follows the "Start with Windows" toggle
  ecoRecover();                     // a power-saving plan left active by a run that did not end cleanly (crash, kill, shutdown) is undone first
  logLine(std::wstring(L"[AFK] Sentinel ")+SW_VERSION_W+L" build "+SW_BUILD_ID_W+L" starting"+(background?L" (hidden in the tray)":L""));
  TR("before loadContent();");
  loadContent();                    // quotes / fun-facts config files
  TR("before dashInit(); ");
  dashInit();                       // avatar + host / cpu / ram / display for the Device Info card

  // baseline + watchers
  { std::ifstream f(wpath(baseFile())); std::string ln;
    while(std::getline(f,ln)){ if(!ln.empty()&&ln.back()=='\r')ln.pop_back();
      if(!ln.empty()) g_known.insert(fromUtf8(ln)); } }
  TR("before if(g_known.empty())");
  if(g_known.empty()) g_known=scanAutoruns();
  // Background monitors run BELOW_NORMAL so the AFK render thread (and everything else the user is
  // looking at) always wins the CPU.
  g_wmi=CreateThread(0,0,WmiThread,0,0,0); if(g_wmi)SetThreadPriority(g_wmi,THREAD_PRIORITY_BELOW_NORMAL);
  for(auto k:{envW(L"LOCALAPPDATA"),envW(L"APPDATA")}) if(!k.empty()){
    HANDLE t=CreateThread(0,0,DirThread,new std::wstring(k),0,0); if(t){ SetThreadPriority(t,THREAD_PRIORITY_BELOW_NORMAL); CloseHandle(t);} }
  { HANDLE tg=CreateThread(0,0,TokenGuardThread,0,0,0); if(tg){ SetThreadPriority(tg,THREAD_PRIORITY_BELOW_NORMAL); CloseHandle(tg);} }
  SetTimer(g_msg,TMR_AUTOSCAN,300000,0);
  { HANDLE pt=CreateThread(0,0,ProcScanThread,0,0,0); if(pt){ SetThreadPriority(pt,THREAD_PRIORITY_BELOW_NORMAL); CloseHandle(pt);} }

  TR("before afkScreenStart();");
  afkScreenStart();                 // AFK render thread: window, GPU/CPU back-ends, preload (all off the UI thread)
  TR("before openDash();");
  if(!background) openDash();       // show on a normal launch; a start with Windows stays in the tray (double-click the icon, or start the exe again, to open it)
#ifdef SW_TEST_HOOKS
  { wchar_t b[16]; if(GetEnvironmentVariableW(L"SW_TEST_DASH",b,16)){                 // SW_TEST_DASH=min|close: AFK runs with the dashboard minimized / closed to the tray
      if(!wcscmp(b,L"min")) ShowWindow(g_dash,SW_MINIMIZE); else if(!wcscmp(b,L"close")) DestroyWindow(g_dash);
      logLine(std::wstring(L"[TEST] dashboard ")+b+L": window exists "+std::to_wstring(g_dash&&IsWindow(g_dash))+L", minimized "+std::to_wstring(g_dash&&IsIconic(g_dash))); } }
#endif

  MSG msg;
  while(GetMessageW(&msg,0,0,0)){ TranslateMessage(&msg); DispatchMessageW(&msg); }

  g_running.store(false);
  ecoStop();                        // the user's power plan back, if power saving was on
  const bool afkClean=afkScreenStop();
  if(g_wmi){ WaitForSingleObject(g_wmi,2000); CloseHandle(g_wmi); }
  dashShutdown();
  if(afkClean){ gfxShutdown(); GdiplusShutdown(g_gdip); }   // an AFK thread that had to be left behind may still be drawing: the process exit cleans up after it instead
  if(mtx)CloseHandle(mtx);
  return 0;
}
