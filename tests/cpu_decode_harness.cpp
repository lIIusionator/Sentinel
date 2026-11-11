// cpu_decode_harness.cpp -- drives the CPU renderer's decode worker (videoMain in afk/afk_cpu_backend.cpp) against a SCRIPTED fake IMFSourceReader.
//
// The decode worker is where the earlier review found a hot loop, a silent thread death and a bad-frame validation hole; none of it can be exercised on a
// machine without the right codecs, so this harness replaces the one thing that needs them -- the source reader -- with an object that does exactly what the
// script says (frames in every layout, truncated or zero-stride buffers, read errors, a failing seek, a format change, a clip that cannot be reopened ...)
// and checks what the worker does about it: it must neither spin, nor die while a picture is on screen, nor read outside a buffer, and it must end in a
// defined state. Sleep() is replaced by a counter, so even the 30 s back-offs run instantly.
//
// Build:  make CROSS=x86_64-w64-mingw32- harness      Run: harness.exe (natively on Windows, or under Wine). Needs only Media Foundation's attribute /
// sample / buffer objects -- no codec, no GPU. Exit code 0 = every check passed.
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
#include <functional>
#include <vector>
#include <string>
#include <stdio.h>

// ------------------------------------------------------------------------------------------------ what sw_common.cpp would provide
static std::vector<std::wstring> g_log;
void logLine(const std::wstring& s){ g_log.push_back(s); }
void cpuNameThread(const wchar_t*){}
std::wstring hrHex(HRESULT hr){ wchar_t b[16]; wsprintfW(b,L"0x%08X",(unsigned)hr); return b; }
std::wstring fmt1(double v){ int x=(int)(v*10.0+(v<0?-0.5:0.5)); return std::to_wstring(x/10)+L"."+std::to_wstring(x<0?-(x%10):x%10); }
int64_t hnsNow(){ static LARGE_INTEGER f={}; if(!f.QuadPart) QueryPerformanceFrequency(&f);
  LARGE_INTEGER c; QueryPerformanceCounter(&c);
  return (int64_t)(c.QuadPart/f.QuadPart)*10000000LL + (int64_t)((c.QuadPart%f.QuadPart)*10000000LL/f.QuadPart); }
std::wstring lc(std::wstring s){ for(auto&c:s)c=(wchar_t)towlower(c); return s; }
bool endsW(const std::wstring&s,const std::wstring&e){ return s.size()>=e.size()&&lc(s.substr(s.size()-e.size()))==e; }
std::wstring appDir(){ return L"."; }

// ------------------------------------------------------------------------------------------------ the script
struct Plan{                                   // what ONE open of the clip does
  bool openFails=false;                        // MFCreateSourceReaderFromURL fails
  enum Layout{ TOPDOWN, BOTTOMUP, TWOD_TOPDOWN, TWOD_BOTTOMUP, ZERO_STRIDE, SHORT_BUFFER } layout=TOPDOWN;
  int  frames=4;                               // good frames per pass, then end-of-stream
  int  readFailsFirst=0;                       // ReadSample fails this many times (in a row) before it starts to work
  int  failAfterFrames=-1;                     // after this many frames ReadSample fails for good
  bool seekFails=false;                        // SetCurrentPosition fails
  bool eosAfterSeek=false;                     // the first read after a loop restart is end-of-stream again
  int  typeChangeAfter=-1;                     // CURRENTMEDIATYPECHANGED instead of the frame with this index
};
static std::vector<Plan> g_plans; static Plan g_planAfter;       // plans[n] is the nth open; later opens use g_planAfter
static UINT32 g_nativeW=1920, g_nativeH=1080;

