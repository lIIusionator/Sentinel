// afk_tiers.cpp -- back-end lifecycle and the tier ladder (see afk_controller.h).
//
//   tier 0  4K 60 fps clip on the GPU player      tier 1  1080p 30 fps clip on the GPU player      tier 2  CPU renderer
// The controller starts at the best tier this machine is not known to fail (afkStartTier) and walks down when one cannot start,
// is too slow, or keeps losing the GPU. Every step is logged, and what was learned is remembered for the next run (AfkTierMemory).
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include "afk_controller.h"
#include "sw_lock.h"
#include <ctime>

static SwCs g_memCs;

const wchar_t* afkTierName(int t){
  switch(t){
    case AFK_TIER_GPU4K:    return L"4K 60 fps clip, GPU player";
    case AFK_TIER_GPULITE:  return L"1080p 30 fps clip, GPU player";
    case AFK_TIER_CPU:      return L"CPU renderer";
    default:                return L"none"; }
}

// ------------------------------------------------------------------------------------------------ persistence
void afkPersistDowngrade(int toTier,bool hard){
  SwGuard g(g_memCs);
  AfkTierMemory m; m.minTier=g_afkMinTier.load(); m.strikes=g_afkStrikes.load(); m.minTierAtSec=(int64_t)g_afkMinTierAt.load();
  if(!afkMemOnDowngrade(m,toTier,hard,(int64_t)time(nullptr))) return;
  g_afkMinTier.store(m.minTier); g_afkStrikes.store(m.strikes); g_afkMinTierAt.store((long long)m.minTierAtSec);
  saveSettings();
  logLine(L"[AFK] remembered: start at tier "+std::to_wstring(m.minTier)+L" ("+afkTierName(m.minTier)+L"), slow sessions in a row "+std::to_wstring(m.strikes)
    +(hard?L" (hard failure)":L"")+L"; this expires after a week or with the next build");
}
void afkPersistGoodRun(){
  SwGuard g(g_memCs);
  AfkTierMemory m; m.minTier=g_afkMinTier.load(); m.strikes=g_afkStrikes.load(); m.minTierAtSec=(int64_t)g_afkMinTierAt.load();
  if(!afkMemOnGoodRun(m)) return;
  g_afkStrikes.store(m.strikes); saveSettings();
  logLine(L"[AFK] a full minute of smooth playback -- the slow-session count is cleared");
}
// loadSettings() already drops a stale verdict at start-up; this does the same for an app that has been running for more than a week.
void afkExpireMemory(){
  SwGuard g(g_memCs);
  AfkTierMemory m; m.minTier=g_afkMinTier.load(); m.strikes=g_afkStrikes.load(); m.minTierAtSec=(int64_t)g_afkMinTierAt.load();
  const int was=m.minTier;
  if(!afkMemExpire(m,(int64_t)time(nullptr))) return;
  g_afkMinTier.store(m.minTier); g_afkStrikes.store(m.strikes); g_afkMinTierAt.store((long long)m.minTierAtSec);
  saveSettings();
  logLine(L"[AFK] a remembered verdict is more than a week old -- forgotten (start tier "+std::to_wstring(was)+L" -> "+std::to_wstring(m.minTier)+L"), every tier is tried again");
}

// ------------------------------------------------------------------------------------------------ ladder
int AfkRunner::startTier() const{
  if(!m_haveMedia) return AFK_TIER_CPU;                          // nothing to play: the CPU back-end shows the dark background and the HUD
  afkExpireMemory();
  int t=afkStartTier(m_mode,g_afkMinTier.load(),m_media.userClip);
  if(t<AFK_TIER_CPU&&g_afkGpuPoisoned.load()) t=(m_mode==AFK_GPU)?AFK_TIER_NONE:AFK_TIER_CPU;
  return t;
}
int AfkRunner::nextTier(int cur) const{
  int n=afkNextTier(cur,m_mode,m_media.userClip);
  if(n<AFK_TIER_CPU&&g_afkGpuPoisoned.load()) n=(m_mode==AFK_GPU)?AFK_TIER_NONE:AFK_TIER_CPU;
  return n;
}

