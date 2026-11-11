// afk_gpu_backend.cpp -- tiers 0 and 1: the GPU video player (media-engine/) with the Direct2D HUD on top.
//
// The render loop in afk_screen.cpp calls render() once per display refresh; this class
//   * lets the media engine pick up a new decoded frame if there is one, copy it into the swap chain,
//   * draws the HUD on top (afk_overlay_d2d.cpp) and presents (vsync-paced -- Present blocks until the next refresh),
//   * keeps the numbers the watchdog and the log need (video frames shown, per-frame costs).
// Nothing here touches the window except through the engine's swap chain.
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include "afk_backend.h"
#include "afk_overlay_d2d.h"
#include "media_engine.h"
#include "settings.h"
#include <dxgi.h>

static void CALLBACK engineLogThunk(const wchar_t* line){ logLine(std::wstring(L"[AFK-GPU] ")+line); }

namespace {

class AfkGpuBackend final : public IAfkBackend{
 public:
  explicit AfkGpuBackend(int tier):m_tier(tier){}
  ~AfkGpuBackend() override { shutdown(); }

  const wchar_t* name() const override { return m_tier==AFK_TIER_GPU4K?L"GPU player, 4K 60 fps clip":L"GPU player, 1080p 30 fps clip"; }

  bool init(HWND hwnd,int W,int H,const std::wstring& clip,double clipFps,float dpiScale,std::wstring& why) override{
    (void)dpiScale; m_clipFps=clipFps>1.0?clipFps:30.0;
    MediaEngine_SetLogger(engineLogThunk);
    MeConfig cfg={}; cfg.structSize=sizeof(cfg); cfg.width=(UINT32)W; cfg.height=(UINT32)H; cfg.syncInterval=(UINT32)m_sync;
    cfg.loop=TRUE; cfg.muted=TRUE; cfg.volume=1.0f; cfg.clipFps=m_clipFps; cfg.clearColor=0xFF060810u;
    const int gpuPref=g_afkGpuPref.load(); cfg.preferredVendor=afkGpuVendorId(gpuPref);
    if(gpuPref!=AFKGPU_DEFAULT) logLine(std::wstring(L"[AFK-GPU] settings.json asks for the ")+fromUtf8(afkGpuPrefName(gpuPref))+L" GPU (afkGpu)");
    HRESULT hr=MediaEngine_InitEx(hwnd,clip.c_str(),&cfg);
    if(FAILED(hr)){ why=L"MediaEngine_InitEx failed, hr="+hrHex(hr); return false; }
    m_inited=true; return true; }

  int pump(std::wstring& why) override{
    if(!m_inited){ why=L"not initialised"; return AFKB_FAILED; }
    HRESULT hr=MediaEngine_Pump();
    if(hr==S_OK){ if(!m_ready){ m_ready=true; m_readyAt=hnsNow(); } return AFKB_READY; }
    if(hr==S_FALSE) return AFKB_LOADING;
    bool lost=(hr==DXGI_ERROR_DEVICE_REMOVED||hr==DXGI_ERROR_DEVICE_RESET||hr==DXGI_ERROR_DEVICE_HUNG||hr==DXGI_ERROR_DRIVER_INTERNAL_ERROR);
    why=(lost?std::wstring(L"GPU device removed/reset"):std::wstring(L"media engine error"))+L", hr="+hrHex(hr);
    return lost?AFKB_DEVICE_LOST:AFKB_FAILED; }

  void setActive(bool active) override{ if(!m_inited) return; m_active=active; MediaEngine_SetPlaybackState(active?ME_STATE_PLAYING:ME_STATE_PAUSED); }
  void setAudio(float volume,bool muted) override{ if(m_inited) MediaEngine_SetVolume(volume,muted?TRUE:FALSE); }
  void setRefreshHz(int hz) override{ m_sync=afkSyncInterval(hz); if(m_inited) MediaEngine_SetSyncInterval((UINT32)m_sync); }

  bool resize(int W,int H) override{
    if(!m_inited) return false;
    m_overlay.releaseTargets();                         // D2D must not hold the old back buffers during ResizeBuffers
    return SUCCEEDED(MediaEngine_Resize((UINT32)W,(UINT32)H)); }

  int render(const AfkFrameParams& p) override{
    if(!m_inited) return AFKR_FAILED;
    m_params=p;
    float ready=m_ready?afkVideoReadyFade((hnsNow()-m_readyAt)/10000):0.f;       // never pop the video in late
    m_videoFade=p.af*p.vidF*ready;
    UINT32 flags=0; HRESULT hr=MediaEngine_RenderFrameEx(&AfkGpuBackend::overlayThunk,this,&flags);
    if(flags&ME_FRAME_NEW_VIDEO) m_frames++;
    if(hr==S_OK){ m_presents++; return AFKR_PRESENTED; }
    if(hr==S_FALSE) return AFKR_OCCLUDED;
    if(hr==DXGI_ERROR_DEVICE_REMOVED||hr==DXGI_ERROR_DEVICE_RESET||hr==DXGI_ERROR_DEVICE_HUNG||hr==DXGI_ERROR_DRIVER_INTERNAL_ERROR) return AFKR_DEVICE_LOST;
    return AFKR_FAILED; }