static int g_opens=0, g_reads=0, g_seeks=0, g_delivered=0, g_stopAfter=0; static unsigned long long g_sleptMs=0, g_sleeps=0, g_sleep50=0;
static std::vector<int> g_openKinds;           // 1 = legacy video processing (+ hardware hint), 2 = advanced video processor
static std::vector<long long> g_seekPos;
static std::vector<DWORD> g_sleepLog;
static bool g_runaway=false;
static std::function<void()> g_onSleep, g_onOpen;
static std::function<void()> g_stopFn;         // sets S.stop

static void WINAPI fakeSleep(DWORD ms){
  g_sleptMs+=ms; g_sleeps++; if(ms==50) g_sleep50++; if(g_sleepLog.size()<64) g_sleepLog.push_back(ms);
  if(g_sleeps>200000){ g_runaway=true; if(g_stopFn) g_stopFn(); }
  if(g_onSleep) g_onSleep();
  ::Sleep(0); }

// pattern: image row y of frame f has B=(3y+1), G=f, R=0x11 -- the top row is therefore recognisable in the published frame, whatever the layout
static inline uint32_t pat(int y,int f){ return (uint32_t)((3*y+1)&0xFF) | ((uint32_t)(f&0xFF)<<8) | (0x11u<<16); }

static IMFMediaType* makeType(UINT32 w,UINT32 h,bool rgb32,LONG stride){
  IMFMediaType* t=nullptr; if(FAILED(MFCreateMediaType(&t))||!t) return nullptr;
  t->SetGUID(MF_MT_MAJOR_TYPE,MFMediaType_Video); t->SetGUID(MF_MT_SUBTYPE,rgb32?MFVideoFormat_RGB32:MFVideoFormat_H264);
  MFSetAttributeSize(t,MF_MT_FRAME_SIZE,w,h); MFSetAttributeRatio(t,MF_MT_FRAME_RATE,30,1);
  if(rgb32) t->SetUINT32(MF_MT_DEFAULT_STRIDE,(UINT32)stride);
  return t; }

