// media_engine.cpp -- GPU video player: Media Foundation Media Engine (frame-server mode) + D3D11 + DXGI flip-model swap chain.
//
// Why this design (the lessons of the earlier CPU / SourceReader attempts on the target laptop):
//  * Decoding goes through the Media Engine, which builds the same hardware-accelerated pipeline the browsers use
//    (DXVA decoder -> GPU video processor); the CPU never copies pixels. A 4K60 clip costs roughly what it costs in a browser.
//  * New frames are detected with OnVideoStreamTick and moved into a back-buffer-sized texture with TransferVideoFrame
//    (GPU colour conversion + scaling + cover-crop in one blit). Each render tick then only copies that texture into the
//    swap chain, lets the host draw its HUD (Direct2D) and presents -- so the HUD can animate at the full refresh rate
//    even when the video is slower, and a slow decoder never blocks the UI.
//  * Everything D3D/DXGI/MF-specific is resolved at run time, so the host exe still starts (and falls back to the CPU
//    renderer) on a machine without a working GPU stack. Every failing step logs its name and HRESULT.
//
// All entry points must be called from one thread. Media Foundation's callback threads only touch atomics.
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include "media_engine.h"
#include "me_util.h"
#include <d3d10.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mferror.h>
#include "mfmediaengine.h"
#include <atomic>
#include <string>
#include <stdarg.h>
#include <stdio.h>
#include <wchar.h>
#include <math.h>

// ------------------------------------------------------------------------------------------------ small utilities
static std::atomic<MeLogProc> g_logProc{nullptr};
static void LOG(const wchar_t* fmt,...){
  wchar_t b[700]; va_list a; va_start(a,fmt); _vsnwprintf(b,699,fmt,a); va_end(a); b[699]=0;
  MeLogProc p=g_logProc.load(); if(p) p(b); else { OutputDebugStringW(b); OutputDebugStringW(L"\n"); } }
static double nowMs(){ static LARGE_INTEGER f={}; if(!f.QuadPart) QueryPerformanceFrequency(&f);
  LARGE_INTEGER c; QueryPerformanceCounter(&c); return (double)c.QuadPart*1000.0/(double)f.QuadPart; }

template<class T> struct MeCom{
  T* p=nullptr; MeCom(){} ~MeCom(){ reset(); } MeCom(const MeCom&)=delete; MeCom& operator=(const MeCom&)=delete;
  void reset(){ if(p){ p->Release(); p=nullptr; } }
  T** put(){ reset(); return &p; }
  T* get() const { return p; } T* operator->() const { return p; } explicit operator bool() const { return p!=nullptr; } };

// The few GUIDs the engine needs are spelled out here instead of coming from libmfuuid, because older MinGW import
// libraries predate some of them (values from mfmediaengine.h).
static const GUID kCLSID_MFMediaEngineClassFactory   ={0xb44392da,0x499b,0x446b,{0xa4,0xcb,0x00,0x5f,0xea,0xd0,0xe6,0xd5}};
static const GUID kMF_MEDIA_ENGINE_CALLBACK          ={0xc60381b8,0x83a4,0x41f8,{0xa3,0xd0,0xde,0x05,0x07,0x68,0x49,0xa9}};
static const GUID kMF_MEDIA_ENGINE_DXGI_MANAGER      ={0x065702da,0x1094,0x486d,{0x86,0x17,0xee,0x7c,0xc4,0xee,0x46,0x48}};
static const GUID kMF_MEDIA_ENGINE_VIDEO_OUTPUT_FORMAT={0x5066893c,0x8cf9,0x42bc,{0x8b,0x8a,0x47,0x22,0x12,0xe5,0x27,0x26}};

typedef HRESULT (WINAPI *PFN_MFCreateDXGIDeviceManager_t)(UINT*,IMFDXGIDeviceManager**);

// ------------------------------------------------------------------------------------------------ engine state
struct Engine;

// IMFMediaEngineNotify: called by Media Foundation on its own threads (sometimes synchronously from inside our calls).
// It must never call back into the engine; it only records what happened.
class MeNotify final : public IMFMediaEngineNotify {
 public:
  explicit MeNotify(Engine* e):m_owner(e){}
  STDMETHODIMP QueryInterface(REFIID riid,void** ppv) override {
    if(!ppv) return E_POINTER;
    if(riid==IID_IUnknown||riid==__uuidof(IMFMediaEngineNotify)){ *ppv=static_cast<IMFMediaEngineNotify*>(this); AddRef(); return S_OK; }
    *ppv=nullptr; return E_NOINTERFACE; }
  STDMETHODIMP_(ULONG) AddRef() override { return (ULONG)++m_ref; }
  STDMETHODIMP_(ULONG) Release() override { long r=--m_ref; if(!r) delete this; return (ULONG)r; }
  STDMETHODIMP EventNotify(DWORD ev,DWORD_PTR p1,DWORD p2) override;
  // After detach() returns no callback is running and none will touch the engine again.
  void detach(){ m_owner.store(nullptr); for(int i=0;i<2000 && m_inflight.load()>0;i++) Sleep(1); }
 private:
  std::atomic<long> m_ref{1}; std::atomic<Engine*> m_owner; std::atomic<int> m_inflight{0};
};

struct Engine{
  // configuration
  HWND hwnd=nullptr; std::wstring path; MeConfig cfg{}; UINT sync=1;
  // D3D / DXGI
  MeCom<ID3D11Device> dev; MeCom<ID3D11DeviceContext> ctx; MeCom<IDXGISwapChain1> sc; MeCom<ID3D11Texture2D> vtex;
  MeCom<IMFDXGIDeviceManager> mgr; UINT resetToken=0;
  UINT bw=0,bh=0; UINT swapEffect=0; UINT vendor=0,deviceId=0; wchar_t adapter[96]={0};
  // media engine
  MeCom<IMFMediaEngine> eng; MeCom<IMFMediaEngineEx> engx; MeNotify* notify=nullptr;
  bool mfStarted=false, comOwned=false;
  MeOverlayProc overlayProc=nullptr; void* overlayUser=nullptr;
  // lifecycle (render thread)
  int status=ME_STATUS_LOADING; HRESULT lastError=S_OK;
  bool started=false;            // data loaded, first playback kick issued
  bool haveFrame=false;          // first frame captured into vtex
  bool forceTransfer=false;      // re-draw vtex from the engine's current frame (after resize)
  bool wantPlay=false, playIssued=false, muted=true; float volume=1.0f;
  float appliedVol=-1.f; int appliedMute=-1;
  bool noCrop=false, blind=false, cropLogged=false;
  int srcAttempt=0; bool retry=false; double retryAt=0; wchar_t srcName[24]={0};
  double t0=0, startedAt=0, firstFrameAt=0, playStartMs=0, playStartMediaT=0;
  // loadT0: when the first way of opening the clip was handed to the engine -- the first-frame budget counts from there, not from Init (a slow GPU
  // wake-up inside Init must not eat it). openedAt: the latest open attempt. metaSeenAt: when LOADEDMETADATA was first noticed.
  double loadT0=0, openedAt=0, metaSeenAt=0;
  HRESULT lastTickHr=S_FALSE;    // the latest OnVideoStreamTick result (the "no first frame" report and MeStats)
  HRESULT lastPlayHr=S_OK; int playLogged=0;      // the latest Play() / Pause() result, and how many of them were logged (notePlay)
#ifdef SW_TEST_HOOKS
  bool testStuck=false;          // SW_TEST_STUCK: this player ignores the host's request to play (the host's video check must notice, rebuild, fall back)
#endif
  unsigned long delivered=0, presents=0, ticksSincePlay=0, transferFails=0, drawFails=0; MeBlindCounter blindCtr;
  DWORD vw=0,vh=0; double duration=0;
  double avgTransfer=0, avgOverlay=0, avgPresent=0;
  double lastNewAt=0, maxGapMs=0; unsigned long stalls=0;     // gaps between new video frames (see noteFrame)
  // set from Media Foundation threads
  std::atomic<bool> evMeta{false}, evData{false}, evCanPlay{false}, evFirst{false}, evPlaying{false}, evEnded{false}, evError{false}, evResLost{false};
  std::atomic<int> errCode{0}; std::atomic<unsigned> errHr{0}; std::atomic<int> evLogged{0}; std::atomic<int> evRenderErr{0};

