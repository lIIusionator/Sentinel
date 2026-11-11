// afk_state.cpp -- thread-safe hand-off of UI-thread state to the AFK render thread.
#include "afk_state.h"

static CRITICAL_SECTION g_snapCs;
static HANDLE g_snapEvent=nullptr;
static std::atomic<uint32_t> g_snapVer{1};
static struct SnapInit{ SnapInit(){ InitializeCriticalSection(&g_snapCs); g_snapEvent=CreateEventW(nullptr,FALSE,FALSE,nullptr); } } s_snapInit;
static AfkSnapshot g_snap;

void afkScreenPublish(const AfkSnapshot& s){
  EnterCriticalSection(&g_snapCs); g_snap=s; g_snapVer.fetch_add(1); LeaveCriticalSection(&g_snapCs);
  if(g_snapEvent) SetEvent(g_snapEvent);
}
AfkSnapshot afkScreenSnapshot(){
  EnterCriticalSection(&g_snapCs); AfkSnapshot s=g_snap; LeaveCriticalSection(&g_snapCs);
  return s;
}
uint32_t afkSnapshotVersion(){ return g_snapVer.load(); }
HANDLE afkStateEvent(){ return g_snapEvent; }
