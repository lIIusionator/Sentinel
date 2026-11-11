// sentinel.cpp  -- Sentinel Watch Suite (native GDI+ control panel)
// ---------------------------------------------------------------------------
// A fully native Windows app (no HTML, no browser). It hosts three background
// watchers -- new processes (WMI event feed), autorun entries, and new files
// in AppData -- and draws its own dark HUD dashboard with GDI+.
//
//   * Render loop capped at 60 FPS, throttled to 5 FPS when minimized, and
//     fully stopped (headless, zero render cost) when closed to the tray.
//   * Notification toggle (on/off) gates the popup alerts.
//   * Quick Scan from the tray menu and the Protection tab.
//   * A cache folder under %LOCALAPPDATA%\SentinelWatch\cache stores the
//     decoded avatar bitmap so it is loaded once, not rebuilt per frame.
//
// It observes your own system only: it never hooks, reads, or modifies another
// process. Written from scratch; owes nothing to any other codebase.
//
// Build (MinGW):
//   x86_64-w64-mingw32-g++ -std=c++17 -O2 -mwindows -municode -static ^
//     sentinel.cpp -o sentinel.exe -lgdiplus -lshell32 -lgdi32 -luser32 ^
//     -ladvapi32 -lole32 -loleaut32 -lwbemuuid -luuid -lcrypt32
// Build (MSVC):
//   cl /std:c++17 /EHsc /O2 sentinel.cpp /link gdiplus.lib shell32.lib ^
//     gdi32.lib user32.lib advapi32.lib ole32.lib oleaut32.lib wbemuuid.lib ^
//     crypt32.lib

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define _WIN32_DCOM

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <wincrypt.h>
#include <wintrust.h>
#include <softpub.h>
#include <tlhelp32.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <d3d11.h>   // types only -- D3D11CreateDevice is resolved at runtime (afkEnsureD3DManager)
#include <d3d10.h>   // ID3D10Multithread -- the multithread-protection interface D3D11 devices also expose
#define SECURITY_WIN32
#include <secext.h>
#include <wbemidl.h>
#include <gdiplus.h>
#include <string>
#include <vector>
#include <deque>
#include <set>
#include <atomic>
#include <fstream>
#include <algorithm>
#include <cmath>
#include <iterator>
#include <cstdlib>
#include <audiopolicy.h>

// ---------------------------------------------------------------------------
// Embedded AFK screensaver clip. Inlined here so the whole program is one
// translation unit: sentinel.cpp -> sentinel.exe, no side files.
// Unpacked once into %LOCALAPPDATA%\SentinelWatch\content\afk\afk_bg.mp4
// by materializeAfkVideo() below.
// ---------------------------------------------------------------------------
// Clip: H.264 2560x1440 30fps + AAC 44.1kHz stereo, ~32.0s. Kept at native
// resolution -- a lower-res transcode was tried and reverted at the user's
// request: hardware capable of smooth 4K60 in a browser should be capable
// of this natively, and the real fix is getting this app's own hardware
// decode path working, not permanently trading away quality. See the video
// decode thread's comments for the GPU-decode approach and its safety
// design (a bounded-timeout probe, so a repeat of the driver-level hang
// seen during development can never freeze real playback again).
// Generated blob. Unpacked once into %%LOCALAPPDATA%%\SentinelWatch\content\afk\afk_bg.mp4
// Raw size: 41240486 bytes. Base64 size: 54987316 chars in 6874 chunks.

// [blob removed for working copy: AFK_VIDEO_RAW_BYTES / AFK_VIDEO_B64 / AFK_VIDEO_B64_COUNT]

#pragma comment(lib,"gdiplus.lib")
#pragma comment(lib,"wbemuuid.lib")
#pragma comment(lib,"crypt32.lib")
#pragma comment(lib,"wintrust.lib")
#pragma comment(lib,"mfplat.lib")
#pragma comment(lib,"mf.lib")
#pragma comment(lib,"mfreadwrite.lib")
#pragma comment(lib,"mfuuid.lib")
#pragma comment(lib,"secur32.lib")

using namespace Gdiplus;

// ---------- ids / constants ----------
static const wchar_t* CLS_MSG = L"SentinelMsg";
static const wchar_t* CLS_DASH= L"SentinelDash";
static const wchar_t* CLS_OVL = L"SentinelOvl";
static const wchar_t* CLS_AFKSCR = L"SentinelAfkScreen";
static const UINT WM_TRAY   = WM_APP+1;
static const UINT WM_PROCEVT = WM_APP+2;
static const UINT WM_SHOWTOAST = WM_APP+3;
static const UINT ID_OPEN=1, ID_SCAN=2, ID_NOTIFY=3, ID_EXIT=4;
static const UINT TMR_RENDER=10, TMR_AUTOSCAN=11, TMR_HIDEOVL=12, TMR_AFK=13;
static const int DASH_W=940, DASH_H=600;      // default / initial size
static int g_dashW=DASH_W, g_dashH=DASH_H;    // current size (resizable + persisted)
static const int DASH_MINW=780, DASH_MINH=560; // smallest the layout stays intact at
static const int SIDE_W=210, BAR_H=46;

// ---------- palette ----------
static const Color C_BG(255,10,14,26), C_BG2(255,14,18,34),
  C_PANEL(255,20,26,46), C_PANEL2(255,25,32,56), C_LINE(255,40,48,82),
  C_LINE2(255,54,66,108), C_VIOLET(255,120,196,255), C_VDIM(255,150,124,246),
  C_GREEN(255,92,230,164), C_RED(255,248,116,140),
  C_TX(255,231,234,244), C_TXD(255,140,150,178), C_TXM(255,92,100,134);

// ---------- globals ----------
static HINSTANCE g_hInst=nullptr;
static HWND g_msg=nullptr, g_dash=nullptr, g_ovl=nullptr, g_afkWin=nullptr;
// Backbuffer for the dedicated AFK screensaver window -- separate from
// g_dash's own g_back, since the two windows are independently sized and
// independently visible (g_afkWin can be up covering the whole monitor while
// g_dash is closed entirely, or a completely different size if it's open).
// DIB-section-backed backbuffer: GDI+ draws into it exactly as into any other Bitmap (it's the same
// wrap-an-existing-buffer constructor already used for decoded video frames elsewhere in this file),
// but presenting it is a raw BitBlt from the DIB section's own memory DC rather than a GDI+
// Graphics::DrawImage onto the window DC. For a same-size, same-format, no-transform copy -- which
// this always is -- BitBlt skips GDI+'s per-call setup entirely and is measurably cheaper at
// screen resolution; DrawImage still ends up doing the equivalent copy underneath, just through
// several more layers to get there.
static Bitmap* g_afkBack=nullptr; static int g_afkBw=0,g_afkBh=0;
static HBITMAP g_afkBackDib=nullptr; static HDC g_afkBackDC=nullptr; static HBITMAP g_afkBackOldBmp=nullptr;
static void afkEnsureBackbuffer(int W,int H){
  if(g_afkBack&&g_afkBw==W&&g_afkBh==H) return;
  if(g_afkBack){ delete g_afkBack; g_afkBack=nullptr; }
  if(g_afkBackDC){ if(g_afkBackOldBmp) SelectObject(g_afkBackDC,g_afkBackOldBmp); DeleteDC(g_afkBackDC); g_afkBackDC=nullptr; }
  if(g_afkBackDib){ DeleteObject(g_afkBackDib); g_afkBackDib=nullptr; }
  g_afkBw=W; g_afkBh=H; if(W<=0||H<=0) return;
  BITMAPINFO bi; ZeroMemory(&bi,sizeof(bi)); bi.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
  bi.bmiHeader.biWidth=W; bi.bmiHeader.biHeight=-H;           // negative: top-down, matching every other buffer in this file
  bi.bmiHeader.biPlanes=1; bi.bmiHeader.biBitCount=32; bi.bmiHeader.biCompression=BI_RGB;
  void* bits=nullptr; HDC screenDC=GetDC(nullptr);
  g_afkBackDib=CreateDIBSection(screenDC,&bi,DIB_RGB_COLORS,&bits,nullptr,0);
  ReleaseDC(nullptr,screenDC);
  if(!g_afkBackDib||!bits){ if(g_afkBackDib){DeleteObject(g_afkBackDib);g_afkBackDib=nullptr;} return; }
  g_afkBackDC=CreateCompatibleDC(nullptr);
  if(!g_afkBackDC){ DeleteObject(g_afkBackDib); g_afkBackDib=nullptr; return; }
  g_afkBackOldBmp=(HBITMAP)SelectObject(g_afkBackDC,g_afkBackDib);
  g_afkBack=new Bitmap(W,H,W*4,PixelFormat32bppPARGB,(BYTE*)bits);
}
static UINT g_afkTimerMs=1000;   // slow idle-poll rate; drops to 16ms only while actually showing
static NOTIFYICONDATAW g_nid={};
static ULONG_PTR g_gdip=0;
static std::atomic<bool> g_running{true};
static std::atomic<bool> g_tAutorun{true},g_tProc{true},g_tFile{true},
  g_tAllFiles{false},g_tNotify{true},g_tBeep{true},g_tIgnoreSys{true},g_tTokens{true};
static std::atomic<int> g_procCount{0}, g_fileCount{0}, g_tokenAlerts{0};
static std::set<std::wstring> g_known;
static ULONGLONG g_start=0;
static int g_tab=0;              // 0 dash 1 protect 2 events 3 credits
static bool g_splash=true; static ULONGLONG g_splashT0=0;
static HANDLE g_wmi=nullptr;
static Bitmap* g_avatar=nullptr;
static Bitmap* g_back=nullptr; static int g_bw=0,g_bh=0;

struct Toggle{ std::atomic<bool>* v; const char* key; const wchar_t* name; const wchar_t* desc; };
static const int TOG_N=8;
static Toggle g_toggles[TOG_N]={
  {&g_tAutorun ,"autorun",  L"Autorun watch",                  L"Run keys, Startup folders, Tasks"},
  {&g_tProc    ,"proc",     L"Live process watch",             L"Every new process via the WMI feed"},
  {&g_tFile    ,"file",     L"File watch",                     L"New files in AppData + folders"},
  {&g_tAllFiles,"allfiles", L"Watch all file types",           L"Not just executables (noisier)"},
  {&g_tTokens  ,"tokens",   L"Token Guard",                    L"Warn if unsigned apps read your logins/tokens"},
  {&g_tNotify  ,"notify",   L"Notifications",                  L"Pop a toast on suspicious activity"},
  {&g_tBeep    ,"beep",     L"Beep on suspicious",             L"Audible even behind a game"},
  {&g_tIgnoreSys,"ignoresys",L"Ignore signed system processes",L"Silences sppsvc.exe-style noise"},
};
static int g_splashMs=3000;
static std::atomic<bool> g_dirty{true};
static ULONGLONG g_secWin=0; static int g_secCnt=0;
static int g_mx=-1,g_my=-1;                 // mouse (hover)
static ULONGLONG g_tabT0=0;                 // tab-switch time (scramble)
static ULONGLONG g_flashT0=0; static RECT g_flashR={0,0,0,0}; // press flash
static RECT g_rScanTop={0,0,0,0};
static ULONGLONG g_sessionStart=0;
static std::vector<std::wstring> g_facts,g_quotes,g_afk,g_all;
static ULONGLONG g_qT0=0; static int g_qIdx=0;
static std::vector<std::wstring> g_normal,g_special;   // facts+quotes / afk-special
static std::wstring g_curQuote,g_pendingQuote;
// Type ladders for auto-fitting quotes. Quote length varies a lot, and the
// boxes they live in are fixed, so the text picks the largest step that fits
// rather than overflowing and getting clipped mid-line.
struct RichFont{ Font* n; Font* b; REAL lh; };
static std::vector<RichFont> g_ladderSide;   // sidebar panel, largest first
static std::vector<RichFont> g_ladderBig;    // full-screen AFK quote
static int g_fadePhase=2;                              // 0 stable, 1 fade-out, 2 fade-in
static UINT g_frameMs=33;                             // current render interval
static int g_quoteAlpha=255;                          // quote crossfade alpha
static std::vector<Bitmap*> g_afkFrames;              // looping AFK background frames
static ULONGLONG g_afkFrameT0=0; static int g_afkA=0; // overlay fade alpha
static Bitmap* g_videoFrame=nullptr; static CRITICAL_SECTION g_vcs;
static std::atomic<bool> g_videoRun{false}; static std::wstring g_videoPath;
static ULONGLONG g_fadeT0=0; static bool g_afkNow=false;
static ULONGLONG g_afkEnterT0=0;   // wall-clock when AFK started -- drives the staged reveal (stars, then video, then text)
static std::atomic<bool> g_afkAudio{true};      // AFK screensaver sound on/off
static float g_afkVol=1.0f;                      // 0.0 .. 1.0 playback volume
static HANDLE g_audThread=nullptr;
static std::wstring g_userName=L"Lunaris";
struct Part{ float x,y,vy; int a; };
static std::vector<Part> g_parts;
static std::vector<Part> g_stars;   // AFK placeholder starfield
static float g_creatorT=0; static RECT g_rCreator={0,0,0,0};   // Creator hover-zoom
static ULONGLONG g_quoteChangeT0=0; static int g_evScroll=0;
static RECT g_evRowR[40]; static std::wstring g_evRowPath[40]; static bool g_evRowWarn[40]={false}; static int g_evRowCount=0;
static RECT g_rFw={0,0,0,0};
static std::wstring g_devHost,g_devCPU,g_devRAM,g_devRes;      // device info
struct Note{ const wchar_t* ver; const wchar_t* kind; const wchar_t* text; };
static Note g_notes[]={
  {L"v1.6",L"FEATURE",L"Token Guard \u2014 warns when unsigned apps read your logins/tokens"},
  {L"v1.6",L"FEATURE",L"Resizable / scalable window (drag any edge or corner)"},
  {L"v1.6",L"FIX",    L"Sharper AFK screensaver video (high-quality bicubic upscaling)"},
  {L"v1.5",L"FEATURE",L"Blue/purple theme, moon icon, hover + press animations"},
  {L"v1.5",L"FEATURE",L"Scramble tab titles, quote & fun-fact engine w/ config files"},
  {L"v1.5",L"FIX",    L"Session timer format (no more lu:lu:lu)"},
  {L"v1.5",L"FIX",    L"Event text no longer bleeds past its card"},
  {L"v1.5",L"IMPROVE",L"Scan now lists already-running apps (Roblox, Explorer)"},
  {L"v1.5",L"IMPROVE",L"Signature-based suspicion; signed apps are not flagged"},
  {L"v1.4",L"FIX",    L"Fixed a render recursion that froze the app"},
  {L"v1.4",L"FIX",    L"AppData watch storm no longer hangs the UI"},
  {L"v1.3",L"FEATURE",L"Tabbed dashboard, protection toggles, JSON settings"},
  {L"v1.2",L"FEATURE",L"AppData file watch, auto-start, cache folder"},
  {L"v1.1",L"FEATURE",L"Live process watch via WMI event feed"},
  {L"v1.0",L"FEATURE",L"Autorun watch (Run keys, Startup, Tasks)"},
};
static HICON g_moon=nullptr;

struct Ev{ std::wstring tag,name,path,tm; };
static std::deque<Ev> g_events;
static CRITICAL_SECTION g_cs;
static std::wstring g_ovlTitle,g_ovlText; static Color g_ovlAccent=C_VIOLET;

// interactive hit rects (set during render, read on click)
static RECT g_rNav[4], g_rTog[TOG_N], g_rScan, g_rMin, g_rClose, g_rSoc[3];
// Fullscreen fills the WORK AREA (screen minus taskbar), not the whole
// monitor -- that's the explicit ask ("except filling out taskbar"). Distinct
// from the AFK screensaver's own full-monitor takeover further down, which
// deliberately does cover the taskbar.
static bool g_fullscreen=false; static RECT g_preFullRect={0,0,0,0};
static const wchar_t* g_socUrl[3]={
  L"https://www.youtube.com/@lIIusionator",
  L"https://www.roblox.com/users/10679736474/profile",
  L""}; // discord = copy

// fonts
static FontFamily *famUI=nullptr,*famMono=nullptr;
static Font *fTitle=nullptr,*fH=nullptr,*fLbl=nullptr,*fBody=nullptr,*fBodyB=nullptr,
  *fBig=nullptr,*fMono=nullptr,*fMonoBig=nullptr,*fQuote=nullptr,*fQuoteB=nullptr,*fClock=nullptr;

// ---------- helpers ----------
static std::wstring envW(const wchar_t* n){ wchar_t b[MAX_PATH];
  DWORD k=GetEnvironmentVariableW(n,b,MAX_PATH); return (k&&k<MAX_PATH)?std::wstring(b,k):L""; }
static std::string toU8(const std::wstring& w){ if(w.empty())return{};
  int n=WideCharToMultiByte(CP_UTF8,0,w.data(),(int)w.size(),0,0,0,0);
  std::string s(n,0); WideCharToMultiByte(CP_UTF8,0,w.data(),(int)w.size(),s.data(),n,0,0); return s; }
static std::wstring fromUtf8(const std::string& s){ if(s.empty())return{};
  int n=MultiByteToWideChar(CP_UTF8,0,s.data(),(int)s.size(),0,0); std::wstring w(n,0);
  MultiByteToWideChar(CP_UTF8,0,s.data(),(int)s.size(),w.data(),n); return w; }
static std::wstring lc(std::wstring s){ for(auto&c:s)c=(wchar_t)towlower(c); return s; }
static bool endsW(const std::wstring&s,const std::wstring&e){
  return s.size()>=e.size()&&lc(s.substr(s.size()-e.size()))==e; }

static std::wstring appDir(){ std::wstring d=envW(L"LOCALAPPDATA"); if(d.empty())d=envW(L"TEMP"); if(d.empty())d=L".";
  d+=L"\\SentinelWatch"; SHCreateDirectoryExW(0,d.c_str(),0); return d; }
static std::wstring cacheDir(){ std::wstring d=appDir()+L"\\cache"; SHCreateDirectoryExW(0,d.c_str(),0); return d; }
static std::wstring logPath(){ return appDir()+L"\\events.log"; }
static std::wstring baseFile(){ return appDir()+L"\\baseline.txt"; }
static std::wstring settingsPath(){ return appDir()+L"\\settings.json"; }
static void saveSettings(){
  std::string j="{\n";
  for(int i=0;i<TOG_N;i++){ j+="  \""; j+=g_toggles[i].key; j+="\": ";
    j+=(g_toggles[i].v->load()?"true":"false"); j+=",\n"; }
  j+="  \"splashMs\": "+std::to_string(g_splashMs)+",\n";
  j+="  \"dashW\": "+std::to_string(g_dashW)+",\n";
  j+="  \"dashH\": "+std::to_string(g_dashH)+"\n}\n";
  std::ofstream f(toU8(settingsPath()),std::ios::trunc|std::ios::binary); f.write(j.data(),j.size());
}
static void loadSettings(){
  std::ifstream f(toU8(settingsPath())); if(!f)return;
  std::string s((std::istreambuf_iterator<char>(f)),std::istreambuf_iterator<char>());
  for(int i=0;i<TOG_N;i++){ std::string key="\""+std::string(g_toggles[i].key)+"\"";
    size_t k=s.find(key); if(k==std::string::npos)continue;
    size_t c=s.find(':',k); if(c==std::string::npos)continue;
    size_t q=c+1; while(q<s.size()&&(s[q]==' '||s[q]=='\t'))q++;
    g_toggles[i].v->store(s.compare(q,4,"true")==0); }
  size_t k=s.find("\"splashMs\"");
  if(k!=std::string::npos){ size_t c=s.find(':',k);
    if(c!=std::string::npos){ int v=atoi(s.c_str()+c+1); if(v>=0&&v<=10000)g_splashMs=v; } }
  auto readInt=[&](const char* key,int lo,int hi,int& out){
    size_t kk=s.find(std::string("\"")+key+"\""); if(kk==std::string::npos)return;
    size_t cc=s.find(':',kk); if(cc==std::string::npos)return;
    int v=atoi(s.c_str()+cc+1); if(v>=lo&&v<=hi)out=v; };
  readInt("dashW",DASH_MINW,10000,g_dashW);
  readInt("dashH",DASH_MINH,10000,g_dashH);
}

static void logLine(const std::wstring& s){ SYSTEMTIME t; GetLocalTime(&t); wchar_t ts[64];
  wsprintfW(ts,L"[%02d:%02d:%02d] ",t.wHour,t.wMinute,t.wSecond);
  std::ofstream f(toU8(logPath()),std::ios::app|std::ios::binary);
  std::string u=toU8(std::wstring(ts)+s); f.write(u.data(),u.size()); f.put('\n'); }

static bool isExecExt(const std::wstring& p){
  static const wchar_t* ex[]={L".exe",L".dll",L".pyd",L".scr",L".bat",L".cmd",
    L".js",L".vbs",L".ps1",L".msi",L".com",L".lnk",L".jar"};
  for(auto e:ex) if(endsW(p,e))return true; return false; }
static bool suspPath(const std::wstring& p){ if(p.empty())return true; std::wstring l=lc(p);
  for(auto m:{L"\\appdata\\",L"\\temp\\",L"\\downloads\\",L"\\programdata\\",L"\\users\\public\\"})
    if(l.find(m)!=std::wstring::npos)return true; return false; }
static bool isSystemProc(const std::wstring& p){ std::wstring l=lc(p);
  std::wstring win=lc(envW(L"SystemRoot"));
  return (!win.empty()&&l.rfind(win,0)==0); }
static bool userWritable(const std::wstring& p){ if(p.empty())return false; std::wstring l=lc(p);
  for(auto m:{L"\\appdata\\",L"\\temp\\",L"\\downloads\\",L"\\programdata\\",L"\\users\\public\\"})
    if(l.find(m)!=std::wstring::npos)return true; return false; }