  // Events that fire continuously (every volume step of the fade, every clock tick) tell nothing: they are neither logged nor counted
  // against the log budget, so that a late ERROR / ENDED / RESOURCELOST still makes it into the log.
  static bool noisyEvent(DWORD ev){
    return ev==MF_MEDIA_ENGINE_EVENT_TIMEUPDATE||ev==MF_MEDIA_ENGINE_EVENT_PROGRESS||ev==MF_MEDIA_ENGINE_EVENT_VOLUMECHANGE
         ||ev==MF_MEDIA_ENGINE_EVENT_PURGEQUEUEDEVENTS||ev==MF_MEDIA_ENGINE_EVENT_BALANCECHANGE; }

  static const wchar_t* evName(DWORD ev){
    switch(ev){
      case MF_MEDIA_ENGINE_EVENT_LOADSTART:          return L"LOADSTART";         case MF_MEDIA_ENGINE_EVENT_SUSPEND:            return L"SUSPEND";
      case MF_MEDIA_ENGINE_EVENT_ABORT:              return L"ABORT";             case MF_MEDIA_ENGINE_EVENT_ERROR:              return L"ERROR";
      case MF_MEDIA_ENGINE_EVENT_EMPTIED:            return L"EMPTIED";           case MF_MEDIA_ENGINE_EVENT_STALLED:            return L"STALLED";
      case MF_MEDIA_ENGINE_EVENT_PLAY:               return L"PLAY";              case MF_MEDIA_ENGINE_EVENT_PAUSE:              return L"PAUSE";
      case MF_MEDIA_ENGINE_EVENT_LOADEDMETADATA:     return L"LOADEDMETADATA";    case MF_MEDIA_ENGINE_EVENT_LOADEDDATA:         return L"LOADEDDATA";
      case MF_MEDIA_ENGINE_EVENT_WAITING:            return L"WAITING";           case MF_MEDIA_ENGINE_EVENT_PLAYING:            return L"PLAYING";
      case MF_MEDIA_ENGINE_EVENT_CANPLAY:            return L"CANPLAY";           case MF_MEDIA_ENGINE_EVENT_CANPLAYTHROUGH:     return L"CANPLAYTHROUGH";
      case MF_MEDIA_ENGINE_EVENT_SEEKING:            return L"SEEKING";           case MF_MEDIA_ENGINE_EVENT_SEEKED:             return L"SEEKED";
      case MF_MEDIA_ENGINE_EVENT_ENDED:              return L"ENDED";             case MF_MEDIA_ENGINE_EVENT_RATECHANGE:         return L"RATECHANGE";
      case MF_MEDIA_ENGINE_EVENT_DURATIONCHANGE:     return L"DURATIONCHANGE";    case MF_MEDIA_ENGINE_EVENT_FORMATCHANGE:       return L"FORMATCHANGE";
      case MF_MEDIA_ENGINE_EVENT_BUFFERINGSTARTED:   return L"BUFFERINGSTARTED";  case MF_MEDIA_ENGINE_EVENT_BUFFERINGENDED:     return L"BUFFERINGENDED";
      case MF_MEDIA_ENGINE_EVENT_FRAMESTEPCOMPLETED: return L"FRAMESTEPCOMPLETED";case MF_MEDIA_ENGINE_EVENT_FIRSTFRAMEREADY:   return L"FIRSTFRAMEREADY";
      case MF_MEDIA_ENGINE_EVENT_TRACKSCHANGE:       return L"TRACKSCHANGE";      case MF_MEDIA_ENGINE_EVENT_RESOURCELOST:       return L"RESOURCELOST";
      case MF_MEDIA_ENGINE_EVENT_STREAMRENDERINGERROR:return L"STREAMRENDERINGERROR"; case MF_MEDIA_ENGINE_EVENT_AUDIOENDPOINTCHANGE:return L"AUDIOENDPOINTCHANGE";
      default: return L"?"; } }

  void onEvent(DWORD ev,DWORD_PTR p1,DWORD p2){
    if(!noisyEvent(ev) && evLogged.fetch_add(1)<80)
      LOG(L"[ME] event %ls (%u) (p1=%u, p2=0x%08X)",evName(ev),(unsigned)ev,(unsigned)p1,(unsigned)p2);
    switch(ev){
      case MF_MEDIA_ENGINE_EVENT_LOADEDMETADATA: evMeta=true; break;
      case MF_MEDIA_ENGINE_EVENT_LOADEDDATA:     evData=true; break;
      case MF_MEDIA_ENGINE_EVENT_CANPLAY:
      case MF_MEDIA_ENGINE_EVENT_CANPLAYTHROUGH: evCanPlay=true; break;
      case MF_MEDIA_ENGINE_EVENT_FIRSTFRAMEREADY:evFirst=true; break;
      case MF_MEDIA_ENGINE_EVENT_PLAYING:        evPlaying=true; break;
      case MF_MEDIA_ENGINE_EVENT_PAUSE:          evPlaying=false; break;
      case MF_MEDIA_ENGINE_EVENT_ENDED:          evEnded=true; break;
      case MF_MEDIA_ENGINE_EVENT_ERROR:          errCode=(int)p1; errHr=(unsigned)p2; evError=true; break;
      case MF_MEDIA_ENGINE_EVENT_RESOURCELOST:   evResLost=true; break;
      // Only counted (and logged above): it can also be an audio-endpoint problem while the picture is fine, so it must not stop the player by itself.
      case MF_MEDIA_ENGINE_EVENT_STREAMRENDERINGERROR: evRenderErr++; break;
      default: break; }
  }
};

STDMETHODIMP MeNotify::EventNotify(DWORD ev,DWORD_PTR p1,DWORD p2){
  m_inflight++;
  Engine* e=m_owner.load();
  if(e) e->onEvent(ev,p1,p2);
  m_inflight--;
  return S_OK; }

static Engine* g_e=nullptr;

