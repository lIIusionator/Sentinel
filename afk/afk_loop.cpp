// afk_loop.cpp -- the AFK controller thread: start-up, the hidden/visible state machine and the per-frame logic (see afk_controller.h).
//
// While the screen is hidden the thread sleeps on the "state published" event (woken the moment the UI thread says the user went idle),
// keeps the preloaded back-end healthy and costs nothing measurable. While it is visible every pass renders one frame: the GPU player's
// Present() blocks until the next refresh, the CPU renderer asks for a short pause (frameDelayMs). Fades are timed from the clock, not
// counted in frames, so they take the same time at 15, 30 or 144 fps.
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include "afk_controller.h"
#include <math.h>
#include <stdlib.h>

static inline int64_t tickMs(){ return (int64_t)GetTickCount64(); }

void AfkRunner::run(){
  try{ body(); }
  catch(...){ logLine(L"[AFK] the AFK thread hit an unexpected C++ exception -- cleaning up"); }
  try{ finish(); } catch(...){}
}

void AfkRunner::body(){
  m_startMs=tickMs(); C->beat.store(m_startMs);
  afkThreadDpiAware();                                           // before the first window: the awareness is fixed at creation
  m_coInit=SUCCEEDED(CoInitializeEx(nullptr,COINIT_MULTITHREADED));
  m_mode=C->cpuOnly?(int)AFK_CPU:g_afkModePref.load();
#ifdef SW_TEST_HOOKS
  { wchar_t b[64];
    if(GetEnvironmentVariableW(L"SW_TEST_MODE",b,64)){ std::wstring v=b; m_mode=(v==L"gpu")?(int)AFK_GPU:(v==L"cpu")?(int)AFK_CPU:(int)AFK_AUTO; }
    if(GetEnvironmentVariableW(L"SW_TEST_AT",b,64)) m_testAt=_wtoi(b);
    if(GetEnvironmentVariableW(L"SW_TEST_FOR",b,64)) m_testFor=_wtoi(b);
    if(GetEnvironmentVariableW(L"SW_TEST_EVERY",b,64)) m_testEvery=_wtoi(b);
    m_testHang=GetEnvironmentVariableW(L"SW_TEST_HANG",b,64)>0; m_testNoInput=GetEnvironmentVariableW(L"SW_TEST_NOINPUT",b,64)>0; }
#endif
  logLine(std::wstring(L"[AFK] controller started (")+(C->cpuOnly?L"CPU renderer only: the GPU player stopped responding earlier":L"mode ")+(C->cpuOnly?L"":(m_mode==AFK_GPU?L"gpu":m_mode==AFK_CPU?L"cpu":L"auto"))+L")");
  { AfkBusy busy(C,L"unpacking the built-in clips"); m_haveMedia=afkAssetsPrepare(m_media); }
  if(C->abandoned.load()) return;
  if(!m_haveMedia) logLine(L"[AFK] no video clip is available -- the AFK screen shows the dark background and the HUD only");

  // A GPU player that was active when the previous run died (driver crash, hang, killed by the user) is not trusted again for a while.
  if(!C->cpuOnly){
    const int mk=afkMarkerRead();
    if(mk>=0){
      afkMarkerClear(); const int to=afkNextTier(mk,AFK_AUTO,m_media.userClip);
      logLine(L"[AFK] the previous run ended while tier "+std::to_wstring(mk)+L" ("+afkTierName(mk)+L") was active (crash, hang or kill) -- treating that tier as failed");
      afkPersistDowngrade(to,true); } }

  m_buildAtMs=tickMs()+1500;                                    // preload shortly after start-up, not during it
  while(!C->stop.load()&&!C->abandoned.load()){
    C->beat.store(tickMs());
    afkPumpMessages();
    refreshSnapshot();
    if(m_visible){ frame(); continue; }
    // The screen is due -- or will be within the lead time (m_snap.warm: the user has been idle for most of the wait). A player that already ran a screen is replaced, not
    // resumed, and the replacement is built NOW, so that it has its first frame by the time the screen comes up. (Built only when the screen was due, as it used to be,
    // every screen after the first appeared on a player that was still loading: the clock on black, the video seconds late or never, and a log that called it "loading".)
    const bool soon=wantAfk()||warmNow();
    if(soon&&m_s.be&&m_s.used) renewSlot();
    if(soon&&!m_s.be) m_buildAtMs=0;                            // needed soon: do not wait for the preload slot
    upkeep();
    if(C->stop.load()||C->abandoned.load()) break;
    refreshSnapshot();                                          // upkeep() can take seconds (a driver call): decide on what the UI thread says NOW
    if(wantAfk()&&m_s.be&&enterAfk()) continue;
    sleepWait(m_s.be?(m_s.ready?1000u:50u):200u,true);
  }
}