struct FakeReader final : IMFSourceReader{
  LONG ref=1; Plan plan; bool probe=false; bool rgb32=false; UINT32 vw=0,vh=0; int framesThisPass=0, framesTotal=0, readFailsLeft=0; bool justSeeked=false, typeChangeSent=false, failedForGood=false;
  explicit FakeReader(const Plan& p,bool pr):plan(p),probe(pr){ readFailsLeft=p.readFailsFirst; vw=g_nativeW; vh=g_nativeH; }
  // IUnknown
  STDMETHODIMP QueryInterface(REFIID r,void** o) override { if(!o) return E_POINTER; if(r==IID_IUnknown||r==__uuidof(IMFSourceReader)){ *o=static_cast<IMFSourceReader*>(this); AddRef(); return S_OK; } *o=nullptr; return E_NOINTERFACE; }
  STDMETHODIMP_(ULONG) AddRef() override { return (ULONG)InterlockedIncrement(&ref); }
  STDMETHODIMP_(ULONG) Release() override { LONG n=InterlockedDecrement(&ref); if(!n) delete this; return (ULONG)n; }
  // IMFSourceReader
  HRESULT STDMETHODCALLTYPE GetStreamSelection(DWORD,WINBOOL* s) override { if(s) *s=TRUE; return S_OK; }
  HRESULT STDMETHODCALLTYPE SetStreamSelection(DWORD,WINBOOL) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE GetNativeMediaType(DWORD,DWORD ti,IMFMediaType** t) override { if(ti) return MF_E_NO_MORE_TYPES; *t=makeType(g_nativeW,g_nativeH,false,0); return *t?S_OK:E_FAIL; }
  HRESULT STDMETHODCALLTYPE GetCurrentMediaType(DWORD,IMFMediaType** t) override { *t=makeType(vw,vh,rgb32,strideForType()); return *t?S_OK:E_FAIL; }
  HRESULT STDMETHODCALLTYPE SetCurrentMediaType(DWORD,DWORD*,IMFMediaType* t) override {
    UINT32 w=0,h=0; if(t&&SUCCEEDED(MFGetAttributeSize(t,MF_MT_FRAME_SIZE,&w,&h))&&w&&h){ vw=w; vh=h; } else { vw=g_nativeW; vh=g_nativeH; }
    rgb32=true; return S_OK; }
  HRESULT STDMETHODCALLTYPE SetCurrentPosition(REFGUID,REFPROPVARIANT pos) override {
    g_seeks++; g_seekPos.push_back(pos.vt==VT_I8?(long long)pos.hVal.QuadPart:-1); if(plan.seekFails) return E_FAIL;
    framesThisPass=0; justSeeked=true; return S_OK; }
  HRESULT STDMETHODCALLTYPE ReadSample(DWORD,DWORD,DWORD* actual,DWORD* sflags,LONGLONG* ts,IMFSample** out) override {
    g_reads++; if(g_reads>20000){ g_runaway=true; if(g_stopFn) g_stopFn(); }
    *out=nullptr; if(sflags) *sflags=0; if(ts) *ts=0; if(actual) *actual=0;
    if(plan.failAfterFrames>=0&&framesTotal>=plan.failAfterFrames) return E_FAIL;
    if(readFailsLeft>0){ readFailsLeft--; return E_FAIL; }
    if(plan.typeChangeAfter>=0&&framesTotal==plan.typeChangeAfter&&!typeChangeSent){ typeChangeSent=true; *sflags=MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED; return S_OK; }
    if(justSeeked&&plan.eosAfterSeek){ *sflags=MF_SOURCE_READERF_ENDOFSTREAM; return S_OK; }
    if(framesThisPass>=plan.frames){ *sflags=MF_SOURCE_READERF_ENDOFSTREAM; return S_OK; }
    IMFSample* smp=makeSample(framesTotal); if(!smp) return E_OUTOFMEMORY;
    *out=smp; if(ts) *ts=(LONGLONG)framesThisPass*333333LL; framesThisPass++; framesTotal++; justSeeked=false; g_delivered++;
    if(g_stopAfter>0&&g_delivered>=g_stopAfter&&g_stopFn) g_stopFn();
    return S_OK; }
  HRESULT STDMETHODCALLTYPE Flush(DWORD) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE GetServiceForStream(DWORD,REFGUID,REFIID,void**) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetPresentationAttribute(DWORD,REFGUID g,PROPVARIANT* a) override {
    if(g==MF_PD_DURATION){ a->vt=VT_UI8; a->uhVal.QuadPart=100000000ULL; return S_OK; } return MF_E_ATTRIBUTENOTFOUND; }