static const wchar_t* hrName(HRESULT hr){
  switch((unsigned)hr){
    case 0x887A0005u: return L"DXGI_ERROR_DEVICE_REMOVED"; case 0x887A0007u: return L"DXGI_ERROR_DEVICE_RESET";
    case 0x887A0006u: return L"DXGI_ERROR_DEVICE_HUNG";    case 0x887A0020u: return L"DXGI_ERROR_DRIVER_INTERNAL_ERROR";
    case 0x887A0001u: return L"DXGI_ERROR_INVALID_CALL";   case 0x80070057u: return L"E_INVALIDARG";
    case 0xC00D36C4u: return L"MF_E_UNSUPPORTED_BYTESTREAM_TYPE"; case 0xC00D36B4u: return L"MF_E_INVALIDMEDIATYPE";
    case 0xC00D5212u: return L"MF_E_TOPO_CODEC_NOT_FOUND"; case 0xC00D36E6u: return L"MF_E_SOURCERESOLVER_MUTUALLY_EXCLUSIVE_FLAGS";
    default: return L""; } }
#define HRN(h) (unsigned)(h), hrName(h)

// Every Play() / Pause() the engine is given. The results used to be dropped, so a player that refused to start left no trace anywhere: the first ten per player
// are logged whatever they return (a screen needs about four), failures up to forty; every result is kept for the host's "the picture does not move" report.
static void notePlay(Engine& e,const wchar_t* what,HRESULT hr){
  e.lastPlayHr=hr;
  const bool log=e.playLogged<10||(FAILED(hr)&&e.playLogged<40); e.playLogged++;
  if(log) LOG(L"[ME] %ls -> hr=0x%08X %ls (clip time %.2f s, ready state %u)",what,HRN(hr),e.eng?e.eng->GetCurrentTime():0.0,e.eng?(unsigned)e.eng->GetReadyState():0u); }

// Pause before the next way of opening the clip after the engine reported an error: the failed attempt's file handle and decoder objects
// are released on Media Foundation's own threads, and an immediate retry would race them. (A synchronous rejection needs no pause.)
static const double kRetryBackoffMs=750.0;
// A way of opening the clip that neither fails nor reports LOADEDMETADATA within this time is stuck: the next way is tried, as after an error.
static const double kOpenStallMs=8000.0;
// LOADEDMETADATA arrived but neither LOADEDDATA nor CANPLAY: start the (silent) preload anyway after this long.
static const double kMetaOnlyPlayMs=3000.0;
// Time allowed for the first frame, counted from the moment the clip was handed to the engine.
static const double kFirstFrameBudgetMs=20000.0;

// ------------------------------------------------------------------------------------------------ construction
typedef HRESULT (WINAPI *PFN_CreateDXGIFactory1_t)(REFIID,void**);

// Lists the display adapters in the log (a laptop with two GPUs is the usual reason for "slow on my PC") and, when the host asked for a
// vendor (settings.json "afkGpu"), returns the first hardware adapter of that vendor, AddRef'd. With vendor 0 nothing is returned and Windows
// chooses: it honours the per-app "graphics preference" in Settings and normally picks the GPU that drives the screen.
static IDXGIAdapter1* pickAdapter(UINT vendor){
  HMODULE m=LoadLibraryW(L"dxgi.dll"); if(!m) return nullptr;
  PFN_CreateDXGIFactory1_t cf=(PFN_CreateDXGIFactory1_t)(void*)GetProcAddress(m,"CreateDXGIFactory1"); if(!cf) return nullptr;
  MeCom<IDXGIFactory1> fac; if(FAILED(cf(__uuidof(IDXGIFactory1),(void**)fac.put()))||!fac) return nullptr;
  IDXGIAdapter1* pick=nullptr;
  for(UINT i=0;i<16;i++){
    MeCom<IDXGIAdapter1> a; if(fac->EnumAdapters1(i,a.put())!=S_OK||!a) break;
    DXGI_ADAPTER_DESC1 d; if(FAILED(a->GetDesc1(&d))) continue;
    const bool sw=(d.Flags&DXGI_ADAPTER_FLAG_SOFTWARE)!=0;
    LOG(L"[ME] adapter %u: \"%ls\" (vendor 0x%04X, %u MB video memory)%ls",i,d.Description,d.VendorId,(unsigned)(d.DedicatedVideoMemory>>20),sw?L" [software]":L"");
    if(vendor && !pick && !sw && d.VendorId==vendor){ pick=a.get(); pick->AddRef(); } }
  return pick; }

static HRESULT createDevice(Engine& e){
  HMODULE m=LoadLibraryW(L"d3d11.dll"); if(!m) return HRESULT_FROM_WIN32(GetLastError());
  PFN_D3D11_CREATE_DEVICE fn=(PFN_D3D11_CREATE_DEVICE)GetProcAddress(m,"D3D11CreateDevice"); if(!fn) return E_NOINTERFACE;
  // BGRA for Direct2D, VIDEO for the GPU video processor / DXVA decoders the Media Engine uses.
  UINT flags=D3D11_CREATE_DEVICE_BGRA_SUPPORT|D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
  D3D_FEATURE_LEVEL fls[]={D3D_FEATURE_LEVEL_11_1,D3D_FEATURE_LEVEL_11_0,D3D_FEATURE_LEVEL_10_1,D3D_FEATURE_LEVEL_10_0}, got=D3D_FEATURE_LEVEL_10_0;
  auto make=[&](IDXGIAdapter* adapter)->HRESULT{
    const D3D_DRIVER_TYPE dt=adapter?D3D_DRIVER_TYPE_UNKNOWN:D3D_DRIVER_TYPE_HARDWARE;          // an explicit adapter requires the UNKNOWN driver type
    HRESULT h=fn(adapter,dt,nullptr,flags,fls,4,D3D11_SDK_VERSION,e.dev.put(),&got,e.ctx.put());
    if(h==E_INVALIDARG) h=fn(adapter,dt,nullptr,flags,fls+1,3,D3D11_SDK_VERSION,e.dev.put(),&got,e.ctx.put());   // runtime without 11.1
    if(SUCCEEDED(h)&&(!e.dev||!e.ctx)) h=E_FAIL;
    return h; };
  MeCom<IDXGIAdapter1> want; want.p=pickAdapter(e.cfg.preferredVendor);
  HRESULT hr=E_FAIL;
  if(want){ hr=make(want.get());
    if(FAILED(hr)){ LOG(L"[ME] D3D11CreateDevice on the requested adapter failed, hr=0x%08X %ls -- using the default adapter",HRN(hr)); e.dev.reset(); e.ctx.reset(); } }
  else if(e.cfg.preferredVendor) LOG(L"[ME] no hardware adapter with vendor id 0x%04X was found -- using the default adapter",(unsigned)e.cfg.preferredVendor);
  if(FAILED(hr)){ hr=make(nullptr);
    if(FAILED(hr)){ LOG(L"[ME] D3D11CreateDevice failed, hr=0x%08X %ls",HRN(hr)); return hr; } }
  // The Media Engine's decoder threads share this device: the immediate context must be thread-safe.
  MeCom<ID3D10Multithread> mt; if(SUCCEEDED(e.dev->QueryInterface(__uuidof(ID3D10Multithread),(void**)mt.put()))&&mt) mt->SetMultithreadProtected(TRUE);
  MeCom<IDXGIDevice> dxgi; MeCom<IDXGIAdapter> ad;
  if(SUCCEEDED(e.dev->QueryInterface(__uuidof(IDXGIDevice),(void**)dxgi.put()))&&dxgi){
    if(SUCCEEDED(dxgi->GetAdapter(ad.put()))&&ad){ DXGI_ADAPTER_DESC d; if(SUCCEEDED(ad->GetDesc(&d))){ lstrcpynW(e.adapter,d.Description,96); e.vendor=d.VendorId; e.deviceId=d.DeviceId; } }
    // Two frames may be queued: the queue stays short, but one frame of slack absorbs a decoder / GPU-clock hiccup without a visible stutter.
    MeCom<IDXGIDevice1> d1; if(SUCCEEDED(dxgi->QueryInterface(__uuidof(IDXGIDevice1),(void**)d1.put()))&&d1) d1->SetMaximumFrameLatency(2);
  }
  LOG(L"[ME] D3D11 device on \"%ls\" (vendor 0x%04X, feature level 0x%X)",e.adapter,e.vendor,(unsigned)got);
  HMODULE mf=GetModuleHandleW(L"mfplat.dll"); if(!mf) mf=LoadLibraryW(L"mfplat.dll");
  PFN_MFCreateDXGIDeviceManager_t mk=mf?(PFN_MFCreateDXGIDeviceManager_t)GetProcAddress(mf,"MFCreateDXGIDeviceManager"):nullptr;
  if(!mk){ LOG(L"[ME] MFCreateDXGIDeviceManager not found in mfplat.dll"); return E_NOINTERFACE; }
  hr=mk(&e.resetToken,e.mgr.put()); if(FAILED(hr)||!e.mgr){ LOG(L"[ME] MFCreateDXGIDeviceManager failed, hr=0x%08X %ls",HRN(hr)); return FAILED(hr)?hr:E_FAIL; }
  hr=e.mgr->ResetDevice(e.dev.get(),e.resetToken); if(FAILED(hr)){ LOG(L"[ME] IMFDXGIDeviceManager::ResetDevice failed, hr=0x%08X %ls",HRN(hr)); return hr; }
  return S_OK;
}

