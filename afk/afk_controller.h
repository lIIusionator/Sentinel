// afk_controller.h -- internal to the AFK screen module (afk_screen.cpp, afk_tiers.cpp, afk_loop.cpp). Nothing else includes it.
//
// One AfkRunner lives on one "controller thread". It owns that thread's window and video back-end; the UI thread only reaches it
// through the published snapshot (afk_state.h) and through the AfkCtl block below, which the supervisor (afkScreenWatch) reads.
#pragma once
#include "afk_backend.h"
#include "afk_assets.h"
#include "afk_screen.h"
#include "afk_win.h"
#include "settings.h"
#include <deque>

// ------------------------------------------------------------------------------------------------ shared with the supervisor
struct AfkCtl{
  std::atomic<bool> stop{false};          // asked to finish
  std::atomic<bool> abandoned{false};     // replaced by a newer controller: touch nothing shared, unwind and exit
  std::atomic<bool> finished{false};      // the thread function has returned
  std::atomic<bool> visible{false};       // the AFK window is on screen
  std::atomic<int64_t> beat{0};           // GetTickCount64() of the last pass through the loop
  std::atomic<int64_t> busySince{0};      // != 0 while inside a call that may legitimately take long (GPU driver, unpacking clips)
  std::atomic<const wchar_t*> busyWhat{L""};
  std::atomic<int> refs{1};               // the creator's reference (handed to the module global) -- the thread adds its own
  bool cpuOnly=false;                     // a replacement started after the GPU player hung: never touches the GPU again
  HANDLE thread=nullptr;
};

// Marks the controller as "inside a call that may block" for the supervisor's report; refreshes the heartbeat on the way out.
struct AfkBusy{
  AfkCtl* c;
  AfkBusy(AfkCtl* x,const wchar_t* what):c(x){ c->busyWhat.store(what); c->busySince.store((int64_t)GetTickCount64()); }
  ~AfkBusy(){ c->busySince.store(0); c->beat.store((int64_t)GetTickCount64()); }
  AfkBusy(const AfkBusy&)=delete; AfkBusy& operator=(const AfkBusy&)=delete;
};

// Set once the GPU player hung or crashed in this process: no controller touches the media engine / D3D any more.
extern std::atomic<bool> g_afkGpuPoisoned;

// afk_tiers.cpp -- what the app remembers about this machine
void afkPersistDowngrade(int toTier,bool hard);
void afkPersistGoodRun();
void afkExpireMemory();                               // drop remembered verdicts older than a week (the app can run for weeks without a restart)
const wchar_t* afkTierName(int tier);

// ------------------------------------------------------------------------------------------------ one back-end and its window
struct AfkSlot{
  IAfkBackend* be=nullptr; HWND hwnd=nullptr; int tier=AFK_TIER_NONE;
  RECT rc{0,0,0,0}; int W=0,H=0; int hz=60; float dpi=1.0f;
  int64_t openedMs=0; bool ready=false; double clipFps=30.0;
  bool used=false;                                    // a screen has already been shown on this player (see AfkRunner::renewSlot)
};

class AfkRunner{
 public:
  explicit AfkRunner(AfkCtl* c):C(c){}
  void run();                                         // the thread body (never throws)

 private:
  // ---- afk_tiers.cpp: back-end lifecycle and the tier ladder
  int  startTier() const;
  int  nextTier(int cur) const;
  bool openSlot(int tier,const AfkMonitor& mon,AfkSlot& out,std::wstring& why);
  void stopSlot(AfkSlot& s);                          // shut the back-end down (the window stays)
  void renewSlot();                                   // the next screen is near (snapshot.warm): drop the player that already ran a screen, the preload path builds a fresh one
  bool climbDown(int tier,const AfkMonitor& mon);     // open `tier`, or the next ones, into m_s; false when nothing works
  bool replaceBackend(int tier,const AfkMonitor& mon);// stop the live back-end and open `tier` (or lower) in its place, keeping the screen up
  void fallBack(bool hard,const std::wstring& why);   // the live tier failed: remember it, go one tier down
  void recoverDevice(const std::wstring& why);        // GPU reset / removed: rebuild the same tier, or fall back if it keeps happening
  void upkeep();                                      // hidden: preload, pump, health check