// ------------------------------------------------------------------------------------------------ one back-end
bool AfkRunner::openSlot(int tier,const AfkMonitor& mon,AfkSlot& out,std::wstring& why){
  out=AfkSlot(); m_lastOpenBlame=false;                          // set below only when the player itself failed to start
  const bool gpu=(tier==AFK_TIER_GPU4K||tier==AFK_TIER_GPULITE);
  if(gpu&&g_afkGpuPoisoned.load()){ why=L"the GPU player is switched off for this session"; return false; }
  // the 4K tier plays the 4K clip; the lite and CPU tiers play the 1080p clip (each stands in for a missing other)
  AfkClip clip; if(tier==AFK_TIER_GPU4K) clip=m_media.big.valid()?m_media.big:m_media.lite; else clip=m_media.lite.valid()?m_media.lite:m_media.big;
  if(gpu&&!clip.valid()){ why=L"no video file is available"; return false; }
  const int W=mon.width(), H=mon.height();
  if(W<=0||H<=0){ why=L"no usable monitor size"; return false; }
  HWND hw=afkCreateScreenWindow(mon.rc); if(!hw){ why=L"the window could not be created"; return false; }
  IAfkBackend* be=gpu?afkCreateGpuBackend(tier):afkCreateCpuBackend();
  if(!be){ afkDestroyScreenWindow(hw); why=L"out of memory"; return false; }
  const double fps=(clip.valid()&&clip.fps>1.0)?clip.fps:(tier==AFK_TIER_GPU4K?60.0:30.0);
  bool ok=false;
  { AfkBusy busy(C,gpu?L"starting the GPU player":L"starting the CPU renderer");
    if(gpu) afkMarkerWrite(tier);                                // removed again once the player is idle and healthy
#ifdef SW_TEST_HOOKS
    if(gpu&&m_testHang){ logLine(L"[AFK] (test) blocking forever inside the GPU player start"); Sleep(600000); }
#endif
    ok=be->init(hw,W,H,clip.valid()?clip.path:std::wstring(),fps,mon.dpiScale,why); }
  if(C->abandoned.load()){ be->shutdown(); delete be; afkDestroyScreenWindow(hw); why=L"this controller was replaced while starting"; return false; }
  if(!ok){
    m_lastOpenBlame=true;                                        // the player was asked to start and could not
    { AfkBusy busy(C,L"stopping a back-end that failed to start"); be->shutdown(); }
    delete be; afkDestroyScreenWindow(hw); if(gpu) afkMarkerClear(); if(why.empty()) why=L"init failed"; return false; }
  if(!gpu) afkMarkerClear();                                     // a GPU tier that was on screen a moment ago left its marker: the CPU renderer is not under suspicion
  out.be=be; out.hwnd=hw; out.tier=tier; out.rc=mon.rc; out.W=W; out.H=H; out.hz=mon.hz; out.dpi=mon.dpiScale;
  out.openedMs=(int64_t)GetTickCount64(); out.clipFps=fps; out.ready=false;
  logLine(L"[AFK] tier "+std::to_wstring(tier)+L" started: "+be->name()+L", "+std::to_wstring(W)+L"x"+std::to_wstring(H)+L" at "+std::to_wstring(mon.hz)+L" Hz, clip "
    +(clip.valid()?clip.path+L" ("+std::to_wstring(clip.w)+L"x"+std::to_wstring(clip.h)+L" @ "+fmt1(fps)+L" fps)":std::wstring(L"none")));
  return true;
}

void AfkRunner::stopSlot(AfkSlot& s){
  if(!s.be) return;
  { AfkBusy busy(C,L"stopping the video back-end"); s.be->shutdown(); }
  delete s.be; s.be=nullptr; s.ready=false;
}