static HRESULT createSwapChain(Engine& e){
  MeCom<IDXGIDevice> dxgi; MeCom<IDXGIAdapter> ad; MeCom<IDXGIFactory2> fac;
  HRESULT hr=e.dev->QueryInterface(__uuidof(IDXGIDevice),(void**)dxgi.put()); if(FAILED(hr)) return hr;
  hr=dxgi->GetAdapter(ad.put()); if(FAILED(hr)) return hr;
  hr=ad->GetParent(__uuidof(IDXGIFactory2),(void**)fac.put()); if(FAILED(hr)||!fac){ LOG(L"[ME] IDXGIFactory2 unavailable, hr=0x%08X %ls",HRN(hr)); return FAILED(hr)?hr:E_NOINTERFACE; }
  struct Try{ DXGI_SWAP_EFFECT eff; UINT bufs; UINT code; const wchar_t* name; };
  const Try tries[]={ {DXGI_SWAP_EFFECT_FLIP_DISCARD,3,1,L"flip-discard"}, {DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL,3,2,L"flip-sequential"}, {DXGI_SWAP_EFFECT_DISCARD,1,0,L"bit-blit"} };
  hr=E_FAIL;
  for(const Try& t:tries){
    DXGI_SWAP_CHAIN_DESC1 d={}; d.Width=e.bw; d.Height=e.bh; d.Format=DXGI_FORMAT_B8G8R8A8_UNORM; d.SampleDesc.Count=1;
    d.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT; d.BufferCount=t.bufs; d.SwapEffect=t.eff; d.AlphaMode=DXGI_ALPHA_MODE_IGNORE; d.Scaling=DXGI_SCALING_STRETCH;
    hr=fac->CreateSwapChainForHwnd(e.dev.get(),e.hwnd,&d,nullptr,nullptr,e.sc.put());
    if(SUCCEEDED(hr)&&e.sc){ e.swapEffect=t.code; LOG(L"[ME] swap chain %ux%u, %ls model",e.bw,e.bh,t.name); break; }
    LOG(L"[ME] CreateSwapChainForHwnd (%ls) failed, hr=0x%08X %ls",t.name,HRN(hr)); e.sc.reset(); }
  if(FAILED(hr)||!e.sc) return FAILED(hr)?hr:E_FAIL;
  fac->MakeWindowAssociation(e.hwnd,DXGI_MWA_NO_ALT_ENTER|DXGI_MWA_NO_WINDOW_CHANGES);   // no Alt+Enter exclusive fullscreen, no DXGI window hooks
  return S_OK;
}

static HRESULT createVideoTexture(Engine& e){
  e.vtex.reset();
  D3D11_TEXTURE2D_DESC d={}; d.Width=e.bw; d.Height=e.bh; d.MipLevels=1; d.ArraySize=1; d.Format=DXGI_FORMAT_B8G8R8A8_UNORM;
  d.SampleDesc.Count=1; d.Usage=D3D11_USAGE_DEFAULT; d.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
  HRESULT hr=e.dev->CreateTexture2D(&d,nullptr,e.vtex.put());
  if(FAILED(hr)||!e.vtex){ LOG(L"[ME] CreateTexture2D(%ux%u) failed, hr=0x%08X %ls",e.bw,e.bh,HRN(hr)); return FAILED(hr)?hr:E_FAIL; }
  return S_OK;
}

static void applyAudio(Engine& e){
  if(!e.eng) return;
  bool kick=e.started && !e.haveFrame;                        // the preload play is always silent
  int mute=(e.muted||kick)?1:0;
  if(e.appliedVol!=e.volume){ e.eng->SetVolume((double)e.volume); e.appliedVol=e.volume; }
  if(e.appliedMute!=mute){ e.eng->SetMuted(mute?TRUE:FALSE); e.appliedMute=mute; }
}

static HRESULT createEngine(Engine& e){
  MeCom<IMFMediaEngineClassFactory> fac;
  HRESULT hr=CoCreateInstance(kCLSID_MFMediaEngineClassFactory,nullptr,CLSCTX_INPROC_SERVER,__uuidof(IMFMediaEngineClassFactory),(void**)fac.put());
  if(hr==CO_E_NOTINITIALIZED){ if(SUCCEEDED(CoInitializeEx(nullptr,COINIT_MULTITHREADED))) e.comOwned=true;
    hr=CoCreateInstance(kCLSID_MFMediaEngineClassFactory,nullptr,CLSCTX_INPROC_SERVER,__uuidof(IMFMediaEngineClassFactory),(void**)fac.put()); }
  if(FAILED(hr)||!fac){ LOG(L"[ME] CoCreateInstance(MFMediaEngineClassFactory) failed, hr=0x%08X %ls",HRN(hr)); return FAILED(hr)?hr:E_NOINTERFACE; }
  MeCom<IMFAttributes> at; hr=MFCreateAttributes(at.put(),4); if(FAILED(hr)) return hr;
  e.notify=new MeNotify(&e);
  at->SetUnknown(kMF_MEDIA_ENGINE_CALLBACK,static_cast<IMFMediaEngineNotify*>(e.notify));
  at->SetUnknown(kMF_MEDIA_ENGINE_DXGI_MANAGER,e.mgr.get());                       // hardware decode + frame-server output on our device
  at->SetUINT32(kMF_MEDIA_ENGINE_VIDEO_OUTPUT_FORMAT,(UINT32)DXGI_FORMAT_B8G8R8A8_UNORM);
  hr=fac->CreateInstance(0,at.get(),e.eng.put());
  if(FAILED(hr)||!e.eng){ LOG(L"[ME] IMFMediaEngineClassFactory::CreateInstance failed, hr=0x%08X %ls",HRN(hr)); return FAILED(hr)?hr:E_FAIL; }
  hr=e.eng->QueryInterface(__uuidof(IMFMediaEngineEx),(void**)e.engx.put());
  if(FAILED(hr)||!e.engx){ LOG(L"[ME] IMFMediaEngineEx unavailable, hr=0x%08X %ls",HRN(hr)); return FAILED(hr)?hr:E_NOINTERFACE; }
  e.eng->SetPreload(MF_MEDIA_ENGINE_PRELOAD_AUTOMATIC);
  e.eng->SetLoop(e.cfg.loop?TRUE:FALSE);
  e.appliedVol=-1.f; e.appliedMute=-1; applyAudio(e);
  e.evMeta=e.evData=e.evCanPlay=e.evFirst=e.evPlaying=e.evEnded=e.evError=e.evResLost=false;
  return S_OK;
}