  LONG strideForType() const{
    switch(plan.layout){ case Plan::BOTTOMUP: case Plan::TWOD_BOTTOMUP: return -(LONG)(vw*4); case Plan::ZERO_STRIDE: return 0; default: return (LONG)(vw*4); } }
  IMFSample* makeSample(int f){
    IMFMediaBuffer* b=nullptr; const size_t row=(size_t)vw*4, full=row*vh;
    switch(plan.layout){
      case Plan::TWOD_TOPDOWN: case Plan::TWOD_BOTTOMUP: {
        if(FAILED(MFCreate2DMediaBuffer(vw,vh,22/*D3DFMT_X8R8G8B8*/,plan.layout==Plan::TWOD_BOTTOMUP,&b))||!b) return nullptr;
        IMF2DBuffer* b2=nullptr; BYTE* s0=nullptr; LONG pitch=0;
        if(FAILED(b->QueryInterface(IID_PPV_ARGS(&b2)))||!b2){ b->Release(); return nullptr; }
        if(FAILED(b2->Lock2D(&s0,&pitch))||!s0){ b2->Release(); b->Release(); return nullptr; }
        for(UINT32 y=0;y<vh;y++){ uint32_t* p=(uint32_t*)(s0+(ptrdiff_t)y*pitch); for(UINT32 x=0;x<vw;x++) p[x]=pat((int)y,f); }   // s0 = TOP row, pitch signed
        b2->Unlock2D(); b2->Release(); DWORD len=0; b->GetMaxLength(&len); b->SetCurrentLength(len); break; }
      case Plan::SHORT_BUFFER: {
        const size_t len=full-row;                           // one row short
        if(FAILED(MFCreateMemoryBuffer((DWORD)len,&b))||!b) return nullptr;
        BYTE* d=nullptr; DWORD ml=0,cl=0; if(SUCCEEDED(b->Lock(&d,&ml,&cl))){ memset(d,0x5A,len); b->Unlock(); } b->SetCurrentLength((DWORD)len); break; }
      case Plan::ZERO_STRIDE: {                              // a type that does not say its stride and a buffer far smaller than a picture
        if(FAILED(MFCreateMemoryBuffer(16,&b))||!b) return nullptr;
        BYTE* d=nullptr; DWORD ml=0,cl=0; if(SUCCEEDED(b->Lock(&d,&ml,&cl))){ memset(d,0x5A,16); b->Unlock(); } b->SetCurrentLength(16); break; }
      default: {                                             // TOPDOWN / BOTTOMUP flat buffers
        if(FAILED(MFCreateMemoryBuffer((DWORD)full,&b))||!b) return nullptr;
        BYTE* d=nullptr; DWORD ml=0,cl=0; if(FAILED(b->Lock(&d,&ml,&cl))||!d){ b->Release(); return nullptr; }
        for(UINT32 r=0;r<vh;r++){ const UINT32 y=(plan.layout==Plan::BOTTOMUP)?(vh-1-r):r; uint32_t* p=(uint32_t*)(d+(size_t)r*row); for(UINT32 x=0;x<vw;x++) p[x]=pat((int)y,f); }
        b->Unlock(); b->SetCurrentLength((DWORD)full); break; } }
    IMFSample* s=nullptr; if(FAILED(MFCreateSample(&s))||!s){ b->Release(); return nullptr; }
    s->AddBuffer(b); b->Release(); return s; }
};

static HRESULT WINAPI fakeCreateReader(LPCWSTR,IMFAttributes* at,IMFSourceReader** out){
  *out=nullptr;
  if(!at){ FakeReader* r=new FakeReader(Plan(),true); *out=r; return S_OK; }              // the size probe
  int kind=0; UINT32 v=0;
  if(SUCCEEDED(at->GetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING,&v))&&v) kind=1;
  else if(SUCCEEDED(at->GetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING,&v))&&v) kind=2;
  g_openKinds.push_back(kind);
  const int n=g_opens++; if(g_opens>5000){ g_runaway=true; if(g_stopFn) g_stopFn(); }
  const Plan p=(n<(int)g_plans.size())?g_plans[(size_t)n]:g_planAfter;
  if(g_onOpen) g_onOpen();
  if(p.openFails) return MF_E_UNSUPPORTED_BYTESTREAM_TYPE;
  *out=new FakeReader(p,false); return S_OK; }

// the code under test, with its source reader and its Sleep replaced
#define MFCreateSourceReaderFromURL fakeCreateReader
#define Sleep fakeSleep
#ifndef CPU_BACKEND_SRC
#define CPU_BACKEND_SRC "../afk/afk_cpu_backend.cpp"
#endif
#include CPU_BACKEND_SRC
#undef Sleep
#undef MFCreateSourceReaderFromURL

// ------------------------------------------------------------------------------------------------ the checks
static int g_checks=0, g_failed=0;
#define CHECK(c) do{ g_checks++; if(!(c)){ g_failed++; printf("  FAILED line %d: %s\n",__LINE__,#c); } }while(0)
static const char* g_scn="";
static void scenario(const char* n){ g_scn=n; printf("[%s]\n",n); }

static bool logHas(const wchar_t* part){ for(size_t i=0;i<g_log.size();i++) if(g_log[i].find(part)!=std::wstring::npos) return true; return false; }