  int frameDelayMs() override{ return 0; }               // Present() already blocks until the next refresh
  uint64_t videoFrames() override{ return m_frames; }
  double clipFps() override{ return m_clipFps; }

  void logStats(uint64_t elapsedMs) override{
    if(!m_inited||!elapsedMs) return;
    MeStats st; memset(&st,0,sizeof(st)); st.structSize=sizeof(st); MediaEngine_GetStats(&st);
    double sec=(double)elapsedMs/1000.0; uint64_t df=m_frames-m_stFrames, dp=m_presents-m_stPresents; m_stFrames=m_frames; m_stPresents=m_presents;
    std::wstring l=L"[AFKVID] stats (GPU, "+std::to_wstring(st.bufferWidth)+L"x"+std::to_wstring(st.bufferHeight)+L"): video "+fmt1((double)df/sec)+L" fps of "+fmt1(m_clipFps)
      +L" | display "+fmt1((double)dp/sec)+L" fps (sync "+std::to_wstring(m_sync)+L") | per frame: frame-transfer "+fmt1(st.avgTransferMs)+L" ms, HUD "+fmt1(st.avgOverlayMs)
      +L" ms, present "+fmt1(st.avgPresentMs)+L" ms | longest gap between video frames "+fmt1(st.maxFrameGapMs)+L" ms, stalls over 120 ms "+std::to_wstring(st.frameStalls)
      +L" | engine dropped "+std::to_wstring(st.engineFramesDropped)+(st.blindMode?L" | blind mode":L"")+L" | "+st.adapter;
    logLine(l); }

  std::wstring diag() override{
    MeStats st; memset(&st,0,sizeof(st)); st.structSize=sizeof(st);
    if(!m_inited||!MediaEngine_GetStats(&st)) return L"the player is not initialised";
    auto f=[&](UINT32 bit,const wchar_t* yes,const wchar_t* no){ return std::wstring((st.stateFlags&bit)?yes:no); };
    return L"player: status "+std::to_wstring(st.status)+L" (0 idle, 1 loading, 2 ready, 3 error, 4 device lost), "+f(ME_SF_STARTED,L"loaded",L"NOT loaded")+L", "
      +f(ME_SF_HAVE_FRAME,L"first frame captured",L"NO first frame")+L", "+f(ME_SF_WANT_PLAY,L"play requested",L"play NOT requested")+L", "
      +f(ME_SF_PLAY_ISSUED,L"Play() in effect",L"Play() NOT in effect")+L", engine says "+f(ME_SF_PAUSED,L"PAUSED",L"playing")+f(ME_SF_ENDED,L" + ENDED",L"")
      +L", clip time "+fmt1(st.clipTimeSec)+L" of "+fmt1(st.durationSec)+L" s, ready state "+std::to_wstring(st.readyState)+L", last Play/Pause hr="+hrHex(st.lastPlayHr)
      +L", last frame poll hr="+hrHex(st.lastTickHr)+L", frames delivered "+std::to_wstring(st.videoFramesDelivered)+L", stream-rendering errors "+std::to_wstring(st.renderErrors)
      +L", transfer failures "+std::to_wstring(st.transferFailures)+(st.blindMode?L", blind mode":L"")+L", opened by "+st.source+L", "+st.adapter; }

  void shutdown() override{
    if(!m_inited) return; m_inited=false;
    m_overlay.releaseAll(); MediaEngine_Shutdown(); }

 private:
  static void CALLBACK overlayThunk(void* user,const MeOverlayArgs* a){ if(user&&a) static_cast<AfkGpuBackend*>(user)->onOverlay(*a); }
  void onOverlay(const MeOverlayArgs& a){
    if(a.op==ME_OVERLAY_RELEASE){ m_overlay.releaseTargets(); return; }
    m_overlay.draw(a.d3dDevice,a.backBuffer,a.width,a.height,(a.videoFlags&ME_FRAME_HAVE_VIDEO)!=0,m_videoFade,m_params); }

  int m_tier; bool m_inited=false, m_ready=false, m_active=false; int64_t m_readyAt=0; int m_sync=1;
  double m_clipFps=60.0; float m_videoFade=0.f;
  AfkOverlayD2D m_overlay; AfkFrameParams m_params;
  uint64_t m_frames=0,m_presents=0,m_stFrames=0,m_stPresents=0;
};

} // namespace

IAfkBackend* afkCreateGpuBackend(int tier){ return new AfkGpuBackend(tier); }