void AfkRunner::finish(){
  if(m_visible) leaveAfk(L"the app is closing");
  { AfkSlot s=m_s; m_s=AfkSlot(); stopSlot(s); afkDestroyScreenWindow(s.hwnd); }
  m_boost.off();
  if(!C->abandoned.load()) afkMarkerClear();                    // a clean stop; an abandoned (hung) controller leaves the marker for the next start
  if(m_coInit){ CoUninitialize(); m_coInit=false; }
  logLine(L"[AFK] controller stopped");
}

// ------------------------------------------------------------------------------------------------ snapshot and intent
void AfkRunner::refreshSnapshot(){
  const uint32_t v=afkSnapshotVersion();
  if(v!=m_snapVer){ m_snap=afkScreenSnapshot(); m_snapVer=v; m_fp.quote=m_snap.quote; m_fp.user=m_snap.user; }
#ifdef SW_TEST_HOOKS
  if(m_testAt>=0){
    const int64_t d=tickMs()-m_startMs-(int64_t)m_testAt*1000, per=(int64_t)m_testEvery*1000;   // d: time since the first idle period began
    const int64_t cyc=(per>0&&d>=0)?d/per:0, ph=(per>0&&d>=0)?d%per:d;                          // which repeat we are in, and how far into it
    m_snap.afkNow=(d>=0)&&(m_testFor<0||ph<(int64_t)m_testFor*1000);
    m_snap.enterT0=(ULONGLONG)(m_startMs+(int64_t)m_testAt*1000+cyc*per);                         // a new session number per repeat, as the UI thread would publish
    { const int64_t nextIn=(d<0)?-d:(per>0?per-ph:INT64_MAX);                                   // "idle for most of the wait", as the UI thread publishes it: the lead is the same share of the
      m_snap.warm=m_snap.afkNow||nextIn<=afkPreloadLeadMs(per>0?per:(int64_t)m_testAt*1000); }    // wait (the repeat period, else the first delay) as in the real thing
    if(m_fp.quote.empty()){ m_fp.quote=L"A test quote with **bold** words, written for {user}."; m_fp.user=L"Tester"; } }
#endif
  // A new idle period (the snapshot flips to "idle", or a new session number arrives): remember the last-input tick it started with (see m_snapInput).
  if(m_snap.afkNow&&(!m_prevAfk||m_snap.enterT0!=m_prevEnterT0)) m_snapInput=afkLastInputTick();
  m_prevAfk=m_snap.afkNow; m_prevEnterT0=m_snap.enterT0;
}

// Has the user been idle for most of the wait? The UI thread's flag lags behind the input this thread saw first: for a moment after a screen ends it still says "yes", and
// believing it would build the next player at the very moment the user is back (a hitch, for nothing). It is trusted again once it has been seen to say "no".
bool AfkRunner::warmNow(){
  if(m_warmStale&&!m_snap.warm) m_warmStale=false;
  return m_snap.warm&&!m_warmStale;
}