// A fresh Shared for one run of videoMain(); everything the fakes count is reset.
static std::unique_ptr<Shared> begin(UINT32 tw,UINT32 th,bool run){
  g_plans.clear(); g_planAfter=Plan(); g_opens=g_reads=g_seeks=g_delivered=g_stopAfter=0; g_sleptMs=0; g_sleeps=0; g_sleep50=0; g_openKinds.clear(); g_seekPos.clear(); g_sleepLog.clear();
  g_runaway=false; g_log.clear(); g_onSleep=nullptr; g_onOpen=nullptr; g_nativeW=tw; g_nativeH=th;
  std::unique_ptr<Shared> S(new Shared()); S->path=L"fake.mp4"; S->targetW=tw; S->targetH=th; S->run.store(run); S->workers=0;
  Shared* raw=S.get(); g_stopFn=[raw](){ raw->stop.store(true); }; return S; }

static inline uint8_t rowB(int y){ return (uint8_t)((3*y+1)&0xFF); }     // the blue channel the fake writes into image row y

// the newest published frame's row-0 / last-row blue channel and green (frame number), or -1
static bool newest(Shared& S,uint8_t& b0,uint8_t& bLast,uint8_t& g0,uint32_t& w,uint32_t& h){
  CpuFrame *c=nullptr,*p=nullptr; int64_t ct=0,pt=0; S.vx.acquireForDraw(c,p,ct,pt); bool ok=false;
  if(c&&c->px){ b0=c->px[0]; g0=c->px[1]; bLast=c->px[(size_t)(c->h-1)*c->w*4]; w=c->w; h=c->h; ok=(c->px[3]==0xFF); }
  S.vx.releaseAfterDraw(); return ok; }

