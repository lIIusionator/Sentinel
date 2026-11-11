// afk_backend.h -- what the AFK controller needs from a video back-end, and what it hands one per frame.
//
// Two implementations exist:
//   afk_gpu_backend.cpp  Media Engine (GPU decode) + flip-model swap chain + Direct2D overlay     tiers 0 and 1
//   afk_cpu_backend.cpp  Media Foundation source reader + GDI+ compositing + BitBlt                tier 2 (last resort)
// The controller (afk_screen.cpp) owns the window and the render thread; back-ends never create threads that touch
// the window, so a back-end can be torn down and replaced (tier fallback, device loss) without recreating the process.
#pragma once
#include "sw_common.h"
#include "afk_helpers.h"

// Everything a back-end needs to draw one frame. The controller derives it from the UI-thread snapshot plus its own
// clock, so fades stay smooth even while the UI thread is busy.
struct AfkFrameParams{
  int W=0,H=0;                    // physical pixels of the window / swap chain
  float uiScale=1.0f;             // DPI scale, at least H/1080 (see afkUiScale)
  uint64_t nowMs=0;               // GetTickCount64()
  double dtSec=0.016;             // seconds since the previous frame (clamped)
  float af=0.f;                   // global fade 0..1 (screen to black, and back)
  float starF=0.f, vidF=0.f, txtF=0.f;   // staged reveal of stars / video / text
  float quoteA=1.f;               // quote cross-fade 0..1
  int hh=0,mm=0,ss=0;             // local time for the clock readout
  std::wstring quote;             // current quote, markup intact ({user}, **bold**)
  std::wstring user;
};

// pump() result: LOADING (first frame not there yet), READY, FAILED (cannot continue on this back-end),
// DEVICE_LOST (the GPU was reset / removed: the controller rebuilds the back-end, which is not the back-end's fault).
enum AfkBackendState{ AFKB_LOADING=0, AFKB_READY=1, AFKB_FAILED=2, AFKB_DEVICE_LOST=3 };
enum AfkRenderResult{ AFKR_PRESENTED=0, AFKR_OCCLUDED=1, AFKR_DEVICE_LOST=2, AFKR_FAILED=3 };

struct IAfkBackend{
  virtual ~IAfkBackend(){}
  virtual const wchar_t* name() const=0;
  // Creates the renderer on `hwnd` (a window the caller created on this thread and owns) and starts loading the clip
  // in the background (preload). `why` explains a failure.
  virtual bool init(HWND hwnd,int W,int H,const std::wstring& clip,double clipFps,float dpiScale,std::wstring& why)=0;
  // Housekeeping while hidden / loading. AFKB_READY once the first frame is available.
  virtual int pump(std::wstring& why)=0;
  virtual void setActive(bool active)=0;                  // start / pause video + audio
  virtual void setAudio(float volume,bool muted)=0;
  virtual bool resize(int W,int H)=0;
  virtual void setRefreshHz(int hz){ (void)hz; }          // display refresh rate (GPU back-end derives its present interval from it)
  virtual int render(const AfkFrameParams& p)=0;          // draw one frame and show it
  virtual int frameDelayMs()=0;                           // extra wait the controller should add after render() (CPU pacing)
  virtual uint64_t videoFrames()=0;                       // video frames shown so far (watchdog input)
  virtual double clipFps()=0;
  virtual void logStats(uint64_t elapsedMs)=0;            // called every ~5 s while visible
  virtual std::wstring diag(){ return std::wstring(); }   // the player's internal state in one line, for a failure report (the video check); empty = nothing to add
  virtual void shutdown()=0;
};

IAfkBackend* afkCreateGpuBackend(int tier);     // tier 0: 4K60 clip, tier 1: 1080p30 clip (same engine)
IAfkBackend* afkCreateCpuBackend();