// Authenticode check: legit apps are signed; unsigned exes in user-writable
// spots are the real red flag. Verified offline (no revocation network call).
static bool isSigned(const std::wstring& path){
  if(path.empty())return false;
  WINTRUST_FILE_INFO fi; ZeroMemory(&fi,sizeof(fi)); fi.cbStruct=sizeof(fi); fi.pcwszFilePath=path.c_str();
  GUID gv=WINTRUST_ACTION_GENERIC_VERIFY_V2;
  WINTRUST_DATA wd; ZeroMemory(&wd,sizeof(wd)); wd.cbStruct=sizeof(wd);
  wd.dwUIChoice=WTD_UI_NONE; wd.fdwRevocationChecks=WTD_REVOKE_NONE;
  wd.dwUnionChoice=WTD_CHOICE_FILE; wd.pFile=&fi;
  wd.dwStateAction=WTD_STATEACTION_VERIFY; wd.dwProvFlags=WTD_CACHE_ONLY_URL_RETRIEVAL;
  LONG st=WinVerifyTrust((HWND)INVALID_HANDLE_VALUE,&gv,&wd);
  wd.dwStateAction=WTD_STATEACTION_CLOSE; WinVerifyTrust((HWND)INVALID_HANDLE_VALUE,&gv,&wd);
  return st==0;
}
// suspicious = runs from a user-writable location AND is not validly signed.
// Program Files / Windows, or anything properly signed, is treated as normal.
static bool suspiciousExe(const std::wstring& path){
  if(path.empty())return false;
  if(!userWritable(path))return false;
  return !isSigned(path);
}
// Filenames a token/credential stealer copies out of Discord / browsers. Seeing
// one of these appear somewhere it does not belong is a strong theft signal.
static bool isCredArtifactName(const std::wstring& p){
  std::wstring l=lc(p);
  for(auto m:{L"cookies",L"login data",L"web data",L"key4.db",L"key3.db",
              L"logins.json",L"places.sqlite",L".ldb",L"leveldb",L"local state"})
    if(l.find(m)!=std::wstring::npos)return true;
  return false;
}
// "staging" spots where an exfil copy of the above would land, but the real
// stores never live (so a credential file here is not just Discord/Chrome
// touching its own data).
static bool isStagingDir(const std::wstring& p){ std::wstring l=lc(p);
  for(auto m:{L"\\temp\\",L"\\downloads\\",L"\\desktop\\"})
    if(l.find(m)!=std::wstring::npos)return true; return false; }

// ---------- GDI+ typed wrappers (avoid int/REAL overload ambiguity) ----------
static void FRect(Graphics&g,const Brush*b,REAL x,REAL y,REAL w,REAL h){ g.FillRectangle(b,x,y,w,h); }
static void FEll(Graphics&g,const Brush*b,REAL x,REAL y,REAL w,REAL h){ g.FillEllipse(b,x,y,w,h); }
static void DEll(Graphics&g,const Pen*pn,REAL x,REAL y,REAL w,REAL h){ g.DrawEllipse(pn,x,y,w,h); }
static void DLine(Graphics&g,const Pen*pn,REAL x1,REAL y1,REAL x2,REAL y2){ g.DrawLine(pn,x1,y1,x2,y2); }
static void DImg(Graphics&g,Image*im,REAL x,REAL y,REAL w,REAL h){ g.DrawImage(im,x,y,w,h); }

// ---------- overlay toast (posted to UI thread; safe from any thread) ----------
struct Toast{ std::wstring title,text; Color accent; };
static void showOverlay(const std::wstring& title,const std::wstring& text,Color ac){
  if(!g_tNotify.load())return;
  PostMessageW(g_msg,WM_SHOWTOAST,0,(LPARAM)new Toast{title,text,ac});
}
static LRESULT CALLBACK OvlProc(HWND h,UINT m,WPARAM w,LPARAM l){
  switch(m){
  case WM_PAINT:{ PAINTSTRUCT ps; HDC dc=BeginPaint(h,&ps); RECT rc; GetClientRect(h,&rc);
    Graphics g(dc); g.SetSmoothingMode(SmoothingModeAntiAlias); g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
    SolidBrush bg(Color(255,24,26,32)); FRect(g,&bg,0,0,rc.right,rc.bottom);
    SolidBrush acb(g_ovlAccent); FRect(g,&acb,0,0,6,rc.bottom);
    SolidBrush t1(Color(255,240,240,245)),t2(Color(255,220,220,228));
    g.DrawString(g_ovlTitle.c_str(),-1,fH,PointF(20,12),&t1);
    RectF rb(20,44,(REAL)rc.right-34,(REAL)rc.bottom-54); StringFormat sf;
    g.DrawString(g_ovlText.c_str(),-1,fBody,rb,&sf,&t2);
    EndPaint(h,&ps); return 0; }
  case WM_TIMER: if(w==TMR_HIDEOVL){KillTimer(h,TMR_HIDEOVL);ShowWindow(h,SW_HIDE);} return 0;
  case WM_LBUTTONUP: KillTimer(h,TMR_HIDEOVL); ShowWindow(h,SW_HIDE); return 0;
  }
  return DefWindowProcW(h,m,w,l);
}

// ---------- shared event push ----------
static void pushEv(const wchar_t* tag,const std::wstring& name,const std::wstring& path){
  SYSTEMTIME t; GetLocalTime(&t); wchar_t tm[16]; wsprintfW(tm,L"%02d:%02d:%02d",t.wHour,t.wMinute,t.wSecond);
  bool drop=false;
  EnterCriticalSection(&g_cs);
  ULONGLONG nowms=GetTickCount64();
  if(nowms-g_secWin>=1000){ g_secWin=nowms; g_secCnt=0; }
  if(g_secCnt>=30) drop=true;                 // storm guard: cap 30 events/sec
  else { g_secCnt++; g_events.push_front({tag,name,path,tm}); if(g_events.size()>80) g_events.pop_back(); }
  LeaveCriticalSection(&g_cs);
  if(!drop){ g_dirty.store(true); logLine(std::wstring(tag)+L" "+name+L" | "+path); }
}
// bypasses the rate cap (used by an explicit running-process scan)
static void pushEvRaw(const wchar_t* tag,const std::wstring& name,const std::wstring& path){
  SYSTEMTIME t; GetLocalTime(&t); wchar_t tm[16]; wsprintfW(tm,L"%02d:%02d:%02d",t.wHour,t.wMinute,t.wSecond);
  EnterCriticalSection(&g_cs); g_events.push_front({tag,name,path,tm}); if(g_events.size()>80) g_events.pop_back(); LeaveCriticalSection(&g_cs);
  g_dirty.store(true);
}
// Enumerate CURRENTLY running processes (catches apps started before us:
// Roblox, Explorer, etc.), deduped by name. Runs on its own thread.
static DWORD WINAPI ProcScanThread(LPVOID){
  HANDLE snap=CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0);
  if(snap==INVALID_HANDLE_VALUE)return 0;
  PROCESSENTRY32W pe; pe.dwSize=sizeof(pe);
  std::set<std::wstring> seen; int count=0;
  if(Process32FirstW(snap,&pe)){ do{
    count++; std::wstring name=pe.szExeFile, key=lc(name);
    if(seen.count(key))continue; seen.insert(key);
    std::wstring path; HANDLE ph=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pe.th32ProcessID);
    if(ph){ wchar_t buf[MAX_PATH]; DWORD sz=MAX_PATH; if(QueryFullProcessImageNameW(ph,0,buf,&sz))path=buf; CloseHandle(ph); }
    bool susp=suspiciousExe(path);
    pushEvRaw(susp?L"WARN":L"PROC",name,path.empty()?L"(running)":path);
  }while(Process32NextW(snap,&pe)); }
  CloseHandle(snap); g_procCount.store(count); return 0;
}

// ---------- autorun ----------
static void enumReg(HKEY r,const wchar_t* sub,const wchar_t* lab,std::set<std::wstring>& o){
  HKEY h; if(RegOpenKeyExW(r,sub,0,KEY_READ,&h)!=ERROR_SUCCESS)return;
  wchar_t nm[16384]; BYTE d[16384];
  for(DWORD i=0;;++i){ DWORD nl=16384/2,dl=sizeof(d),ty=0;
    if(RegEnumValueW(h,i,nm,&nl,0,&ty,d,&dl)!=ERROR_SUCCESS)break;
    std::wstring v; if(ty==REG_SZ||ty==REG_EXPAND_SZ)v=(wchar_t*)d;
    o.insert(std::wstring(L"REG ")+lab+L" | "+nm+L" = "+v); }
  RegCloseKey(h);
}
static void enumDir(const std::wstring& p,const wchar_t* lab,std::set<std::wstring>& o){
  if(p.empty())return; WIN32_FIND_DATAW fd; HANDLE h=FindFirstFileW((p+L"\\*").c_str(),&fd);
  if(h==INVALID_HANDLE_VALUE)return;
  do{ std::wstring f=fd.cFileName; if(f==L"."||f==L".."||f==L"desktop.ini")continue;
    o.insert(std::wstring(L"DIR ")+lab+L" | "+f);}while(FindNextFileW(h,&fd)); FindClose(h);
}
static std::wstring kf(REFKNOWNFOLDERID id){ PWSTR p=0; std::wstring o;
  if(SUCCEEDED(SHGetKnownFolderPath(id,0,0,&p))&&p)o=p; if(p)CoTaskMemFree(p); return o; }
static std::set<std::wstring> scanAutoruns(){ std::set<std::wstring> s;
  enumReg(HKEY_CURRENT_USER,L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",L"HKCU Run",s);
  enumReg(HKEY_CURRENT_USER,L"Software\\Microsoft\\Windows\\CurrentVersion\\RunOnce",L"HKCU RunOnce",s);
  enumReg(HKEY_LOCAL_MACHINE,L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",L"HKLM Run",s);
  enumDir(kf(FOLDERID_Startup),L"Startup",s); enumDir(kf(FOLDERID_CommonStartup),L"Startup*",s);
  std::wstring sr=envW(L"SystemRoot"); if(!sr.empty())enumDir(sr+L"\\System32\\Tasks",L"Tasks",s);
  return s; }
static void doScan(bool announce){
  { HANDLE pt=CreateThread(0,0,ProcScanThread,0,0,0); if(pt){ SetThreadPriority(pt,THREAD_PRIORITY_BELOW_NORMAL); CloseHandle(pt);} }  // background scan burst -- yields CPU to the video thread
  if(!g_tAutorun.load()){ pushEv(L"SYS",L"scan skipped",L"autorun watch off"); return; }
  auto cur=scanAutoruns(); int added=0;
  for(auto&e:cur)if(!g_known.count(e)){ pushEv(L"WARN",L"new autorun",e); added++; }
  if(added&&announce) showOverlay(L"\u26A0 New startup entry",std::to_wstring(added)+L" new autorun entry(s)",C_RED);
  if(added){ g_known=cur; std::ofstream f(toU8(baseFile()),std::ios::trunc|std::ios::binary);
    for(auto&e:cur){ std::string u=toU8(e); f.write(u.data(),u.size()); f.put('\n'); } }
  if(!added) pushEv(L"SYS",L"manual scan",L"autoruns re-swept \u2014 no changes");
}

// ---------- file watch ----------
static DWORD WINAPI DirThread(LPVOID param){
  std::wstring dir=*(std::wstring*)param; delete (std::wstring*)param;
  HANDLE h=CreateFileW(dir.c_str(),FILE_LIST_DIRECTORY,
    FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,0,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS,0);
  if(h==INVALID_HANDLE_VALUE)return 0; std::vector<BYTE> buf(64*1024);
  while(g_running.load()){ DWORD ret=0;
    if(!ReadDirectoryChangesW(h,buf.data(),(DWORD)buf.size(),TRUE,
      FILE_NOTIFY_CHANGE_FILE_NAME|FILE_NOTIFY_CHANGE_CREATION,&ret,0,0)||!ret)continue;
    if(!g_tFile.load())continue;
    for(BYTE* p=buf.data();;){ auto* fni=(FILE_NOTIFY_INFORMATION*)p;
      if(fni->Action==FILE_ACTION_ADDED||fni->Action==FILE_ACTION_RENAMED_NEW_NAME){
        std::wstring rel(fni->FileName,fni->FileNameLength/2), full=dir+L"\\"+rel;
        bool exec=isExecExt(full);
        bool cred=g_tTokens.load()&&isCredArtifactName(full);
        if((g_tAllFiles.load()||exec||cred) && lc(full).find(L"\\sentinelwatch\\")==std::wstring::npos){ g_fileCount++;
          if(cred&&isStagingDir(full)){                 // a copy of a login/token store, dropped where it shouldn't be
            g_tokenAlerts.fetch_add(1);
            pushEv(L"WARN",L"token file staged: "+rel,full);
            showOverlay(L"\u26A0 Credential file copied",rel+L"\n"+full+L"\n(possible token-theft staging)",C_RED);
          } else if(!cred || exec){                      // fall back to normal exec handling
            bool susp=exec&&suspiciousExe(full);
            pushEv(susp?L"WARN":L"FILE",rel,full);
            if(susp) showOverlay(L"\u26A0 New unsigned executable",rel+L"\n"+full,C_RED);
          }
        }
      }
      if(!fni->NextEntryOffset)break; p+=fni->NextEntryOffset; }
  }
  CloseHandle(h); return 0;
}

// ---------- WMI process watch ----------
static std::wstring wStr(IWbemClassObject* o,const wchar_t* p){ VARIANT v; VariantInit(&v);
  std::wstring r; if(o->Get(p,0,&v,0,0)==WBEM_S_NO_ERROR&&v.vt==VT_BSTR&&v.bstrVal)r=v.bstrVal;
  VariantClear(&v); return r; }
static DWORD WINAPI WmiThread(LPVOID){
  if(FAILED(CoInitializeEx(0,COINIT_MULTITHREADED)))return 0;
  IWbemLocator* loc=0;
  if(FAILED(CoCreateInstance(CLSID_WbemLocator,0,CLSCTX_INPROC_SERVER,IID_IWbemLocator,(void**)&loc))){CoUninitialize();return 0;}
  IWbemServices* svc=0; BSTR ns=SysAllocString(L"ROOT\\CIMV2");
  HRESULT hr=loc->ConnectServer(ns,0,0,0,0,0,0,&svc); SysFreeString(ns);
  if(FAILED(hr)){loc->Release();CoUninitialize();return 0;}
  CoSetProxyBlanket(svc,RPC_C_AUTHN_WINNT,RPC_C_AUTHZ_NONE,0,RPC_C_AUTHN_LEVEL_CALL,RPC_C_IMP_LEVEL_IMPERSONATE,0,EOAC_NONE);
  IEnumWbemClassObject* en=0; BSTR lang=SysAllocString(L"WQL");
  BSTR q=SysAllocString(L"SELECT * FROM __InstanceCreationEvent WITHIN 1 WHERE TargetInstance ISA 'Win32_Process'");
  hr=svc->ExecNotificationQuery(lang,q,WBEM_FLAG_FORWARD_ONLY|WBEM_FLAG_RETURN_IMMEDIATELY,0,&en);
  SysFreeString(lang);SysFreeString(q);
  if(FAILED(hr)){svc->Release();loc->Release();CoUninitialize();return 0;}
  while(g_running.load()){ IWbemClassObject* ev=0; ULONG ret=0;
    hr=en->Next(1000,1,&ev,&ret); if(hr==WBEM_S_TIMEDOUT||!ret||!ev){ if(ev)ev->Release(); continue; }
    if(g_tProc.load()){ VARIANT vt; VariantInit(&vt);
      if(ev->Get(L"TargetInstance",0,&vt,0,0)==WBEM_S_NO_ERROR&&vt.vt==VT_UNKNOWN&&vt.punkVal){
        IWbemClassObject* ti=0; vt.punkVal->QueryInterface(IID_IWbemClassObject,(void**)&ti);
        if(ti){ std::wstring name=wStr(ti,L"Name"),path=wStr(ti,L"ExecutablePath");
          bool sys=isSystemProc(path);
          if(!(sys&&g_tIgnoreSys.load())){ g_procCount++;
            bool susp=suspiciousExe(path);
            std::wstring shown=path.empty()?L"(no path reported)":path;
            pushEv(susp?L"WARN":L"PROC",name,shown);
            if(susp) showOverlay(L"\u26A0 Unsigned app in user folder",name+L"\n"+shown,C_RED);
          }
          ti->Release();
        } }
      VariantClear(&vt); }
    ev->Release();
  }
  en->Release();svc->Release();loc->Release();CoUninitialize(); return 0;
}

// ---------- Token Guard: catch credential/token theft ----------
// Stealers read the small files where Discord and browsers keep your logged-in
// session tokens. This watcher looks for an UNSIGNED process running from a
// user-writable spot that currently holds an open handle to one of those
// stores -- i.e. reading your tokens right now. It only ever inspects processes
// already flagged suspicious, so when nothing shady is running it costs almost
// nothing. It never kills anything: it raises a WARN (which the Events tab can
// one-click firewall-block) and a toast. The staged-copy case is handled in
// DirThread above.
//
// SystemExtendedHandleInformation is not in the public headers, so the few
// shapes we need are declared here and NtQuerySystemInformation is resolved
// from ntdll at runtime.
typedef struct _SNT_HANDLE_ENTRY {
  PVOID Object; ULONG_PTR UniqueProcessId; ULONG_PTR HandleValue;
  ULONG GrantedAccess; USHORT CreatorBackTraceIndex; USHORT ObjectTypeIndex;
  ULONG HandleAttributes; ULONG Reserved;
} SNT_HANDLE_ENTRY;
typedef struct _SNT_HANDLE_INFO {
  ULONG_PTR NumberOfHandles; ULONG_PTR Reserved; SNT_HANDLE_ENTRY Handles[1];
} SNT_HANDLE_INFO;
typedef LONG (WINAPI* PFN_NTQSI)(ULONG,PVOID,ULONG,PULONG);
static const ULONG SNT_ExtendedHandleInformation=64;
static const LONG  SNT_STATUS_INFO_LEN_MISMATCH=(LONG)0xC0000004L;

static std::vector<std::wstring> g_credDirs;      // lowercased store dir prefixes
static void buildCredDirs(){
  if(!g_credDirs.empty())return;
  std::wstring ad=lc(envW(L"APPDATA")), la=lc(envW(L"LOCALAPPDATA"));
  auto add=[&](const std::wstring& p){ if(!p.empty())g_credDirs.push_back(p); };
  if(!ad.empty()){
    add(ad+L"\\discord\\local storage\\leveldb");
    add(ad+L"\\discordptb\\local storage\\leveldb");
    add(ad+L"\\discordcanary\\local storage\\leveldb");
    add(ad+L"\\opera software\\opera stable");
    add(ad+L"\\opera software\\opera gx stable");
    add(ad+L"\\mozilla\\firefox\\profiles");
  }
  if(!la.empty()){
    add(la+L"\\google\\chrome\\user data");
    add(la+L"\\google\\chrome beta\\user data");
    add(la+L"\\microsoft\\edge\\user data");
    add(la+L"\\bravesoftware\\brave-browser\\user data");
    add(la+L"\\vivaldi\\user data");
    add(la+L"\\yandex\\yandexbrowser\\user data");
  }
}
static std::wstring credLabel(const std::wstring& d){
  auto has=[&](const wchar_t* s){ return d.find(s)!=std::wstring::npos; };
  if(has(L"discord"))return L"Discord";
  if(has(L"chrome")) return L"Chrome";
  if(has(L"edge"))   return L"Edge";
  if(has(L"brave"))  return L"Brave";
  if(has(L"vivaldi"))return L"Vivaldi";
  if(has(L"yandex")) return L"Yandex";
  if(has(L"opera"))  return L"Opera";
  if(has(L"firefox"))return L"Firefox";
  return L"credential store";
}
static bool pathUnderCredDir(const std::wstring& lpath,std::wstring& storeOut){
  for(auto& d:g_credDirs) if(lpath.rfind(d,0)==0){ storeOut=d; return true; }
  return false;
}
// Duplicate a handle out of another process and resolve it to a DOS path.
static bool resolveHandlePath(HANDLE proc,ULONG_PTR hval,std::wstring& out){
  HANDLE dup=nullptr;
  if(!DuplicateHandle(proc,(HANDLE)hval,GetCurrentProcess(),&dup,0,FALSE,DUPLICATE_SAME_ACCESS)||!dup)return false;
  wchar_t buf[1024];
  DWORD n=GetFinalPathNameByHandleW(dup,buf,1023,FILE_NAME_NORMALIZED|VOLUME_NAME_DOS);
  CloseHandle(dup);
  if(n==0||n>=1024)return false;
  std::wstring p(buf,n);
  if(p.rfind(L"\\\\?\\",0)==0)p=p.substr(4);      // strip \\?\ prefix
  out=lc(p); return true;
}
static DWORD WINAPI TokenGuardThread(LPVOID){
  buildCredDirs();
  PFN_NTQSI NtQSI=(PFN_NTQSI)GetProcAddress(GetModuleHandleW(L"ntdll.dll"),"NtQuerySystemInformation");
  std::set<DWORD> alerted;                          // pids already warned about
  std::vector<BYTE> buf(1u<<20);
  while(g_running.load()){
    for(int i=0;i<40 && g_running.load();++i) Sleep(100);   // ~4s, but wakes fast on exit
    if(!g_running.load())break;
    if(!g_tTokens.load())continue;

    // 1) suspicious processes = unsigned + user-writable. Normally none.
    struct SP{ DWORD pid; std::wstring name,path; };
    std::vector<SP> susp; std::set<DWORD> alive;
    HANDLE snap=CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0);
    if(snap!=INVALID_HANDLE_VALUE){ PROCESSENTRY32W pe; pe.dwSize=sizeof(pe);
      if(Process32FirstW(snap,&pe)){ do{
        DWORD pid=pe.th32ProcessID; if(pid<=4)continue; alive.insert(pid);
        if(pid==GetCurrentProcessId())continue;
        HANDLE ph=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid);
        std::wstring path; if(ph){ wchar_t b[MAX_PATH]; DWORD sz=MAX_PATH;
          if(QueryFullProcessImageNameW(ph,0,b,&sz))path=b; CloseHandle(ph); }
        if(!path.empty() && suspiciousExe(path)) susp.push_back({pid,pe.szExeFile,path});
      }while(Process32NextW(snap,&pe)); }
      CloseHandle(snap);
    }
    for(auto it=alerted.begin();it!=alerted.end();){ if(!alive.count(*it)) it=alerted.erase(it); else ++it; }
    if(susp.empty()||!NtQSI)continue;

    // 2) one system-wide handle snapshot; a probe file lets us learn the File
    //    object-type index so we only try to resolve actual file handles.
    HANDLE probe=CreateFileW(settingsPath().c_str(),GENERIC_READ,
        FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,0,OPEN_EXISTING,0,0);
    if(probe==INVALID_HANDLE_VALUE) probe=CreateFileW(appDir().c_str(),0,
        FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,0,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS,0);
    ULONG need=0; LONG st=SNT_STATUS_INFO_LEN_MISMATCH;
    for(int tries=0;tries<6;tries++){
      st=NtQSI(SNT_ExtendedHandleInformation,buf.data(),(ULONG)buf.size(),&need);
      if(st==SNT_STATUS_INFO_LEN_MISMATCH||(need&&need>buf.size())){ buf.resize(need?need+65536:buf.size()*2); continue; }
      break;
    }
    if(st!=0){ if(probe!=INVALID_HANDLE_VALUE)CloseHandle(probe); continue; }
    auto* info=(SNT_HANDLE_INFO*)buf.data();
    ULONG_PTR cnt=info->NumberOfHandles;
    DWORD self=GetCurrentProcessId(); USHORT fileType=0xFFFF;
    if(probe!=INVALID_HANDLE_VALUE){
      for(ULONG_PTR i=0;i<cnt;i++){ auto& e=info->Handles[i];
        if((DWORD)e.UniqueProcessId==self && e.HandleValue==(ULONG_PTR)probe){ fileType=e.ObjectTypeIndex; break; } } }

    for(auto& sp:susp){
      if(alerted.count(sp.pid))continue;
      HANDLE tp=OpenProcess(PROCESS_DUP_HANDLE|PROCESS_QUERY_LIMITED_INFORMATION,FALSE,sp.pid);
      if(!tp)continue;                                  // usually means it is elevated; needs admin to inspect
      bool hit=false; std::wstring store;
      for(ULONG_PTR i=0;i<cnt && !hit;i++){ auto& e=info->Handles[i];
        if((DWORD)e.UniqueProcessId!=sp.pid)continue;
        if(fileType!=0xFFFF && e.ObjectTypeIndex!=fileType)continue;   // only file objects
        std::wstring p; if(!resolveHandlePath(tp,e.HandleValue,p))continue;
        if(pathUnderCredDir(p,store)) hit=true;
      }
      CloseHandle(tp);
      if(hit){
        alerted.insert(sp.pid); g_tokenAlerts.fetch_add(1);
        std::wstring lbl=credLabel(store);
        pushEv(L"WARN",L"token access ("+lbl+L"): "+sp.name,sp.path);
        showOverlay(L"\u26A0 Credential/token access",
          sp.name+L"\n"+sp.path+L"\nreading your "+lbl+L" login data",C_RED);
      }
    }
    if(probe!=INVALID_HANDLE_VALUE)CloseHandle(probe);
  }
  return 0;
}

