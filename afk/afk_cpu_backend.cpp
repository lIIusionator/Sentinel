// afk_cpu_backend.cpp -- tier 2, the last resort: software decode (Media Foundation source reader) + GDI+ HUD + BitBlt present.
//
// This is the v13 renderer restructured so the controller can start, stop and replace it at will (the controller owns the
// window and the render thread), plus what was measured on the target laptop (render 9.3 fps: video draw 11.9 ms, vignette
// 11.2 ms, present 2.9 ms per frame, all GDI+ on a starved UI-thread timer):
//   * When the decoded frame is exactly the window size (the built-in 1080p clip on a 1080p screen) the video, the cross-fade
//     between two frames and the vignette are composed in ONE SSE2 pass straight into the back buffer -- no GDI+ video draw,
//     no GDI+ alpha blend of a full-screen bitmap. Any other size takes the v13 GDI+ path.
//   * The quote's word layout is measured once per quote, not eight times per frame.
//   * Frame pacing, loop resync to the audio clock, image-sequence fallback and the HUD layout are the v13 ones.
// Worker threads (decode, audio) only touch `Shared`, which they co-own, so a thread that is slow to stop can never see a
// destroyed backend.
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include "afk_backend.h"
#include "gfx.h"
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <propvarutil.h>
#include <malloc.h>
#include <algorithm>
#include <memory>

namespace {

struct CsLock{ CRITICAL_SECTION cs; CsLock(){ InitializeCriticalSection(&cs); } ~CsLock(){ DeleteCriticalSection(&cs); }
  void lock(){ EnterCriticalSection(&cs); } void unlock(){ LeaveCriticalSection(&cs); } };

// One decoded frame: opaque BGRX, packed rows. (GDI+ wraps the memory for the rare scaled draw.)
struct CpuFrame{
  uint32_t w,h; uint8_t* px;
  CpuFrame(uint32_t W,uint32_t H):w(W),h(H),px(nullptr){ px=(uint8_t*)_aligned_malloc((size_t)W*H*4,32); }
  ~CpuFrame(){ if(px) _aligned_free(px); }
  CpuFrame(const CpuFrame&)=delete; CpuFrame& operator=(const CpuFrame&)=delete;
  UINT GetWidth() const { return w; } UINT GetHeight() const { return h; }
};

// Everything the worker threads use. Owned jointly by the backend and every worker (shared_ptr).
struct Shared{
  std::wstring path; UINT32 targetW=0,targetH=0; double clipFps=30.0;
  std::atomic<bool> stop{false}, run{false};
  std::atomic<int> state{0};                               // 0 opening, 1 first frame published, 2 no playable video (reason in `why`)
  std::atomic<unsigned long long> frames{0}, stReadUs{0}, stCopyUs{0}, stSlips{0};
  std::atomic<float> volume{1.0f}; std::atomic<bool> muted{true};
  std::atomic<int> workers{0};
  std::atomic<int> refs{1};                                // intrusive count: the back-end + every worker thread (std::shared_ptr is not
                                                           // thread-safe in MinGW's "win32" thread model, so it is not used across threads)
  std::wstring why;                                        // written before state becomes 2
  AfkFrameExchange<CpuFrame,CsLock> vx;                    // decoded frames: decode thread -> render thread
  ~Shared(){ vx.shutdown(); }
};

static void shAddRef(Shared* s){ s->refs.fetch_add(1); }
static void shRelease(Shared* s){ if(s->refs.fetch_sub(1)==1) delete s; }
// Sleeps `ms`, but wakes at once when the back-end is told to stop (workers must never delay a shutdown by a whole back-off).
static void sleepStop(Shared& S,DWORD ms){ while(ms&&!S.stop.load()){ const DWORD n=ms<50?ms:50; Sleep(n); ms-=n; } }

// ------------------------------------------------------------------------------------------------ decode configurations
// vp: 1 legacy software video processing, 2 advanced video processor (also resizes + converts in one step). The D3D-manager
// variants of v13 are gone for good: that combination was rejected on the target machine (E_INVALIDARG) and the GPU
// tiers do hardware decoding properly through the Media Engine.
struct DecCfg{ const wchar_t* name; int vp; bool hw; bool scaled; };
static const DecCfg kCfgs[3]={
  {L"hardware-transform hint / legacy video processing / native",1,true,false},   // the v13 default: confirmed to show video on the target laptop
  {L"software / advanced video processor / native",              2,false,false},
  {L"software / advanced video processor / window-size output",  2,false,true },
};
struct DecOpen{ IMFSourceReader* rd=nullptr; UINT32 vw=0,vh=0,nw=0,nh=0; LONG stride=0; double fps=30.0; int64_t durationHns=0; HRESULT hr=S_OK; const wchar_t* step=L""; bool scaledApplied=false; };

static bool probeNative(const std::wstring& path,UINT32& w,UINT32& h){
  IMFSourceReader* rd=nullptr; if(FAILED(MFCreateSourceReaderFromURL(path.c_str(),nullptr,&rd))||!rd) return false;
  IMFMediaType* nat=nullptr; bool ok=false;
  if(SUCCEEDED(rd->GetNativeMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM,0,&nat))&&nat){ ok=SUCCEEDED(MFGetAttributeSize(nat,MF_MT_FRAME_SIZE,&w,&h))&&w&&h; nat->Release(); }
  rd->Release(); return ok; }