// Does the user's idleness (as published by the UI thread) call for the screen right now?
bool AfkRunner::wantAfk(){
  if(m_latched&&(!m_snap.afkNow||m_snap.enterT0!=m_latchT0)) m_latched=false;     // the UI thread has caught up with the input we saw first
  return m_snap.afkNow&&!m_latched;
}

void AfkRunner::sleepWait(DWORD ms,bool wakeOnState){
  HANDLE h=wakeOnState?afkStateEvent():nullptr;
  MsgWaitForMultipleObjectsEx(h?1:0,h?&h:nullptr,ms,QS_ALLINPUT,MWMO_INPUTAVAILABLE);
  afkPumpMessages();
}

// ------------------------------------------------------------------------------------------------ show / hide
bool AfkRunner::enterAfk(){
  // The "idle" this decision rests on may be old (the thread was busy in a driver call, the UI thread may be lagging): if the user touched anything since,
  // the screen must not come up. The latch holds it off until the UI thread publishes the new state, exactly as for input seen while the screen is up.
  bool checkInput=true;
#ifdef SW_TEST_HOOKS
  if(m_testNoInput) checkInput=false;
#endif
  if(checkInput){ const DWORD li=afkLastInputTick();
    if(li&&li!=m_snapInput){ m_latched=true; m_latchT0=m_snap.enterT0; logLine(L"[AFK] input arrived while the screen was being prepared -- not showing it"); return false; } }
  AfkMonitor mon;
  if(!afkMonitorFromCursor(mon)||!m_s.be||!m_s.hwnd){
    noShow(!m_s.be?L"there is no video player":!m_s.hwnd?L"the player has no window":L"the monitor under the cursor could not be read"); return false; }
  m_s.rc=mon.rc; m_s.hz=mon.hz; m_s.dpi=mon.dpiScale;
  if(mon.width()!=m_s.W||mon.height()!=m_s.H){                   // the screen the user is on differs from the one preloaded for
    SetWindowPos(m_s.hwnd,nullptr,mon.rc.left,mon.rc.top,mon.width(),mon.height(),SWP_NOZORDER|SWP_NOACTIVATE);
    if(m_s.be->resize(mon.width(),mon.height())){ m_s.W=mon.width(); m_s.H=mon.height(); }
    else{ logLine(L"[AFK] the back-end could not resize to "+std::to_wstring(mon.width())+L"x"+std::to_wstring(mon.height())+L" -- rebuilding it");
      if(!replaceBackend(m_s.tier,mon)){ noShow(L"the player could not be rebuilt for this monitor (see the lines above)"); return false; } } }
  const int64_t now=tickMs();
  m_visible=true; C->visible.store(true);
  m_af=0.f; m_latched=false; m_inputBase=afkLastInputTick(); m_enterMs=now; m_lastFrameMs=now; m_goodRun=false; m_statsMs=now; m_gateFails=0; m_gateEco=false; m_framesBanked=0; m_firstPassMs=-1;
  m_lockedNow=false; m_lockChkMs=0;
  m_boost.on();
  activateSlot();
  m_framesAtEnter=m_s.be?m_s.be->videoFrames():0;
  logLine(L"[AFK] screen up: "+std::to_wstring(m_s.W)+L"x"+std::to_wstring(m_s.H)+L" at "+std::to_wstring(m_s.hz)+L" Hz, UI scale "+fmt1(afkUiScale(m_s.dpi,m_s.H))
    +L", tier "+std::to_wstring(m_s.tier)+L" ("+afkTierName(m_s.tier)+L")"+(m_s.ready?L"":L", still loading"));
  return true;
}