// ---------- drawing helpers ----------
static void addRound(GraphicsPath& gp,REAL x,REAL y,REAL w,REAL h,REAL r){
  gp.AddArc(x,y,r*2,r*2,180,90); gp.AddArc(x+w-r*2,y,r*2,r*2,270,90);
  gp.AddArc(x+w-r*2,y+h-r*2,r*2,r*2,0,90); gp.AddArc(x,y+h-r*2,r*2,r*2,90,90); gp.CloseFigure(); }
static void fillRound(Graphics& g,REAL x,REAL y,REAL w,REAL h,REAL r,Color c){
  GraphicsPath gp; addRound(gp,x,y,w,h,r); SolidBrush b(c); g.FillPath(&b,&gp); }
static void strokeRound(Graphics& g,REAL x,REAL y,REAL w,REAL h,REAL r,Color c,REAL t=1){
  GraphicsPath gp; addRound(gp,x,y,w,h,r); Pen p(c,t); g.DrawPath(&p,&gp); }
static void drawStr(Graphics& g,const wchar_t* s,Font* f,Color c,REAL x,REAL y){
  SolidBrush b(c); g.DrawString(s,-1,f,PointF(x,y),&b); }
static void drawStrR(Graphics& g,const std::wstring& s,Font* f,Color c,REAL right,REAL y){
  SolidBrush b(c); RectF box; g.MeasureString(s.c_str(),-1,f,PointF(0,0),&box);
  g.DrawString(s.c_str(),-1,f,PointF(right-box.Width,y),&b); }
static bool inR(RECT r,int x,int y){ return x>=r.left&&x<r.right&&y>=r.top&&y<r.bottom; }
static bool hov(RECT r){ return inR(r,g_mx,g_my); }

static std::wstring scramble(const std::wstring& t,double el,double per){
  static const wchar_t* G=L"ABCDEFGHJKLMNPQRSTUVWXYZ0123456789#%&*<>/=+"; int gl=(int)wcslen(G);
  std::wstring o; for(size_t i=0;i<t.size();++i){ double rt=(i+1)*per;
    if(el>=rt||t[i]==L' ')o+=t[i]; else o+=G[rand()%gl]; } return o; }

// ---------- render ----------
static void card(Graphics& g,REAL x,REAL y,REAL w,REAL h){
  fillRound(g,x,y,w,h,12,C_PANEL); strokeRound(g,x,y,w,h,12,C_LINE);
  // corner brackets
  Pen p(C_LINE2,2);
  DLine(g,&p,x+8,y+8,x+20,y+8); DLine(g,&p,x+8,y+8,x+8,y+20);
  DLine(g,&p,x+w-8,y+h-8,x+w-20,y+h-8); DLine(g,&p,x+w-8,y+h-8,x+w-8,y+h-20);
}
static void label(Graphics& g,const wchar_t* s,REAL x,REAL y){
  SolidBrush v(C_VIOLET); FRect(g,&v,x,y+7,14,2);
  drawStr(g,s,fLbl,C_TXM,x+20,y); }

// ---------- rich text (**bold** emphasis, word-wrap) + content ----------
// Returns the height the wrapped text occupies. With measureOnly=true nothing
// is drawn, so the caller can size the type before committing to it.
static REAL drawRich(Graphics& g,std::wstring text,REAL x,REAL y,REAL maxW,Color c,double scrEl=-1,double scrPer=0.03,
                     Font* fnR=nullptr,Font* fnB=nullptr,REAL lh=27,bool measureOnly=false){
  size_t up; while((up=text.find(L"{user}"))!=std::wstring::npos) text.replace(up,6,g_userName);
  struct Run{ std::wstring s; bool b; }; std::vector<Run> runs; bool b=false; std::wstring cur;
  for(size_t i=0;i<text.size();){ if(text.compare(i,2,L"**")==0){ if(!cur.empty()){runs.push_back({cur,b});cur.clear();} b=!b; i+=2; } else cur+=text[i++]; }
  if(!cur.empty())runs.push_back({cur,b});
  static const wchar_t* GL=L"ABCDEFGHJKLMNPQRSTUVWXYZ0123456789#%&*<>/=+"; int gln=(int)wcslen(GL); int ci=0;
  SolidBrush br(c); REAL cxp=x,cyp=y;
  Font* FR=fnR?fnR:fBody; Font* FB=fnB?fnB:fBodyB;
  for(auto&r:runs){ Font* fn=r.b?FB:FR; std::wstring word;
    auto flush=[&](){ if(word.empty())return; RectF bb; g.MeasureString(word.c_str(),-1,fn,PointF(0,0),&bb);
      if(cxp+bb.Width>x+maxW){ cxp=x; cyp+=lh; }
      if(!measureOnly){
        REAL wy=cyp+(lh-bb.Height)/2;               // center taller emphasis words on the line
        g.DrawString(word.c_str(),-1,fn,PointF(cxp,wy),&br); }
      cxp+=bb.Width; word.clear(); };
    for(wchar_t ch:r.s){ wchar_t oc=ch;
        if(ch!=L' '){ if(scrEl>=0 && scrEl < (ci+1)*scrPer) oc=GL[rand()%gln]; ci++; }
        word+=oc; if(oc==L' ') flush(); } flush(); }
  return cyp+lh-y;
}
// Largest ladder step whose wrapped height fits maxH; falls back to the smallest.
static const RichFont* fitRich(Graphics& g,const std::wstring& text,REAL maxW,REAL maxH,
                               const std::vector<RichFont>& ladder){
  if(ladder.empty())return nullptr;
  for(auto&rf:ladder){
    if(drawRich(g,text,0,0,maxW,Color(0,0,0,0),-1,0.03,rf.n,rf.b,rf.lh,true)<=maxH) return &rf; }
  return &ladder.back();
}
static void writeIfMissing(const std::wstring& path,const std::vector<std::wstring>& lines){
  if(GetFileAttributesW(path.c_str())!=INVALID_FILE_ATTRIBUTES)return;
  std::ofstream f(toU8(path),std::ios::binary);
  for(auto&l:lines){ std::string u=toU8(l); f.write(u.data(),u.size()); f.put('\n'); }
}
static std::vector<std::wstring> readLines(const std::wstring& path){
  std::vector<std::wstring> v; std::ifstream f(toU8(path)); std::string ln;
  while(std::getline(f,ln)){ if(!ln.empty()&&ln.back()=='\r')ln.pop_back();
    if(!ln.empty()&&ln[0]!='#')v.push_back(fromUtf8(ln)); } return v;
}
static void loadContent(){
  std::wstring cd=appDir()+L"\\content"; SHCreateDirectoryExW(0,cd.c_str(),0);
  std::wstring afkDir=cd+L"\\afk"; SHCreateDirectoryExW(0,afkDir.c_str(),0);
  writeIfMissing(afkDir+L"\\README.txt", {
    L"SENTINEL - AFK screensaver background",
    L"",
    L"Drop a looping background here. It plays full-screen behind the quotes",
    L"while you are AFK, and loops forever.",
    L"",
    L"A clip is built into sentinel.exe and unpacks itself here as afk_bg.mp4",
    L"the first time you go AFK. Delete it and it comes back; replace it and",
    L"your copy is kept. Any other video in here outranks it.",
    L"",
    L"Supported: .mp4 / .mov / .mkv / .webm / .avi / .wmv (video + sound), or",
    L"an image sequence (a folder of frames).",
    L"  - Put numbered images here: frame_0001.png, frame_0002.png, ...",
    L"  - PNG / JPG / BMP accepted; they play in filename order at ~24 fps.",
    L"  - Up to 240 frames are loaded.",
    L"",
    L"A single image also works as a static background.",
    L"",
    L"Want to use an .mp4 directly? Tell me and I will add video playback",
    L"(Media Foundation) or convert your clip into a frame sequence."
  });
  std::vector<std::wstring> facts={
    L"Do you know Lunaris enjoys eating food, but having a **cold drink** nearby is easily the top tier preference?",
    L"Do you know Lunaris spends most of his time **alone**, just staying completely in his own space?",
    L"Do you know despite Lunaris having nearsighted vision, his eyes instantly spot any **tiny movement** in the room?",
    L"Do you know Lunaris picks up new skills **insanely fast** when he approaches them from a unique angle?",
    L"Do you know Lunaris loves **drawing** simply out of a deep appreciation for art itself?",
    L"Do you know Lunaris plays competitive games **solo**, even though his original goal was just having fun with friends?",
    L"Do you know Lunaris finds making **major life decisions** alone almost impossible without a clear guide?",
    L"Do you know Lunaris feels sharing too much personal stuff is like handing someone a **weapon** to hold against him?",
    L"Do you know Lunaris will favor his favorite people, until **raw logic** forces him to the opposite side?",
    L"Do you know Lunaris finds reorganizing code into its **simplest, cleanest** form deeply satisfying?",
    L"Do you know honey **never truly spoils**, and edible jars have been found in ancient Egyptian tombs?",
    L"Do you know sound travels about **four times faster** through water than through air?",
    L"Do you know a **day on Venus** is actually longer than an entire year on Venus?"};
  std::vector<std::wstring> quotes={
    L"Everything feels heavy right now, but I still like to think **tomorrow** might be a little quieter.",
    L"I don't know where I'm going, but **standing still** forever isn't an option either.",
    L"People can hurt you if you let them too close, but **isolation gets old**, so we keep trying.",
    L"It's hard to trust the process when you're lost in the noise, but the **clarity** always comes back.",
    L"My head is everywhere at once, but maybe that means **something good** is being built underneath.",
    L"The floor might feel unstable today, but I'm still **curious** how the story turns out.",
    L"And just like fine wine, our **memories** will only get better with time.",
    L"Keep **smiling**, even if life is hard.",
    L"Keep **moving**, so that you can't think about it."};
  std::vector<std::wstring> afk={
    L"You are you, only you can do it, because I know you. It will be fine; that is **Lunaris's guarantee**.",
    L"Itterasshai, {user}.",
    L"It is a cycle, one of a **destiny** that we cannot escape."};
  writeIfMissing(cd+L"\\facts.txt",facts);
  writeIfMissing(cd+L"\\quotes.txt",quotes);
  writeIfMissing(cd+L"\\afk.txt",afk);
  g_facts=readLines(cd+L"\\facts.txt"); g_quotes=readLines(cd+L"\\quotes.txt"); g_afk=readLines(cd+L"\\afk.txt");
  g_normal.clear(); for(auto&x:g_facts)g_normal.push_back(x); for(auto&x:g_quotes)g_normal.push_back(x);
  g_special=g_afk;
  if(!g_normal.empty()) g_curQuote=g_normal[rand()%(int)g_normal.size()];
  g_fadePhase=2; g_fadeT0=GetTickCount64();
  { wchar_t dn[256]; ULONG dl=256;
    if(GetUserNameExW(NameDisplay,dn,&dl)&&dl>0) g_userName=dn;
    else { wchar_t un[256]; DWORD us=256; if(GetUserNameW(un,&us)&&us>1) g_userName=un; } }
}

// desktop idle -> AFK after ~60s of no keyboard/mouse input
static bool isAfk(DWORD ms){ LASTINPUTINFO li; li.cbSize=sizeof(li);
  if(!GetLastInputInfo(&li))return false; return (GetTickCount()-li.dwTime)>=ms; }
// system idle timeout (screensaver setting) if one is enabled, else 5 minutes
static DWORD afkThresholdMs(){
  BOOL ssActive=FALSE; int ssTimeout=0;
  if(SystemParametersInfoW(SPI_GETSCREENSAVEACTIVE,0,&ssActive,0) && ssActive
     && SystemParametersInfoW(SPI_GETSCREENSAVETIMEOUT,0,&ssTimeout,0) && ssTimeout>0)
    return (DWORD)ssTimeout*1000;
  return 300000; // 5 minutes
}
static void pickPending(){
  std::vector<std::wstring>& pool = g_afkNow ? g_special : g_normal;
  if(pool.empty()){ g_pendingQuote.clear(); return; }
  int idx=rand()%(int)pool.size();
  if(pool.size()>1){ int guard=0; while(pool[idx]==g_curQuote && guard++<8) idx=rand()%(int)pool.size(); }
  g_pendingQuote=pool[idx];
}
static void triggerQuoteChange(){ pickPending(); g_fadePhase=1; g_fadeT0=GetTickCount64(); }

static void blockInFirewall(const std::wstring& path){
  if(path.empty()||path==L"(running)"||path==L"(no path reported)")return;
  std::wstring pr=L"/c netsh advfirewall firewall add rule name=\"Sentinel Block Out\" dir=out action=block program=\""+path+L"\" enable=yes"
                 L" & netsh advfirewall firewall add rule name=\"Sentinel Block In\" dir=in action=block program=\""+path+L"\" enable=yes";
  ShellExecuteW(0,L"runas",L"cmd.exe",pr.c_str(),0,SW_HIDE);   // UAC prompt -> adds real firewall rules
}
static void gatherDevice(){
  wchar_t cn[256]; DWORD cs=256; if(GetComputerNameW(cn,&cs)) g_devHost=cn;
  SYSTEM_INFO si; GetSystemInfo(&si); g_devCPU=std::to_wstring((int)si.dwNumberOfProcessors)+L" cores";
  MEMORYSTATUSEX ms; ms.dwLength=sizeof(ms);
  if(GlobalMemoryStatusEx(&ms)) g_devRAM=std::to_wstring((int)((ms.ullTotalPhys+536870912ull)/1073741824ull))+L" GB";
  g_devRes=std::to_wstring(GetSystemMetrics(SM_CXSCREEN))+L" x "+std::to_wstring(GetSystemMetrics(SM_CYSCREEN));
}
// ---------- AFK screensaver ----------
// The default clip ships inside the binary as the base64 blob near the top.
// It is unpacked once into content\afk -- the same folder findVideo() scans --
// so from the second launch on it is just another file sitting there and the
// blob is never touched again. Returns L"" if the decode or the write fails.
static const wchar_t* AFK_BUILTIN_NAME = L"afk_bg.mp4";
static std::wstring materializeAfkVideo(){
  static std::wstring cached; if(!cached.empty()) return cached;
  std::wstring dir=appDir()+L"\\content\\afk"; SHCreateDirectoryExW(0,dir.c_str(),0);
  std::wstring path=dir+L"\\"+AFK_BUILTIN_NAME;
  { // one-time sweep: older builds unpacked into the cache folder instead
    std::wstring stale=cacheDir()+L"\\afk_bg.mp4";
    if(GetFileAttributesW(stale.c_str())!=INVALID_FILE_ATTRIBUTES) DeleteFileW(stale.c_str());
  }
  bool ok=false;
  WIN32_FILE_ATTRIBUTE_DATA fa={};
  if(GetFileAttributesExW(path.c_str(),GetFileExInfoStandard,&fa)){
    ULONGLONG sz=((ULONGLONG)fa.nFileSizeHigh<<32)|fa.nFileSizeLow;
    ok = (sz==AFK_VIDEO_RAW_BYTES);            // size match -> already unpacked
  }
  if(!ok){
    logLine(L"[AFKVID] unpacking built-in clip -> "+path);
    std::string b64; b64.reserve((size_t)AFK_VIDEO_B64_COUNT*8000u+16u);
    for(int i=0;i<AFK_VIDEO_B64_COUNT;i++) b64.append(AFK_VIDEO_B64[i]);
    DWORD cb=0;
    if(!CryptStringToBinaryA(b64.c_str(),(DWORD)b64.size(),CRYPT_STRING_BASE64,0,&cb,0,0)){ logLine(L"[AFKVID] base64 size query failed, err="+std::to_wstring(GetLastError())); return L""; }
    std::vector<BYTE> bin(cb);
    if(!CryptStringToBinaryA(b64.c_str(),(DWORD)b64.size(),CRYPT_STRING_BASE64,bin.data(),&cb,0,0)){ logLine(L"[AFKVID] base64 decode failed, err="+std::to_wstring(GetLastError())); return L""; }
    std::ofstream f(toU8(path),std::ios::binary|std::ios::trunc);
    if(!f){ logLine(L"[AFKVID] could not open "+path+L" for writing"); return L""; }
    f.write((const char*)bin.data(),(std::streamsize)cb);
    f.close();
    if(!f){ logLine(L"[AFKVID] write to "+path+L" failed"); return L""; }
    logLine(L"[AFKVID] unpacked ok, "+std::to_wstring(cb)+L" bytes (expected "+std::to_wstring(AFK_VIDEO_RAW_BYTES)+L")");
  }
  cached=path; return cached;
}
static std::wstring findVideo(){
  std::wstring dir=appDir()+L"\\content\\afk";
  WIN32_FIND_DATAW fd; HANDLE h=FindFirstFileW((dir+L"\\*").c_str(),&fd);
  if(h==INVALID_HANDLE_VALUE){ std::wstring p=materializeAfkVideo(); logLine(L"[AFKVID] findVideo: content\\afk missing/unreadable, using "+(p.empty()?L"(none)":p)); return p; }
  std::wstring found;
  do{ std::wstring f=fd.cFileName,l=lc(f);
    if(l==lc(AFK_BUILTIN_NAME)) continue;   // our own unpacked clip -- anything else outranks it
    if(endsW(l,L".mp4")||endsW(l,L".mov")||endsW(l,L".mkv")||endsW(l,L".webm")||endsW(l,L".avi")||endsW(l,L".wmv")){ found=dir+L"\\"+f; break; }
  }while(FindNextFileW(h,&fd)); FindClose(h);
  if(found.empty()) found=materializeAfkVideo();   // nothing dropped in the folder -> use the built-in clip
  logLine(L"[AFKVID] findVideo -> "+(found.empty()?L"(none)":found));
  return found;
}
// ---------- shared clock ----------
// Published by AudioThread once its session is playing; read by VideoThread.
// Video paces its decode against this instead of its own GetTickCount64
// guess -- the SAME clock driving the audio renderer (a real
// IMFPresentationClock, ultimately tied to the sound card) -- so the two
// don't independently drift. A plain critical section, not an atomic,
// because releasing the old clock safely needs the read-and-clear to be one
// indivisible step.
static CRITICAL_SECTION g_clkcs; static bool g_clkcsInit=false;
static IMFClock* g_sessClock=nullptr;
static void publishClock(IMFClock* c){ if(c)c->AddRef();
  EnterCriticalSection(&g_clkcs); IMFClock* old=g_sessClock; g_sessClock=c; LeaveCriticalSection(&g_clkcs);
  if(old) old->Release(); }
static LONGLONG readClock(bool& ok){ ok=false; LONGLONG t=0;
  EnterCriticalSection(&g_clkcs); IMFClock* c=g_sessClock; if(c)c->AddRef(); LeaveCriticalSection(&g_clkcs);
  if(c){ MFTIME sysT=0; if(SUCCEEDED(c->GetCorrelatedTime(0,&t,&sysT))) ok=true; c->Release(); }
  return t; }
// Formats an HRESULT as 0xNNNNNNNN for the diagnostic log below -- callers
// can look the code up (e.g. "hresult 0xC00D36D5") without needing a
// debugger attached.
static std::wstring hrHex(HRESULT hr){ wchar_t b[16]; wsprintfW(b,L"0x%08X",(unsigned)hr); return b; }

// ---------- AFK video pipeline ----------
// Playback is CPU-composited (GDI+) rather than an EVR-hosted window: three earlier attempts at
// hardware-composited playback failed invisibly with no way to diagnose them off-machine, while this
// path is the one confirmed to show video, audio and overlay together.
//
// What actually limited smoothness on a 2560x1440 clip (found by reading the render path, not by
// guessing at the GPU): (1) the UI drew every frame with HighQualityBicubic + a colour matrix -- GDI+'s
// slowest path -- WHILE holding the lock the decoder needs to publish its next frame; (2) every decoded
// frame was converted by GDI+ (a 14.7MB alloc + per-pixel convert); (3) the vignette was a full-screen
// path-gradient fill per tick; (4) the "catch up" seek re-landed on the same keyframe forever. Fixed
// here: lock-free frame hand-off + pooling, an SSE2 copy, cheap opaque video draw, a cached vignette
// bitmap, and a private-clock pacer that slips instead of seeking (the picture can never freeze).
//
// Which Media Foundation configuration decodes fastest is machine-specific, so it is MEASURED at
// startup (DecodeBenchThread) and the winner is used; every result is written to events.log.
#include <atomic>
// >>> AFK_HELPERS_BEGIN  -- pure and platform-independent: also compiled natively by the unit tests
#include <emmintrin.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <vector>