static void destroyEngineObject(Engine& e){
  if(e.notify) e.notify->detach();
  if(e.eng) e.eng->Shutdown();
  e.engx.reset(); e.eng.reset();
  if(e.notify){ e.notify->Release(); e.notify=nullptr; }
}

// Three ways to hand the file to the engine, tried in order if the previous one is rejected: an IMFByteStream
// (immune to odd characters in the path), the plain path, a percent-encoded file:// URL.
static HRESULT openSource(Engine& e){
  HRESULT hr=E_FAIL; BSTR b=nullptr;
  if(e.srcAttempt==0){
    MeCom<IMFByteStream> bs;
    hr=MFCreateFile(MF_ACCESSMODE_READ,MF_OPENMODE_FAIL_IF_NOT_EXIST,MF_FILEFLAGS_NONE,e.path.c_str(),bs.put());
    if(FAILED(hr)||!bs){ LOG(L"[ME] MFCreateFile failed, hr=0x%08X %ls",HRN(hr)); return FAILED(hr)?hr:E_FAIL; }
    b=SysAllocString(me_fileUrl(e.path).c_str());
    hr=e.engx->SetSourceFromByteStream(bs.get(),b); SysFreeString(b); lstrcpynW(e.srcName,L"byte stream",24); }
  else if(e.srcAttempt==1){ b=SysAllocString(e.path.c_str()); hr=e.eng->SetSource(b); SysFreeString(b); lstrcpynW(e.srcName,L"path",24); }
  else { b=SysAllocString(me_fileUrl(e.path).c_str()); hr=e.eng->SetSource(b); SysFreeString(b); lstrcpynW(e.srcName,L"url",24); }
  LOG(L"[ME] source attempt %d (%ls): %ls -> hr=0x%08X",e.srcAttempt,e.srcName,e.path.c_str(),(unsigned)hr);
  return hr;
}

// ------------------------------------------------------------------------------------------------ teardown
static void teardown(Engine* e){
  if(!e) return;
  if(e->overlayProc){ MeOverlayArgs a={sizeof(a),ME_OVERLAY_RELEASE,nullptr,nullptr,0,0,0}; e->overlayProc(e->overlayUser,&a); }
  destroyEngineObject(*e);
  e->vtex.reset();
  if(e->ctx){ e->ctx->ClearState(); e->ctx->Flush(); }
  e->sc.reset(); e->mgr.reset(); e->ctx.reset(); e->dev.reset();
  if(e->mfStarted) MFShutdown();
  if(e->comOwned) CoUninitialize();
  delete e;
}

// ------------------------------------------------------------------------------------------------ per-frame work
static HRESULT transferFrame(Engine& e){
  if(!e.eng||!e.vtex) return E_UNEXPECTED;
  RECT dst={0,0,(LONG)e.bw,(LONG)e.bh}; MFARGB border={0,0,0,255};
  MFVideoNormalizedRect src={0,0,1,1}; bool crop=false;
  if(!e.noCrop && e.vw && e.vh){ float c[4]; me_coverSrcNorm(e.vw,e.vh,e.bw,e.bh,c); src.left=c[0]; src.top=c[1]; src.right=c[2]; src.bottom=c[3];
    crop=(c[0]>0||c[1]>0||c[2]<1||c[3]<1); }
  HRESULT hr=e.eng->TransferVideoFrame(e.vtex.get(),crop?&src:nullptr,&dst,&border);
  if(FAILED(hr)&&crop){ if(!e.cropLogged){ e.cropLogged=true; LOG(L"[ME] TransferVideoFrame with cover-crop failed (hr=0x%08X %ls) -- using the whole frame",HRN(hr)); }
    e.noCrop=true; hr=e.eng->TransferVideoFrame(e.vtex.get(),nullptr,&dst,&border); }
  return hr;
}

static void markLost(Engine& e,HRESULT hr){
  HRESULT why=e.dev?e.dev->GetDeviceRemovedReason():hr;
  e.lastError=hr; e.status=ME_STATUS_DEVICE_LOST;
  LOG(L"[ME] GPU device lost: present hr=0x%08X %ls, removed reason=0x%08X %ls",HRN(hr),HRN(why)); }

// Gap between two NEW video frames. The loop point is expected to cost one per pass; anything else means the decoder or the GPU fell behind. The first
// few long gaps are logged; the longest one since the last MediaEngine_GetStats call goes into the stats line.
static void noteFrame(Engine& e,double now){
  if(e.lastNewAt>0.0){
    const double gap=now-e.lastNewAt; if(gap>e.maxGapMs) e.maxGapMs=gap;
    if(gap>120.0){ e.stalls++; if(e.stalls<=25) LOG(L"[ME] video stalled for %.0f ms (clip time %.2f of %.2f s)",gap,e.eng?e.eng->GetCurrentTime():0.0,e.duration); } }
  e.lastNewAt=now; }