// Opens `path` with configuration c and negotiates RGB32 (optionally resized to cover tw x th -- never upscaled).
static bool openCfg(const DecCfg& c,const std::wstring& path,UINT32 tw,UINT32 th,DecOpen& o){
  o=DecOpen(); IMFAttributes* at=nullptr; IMFSourceReader* rd=nullptr; IMFMediaType* mt=nullptr; IMFMediaType* cur=nullptr; bool ok=false;
  do{
    if(FAILED(o.hr=MFCreateAttributes(&at,4))){ o.step=L"MFCreateAttributes"; break; }
    if(c.vp==1) at->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING,TRUE);
    if(c.vp==2) at->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING,TRUE);
    if(c.hw)    at->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS,TRUE);
    if(FAILED(o.hr=MFCreateSourceReaderFromURL(path.c_str(),at,&rd))){ o.step=L"MFCreateSourceReaderFromURL"; break; }
    rd->SetStreamSelection((DWORD)MF_SOURCE_READER_ALL_STREAMS,FALSE);
    rd->SetStreamSelection((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM,TRUE);
    { IMFMediaType* nat=nullptr;
      if(SUCCEEDED(rd->GetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM,&nat))&&nat){
        MFGetAttributeSize(nat,MF_MT_FRAME_SIZE,&o.nw,&o.nh); UINT32 fn=0,fd=0;
        if(SUCCEEDED(MFGetAttributeRatio(nat,MF_MT_FRAME_RATE,&fn,&fd))&&fn>0&&fd>0) o.fps=(double)fn/(double)fd; nat->Release(); } }
    if(!o.nw||!o.nh){ o.hr=E_FAIL; o.step=L"native size"; break; }
    { PROPVARIANT dv; PropVariantInit(&dv);
      if(SUCCEEDED(rd->GetPresentationAttribute((DWORD)MF_SOURCE_READER_MEDIASOURCE,MF_PD_DURATION,&dv))) o.durationHns=dv.vt==VT_UI8?(int64_t)dv.uhVal.QuadPart:0;
      PropVariantClear(&dv); }
    if(FAILED(o.hr=MFCreateMediaType(&mt))){ o.step=L"MFCreateMediaType"; break; }
    mt->SetGUID(MF_MT_MAJOR_TYPE,MFMediaType_Video); mt->SetGUID(MF_MT_SUBTYPE,MFVideoFormat_RGB32);
    if(c.scaled&&tw&&th){
      if(o.nw>tw||o.nh>th){ double sc=(double)tw/o.nw, sc2=(double)th/o.nh; if(sc2>sc) sc=sc2;
        UINT32 dw=((UINT32)(o.nw*sc+0.5))&~1u, dh=((UINT32)(o.nh*sc+0.5))&~1u;
        if(dw>0&&dh>0&&(dw<o.nw||dh<o.nh)){ MFSetAttributeSize(mt,MF_MT_FRAME_SIZE,dw,dh); o.scaledApplied=true; } } }
    if(FAILED(o.hr=rd->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM,NULL,mt))){ o.step=o.scaledApplied?L"SetCurrentMediaType(RGB32+resize)":L"SetCurrentMediaType(RGB32)"; break; }
    if(FAILED(o.hr=rd->GetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM,&cur))){ o.step=L"GetCurrentMediaType"; break; }
    GUID sub=GUID_NULL; cur->GetGUID(MF_MT_SUBTYPE,&sub); MFGetAttributeSize(cur,MF_MT_FRAME_SIZE,&o.vw,&o.vh);
    UINT32 st=0; o.stride=SUCCEEDED(cur->GetUINT32(MF_MT_DEFAULT_STRIDE,&st))?(LONG)st:(LONG)o.vw*4;
    if(!IsEqualGUID(sub,MFVideoFormat_RGB32)||!o.vw||!o.vh){ o.hr=E_FAIL; o.step=L"negotiated type"; break; }
    ok=true;
  }while(false);
  if(cur)cur->Release(); if(mt)mt->Release(); if(at)at->Release();
  if(ok) o.rd=rd; else if(rd) rd->Release();
  return ok;
}

// ------------------------------------------------------------------------------------------------ decode thread
// Copies a picture into a pooled frame. `top` is the first image row, `stride` the (signed) distance to the next one; the caller has already
// checked with afkFrameLayoutOk that every row lies inside the buffer.
static CpuFrame* copyFrame(Shared& S,const BYTE* top,LONG stride,UINT32 vw,UINT32 vh){
  CpuFrame* f=S.vx.obtain(vw,vh,[](uint32_t w,uint32_t h){ return new CpuFrame(w,h); });
  if(!f) return nullptr; if(!f->px){ delete f; return nullptr; }
  afkCopyOpaque32(top,(int32_t)stride,f->px,(int32_t)(vw*4),vw,vh);
  return f;
}

// Takes the picture out of a decoded sample's buffer. A 2-D buffer says where its top row is and how wide a row really is (negative pitch =
// bottom-up); a flat Lock() only has the media type's default stride to go by. Either way the layout is validated against the buffer's length.
static CpuFrame* frameFromBuffer(Shared& S,IMFMediaBuffer* buf,LONG typeStride,UINT32 vw,UINT32 vh){
  DWORD curLen=0; if(FAILED(buf->GetCurrentLength(&curLen))) curLen=0;
  CpuFrame* f=nullptr;
  IMF2DBuffer* b2=nullptr;
  if(SUCCEEDED(buf->QueryInterface(IID_PPV_ARGS(&b2)))&&b2){
    BYTE* s0=nullptr; LONG pitch=0;
    if(SUCCEEDED(b2->Lock2D(&s0,&pitch))&&s0){ if(afkFrameLayoutOk((int64_t)pitch,vw,vh,(uint64_t)curLen)) f=copyFrame(S,s0,pitch,vw,vh); b2->Unlock2D(); }
    b2->Release(); }
  if(f) return f;
  BYTE* d=nullptr; DWORD ml=0,cl=0;
  if(SUCCEEDED(buf->Lock(&d,&ml,&cl))&&d){
    if(afkFrameLayoutOk((int64_t)typeStride,vw,vh,(uint64_t)cl)){
      const BYTE* top=typeStride<0?d+(size_t)(vh-1)*(size_t)(-(int64_t)typeStride):d; f=copyFrame(S,top,typeStride,vw,vh); }
    buf->Unlock(); }
  return f;
}