// 32bpp XRGB/ARGB -> opaque premultiplied ARGB (alpha forced to 0xFF). srcTop is the visually-first
// row; strides are in bytes and may be negative (bottom-up DIB). Replaces a GDI+ DrawImage
// conversion of a whole frame, which cost far more than the copy itself.
static void afkCopyOpaque32(const uint8_t* srcTop,int32_t srcStride,uint8_t* dstTop,int32_t dstStride,uint32_t w,uint32_t h){
  const __m128i a=_mm_set1_epi32((int)0xFF000000u);
  for(uint32_t y=0;y<h;y++){
    const uint8_t* s=srcTop+(ptrdiff_t)y*srcStride; uint8_t* d=dstTop+(ptrdiff_t)y*dstStride;
    uint32_t x=0;
    for(;x+4<=w;x+=4){ __m128i v=_mm_loadu_si128((const __m128i*)(s+(size_t)x*4)); _mm_storeu_si128((__m128i*)(d+(size_t)x*4),_mm_or_si128(v,a)); }
    for(;x<w;x++){ uint32_t p; memcpy(&p,s+(size_t)x*4,4); p|=0xFF000000u; memcpy(d+(size_t)x*4,&p,4); }
  }
}

// True if a coarse grid of pixels shows real picture content (not all black / all identical).
static bool afkHasContent(const uint8_t* bgra,uint32_t w,uint32_t h){
  if(!w||!h) return false; uint32_t mn=255,mx=0;
  for(uint32_t gy=0;gy<16;gy++) for(uint32_t gx=0;gx<16;gx++){
    uint32_t x=(uint32_t)(((uint64_t)gx*(w-1))/15), y=(uint32_t)(((uint64_t)gy*(h-1))/15);
    const uint8_t* p=bgra+((size_t)y*w+x)*4; uint32_t l=(uint32_t)p[0]+p[1]+p[2]; if(l<mn)mn=l; if(l>mx)mx=l; }
  return mx>48 && mx>mn+8;
}

// ---- frame pacing ------------------------------------------------------------------------
// Private-clock pacer. Maps a frame's timestamp to a wall-clock due time. If frames run late it
// SLIPS the timeline (shows the frame now, re-anchors) instead of dropping or seeking, so the
// picture always keeps moving -- at decode speed if that is all the machine can manage. Deliberately
// NOT slaved to the audio clock: that coupling had nasty edge cases (stale clock at session start,
// audio loop vs video end-of-stream ordering) and ambient background video doesn't need lip-sync.
struct AfkPacer{
  static const int64_t SLIP=2000000, MAXEARLY=10000000;   // 200ms late -> slip; 1s early -> re-anchor
  bool init=false; int64_t tsBase=0,wallBase=0;
  void reset(){ init=false; }
  int64_t schedule(int64_t ts,int64_t wallNow,bool* slipped=nullptr){
    if(slipped)*slipped=false;
    if(!init){ init=true; tsBase=ts; wallBase=wallNow; return 0; }
    int64_t due=wallBase+(ts-tsBase), delta=due-wallNow;
    if(delta<-SLIP){ wallBase+=(wallNow-due); if(slipped)*slipped=true; return 0; }
    if(delta>MAXEARLY){ wallBase-=delta; return 0; }
    return delta>0?delta:0;
  }
};
// ---- decode-config selection ---------------------------------------------------------------
// Fastest measured config wins; among those within 10% of the fastest the EARLIEST wins (the
// table is ordered by preference). Returns -1 when nothing produced frames.
static int afkPickBest(const double* fps,const bool* ok,int n){
  double mx=0; for(int i=0;i<n;i++) if(ok[i]&&fps[i]>mx) mx=fps[i];
  if(mx<=0) return -1;
  for(int i=0;i<n;i++) if(ok[i]&&fps[i]>=mx*0.9) return i;
  return -1;
}

// ---- vignette ------------------------------------------------------------------------------
// 0 inside the focus circle, smoothstep up to 1 at outerR. Explicit maths instead of a GDI+
// path-gradient brush: cheaper (built once per resolution) and no dependence on GDI+'s reversed
// blend-position convention for path gradients.
static inline float afkVignette01(float r,float focusR,float outerR){
  if(r<=focusR) return 0.f; float t=(r-focusR)/(outerR-focusR); if(t>1.f)t=1.f; return t*t*(3.f-2.f*t);
}
// Fills W*H premultiplied-ARGB pixels: black with alpha = maxAlpha01 * profile.
static void afkVignetteFill(uint8_t* px,int32_t stride,int W,int H,float maxAlpha01){
  float cx=(W-1)*0.5f, cy=(H-1)*0.5f; float m=(float)(W<H?W:H);
  float focusR=m*0.30f, outerR=sqrtf(cx*cx+cy*cy)*1.06f;
  for(int y=0;y<H;y++){ uint32_t* row=(uint32_t*)(px+(ptrdiff_t)y*stride); float dy=y-cy;
    for(int x=0;x<W;x++){ float dx=x-cx; float p=afkVignette01(sqrtf(dx*dx+dy*dy),focusR,outerR);
      uint32_t a=(uint32_t)(maxAlpha01*p*255.f+0.5f); if(a>255)a=255; row[x]=a<<24; } }
}

// ---- cross-fade timing for sparse frames ("powerpoint slides" fix) ----
// At native frame rate the gap between published frames is small and every render tick already
// falls on-or-past the newest frame, so this returns 1.0 (opaque, zero extra blend cost) almost
// every time -- it only produces a fractional blend once decode is slow enough that a hard cut
// would actually be visible, which is exactly when smoothing is worth its cost. "No previous frame
// yet" is the CALLER's job to check (prev!=nullptr) before calling this -- gap math alone can't
// safely tell "no previous" apart from "previous published long ago".
static inline float afkBlendT(int64_t now,int64_t curT,int64_t prevT,int64_t minGapToBlend){
  int64_t gap=curT-prevT;
  if(gap<=minGapToBlend||gap<=0) return 1.0f;                 // native-rate path: no blending, no cost
  int64_t since=now-curT; if(since<=0) return 0.0f;           // just published: start of the cross-fade
  float t=(float)since/(float)gap; if(t>1.0f)t=1.0f; if(t<0.0f)t=0.0f; return t;
}

// ---- loop-restart resync (audio/video drift fix) ----
// Video paces itself off its own private clock (AfkPacer), deliberately not audio's, so a slow
// decoder can never freeze it -- but with nothing pulling it back, sustained heavy load lets video
// drift arbitrarily far behind audio's real-time position over many loops. Cheap, low-risk fix:
// each time video reaches its OWN natural end-of-stream (already seeking to restart the loop
// regardless), retarget that existing seek to audio's current position instead of always position
// zero. This costs nothing extra (the seek was happening anyway) and bounds the drift to at most
// one full clip traversal, resetting at every video loop, without any mid-playback seeking (so it
// can't repeat rapidly and thrash the way seeking-while-still-behind did in an earlier design).
static inline int64_t afkLoopRestartPos(bool haveAudioClk,int64_t audioPos,int64_t clipDurationHns){
  if(!haveAudioClk||clipDurationHns<=0) return 0;
  int64_t p=audioPos%clipDurationHns; if(p<0) p+=clipDurationHns; return p;
}

// ---- lock-free-for-the-reader frame hand-off -------------------------------------------------
// The decode thread publishes frames; the UI thread draws the two most recent (for cross-fading --
// see afkBlendT above) without ever holding the lock while drawing (drawing is slow, so the decoder
// must never be stalled by it). "In use" is tracked by pointer identity, not by curr/prev role: a
// frame handed to the UI keeps the same protection no matter which slot it occupies by the time the
// UI releases it. A published frame is only ever freed/pooled after nothing outstanding references
// it. Finished frames are pooled and reused, avoiding a large allocation (~15MB at 2560x1440 RGB32)
// per video frame.
template<class Frame,class Lock> struct AfkFrameExchange{
  Lock lk; Frame* curr=nullptr; Frame* prev=nullptr; int64_t currT=0,prevT=0;
  Frame* inUseA=nullptr; Frame* inUseB=nullptr; std::vector<Frame*> deferred; std::vector<Frame*> pool;
  static const size_t POOL_MAX=3;
  struct G{ Lock& l; G(Lock& x):l(x){l.lock();} ~G(){l.unlock();} };
  bool inUse(Frame* f){ return f&&(f==inUseA||f==inUseB); }
  void recycleLocked(Frame* f){ if(!f) return; if(pool.size()<POOL_MAX) pool.push_back(f); else delete f; }
  // Returns the current frame and, if one exists, the previous frame too, along with both publish
  // timestamps so the caller can compute the blend factor itself. Both pointers stay valid (never
  // freed/reused) until releaseAfterDraw().
  void acquireForDraw(Frame*& c,Frame*& p,int64_t& ct,int64_t& pt){ G g(lk); inUseA=curr; inUseB=prev; c=curr; p=prev; ct=currT; pt=prevT; }
  void releaseAfterDraw(){ G g(lk); inUseA=nullptr; inUseB=nullptr;
    for(size_t i=0;i<deferred.size();i++) recycleLocked(deferred[i]); deferred.clear(); }
  template<class Maker> Frame* obtain(uint32_t w,uint32_t h,Maker mk){
    Frame* f=nullptr;
    { G g(lk); for(size_t i=0;i<pool.size();){ Frame* c=pool[i]; pool.erase(pool.begin()+i);
        if((uint32_t)c->GetWidth()==w&&(uint32_t)c->GetHeight()==h){ f=c; break; } delete c; } }
    return f?f:mk(w,h);
  }
  // nowT: the decode thread's own monotonic clock (see afkHnsNow), used only for the cross-fade
  // factor, never compared against a clock from another source.
  void publish(Frame* nf,int64_t nowT=0){ G g(lk);
    Frame* retiring=prev;                 // about to fall out of the 2-deep window entirely
    prev=curr; prevT=currT; curr=nf; currT=nowT;
    if(retiring){ if(inUse(retiring)) deferred.push_back(retiring); else recycleLocked(retiring); }
  }
  void clear(){ G g(lk); Frame* a=curr; Frame* b=prev; curr=prev=nullptr; currT=prevT=0;
    if(a){ if(inUse(a)) deferred.push_back(a); else recycleLocked(a); }
    if(b && b!=a){ if(inUse(b)) deferred.push_back(b); else recycleLocked(b); } }
  void shutdown(){ G g(lk); if(curr){delete curr;curr=nullptr;} if(prev){delete prev;prev=nullptr;}
    for(size_t i=0;i<deferred.size();i++) delete deferred[i]; deferred.clear();
    for(size_t i=0;i<pool.size();i++) delete pool[i]; pool.clear(); }
};
// <<< AFK_HELPERS_END

struct AfkCsLock{ CRITICAL_SECTION cs; AfkCsLock(){ InitializeCriticalSection(&cs); } void lock(){ EnterCriticalSection(&cs); } void unlock(){ LeaveCriticalSection(&cs); } };
static AfkFrameExchange<Bitmap,AfkCsLock> g_vx;       // decoded frames: decode thread -> UI thread
// Stats, logged every ~5s while the screensaver is up.
static std::atomic<long long> g_stFrames{0}, g_stReadUs{0}, g_stCopyUs{0}, g_stSlips{0};   // decode thread
static long long g_rN=0, g_rTotUs=0, g_rVideoUs=0, g_rVigUs=0, g_rPresUs=0;                // UI thread only
static int g_afkTickWant=33;                                                              // ms between renders while visible

static inline int64_t afkHnsNow(){ static LARGE_INTEGER f={}; if(!f.QuadPart) QueryPerformanceFrequency(&f);
  LARGE_INTEGER c; QueryPerformanceCounter(&c);
  return (int64_t)(c.QuadPart/f.QuadPart)*10000000LL + (int64_t)((c.QuadPart%f.QuadPart)*10000000LL/f.QuadPart); }
static std::wstring afkF1(double v){ int x=(int)(v*10.0+0.5); return std::to_wstring(x/10)+L"."+std::to_wstring(x%10); }

// One decoded RGB32 frame -> a pooled GDI+ bitmap, opaque (alpha forced to 0xFF -- see
// afkCopyOpaque32). Deliberately non-premultiplied (ARGB, not PARGB): at alpha=255 the two formats
// are byte-identical (premultiplying by 1.0 is a no-op), so the opaque fast-draw path costs nothing
// extra either way -- but ARGB is what makes the cross-fade blend (afkBlendT + ImageAttributes alpha
// scaling below) come out correct; scaling alpha via a colour matrix on an already-premultiplied
// source double-applies the alpha to the colour channels. nullptr if the buffer is too small.
static Bitmap* afkMakeFrame(const BYTE* data,DWORD cl,LONG stride,UINT32 vw,UINT32 vh){
  DWORD need=(DWORD)((stride<0?-stride:stride))*(DWORD)vh; if(cl<need||!vw||!vh) return nullptr;
  Bitmap* bmp=g_vx.obtain(vw,vh,[](uint32_t w,uint32_t h){ return new Bitmap((INT)w,(INT)h,PixelFormat32bppARGB); });
  if(!bmp) return nullptr;
  if(bmp->GetLastStatus()!=Ok){ delete bmp; return nullptr; }
  BitmapData bd; Rect r(0,0,(INT)vw,(INT)vh);
  if(bmp->LockBits(&r,ImageLockModeWrite,PixelFormat32bppARGB,&bd)!=Ok){ delete bmp; return nullptr; }
  const BYTE* top=data; if(stride<0) top=data+(size_t)(vh-1)*(size_t)(-stride);
  afkCopyOpaque32(top,(int32_t)stride,(uint8_t*)bd.Scan0,(int32_t)bd.Stride,vw,vh);
  bmp->UnlockBits(&bd);
  return bmp;
}

// ---- cached vignette overlay (built once per resolution) ----
static Bitmap* g_vigBmp=nullptr; static int g_vigW=0,g_vigH=0;
static void afkDrawVignette(Graphics& g,int W,int H){
  if(W<=0||H<=0) return;
  if(!g_vigBmp||g_vigW!=W||g_vigH!=H){
    if(g_vigBmp){ delete g_vigBmp; g_vigBmp=nullptr; }
    Bitmap* b=new Bitmap(W,H,PixelFormat32bppPARGB); BitmapData bd; Rect r(0,0,W,H);
    if(b->GetLastStatus()==Ok && b->LockBits(&r,ImageLockModeWrite,PixelFormat32bppPARGB,&bd)==Ok){
      afkVignetteFill((uint8_t*)bd.Scan0,(int32_t)bd.Stride,W,H,200.0f/255.0f); b->UnlockBits(&bd); g_vigBmp=b; g_vigW=W; g_vigH=H; }
    else { delete b; return; }
  }
  g.DrawImage(g_vigBmp,0,0,W,H);
}

// ---- optional GPU device manager (only ever created if software decode can't keep up) ----
// Resolved dynamically so the exe has no static d3d11 dependency (and still starts on systems without it).
static IMFDXGIDeviceManager* g_dxgiMgr=nullptr;
static bool afkEnsureD3DManager(HRESULT* hrOut,const wchar_t** stepOut){
  if(g_dxgiMgr) return true;
  HMODULE d3d=LoadLibraryW(L"d3d11.dll");
  if(!d3d){ *hrOut=HRESULT_FROM_WIN32(GetLastError()); *stepOut=L"LoadLibrary(d3d11.dll)"; return false; }
  PFN_D3D11_CREATE_DEVICE pCreate=(PFN_D3D11_CREATE_DEVICE)GetProcAddress(d3d,"D3D11CreateDevice");
  typedef HRESULT (WINAPI *PFN_MFDXGIMGR)(UINT*,IMFDXGIDeviceManager**);
  PFN_MFDXGIMGR pMgr=(PFN_MFDXGIMGR)GetProcAddress(GetModuleHandleW(L"mfplat.dll"),"MFCreateDXGIDeviceManager");
  if(!pCreate||!pMgr){ *hrOut=E_NOTIMPL; *stepOut=L"resolve D3D11/MF entry points"; return false; }
  ID3D11Device* dev=nullptr; ID3D11DeviceContext* ctx=nullptr; D3D_FEATURE_LEVEL fl;
  HRESULT h=pCreate(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT|D3D11_CREATE_DEVICE_VIDEO_SUPPORT,nullptr,0,D3D11_SDK_VERSION,&dev,&fl,&ctx);
  if(FAILED(h)||!dev){ *hrOut=h; *stepOut=L"D3D11CreateDevice"; return false; }
  ID3D10Multithread* mtp=nullptr; if(SUCCEEDED(dev->QueryInterface(__uuidof(ID3D10Multithread),(void**)&mtp))&&mtp){ mtp->SetMultithreadProtected(TRUE); mtp->Release(); }
  UINT tok=0; IMFDXGIDeviceManager* mgr=nullptr; h=pMgr(&tok,&mgr);
  if(FAILED(h)||!mgr){ dev->Release(); if(ctx)ctx->Release(); *hrOut=h; *stepOut=L"MFCreateDXGIDeviceManager"; return false; }
  h=mgr->ResetDevice(dev,tok); dev->Release(); if(ctx)ctx->Release();
  if(FAILED(h)){ mgr->Release(); *hrOut=h; *stepOut=L"ResetDevice"; return false; }
  g_dxgiMgr=mgr; return true;
}

// ---- decode configurations ----
// Ordered by preference (see afkPickBest). vp: 0 none, 1 legacy software video processing, 2 advanced
// video processor (also resizes+converts in one step, which legacy processing cannot: that
// combination was rejected with MF_E_INVALIDMEDIATYPE on a real machine). The GPU device-manager
// configs use MF_SOURCE_READER_D3D_MANAGER with ADVANCED processing: the legacy-processing + D3D
// manager combination is rejected outright (E_INVALIDARG, seen on a real machine -- which also
// explains the earlier "black screen": that build had no software fallback, so the reader never opened).
struct DecCfg{ const wchar_t* name; int vp; bool hw; bool d3d; bool scaled; };
static const DecCfg kDecCfgs[]={
  {L"software / advanced video processor / screen-size output",2,false,false,true },
  {L"software / advanced video processor / native size",       2,false,false,false},
  {L"hardware-transform hint / legacy video processing / native (previous default)",1,true,false,false},
  {L"hardware-transform hint / advanced video processor / native",2,true,false,false},
  {L"GPU device manager / advanced video processor / screen-size output",2,true,true,true },
  {L"GPU device manager / advanced video processor / native size",       2,true,true,false},
};
static const int kNumDecCfgs=6, kDefaultDecCfg=2;
static std::atomic<int> g_decCfg{kDefaultDecCfg};

struct AfkOpen{ IMFSourceReader* rd=nullptr; UINT32 vw=0,vh=0,nw=0,nh=0; LONG stride=0; double fps=30.0; int64_t durationHns=0; HRESULT hr=S_OK; const wchar_t* step=L""; bool scaledApplied=false; };
static BOOL CALLBACK afkMonProc(HMONITOR,HDC,LPRECT r,LPARAM l){ int* m=(int*)l; int w=r->right-r->left,h=r->bottom-r->top; if(w>m[0])m[0]=w; if(h>m[1])m[1]=h; return TRUE; }
static void afkMaxMonitor(int* w,int* h){ int m[2]={0,0}; EnumDisplayMonitors(nullptr,nullptr,afkMonProc,(LPARAM)m);
  if(m[0]<=0||m[1]<=0){ m[0]=GetSystemMetrics(SM_CXSCREEN); m[1]=GetSystemMetrics(SM_CYSCREEN); } *w=m[0]; *h=m[1]; }

// Opens `path` with configuration c, negotiates RGB32 (optionally resized to cover the largest monitor
// -- never upscaled, so no visible quality is given up), and reports what it got. Every failure names
// the step and HRESULT so the log shows exactly what a machine rejected.
static bool afkOpenCfg(const DecCfg& c,const std::wstring& path,AfkOpen& o){
  o=AfkOpen(); IMFAttributes* at=nullptr; IMFSourceReader* rd=nullptr; IMFMediaType* mt=nullptr; IMFMediaType* cur=nullptr; bool ok=false;
  do{
    if(c.d3d && !afkEnsureD3DManager(&o.hr,&o.step)) break;
    if(FAILED(o.hr=MFCreateAttributes(&at,5))){ o.step=L"MFCreateAttributes"; break; }
    if(c.vp==1) at->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING,TRUE);
    if(c.vp==2) at->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING,TRUE);
    if(c.hw)    at->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS,TRUE);
    if(c.d3d)   at->SetUnknown(MF_SOURCE_READER_D3D_MANAGER,g_dxgiMgr);
    if(FAILED(o.hr=MFCreateSourceReaderFromURL(path.c_str(),at,&rd))){ o.step=L"MFCreateSourceReaderFromURL"; break; }
    rd->SetStreamSelection((DWORD)MF_SOURCE_READER_ALL_STREAMS,FALSE);
    rd->SetStreamSelection((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM,TRUE);
    { IMFMediaType* nat=nullptr;
      if(SUCCEEDED(rd->GetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM,&nat))&&nat){
        MFGetAttributeSize(nat,MF_MT_FRAME_SIZE,&o.nw,&o.nh); UINT32 fn=0,fd=0;
        if(SUCCEEDED(MFGetAttributeRatio(nat,MF_MT_FRAME_RATE,&fn,&fd))&&fn>0&&fd>0) o.fps=(double)fn/(double)fd; nat->Release(); } }
    if(!o.nw||!o.nh){ o.hr=E_FAIL; o.step=L"native size"; break; }
    { PROPVARIANT dv; PropVariantInit(&dv);
      if(SUCCEEDED(rd->GetPresentationAttribute((DWORD)MF_SOURCE_READER_MEDIASOURCE,MF_PD_DURATION,&dv)))
        o.durationHns=dv.vt==VT_UI8?(int64_t)dv.uhVal.QuadPart:0;
      PropVariantClear(&dv); }
    if(FAILED(o.hr=MFCreateMediaType(&mt))){ o.step=L"MFCreateMediaType"; break; }
    mt->SetGUID(MF_MT_MAJOR_TYPE,MFMediaType_Video); mt->SetGUID(MF_MT_SUBTYPE,MFVideoFormat_RGB32);
    if(c.scaled){ int mw=0,mh=0; afkMaxMonitor(&mw,&mh);
      if((int)o.nw>mw||(int)o.nh>mh){ double sc=(double)mw/o.nw, sc2=(double)mh/o.nh; if(sc2>sc)sc=sc2;
        UINT32 dw=((UINT32)(o.nw*sc+0.5))&~1u, dh=((UINT32)(o.nh*sc+0.5))&~1u;
        if(dw>0&&dh>0&&(dw<o.nw||dh<o.nh)){ MFSetAttributeSize(mt,MF_MT_FRAME_SIZE,dw,dh); o.scaledApplied=true; } } }
    if(FAILED(o.hr=rd->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM,NULL,mt))){ o.step=o.scaledApplied?L"SetCurrentMediaType(RGB32+resize)":L"SetCurrentMediaType(RGB32)"; break; }
    if(FAILED(o.hr=rd->GetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM,&cur))){ o.step=L"GetCurrentMediaType"; break; }
    GUID sub=GUID_NULL; cur->GetGUID(MF_MT_SUBTYPE,&sub); MFGetAttributeSize(cur,MF_MT_FRAME_SIZE,&o.vw,&o.vh);
    UINT32 st=0; o.stride=SUCCEEDED(cur->GetUINT32(MF_MT_DEFAULT_STRIDE,&st))?(LONG)st:(LONG)o.vw*4;
    if(!IsEqualGUID(sub,MFVideoFormat_RGB32)||!o.vw||!o.vh){ o.hr=E_FAIL; o.step=L"negotiated type"; break; }
    ok=true;
  }while(false);
  if(cur)cur->Release(); if(mt)mt->Release(); if(at)at->Release();
  if(ok) o.rd=rd; else if(rd) rd->Release();
  return ok;
}