// First-frame capture, source retries, loop fallback. Cheap; called from Pump() and at the top of every render.
static void pumpInternal(Engine& e){
  if(e.status==ME_STATUS_ERROR||e.status==ME_STATUS_DEVICE_LOST||!e.eng) return;
  // A sleep/resume or a driver reset is noticed here even while nothing is being presented (the screen is hidden).
  if(e.dev){ HRESULT rr=e.dev->GetDeviceRemovedReason(); if(FAILED(rr)){ markLost(e,rr); return; } }
  double now=nowMs();
  if(e.evMeta.load() && e.metaSeenAt==0.0) e.metaSeenAt=now;
  if(e.evError.exchange(false)){
    int code=e.errCode.load(); HRESULT hr=(HRESULT)e.errHr.load();
    LOG(L"[ME] media engine error %d (hr=0x%08X %ls) while %ls (source attempt %d)",code,HRN(hr),
        e.haveFrame?L"playing":(e.started?L"starting playback":L"opening the clip"),e.srcAttempt);
    e.lastError=hr?hr:E_FAIL;
    if(e.haveFrame){
      // It was playing, then the engine broke (decoder reset, audio endpoint change, a GPU hiccup under it). That says nothing against the clip or
      // the tier, so it is reported like a lost device: the host rebuilds the player (a bounded number of times) instead of giving the tier up for good.
      e.status=ME_STATUS_DEVICE_LOST;
      LOG(L"[ME] error after playback had started -- reported as a lost device so that the host rebuilds the player"); return; }
    if(!e.started && e.srcAttempt<2){ e.srcAttempt++; e.retry=true; e.retryAt=now+kRetryBackoffMs; }
    else { e.status=ME_STATUS_ERROR; return; } }
  // The open neither failed nor made any progress (not even LOADEDMETADATA): that way of opening the clip is stuck. Next way, as after an error.
  if(!e.retry && !e.started && !e.haveFrame && e.openedAt>0.0 && !e.evMeta.load() && !e.evData.load() && now-e.openedAt>kOpenStallMs){
    LOG(L"[ME] source attempt %d (%ls) shows no sign of life after %.0f s (no LOADEDMETADATA)",e.srcAttempt,e.srcName,(now-e.openedAt)/1000.0);
    e.openedAt=0.0;
    if(e.srcAttempt<2){ e.srcAttempt++; e.retry=true; e.retryAt=now+kRetryBackoffMs; }
    else { e.lastError=HRESULT_FROM_WIN32(ERROR_TIMEOUT); e.status=ME_STATUS_ERROR; return; } }
  if(e.retry && now>=e.retryAt){
    e.retry=false; LOG(L"[ME] retrying with the next way of opening the clip");
    destroyEngineObject(e);
    e.metaSeenAt=0.0; e.openedAt=0.0;
    HRESULT hr=createEngine(e); if(SUCCEEDED(hr)) hr=openSource(e);
    if(FAILED(hr)){
      e.lastError=hr;
      if(e.eng && e.srcAttempt<2){ e.srcAttempt++; e.retry=true; e.retryAt=now; LOG(L"[ME] that way was rejected at once -- will try the next"); }   // a synchronous "no": next way, no pause needed
      else e.status=ME_STATUS_ERROR;                                                                                                              // no engine could be created, or nothing is left to try
      return; }
    e.openedAt=nowMs();
    return; }
  if(!e.eng) return;
  // LOADEDDATA is the usual signal, CANPLAY / FIRSTFRAMEREADY are as good; and if only the metadata ever arrives, play anyway after a short wait
  // (a silent preload costs nothing, and waiting for an event that never comes costs the whole tier).
  const bool dataSignal=e.evData.load()||e.evCanPlay.load()||e.evFirst.load();
  const bool metaOnly=!dataSignal && e.evMeta.load() && e.metaSeenAt>0.0 && now-e.metaSeenAt>kMetaOnlyPlayMs;
  if((dataSignal||metaOnly) && !e.started){
    e.started=true; e.startedAt=now;
    e.eng->GetNativeVideoSize(&e.vw,&e.vh); e.duration=e.eng->GetDuration();
    LOG(L"[ME] clip loaded after %.0f ms%ls: %ux%u, %.1f s -- starting a silent preload to capture the first frame",now-e.t0,
        metaOnly?L" (only the metadata so far; starting anyway)":(e.evData.load()?L"":L" (CANPLAY / FIRSTFRAMEREADY, no LOADEDDATA)"),(unsigned)e.vw,(unsigned)e.vh,e.duration);
    applyAudio(e); notePlay(e,L"Play (silent preload)",e.eng->Play()); e.playIssued=true; e.playStartMs=now; e.playStartMediaT=0; e.ticksSincePlay=0; }
  if(e.started && e.evEnded.exchange(false)){      // SetLoop should make this unreachable; belt and braces
    e.eng->SetCurrentTime(0.0); if(e.playIssued) notePlay(e,L"Play (restart after ENDED)",e.eng->Play()); }
  if(e.started && !e.haveFrame){
    LONGLONG pts=0; HRESULT th=e.eng->OnVideoStreamTick(&pts); e.lastTickHr=th;
    bool blindStart=(now-e.startedAt>1500.0) && e.eng->GetCurrentTime()>0.2;      // frames are flowing but never signalled
    if(th==S_OK||blindStart){
      HRESULT hr=transferFrame(e);
      if(SUCCEEDED(hr)){
        e.haveFrame=true; e.firstFrameAt=now; e.status=ME_STATUS_READY;
        if(th==S_OK) e.ticksSincePlay++;
        LOG(L"[ME] first frame captured %.0f ms after Init%ls",now-e.t0,th==S_OK?L"":L" (blind: OnVideoStreamTick had not signalled)");
        applyAudio(e);
        if(!e.wantPlay
#ifdef SW_TEST_HOOKS
           || e.testStuck                // (a player that ignores the request to play stays paused even when the screen asked before its first frame)
#endif
          ){ notePlay(e,L"Pause (first frame captured, parked)",e.eng->Pause()); e.playIssued=false; } }
      else if((e.transferFails++%50)==0) LOG(L"[ME] TransferVideoFrame (first frame) failed, hr=0x%08X %ls",HRN(hr)); } }
  const double budgetT0=e.loadT0>0.0?e.loadT0:e.t0;          // counted from the hand-over of the clip, not from Init
  if(!e.haveFrame && now-budgetT0>kFirstFrameBudgetMs){ e.lastError=HRESULT_FROM_WIN32(ERROR_TIMEOUT); e.status=ME_STATUS_ERROR;
    LOG(L"[ME] no first frame %.0f s after the clip was handed over (events: metadata=%d data=%d canplay=%d firstframe=%d playing=%d stream-rendering-errors=%d; source attempt %d (%ls), "
        L"started=%d, last OnVideoStreamTick hr=0x%08X, transfer failures %lu) -- giving up on this clip",
        (now-budgetT0)/1000.0,(int)e.evMeta.load(),(int)e.evData.load(),(int)e.evCanPlay.load(),(int)e.evFirst.load(),(int)e.evPlaying.load(),e.evRenderErr.load(),
        e.srcAttempt,e.srcName,(int)e.started,(unsigned)e.lastTickHr,e.transferFails); }
}