  // ---- afk_loop.cpp: the screen itself
  void body();
  void finish();
  void refreshSnapshot();
  bool wantAfk();
  bool warmNow();                                     // idle for most of the wait (snapshot.warm), unless that is the stale value from before the screen we just left
  bool enterAfk();
  void activateSlot();                                // show the window, start playback (used on entry and after a back-end swap)
  void leaveAfk(const wchar_t* why);
  void noShow(const wchar_t* why);                    // the screen is due but cannot come up: say so (once per idle period)
  void videoCheckFailed();                            // the player is ready and was told to play, yet no picture moved: rebuild it once, then go one tier down
  void frame();
  void recheckDisplay();
  void sleepWait(DWORD ms,bool wakeOnState);

  AfkCtl* C;
  AfkMedia m_media; bool m_haveMedia=false; int m_mode=AFK_AUTO;
  AfkSlot m_s;                                        // the live back-end (be == nullptr: none)
  bool m_noBackend=false; int64_t m_buildAtMs=0, m_retryBuildMs=0, m_nextPumpMs=0, m_startMs=0; bool m_coInit=false;

  AfkSnapshot m_snap; uint32_t m_snapVer=0; AfkFrameParams m_fp;

  bool m_visible=false; float m_af=0.f; int64_t m_enterMs=0, m_lastFrameMs=0;
  bool m_warmStale=false;                                                 // the screen just ended: the UI thread's "warm" may still be the old one (cleared once it publishes false)
  bool m_latched=false; ULONGLONG m_latchT0=0; DWORD m_inputBase=0;      // input seen on this thread ends the screen at once
  // The snapshot says "idle" -- but it may be old by the time this thread gets to act on it (a driver call can take seconds). m_snapInput is the last-input
  // tick when the current idle period was first seen; if the tick has moved by the time the screen is about to appear, the user is back.
  DWORD m_snapInput=0; bool m_prevAfk=false; ULONGLONG m_prevEnterT0=0;
  // The tier the current player STARTED on (set at every preload): slow-session verdicts are about that tier only (afkJudgesTier). m_lastOpenBlame: the last failed openSlot was the player's own
  // failure (not a missing window, monitor or clip), so it may be held against the tier.
  int m_entryTier=AFK_TIER_NONE; bool m_lastOpenBlame=false;
  int64_t m_lockChkMs=0; bool m_lockedNow=false;                         // CPU tier: is the input desktop somebody else's (lock screen)? probed twice a second
  AfkBoost m_boost; AfkWatchdog m_wd; bool m_wdOff=false; double m_expectFps=60.0; float m_lastVol=-1.f;   // m_wdOff: slow, but no lower tier exists
  int64_t m_occSince=0; bool m_pausedOcc=false;
  int m_failStreak=0; int64_t m_failSince=0; std::deque<int64_t> m_losses;   // consecutive failed frames, and since when
  uint64_t m_framesAtEnter=0; int64_t m_statsMs=0; bool m_goodRun=false;
  // The video check (AfkVideoGate): restarted by every activateSlot(); the clip is not revealed (nor its sound let in) before it has passed. m_gateFails counts the failed
  // checks of THIS screen (the first rebuilds the player, the second goes a tier down); m_gatePassMs: when it passed, from the screen's start (-1: it did not).
// m_gateEco: power saving (g_ecoOn) was on, so the check was skipped and the clip revealed (the CPU is capped; nobody judges the player for that).
  AfkVideoGate m_gate; bool m_gateDone=false; bool m_gateEco=false; int m_gateFails=0; uint64_t m_gateHoldMs=0; int64_t m_gatePassMs=-1;
  // For the "screen down" line, which must describe the whole screen even when the player was swapped during it: frames shown by players that were replaced, and when the
  // first video check passed on this screen (-1: none did).
  uint64_t m_framesBanked=0; int64_t m_firstPassMs=-1;
  ULONGLONG m_noShowT0=0;                                                // the idle period for which "the screen is due but cannot come up" was already logged

#ifdef SW_TEST_HOOKS
  // Environment switches for the Wine smoke tests (compiled out of release builds):
  //   SW_TEST_AT=<s>   pretend the user went idle <s> seconds after start    SW_TEST_FOR=<s>  ... and came back <s> seconds later
  //   SW_TEST_EVERY=<s>  repeat that idle period every <s> seconds (several AFK sessions in one run)
  //   SW_TEST_DASH=min|close  (ui/main.cpp) start with the dashboard minimized / closed to the tray
  //   SW_TEST_HANG=1   block forever inside the first GPU back-end start (exercises the supervisor)
  //   SW_TEST_MODE=auto|gpu|cpu   override settings.json                       SW_TEST_NOINPUT=1  ignore real input
  int m_testAt=-1, m_testFor=-1, m_testEvery=0; bool m_testHang=false, m_testNoInput=false;
#endif
};