// Decodes ~36 frames with configuration idx and returns the achieved fps (0 = unusable, -1 = same as
// the native-size config so not measured). Includes the RGB32 copy, since real playback pays for it.
static double afkBenchCfg(int idx,const std::wstring& path,double* nativeFps,bool* cachedFirst,bool* sane){
  const DecCfg& c=kDecCfgs[idx]; AfkOpen o; *sane=true; std::wstring tag=L"[AFKVID] bench #"+std::to_wstring(idx)+L" ("+c.name+L"): ";
  int64_t o0=afkHnsNow();
  if(!afkOpenCfg(c,path,o)){ logLine(tag+L"unavailable -- "+o.step+L" failed, hr="+hrHex(o.hr)); return 0.0; }
  *nativeFps=o.fps;
  if(c.scaled && !o.scaledApplied){ o.rd->Release(); logLine(tag+L"identical to native size on this display -- skipped"); return -1.0; }
  std::vector<uint8_t> scratch((size_t)o.vw*o.vh*4); bool nonBlack=false; int frames=0; int64_t tStart=afkHnsNow(),t0=0,ttff=-1;
  const int WARM=3, N=36; const int64_t BUDGET=45000000LL;
  for(int i=0,tries=0;i<WARM+N && tries<WARM+N+40;tries++){
    DWORD fl=0,si=0; LONGLONG ts=0; IMFSample* smp=nullptr;
    HRESULT hr=o.rd->ReadSample((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM,0,&si,&fl,&ts,&smp);
    if(FAILED(hr)){ logLine(tag+L"ReadSample failed, hr="+hrHex(hr)); break; }
    if(fl&MF_SOURCE_READERF_ENDOFSTREAM){ if(smp)smp->Release(); break; }
    if(!smp) continue;
    IMFMediaBuffer* buf=nullptr;
    if(SUCCEEDED(smp->ConvertToContiguousBuffer(&buf))){ BYTE* d=nullptr; DWORD ml=0,cl=0;
      if(SUCCEEDED(buf->Lock(&d,&ml,&cl))){ DWORD need=(DWORD)((o.stride<0?-o.stride:o.stride))*o.vh;
        if(cl>=need){ const BYTE* top=d; if(o.stride<0) top=d+(size_t)(o.vh-1)*(size_t)(-o.stride);
          afkCopyOpaque32(top,(int32_t)o.stride,scratch.data(),(int32_t)o.vw*4,o.vw,o.vh);
          if(!nonBlack) nonBlack=afkHasContent(scratch.data(),o.vw,o.vh);
          if(!*cachedFirst && i==WARM){ Bitmap* fb=afkMakeFrame(d,cl,o.stride,o.vw,o.vh); if(fb){ g_vx.publish(fb,afkHnsNow()); *cachedFirst=true; } } }
        buf->Unlock(); } buf->Release(); }
    smp->Release();
    if(ttff<0) ttff=afkHnsNow()-o0;
    if(i==WARM-1) t0=afkHnsNow(); else if(i>=WARM) frames++;
    i++;
    if(afkHnsNow()-tStart>BUDGET) break;
  }
  int64_t t1=afkHnsNow(); o.rd->Release();
  double sec=(t1-t0)/1e7; double fps=(t0>0&&sec>0.05&&frames>=5)?frames/sec:0.0;
  if(c.d3d && !nonBlack) *sane=false;      // a GPU path returning blank frames is not a working GPU path
  logLine(tag+afkF1(fps)+L" fps ("+std::to_wstring(o.vw)+L"x"+std::to_wstring(o.vh)+L", native clip "+afkF1(o.fps)+L" fps, first frame after "+afkF1(ttff/10000.0)+L" ms"+(*sane?L"":L", BLANK frames -- rejected")+L")");
  return fps;
}

// Runs once at startup on a low-priority thread. Measures the configurations in preference order and
// keeps g_decCfg pointing at the best so far, so playback started at any moment already uses the best
// result available. Stops early once one comfortably beats real time; only tries the GPU configs if
// software cannot sustain the clip (which also avoids waking a discrete GPU needlessly). A stuck
// candidate can only strand THIS thread -- real playback never runs an unmeasured config.
static DWORD WINAPI DecodeBenchThread(LPVOID){
  std::wstring path=findVideo(); if(path.empty()){ logLine(L"[AFKVID] bench: no video available"); return 0; }
  MFStartup(MF_VERSION);
  double fps[kNumDecCfgs]; bool ok[kNumDecCfgs]; for(int i=0;i<kNumDecCfgs;i++){ fps[i]=0; ok[i]=false; }
  double nativeFps=30.0, bestSw=0.0; bool cached=false;
  for(int i=0;i<kNumDecCfgs;i++){
    if(kDecCfgs[i].d3d && bestSw>=nativeFps*1.15){ logLine(L"[AFKVID] bench: software already sustains real time -- GPU configs not needed"); break; }
    bool sane=true; double f=afkBenchCfg(i,path,&nativeFps,&cached,&sane);
    if(f<0) continue;
    fps[i]=f; ok[i]=(f>0)&&sane; if(ok[i]&&!kDecCfgs[i].d3d&&f>bestSw) bestSw=f;
    int b=afkPickBest(fps,ok,kNumDecCfgs); if(b>=0) g_decCfg.store(b);
    if(!kDecCfgs[i].d3d && ok[i] && f>=nativeFps*1.6){ logLine(L"[AFKVID] bench: this config is comfortably faster than real time -- stopping here"); break; }
  }
  int b=g_decCfg.load();
  logLine(L"[AFKVID] bench result: using config #"+std::to_wstring(b)+L" ("+kDecCfgs[b].name+L")");
  MFShutdown(); return 0;
}

// ---- playback ----
// Private-clock pacing (AfkPacer): a late frame is shown immediately and the timeline slips, so the
// picture keeps moving at whatever rate the machine can sustain -- never dropped in bulk, never
// seeked WHILE BEHIND (that repeated-seek-while-still-behind thrashing is what froze earlier
// builds: each seek landed near the same keyframe as the last because "behind" barely changes
// between two seeks a few ms apart). The one seek this thread still does is the loop restart at
// natural end-of-stream, which was always happening anyway -- afkLoopRestartPos just retargets
// that existing seek to audio's current position instead of always position zero, so sustained
// heavy load can't let video drift arbitrarily far behind audio: every loop, not just every frame,
// the drift resets instead of accumulating. This happens once per full clip traversal at most, so
// it can't thrash the way per-frame catch-up seeking did.
static DWORD WINAPI VideoThread(LPVOID){
  MFStartup(MF_VERSION);
  IMFSourceReader* rd=nullptr; std::wstring openPath; UINT32 vw=0,vh=0; LONG stride=0; int curCfg=-1; int64_t durationHns=0;
  AfkPacer pacer; int readFails=0; bool firstLogged=false,badLogged=false,periodOn=false;
  auto closeRd=[&](){ if(rd){ rd->Release(); rd=nullptr; } openPath.clear(); g_vx.clear(); };
  auto seekLoop=[&](){
    bool haveClk=false; int64_t audioPos=readClock(haveClk);
    int64_t target=afkLoopRestartPos(haveClk,audioPos,durationHns);
    PROPVARIANT v; PropVariantInit(&v); v.vt=VT_I8; v.hVal.QuadPart=target; rd->SetCurrentPosition(GUID_NULL,v); PropVariantClear(&v);
    pacer.reset();
  };
  while(g_running.load()){
    if(!g_videoRun.load()){ if(rd)closeRd(); if(periodOn){ timeEndPeriod(1); periodOn=false; } Sleep(80); continue; }
    if(!rd || openPath!=g_videoPath){
      if(rd)closeRd(); openPath=g_videoPath;
      if(openPath.empty()){ Sleep(120); continue; }
      int chain[2]={g_decCfg.load(),kDefaultDecCfg}; AfkOpen o; int used=-1;
      for(int k=0;k<2&&used<0;k++){ if(k==1&&chain[1]==chain[0]) break;
        if(afkOpenCfg(kDecCfgs[chain[k]],openPath,o)) used=chain[k];
        else logLine(L"[AFKVID] open failed for config #"+std::to_wstring(chain[k])+L" at "+o.step+L", hr="+hrHex(o.hr)); }
      if(used<0){ openPath.clear(); Sleep(300); continue; }
      rd=o.rd; vw=o.vw; vh=o.vh; stride=o.stride; curCfg=used; durationHns=o.durationHns; readFails=0; firstLogged=false; badLogged=false; pacer.reset();
      logLine(L"[AFKVID] playing "+std::to_wstring(vw)+L"x"+std::to_wstring(vh)+L" via config #"+std::to_wstring(used)+L": "+kDecCfgs[used].name+
        L", duration "+afkF1(durationHns/1e7)+L"s");
      if(!periodOn){ timeBeginPeriod(1); periodOn=true; }     // 1ms Sleep granularity while video runs (default is ~15.6ms)
    }
    DWORD flags=0,sidx=0; LONGLONG ts=0; IMFSample* smp=nullptr;
    int64_t r0=afkHnsNow();
    HRESULT hr=rd->ReadSample((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM,0,&sidx,&flags,&ts,&smp);
    g_stReadUs+=(afkHnsNow()-r0)/10;
    if(FAILED(hr)){ readFails++; if(readFails==1) logLine(L"[AFKVID] ReadSample failed, hr="+hrHex(hr));
      if(readFails>=3 && curCfg!=kDefaultDecCfg){ logLine(L"[AFKVID] demoting to the default config"); g_decCfg.store(kDefaultDecCfg); }
      closeRd(); Sleep(200); continue; }
    readFails=0;
    if(flags&MF_SOURCE_READERF_ENDOFSTREAM){ if(smp)smp->Release(); seekLoop(); continue; }   // loop, resynced to audio's current position
    if(!smp){ Sleep(1); continue; }
    int64_t c0=afkHnsNow(); Bitmap* fb=nullptr; IMFMediaBuffer* buf=nullptr;
    if(SUCCEEDED(smp->ConvertToContiguousBuffer(&buf))){ BYTE* d=nullptr; DWORD ml=0,cl=0;
      if(SUCCEEDED(buf->Lock(&d,&ml,&cl))){ fb=afkMakeFrame(d,cl,stride,vw,vh); buf->Unlock(); } buf->Release(); }
    smp->Release(); g_stCopyUs+=(afkHnsNow()-c0)/10;
    if(!fb){ if(!badLogged){ badLogged=true; logLine(L"[AFKVID] a decoded buffer was unusable (too small / lock failed) -- skipping such frames"); } continue; }
    bool sl=false; int64_t wait=pacer.schedule(ts,afkHnsNow(),&sl); if(sl) g_stSlips++;
    DWORD ms=(DWORD)(wait/10000); if(ms>100) ms=100; if(ms) Sleep(ms);
    g_vx.publish(fb,afkHnsNow()); g_stFrames++;
    if(!firstLogged){ firstLogged=true; logLine(L"[AFKVID] first frame published"); }
  }
  if(rd)closeRd(); if(periodOn) timeEndPeriod(1); MFShutdown(); return 0;
}

// Every ~5s while the screensaver is up: where the time actually goes, so the next tuning step is
// data instead of a guess. Also retunes the render rate to the measured cost (30fps floor, 15fps ceiling).
static void afkLogStats(ULONGLONG ms){
  if(!g_rN||!ms) return;
  double n=(double)g_rN, avg=g_rTotUs/n/1000.0, vid=g_rVideoUs/n/1000.0, vig=g_rVigUs/n/1000.0, pres=g_rPresUs/n/1000.0;
  long long df=g_stFrames.exchange(0), rus=g_stReadUs.exchange(0), cus=g_stCopyUs.exchange(0), sl=g_stSlips.exchange(0);
  double dfps=df*1000.0/(double)ms, rfps=n*1000.0/(double)ms;
  logLine(L"[AFKVID] stats: render "+afkF1(rfps)+L" fps, "+afkF1(avg)+L" ms/frame (video "+afkF1(vid)+L", vignette "+afkF1(vig)+L", present "+afkF1(pres)+
    L") | decode published "+afkF1(dfps)+L" fps, ReadSample "+afkF1(df?rus/(double)df/1000.0:0)+L" ms, copy "+afkF1(df?cus/(double)df/1000.0:0)+L" ms, timeline slips "+std::to_wstring(sl));
  int want=(int)(avg*1.3+0.5); if(want<33)want=33; if(want>66)want=66; g_afkTickWant=want;
  g_rN=g_rTotUs=g_rVideoUs=g_rVigUs=g_rPresUs=0;
}

// ---------- AFK screensaver: audio session (clock published for video) ----------
// Builds the audio-only topology and starts the session. That session's clock
// is what VideoThread paces its decode against (see readClock()/publishClock()
// above) -- so the two are still one clock in spirit, without depending on the
// topology loader auto-inserting a decoder ahead of a sample-grabber sink, or
// on a native video-renderer window (both tried, both failed invisibly on the
// user's machine with no way for me to diagnose why from here).
// (Function/thread/handle names kept as "Audio*" so nothing else needed to
// change; it now only carries the audio branch.)
static DWORD WINAPI AudioThread(LPVOID){
  CoInitializeEx(0,COINIT_MULTITHREADED);
  MFStartup(MF_VERSION);
  IMFMediaSession* sess=nullptr; IMFMediaSource* src=nullptr;
  std::wstring openPath; bool playing=false;

  auto tearDown=[&](){
    publishClock(nullptr);           // stop VideoThread pacing off a clock that's about to die
    if(sess){ sess->Stop(); sess->Close(); sess->Shutdown(); sess->Release(); sess=nullptr; }
    if(src){ src->Shutdown(); src->Release(); src=nullptr; }
    openPath.clear(); playing=false;
  };
  auto applyVolume=[&](){
    if(!sess)return; IMFSimpleAudioVolume* v=nullptr;
    if(SUCCEEDED(MFGetService(sess,MR_POLICY_VOLUME_SERVICE,IID_PPV_ARGS(&v))) && v){
      float vol=g_afkVol; if(vol<0.f)vol=0.f; if(vol>1.f)vol=1.f;
      // Muting is now just SetMute -- the session (and the video branch riding
      // along on the same clock) stays up. Tearing the whole pipeline down to
      // mute would have frozen the picture too, since it's the same session.
      v->SetMasterVolume(vol); v->SetMute(g_afkAudio.load()?FALSE:TRUE); v->Release(); }
  };

  while(g_running.load()){
    if(!g_videoRun.load()){ if(sess||src) tearDown(); Sleep(80); continue; }   // g_afkAudio no longer gates the whole pipeline -- see applyVolume()

    if(!playing || openPath!=g_videoPath){
      tearDown(); openPath=g_videoPath;
      if(openPath.empty()){ Sleep(120); continue; }

      IMFSourceResolver* res=nullptr;
      if(FAILED(MFCreateSourceResolver(&res))||!res){ Sleep(400); continue; }
      MF_OBJECT_TYPE ot=MF_OBJECT_INVALID; IUnknown* unk=nullptr;
      HRESULT hr=res->CreateObjectFromURL(openPath.c_str(),MF_RESOLUTION_MEDIASOURCE,NULL,&ot,&unk);
      res->Release();
      if(FAILED(hr)||!unk){ if(unk)unk->Release(); Sleep(400); continue; }
      hr=unk->QueryInterface(IID_PPV_ARGS(&src)); unk->Release();
      if(FAILED(hr)||!src){ src=nullptr; Sleep(400); continue; }

      IMFPresentationDescriptor* pd=nullptr;
      if(FAILED(src->CreatePresentationDescriptor(&pd))||!pd){ tearDown(); Sleep(400); continue; }

      IMFTopology* topo=nullptr;
      if(FAILED(MFCreateTopology(&topo))||!topo){ pd->Release(); tearDown(); Sleep(400); continue; }

      DWORD nsd=0; pd->GetStreamDescriptorCount(&nsd); bool added=false;
      for(DWORD i=0;i<nsd;i++){
        BOOL sel=FALSE; IMFStreamDescriptor* sd=nullptr;
        if(FAILED(pd->GetStreamDescriptorByIndex(i,&sel,&sd))||!sd) continue;
        IMFMediaTypeHandler* th=nullptr; GUID maj=GUID_NULL;
        if(SUCCEEDED(sd->GetMediaTypeHandler(&th))&&th) th->GetMajorType(&maj);
        if(sel && maj==MFMediaType_Audio && !added){
          IMFTopologyNode* sn=nullptr; IMFTopologyNode* on=nullptr; IMFActivate* ar=nullptr;
          if(SUCCEEDED(MFCreateTopologyNode(MF_TOPOLOGY_SOURCESTREAM_NODE,&sn))&&sn){
            sn->SetUnknown(MF_TOPONODE_SOURCE,src);
            sn->SetUnknown(MF_TOPONODE_PRESENTATION_DESCRIPTOR,pd);
            sn->SetUnknown(MF_TOPONODE_STREAM_DESCRIPTOR,sd);
            topo->AddNode(sn);
            if(SUCCEEDED(MFCreateAudioRendererActivate(&ar))&&ar){
              if(SUCCEEDED(MFCreateTopologyNode(MF_TOPOLOGY_OUTPUT_NODE,&on))&&on){
                on->SetObject(ar);
                on->SetUINT32(MF_TOPONODE_STREAMID,0);
                on->SetUINT32(MF_TOPONODE_NOSHUTDOWN_ON_REMOVE,FALSE);
                topo->AddNode(on);
                sn->ConnectOutput(0,on,0);
                added=true; on->Release();
              }
              ar->Release();
            }
            sn->Release();
          }
        } else {
          pd->DeselectStream(i);        // video is decoded separately by VideoThread, paced off this session's clock
        }
        if(th)th->Release(); sd->Release();
      }

      if(!added){ topo->Release(); pd->Release(); tearDown(); Sleep(1000); continue; }
      if(FAILED(MFCreateMediaSession(NULL,&sess))||!sess){ topo->Release(); pd->Release(); tearDown(); Sleep(400); continue; }
      sess->SetTopology(0,topo);
      topo->Release(); pd->Release();

      PROPVARIANT sp; PropVariantInit(&sp);
      sess->Start(&GUID_NULL,&sp); PropVariantClear(&sp);
      playing=true;
      applyVolume();
      { IMFClock* clk=nullptr; if(SUCCEEDED(sess->GetClock(&clk)) && clk){ publishClock(clk); clk->Release(); } }
    }

    if(sess){
      IMFMediaEvent* ev=nullptr;
      if(SUCCEEDED(sess->GetEvent(MF_EVENT_FLAG_NO_WAIT,&ev)) && ev){
        MediaEventType t=MEUnknown; ev->GetType(&t);
        if(t==MEEndOfPresentation){
          // Restarting the SESSION restarts both branches -- audio renderer
          // and the video sample-grabber -- from the same instant, since
          // they're two outputs of the same topology on the same clock.
          // That's the whole fix: there is no second thread/clock to nudge
          // back into line anymore.
          PROPVARIANT z; PropVariantInit(&z); z.vt=VT_I8; z.hVal.QuadPart=0;
          sess->Start(&GUID_NULL,&z); PropVariantClear(&z);
        } else if(t==MESessionTopologyStatus){
          UINT32 st=0; ev->GetUINT32(MF_EVENT_TOPOLOGY_STATUS,&st);
          if(st==MF_TOPOSTATUS_READY) applyVolume();
        } else if(t==MEError){
          tearDown();
        }
        ev->Release();
      }
    }
    Sleep(40);
  }
  tearDown(); MFShutdown(); CoUninitialize(); return 0;
}
static void freeAfkFrames(){ for(auto* b:g_afkFrames) delete b; g_afkFrames.clear(); }
static void loadAfkFrames(){ freeAfkFrames();
  std::wstring dir=appDir()+L"\\content\\afk";
  WIN32_FIND_DATAW fd; HANDLE h=FindFirstFileW((dir+L"\\*").c_str(),&fd);
  if(h==INVALID_HANDLE_VALUE)return;
  std::vector<std::wstring> names;
  do{ std::wstring f=fd.cFileName, l=lc(f);
    if(endsW(l,L".png")||endsW(l,L".jpg")||endsW(l,L".jpeg")||endsW(l,L".bmp"))
      names.push_back(f);
  }while(FindNextFileW(h,&fd)); FindClose(h);
  std::sort(names.begin(),names.end());
  for(auto& nm:names){ if((int)g_afkFrames.size()>=240)break;
    Bitmap* b=Bitmap::FromFile((dir+L"\\"+nm).c_str());
    if(b&&b->GetLastStatus()==Ok) g_afkFrames.push_back(b); else delete b; }
}
// runs every frame: AFK detect, pool switch, 2-min rotate, crossfade, overlay fade.
// Called both from g_dash's own render loop (so the sidebar quote widget
// keeps crossfading smoothly while the dashboard is open) and from the
// always-on g_msg timer below (so AFK detection and the screensaver itself
// keep working even with the dashboard closed/unfocused). Being called from
// two places means the ~500ms fade timings can occasionally take one extra
// 1ms-minimum step if both happen to fire in the same instant -- bounded,
// not visible in practice, and far simpler than trying to coordinate two
// independent timers to call this exactly once.
static void updateQuoteState(){
  ULONGLONG now=GetTickCount64(); bool afk=isAfk(afkThresholdMs());
  if(afk!=g_afkNow){ g_afkNow=afk; triggerQuoteChange();
    if(afk){ g_videoPath=findVideo();
      if(!g_videoPath.empty()) g_videoRun.store(true); else loadAfkFrames();
      g_afkFrameT0=now; g_afkEnterT0=now;
    }
    else { g_videoRun.store(false); freeAfkFrames(); }
  }
  else if(g_fadePhase==0 && now-g_qT0>=120000) triggerQuoteChange();
  if(g_fadePhase==1){ double t=(double)(now-g_fadeT0)/380.0;
    if(t>=1){ g_curQuote=g_pendingQuote; g_fadePhase=2; g_fadeT0=now; g_quoteChangeT0=now; g_quoteAlpha=0; } else g_quoteAlpha=(int)(255*(1-t)); }
  else if(g_fadePhase==2){ double t=(double)(now-g_fadeT0)/380.0;
    if(t>=1){ g_fadePhase=0; g_qT0=now; g_quoteAlpha=255; } else g_quoteAlpha=(int)(255*t); }
  else g_quoteAlpha=255;
  { static ULONGLONG lastT=now; ULONGLONG dt=(now>lastT)?(now-lastT):0; lastT=now; if(dt>250)dt=250; // clamp huge gaps (e.g. was minimized)
    int tgt=g_afkNow?255:0;
    int step=(int)(255.0*dt/500.0); if(step<1)step=1;    // ~500ms full sweep, independent of render fps
    if(g_afkA<tgt) g_afkA=(g_afkA+step>tgt)?tgt:g_afkA+step;
    else if(g_afkA>tgt) g_afkA=(g_afkA-step<tgt)?tgt:g_afkA-step;
  }
}
// centered, word-wrapped rich text with bigger **emphasis**
static REAL drawRichCentered(Graphics& g,std::wstring text,REAL cxr,REAL topY,REAL maxW,Color c,Font* fn,Font* fb,REAL lh,
                             bool measureOnly=false){
  size_t up; while((up=text.find(L"{user}"))!=std::wstring::npos) text.replace(up,6,g_userName);
  struct W{ std::wstring s; bool b; }; std::vector<W> words; bool bold=false; std::wstring cur;
  auto pw=[&](bool bb){ if(!cur.empty()){ words.push_back({cur,bb}); cur.clear(); } };
  for(size_t i=0;i<text.size();){ if(text.compare(i,2,L"**")==0){ pw(bold); bold=!bold; i+=2; }
    else if(text[i]==L' '){ cur+=L' '; pw(bold); i++; } else cur+=text[i++]; }
  pw(bold);
  SolidBrush br(c); std::vector<std::vector<W>> lines; std::vector<W> line; REAL lw=0;
  for(auto&w:words){ Font* f=w.b?fb:fn; RectF bb; g.MeasureString(w.s.c_str(),-1,f,PointF(0,0),&bb);
    if(lw+bb.Width>maxW && !line.empty()){ lines.push_back(line); line.clear(); lw=0; }
    line.push_back(w); lw+=bb.Width; }
  if(!line.empty()) lines.push_back(line);
  if(measureOnly) return (REAL)lines.size()*lh;
  REAL y=topY;
  for(auto&ln:lines){ REAL tot=0; std::vector<REAL> ws;
    for(auto&w:ln){ Font* f=w.b?fb:fn; RectF bb; g.MeasureString(w.s.c_str(),-1,f,PointF(0,0),&bb); ws.push_back(bb.Width); tot+=bb.Width; }
    REAL x=cxr-tot/2;
    for(size_t i=0;i<ln.size();++i){ Font* f=ln[i].b?fb:fn; RectF bb; g.MeasureString(ln[i].s.c_str(),-1,f,PointF(0,0),&bb);
      g.DrawString(ln[i].s.c_str(),-1,f,PointF(x,y+(lh-bb.Height)/2),&br); x+=ws[i]; }
    y+=lh; }
  return (REAL)lines.size()*lh;
}
static void drawAfkOverlay(Graphics& g,int W,int H){
  int a=g_afkA; if(a<=0)return; float af=a/255.0f; ULONGLONG now=GetTickCount64();
  // Staged reveal: screen fades to black first (that's g_afkA itself, above),
  // then stars, then video, then clock/quote/hint -- each its own fade-in
  // rather than everything appearing together. Timings are offsets from
  // g_afkEnterT0, each phase's own fade-in duration in ms.
  //   0 ------ 500ms : screen to black (af itself)
  // 500 ------ 1300ms : starfield fades in
  //1300 ------ 2100ms : video fades in
  //2100 ------ 2800ms : clock/quote/hint fade in
  // Once AFK is ending (g_afkNow false), all three snap to fully-revealed and
  // the exit is a single uniform fade-out driven by af alone -- simpler, and
  // matches how it's dismissed: on real activity, not on a schedule.
  auto stage=[&](ULONGLONG startMs,ULONGLONG durMs)->float{
    if(!g_afkNow) return 1.0f;
    ULONGLONG sinceEnter = now>=g_afkEnterT0 ? now-g_afkEnterT0 : 0;
    if(sinceEnter<=startMs) return 0.0f;
    ULONGLONG el=sinceEnter-startMs;
    if(el>=durMs) return 1.0f;
    return (float)el/(float)durMs;
  };
  float starF=stage(500,800), vidF=stage(1300,800), txtF=stage(2100,700);
  bool haveVideo=false;
  // --- background: video (fast path), else image seq, else dark veil ---
  // Video is drawn OPAQUE with cheap resampling (nearest when 1:1, bilinear otherwise) and no colour
  // matrix; the fade-in is a solid overlay in the background colour instead. That is exactly equivalent
  // because renderAfkWin pre-fills the backbuffer with that same solid colour. The old draw was
  // HighQualityBicubic + colour matrix on a 3.7MP frame while holding the decoder's lock, which alone
  // could cost well over 100ms per frame. The frame is borrowed (acquire/release), never locked for
  // the duration of the draw.
  InterpolationMode oldIM=g.GetInterpolationMode(); PixelOffsetMode oldPO=g.GetPixelOffsetMode(); CompositingMode oldCM=g.GetCompositingMode();
  int64_t tv0=afkHnsNow();
  { Bitmap *curr=nullptr,*prev=nullptr; int64_t currT=0,prevT=0;
    g_vx.acquireForDraw(curr,prev,currT,prevT);
    if(curr){ REAL fw=(REAL)curr->GetWidth(),fh=(REAL)curr->GetHeight(),Wf=(REAL)W,Hf=(REAL)H;
      REAL sc=(Wf/fw>Hf/fh)?(Wf/fw):(Hf/fh); REAL dw=fw*sc,dh=fh*sc;
      bool oneToOne=(sc>0.998f&&sc<1.002f);
      // Float dest rect, not an integer Rect: an int (W-dw)/2,(int)(fw*sc) pair could floor 1px short of
      // full cover and show a sliver of background at an edge -- the "black line".
      RectF dest((Wf-dw)/2.0f,(Hf-dh)/2.0f,dw,dh);
      g.SetPixelOffsetMode(PixelOffsetModeHalf);
      g.SetInterpolationMode(oneToOne?InterpolationModeNearestNeighbor:InterpolationModeBilinear);
      // Cross-fade fix for slow decode ("powerpoint slides"): only kicks in once the gap between the
      // two most recent frames is wide enough that a hard cut would actually be visible (see
      // afkBlendT) -- at native frame rate this is always exactly 1.0f, so the fast single-image
      // SourceCopy path below runs completely unchanged and pays nothing extra for this existing.
      bool usePrev=prev&&(UINT)prev->GetWidth()==(UINT)curr->GetWidth()&&(UINT)prev->GetHeight()==(UINT)curr->GetHeight();
      const int64_t MIN_GAP_TO_BLEND=600000;   // 60ms (~<16fps) -- below this a hard cut isn't perceptible; don't pay any blend cost
      float t=usePrev?afkBlendT(afkHnsNow(),currT,prevT,MIN_GAP_TO_BLEND):1.0f;
      if(t>=0.999f){
        g.SetCompositingMode(CompositingModeSourceCopy);
        if(oneToOne) g.DrawImage(curr,dest,0.0f,0.0f,fw,fh,UnitPixel);
        else { ImageAttributes ia; ia.SetWrapMode(WrapModeTileFlipXY); g.DrawImage(curr,dest,0.0f,0.0f,fw,fh,UnitPixel,&ia); }   // wrap: no edge fringe
      } else {
        // prev opaque as the base, curr blended on top at alpha=t: a standard cross-dissolve. Costs
        // a colour-matrix draw (the thing removed from the normal path for performance) but only for
        // as many render ticks as the gap between real decoded frames actually spans.
        g.SetCompositingMode(CompositingModeSourceCopy);
        if(oneToOne) g.DrawImage(prev,dest,0.0f,0.0f,fw,fh,UnitPixel);
        else { ImageAttributes ia0; ia0.SetWrapMode(WrapModeTileFlipXY); g.DrawImage(prev,dest,0.0f,0.0f,fw,fh,UnitPixel,&ia0); }
        g.SetCompositingMode(CompositingModeSourceOver);
        ColorMatrix cm={ {{1,0,0,0,0},{0,1,0,0,0},{0,0,1,0,0},{0,0,0,t,0},{0,0,0,0,1}} };
        ImageAttributes ia; ia.SetColorMatrix(&cm,ColorMatrixFlagsDefault,ColorAdjustTypeBitmap); ia.SetWrapMode(WrapModeTileFlipXY);
        g.DrawImage(curr,dest,0.0f,0.0f,fw,fh,UnitPixel,&ia);
      }
      g.SetCompositingMode(CompositingModeSourceOver);
      float fade=af*vidF; if(fade<0.995f){ if(fade<0.0f)fade=0.0f; SolidBrush fadeb(Color((int)((1.0f-fade)*255.0f+0.5f),6,8,16)); FRect(g,&fadeb,0,0,Wf,Hf); }
      haveVideo=true; }
    g_vx.releaseAfterDraw(); }
  g_rVideoUs+=(afkHnsNow()-tv0)/10;
  if(!haveVideo && !g_afkFrames.empty()){
    g.SetInterpolationMode(InterpolationModeHighQualityBicubic); g.SetPixelOffsetMode(PixelOffsetModeHalf);
    int n=(int)g_afkFrames.size(); int fi=(int)(((now-g_afkFrameT0)/42)%n);
    Bitmap* fr=g_afkFrames[fi]; if(fr){ REAL fw=(REAL)fr->GetWidth(),fh=(REAL)fr->GetHeight(),Wf=(REAL)W,Hf=(REAL)H;
      REAL sc=(Wf/fw>Hf/fh)?(Wf/fw):(Hf/fh); REAL dw=fw*sc,dh=fh*sc;
      ColorMatrix cm; ZeroMemory(&cm,sizeof(cm)); cm.m[0][0]=cm.m[1][1]=cm.m[2][2]=1; cm.m[4][4]=1; cm.m[3][3]=af*vidF;
      ImageAttributes ia; ia.SetColorMatrix(&cm); ia.SetWrapMode(WrapModeTileFlipXY);
      RectF dest((Wf-dw)/2.0f,(Hf-dh)/2.0f,dw,dh); g.DrawImage(fr,dest,0.0f,0.0f,fw,fh,UnitPixel,&ia); haveVideo=true; }
  }
  g.SetInterpolationMode(oldIM); g.SetPixelOffsetMode(oldPO); g.SetCompositingMode(oldCM);
  if(!haveVideo){ SolidBrush veil(Color((int)(a*0.92f),6,8,16)); FRect(g,&veil,0,0,(REAL)W,(REAL)H); }

  // --- camera-lens vignette over the video: clear focus circle, smooth falloff to dark edges ---
  // BEFORE the starfield, so only the video's own edges darken and the stars stay fully bright.
  // Drawn from a cached per-resolution bitmap (see afkDrawVignette): one cheap blit per frame.
  if(haveVideo){ int64_t tg0=afkHnsNow(); afkDrawVignette(g,W,H); g_rVigUs+=(afkHnsNow()-tg0)/10; }

  // --- brighter, glowing starfield ON TOP of the video+vignette (no moon) ---
  if(g_stars.empty()){ for(int i=0;i<90;i++){ Part st; st.x=(float)(rand()%(W>0?W:1)); st.y=(float)(rand()%(H>0?H:1));
    st.vy=0.10f+(rand()%40)/150.0f; st.a=70+rand()%170; g_stars.push_back(st);} }
  // Dimmed within the lens focus circle (same radius as drawVignette's
  // clear plateau) so stars don't clutter the sharp, in-focus middle of the
  // shot -- they're still there, just quiet -- and ramp back up to full
  // brightness once outside it, where they help fill in the vignette's dark
  // edges instead of leaving them empty. Smoothstep-shaped rather than
  // linear: stays low through most of the focus zone instead of visibly
  // brightening star-by-star as each one drifts a few pixels off centre,
  // then ramps up over a short band right at the zone's edge.
  REAL scx=(REAL)W/2.0f, scy=(REAL)H/2.0f;
  REAL sFocusR=(REAL)(W<H?W:H)*0.30f;
  const float STAR_CENTER_MIN=0.06f;         // opacity floor at dead centre -- dim, not gone
  for(auto&st:g_stars){ st.y-=st.vy; if(st.y<0){ st.y=(float)H; st.x=(float)(rand()%(W>0?W:1)); }
    float ddx=st.x-scx, ddy=st.y-scy; float dist=std::sqrt(ddx*ddx+ddy*ddy);
    float t=dist/sFocusR; if(t>1.0f)t=1.0f; if(t<0.0f)t=0.0f;
    float shaped=t*t*(3.0f-2.0f*t);           // smoothstep
    float centerDim=STAR_CENTER_MIN+(1.0f-STAR_CENTER_MIN)*shaped;
    int sa=(int)(st.a*af*starF*centerDim);
    SolidBrush glow(Color(sa/3,170,205,255)); FEll(g,&glow,st.x-2.0f,st.y-2.0f,7.0f,7.0f);
    SolidBrush core(Color(sa,225,238,255)); FEll(g,&core,st.x,st.y,2.6f,2.6f); }

  // --- black gradient rising from the bottom, fading out toward the top ---
  { int ga=(int)(215*af*txtF); REAL gy=(REAL)H*0.42f;
    LinearGradientBrush lg(PointF(0.0f,gy),PointF(0.0f,(REAL)H),Color(0,0,0,0),Color(ga,0,0,0));
    g.FillRectangle(&lg,0.0f,gy,(REAL)W,(REAL)H-gy); }

  { SYSTEMTIME st; GetLocalTime(&st); wchar_t clk[16]; wsprintfW(clk,L"%02d:%02d:%02d",st.wHour,st.wMinute,st.wSecond);
    RectF bb; g.MeasureString(clk,-1,fClock,PointF(0,0),&bb);
    SolidBrush cb(Color((int)(a*txtF),220,230,245)); g.DrawString(clk,-1,fClock,PointF((REAL)W/2-bb.Width/2,(REAL)H*0.44f),&cb); }
  if(!g_curQuote.empty()){ int comb=(int)((g_afkA/255.0f)*(g_quoteAlpha/255.0f)*txtF*255);
    REAL qTop=(REAL)H*0.58f, qW=(REAL)W*0.70f;
    REAL qAvail=((REAL)H-54)-24-qTop;                 // clock above, AFK hint below
    Color qc2(comb,236,239,249);
    const RichFont* rf=nullptr;
    for(auto&cand:g_ladderBig){
      if(drawRichCentered(g,g_curQuote,(REAL)W/2,qTop,qW,qc2,cand.n,cand.b,cand.lh,true)<=qAvail){ rf=&cand; break; } }
    if(!rf && !g_ladderBig.empty()) rf=&g_ladderBig.back();
    // keep the block vertically centered in the space it was given
    REAL qh=rf?drawRichCentered(g,g_curQuote,(REAL)W/2,qTop,qW,qc2,rf->n,rf->b,rf->lh,true):0;
    REAL qy2=qTop+(qAvail-qh)/2; if(qy2<qTop) qy2=qTop;
    if(rf) drawRichCentered(g,g_curQuote,(REAL)W/2,qy2,qW,qc2,rf->n,rf->b,rf->lh);
    else   drawRichCentered(g,g_curQuote,(REAL)W/2,qTop,qW,qc2,fQuote,fQuoteB,36); }
  { const wchar_t* hint=L"AFK  \u00b7  move or press a key to resume";
    RectF bb; g.MeasureString(hint,-1,fMono,PointF(0,0),&bb);
    SolidBrush hb(Color((int)(a*0.7f*txtF),150,160,190)); g.DrawString(hint,-1,fMono,PointF((REAL)W/2-bb.Width/2,(REAL)H-54),&hb); }
}

static void renderScene(HDC dc,int W,int H){
  if(W<=0||H<=0)return;
  if(!g_back||g_bw!=W||g_bh!=H){ if(g_back){delete g_back;g_back=nullptr;} g_back=new Bitmap(W,H,PixelFormat32bppPARGB); g_bw=W; g_bh=H; }
  if(!g_back||g_back->GetLastStatus()!=Ok)return;
  Graphics g(g_back);
  g.SetSmoothingMode(SmoothingModeAntiAlias);
  g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
  SolidBrush bg(C_BG); FRect(g,&bg,0,0,W,H);

  double now=(GetTickCount64()-g_start)/1000.0;
  std::vector<Ev> evs; { EnterCriticalSection(&g_cs); evs.assign(g_events.begin(),g_events.end()); LeaveCriticalSection(&g_cs); }

  // ---- splash / loading ----
  if(g_splash){
    double el=(GetTickCount64()-g_splashT0)/1000.0;
    int steps=19; int done=(int)(el/0.14); if(done>steps)done=steps;
    int pct=(int)(100.0*done/steps);
    REAL m=48;
    Pen fb(C_LINE2,2);
    DLine(g,&fb,m,m,m+22,m); DLine(g,&fb,m,m,m,m+22);
    DLine(g,&fb,W-m,m,W-m-22,m); DLine(g,&fb,W-m,m,W-m,m+22);
    DLine(g,&fb,m,H-m,m+22,H-m); DLine(g,&fb,m,H-m,m,H-m-22);
    DLine(g,&fb,W-m,H-m,W-m-22,H-m); DLine(g,&fb,W-m,H-m,W-m,H-m-22);
    SolidBrush dotb(C_VIOLET); FEll(g,&dotb,m+6,m+24,9,9);
    drawStr(g,L"SENTINEL",fH,C_TX,m+24,m+16);
    drawStr(g,L"WATCH SUITE",fMono,C_TXM,m+24,m+38);
    SYSTEMTIME st; GetLocalTime(&st); wchar_t ts[16]; wsprintfW(ts,L"%02d:%02d:%02d",st.wHour,st.wMinute,st.wSecond);
    drawStrR(g,ts,fMono,C_TXD,W-m-6,m+16);
    wchar_t cc[16]; wsprintfW(cc,L"%02d / %02d",done,steps); drawStrR(g,cc,fMono,C_TXM,W-m-6,m+36);
    std::wstring title=scramble(L"INITIALIZING",el,0.12);
    drawStr(g,title.c_str(),fBig,C_TX,m+6,(REAL)H/2-72);
    drawStr(g,L"\u203a\u203a\u203a",fBig,C_VDIM,m+250,(REAL)H/2-72);
    REAL rcx=W-m-92, rcy=(REAL)H/2-42, rr=54;
    Pen rp(C_LINE2,1);
    DEll(g,&rp,rcx-rr,rcy-rr,rr*2,rr*2); DEll(g,&rp,rcx-rr*0.6f,rcy-rr*0.6f,rr*1.2f,rr*1.2f);
    REAL ang=(REAL)fmod(el*140.0,360.0);
    GraphicsPath pie; pie.AddPie((REAL)(rcx-rr),(REAL)(rcy-rr),(REAL)(rr*2),(REAL)(rr*2),(REAL)ang,(REAL)70);
    SolidBrush pb(Color(85,150,124,246)); g.FillPath(&pb,&pie);
    DLine(g,&rp,rcx,rcy,rcx+rr,rcy);
    std::wstring sub=L"resolving \u00b7 "+std::to_wstring(done)+L" / "+std::to_wstring(steps);
    drawStr(g,sub.c_str(),fMono,C_VIOLET,m+8,(REAL)H/2-16);
    wchar_t pc[8]; wsprintfW(pc,L"%d%%",pct); drawStrR(g,pc,fH,C_TX,rcx+rr,(REAL)H/2-20);
    REAL bx=m+8, bw=(REAL)W-m*2-16, prog=(REAL)done/steps;
    strokeRound(g,bx,(REAL)H/2+8,bw,7,3,C_LINE2); fillRound(g,bx,(REAL)H/2+8,bw*prog,7,3,C_VIOLET);
    REAL wy=(REAL)H-m-46; int bars=40; REAL step=((REAL)W-m*2)/(REAL)bars;
    for(int i=0;i<bars;i++){ int hh=8+(rand()%22); SolidBrush wb(Color(140,150,124,246));
      FRect(g,&wb,m+i*step,wy+(26-hh),3,(REAL)hh); }
    std::wstring hex; static const wchar_t* HX=L"0123456789ABCDEF";
    for(int i=0;i<20;i++){ hex+=HX[rand()%16]; hex+=HX[rand()%16]; hex+=L' '; }
    drawStr(g,hex.c_str(),fMono,C_TXM,m+8,(REAL)H-m-14);
    drawStrR(g,L"SENTINEL",fMono,C_TXM,W-m-6,(REAL)H-m-14);
    if(el*1000.0>g_splashMs){ g_splash=false; g_sessionStart=GetTickCount64(); }
    for(int y=0;y<H;y+=3){ SolidBrush sl(Color(28,0,0,0)); FRect(g,&sl,0,(REAL)y,(REAL)W,1); }
    Graphics wg(dc); wg.DrawImage(g_back,0,0); return;
  }

  updateQuoteState();

  // ---- background particles (subtle motion in empty space) ----
  if(g_parts.empty()){ for(int i=0;i<14;i++){ Part pt; pt.x=(float)(SIDE_W+(W>SIDE_W?rand()%(W-SIDE_W):0));
    pt.y=(float)(H>0?rand()%H:0); pt.vy=0.15f+(rand()%50)/120.0f; pt.a=18+rand()%40; g_parts.push_back(pt);} }
  for(auto&pt:g_parts){ pt.y-=pt.vy; if(pt.y<BAR_H){ pt.y=(float)H; pt.x=(float)(SIDE_W+(W>SIDE_W?rand()%(W-SIDE_W):0)); }
    SolidBrush pb(Color(pt.a,150,190,255)); FEll(g,&pb,pt.x,pt.y,2.5f,2.5f); }

  // ---- sidebar ----
  SolidBrush sb(C_BG2); FRect(g,&sb,0,0,SIDE_W,H);
  Pen edge(C_VDIM,2); DLine(g,&edge,SIDE_W-1,0,SIDE_W-1,H);
  SolidBrush dot(C_VIOLET); FEll(g,&dot,18,20,9,9);
  drawStr(g,L"SENTINEL",fH,C_TX,34,15);
  drawStr(g,L"WATCH SUITE",fMono,C_TXM,34,36);

  const wchar_t* nav[4]={L"Dashboard",L"Protection",L"Events",L"Updates"};
  for(int i=0;i<4;i++){ REAL y=78+i*46;
    g_rNav[i]={12,(LONG)y,SIDE_W-12,(LONG)y+40};
    if(i==g_tab){ fillRound(g,12,y,SIDE_W-24,40,9,Color(48,150,124,246));
      SolidBrush vb(C_VIOLET); FRect(g,&vb,12,y+6,3,28); }
    else if(hov(g_rNav[i])) fillRound(g,12,y,SIDE_W-24,40,9,Color(24,150,124,246));
    drawStr(g,nav[i],fBody,(i==g_tab||hov(g_rNav[i]))?C_TX:C_TXD,30,y+9);
  }
  { REAL qy=(REAL)H-214;
    drawStr(g, g_afkNow?L"\u25cf AFK":L"\u25cf LIVE", fMono, g_afkNow?C_VDIM:C_GREEN, 18, qy);
    if(!g_curQuote.empty()){
      const REAL qBoxH=150, qBoxW=(REAL)SIDE_W-36;
      RectF qc(16,qy+22,(REAL)SIDE_W-30,qBoxH); g.SetClip(qc);
      double sel=(double)(GetTickCount64()-g_quoteChangeT0)/1000.0;
      bool glitch=(!g_afkNow && sel<0.75);
      Color qcCol = g_afkNow?Color(g_quoteAlpha,198,206,226):Color(255,198,206,226);
      const RichFont* rf=fitRich(g,g_curQuote,qBoxW,qBoxH-8,g_ladderSide);   // -8 keeps a little breathing room
      if(rf) drawRich(g,g_curQuote,18,qy+26,qBoxW,qcCol, glitch?sel:-1, 0.03, rf->n,rf->b,rf->lh);
      else   drawRich(g,g_curQuote,18,qy+26,qBoxW,qcCol, glitch?sel:-1, 0.03);
      g.ResetClip(); } }
  { std::wstring gl; static const wchar_t* G=L"01<>/\\#*+=x"; for(int i=0;i<12;i++) gl+=G[rand()%10];
    drawStr(g,gl.c_str(),fMono,Color(70,120,160,210),18,H-42);
    drawStr(g,L"SENTINEL // ONLINE",fMono,C_TXM,18,H-24); }

  // ---- top bar ----
  REAL cx=SIDE_W+24;
  const wchar_t* titles[4]={L"DASHBOARD",L"PROTECTION",L"EVENTS",L"UPDATES"};
  double tel=(GetTickCount64()-g_tabT0)/1000.0;
  std::wstring tt = tel<0.6 ? scramble(titles[g_tab],tel,0.04) : std::wstring(titles[g_tab]);
  if(tel<0.6) g_dirty.store(true);
  drawStr(g,tt.c_str(),fTitle,C_TX,cx,16);
  g_rScanTop={(LONG)(cx+232),12,(LONG)(cx+348),38};
  fillRound(g,cx+232,12,116,26,7, hov(g_rScanTop)?C_VIOLET:C_VDIM);
  drawStr(g,L"Quick scan",fMono,Color(255,255,255,255),cx+252,16);
  drawStr(g,L"\u203a\u203a\u203a",fTitle,C_VDIM,cx+180,16);
  // clock
  SYSTEMTIME st; GetLocalTime(&st); wchar_t clk[16]; wsprintfW(clk,L"%02d:%02d:%02d",st.wHour,st.wMinute,st.wSecond);
  drawStrR(g,clk,fMono,C_TXD,W-150,20);
  // version chip + window buttons (fullscreen is F11 now, not a button -- see WM_KEYDOWN)
  fillRound(g,W-130,14,44,24,6,C_PANEL); drawStr(g,L"v1.6",fMono,C_VIOLET,W-122,17);
  g_rMin={W-76,14,W-50,38}; g_rClose={W-44,14,W-18,38};
  fillRound(g,W-76,14,26,24,6,hov(g_rMin)?C_LINE2:C_PANEL); drawStr(g,L"\u2013",fBody,hov(g_rMin)?C_TX:C_TXD,W-68,15);
  fillRound(g,W-44,14,26,24,6,hov(g_rClose)?C_RED:C_PANEL); drawStr(g,L"\u00d7",fBody,hov(g_rClose)?Color(255,255,255,255):C_TXD,W-37,15);

  REAL top=BAR_H+14, W2=W-cx-24;

  auto seconds=[&](){ ULONGLONG base=g_sessionStart?g_sessionStart:g_start; unsigned long long s=(GetTickCount64()-base)/1000;
    wchar_t b[16]; wsprintfW(b,L"%02u:%02u:%02u",(unsigned)(s/3600),(unsigned)(s%3600/60),(unsigned)(s%60)); return std::wstring(b); };

  if(g_tab==0){ // DASHBOARD
    REAL cw=(W2-32)/3;
    struct{const wchar_t* l; std::wstring v; const wchar_t* h;} sc[3]={
      {L"SESSION",seconds(),L"this run"},
      {L"PROCESSES SEEN",std::to_wstring(g_procCount.load()),L"since launch"},
      {L"NEW FILES SEEN",std::to_wstring(g_fileCount.load()),L"watched folders"}};
    for(int i=0;i<3;i++){ REAL x=cx+i*(cw+16); card(g,x,top,cw,96); label(g,sc[i].l,x+16,top+14);
      drawStr(g,sc[i].v.c_str(),fBig,C_TX,x+16,top+34);
      drawStr(g,sc[i].h,fMono,C_TXM,x+16,top+74); }
    // events + side column
    REAL fy=top+112, fh=H-fy-20, fw=W2*0.62f;
    card(g,cx,fy,fw,fh); label(g,L"RECENT EVENTS",cx+16,fy+14);
    SolidBrush lg(C_GREEN); FEll(g,&lg,cx+fw-118,fy+16,7,7);
    drawStr(g,L"MONITORING",fMono,C_GREEN,cx+fw-104,fy+12);
    { REAL ry=fy+44; int n=0;
      for(auto&e:evs){ if(n++>=9)break;
        Color tc=C_VIOLET; if(e.tag==L"WARN")tc=C_RED; else if(e.tag==L"FILE")tc=C_GREEN; else if(e.tag==L"SYS")tc=C_TXM;
        strokeRound(g,cx+16,ry,52,18,4,tc); drawStr(g,e.tag.c_str(),fMono,tc,cx+22,ry+1);
        { RectF nc(cx+78,ry-4,fw-78-62,36); g.SetClip(nc);
          drawStr(g,e.name.c_str(),fBody,C_TX,cx+78,ry-2);
          drawStr(g,e.path.c_str(),fMono,C_TXM,cx+78,ry+14); g.ResetClip(); }
        drawStrR(g,e.tm,fMono,C_TXM,cx+fw-16,ry);
        ry+=42; }
    }
    // radar + autoruns
    REAL rx=cx+fw+16, rw=W2-fw-16;
    card(g,rx,fy,rw,fh*0.58f); label(g,L"WATCHERS",rx+16,fy+14);
    REAL ccx=rx+rw/2, ccy=fy+fh*0.30f, rad=44;
    Pen rp(C_LINE2,1); DEll(g,&rp,ccx-rad,ccy-rad,rad*2,rad*2);
    DEll(g,&rp,ccx-rad*0.6f,ccy-rad*0.6f,rad*1.2f,rad*1.2f);
    REAL ang=(REAL)fmod(now*90.0,360.0);
    GraphicsPath pie; pie.AddPie((REAL)(ccx-rad),(REAL)(ccy-rad),(REAL)(rad*2),(REAL)(rad*2),(REAL)ang,(REAL)60);
    SolidBrush pb(Color(85,150,124,246)); g.FillPath(&pb,&pie);
    drawStr(g,L"4 / 4 active",fBody,C_TX,rx+16,fy+fh*0.44f);
    card(g,rx,fy+fh*0.62f,rw,fh*0.38f); label(g,L"AUTORUNS",rx+16,fy+fh*0.62f+14);
    drawStr(g,std::to_wstring((int)g_known.size()).c_str(),fBig,C_TX,rx+16,fy+fh*0.62f+32);
  }
  else if(g_tab==1){ // PROTECTION
    card(g,cx,top,W2,H-top-72); label(g,L"WATCHERS",cx+16,top+14);
    { bool on=g_tTokens.load(); Color sc=on?C_GREEN:C_TXM;
      drawStrR(g,on?L"TOKEN GUARD \u25cf ON":L"TOKEN GUARD \u25cb OFF",fMono,sc,cx+W2-16,top+12);
      if(g_tokenAlerts.load()>0)
        drawStrR(g,(std::to_wstring(g_tokenAlerts.load())+L" token alert(s)").c_str(),fMono,C_RED,cx+W2-16,top+28); }
    for(int i=0;i<TOG_N;i++){ REAL y=top+44+i*46; bool on=g_toggles[i].v->load();
      drawStr(g,g_toggles[i].name,fBody,C_TX,cx+16,y);
      drawStr(g,g_toggles[i].desc,fMono,C_TXM,cx+16,y+18);
      REAL swx=cx+W2-80, swy=y+2;
      g_rTog[i]={(LONG)swx,(LONG)swy,(LONG)swx+46,(LONG)swy+26};
      fillRound(g,swx,swy,46,26,13, on?C_VDIM:Color(255,12,13,24));
      if(!on)strokeRound(g,swx,swy,46,26,13,C_LINE2);
      SolidBrush kn(on?Color(255,255,255,255):C_TXM);
      FEll(g,&kn, on?swx+22:swx+3, swy+3, 20,20);
      if(i<TOG_N-1){ Pen ln(C_LINE,1); DLine(g,&ln,cx+16,y+38,cx+W2-16,y+38); }
    }
    // scan button
    g_rScan={(LONG)cx,(LONG)(H-58),(LONG)cx+150,(LONG)(H-24)};
    fillRound(g,cx,H-58,150,34,9, hov(g_rScan)?C_VIOLET:C_VDIM); drawStr(g,L"Quick scan",fBody,Color(255,255,255,255),cx+38,H-52);
    g_rFw={(LONG)(cx+164),(LONG)(H-58),(LONG)(cx+164+210),(LONG)(H-24)};
    fillRound(g,cx+164,H-58,210,34,9, hov(g_rFw)?C_LINE2:C_PANEL); strokeRound(g,cx+164,H-58,210,34,9,C_LINE);
    drawStr(g,L"Open Windows Firewall",fBody,C_TX,cx+180,H-52);
  }
  else if(g_tab==2){ // EVENTS
    card(g,cx,top,W2,H-top-20); label(g,L"EVENT STREAM",cx+16,top+14);
    SolidBrush lg(C_GREEN); FEll(g,&lg,cx+W2-70,top+16,7,7); drawStr(g,L"LIVE",fMono,C_GREEN,cx+W2-56,top+12);
    int total=(int)evs.size(); int rows=(int)((H-top-20-44)/40); if(rows<1)rows=1;
    int maxScroll=total-rows; if(maxScroll<0)maxScroll=0; if(g_evScroll>maxScroll)g_evScroll=maxScroll; if(g_evScroll<0)g_evScroll=0;
    REAL ry=top+44; g_evRowCount=0;
    for(int i=g_evScroll;i<total && i<g_evScroll+rows;i++){ auto&e=evs[i]; bool warn=(e.tag==L"WARN");
      Color tc=C_VIOLET; if(warn)tc=C_RED; else if(e.tag==L"FILE")tc=C_GREEN; else if(e.tag==L"SYS")tc=C_TXM;
      if(g_evRowCount<40){ g_evRowR[g_evRowCount]={(LONG)(cx+12),(LONG)(ry-6),(LONG)(cx+W2-12),(LONG)(ry+32)};
        g_evRowPath[g_evRowCount]=e.path; g_evRowWarn[g_evRowCount]=warn; g_evRowCount++; }
      if(warn && hov({(LONG)(cx+12),(LONG)(ry-6),(LONG)(cx+W2-12),(LONG)(ry+32)}))
        fillRound(g,cx+12,ry-6,W2-24,38,7,Color(26,248,116,140));
      strokeRound(g,cx+16,ry,52,18,4,tc); drawStr(g,e.tag.c_str(),fMono,tc,cx+22,ry+1);
      { RectF nc(cx+78,ry-4,W2-78-72,36); g.SetClip(nc);
        drawStr(g,e.name.c_str(),fBody,C_TX,cx+78,ry-2);
        drawStr(g,e.path.c_str(),fMono,C_TXM,cx+78,ry+14); g.ResetClip(); }
      drawStrR(g,e.tm,fMono,C_TXM,cx+W2-16,ry); ry+=40; }
    if(total>rows){ REAL trackH=(REAL)(H-top-20-44); REAL th=trackH*rows/total; REAL ty=top+44+trackH*g_evScroll/total;
      fillRound(g,cx+W2-8,top+44,3,trackH,1,C_LINE); fillRound(g,cx+W2-8,ty,3,th,1,C_VDIM); }
    drawStr(g,L"scroll to see more · click a red WARN row to block it in Windows Firewall",fMono,C_TXM,cx+16,(REAL)H-30);
  }
  else { // UPDATES & CREDITS
    REAL logW=W2*0.60f, rx=cx+logW+16, rw=W2-logW-16;
    card(g,cx,top,logW,H-top-20); label(g,L"UPDATE LOG",cx+16,top+14);
    REAL ly=top+44;
    for(auto&nt:g_notes){ if(ly>H-38)break;
      Color kc=(wcscmp(nt.kind,L"FIX")==0)?C_GREEN:(wcscmp(nt.kind,L"IMPROVE")==0)?C_VIOLET:C_VDIM;
      strokeRound(g,cx+16,ly,66,18,4,kc); drawStr(g,nt.kind,fMono,kc,cx+21,ly+1);
      drawStr(g,nt.ver,fMono,C_TXM,cx+90,ly+1);
      { RectF tc(cx+138,ly-3,logW-138-16,22); g.SetClip(tc);
        drawStr(g,nt.text,fBody,C_TX,cx+138,ly-2); g.ResetClip(); }
      ly+=32; }
    // Creator (hover-zoom) grows while Device Info shrinks, smoothly
    REAL totalH=(REAL)H-top-20, gap=16;
    // Social row geometry. The card height is derived from these rather than
    // being its own constant -- that is what let the third row (Discord) fall
    // outside the card at rest: rows ended at top+166, the card at top+150.
    const REAL socY0=96, socY0G=24;      // first row offset; +G when hovered
    const REAL socStep=24, socStepG=8;   // row pitch;        +G when hovered
    const REAL socRowH=22, socPadB=16;   // row height, bottom padding
    auto creatorNeed=[&](REAL t){ return socY0+t*socY0G + 2*(socStep+t*socStepG) + socRowH + socPadB; };
    REAL base=creatorNeed(0.0f);                  // 182: last row + padding fits
    REAL grow=(creatorNeed(1.0f)+32.0f)-base;     // hovered keeps a little slack
    { REAL tgt=hov(g_rCreator)?1.0f:0.0f; g_creatorT+=(tgt-g_creatorT)*0.18f;
      if(g_creatorT<0.002f)g_creatorT=0; if(g_creatorT>0.998f)g_creatorT=1; }
    REAL ch=base+g_creatorT*grow;
    g_rCreator={(LONG)rx,(LONG)top,(LONG)(rx+rw),(LONG)(top+ch)};
    card(g,rx,top,rw,ch); label(g,L"CREATOR",rx+16,top+14);
    REAL av=44+g_creatorT*20;
    if(g_avatar){ GraphicsPath cp; cp.AddEllipse((REAL)(rx+16),(REAL)(top+38),(REAL)av,(REAL)av); g.SetClip(&cp);
      DImg(g,g_avatar,(REAL)rx+16,(REAL)top+38,av,av); g.ResetClip();
      Pen ap(C_LINE2,1); DEll(g,&ap,rx+16,top+38,av,av); }
    drawStr(g,L"Lunaris",fH,C_TX,rx+72+g_creatorT*10,top+42);
    drawStr(g,L"OPERATOR",fMono,C_GREEN,rx+72+g_creatorT*10,top+66);
    const wchar_t* soc[3]={L"YouTube  @lIIusionator",L"Roblox  Profile",L"Discord  liiusionator"};
    for(int i=0;i<3;i++){ REAL y=top+socY0+g_creatorT*socY0G+i*(socStep+g_creatorT*socStepG);
      g_rSoc[i]={(LONG)(rx+16),(LONG)y,(LONG)(rx+rw-16),(LONG)(y+socRowH)};
      SolidBrush vb(C_VIOLET); FEll(g,&vb,rx+20,y+7,8,8);
      { RectF sc(rx+34,y,rw-40,socRowH); g.SetClip(sc); drawStr(g,soc[i],fMono,hov(g_rSoc[i])?C_TX:C_TXD,rx+34,y+2); g.ResetClip(); } }
    // Device Info (shrinks as Creator grows)
    REAL dy=top+ch+gap, dh=totalH-ch-gap;
    if(dh>44){ card(g,rx,dy,rw,dh); label(g,L"DEVICE INFO",rx+16,dy+14);
      ULONGLONG up=GetTickCount64()/1000; wchar_t ub[32]; wsprintfW(ub,L"%uh %um",(unsigned)(up/3600),(unsigned)(up%3600/60));
      struct{ const wchar_t* k; std::wstring v; } rows[]={
        {L"HOST",g_devHost},{L"USER",g_userName},{L"CPU",g_devCPU},{L"MEMORY",g_devRAM},{L"DISPLAY",g_devRes},{L"UPTIME",ub}};
      REAL ry2=dy+44;
      for(auto&r:rows){ if(ry2>dy+dh-16)break;
        drawStr(g,r.k,fMono,C_TXM,rx+16,ry2);
        { RectF vc(rx+92,ry2-2,rw-92-16,22); g.SetClip(vc); drawStrR(g,r.v,fBody,C_TX,rx+rw-16,ry2-2); g.ResetClip(); }
        ry2+=28; } }
  }

  // ---- press flash animation ----
  { double fel=(GetTickCount64()-g_flashT0)/1000.0;
    if(fel>=0&&fel<0.18&&g_flashR.right>g_flashR.left){
      int a=(int)(80*(1.0-fel/0.18));
      fillRound(g,(REAL)g_flashR.left,(REAL)g_flashR.top,(REAL)(g_flashR.right-g_flashR.left),(REAL)(g_flashR.bottom-g_flashR.top),8,Color(a,255,255,255));
      g_dirty.store(true); } }

  // ---- CRT scanlines ----
  for(int y=0;y<H;y+=3){ SolidBrush sl(Color(22,0,0,0)); FRect(g,&sl,0,y,W,1); }

  // AFK content no longer draws here -- it's a dedicated always-on-top
  // window now (g_afkWin, driven from g_msg's timer) so the screensaver keeps
  // working even with this window closed/unfocused/behind other windows.
  Graphics wg(dc); wg.DrawImage(g_back,0,0);
}

// ---------- dashboard window ----------
static void openDash();
// Work-area fullscreen toggle for the borderless dashboard window. Not a
// real Win32 "maximize" (this window has no native chrome/max button to
// begin with) -- just resizes to the work area and back, remembering the
// windowed rect to restore to.
static void toggleFullscreen(HWND h){
  if(!g_fullscreen){
    GetWindowRect(h,&g_preFullRect);
    RECT wa; SystemParametersInfoW(SPI_GETWORKAREA,0,&wa,0);
    SetWindowPos(h,0,wa.left,wa.top,wa.right-wa.left,wa.bottom-wa.top,SWP_NOZORDER);
    g_fullscreen=true;
  } else {
    int w=g_preFullRect.right-g_preFullRect.left, ht=g_preFullRect.bottom-g_preFullRect.top;
    if(w>0&&ht>0) SetWindowPos(h,0,g_preFullRect.left,g_preFullRect.top,w,ht,SWP_NOZORDER);
    g_fullscreen=false;
  }
}
static LRESULT CALLBACK DashProc(HWND h,UINT m,WPARAM w,LPARAM l){
  switch(m){
  case WM_CREATE: g_splash=true; g_splashT0=GetTickCount64(); g_tabT0=GetTickCount64(); g_dirty.store(true);
    SetTimer(h,TMR_RENDER,33,0); return 0;
  case WM_TIMER: if(w==TMR_RENDER&&!IsIconic(h)){ RECT rc; GetClientRect(h,&rc);
      if(rc.right>0&&rc.bottom>0){ HDC dc=GetDC(h); renderScene(dc,rc.right,rc.bottom); ReleaseDC(h,dc);} } return 0;
  case WM_GETMINMAXINFO:{ MINMAXINFO* mmi=(MINMAXINFO*)l;   // clamp how small the user can drag it
      mmi->ptMinTrackSize.x=DASH_MINW; mmi->ptMinTrackSize.y=DASH_MINH; return 0; }
  case WM_NCHITTEST:{ const LONG B=8;                       // 8px grab border -> resize a borderless popup
      POINT p={(LONG)(short)LOWORD(l),(LONG)(short)HIWORD(l)}; ScreenToClient(h,&p);
      RECT rc; GetClientRect(h,&rc);
      if(inR(g_rClose,p.x,p.y)||inR(g_rMin,p.x,p.y)||inR(g_rScanTop,p.x,p.y)) return HTCLIENT; // buttons win over the strip
      bool L_=p.x<B, R_=p.x>=rc.right-B, T_=p.y<B, D_=p.y>=rc.bottom-B;
      if(T_&&L_)return HTTOPLEFT;    if(T_&&R_)return HTTOPRIGHT;
      if(D_&&L_)return HTBOTTOMLEFT; if(D_&&R_)return HTBOTTOMRIGHT;
      if(L_)return HTLEFT; if(R_)return HTRIGHT; if(T_)return HTTOP; if(D_)return HTBOTTOM;
      return HTCLIENT; }
  case WM_ERASEBKGND: return 1;
  case WM_PAINT:{ PAINTSTRUCT ps; BeginPaint(h,&ps); RECT rc; GetClientRect(h,&rc);
      if(rc.right>0&&rc.bottom>0) renderScene(ps.hdc,rc.right,rc.bottom); EndPaint(h,&ps); return 0; }
  case WM_MOUSEWHEEL:{ int d=GET_WHEEL_DELTA_WPARAM(w); if(g_tab==2){ g_evScroll+=(d>0?-3:3); if(g_evScroll<0)g_evScroll=0; g_dirty.store(true);} return 0; }
  case WM_KEYDOWN: if(w==VK_F11){ toggleFullscreen(h); g_dirty.store(true); } return 0;
  case WM_MOUSEMOVE:{ g_mx=(int)(short)LOWORD(l); g_my=(int)(short)HIWORD(l); g_dirty.store(true);
    TRACKMOUSEEVENT tme={sizeof(tme),TME_LEAVE,h,0}; TrackMouseEvent(&tme); return 0; }
  case WM_MOUSELEAVE: g_mx=g_my=-1; g_dirty.store(true); return 0;
  case WM_LBUTTONDOWN:{ int x=LOWORD(l),y=HIWORD(l); g_dirty.store(true);
    auto flash=[&](RECT r){ g_flashR=r; g_flashT0=GetTickCount64(); };
    if(inR(g_rClose,x,y)){ DestroyWindow(h); return 0; }
    if(inR(g_rMin,x,y)){ flash(g_rMin); ShowWindow(h,SW_MINIMIZE); return 0; }
    if(inR(g_rScanTop,x,y)){ doScan(true); flash(g_rScanTop); return 0; }
    for(int i=0;i<4;i++) if(inR(g_rNav[i],x,y)){ g_tab=i; g_tabT0=GetTickCount64(); flash(g_rNav[i]); return 0; }
    if(g_tab==1){
      for(int i=0;i<TOG_N;i++) if(inR(g_rTog[i],x,y)){ g_toggles[i].v->store(!g_toggles[i].v->load()); saveSettings(); flash(g_rTog[i]); return 0; }
      if(inR(g_rScan,x,y)){ doScan(true); flash(g_rScan); return 0; }
      if(inR(g_rFw,x,y)){ flash(g_rFw);
        // firewall.cpl's registered verb is "cplopen", not "open" -- asking for
        // "open" finds no matching verb and ShellExecute silently no-ops, which
        // is why the button looked like it did nothing. control.exe accepts the
        // cpl name as a plain parameter, sidestepping verb lookup entirely.
        HINSTANCE r=ShellExecuteW(0,L"open",L"control.exe",L"firewall.cpl",0,SW_SHOW);
        if((INT_PTR)r<=32) r=ShellExecuteW(0,L"open",L"rundll32.exe",L"shell32.dll,Control_RunDLL firewall.cpl",0,SW_SHOW);
        if((INT_PTR)r<=32) MessageBoxW(h,L"Could not open Windows Firewall settings.",L"Sentinel",MB_OK|MB_ICONWARNING);
        return 0; } }
    if(g_tab==2){ for(int i=0;i<g_evRowCount;i++) if(inR(g_evRowR[i],x,y) && g_evRowWarn[i] && !g_evRowPath[i].empty()){
      std::wstring m=L"Block this app's internet access using Windows Firewall?\n\n"+g_evRowPath[i]+L"\n\n(You'll get a UAC prompt to apply the rule.)";
      if(MessageBoxW(h,m.c_str(),L"Sentinel \u2014 Firewall",MB_YESNO|MB_ICONWARNING)==IDYES) blockInFirewall(g_evRowPath[i]);
      return 0; } }
    if(g_tab==3){ for(int i=0;i<3;i++) if(inR(g_rSoc[i],x,y)){ flash(g_rSoc[i]);
        if(i==2){ if(OpenClipboard(h)){ EmptyClipboard();
            const wchar_t* d=L"liiusionator"; HGLOBAL hm=GlobalAlloc(GMEM_MOVEABLE,(wcslen(d)+1)*2);
            memcpy(GlobalLock(hm),d,(wcslen(d)+1)*2); GlobalUnlock(hm);
            SetClipboardData(CF_UNICODETEXT,hm); CloseClipboard(); } }
        else ShellExecuteW(0,L"open",g_socUrl[i],0,0,SW_SHOW); return 0; } }
    // drag from top bar
    if(y<BAR_H){ ReleaseCapture(); SendMessageW(h,WM_NCLBUTTONDOWN,HTCAPTION,0); }
    return 0; }
  case WM_SIZE:
    if(w==SIZE_MINIMIZED){ SetTimer(h,TMR_RENDER,200,0); g_frameMs=200; }       // 5 fps minimized
    else { SetTimer(h,TMR_RENDER,33,0); g_frameMs=33; g_dirty.store(true);      // 30 fps active
      g_dashW=LOWORD(l); g_dashH=HIWORD(l); }        // borderless popup: client size == window size
    return 0;
  case WM_EXITSIZEMOVE: saveSettings(); return 0;     // write size to JSON once, after the drag ends
  case WM_CLOSE: DestroyWindow(h); return 0;                   // close -> headless tray
  case WM_DESTROY: KillTimer(h,TMR_RENDER); g_dash=nullptr; return 0;
  }
  return DefWindowProcW(h,m,w,l);
}
static void openDash(){
  if(!g_dash){
    g_dash=CreateWindowExW(WS_EX_APPWINDOW,CLS_DASH,L"Sentinel Watch Suite",
      WS_POPUP|WS_MINIMIZEBOX,CW_USEDEFAULT,CW_USEDEFAULT,g_dashW,g_dashH,0,0,g_hInst,0);
    // center at the persisted size
    RECT wa; SystemParametersInfoW(SPI_GETWORKAREA,0,&wa,0);
    SetWindowPos(g_dash,0,(wa.right-g_dashW)/2,(wa.bottom-g_dashH)/2,0,0,SWP_NOSIZE|SWP_NOZORDER);
  }
  ShowWindow(g_dash,SW_SHOW); SetForegroundWindow(g_dash); UpdateWindow(g_dash);
}

// ---------- tray / message window ----------
// ---------- dedicated AFK screensaver window ----------
// A plain, opaque, borderless popup -- structurally identical to how g_dash
// already renders itself (GDI+ into a backbuffer, blitted via GetDC/BeginPaint).
// No layering, no child-window tricks, nothing EVR-specific: this is the same
// mechanism already proven working in g_dash, just in a second window that
// exists independently of it, so the screensaver isn't tied to the dashboard
// being open, focused, or on top.
static LRESULT CALLBACK AfkScrProc(HWND h,UINT m,WPARAM w,LPARAM l){
  switch(m){
  case WM_ERASEBKGND: return 1;   // we always paint the full client area ourselves -- skip the default flash-to-white
  case WM_PAINT:{ PAINTSTRUCT ps; HDC dc=BeginPaint(h,&ps);
    if(g_afkBack&&g_afkBackDC){ BitBlt(dc,0,0,g_afkBw,g_afkBh,g_afkBackDC,0,0,SRCCOPY); }
    EndPaint(h,&ps); return 0; }
  }
  return DefWindowProcW(h,m,w,l);
}
// Renders one frame of the screensaver into g_afkWin. Recreates the backbuffer
// only when the window's size actually changes (it does, once, when first
// sized to the current monitor) -- same pattern as g_dash's g_back.
static void renderAfkWin(){
  if(!g_afkWin) return;
  int64_t t0=afkHnsNow();
  RECT rc; GetClientRect(g_afkWin,&rc); int W=rc.right-rc.left,H=rc.bottom-rc.top;
  if(W<=0||H<=0) return;
  afkEnsureBackbuffer(W,H);
  if(!g_afkBack||g_afkBack->GetLastStatus()!=Ok||!g_afkBackDC) return;
  Graphics g(g_afkBack);
  g.SetSmoothingMode(SmoothingModeAntiAlias);
  g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
  SolidBrush bg(Color(255,6,8,16)); FRect(g,&bg,0,0,(REAL)W,(REAL)H);   // opaque base -- this window owns its whole screen, nothing else to show through
  drawAfkOverlay(g,W,H);
  int64_t t1=afkHnsNow();
  // Raw BitBlt, not GDI+: same size, same 32bpp format, no transform -- a plain memory copy, and
  // BitBlt is the cheapest API that does exactly that. g_afkBackDC already has the DIB section
  // selected into it, so this reads directly from the same memory GDI+ just drew into.
  HDC dc=GetDC(g_afkWin); BitBlt(dc,0,0,W,H,g_afkBackDC,0,0,SRCCOPY); ReleaseDC(g_afkWin,dc);
  int64_t t2=afkHnsNow();
  g_rN++; g_rTotUs+=(t2-t0)/10; g_rPresUs+=(t2-t1)/10;
  static ULONGLONG s_logT=0; ULONGLONG nowMs=GetTickCount64(); if(!s_logT) s_logT=nowMs;
  if(nowMs-s_logT>=5000){ afkLogStats(nowMs-s_logT); s_logT=nowMs; }
}

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
      updateQuoteState();               // AFK detection itself lives here now -- runs regardless of g_dash
      bool showing = g_afkNow || g_afkA>0;   // keep showing through the fade-OUT too, not just fade-in
      if(showing){
        if(!IsWindowVisible(g_afkWin)){
          // Whichever monitor the cursor is on -- a reasonable proxy for
          // "the one the user was looking at" right as they went idle, and
          // it works with no app window to anchor to (g_dash may be closed).
          POINT cp; GetCursorPos(&cp);
          HMONITOR mon=MonitorFromPoint(cp,MONITOR_DEFAULTTONEAREST);
          MONITORINFO mi; ZeroMemory(&mi,sizeof(mi)); mi.cbSize=sizeof(mi);
          if(GetMonitorInfoW(mon,&mi))
            // Actually takes focus now (SWP_NOACTIVATE dropped) -- a real
            // screensaver, not just a topmost overlay sitting behind
            // whatever still technically owns the foreground. Beyond just
            // being the expected screensaver behavior, DWM/the compositor
            // and the GPU scheduler both tend to prioritize present() calls
            // for whichever window is actually foreground/active, so this
            // also gives the video's own rendering a better shot at a
            // steady frame rate than a topmost-but-background window would.
            SetWindowPos(g_afkWin,HWND_TOPMOST,mi.rcMonitor.left,mi.rcMonitor.top,
              mi.rcMonitor.right-mi.rcMonitor.left,mi.rcMonitor.bottom-mi.rcMonitor.top,
              SWP_SHOWWINDOW);
          SetForegroundWindow(g_afkWin); BringWindowToTop(g_afkWin); SetFocus(g_afkWin);
        }
        renderAfkWin();
        if((int)g_afkTimerMs!=g_afkTickWant){ g_afkTimerMs=(UINT)g_afkTickWant; SetTimer(h,TMR_AFK,(UINT)g_afkTickWant,0); }   // ~30fps while visible; retuned from measured render cost (afkLogStats)
      } else {
        if(IsWindowVisible(g_afkWin)) ShowWindow(g_afkWin,SW_HIDE);
        if(g_afkTimerMs!=1000){ g_afkTimerMs=1000; SetTimer(h,TMR_AFK,1000,0); } // cheap idle poll otherwise
      }
    }
    return 0;
  case WM_SHOWTOAST:{ Toast* t=(Toast*)l; if(t){ if(g_tNotify.load()){
      g_ovlTitle=t->title; g_ovlText=t->text; g_ovlAccent=t->accent;
      RECT wa; SystemParametersInfoW(SPI_GETWORKAREA,0,&wa,0);
      SetWindowPos(g_ovl,HWND_TOPMOST,wa.right-416,wa.bottom-166,400,150,SWP_NOACTIVATE);
      ShowWindow(g_ovl,SW_SHOWNOACTIVATE); InvalidateRect(g_ovl,0,TRUE);
      SetTimer(g_ovl,TMR_HIDEOVL,5000,0);
      if(g_tBeep.load()){ Beep(1000,120); Beep(1400,120); } }
    delete t; } return 0; }
  case WM_DESTROY: Shell_NotifyIconW(NIM_DELETE,&g_nid); PostQuitMessage(0); return 0;
  }
  return DefWindowProcW(h,m,w,l);
}