// ------------------------------------------------------------------------------------------------ public API
extern "C" {

ME_API void MediaEngine_SetLogger(MeLogProc proc){ g_logProc.store(proc); }

ME_API HRESULT MediaEngine_Init(HWND hwnd,const wchar_t* videoPath){ return MediaEngine_InitEx(hwnd,videoPath,nullptr); }

ME_API HRESULT MediaEngine_InitEx(HWND hwnd,const wchar_t* videoPath,const MeConfig* cfg){
  if(g_e) MediaEngine_Shutdown();
  if(!hwnd||!videoPath||!*videoPath) return E_INVALIDARG;
  Engine* e=new Engine(); g_e=e;
  e->hwnd=hwnd; e->path=videoPath; e->t0=nowMs();
  e->cfg.structSize=sizeof(MeConfig); e->cfg.loop=TRUE; e->cfg.muted=TRUE; e->cfg.volume=1.0f; e->cfg.syncInterval=1; e->cfg.clearColor=0xFF060810u;
  if(cfg && cfg->structSize>=sizeof(UINT32)){ MeConfig c=e->cfg; size_t n=cfg->structSize<sizeof(MeConfig)?cfg->structSize:sizeof(MeConfig); memcpy(&c,cfg,n); e->cfg=c; e->cfg.structSize=sizeof(MeConfig); }
  e->muted=e->cfg.muted?true:false; e->volume=e->cfg.volume<0?0:(e->cfg.volume>1?1:e->cfg.volume);
  e->sync=e->cfg.syncInterval?e->cfg.syncInterval:1; if(e->sync>4)e->sync=4;
#ifdef SW_TEST_HOOKS
  { static int left=-1;                                      // SW_TEST_STUCK=<n>: the first n players never start playing when asked (see MediaEngine_SetPlaybackState)
    if(left<0){ wchar_t b[16]; left=GetEnvironmentVariableW(L"SW_TEST_STUCK",b,16)?_wtoi(b):0; }
    if(left>0){ left--; e->testStuck=true; LOG(L"[ME] (test) this player will ignore the request to play"); } }
#endif
  RECT rc={0,0,0,0}; GetClientRect(hwnd,&rc);
  e->bw=e->cfg.width?e->cfg.width:(UINT)(rc.right-rc.left); e->bh=e->cfg.height?e->cfg.height:(UINT)(rc.bottom-rc.top);
  if(!e->bw||!e->bh){ e->bw=1280; e->bh=720; }
  LOG(L"[ME] init: window %p, buffers %ux%u, sync interval %u, clip %ls",(void*)hwnd,e->bw,e->bh,e->sync,videoPath);

  HRESULT hr=MFStartup(MF_VERSION,MFSTARTUP_FULL);
  if(FAILED(hr)){ LOG(L"[ME] MFStartup failed, hr=0x%08X %ls",HRN(hr)); teardown(e); g_e=nullptr; return hr; }
  e->mfStarted=true;
  hr=createDevice(*e);       if(FAILED(hr)){ teardown(e); g_e=nullptr; return hr; }
  hr=createSwapChain(*e);    if(FAILED(hr)){ teardown(e); g_e=nullptr; return hr; }
  hr=createVideoTexture(*e); if(FAILED(hr)){ teardown(e); g_e=nullptr; return hr; }
  hr=createEngine(*e);       if(FAILED(hr)){ teardown(e); g_e=nullptr; return hr; }
  hr=openSource(*e);
  e->loadT0=nowMs(); e->openedAt=e->loadT0;                   // the first-frame budget and the stall check start here, after the slow part (device, driver) is over
  if(FAILED(hr)){ e->retry=true; e->retryAt=nowMs(); e->srcAttempt++; e->openedAt=0.0; LOG(L"[ME] first source attempt rejected at once -- will try the next"); }
  e->status=ME_STATUS_LOADING;
  return S_OK;
}

ME_API void MediaEngine_Shutdown(void){
  Engine* e=g_e; if(!e) return; g_e=nullptr;
  LOG(L"[ME] shutdown (%lu video frames delivered, %lu presents)",e->delivered,e->presents);
  teardown(e);
}

ME_API HRESULT MediaEngine_Pump(void){
  Engine* e=g_e; if(!e) return E_UNEXPECTED;
  pumpInternal(*e);
  if(e->status==ME_STATUS_DEVICE_LOST) return DXGI_ERROR_DEVICE_REMOVED;
  if(e->status==ME_STATUS_ERROR) return FAILED(e->lastError)?e->lastError:E_FAIL;
  return e->status==ME_STATUS_READY?S_OK:S_FALSE;
}

ME_API HRESULT MediaEngine_Resize(UINT32 w,UINT32 h){
  Engine* e=g_e; if(!e) return E_UNEXPECTED;
  if(!w||!h) return E_INVALIDARG;
  if(w==e->bw&&h==e->bh) return S_OK;
  if(e->overlayProc){ MeOverlayArgs a={sizeof(a),ME_OVERLAY_RELEASE,nullptr,nullptr,0,0,0}; e->overlayProc(e->overlayUser,&a); }   // drop D2D targets on the old buffers
  e->vtex.reset(); if(e->ctx){ e->ctx->ClearState(); e->ctx->Flush(); }
  HRESULT hr=e->sc->ResizeBuffers(0,w,h,DXGI_FORMAT_UNKNOWN,0);
  if(FAILED(hr)){ LOG(L"[ME] ResizeBuffers(%ux%u) failed, hr=0x%08X %ls",w,h,HRN(hr));
    if(hr==DXGI_ERROR_DEVICE_REMOVED||hr==DXGI_ERROR_DEVICE_RESET) markLost(*e,hr); else { e->lastError=hr; e->status=ME_STATUS_ERROR; }
    return hr; }
  e->bw=w; e->bh=h;
  hr=createVideoTexture(*e); if(FAILED(hr)){ e->lastError=hr; e->status=ME_STATUS_ERROR; return hr; }
  e->forceTransfer=e->haveFrame;      // redraw the texture from the engine's current frame on the next render
  LOG(L"[ME] resized to %ux%u",w,h);
  return S_OK;
}

ME_API HRESULT MediaEngine_RenderFrame(void){ return MediaEngine_RenderFrameEx(nullptr,nullptr,nullptr); }

ME_API HRESULT MediaEngine_RenderFrameEx(MeOverlayProc overlay,void* user,UINT32* flagsOut){
  if(flagsOut) *flagsOut=0;
  Engine* e=g_e; if(!e) return E_UNEXPECTED;
  if(overlay){ e->overlayProc=overlay; e->overlayUser=user; }
  if(e->status==ME_STATUS_DEVICE_LOST) return DXGI_ERROR_DEVICE_REMOVED;
  if(!e->sc||!e->vtex) return E_UNEXPECTED;
  pumpInternal(*e);
  // pumpInternal may just have noticed that the player is lost or failed (and may have torn the engine down to retry): do not draw on that state
  if(e->status==ME_STATUS_DEVICE_LOST) return DXGI_ERROR_DEVICE_REMOVED;
  if(e->status==ME_STATUS_ERROR) return FAILED(e->lastError)?e->lastError:E_FAIL;
  UINT32 flags=0; double t0=nowMs();

  // 1. a new decoded video frame -> vtex
  bool fresh=false, tick=false;
  if(e->haveFrame && e->eng){
    if(e->playIssued){
      if(e->blind){ unsigned long add=e->blindCtr.advance(e->eng->GetCurrentTime(),e->cfg.clipFps); if(add){ e->delivered+=add; fresh=true; tick=true; } }
      else {
        LONGLONG pts=0; HRESULT th=e->eng->OnVideoStreamTick(&pts); e->lastTickHr=th;
        if(th==S_OK){ fresh=true; tick=true; e->delivered++; e->ticksSincePlay++; }
        else if(e->ticksSincePlay==0 && t0-e->playStartMs>2000.0){
          double ct=e->eng->GetCurrentTime();
          if(ct>e->playStartMediaT+0.5){ e->blind=true; e->blindCtr=MeBlindCounter(); e->blindCtr.advance(ct,e->cfg.clipFps);
            LOG(L"[ME] OnVideoStreamTick never signalled but the clip is advancing (%.1f s) -- switching to blind mode (frames derived from the media clock)",ct); } } } }
    if(e->forceTransfer){ fresh=true; e->forceTransfer=false; }
    if(fresh){ HRESULT hr=transferFrame(*e);
      if(FAILED(hr)){ if((e->transferFails++%100)==0) LOG(L"[ME] TransferVideoFrame failed, hr=0x%08X %ls",HRN(hr)); fresh=false; }
      else{ flags|=ME_FRAME_NEW_VIDEO; if(tick) noteFrame(*e,t0); } } }
  double t1=nowMs();

  // 2. back buffer: video texture (or the clear colour until the first frame exists)
  MeCom<ID3D11Texture2D> bb; HRESULT hr=e->sc->GetBuffer(0,__uuidof(ID3D11Texture2D),(void**)bb.put());
  if(FAILED(hr)||!bb){ if(hr==DXGI_ERROR_DEVICE_REMOVED||hr==DXGI_ERROR_DEVICE_RESET) markLost(*e,hr);
    else if((e->drawFails++%60)==0) LOG(L"[ME] GetBuffer failed, hr=0x%08X %ls (failure %lu)",HRN(hr),e->drawFails);       // every 60th: a failing display must not flood the log
    return FAILED(hr)?hr:E_FAIL; }
  if(e->haveFrame){ e->ctx->CopyResource(bb.get(),e->vtex.get()); flags|=ME_FRAME_HAVE_VIDEO; }
  else { MeCom<ID3D11RenderTargetView> rtv;
    if(SUCCEEDED(e->dev->CreateRenderTargetView(bb.get(),nullptr,rtv.put()))&&rtv){
      UINT32 c=e->cfg.clearColor; float col[4]={((c>>16)&255)/255.f,((c>>8)&255)/255.f,(c&255)/255.f,1.f}; e->ctx->ClearRenderTargetView(rtv.get(),col); } }

  // 3. host overlay (Direct2D HUD)
  if(overlay){ MeOverlayArgs a={sizeof(a),ME_OVERLAY_DRAW,e->dev.get(),bb.get(),e->bw,e->bh,flags}; overlay(user,&a); }
  bb.reset();
  double t2=nowMs();

  // 4. present
  hr=e->sc->Present(e->sync,0);
  double t3=nowMs();
  e->avgTransfer=me_ema(e->avgTransfer,t1-t0); e->avgOverlay=me_ema(e->avgOverlay,t2-t1); e->avgPresent=me_ema(e->avgPresent,t3-t2);
  if(flagsOut) *flagsOut=flags;
  if(hr==S_OK){ e->presents++; if(flagsOut) *flagsOut|=ME_FRAME_PRESENTED; return S_OK; }
  // Nothing reached the screen: window covered / display off, the session is on another desktop (lock screen, secure desktop), a display mode switch is under way.
  // The host sleeps instead of spinning on a Present() that returns at once, and pauses playback if it lasts.
  if(hr==DXGI_STATUS_OCCLUDED||hr==DXGI_STATUS_NO_DESKTOP_ACCESS||hr==DXGI_STATUS_MODE_CHANGE_IN_PROGRESS){ if(flagsOut) *flagsOut|=ME_FRAME_OCCLUDED; return S_FALSE; }
  if(hr==DXGI_ERROR_DEVICE_REMOVED||hr==DXGI_ERROR_DEVICE_RESET||hr==DXGI_ERROR_DEVICE_HUNG||hr==DXGI_ERROR_DRIVER_INTERNAL_ERROR){ markLost(*e,hr); return hr; }
  if(SUCCEEDED(hr)) return S_OK;
  e->lastError=hr; if((e->drawFails++%60)==0) LOG(L"[ME] Present failed, hr=0x%08X %ls (failure %lu)",HRN(hr),e->drawFails); return hr;
}

ME_API void MediaEngine_SetPlaybackState(int state){
  Engine* e=g_e; if(!e||!e->eng) return;
  if(state==ME_STATE_PLAYING) e->wantPlay=true;
  else { e->wantPlay=false; }
#ifdef SW_TEST_HOOKS
  if(e->testStuck && state==ME_STATE_PLAYING){ LOG(L"[ME] (test) the request to play is ignored"); return; }
#endif
  if(e->started && e->haveFrame){
    if(e->wantPlay && !e->playIssued){ notePlay(*e,L"Play (the screen wants the video)",e->eng->Play()); e->playIssued=true; e->playStartMs=nowMs(); e->ticksSincePlay=0; e->playStartMediaT=e->eng->GetCurrentTime(); e->blind=false; e->lastNewAt=0; }
    else if(!e->wantPlay && e->playIssued){ notePlay(*e,L"Pause (the screen is over)",e->eng->Pause()); e->playIssued=false; e->lastNewAt=0; }
    if(state==ME_STATE_STOPPED) e->eng->SetCurrentTime(0.0); }
  applyAudio(*e);
}

ME_API void MediaEngine_SetVolume(float volume,BOOL muted){
  Engine* e=g_e; if(!e) return;
  e->volume=volume<0.f?0.f:(volume>1.f?1.f:volume); e->muted=muted?true:false; applyAudio(*e);
}

ME_API void MediaEngine_SetSyncInterval(UINT32 n){ Engine* e=g_e; if(!e) return; e->sync=n<1?1:(n>4?4:n); }

ME_API BOOL MediaEngine_GetStats(MeStats* out){
  Engine* e=g_e; if(!out||out->structSize<sizeof(UINT32)) return FALSE;
  const UINT32 sz=out->structSize;
  MeStats s; memset(&s,0,sizeof(s)); s.structSize=(UINT32)sizeof(MeStats);
  if(!e) s.status=ME_STATUS_IDLE;
  else{
    s.status=(UINT32)e->status; s.playState=e->wantPlay?ME_STATE_PLAYING:ME_STATE_PAUSED;
    s.videoWidth=e->vw; s.videoHeight=e->vh; s.bufferWidth=e->bw; s.bufferHeight=e->bh; s.durationSec=e->duration;
    s.videoFramesDelivered=e->delivered; s.presents=e->presents; s.lastError=e->lastError;
    s.vendorId=e->vendor; s.deviceId=e->deviceId; s.swapEffect=e->swapEffect; s.blindMode=e->blind?1u:0u;
    s.avgTransferMs=e->avgTransfer; s.avgOverlayMs=e->avgOverlay; s.avgPresentMs=e->avgPresent;
    s.maxFrameGapMs=e->maxGapMs; e->maxGapMs=0.0; s.frameStalls=e->stalls;            // the gap maximum covers the time since the previous call
    lstrcpynW(s.adapter,e->adapter,96); lstrcpynW(s.source,e->srcName,24);
    s.stateFlags=(e->started?ME_SF_STARTED:0u)|(e->haveFrame?ME_SF_HAVE_FRAME:0u)|(e->wantPlay?ME_SF_WANT_PLAY:0u)|(e->playIssued?ME_SF_PLAY_ISSUED:0u)|(e->muted?ME_SF_MUTED:0u)|(e->blind?ME_SF_BLIND:0u);
    s.lastPlayHr=e->lastPlayHr; s.lastTickHr=e->lastTickHr; s.renderErrors=(UINT32)e->evRenderErr.load(); s.transferFailures=e->transferFails;
    if(e->eng){ s.clipTimeSec=e->eng->GetCurrentTime(); s.readyState=(UINT32)e->eng->GetReadyState();
      if(e->eng->IsPaused()) s.stateFlags|=ME_SF_PAUSED; if(e->eng->IsEnded()) s.stateFlags|=ME_SF_ENDED; }
    if(e->engx){ PROPVARIANT pv; PropVariantInit(&pv);
      if(SUCCEEDED(e->engx->GetStatistics(MF_MEDIA_ENGINE_STATISTIC_FRAMES_DROPPED,&pv))){
        if(pv.vt==VT_UI8) s.engineFramesDropped=(unsigned long)pv.uhVal.QuadPart; else if(pv.vt==VT_UI4) s.engineFramesDropped=pv.ulVal; }
      PropVariantClear(&pv); } }
  // A caller built against an older (shorter) MeStats gets exactly its own size back, never a write past its struct.
  const size_t n=sz<sizeof(MeStats)?sz:sizeof(MeStats);
  memcpy(out,&s,n); out->structSize=(UINT32)n;
  return TRUE;
}

} // extern "C"
