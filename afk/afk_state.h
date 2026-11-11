// afk_state.h -- what the UI thread tells the AFK render thread. The render thread never reads UI-thread
// globals directly; it copies this snapshot (under a lock) once per frame and derives its own fade timing
// from the timestamps, so it stays smooth even when the UI thread is busy.
#pragma once
#include "sw_common.h"

struct AfkSnapshot{
  bool afkNow=false;            // user is idle right now
  bool warm=false;              // idle for most of the wait (afkPreloadLeadMs before the screen is due, and while it is): have a fresh, ready player by then
  ULONGLONG enterT0=0;          // GetTickCount64() when the current AFK session began (drives the staged reveal)
  int phase=0;                  // quote fade: 0 stable, 1 fading out, 2 fading in
  ULONGLONG phaseT0=0;          // GetTickCount64() when `phase` began
  std::wstring quote;           // current quote ({user} not yet substituted, **bold** markers intact)
  std::wstring user;            // display name for {user}
};

void afkScreenPublish(const AfkSnapshot& s);    // UI thread (also wakes the render thread)
AfkSnapshot afkScreenSnapshot();                // render thread
uint32_t afkSnapshotVersion();                  // bumps on every publish: the render thread copies the snapshot only when it changed
HANDLE afkStateEvent();                         // auto-reset event, signalled by every publish (the idle render thread waits on it)
