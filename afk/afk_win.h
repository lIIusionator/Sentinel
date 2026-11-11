// afk_win.h -- the Windows-specific odds and ends of the AFK screen: DPI awareness of the render thread, monitor lookup,
// the screen window itself, taking the foreground, thread priority boost, and the crash marker file.
#pragma once
#include "sw_common.h"

struct AfkMonitor{ RECT rc{0,0,0,0}; int hz=60; float dpiScale=1.0f; int width() const { return rc.right-rc.left; } int height() const { return rc.bottom-rc.top; } };

// The calling thread becomes per-monitor DPI aware (v2 if the OS has it), so windows it creates and the sizes it reads are
// PHYSICAL pixels -- a DPI-unaware process would otherwise get a 1536x864 "screen" on a 125 % laptop and DWM would stretch it.
void afkThreadDpiAware();

bool afkMonitorFromCursor(AfkMonitor& m);     // the monitor the mouse is on (primary if that cannot be determined)
bool afkMonitorPrimary(AfkMonitor& m);

// The screen window: borderless, topmost, hidden until afkShowScreenWindow. Created on (and owned by) the calling thread, and registered
// for the "display switched off / on" notification (afkDisplayOff). Destroy it with afkDestroyScreenWindow, on the same thread.
HWND afkCreateScreenWindow(const RECT& rc);
void afkDestroyScreenWindow(HWND h);
void afkShowScreenWindow(HWND h,const RECT& rc);
void afkHideScreenWindow(HWND h);
void afkCoverWithBlack(HWND h);               // GDI fill, for the moments between two back-ends
bool afkForceForeground(HWND h);              // real keyboard focus, even though nothing the user did started this process; false if Windows refused
bool afkTakeDisplayChanged();                 // true once after every WM_DISPLAYCHANGE
bool afkDisplayOff();                         // the monitor is switched off (power-setting notification; false until Windows says otherwise)
bool afkInputDesktopLocked();                 // the input desktop is not the user's: lock screen / UAC prompt / disconnected remote session
DWORD afkLastInputTick();                     // GetLastInputInfo().dwTime (0 if unavailable)
void afkPumpMessages();                       // dispatch everything waiting for the calling thread

// While the screen is up: 1 ms timer resolution, above-normal priority, MMCSS "Playback". Call on() / off() on the same thread.
class AfkBoost{
 public:
  ~AfkBoost(){ off(); }
  void on();
  void off();
 private:
  bool m_on=false; HANDLE m_mm=nullptr; int m_prio=0; HMODULE m_avrt=nullptr;
};

// "afk_active.flag" in the app folder: written before a GPU back-end is started and while it is on screen, removed once it is
// idle and healthy or the app stops normally. Finding it at the next start means the process died with the GPU player active
// (driver crash, hang, killed by the user): that tier is then treated as failed.
// The marker carries the build id: one left by another build's crash says nothing about this one.
void afkMarkerWrite(int tier);
void afkMarkerClear();
int  afkMarkerRead();                         // tier written by this build, or -1 if there is no (usable) marker