// Start playback and bring the window up with real focus. Used when the screen appears and after a back-end swap while it is up.
void AfkRunner::activateSlot(){
  IAfkBackend* be=m_s.be; if(!be||!m_s.hwnd) return;
  be->setRefreshHz(m_s.hz);
  m_expectFps=m_s.clipFps; { const double cap=(double)m_s.hz/(double)afkSyncInterval(m_s.hz); if(cap<m_expectFps) m_expectFps=cap; }
  be->setAudio(0.f,false); m_lastVol=0.f;
  be->setActive(true);
  m_wd.reset(); m_wdOff=false; m_occSince=0; m_pausedOcc=false; m_failStreak=0;
  m_gate.reset(); m_gateDone=(m_s.tier>AFK_TIER_GPULITE); m_gateHoldMs=0; m_gatePassMs=-1;    // every player is checked afresh, from the moment it is told to play (the CPU renderer has no such check)
  // one black frame first, so the window never appears with whatever the buffers held
  m_fp.W=m_s.W; m_fp.H=m_s.H; m_fp.uiScale=afkUiScale(m_s.dpi,m_s.H); m_fp.nowMs=(uint64_t)tickMs(); m_fp.dtSec=0.016; m_fp.af=0.f;
  m_fp.starF=m_fp.vidF=m_fp.txtF=0.f; m_fp.quoteA=1.f;
  { SYSTEMTIME st; GetLocalTime(&st); m_fp.hh=st.wHour; m_fp.mm=st.wMinute; m_fp.ss=st.wSecond; }
  be->render(m_fp);
  afkShowScreenWindow(m_s.hwnd,m_s.rc);
  if(!afkForceForeground(m_s.hwnd)) logLine(L"[AFK] Windows did not hand the screen keyboard focus -- the first key press may also reach the app underneath");
  if(m_s.tier<=AFK_TIER_GPULITE) afkMarkerWrite(m_s.tier);       // active GPU player: a crash from here on is held against it
  m_lastFrameMs=tickMs();
}

void AfkRunner::leaveAfk(const wchar_t* why){
  if(!m_visible) return;
  m_visible=false; C->visible.store(false);
  m_warmStale=true;
  const int64_t now=tickMs(); uint64_t frames=m_framesBanked;    // (frames shown by players that were swapped out during this screen, plus the live one's)
  if(m_s.hwnd) afkHideScreenWindow(m_s.hwnd);                    // the desktop first: pausing the player is a driver call that can take a while
  if(m_s.be){ const uint64_t cur=m_s.be->videoFrames(); frames+=cur>=m_framesAtEnter?cur-m_framesAtEnter:cur; m_s.be->setActive(false); m_s.be->setAudio(0.f,true); }
  m_boost.off();
  const double sec=(double)(now-m_enterMs)/1000.0;
  std::wstring chk;                                              // the video check's verdict goes in the line that is read first when someone says "there was no video"
  if(m_s.be&&m_s.tier<=AFK_TIER_GPULITE){
    if(m_firstPassMs>=0) chk=L", video check passed "+fmt1((double)m_firstPassMs/1000.0)+L" s in";
    else if(m_gateEco) chk=L", the video check was skipped (power saving was on)";
    else if(m_gateDone) chk=L", the video check never passed (nowhere lower to go) -- NO MOVING VIDEO was shown";
    else if(!m_s.ready) chk=sec>4.0?L", the player was still loading -- NO VIDEO was shown":L", the player was still loading";
    else chk=sec>4.0?L", the video check had not passed -- NO MOVING VIDEO was shown":L", the screen was too short for the video check"; }
  logLine(L"[AFK] screen down after "+fmt1(sec)+L" s ("+why+L"): tier "+std::to_wstring(m_s.tier)+L", "+std::to_wstring(frames)+L" video frames"
    +(sec>1.0?L", "+fmt1((double)frames/sec)+L" fps on average":std::wstring())+chk);
  if(m_s.be&&m_s.tier<=AFK_TIER_GPULITE&&m_s.ready&&!C->abandoned.load()) afkMarkerClear();
  if(m_s.be) m_s.used=true;
  m_af=0.f; m_wd.reset(); m_pausedOcc=false; m_occSince=0;
}

// The screen is due, but nothing can be shown. Said once per idle period (the controller asks again on every pass): "no video and nothing in the log" is the failure to avoid.
void AfkRunner::noShow(const wchar_t* why){
  if(m_noShowT0==m_snap.enterT0) return;
  m_noShowT0=m_snap.enterT0;
  logLine(std::wstring(L"[AFK] the screen is due but cannot come up: ")+why);
}

