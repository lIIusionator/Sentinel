// events.h -- the shared event feed (dashboard "Recent events", Events tab, events.log) and toast hook.
#pragma once
#include "sw_common.h"

struct Ev{ std::wstring tag,name,path,tm; };
extern std::deque<Ev> g_events;
extern CRITICAL_SECTION g_cs;                       // guards g_events (call eventsInit() once at startup)
extern std::atomic<int> g_procCount, g_fileCount, g_tokenAlerts;

void eventsInit();
void pushEv(const wchar_t* tag,const std::wstring& name,const std::wstring& path);      // rate-capped (30/s)
void pushEvRaw(const wchar_t* tag,const std::wstring& name,const std::wstring& path);   // bypasses the cap (explicit scans)

// Toast popup, safe to call from any thread. Colours are 0xAARRGGBB so core code needs no GDI+.
typedef uint32_t ArgbColor;
static const ArgbColor ACC_RED    = 0xFFF8748Cu;   // (255,248,116,140)
static const ArgbColor ACC_VIOLET = 0xFF78C4FFu;   // (255,120,196,255)
void showOverlay(const std::wstring& title,const std::wstring& text,ArgbColor accent);