// The first screen of a run is shown on a player that was built, parked after its first frame and never played -- the only case seen working on the target
// laptop. A later screen would have to resume a player that has played, been paused and sat idle for hours (decoder, audio device, GPU and swap chain in
// whatever state they ended up in). So a player that has run a screen is dropped once the user has been idle for most of the wait (snapshot.warm, afkPreloadLeadMs before
// the screen is due) and the normal preload path builds a fresh one: the user is away by then (no cost to a game), the player has its first frame before the screen
// comes up, and a fresh player they cancel before it is shown is kept for the next time. (Dropping it only when the screen was due left every screen after the first
// to start on a player that was still loading.)
void AfkRunner::renewSlot(){
  logLine(L"[AFK] idle for most of the wait -- replacing the player that already ran a screen with a fresh one (tier "+std::to_wstring(m_s.tier)+L"), so that the next screen starts on a ready player");
  AfkSlot s=m_s; m_s=AfkSlot(); stopSlot(s); afkDestroyScreenWindow(s.hwnd); afkPumpMessages(); m_buildAtMs=0;
}

// Opens `tier`, or -- when it cannot start -- the next ones, into m_s. A start the PLAYER failed (not a missing window, monitor or clip) at the tier this run
// began on is remembered as one slow-or-broken session (a strike; two in a row make it permanent): a one-off driver hiccup must not cost the 4K tier for a week.
bool AfkRunner::climbDown(int tier,const AfkMonitor& mon){
  for(;;){
    if(tier==AFK_TIER_NONE||C->abandoned.load()){
      m_noBackend=true; m_retryBuildMs=(int64_t)GetTickCount64()+60000;
      if(!C->abandoned.load()) logLine(m_mode==AFK_GPU?L"[AFK] no GPU back-end could be started and afkMode is \"gpu\" -- the AFK screen stays off"
                                                         :L"[AFK] no video back-end could be started -- the AFK screen stays off (it will try again in a minute)");
      return false; }
    AfkSlot s; std::wstring why;
    if(openSlot(tier,mon,s,why)){ m_s=s; m_noBackend=false; m_nextPumpMs=0; m_wdOff=false; return true; }
    if(C->abandoned.load()) return false;                        // replaced while starting: the new controller owns everything now
    const int next=nextTier(tier);
    logLine(L"[AFK] tier "+std::to_wstring(tier)+L" ("+afkTierName(tier)+L") cannot start: "+why
      +(next!=AFK_TIER_NONE?L" -> trying tier "+std::to_wstring(next)+L" ("+afkTierName(next)+L")":std::wstring(L" -> no lower tier")));
    if(m_lastOpenBlame&&afkJudgesTier(m_entryTier,tier)) afkPersistDowngrade(next,false);
    afkPumpMessages();                                           // the window that was just destroyed queued messages for this thread
    tier=next;
  }
}

// Stops the live back-end and starts `tier` (or a lower one) in its place. While the screen is up the old window stays, covered in
// black, until the new one is ready, so the desktop never shows through.
bool AfkRunner::replaceBackend(int tier,const AfkMonitor& mon){
  AfkSlot old=m_s; m_s=AfkSlot();
  if(m_visible&&old.be){ const uint64_t cur=old.be->videoFrames(); m_framesBanked+=cur>=m_framesAtEnter?cur-m_framesAtEnter:cur; }   // what this screen has shown so far (see leaveAfk)
  stopSlot(old);                                                   // the media engine is a singleton: the old one must be gone first
  afkPumpMessages();
  if(C->abandoned.load()){ afkDestroyScreenWindow(old.hwnd); return false; }   // replaced meanwhile: show nothing, leave the rest to the new controller
  if(m_visible&&old.hwnd) afkCoverWithBlack(old.hwnd);
  const bool ok=climbDown(tier,mon);
  afkPumpMessages();
  if(C->abandoned.load()){ afkDestroyScreenWindow(old.hwnd); return false; }   // (finish() stops whatever climbDown opened)
  if(ok&&m_visible){ activateSlot(); m_framesAtEnter=0; }         // the new back-end's counters start at zero
  afkDestroyScreenWindow(old.hwnd);
  if(!ok&&m_visible) leaveAfk(L"no back-end left");
  return ok;
}