// ---------- avatar cache ----------
static HICON makeMoon(){
  Bitmap bmp(32,32,PixelFormat32bppPARGB); Graphics g(&bmp);
  g.SetSmoothingMode(SmoothingModeAntiAlias); g.Clear(Color(0,0,0,0));
  SolidBrush glow(Color(60,150,124,246)); g.FillEllipse(&glow,2,2,29,29);
  SolidBrush moon(Color(255,150,205,255)); g.FillEllipse(&moon,4,3,25,25);
  g.SetCompositingMode(CompositingModeSourceCopy);
  SolidBrush hole(Color(0,0,0,0)); g.FillEllipse(&hole,12,1,25,25);
  HICON ic=nullptr; bmp.GetHICON(&ic); return ic;
}
static void loadAvatar(){
  std::wstring path=cacheDir()+L"\\avatar.jpg";
  if(GetFileAttributesW(path.c_str())==INVALID_FILE_ATTRIBUTES){
    static const char* B64="<<B64 JPEG 10330 bytes -> embedded_img.bin>>";
    DWORD cb=0; CryptStringToBinaryA(B64,0,CRYPT_STRING_BASE64,0,&cb,0,0);
    std::vector<BYTE> bin(cb); CryptStringToBinaryA(B64,0,CRYPT_STRING_BASE64,bin.data(),&cb,0,0);
    std::ofstream f(toU8(path),std::ios::binary); f.write((char*)bin.data(),cb);
  }
  g_avatar=Bitmap::FromFile(path.c_str());
  if(g_avatar&&g_avatar->GetLastStatus()!=Ok){ delete g_avatar; g_avatar=nullptr; }
}

