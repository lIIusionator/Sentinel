// events.cpp -- event feed shared by all watchers.
#include "events.h"
#include "settings.h"

std::deque<Ev> g_events;
CRITICAL_SECTION g_cs;
std::atomic<int> g_procCount{0}, g_fileCount{0}, g_tokenAlerts{0};
static ULONGLONG g_secWin=0; static int g_secCnt=0;

void eventsInit(){ InitializeCriticalSection(&g_cs); }

// ---------- shared event push ----------
void pushEv(const wchar_t* tag,const std::wstring& name,const std::wstring& path){
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
void pushEvRaw(const wchar_t* tag,const std::wstring& name,const std::wstring& path){
  SYSTEMTIME t; GetLocalTime(&t); wchar_t tm[16]; wsprintfW(tm,L"%02d:%02d:%02d",t.wHour,t.wMinute,t.wSecond);
  EnterCriticalSection(&g_cs); g_events.push_front({tag,name,path,tm}); if(g_events.size()>80) g_events.pop_back(); LeaveCriticalSection(&g_cs);
  g_dirty.store(true);
}