int main(){
  CoInitializeEx(nullptr,COINIT_MULTITHREADED);
  if(FAILED(MFStartup(MF_VERSION))){ printf("MFStartup failed\n"); return 2; }
  uint8_t b0=0,bl=0,g0=0; uint32_t w=0,h=0;

  scenario("normal clip: preload one frame, then play on and loop with a seek to 0");
  { auto S=begin(1920,1080,false);                                    // a 1080p clip in a 1080p window: configurations {0,1}
    Plan a; a.frames=4; g_plans.push_back(a); g_stopAfter=11;
    int phase=0; g_onSleep=[&](){ if(S->state.load()==1&&phase==0){ phase=1; S->run.store(true); } };       // the screen "appears" once the preload frame exists
    videoMain(*S);
    CHECK(!g_runaway); CHECK(S->state.load()==1); CHECK(g_opens==1); CHECK(g_openKinds.size()==1&&g_openKinds[0]==1);      // configuration #0 first
    CHECK(S->frames.load()>=11); CHECK(g_seeks>=2); for(size_t i=0;i<g_seekPos.size();i++) CHECK(g_seekPos[i]==0);          // a clean loop: always back to 0
    CHECK(newest(*S,b0,bl,g0,w,h)); CHECK(w==1920&&h==1080); CHECK(b0==rowB(0)); CHECK(bl==rowB(1079)); }

  scenario("bottom-up flat buffer (negative default stride) comes out the right way up");
  { auto S=begin(1920,1080,true); Plan a; a.layout=Plan::BOTTOMUP; a.frames=3; g_plans.push_back(a); g_stopAfter=3; videoMain(*S);
    CHECK(newest(*S,b0,bl,g0,w,h)); CHECK(b0==rowB(0)); CHECK(bl==rowB(1079)); }

  scenario("2-D buffers: top-down and bottom-up (the buffer itself says where its top row is)");
  { for(int bu=0;bu<2;bu++){
      auto S=begin(1920,1080,true); Plan a; a.layout=bu?Plan::TWOD_BOTTOMUP:Plan::TWOD_TOPDOWN; a.frames=3; g_plans.push_back(a); g_stopAfter=3; videoMain(*S);
      const bool ok=newest(*S,b0,bl,g0,w,h);
      if(!ok&&S->frames.load()==0) printf("  (this system could not create / lock a 2-D media buffer -- skipped)\n");
      else{ CHECK(b0==rowB(0)); CHECK(bl==rowB(1079)); } } }

  scenario("unusable buffers on the first configuration: 30 rejections, then the NEXT configuration is tried");
  { auto S=begin(1920,1080,true);
    Plan bad; bad.layout=Plan::ZERO_STRIDE; bad.frames=100000; g_plans.push_back(bad); Plan ok; ok.frames=4; g_plans.push_back(ok); g_stopAfter=40;
    videoMain(*S);
    CHECK(!g_runaway); CHECK(g_opens==2); CHECK(g_openKinds.size()==2&&g_openKinds[0]==1&&g_openKinds[1]==2);              // configuration #0, then #1
    CHECK(S->state.load()==1); CHECK(S->frames.load()>=10); CHECK(logHas(L"30 decoded frames in a row were unusable")); }

  scenario("a buffer one row short is refused, never read past -- and a clip that only produces such buffers ends in state 2");
  { auto S=begin(1920,1080,true); Plan bad; bad.layout=Plan::SHORT_BUFFER; bad.frames=100000; g_planAfter=bad;
    videoMain(*S);
    CHECK(!g_runaway); CHECK(S->frames.load()==0); CHECK(S->state.load()==2); CHECK(!S->why.empty()); CHECK(logHas(L"unusable")); CHECK(g_opens==2); CHECK(g_reads<=2*31+4); }

  scenario("every configuration opens but gives no picture (zero stride): state 2, no spinning");
  { auto S=begin(1920,1080,true); Plan bad; bad.layout=Plan::ZERO_STRIDE; bad.frames=100000; g_planAfter=bad;
    videoMain(*S); CHECK(!g_runaway); CHECK(S->state.load()==2); CHECK(S->frames.load()==0); CHECK(g_opens==2); CHECK(g_reads<=2*31+4); }

  scenario("nothing can be opened at all: three rounds, then state 2 after 2 x 500 ms");
  { auto S=begin(1920,1080,true); Plan nf; nf.openFails=true; g_planAfter=nf;
    videoMain(*S); CHECK(!g_runaway); CHECK(S->state.load()==2); CHECK(g_opens==6); CHECK(g_sleptMs==1000);
    CHECK(S->why.find(L"no Media Foundation decode configuration could open")!=std::wstring::npos); }       // (the back-end logs this reason when it picks the state up)

  scenario("an oversize clip tries the window-size configuration first, then the others");
  { auto S=begin(640,360,true); g_nativeW=1280; g_nativeH=720;
    Plan nf; nf.openFails=true; g_plans.push_back(nf); g_plans.push_back(nf); Plan ok; ok.frames=4; g_plans.push_back(ok); g_stopAfter=6; videoMain(*S);
    CHECK(g_openKinds.size()>=3&&g_openKinds[0]==2&&g_openKinds[1]==1&&g_openKinds[2]==2); CHECK(S->state.load()==1);
    CHECK(newest(*S,b0,bl,g0,w,h)); CHECK(w==1280&&h==720); }          // the third open is configuration #1: native size

  scenario("the window-size configuration works: the picture comes at the requested size");
  { auto S=begin(640,360,true); g_nativeW=1280; g_nativeH=720; Plan ok; ok.frames=4; g_plans.push_back(ok); g_stopAfter=4; videoMain(*S);
    CHECK(g_openKinds.size()>=1&&g_openKinds[0]==2); CHECK(newest(*S,b0,bl,g0,w,h)); CHECK(w==640&&h==360); CHECK(b0==rowB(0)); CHECK(bl==rowB(359)); }

  scenario("read errors in the middle of a run: a few are ridden out, a persistent one reopens the SAME configuration");
  { auto S=begin(1920,1080,true);
    Plan a; a.readFailsFirst=3; a.failAfterFrames=6; a.frames=100000; g_plans.push_back(a); Plan b; b.frames=100000; g_plans.push_back(b); g_stopAfter=14; videoMain(*S);
    CHECK(!g_runaway); CHECK(g_opens==2); CHECK(g_openKinds.size()==2&&g_openKinds[0]==1&&g_openKinds[1]==1);              // 3 transient errors ridden out; 6 frames; then configuration #0 again
    CHECK(S->state.load()==1); CHECK(S->frames.load()>=14); CHECK(logHas(L"reopening the clip")); }

  scenario("the loop restart fails (SetCurrentPosition error): reopen instead of freezing on the last frame");
  { auto S=begin(1920,1080,true); Plan a; a.frames=3; a.seekFails=true; g_plans.push_back(a); Plan b; b.frames=3; g_plans.push_back(b); g_stopAfter=7; videoMain(*S);
    CHECK(!g_runaway); CHECK(g_opens>=2); CHECK(logHas(L"the loop restart failed")); CHECK(S->frames.load()>=7); }

  scenario("end of stream again right after the restart: reopen");
  { auto S=begin(1920,1080,true); Plan a; a.frames=3; a.eosAfterSeek=true; g_plans.push_back(a); Plan b; b.frames=3; g_plans.push_back(b); g_stopAfter=8; videoMain(*S);
    CHECK(!g_runaway); CHECK(g_opens>=2); CHECK(logHas(L"end of stream again right after the loop restart")); }

  scenario("the decoder changes its output format mid-stream: reopen (the size and stride assumed so far are void)");
  { auto S=begin(1920,1080,true); Plan a; a.frames=100000; a.typeChangeAfter=2; g_plans.push_back(a); Plan b; b.frames=100000; g_plans.push_back(b); g_stopAfter=8; videoMain(*S);
    CHECK(!g_runaway); CHECK(g_opens>=2); CHECK(logHas(L"changed its output format")); CHECK(S->frames.load()>=8); }

  scenario("the clip becomes unreadable after the first pictures: the last picture stays, the worker keeps trying -- slowly -- and never gives up");
  { auto S=begin(1920,1080,true);
    Plan a; a.frames=100000; a.failAfterFrames=3; g_plans.push_back(a); Plan nf; nf.openFails=true; g_planAfter=nf;
    const int stopAtOpen=14; g_onOpen=[&](){ if(g_opens>=stopAtOpen) S->stop.store(true); };
    videoMain(*S);
    CHECK(!g_runaway); CHECK(S->state.load()==1); CHECK(S->frames.load()==3); CHECK(g_opens>=stopAtOpen);                      // still alive when asked to stop
    CHECK(logHas(L"the clip cannot be reopened")); CHECK(logHas(L"the last picture stays on screen"));
    CHECK(logHas(L"trying again in 5 s")); CHECK(logHas(L"trying again in 15 s")); CHECK(logHas(L"trying again in 30 s"));   // the waits grow
    CHECK(g_sleep50>2*100+3*300+100); CHECK(newest(*S,b0,bl,g0,w,h)); }                                                        // the picture is still there to be shown

  scenario("a stop request in the middle of a long back-off is honoured within one 50 ms step");
  { auto S=begin(1920,1080,true); Plan a; a.frames=100000; a.failAfterFrames=2; g_plans.push_back(a); Plan nf; nf.openFails=true; g_planAfter=nf;
    int sl=0; bool armed=false; g_onOpen=[&](){ if(g_opens>=3) armed=true; };
    g_onSleep=[&](){ if(armed&&++sl==3) S->stop.store(true); };
    videoMain(*S); CHECK(!g_runaway); CHECK(sl>=3&&sl<=4); }

  MFShutdown(); CoUninitialize();
  printf("\n%d checks, %d failed\n",g_checks,g_failed);
  return g_failed?1:0;
}