// The player is ready and has been told to play, yet no picture moved (AfkVideoGate). Seen in the wild as "the video sometimes does not play" with nothing in the log:
// the engine had loaded the clip and captured its first frame, and then never played it. The first failure on a screen rebuilds the player (a fresh one starts the way the
// first screen of a run does); a second one means this tier cannot play here right now, and the usual ladder takes over. Both are logged with the engine's own state.
void AfkRunner::videoCheckFailed(){
  const int tier=m_s.tier; const bool lower=nextTier(tier)!=AFK_TIER_NONE;
  logLine(L"[AFK] VIDEO CHECK FAILED: tier "+std::to_wstring(tier)+L" ("+afkTierName(tier)+L") showed "+std::to_wstring(m_gate.got)+L" new video frame(s) in "+fmt1((double)m_gate.maxMs/1000.0)
    +L" s after it was told to play (failed check "+std::to_wstring(m_gateFails+1)+L" on this screen) -- "+(m_s.be?m_s.be->diag():std::wstring(L"no player")));
  if(++m_gateFails==1){ recoverDevice(L"the video check failed"); return; }
  fallBack(false,L"the video check failed on a fresh player as well: the picture does not move");
  if(!lower){                                                    // nowhere lower to go: the screen keeps this player and shows what it has (faded in, not popped in)
    m_gateDone=true; m_gateHoldMs=afkVideoHold((uint64_t)(tickMs()-m_enterMs));
    logLine(L"[AFK] there is no lower tier -- the screen keeps this player and shows whatever it has"); }
}

void AfkRunner::recheckDisplay(){
  AfkMonitor mon; if(!afkMonitorFromCursor(mon)||!m_s.be) return;
  m_s.hz=mon.hz; m_s.dpi=mon.dpiScale; m_s.be->setRefreshHz(mon.hz);
  if(mon.width()==m_s.W&&mon.height()==m_s.H&&mon.rc.left==m_s.rc.left&&mon.rc.top==m_s.rc.top) return;
  logLine(L"[AFK] the display layout changed while the screen was up -- adjusting to "+std::to_wstring(mon.width())+L"x"+std::to_wstring(mon.height()));
  m_s.rc=mon.rc;
  SetWindowPos(m_s.hwnd,HWND_TOPMOST,mon.rc.left,mon.rc.top,mon.width(),mon.height(),SWP_NOACTIVATE);
  if(mon.width()!=m_s.W||mon.height()!=m_s.H){
    if(m_s.be->resize(mon.width(),mon.height())){ m_s.W=mon.width(); m_s.H=mon.height(); }
    else replaceBackend(m_s.tier,mon); }
}