// ---------- entry ----------
int WINAPI wWinMain(HINSTANCE hi,HINSTANCE,PWSTR,int){
  g_hInst=hi; g_start=GetTickCount64(); srand((unsigned)g_start);
  cacheDir(); // create %LOCALAPPDATA%\\SentinelWatch\\cache up front
  InitializeCriticalSection(&g_cs); InitializeCriticalSection(&g_vcs); InitializeCriticalSection(&g_clkcs);
  CoInitializeEx(0,COINIT_APARTMENTTHREADED);
  CoInitializeSecurity(0,-1,0,0,RPC_C_AUTHN_LEVEL_DEFAULT,RPC_C_IMP_LEVEL_IMPERSONATE,0,EOAC_NONE,0);
  GdiplusStartupInput gsi; GdiplusStartup(&g_gdip,&gsi,0);

  famUI=new FontFamily(L"Segoe UI"); famMono=new FontFamily(L"Consolas");
  fTitle=new Font(famUI,22,FontStyleBold,UnitPixel);
  fBig  =new Font(famUI,30,FontStyleBold,UnitPixel);
  fH    =new Font(famUI,15,FontStyleBold,UnitPixel);
  fBody =new Font(famUI,14,FontStyleRegular,UnitPixel);
  fLbl  =new Font(famMono,10,FontStyleRegular,UnitPixel);
  fMono =new Font(famMono,11,FontStyleRegular,UnitPixel);
  fMonoBig=new Font(famMono,26,FontStyleBold,UnitPixel);
  fBodyB=new Font(famUI,17,FontStyleBold,UnitPixel);   // bigger for **emphasis**
  fQuote=new Font(famUI,26,FontStyleRegular,UnitPixel);
  fQuoteB=new Font(famUI,30,FontStyleBold,UnitPixel);
  fClock=new Font(famMono,46,FontStyleBold,UnitPixel);
  // Type ladders used to fit quotes into their fixed boxes. Ratios match the
  // originals: bold runs sit a few px larger than body, line height ~1.93x
  // (sidebar) and ~1.38x (full screen) the regular size.
  for(REAL sz : {14.0f,13.0f,12.0f,11.0f,10.0f,9.0f})
    g_ladderSide.push_back({ new Font(famUI,sz,FontStyleRegular,UnitPixel),
                             new Font(famUI,sz+3,FontStyleBold,UnitPixel), sz*1.93f });
  for(REAL sz : {26.0f,23.0f,20.0f,18.0f,16.0f,14.0f})
    g_ladderBig.push_back({ new Font(famUI,sz,FontStyleRegular,UnitPixel),
                            new Font(famUI,sz+4,FontStyleBold,UnitPixel), sz*1.385f });

  loadAvatar(); g_moon=makeMoon();

  HANDLE mtx=CreateMutexW(0,TRUE,L"Sentinel_Single");
  if(GetLastError()==ERROR_ALREADY_EXISTS){ if(mtx)CloseHandle(mtx); return 0; }

  WNDCLASSW wcm={}; wcm.lpfnWndProc=MsgProc; wcm.hInstance=hi; wcm.lpszClassName=CLS_MSG; wcm.hIcon=g_moon; RegisterClassW(&wcm);
  WNDCLASSW wcd={}; wcd.lpfnWndProc=DashProc; wcd.hInstance=hi; wcd.lpszClassName=CLS_DASH;
  wcd.hCursor=LoadCursorW(0,IDC_ARROW); wcd.hIcon=g_moon; RegisterClassW(&wcd);
  WNDCLASSW wco={}; wco.lpfnWndProc=OvlProc; wco.hInstance=hi; wco.lpszClassName=CLS_OVL;
  wco.hCursor=LoadCursorW(0,IDC_ARROW); RegisterClassW(&wco);
  WNDCLASSW wca={}; wca.lpfnWndProc=AfkScrProc; wca.hInstance=hi; wca.lpszClassName=CLS_AFKSCR; RegisterClassW(&wca);


  g_msg=CreateWindowExW(0,CLS_MSG,L"",0,0,0,0,0,HWND_MESSAGE,0,hi,0);
  g_ovl=CreateWindowExW(WS_EX_LAYERED|WS_EX_TOPMOST|WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE,
    CLS_OVL,L"",WS_POPUP,0,0,400,150,0,0,hi,0);
  SetLayeredWindowAttributes(g_ovl,0,240,LWA_ALPHA);
  // Created once, up front, independent of g_dash -- hidden until AFK first
  // triggers. No WS_EX_LAYERED: plain opaque GDI+ blit, the same mechanism
  // g_dash already uses, not the per-pixel-alpha path that caused trouble
  // for video quality earlier.
  g_afkWin=CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE,
    CLS_AFKSCR,L"",WS_POPUP,0,0,1,1,0,0,hi,0);
  SetTimer(g_msg,TMR_AFK,g_afkTimerMs,0);   // idle-poll timer; speeds up on its own once AFK actually shows

  g_nid.cbSize=sizeof(g_nid); g_nid.hWnd=g_msg; g_nid.uID=1;
  g_nid.uFlags=NIF_ICON|NIF_MESSAGE|NIF_TIP; g_nid.uCallbackMessage=WM_TRAY;
  g_nid.hIcon=g_moon?g_moon:LoadIconW(0,IDI_APPLICATION);
  lstrcpynW(g_nid.szTip,L"Sentinel Watch Suite",ARRAYSIZE(g_nid.szTip));
  Shell_NotifyIconW(NIM_ADD,&g_nid);

  // baseline + watchers
  { std::ifstream f(toU8(baseFile())); std::string ln;
    while(std::getline(f,ln)){ if(!ln.empty()&&ln.back()=='\r')ln.pop_back();
      if(!ln.empty()){ int n=MultiByteToWideChar(CP_UTF8,0,ln.data(),(int)ln.size(),0,0);
        std::wstring w(n,0); MultiByteToWideChar(CP_UTF8,0,ln.data(),(int)ln.size(),w.data(),n); g_known.insert(w); } } }
  if(g_known.empty()) g_known=scanAutoruns();
  g_wmi=CreateThread(0,0,WmiThread,0,0,0); if(g_wmi)SetThreadPriority(g_wmi,THREAD_PRIORITY_BELOW_NORMAL); // background monitor -- yields CPU to the video thread
  { HANDLE vt=CreateThread(0,0,VideoThread,0,0,0); if(vt){ SetThreadPriority(vt,THREAD_PRIORITY_ABOVE_NORMAL); CloseHandle(vt);} }  // AFK video decoder, paced off the audio session's clock (idles until AFK) -- elevated: everything else yields CPU to this one during playback
  g_audThread=CreateThread(0,0,AudioThread,0,0,0);                           // AFK audio renderer; publishes its session clock for VideoThread (idles until AFK)
  for(auto k:{envW(L"LOCALAPPDATA"),envW(L"APPDATA")}) if(!k.empty()){
    HANDLE t=CreateThread(0,0,DirThread,new std::wstring(k),0,0); if(t){ SetThreadPriority(t,THREAD_PRIORITY_BELOW_NORMAL); CloseHandle(t);} }  // background monitor -- yields CPU to the video thread
  { HANDLE tg=CreateThread(0,0,TokenGuardThread,0,0,0); if(tg){ SetThreadPriority(tg,THREAD_PRIORITY_BELOW_NORMAL); CloseHandle(tg);} } // Token Guard: watch handles to credential stores (idles when toggle off) -- yields CPU to the video thread
  SetTimer(g_msg,TMR_AUTOSCAN,300000,0);

  loadSettings(); saveSettings();   // persist toggle state as JSON in the app folder
  loadContent();                    // quotes / fun-facts config files
  materializeAfkVideo();            // unpack the built-in clip into content\afk (no-op once it is there)
  { HANDLE bt=CreateThread(0,0,DecodeBenchThread,0,0,0); if(bt){ SetThreadPriority(bt,THREAD_PRIORITY_BELOW_NORMAL); CloseHandle(bt);} }  // measure which decode config is fastest on THIS machine (background, once); also caches a first frame
  gatherDevice();                   // host / cpu / ram / display for the Device Info card
  { HANDLE pt=CreateThread(0,0,ProcScanThread,0,0,0); if(pt){ SetThreadPriority(pt,THREAD_PRIORITY_BELOW_NORMAL); CloseHandle(pt);} }  // list running apps at launch -- yields CPU to the video thread
  openDash();   // show on first launch

  MSG msg;
  while(GetMessageW(&msg,0,0,0)){ TranslateMessage(&msg); DispatchMessageW(&msg); }

  g_running.store(false);
  g_videoRun.store(false);
  if(g_audThread){ WaitForSingleObject(g_audThread,3000); CloseHandle(g_audThread); g_audThread=nullptr; }
  if(g_wmi){ WaitForSingleObject(g_wmi,2000); CloseHandle(g_wmi); }
  if(g_avatar)delete g_avatar;
  GdiplusShutdown(g_gdip);
  if(mtx)CloseHandle(mtx);
  return 0;
}
