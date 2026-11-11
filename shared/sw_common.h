// sw_common.h -- includes, ids and small helpers shared by every module of the Sentinel Watch Suite.
//
// The suite is one executable built from several modules (see README.md):
//   shared/        this header, logging, settings
//   core/          the security monitors (process / autorun / file / token watchers)
//   ui/            tray, toast, GDI+ dashboard, quote engine
//   afk/           the AFK screen (render thread, GPU + CPU back-ends, overlay)
//   media-engine/  Direct3D 11 + Media Foundation Media Engine video player (C ABI)
#pragma once
#include "platform.h"
#if !SW_OS_WINDOWS
#error "shared/ core/ ui/ afk/ media-engine/ are the Windows build (MinGW-w64). On macOS run ./build.sh (it builds mac/); on Linux ./build.sh cross-compiles the Windows exe."
#endif
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef _WIN32_DCOM
#define _WIN32_DCOM
#endif

#include <windows.h>
#include <string>
#include <vector>
#include <deque>
#include <set>
#include <atomic>
#include <filesystem>
#include <stdint.h>

#include "version.h"

// ---------- ids / constants ----------
static const wchar_t* const CLS_MSG    = L"SentinelMsg";
static const wchar_t* const CLS_DASH   = L"SentinelDash";
static const wchar_t* const CLS_OVL    = L"SentinelOvl";
static const wchar_t* const CLS_AFKSCR = L"SentinelAfkScreen";
static const UINT WM_TRAY      = WM_APP+1;
static const UINT WM_PROCEVT   = WM_APP+2;
static const UINT WM_SHOWTOAST = WM_APP+3;
static const UINT ID_OPEN=1, ID_SCAN=2, ID_NOTIFY=3, ID_EXIT=4;
static const UINT TMR_RENDER=10, TMR_AUTOSCAN=11, TMR_HIDEOVL=12, TMR_AFK=13;

// ---------- process-wide state ----------
extern HINSTANCE g_hInst;                 // module handle
extern HWND g_msg;                        // hidden message window (tray, timers, toasts)
extern std::atomic<bool> g_running;       // false once the app is shutting down
extern std::atomic<bool> g_dirty;         // dashboard needs a repaint
extern std::atomic<bool> g_ecoOn;         // power saving (shared/eco.h) is on: the CPU is capped, so a slow video is not the player's fault (core/eco.cpp)
extern ULONGLONG g_start;                 // GetTickCount64() at launch
extern std::wstring g_userName;           // display name used in quotes ({user})

// ---------- small utilities (shared/sw_common.cpp) ----------
std::wstring envW(const wchar_t* n);
std::string  toU8(const std::wstring& w);
std::wstring fromUtf8(const std::string& s);
std::wstring lc(std::wstring s);
bool         endsW(const std::wstring& s,const std::wstring& e);

// A path for std::ifstream / std::ofstream. Their narrow-string constructors read the ANSI code page, so a UTF-8 path (toU8) fails as soon as the
// Windows account name contains a character outside it; a std::filesystem::path built from the wide string makes the library open it with _wfopen.
inline std::filesystem::path wpath(const std::wstring& p){ return std::filesystem::path(p); }

std::wstring appDir();                    // %LOCALAPPDATA%\SentinelWatch (created on demand)
std::wstring cacheDir();
std::wstring logPath();                   // events.log

// Thread-safe, timestamped line appended to events.log.
void logLine(const std::wstring& s);
void logRotate();                         // call once at startup: keeps events.log from growing without bound
std::wstring hrHex(HRESULT hr);           // "0x80070057"
std::wstring fmt1(double v);              // one decimal place, no locale surprises
int64_t hnsNow();                         // monotonic clock in 100 ns units (QueryPerformanceCounter)

// Where the CPU goes. A thread calls cpuNameThread() first thing; the UI thread calls cpuReport() from its 1 s timer. Every `everyMs` (a minute) in which the app
// averaged 3 % or more of the whole CPU, events.log gets one "[CPU]" line that names the busiest threads. No line = the app stayed under 3 %.
void cpuNameThread(const wchar_t* name);
void cpuReport(ULONGLONG everyMs=60000);