// ------------------------------------------------------------------------------------------------ one frame
void AfkRunner::frame(){
  const int64_t now=tickMs();
  double dt=(double)(now-m_lastFrameMs)/1000.0; m_lastFrameMs=now; if(dt<0.0) dt=0.0; if(dt>0.25) dt=0.25;

  IAfkBackend* before=m_s.be;
  upkeep();                                                      // throttled; may notice a failure and swap the back-end
  if(!m_visible||!m_s.be||m_s.be!=before) return;

  // Input seen by this thread ends the screen at once -- the UI thread's poll (50 ms, but only when it is not busy) may lag behind.
  bool watchInput=true;
#ifdef SW_TEST_HOOKS
  if(m_testNoInput) watchInput=false;
#endif
  if(watchInput&&!m_latched&&m_snap.afkNow&&now-m_enterMs>300){
    const DWORD li=afkLastInputTick();
    if(li&&li!=m_inputBase){ m_latched=true; m_latchT0=m_snap.enterT0; logLine(L"[AFK] input detected -- fading out"); } }

  const bool target=wantAfk();
  m_af=afkFadeStep(m_af,target,dt*1000.0);
  if(!target&&m_af<=0.001f){ leaveAfk(L"user is back"); return; }
  if(afkTakeDisplayChanged()){ recheckDisplay(); if(!m_visible||m_s.be!=before) return; }

  // per-frame parameters: the staged reveal runs from our own entry time (it always plays in full), then everything snaps to
  // "revealed" so that the exit is one uniform fade
  const bool afkEff=m_snap.afkNow&&!m_latched; const uint64_t since=(uint64_t)(now-m_enterMs);
  m_fp.W=m_s.W; m_fp.H=m_s.H; m_fp.uiScale=afkUiScale(m_s.dpi,m_s.H); m_fp.nowMs=(uint64_t)now; m_fp.dtSec=dt; m_fp.af=m_af;
  m_fp.starF=afkStage(afkEff,since,AFK_STAR_START,AFK_STAR_DUR);
  m_fp.vidF =m_gateDone?afkVideoStage(afkEff,since,m_gateHoldMs):0.f;    // the clip (and its sound) stays hidden until the video check has seen it move
  m_fp.txtF =afkStage(afkEff,since,AFK_TXT_START, AFK_TXT_DUR);
  m_fp.quoteA=afkQuoteAlpha(m_snap.phase,(uint64_t)now,(uint64_t)m_snap.phaseT0);
  { SYSTEMTIME st; GetLocalTime(&st); m_fp.hh=st.wHour; m_fp.mm=st.wMinute; m_fp.ss=st.wSecond; }

  // sound follows the picture: silent until the video starts to appear, fades out with the screen
  { float vol=m_af*m_fp.vidF; if(vol<0.f) vol=0.f; if(vol>1.f) vol=1.f;
    if(fabsf(vol-m_lastVol)>=0.02f||(vol<=0.f&&m_lastVol>0.f)||(vol>=1.f&&m_lastVol<1.f)){ m_s.be->setAudio(vol,false); m_lastVol=vol; } }

  // Nothing reaches the screen while the display is off or the lock screen is up. The GPU back-ends notice by themselves (Present reports "occluded"); the CPU
  // renderer cannot, so it is told here: display-off through the power notification, lock screen / UAC prompt through the input-desktop probe (twice a second).
  bool hidden=afkDisplayOff();
  if(m_s.tier==AFK_TIER_CPU){
    if(!hidden&&now-m_lockChkMs>=500){ m_lockChkMs=now; m_lockedNow=afkInputDesktopLocked(); }
    if(!hidden) hidden=m_lockedNow; }
  const int r=hidden?(int)AFKR_OCCLUDED:m_s.be->render(m_fp);
  if(C->abandoned.load()) return;                                // render() can block inside the driver; if this thread was given up meanwhile, touch nothing
  switch(r){
    case AFKR_PRESENTED:
      m_failStreak=0;
      if(m_occSince){ m_occSince=0;
        if(m_pausedOcc){ m_pausedOcc=false; m_s.be->setActive(true); m_wd.reset(); logLine(L"[AFK] the display is back -- playback resumed"); } }
      break;
    case AFKR_OCCLUDED:                                          // display off, session locked, covered: nothing reaches the screen
      m_failStreak=0;                                            // (a failure streak is CONSECUTIVE frames)
      m_gate.reset();                                            // no frame is shown either: the video check measures again once frames reach the screen
      if(!m_occSince) m_occSince=now;
      if(!m_pausedOcc&&now-m_occSince>3000){ m_pausedOcc=true; m_s.be->setActive(false); m_wd.reset(); logLine(L"[AFK] the window is not visible (display off or covered) -- playback paused"); }
      sleepWait(m_pausedOcc?250u:50u,false); return;
    case AFKR_DEVICE_LOST:
      recoverDevice(L"the GPU device was lost while rendering"); return;
    default:                                                     // a failed frame: a display switch or a driver hiccup may cost a few of these, so it takes 3 s of nothing else
      if(m_failStreak==0) m_failSince=now;
      if(++m_failStreak>=30&&now-m_failSince>=3000){ const int n=m_failStreak; m_failStreak=0;
        fallBack(true,std::to_wstring(n)+L" frames in a row (" +fmt1((double)(now-m_failSince)/1000.0)+L" s) could not be rendered -- see the [ME] lines above for the HRESULT"); return; }
      sleepWait(16,false); return;
  }

  // Power saving (shared/eco.h) caps the CPU: a late start or a slow stretch while it is on is not the player's fault, so neither the video check nor the watchdog judges it
  // (a verdict reached under the cap would be remembered against a tier that is fine). The clip is simply revealed.
  const bool eco=g_ecoOn.load();
  if(eco){ m_wd.reset();
    if(!m_gateDone&&m_s.ready){ m_gateDone=true; m_gateEco=true; m_gate.reset(); m_gateHoldMs=afkVideoHold((uint64_t)(now-m_enterMs));
      logLine(L"[AFK] video check skipped: power saving is on (the CPU is capped), so a slow start is not held against the player; the clip is revealed (tier "+std::to_wstring(m_s.tier)+L")"); } }

  // Is the picture moving? (GPU tiers, once the player is ready.) The clip is not revealed before it is; a player that does not move is rebuilt, then replaced by a lower tier.
  if(!eco&&!m_gateDone&&m_s.ready){
    const AfkVideoGate::Verdict g=m_gate.update(now,m_s.be->videoFrames());
    if(g==AfkVideoGate::V_PASS){
      m_gateDone=true; m_gatePassMs=now-m_enterMs; m_gateHoldMs=afkVideoHold((uint64_t)m_gatePassMs); if(m_firstPassMs<0) m_firstPassMs=m_gatePassMs;
      logLine(L"[AFK] video check passed: "+std::to_wstring(m_gate.got)+L" new frames within "+fmt1((double)(now-m_gate.t0)/1000.0)+L" s of the player being told to play; the clip is revealed (tier "
        +std::to_wstring(m_s.tier)+L")"); }
    else if(g==AfkVideoGate::V_FAIL){ videoCheckFailed(); return; } }

  // Is this tier keeping up? (Only the GPU tiers have somewhere to fall back to.)
  if(m_s.tier<=AFK_TIER_GPULITE&&!m_pausedOcc&&!m_wdOff&&!eco&&m_s.ready){
    const AfkWatchdog::Verdict v=m_wd.update(now,m_s.be->videoFrames(),m_expectFps);
    if(v==AfkWatchdog::V_BAD) logLine(L"[AFK] slow window: "+fmt1(m_wd.lastFps)+L" video fps (clip "+fmt1(m_s.clipFps)+L", display allows "+fmt1(m_expectFps)+L")");
    else if(v==AfkWatchdog::V_DOWNGRADE){
      fallBack(false,L"only "+fmt1(m_wd.lastFps)+L" video frames per second (at least "+fmt1(m_expectFps*m_wd.badFraction)+L" needed)"); return; } }

  if(now-m_statsMs>=5000){ m_s.be->logStats((uint64_t)(now-m_statsMs)); m_statsMs=now; }
  if(!m_goodRun&&now-m_enterMs>=AFK_GOOD_RUN_MS&&m_wd.badRun==0&&m_wd.lastFps>0.0&&afkJudgesTier(m_entryTier,m_s.tier)){ m_goodRun=true; afkPersistGoodRun(); }

  // The CPU renderer asks for a pause that lands the next frame on its grid: wait it out in full (an early wake-up for some message must not pull the next
  // frame forward), but stay responsive to a stop request.
  const int d=m_s.be->frameDelayMs();
  if(d>0){ const int64_t until=tickMs()+d;
    for(;;){ const int64_t left=until-tickMs(); if(left<=0) break; sleepWait((DWORD)left,false); if(C->stop.load()||C->abandoned.load()) break; } }
}