// Opens the clip, decodes the first frame (the preload), then decodes continuously whenever S.run is set.
// Private-clock pacing (AfkPacer): a late frame is shown at once and the timeline slips, so the picture keeps moving at whatever rate the machine
// sustains. The loop restart at end-of-stream always seeks back to the start of the clip -- a clean picture loop; the sound loops on its own session.
// Nothing in here may spin or die quietly:
//   * a configuration that opens but never yields a usable picture is given up on, and the chain moves on to the next one;
//   * when none is left before a first picture exists, S.state becomes 2 (the back-end then runs with the dark background and the HUD);
//   * when the clip cannot be reopened later (a scanner locks the file, a drive went away) the last picture stays and the worker retries, slowly.
static void videoMain(Shared& S){
  UINT32 nw=0,nh=0; bool oversize=false;
  if(probeNative(S.path,nw,nh)) oversize=S.targetW&&S.targetH&&(nw>S.targetW||nh>S.targetH);
  else logLine(L"[AFKVID] (CPU) could not read the clip's size before opening it");
  const int chainBig[3]={2,0,1}, chainSmall[2]={0,1}; const int* chain=oversize?chainBig:chainSmall; const int nChain=oversize?3:2;
  AfkPacer pacer; bool badLogged=false; int openRounds=0; int64_t openedAt=hnsNow();
  int startK=0;                                            // where in the chain the next open starts
  while(!S.stop.load()){
    // ---- open (again after a persistent failure) ----
    DecOpen o; int used=-1, usedK=-1;
    for(int k=startK;k<nChain&&used<0&&!S.stop.load();k++){
      if(openCfg(kCfgs[chain[k]],S.path,S.targetW,S.targetH,o)){ used=chain[k]; usedK=k; }
      else logLine(L"[AFKVID] (CPU) open failed for config #"+std::to_wstring(chain[k])+L" at "+o.step+L", hr="+hrHex(o.hr)); }
    if(used<0){
      if(S.stop.load()) return;
      startK=0; openRounds++;
      if(S.state.load()!=1){                               // nothing has been shown yet: three rounds and the clip is declared unplayable
        if(openRounds>=3){ S.why=L"no Media Foundation decode configuration could open "+S.path+L" (last hr="+hrHex(o.hr)+L" at "+o.step+L")"; S.state.store(2); return; }
        sleepStop(S,500); continue; }
      const DWORD waitMs=openRounds<3?5000u:(openRounds<6?15000u:30000u);
      logLine(L"[AFKVID] (CPU) the clip cannot be reopened (hr="+hrHex(o.hr)+L" at "+o.step+L") -- the last picture stays on screen; trying again in "+std::to_wstring(waitMs/1000)+L" s");
      sleepStop(S,waitMs); continue; }
    openRounds=0;
    IMFSourceReader* rd=o.rd; const UINT32 vw=o.vw,vh=o.vh; const LONG stride=o.stride; const int64_t durationHns=o.durationHns;
    logLine(L"[AFKVID] (CPU) decoding "+std::to_wstring(vw)+L"x"+std::to_wstring(vh)+L" via config #"+std::to_wstring(used)+L": "+kCfgs[used].name+L", duration "+fmt1(durationHns/1e7)+L" s");

    // ---- decode: the first frame is the preload; after that only while S.run is set ----
    bool wasRun=false; int readFails=0, rejected=0; bool reopen=false; unsigned framesThisOpen=0, sinceSeek=0; pacer.reset();
    while(!S.stop.load()){
      const bool published=S.state.load()==1;
      if(published&&!S.run.load()){ wasRun=false; Sleep(30); continue; }                  // preloaded, waiting for the screen to be shown
      if(S.run.load()&&!wasRun){ wasRun=true; pacer.reset(); }
      DWORD flags=0,sidx=0; LONGLONG ts=0; IMFSample* smp=nullptr; const int64_t r0=hnsNow();
      const HRESULT hr=rd->ReadSample((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM,0,&sidx,&flags,&ts,&smp);
      S.stReadUs+=(unsigned long long)((hnsNow()-r0)/10);
      if(FAILED(hr)||(flags&MF_SOURCE_READERF_ERROR)){
        if(smp) smp->Release();
        readFails++; if(readFails==1) logLine(L"[AFKVID] (CPU) ReadSample failed, hr="+hrHex(hr)+((flags&MF_SOURCE_READERF_ERROR)?L" (stream error flag)":L""));
        if(readFails>=5){ reopen=true; break; }
        Sleep(100); continue; }
      readFails=0;
      if(flags&MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED){                                // the picture format changed under us: the size and stride assumed so far are void
        if(smp) smp->Release();
        logLine(L"[AFKVID] (CPU) the decoder changed its output format -- reopening the clip"); reopen=true; break; }
      if(flags&MF_SOURCE_READERF_ENDOFSTREAM){
        if(smp) smp->Release();
        if(!sinceSeek){ logLine(L"[AFKVID] (CPU) end of stream again right after the loop restart -- reopening the clip"); reopen=true; break; }
        PROPVARIANT v; PropVariantInit(&v); v.vt=VT_I8; v.hVal.QuadPart=0;
        const HRESULT sk=rd->SetCurrentPosition(GUID_NULL,v); PropVariantClear(&v);
        if(FAILED(sk)){ logLine(L"[AFKVID] (CPU) the loop restart failed, hr="+hrHex(sk)+L" -- reopening the clip"); reopen=true; break; }
        sinceSeek=0; pacer.reset(); continue; }
      if(!smp){ Sleep(1); continue; }                                                     // a gap in the stream: nothing to show yet
      const int64_t c0=hnsNow(); CpuFrame* fb=nullptr; IMFMediaBuffer* buf=nullptr;
      if(SUCCEEDED(smp->ConvertToContiguousBuffer(&buf))&&buf){ fb=frameFromBuffer(S,buf,stride,vw,vh); buf->Release(); }
      smp->Release(); S.stCopyUs+=(unsigned long long)((hnsNow()-c0)/10);
      if(!fb){
        if(!badLogged){ badLogged=true; logLine(L"[AFKVID] (CPU) a decoded buffer was unusable (wrong layout / lock failed) -- skipping such frames"); }
        if(++rejected>=30){ logLine(L"[AFKVID] (CPU) 30 decoded frames in a row were unusable -- reopening the clip"); reopen=true; break; }
        Sleep(1); continue; }
      rejected=0; framesThisOpen++; sinceSeek++;
      if(S.run.load()){                                                                   // pace only while the screen is up; the preload frame goes out at once
        bool sl=false; const int64_t wait=pacer.schedule(ts,hnsNow(),&sl); if(sl) S.stSlips++;
        DWORD ms=(DWORD)(wait/10000); if(ms>100) ms=100; if(ms) Sleep(ms); }
      S.vx.publish(fb,hnsNow()); S.frames++;
      if(S.state.load()==0){ S.state.store(1); logLine(L"[AFKVID] (CPU) first frame published "+fmt1((hnsNow()-openedAt)/10000.0)+L" ms after the first open"); }
    }
    rd->Release();
    if(!reopen) break;
    if(!framesThisOpen){                                   // this configuration never gave a usable picture: the next one gets its turn
      startK=usedK+1;
      if(startK>=nChain){
        startK=0;
        if(S.state.load()!=1){ S.why=L"no decode configuration produced a usable picture from "+S.path; S.state.store(2); return; }
        logLine(L"[AFKVID] (CPU) no decode configuration produced a picture this time -- the last picture stays; trying again in 5 s");
        sleepStop(S,5000); }
    } else startK=usedK;                                   // it had worked: the same configuration again
    logLine(L"[AFKVID] (CPU) reopening the clip"); sleepStop(S,300);
  }
}

// ------------------------------------------------------------------------------------------------ audio thread
// An audio-only Media Foundation session (the video is decoded above). The session is started only once its topology is resolved and the volume has
// been applied, so nothing is ever heard at the default volume; it loops when MESessionEnded says the whole presentation has been played.
static void audioMain(Shared& S){
  IMFMediaSession* sess=nullptr; IMFMediaSource* src=nullptr;
  bool playing=false, topoReady=false, started=false, gaveUp=false; int failures=0; int64_t setupAt=0;
  float appliedVol=-1.f; int appliedMute=-1;
  auto tearDown=[&](){
    if(sess){
      sess->Close();                                       // asynchronous: Shutdown() while it is still closing races the session's own threads
      const int64_t t0=hnsNow();
      for(;;){ IMFMediaEvent* ev=nullptr; const HRESULT eh=sess->GetEvent(MF_EVENT_FLAG_NO_WAIT,&ev);
        if(SUCCEEDED(eh)&&ev){ MediaEventType t=MEUnknown; ev->GetType(&t); ev->Release(); if(t==MESessionClosed) break; continue; }
        if(eh!=MF_E_NO_EVENTS_AVAILABLE||hnsNow()-t0>10000000LL) break;          // something else went wrong, or a second without news: stop waiting
        Sleep(5); }
      sess->Shutdown(); sess->Release(); sess=nullptr; }
    if(src){ src->Shutdown(); src->Release(); src=nullptr; }
    playing=topoReady=started=false; appliedVol=-1.f; appliedMute=-1; };
  auto applyVolume=[&](bool force){
    if(!sess) return; float vol=S.volume.load(); if(vol<0.f)vol=0.f; if(vol>1.f)vol=1.f; const int mute=S.muted.load()?1:0;
    if(!force&&vol==appliedVol&&mute==appliedMute) return;
    IMFSimpleAudioVolume* v=nullptr;
    if(SUCCEEDED(MFGetService(sess,MR_POLICY_VOLUME_SERVICE,IID_PPV_ARGS(&v)))&&v){ v->SetMasterVolume(vol); v->SetMute(mute?TRUE:FALSE); v->Release(); appliedVol=vol; appliedMute=mute; } };
  auto fail=[&](const wchar_t* step,HRESULT hr){ failures++; if(failures==1||failures==5) logLine(std::wstring(L"[AFKVID] (CPU) audio: ")+step+L" failed, hr="+hrHex(hr)+(failures>=5?L" -- giving up on sound for this session":L""));
    tearDown(); if(failures>=5) gaveUp=true; sleepStop(S,400); };
  auto startAt=[&](bool fromZero)->HRESULT{ PROPVARIANT p; PropVariantInit(&p); if(fromZero){ p.vt=VT_I8; p.hVal.QuadPart=0; } const HRESULT h=sess->Start(&GUID_NULL,&p); PropVariantClear(&p); return h; };

  while(!S.stop.load()){
    if(!S.run.load()){ if(sess||src) tearDown(); failures=0; gaveUp=false; Sleep(60); continue; }
    if(gaveUp){ Sleep(100); continue; }
    if(!playing){
      IMFSourceResolver* res=nullptr; HRESULT hr=MFCreateSourceResolver(&res); if(FAILED(hr)||!res){ fail(L"MFCreateSourceResolver",hr); continue; }
      MF_OBJECT_TYPE ot=MF_OBJECT_INVALID; IUnknown* unk=nullptr;
      hr=res->CreateObjectFromURL(S.path.c_str(),MF_RESOLUTION_MEDIASOURCE,NULL,&ot,&unk); res->Release();
      if(FAILED(hr)||!unk){ if(unk) unk->Release(); fail(L"CreateObjectFromURL",hr); continue; }
      hr=unk->QueryInterface(IID_PPV_ARGS(&src)); unk->Release(); if(FAILED(hr)||!src){ src=nullptr; fail(L"IMFMediaSource",hr); continue; }
      IMFPresentationDescriptor* pd=nullptr; hr=src->CreatePresentationDescriptor(&pd); if(FAILED(hr)||!pd){ fail(L"CreatePresentationDescriptor",hr); continue; }
      IMFTopology* topo=nullptr; hr=MFCreateTopology(&topo); if(FAILED(hr)||!topo){ pd->Release(); fail(L"MFCreateTopology",hr); continue; }
      DWORD nsd=0; pd->GetStreamDescriptorCount(&nsd); bool added=false;
      for(DWORD i=0;i<nsd;i++){
        BOOL sel=FALSE; IMFStreamDescriptor* sd=nullptr; if(FAILED(pd->GetStreamDescriptorByIndex(i,&sel,&sd))||!sd) continue;
        IMFMediaTypeHandler* th=nullptr; GUID maj=GUID_NULL; if(SUCCEEDED(sd->GetMediaTypeHandler(&th))&&th) th->GetMajorType(&maj);
        if(sel&&maj==MFMediaType_Audio&&!added){
          IMFTopologyNode* sn=nullptr; IMFTopologyNode* on=nullptr; IMFActivate* ar=nullptr;
          if(SUCCEEDED(MFCreateTopologyNode(MF_TOPOLOGY_SOURCESTREAM_NODE,&sn))&&sn){
            sn->SetUnknown(MF_TOPONODE_SOURCE,src); sn->SetUnknown(MF_TOPONODE_PRESENTATION_DESCRIPTOR,pd); sn->SetUnknown(MF_TOPONODE_STREAM_DESCRIPTOR,sd); topo->AddNode(sn);
            if(SUCCEEDED(MFCreateAudioRendererActivate(&ar))&&ar){
              if(SUCCEEDED(MFCreateTopologyNode(MF_TOPOLOGY_OUTPUT_NODE,&on))&&on){
                on->SetObject(ar); on->SetUINT32(MF_TOPONODE_STREAMID,0); on->SetUINT32(MF_TOPONODE_NOSHUTDOWN_ON_REMOVE,FALSE); topo->AddNode(on); sn->ConnectOutput(0,on,0); added=true; on->Release(); }
              ar->Release(); }
            sn->Release(); } }
        else pd->DeselectStream(i);                                // the video is decoded by videoMain
        if(th) th->Release(); sd->Release(); }
      if(!added){ topo->Release(); pd->Release(); fail(L"no audio stream to play",E_FAIL); continue; }
      hr=MFCreateMediaSession(NULL,&sess); if(FAILED(hr)||!sess){ sess=nullptr; topo->Release(); pd->Release(); fail(L"MFCreateMediaSession",hr); continue; }
      hr=sess->SetTopology(0,topo); topo->Release(); pd->Release();
      if(FAILED(hr)){ fail(L"SetTopology",hr); continue; }
      playing=true; topoReady=false; started=false; setupAt=hnsNow();                    // Start() waits for MF_TOPOSTATUS_READY, below
    }
    if(sess){
      HRESULT bad=S_OK; const wchar_t* badStep=L""; bool ended=false;
      for(int n=0;n<16;n++){                                   // everything that has queued up, not one event per pass
        IMFMediaEvent* ev=nullptr; if(FAILED(sess->GetEvent(MF_EVENT_FLAG_NO_WAIT,&ev))||!ev) break;
        MediaEventType t=MEUnknown; HRESULT st=S_OK; ev->GetType(&t); ev->GetStatus(&st);
        if(FAILED(st)){ bad=st; badStep=(t==MEError)?L"media session error":L"media session event"; }
        else if(t==MESessionTopologyStatus){ UINT32 ts=0; ev->GetUINT32(MF_EVENT_TOPOLOGY_STATUS,&ts); if(ts==MF_TOPOSTATUS_READY) topoReady=true; }
        else if(t==MESessionEnded) ended=true;                 // the whole presentation has played (MEEndOfPresentation comes earlier, while the renderer is still draining)
        ev->Release(); if(FAILED(bad)) break; }
      if(FAILED(bad)){ fail(badStep,bad); continue; }
      if(topoReady&&!started){
        applyVolume(true);                                     // the volume exists once the topology is resolved: set it BEFORE the first sample is heard
        const HRESULT sh=startAt(false); if(FAILED(sh)){ fail(L"Start",sh); continue; }
        started=true; }
      else if(ended&&started){ const HRESULT sh=startAt(true); if(FAILED(sh)){ fail(L"loop restart",sh); continue; } }
      else if(!started&&hnsNow()-setupAt>50000000LL){ fail(L"the audio topology was not ready within 5 s",E_FAIL); continue; }
      applyVolume(false); }
    Sleep(started?40:5);
  }
  tearDown();
}

struct ThreadArg{ Shared* sh; bool audio; };            // carries one reference to `sh`, released by the worker when it ends
static DWORD WINAPI workerThunk(LPVOID p){
  ThreadArg* a=(ThreadArg*)p; Shared* sh=a->sh; bool audio=a->audio; delete a;
  cpuNameThread(audio?L"cpu-audio":L"cpu-video");
  HRESULT co=CoInitializeEx(nullptr,COINIT_MULTITHREADED); HRESULT mf=MFStartup(MF_VERSION);
  if(SUCCEEDED(mf)){ if(audio) audioMain(*sh); else videoMain(*sh); MFShutdown(); }
  else if(!audio){ sh->why=L"MFStartup failed, hr="+hrHex(mf); sh->state.store(2); }
  if(SUCCEEDED(co)) CoUninitialize();
  sh->workers--; shRelease(sh); return 0; }

// ------------------------------------------------------------------------------------------------ the back-end
class AfkCpuBackend final : public IAfkBackend{
 public:
  ~AfkCpuBackend() override { shutdown(); }
  const wchar_t* name() const override { return L"CPU renderer (GDI+), software decode"; }

  bool init(HWND hwnd,int W,int H,const std::wstring& clip,double clipFps,float dpiScale,std::wstring& why) override{
    (void)dpiScale; m_hwnd=hwnd; m_clipFps=clipFps>1.0?clipFps:30.0; m_t0=hnsNow();
    if(clip.empty()){ m_veil=true; m_inited=true; loadFrames(); logLine(L"[AFKVID] (CPU) no clip: dark background and HUD"+(m_images.empty()?std::wstring():(L", "+std::to_wstring(m_images.size())+L" image frames"))); return true; }
    m_sh=new Shared(); m_sh->path=clip; m_sh->targetW=(UINT32)(W>0?W:0); m_sh->targetH=(UINT32)(H>0?H:0); m_sh->clipFps=m_clipFps;
    m_sh->workers=2;
    HANDLE th[2]={nullptr,nullptr};
    for(int k=0;k<2;k++){ shAddRef(m_sh); ThreadArg* a=new ThreadArg{m_sh,k==1}; th[k]=CreateThread(nullptr,0,workerThunk,a,0,nullptr);
      if(!th[k]){ DWORD e=GetLastError(); delete a; m_sh->workers--; shRelease(m_sh);
        if(k==0){ why=L"CreateThread failed, error "+std::to_wstring(e); m_sh->stop.store(true); shRelease(m_sh); m_sh=nullptr; return false; }
        logLine(L"[AFKVID] (CPU) the audio thread could not be started (error "+std::to_wstring(e)+L") -- no sound"); } }
    m_vth=th[0]; m_ath=th[1];
    if(m_vth) SetThreadPriority(m_vth,THREAD_PRIORITY_BELOW_NORMAL);       // the preload must not compete with the user's work
    m_inited=true; return true; }

  int pump(std::wstring& why) override{
    if(!m_inited){ why=L"not initialised"; return AFKB_FAILED; }
    if(m_veil) return AFKB_READY;
    int st=m_sh->state.load();
    if(st==1){ if(!m_ready){ m_ready=true; m_readyAt=hnsNow(); } return AFKB_READY; }
    if(st==2){ // nothing playable: carry on without video (dark background + HUD) rather than failing the whole screen
      logLine(L"[AFKVID] (CPU) no video: "+m_sh->why+L" -- the AFK screen runs with the dark background and HUD only");
      m_veil=true; m_sh->stop.store(true); loadFrames(); return AFKB_READY; }
    return AFKB_LOADING; }

  void setActive(bool active) override{
    if(!m_inited) return; m_active=active; if(active){ m_frameT0=GetTickCount64(); m_due=0; }     // (the render grid starts over with the screen)
    if(!m_sh) return;
    m_sh->run.store(active);
    if(m_vth) SetThreadPriority(m_vth,active?THREAD_PRIORITY_NORMAL:THREAD_PRIORITY_BELOW_NORMAL); }
  void setAudio(float volume,bool muted) override{ if(m_sh){ m_sh->volume.store(volume); m_sh->muted.store(muted); } }

  bool resize(int W,int H) override{ (void)W; (void)H; return true; }        // the back buffer follows the size in the frame parameters

  int render(const AfkFrameParams& p) override{
    if(!m_inited||p.W<=0||p.H<=0) return AFKR_FAILED;
    int64_t t0=hnsNow();
    if(!ensureBackbuffer(p.W,p.H)) return AFKR_FAILED;
    ensureFonts(p.uiScale);
    if(m_starW!=p.W||m_starH!=p.H||m_stars.empty()){ afkStarsInit(m_stars,90,p.W,p.H,m_rng); m_starW=p.W; m_starH=p.H; }
    afkStarsStep(m_stars,p.dtSec,p.W,p.H,m_rng);
    const int W=p.W,H=p.H; const float af=p.af;

    // ---- video: one pass when the frame is exactly window sized, else the v13 GDI+ draw -----------------------------------
    bool haveVideo=false, vignetteDone=false, slow=false; float vf=0.f;
    if(m_sh){
      CpuFrame *curr=nullptr,*prev=nullptr; int64_t cT=0,pT=0; m_sh->vx.acquireForDraw(curr,prev,cT,pT);
      if(curr&&curr->px){
        if(!m_firstShown){ m_firstShown=true; m_firstShownAt=hnsNow(); }
        vf=af*p.vidF*afkVideoReadyFade((hnsNow()-m_firstShownAt)/10000);
        bool usePrev=prev&&prev->px&&prev->w==curr->w&&prev->h==curr->h;
        float t=usePrev?afkBlendT(hnsNow(),cT,pT,600000):1.0f;                  // 60 ms: below ~16 fps a hard cut starts to show
        if((int)curr->w==W&&(int)curr->h==H&&ensureGain(W,H)){
          if(t>=0.999f) afkMulGainOpaque32(curr->px,W*4,(const uint8_t*)m_gain.data(),W*4,m_bits,W*4,(uint32_t)W,(uint32_t)H);
          else afkBlendGainOpaque32(prev->px,curr->px,W*4,(int)(t*128.0f+0.5f),(const uint8_t*)m_gain.data(),W*4,m_bits,W*4,(uint32_t)W,(uint32_t)H);
          vignetteDone=true; haveVideo=true;
        } else { afkFill32(m_bits,W*4,(uint32_t)W,(uint32_t)H,0xFF060810u); slow=true; haveVideo=true;
          drawScaled(curr,usePrev?prev:nullptr,t,W,H); }
      }
      m_sh->vx.releaseAfterDraw();
    }
    int64_t t1=hnsNow();
    if(!haveVideo) afkFill32(m_bits,W*4,(uint32_t)W,(uint32_t)H,0xFF060810u);

    Graphics g(m_back); g.SetSmoothingMode(SmoothingModeAntiAlias); g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
    if(!haveVideo&&!m_images.empty()){                                          // image-sequence fallback (v13)
      int n=(int)m_images.size(); int fi=(int)(((p.nowMs-m_frameT0)/42)%(unsigned)n); Bitmap* fr=m_images[fi];
      if(fr){ REAL fw=(REAL)fr->GetWidth(),fh=(REAL)fr->GetHeight(); float d[4]; afkCoverDest(fw,fh,(float)W,(float)H,d);
        ColorMatrix cm; ZeroMemory(&cm,sizeof(cm)); cm.m[0][0]=cm.m[1][1]=cm.m[2][2]=1; cm.m[4][4]=1; cm.m[3][3]=af*p.vidF;
        ImageAttributes ia; ia.SetColorMatrix(&cm); ia.SetWrapMode(WrapModeTileFlipXY);
        g.SetInterpolationMode(InterpolationModeHighQualityBicubic); g.SetPixelOffsetMode(PixelOffsetModeHalf);
        g.DrawImage(fr,RectF(d[0],d[1],d[2],d[3]),0.0f,0.0f,fw,fh,UnitPixel,&ia); vf=af*p.vidF; haveVideo=true; } }
    // fade-in / fade-out: lay the background colour over the video (the same solid the back buffer was cleared with)
    if(haveVideo&&vf<0.995f){ if(vf<0.f) vf=0.f; SolidBrush fb(Color((int)((1.0f-vf)*255.0f+0.5f),6,8,16)); g.FillRectangle(&fb,0.0f,0.0f,(REAL)W,(REAL)H); }
    // camera-lens vignette over the video only (the one-pass compose already applied it)
    if(haveVideo&&!vignetteDone){ ensureVignetteBitmap(W,H); if(m_vigBmp){ g.SetCompositingMode(CompositingModeSourceOver); g.DrawImage(m_vigBmp,0,0,W,H); } }
    int64_t t2=hnsNow();

    // ---- HUD: stars (dimmed in the lens circle), bottom gradient, clock, quote, hint --------------------------------------
    const float k=p.uiScale; const AfkLayout L=afkLayout(W,H,k);
    for(size_t i=0;i<m_stars.size();i++){ const AfkStar& st=m_stars[i];
      float sa=st.a*af*p.starF*afkStarCenterDim(st.x,st.y,W,H); if(sa<2.f) continue;
      SolidBrush glow(Color((int)(sa/3.f),170,205,255)); g.FillEllipse(&glow,st.x-2.0f*k,st.y-2.0f*k,7.0f*k,7.0f*k);
      SolidBrush core(Color((int)sa,225,238,255)); g.FillEllipse(&core,st.x,st.y,2.6f*k,2.6f*k); }
    { int ga=(int)(215.0f*af*p.txtF+0.5f); if(ga>0){ LinearGradientBrush lg(PointF(0.0f,L.gradY),PointF(0.0f,(REAL)H),Color(0,0,0,0),Color(ga,0,0,0)); g.FillRectangle(&lg,0.0f,L.gradY,(REAL)W,(REAL)H-L.gradY); } }
    { int a=(int)(255.0f*af*p.txtF+0.5f);
      if(a>0&&m_fClock){ wchar_t clk[9]; afkFmtClock(clk,p.hh,p.mm,p.ss); SolidBrush cb(Color(a,220,230,245)); g.DrawString(clk,-1,m_fClock,PointF((REAL)W*0.5f-m_clockW*0.5f,L.clockY),&cb); } }
    buildQuote(g,p,L);
    { int a=(int)(255.0f*af*p.quoteA*p.txtF+0.5f);
      if(a>0&&!m_qWords.empty()){ SolidBrush qb(Color(a,236,239,249));
        for(size_t i=0;i<m_qWords.size();i++){ const QWord& w=m_qWords[i]; g.DrawString(w.s.c_str(),-1,w.b?m_qBold[m_qStep]:m_qReg[m_qStep],PointF(w.x,w.y),&qb); } } }
    { int a=(int)(255.0f*0.7f*af*p.txtF+0.5f);
      if(a>0&&m_fHint){ SolidBrush hb(Color(a,150,160,190)); g.DrawString(L"AFK  ·  move or press a key to resume",-1,m_fHint,PointF((REAL)W*0.5f-m_hintW*0.5f,L.hintY),&hb); } }
    int64_t t3=hnsNow();

    // ---- present: a plain memory copy of the DIB section ------------------------------------------------------------------
    HDC dc=GetDC(m_hwnd); if(!dc) return AFKR_FAILED;
    BitBlt(dc,0,0,W,H,m_dc,0,0,SRCCOPY); ReleaseDC(m_hwnd,dc);
    int64_t t4=hnsNow();
    m_rN++; m_rTotUs+=(t4-t0)/10; m_rVideoUs+=(t1-t0)/10; m_rVigUs+=(t2-t1)/10; m_rHudUs+=(t3-t2)/10; m_rPresUs+=(t4-t3)/10;
    m_lastCostMs=(t4-t0)/10000.0; m_avgCostMs=m_avgCostMs<=0?m_lastCostMs:m_avgCostMs*0.9+m_lastCostMs*0.1;
    if(slow) m_slowFrames++;
    return AFKR_PRESENTED; }

  // Wait to add after render(): until the next frame is due on an ABSOLUTE grid (afkGridNext), so that a late wake-up or one slow frame does not shift all the
  // following ones. The tick is the measured cost x1.3 within 33..66 ms (30 fps ceiling, 15 fps floor). Never below 1 ms: the thread must always yield.
  int frameDelayMs() override{
    const int64_t now=hnsNow(), tick=(int64_t)afkCpuTickMs(m_avgCostMs)*10000;
    m_due=afkGridNext(m_due,now,tick);
    const int64_t wait=m_due-now; const int ms=(int)((wait+9999)/10000);
    return ms<1?1:ms; }
  uint64_t videoFrames() override{ return m_sh?(uint64_t)m_sh->frames.load():0; }
  double clipFps() override{ return m_clipFps; }

  void logStats(uint64_t elapsedMs) override{
    if(!m_inited||!m_rN||!elapsedMs) return;
    double n=(double)m_rN, sec=(double)elapsedMs/1000.0;
    // deltas since the last report -- the totals themselves must keep counting (videoFrames() feeds the controller)
    unsigned long long fT=m_sh?m_sh->frames.load():0, rT=m_sh?m_sh->stReadUs.load():0, cT=m_sh?m_sh->stCopyUs.load():0, sT=m_sh?m_sh->stSlips.load():0;
    unsigned long long df=fT-m_pDec, rus=rT-m_pRead, cus=cT-m_pCopy, sl=sT-m_pSlip; m_pDec=fT; m_pRead=rT; m_pCopy=cT; m_pSlip=sT;
    logLine(L"[AFKVID] stats (CPU, "+std::to_wstring(m_bw)+L"x"+std::to_wstring(m_bh)+L"): render "+fmt1(n/sec)+L" fps, "+fmt1(m_rTotUs/n/1000.0)+L" ms/frame (video "+fmt1(m_rVideoUs/n/1000.0)+L", fade+vignette "+fmt1(m_rVigUs/n/1000.0)
      +L", HUD "+fmt1(m_rHudUs/n/1000.0)+L", present "+fmt1(m_rPresUs/n/1000.0)+L"; "+std::to_wstring(m_slowFrames)+L" of "+std::to_wstring(m_rN)+L" frames took the scaled GDI+ path) | decode "
      +fmt1((double)df/sec)+L" fps, ReadSample "+fmt1(df?(double)rus/(double)df/1000.0:0.0)+L" ms, copy "+fmt1(df?(double)cus/(double)df/1000.0:0.0)+L" ms, timeline slips "+std::to_wstring(sl));
    m_rN=m_rTotUs=m_rVideoUs=m_rVigUs=m_rHudUs=m_rPresUs=0; m_slowFrames=0; }

  void shutdown() override{
    if(!m_inited) return; m_inited=false;
    if(m_sh){ m_sh->stop.store(true); m_sh->run.store(false);
      // ONE shared budget for both workers (the controller's own stop has to fit in 5 s): a worker still busy after it is abandoned -- it only touches state
      // it co-owns. The audio worker may spend up to a second waiting for its media session to close.
      HANDLE hs[2]; DWORD n=0; if(m_vth) hs[n++]=m_vth; if(m_ath) hs[n++]=m_ath;
      if(n&&WaitForMultipleObjects(n,hs,TRUE,2500)==WAIT_TIMEOUT) logLine(L"[AFKVID] (CPU) a worker thread did not stop within 2.5 s (it is abandoned; it only touches state it co-owns)");
      for(DWORD i=0;i<n;i++) CloseHandle(hs[i]);
      m_vth=m_ath=nullptr; shRelease(m_sh); m_sh=nullptr; }
    for(size_t i=0;i<m_images.size();i++) delete m_images[i]; m_images.clear();
    delete m_vigBmp; m_vigBmp=nullptr; m_vigAlpha.clear(); m_gain.clear(); m_gainW=m_gainH=0;
    dropFonts();
    dropBackbuffer(); }

 private:
  struct QWord{ std::wstring s; bool b; float x,y; };

  // ---- back buffer: a top-down 32 bpp DIB section that GDI+ draws into and BitBlt presents -----------------------------------
  bool ensureBackbuffer(int W,int H){
    if(m_back&&m_bw==W&&m_bh==H&&m_bits) return true;
    dropBackbuffer(); m_bw=W; m_bh=H; if(W<=0||H<=0) return false;
    BITMAPINFO bi; ZeroMemory(&bi,sizeof(bi)); bi.bmiHeader.biSize=sizeof(BITMAPINFOHEADER); bi.bmiHeader.biWidth=W; bi.bmiHeader.biHeight=-H;
    bi.bmiHeader.biPlanes=1; bi.bmiHeader.biBitCount=32; bi.bmiHeader.biCompression=BI_RGB;
    void* bits=nullptr; HDC sdc=GetDC(nullptr); m_dib=CreateDIBSection(sdc,&bi,DIB_RGB_COLORS,&bits,nullptr,0); ReleaseDC(nullptr,sdc);
    if(!m_dib||!bits){ logLine(L"[AFKVID] (CPU) CreateDIBSection("+std::to_wstring(W)+L"x"+std::to_wstring(H)+L") failed, error "+std::to_wstring(GetLastError())); dropBackbuffer(); return false; }
    m_dc=CreateCompatibleDC(nullptr); if(!m_dc){ dropBackbuffer(); return false; }
    m_oldBmp=(HBITMAP)SelectObject(m_dc,m_dib); m_bits=(uint8_t*)bits;
    m_back=new Bitmap(W,H,W*4,PixelFormat32bppPARGB,(BYTE*)bits);
    if(m_back->GetLastStatus()!=Ok){ logLine(L"[AFKVID] (CPU) GDI+ could not wrap the back buffer"); dropBackbuffer(); return false; }
    return true; }
  void dropBackbuffer(){
    delete m_back; m_back=nullptr; m_bits=nullptr;
    if(m_dc){ if(m_oldBmp) SelectObject(m_dc,m_oldBmp); DeleteDC(m_dc); m_dc=nullptr; } m_oldBmp=nullptr;
    if(m_dib){ DeleteObject(m_dib); m_dib=nullptr; }
    m_bw=m_bh=0; }

  // ---- vignette: gain map for the one-pass compose, alpha bitmap for the GDI+ path ---------------------------------------------
  bool ensureGain(int W,int H){
    if(m_gainW==W&&m_gainH==H&&!m_gain.empty()) return true;
    try{ m_gain.assign((size_t)W*(size_t)H,0u); }catch(...){ m_gain.clear(); m_gainW=m_gainH=0; return false; }
    afkGainMapFromVignette(m_gain.data(),W,H,200.0f/255.0f); m_gainW=W; m_gainH=H; return true; }
  void ensureVignetteBitmap(int W,int H){
    if(m_vigBmp&&m_vigW==W&&m_vigH==H) return;
    delete m_vigBmp; m_vigBmp=nullptr; m_vigW=m_vigH=0;
    try{ m_vigAlpha.assign((size_t)W*(size_t)H,0u); }catch(...){ m_vigAlpha.clear(); return; }
    afkVignetteFill((uint8_t*)m_vigAlpha.data(),W*4,W,H,200.0f/255.0f);
    m_vigBmp=new Bitmap(W,H,W*4,PixelFormat32bppPARGB,(BYTE*)m_vigAlpha.data());
    if(m_vigBmp->GetLastStatus()!=Ok){ delete m_vigBmp; m_vigBmp=nullptr; return; }
    m_vigW=W; m_vigH=H; }

  // ---- scaled video draw (v13): used when the frame is not exactly the window size -------------------------------------------
  void drawScaled(CpuFrame* curr,CpuFrame* prev,float t,int W,int H){
    Graphics g(m_back); g.SetPixelOffsetMode(PixelOffsetModeHalf);
    REAL fw=(REAL)curr->w,fh=(REAL)curr->h; float d[4]; afkCoverDest(fw,fh,(float)W,(float)H,d); RectF dest(d[0],d[1],d[2],d[3]);   // float rect: no 1-px sliver at an edge
    bool oneToOne=(d[2]>=fw*0.998f&&d[2]<=fw*1.002f);
    g.SetInterpolationMode(oneToOne?InterpolationModeNearestNeighbor:InterpolationModeBilinear);
    Bitmap bc((INT)curr->w,(INT)curr->h,(INT)(curr->w*4),PixelFormat32bppARGB,curr->px);
    ImageAttributes ia; ia.SetWrapMode(WrapModeTileFlipXY);                                                      // wrap: no edge fringe
    g.SetCompositingMode(CompositingModeSourceCopy);
    if(prev&&t<0.999f){ Bitmap bp((INT)prev->w,(INT)prev->h,(INT)(prev->w*4),PixelFormat32bppARGB,prev->px);
      g.DrawImage(&bp,dest,0.0f,0.0f,fw,fh,UnitPixel,&ia);
      g.SetCompositingMode(CompositingModeSourceOver);
      ColorMatrix cm={ {{1,0,0,0,0},{0,1,0,0,0},{0,0,1,0,0},{0,0,0,t,0},{0,0,0,0,1}} };
      ImageAttributes ib; ib.SetColorMatrix(&cm,ColorMatrixFlagsDefault,ColorAdjustTypeBitmap); ib.SetWrapMode(WrapModeTileFlipXY);
      g.DrawImage(&bc,dest,0.0f,0.0f,fw,fh,UnitPixel,&ib);
    } else g.DrawImage(&bc,dest,0.0f,0.0f,fw,fh,UnitPixel,&ia);
    g.SetCompositingMode(CompositingModeSourceOver); }

  // ---- fonts (own objects: GDI+ objects must not be shared between threads) ---------------------------------------------------
  void dropFonts(){
    delete m_fClock; delete m_fHint; m_fClock=m_fHint=nullptr;
    for(int i=0;i<AFK_QUOTE_STEPS;i++){ delete m_qReg[i]; delete m_qBold[i]; m_qReg[i]=m_qBold[i]=nullptr; }
    delete m_famUI; delete m_famMono; m_famUI=m_famMono=nullptr; m_fontScale=0.f; m_qKey.clear(); m_qWords.clear(); }
  static FontFamily* makeFamily(const wchar_t* a,const wchar_t* b,bool mono){
    FontFamily* f=new FontFamily(a); if(f->GetLastStatus()==Ok) return f; delete f;
    f=new FontFamily(b); if(f->GetLastStatus()==Ok) return f; delete f;
    const FontFamily* gen=mono?FontFamily::GenericMonospace():FontFamily::GenericSansSerif(); return gen->Clone(); }
  void ensureFonts(float s){
    if(m_famUI&&m_fontScale==s) return;
    dropFonts(); m_fontScale=s;
    m_famUI=makeFamily(L"Segoe UI",L"Arial",false); m_famMono=makeFamily(L"Consolas",L"Courier New",true);
    m_fClock=new Font(m_famMono,46.0f*s,FontStyleBold,UnitPixel); m_fHint=new Font(m_famMono,11.0f*s,FontStyleRegular,UnitPixel);
    for(int i=0;i<AFK_QUOTE_STEPS;i++){ m_qReg[i]=new Font(m_famUI,AFK_QUOTE_SIZES[i]*s,FontStyleRegular,UnitPixel); m_qBold[i]=new Font(m_famUI,(AFK_QUOTE_SIZES[i]+4.0f)*s,FontStyleBold,UnitPixel); }
    // widths of the fixed texts, measured once
    Bitmap tmp(8,8,PixelFormat32bppPARGB); Graphics g(&tmp); g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit); RectF bb;
    g.MeasureString(L"00:00:00",-1,m_fClock,PointF(0,0),&bb); m_clockW=bb.Width;
    g.MeasureString(L"AFK  ·  move or press a key to resume",-1,m_fHint,PointF(0,0),&bb); m_hintW=bb.Width; }

  // ---- quote: word layout measured once per (text, user, size, scale) ---------------------------------------------------------
  void buildQuote(Graphics& g,const AfkFrameParams& p,const AfkLayout& L){
    std::wstring key=p.quote; key+=L'\x1f'; key+=p.user; key+=L'\x1f'; key+=std::to_wstring(p.W); key+=L'x'; key+=std::to_wstring(p.H); key+=L'@'; key+=std::to_wstring((int)(p.uiScale*100.0f+0.5f));
    if(key==m_qKey) return;
    m_qKey=key; m_qWords.clear(); m_qStep=AFK_QUOTE_STEPS-1;
    std::wstring plain; std::vector<AfkTextRun> bold; afkParseQuote(p.quote,p.user,plain,bold); if(plain.empty()) return;
    std::vector<char> isB(plain.size(),0); for(size_t r=0;r<bold.size();r++) for(size_t i=bold[r].start;i<bold[r].start+bold[r].len&&i<plain.size();i++) isB[i]=1;
    struct Wd{ std::wstring s; bool b; }; std::vector<Wd> words; std::wstring cur; bool curB=false;
    for(size_t i=0;i<plain.size();i++){ bool b=isB[i]!=0; if(!cur.empty()&&b!=curB){ words.push_back({cur,curB}); cur.clear(); } curB=b; cur+=plain[i];
      if(plain[i]==L' '){ words.push_back({cur,curB}); cur.clear(); } }
    if(!cur.empty()) words.push_back({cur,curB});
    for(int step=0;step<AFK_QUOTE_STEPS;step++){
      const float lh=AFK_QUOTE_SIZES[step]*p.uiScale*1.385f; Font* fr=m_qReg[step]; Font* fb=m_qBold[step];
      std::vector<float> wd(words.size()); std::vector<float> hh(words.size());
      for(size_t i=0;i<words.size();i++){ RectF bb; g.MeasureString(words[i].s.c_str(),-1,words[i].b?fb:fr,PointF(0,0),&bb); wd[i]=bb.Width; hh[i]=bb.Height; }
      std::vector<std::pair<size_t,size_t>> lines; size_t from=0; float lw=0;                                // [from,to) word ranges
      for(size_t i=0;i<words.size();i++){ if(lw+wd[i]>L.quoteW&&i>from){ lines.push_back(std::make_pair(from,i)); from=i; lw=0; } lw+=wd[i]; }
      if(from<words.size()) lines.push_back(std::make_pair(from,words.size()));
      float height=(float)lines.size()*lh;
      if(height<=L.quoteAvail||step==AFK_QUOTE_STEPS-1){
        float top=L.quoteTop+(L.quoteAvail-height)*0.5f; if(top<L.quoteTop) top=L.quoteTop;
        m_qStep=step; m_qWords.clear(); float y=top;
        for(size_t li=0;li<lines.size();li++){ float tot=0; for(size_t i=lines[li].first;i<lines[li].second;i++) tot+=wd[i];
          float x=(float)p.W*0.5f-tot*0.5f;
          for(size_t i=lines[li].first;i<lines[li].second;i++){ QWord w; w.s=words[i].s; w.b=words[i].b; w.x=x; w.y=y+(lh-hh[i])*0.5f; m_qWords.push_back(w); x+=wd[i]; }
          y+=lh; }
        return; } } }

  void loadFrames(){
    for(size_t i=0;i<m_images.size();i++) delete m_images[i]; m_images.clear();
    std::wstring dir=appDir()+L"\\content\\afk"; WIN32_FIND_DATAW fd; HANDLE h=FindFirstFileW((dir+L"\\*").c_str(),&fd); if(h==INVALID_HANDLE_VALUE) return;
    std::vector<std::wstring> names;
    do{ std::wstring l=lc(fd.cFileName); if(endsW(l,L".png")||endsW(l,L".jpg")||endsW(l,L".jpeg")||endsW(l,L".bmp")) names.push_back(fd.cFileName); }while(FindNextFileW(h,&fd)); FindClose(h);
    std::sort(names.begin(),names.end());
    for(size_t i=0;i<names.size()&&m_images.size()<240;i++){ Bitmap* b=Bitmap::FromFile((dir+L"\\"+names[i]).c_str()); if(b&&b->GetLastStatus()==Ok) m_images.push_back(b); else delete b; } }

  // state ------------------------------------------------------------------------------------------------------------------------
  Shared* m_sh=nullptr; HANDLE m_vth=nullptr, m_ath=nullptr;      // one reference owned here, one per running worker
  HWND m_hwnd=nullptr; double m_clipFps=30.0; bool m_inited=false, m_ready=false, m_active=false, m_veil=false, m_firstShown=false;
  int64_t m_t0=0, m_readyAt=0, m_firstShownAt=0; ULONGLONG m_frameT0=0;
  HBITMAP m_dib=nullptr; HDC m_dc=nullptr; HBITMAP m_oldBmp=nullptr; uint8_t* m_bits=nullptr; Bitmap* m_back=nullptr; int m_bw=0,m_bh=0;
  std::vector<uint32_t> m_gain; int m_gainW=0,m_gainH=0; std::vector<uint32_t> m_vigAlpha; Bitmap* m_vigBmp=nullptr; int m_vigW=0,m_vigH=0;
  FontFamily *m_famUI=nullptr,*m_famMono=nullptr; Font *m_fClock=nullptr,*m_fHint=nullptr; Font *m_qReg[AFK_QUOTE_STEPS]={},*m_qBold[AFK_QUOTE_STEPS]={};
  float m_fontScale=0.f, m_clockW=0.f, m_hintW=0.f;
  std::wstring m_qKey; std::vector<QWord> m_qWords; int m_qStep=AFK_QUOTE_STEPS-1;
  std::vector<AfkStar> m_stars; AfkRng m_rng; int m_starW=0,m_starH=0;
  std::vector<Bitmap*> m_images;
  double m_avgCostMs=0, m_lastCostMs=0; long long m_rN=0,m_rTotUs=0,m_rVideoUs=0,m_rVigUs=0,m_rHudUs=0,m_rPresUs=0; long long m_slowFrames=0;
  unsigned long long m_pDec=0,m_pRead=0,m_pCopy=0,m_pSlip=0;       // totals at the previous logStats()
  int64_t m_due=0;                                                  // when the frame just drawn was due (afkGridNext); 0 = the grid starts with the next frame
};

} // namespace

IAfkBackend* afkCreateCpuBackend(){ return new AfkCpuBackend(); }
