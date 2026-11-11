// afk_screen.cpp -- the public face of the AFK screen module: starting / stopping the controller thread, and the supervisor.
//
// The controller thread (afk_loop.cpp / afk_tiers.cpp) does all the work. The supervisor, afkScreenWatch(), runs on the UI thread
// and only watches its heartbeat: GPU drivers occasionally block a call for good (the target laptop once needed more than six
// seconds just to create a D3D11 device), and a controller stuck inside such a call can never fall back by itself. If the heartbeat
// stops for too long the thread is abandoned, the GPU player is switched off for the rest of the session and remembered as failed,
// and a fresh controller that uses the CPU renderer only takes over.
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include "afk_controller.h"
#include "sw_lock.h"
#include <wchar.h>
#include <stdlib.h>

std::atomic<bool> g_afkGpuPoisoned{false};

namespace {
SwCs g_ctlCs;
AfkCtl* g_cur=nullptr;                              // the controller whose window the user sees (this global holds one reference)
int g_spawns=0;
const int64_t kStuckMs=45000;                       // no heartbeat for this long = stuck
const int64_t kStuckUnpackMs=180000;                // ... unless it is copying the 77 MB of built-in clips to a slow disk
const int kMaxSpawns=4;
const int64_t kStaleConfirmMs=2000;                 // the SAME stale heartbeat must be seen again this much later before a thread is given up
// UI thread only (afkScreenWatch). GetTickCount64 keeps counting while the PC sleeps, so the first look after a resume can find a perfectly healthy
// controller "silent for an hour": it gets a moment to beat before it is judged. The pointer is only ever compared, never used.
const AfkCtl* g_staleCtl=nullptr; int64_t g_staleBeat=0, g_staleSince=0;
std::atomic<bool> g_leftBehind{false};              // some controller thread was abandoned and may still be running somewhere inside a driver

void ctlAddRef(AfkCtl* c){ c->refs.fetch_add(1); }
void ctlRelease(AfkCtl* c){ if(c->refs.fetch_sub(1)==1){ if(c->thread) CloseHandle(c->thread); delete c; } }

DWORD WINAPI threadMain(LPVOID p){
  AfkCtl* c=(AfkCtl*)p; cpuNameThread(L"afk");
  { AfkRunner r(c); r.run(); }
  c->visible.store(false); c->finished.store(true);
  ctlRelease(c);                                    // the thread's own reference
  return 0;
}

// Starts a controller thread; the returned pointer carries the creator's reference.
AfkCtl* spawn(bool cpuOnly){
  AfkCtl* c=new AfkCtl(); c->cpuOnly=cpuOnly; c->beat.store((int64_t)GetTickCount64());
  c->refs.fetch_add(1);                             // for the thread
  HANDLE h=CreateThread(nullptr,0,threadMain,c,0,nullptr);
  if(!h){ logLine(L"[AFK] CreateThread failed, error "+std::to_wstring(GetLastError())); delete c; return nullptr; }
  c->thread=h; return c;
}

// UI thread. Called with the controller that stopped answering.
void takeOver(AfkCtl* c,const std::wstring& what){
  SwGuard g(g_ctlCs);
  if(g_cur!=c) return;                              // already replaced
  logLine(L"[AFK] the AFK thread stopped responding ("+what+L") -- abandoning it; the GPU player is switched off and the CPU renderer takes over"
    +(c->visible.load()?L". Its window may stay on screen until the app is restarted":L""));
  c->abandoned.store(true); g_leftBehind.store(true);
  g_afkGpuPoisoned.store(true);
  afkPersistDowngrade(AFK_TIER_CPU,true);
  g_cur=nullptr;
  if(g_spawns>=kMaxSpawns){ logLine(L"[AFK] too many restarts -- the AFK screen is off for the rest of this session"); ctlRelease(c); return; }
  AfkCtl* n=spawn(true); if(n){ g_spawns++; g_cur=n; }
  ctlRelease(c);                                    // the global's reference to the old controller
}
} // namespace

void afkScreenStart(){
  SwGuard g(g_ctlCs); if(g_cur) return;
  g_cur=spawn(false); if(g_cur) g_spawns=1;
}

bool afkScreenStop(){
  AfkCtl* c=nullptr; { SwGuard g(g_ctlCs); c=g_cur; g_cur=nullptr; }
  if(!c) return !g_leftBehind.load();
  c->stop.store(true); SetEvent(afkStateEvent());   // wakes the controller if it is waiting for the next snapshot
  if(c->thread&&WaitForSingleObject(c->thread,5000)==WAIT_TIMEOUT){
    const wchar_t* w=c->busyWhat.load();
    logLine(L"[AFK] the AFK thread did not stop within 5 s (inside: "+std::wstring(w?w:L"?")+L") -- leaving it behind");
    c->abandoned.store(true); g_leftBehind.store(true); }
  ctlRelease(c);
  return !g_leftBehind.load();
}

bool afkScreenVisible(){ SwGuard g(g_ctlCs); return g_cur&&g_cur->visible.load(); }

void afkScreenWatch(){
  AfkCtl* c=nullptr; { SwGuard g(g_ctlCs); c=g_cur; if(c) ctlAddRef(c); }
  if(!c) return;
  if(!c->stop.load()){
    if(c->finished.load()) takeOver(c,L"the thread ended unexpectedly");
    else{
      int64_t lim=kStuckMs;
      const wchar_t* bw=c->busyWhat.load();
      if(c->busySince.load()&&bw&&wcsncmp(bw,L"unpacking",9)==0) lim=kStuckUnpackMs;
#ifdef SW_TEST_HOOKS
      { wchar_t b[32]; if(GetEnvironmentVariableW(L"SW_TEST_STUCK_MS",b,32)) lim=_wtoi(b); }
#endif
      const int64_t beat=c->beat.load(), now=(int64_t)GetTickCount64();
      if(beat&&now-beat>lim){
        if(g_staleCtl!=c||g_staleBeat!=beat){ g_staleCtl=c; g_staleBeat=beat; g_staleSince=now; }          // first sighting of this silence: note it, judge next time
        else if(now-g_staleSince>=kStaleConfirmMs){
          g_staleCtl=nullptr;
          takeOver(c,L"no sign of life for "+std::to_wstring((now-beat)/1000)+L" s, inside: "+std::wstring(c->busySince.load()&&bw?bw:L"the render loop")); } }
      else if(g_staleCtl==c) g_staleCtl=nullptr; } }
  ctlRelease(c);
}