// The live tier failed. hard: it cannot run on this machine; otherwise it runs but too slowly. One tier down.
// A hard failure is remembered whichever tier it was; slowness only counts against the tier this run started on (a swap down is the CONSEQUENCE of that
// verdict, so the lower tier being slow as well, or smooth, must not be counted a second time).
void AfkRunner::fallBack(bool hard,const std::wstring& why){
  if(C->abandoned.load()) return;
  const int from=m_s.tier, next=nextTier(from);
  logLine(L"[AFK] tier "+std::to_wstring(from)+L" ("+afkTierName(from)+L") "+(hard?L"failed: ":L"is too slow on this machine: ")+why
    +(next!=AFK_TIER_NONE?L" -> falling back to tier "+std::to_wstring(next)+L" ("+afkTierName(next)+L")":std::wstring(L" -> no lower tier")));
  if(hard||afkJudgesTier(m_entryTier,from)) afkPersistDowngrade(next,hard);
  AfkMonitor mon; mon.rc=m_s.rc; mon.hz=m_s.hz; mon.dpiScale=m_s.dpi;
  if(next==AFK_TIER_NONE&&!hard){ m_wdOff=true; m_wd.reset(); return; }     // slow but working and nothing below it: keep it
  replaceBackend(next,mon);
}

// The GPU was reset or removed (driver update, sleep/resume, TDR). Rebuild the same tier; if that keeps happening, give the tier up.
void AfkRunner::recoverDevice(const std::wstring& why){
  if(C->abandoned.load()) return;
  const int64_t now=(int64_t)GetTickCount64();
  m_losses.push_back(now); while(!m_losses.empty()&&now-m_losses.front()>60000) m_losses.pop_front();
  AfkMonitor mon; mon.rc=m_s.rc; mon.hz=m_s.hz; mon.dpiScale=m_s.dpi; const int tier=m_s.tier;
  if(m_losses.size()>3){ fallBack(true,L"the GPU was lost "+std::to_wstring(m_losses.size())+L" times within a minute ("+why+L")"); m_losses.clear(); return; }
  logLine(L"[AFK] "+why+L" -- rebuilding tier "+std::to_wstring(tier)+L" ("+std::to_wstring(m_losses.size())+L" of 3 allowed within a minute)");
  replaceBackend(tier,mon);
}

// While hidden (and every half second while visible): build the back-end ahead of time, pump it until the first frame exists,
// and notice failures early so the next AFK start is instant.
void AfkRunner::upkeep(){
  const int64_t now=(int64_t)GetTickCount64();
  if(!m_s.be){
    if(m_noBackend){ if(m_mode==AFK_GPU||now<m_retryBuildMs) return; m_noBackend=false; }
    if(now<m_buildAtMs) return;
    AfkMonitor mon;
    if(!afkMonitorPrimary(mon)){ logLine(L"[AFK] no monitor information available"); m_noBackend=true; m_retryBuildMs=now+60000; return; }
    const int tier=startTier();
    m_entryTier=tier;                                            // the tier this player starts on: the one its slow-session verdicts are about
    logLine(L"[AFK] preloading: tier "+std::to_wstring(tier)+L" ("+afkTierName(tier)+L"), mode "+(m_mode==AFK_GPU?L"gpu":m_mode==AFK_CPU?L"cpu":L"auto")
      +L", remembered minimum tier "+std::to_wstring(g_afkMinTier.load()));
    climbDown(tier,mon); return; }
  if(now<m_nextPumpMs) return;
  const bool gpu=m_s.tier<=AFK_TIER_GPULITE; std::wstring why;
  const int r=m_s.be->pump(why);
  if(C->abandoned.load()) return;                                // pump() can block; if the supervisor gave this thread up meanwhile, do nothing further
  if(r==AFKB_READY){
    if(!m_s.ready){ m_s.ready=true; m_wd.reset(); logLine(L"[AFK] tier "+std::to_wstring(m_s.tier)+L" is ready: first video frame "+fmt1((double)(now-m_s.openedMs))+L" ms after start");
      if(gpu&&!m_visible) afkMarkerClear(); }                      // idle and healthy: a crash from here on is not the player's
    m_nextPumpMs=now+1000; return; }
  if(r==AFKB_LOADING){
    if(gpu&&now-m_s.openedMs>25000){ fallBack(true,L"no video frame within 25 s"); return; }
    m_nextPumpMs=now+50; return; }
  if(r==AFKB_DEVICE_LOST){ recoverDevice(why.empty()?std::wstring(L"GPU device lost"):why); return; }
  fallBack(true,why.empty()?std::wstring(L"the back-end reported a failure"):why);
}
