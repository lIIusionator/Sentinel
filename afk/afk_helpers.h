// afk_helpers.h -- the AFK screen's pure logic: timing, fades, pacing, geometry, tier ladder, watchdog.
//
// Deliberately free of Windows headers so tests/afk_tests.cpp can compile and run all of it natively
// (including under ASAN / UBSAN / TSAN). Both back-ends (GPU player + Direct2D overlay, and the GDI+
// fallback) draw from the same numbers, so the two paths look the same.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <math.h>
#include <vector>
#include <string>
#if defined(__SSE2__) || defined(_M_X64)
#include <emmintrin.h>
#endif

// ======================================================================================
// Frame copy / content probe (CPU back-end)
// ======================================================================================

// 32bpp XRGB/ARGB -> opaque premultiplied ARGB (alpha forced to 0xFF). srcTop is the visually-first
// row; strides are in bytes and may be negative (bottom-up DIB). Replaces a GDI+ DrawImage
// conversion of a whole frame, which cost far more than the copy itself.
static inline void afkCopyOpaque32(const uint8_t* srcTop,int32_t srcStride,uint8_t* dstTop,int32_t dstStride,uint32_t w,uint32_t h){
#if defined(__SSE2__) || defined(_M_X64)
  const __m128i a=_mm_set1_epi32((int)0xFF000000u);
#endif
  for(uint32_t y=0;y<h;y++){
    const uint8_t* s=srcTop+(ptrdiff_t)y*srcStride; uint8_t* d=dstTop+(ptrdiff_t)y*dstStride;
    uint32_t x=0;
#if defined(__SSE2__) || defined(_M_X64)
    for(;x+4<=w;x+=4){ __m128i v=_mm_loadu_si128((const __m128i*)(s+(size_t)x*4)); _mm_storeu_si128((__m128i*)(d+(size_t)x*4),_mm_or_si128(v,a)); }
#endif
    for(;x<w;x++){ uint32_t p; memcpy(&p,s+(size_t)x*4,4); p|=0xFF000000u; memcpy(d+(size_t)x*4,&p,4); }
  }
}

// Does a picture of vw x vh 32-bit pixels, its rows `stride` bytes apart (negative: bottom-up), fit inside a buffer of `bufLen` bytes?
// The buffer starts at the first row IN MEMORY (the last image row when the stride is negative). afkCopyOpaque32 reads vw*4 bytes per row, so the
// stride must be at least that wide -- a stride of 0 (a media type that does not say) would otherwise read vw*4 bytes from a buffer of any length.
static inline bool afkFrameLayoutOk(int64_t stride,uint32_t vw,uint32_t vh,uint64_t bufLen){
  if(!vw||!vh||vw>32768u||vh>32768u) return false;
  const uint64_t row=(uint64_t)vw*4u;
  const uint64_t ap=stride<0?(uint64_t)0-(uint64_t)stride:(uint64_t)stride;      // well defined even for INT64_MIN
  if(ap<row||row>bufLen) return false;
  if(vh>1u&&ap>(bufLen-row)/(uint64_t)(vh-1u)) return false;                      // (vh-1)*ap+row <= bufLen, without letting the product overflow
  return true;
}

// True if a coarse grid of pixels shows real picture content (not all black / all identical).
static inline bool afkHasContent(const uint8_t* bgra,uint32_t w,uint32_t h){
  if(!w||!h) return false; uint32_t mn=255*3,mx=0;
  for(uint32_t gy=0;gy<16;gy++) for(uint32_t gx=0;gx<16;gx++){
    uint32_t x=(uint32_t)(((uint64_t)gx*(w-1))/15), y=(uint32_t)(((uint64_t)gy*(h-1))/15);
    const uint8_t* p=bgra+((size_t)y*w+x)*4; uint32_t l=(uint32_t)p[0]+p[1]+p[2]; if(l<mn)mn=l; if(l>mx)mx=l; }
  return mx>48 && mx>mn+8;
}

// ======================================================================================
// Frame pacing (CPU back-end)
// ======================================================================================

// Private-clock pacer. Maps a frame's timestamp to a wall-clock due time. If frames run late it
// SLIPS the timeline (shows the frame now, re-anchors) instead of dropping or seeking, so the
// picture always keeps moving -- at decode speed if that is all the machine can manage. Deliberately
// NOT slaved to the audio clock: that coupling had nasty edge cases (stale clock at session start,
// audio loop vs video end-of-stream ordering) and ambient background video doesn't need lip-sync.
struct AfkPacer{
  static const int64_t SLIP=2000000, MAXEARLY=10000000;   // 200ms late -> slip; 1s early -> re-anchor
  bool init=false; int64_t tsBase=0,wallBase=0;
  void reset(){ init=false; }
  int64_t schedule(int64_t ts,int64_t wallNow,bool* slipped=nullptr){
    if(slipped)*slipped=false;
    if(!init){ init=true; tsBase=ts; wallBase=wallNow; return 0; }
    int64_t due=wallBase+(ts-tsBase), delta=due-wallNow;
    if(delta<-SLIP){ wallBase+=(wallNow-due); if(slipped)*slipped=true; return 0; }
    if(delta>MAXEARLY){ wallBase-=delta; return 0; }
    return delta>0?delta:0;
  }
};

// Fastest measured decode config wins; among those within 10% of the fastest the EARLIEST wins (the
// table is ordered by preference). Returns -1 when nothing produced frames.
static inline int afkPickBest(const double* fps,const bool* ok,int n){
  double mx=0; for(int i=0;i<n;i++) if(ok[i]&&fps[i]>mx) mx=fps[i];
  if(mx<=0) return -1;
  for(int i=0;i<n;i++) if(ok[i]&&fps[i]>=mx*0.9) return i;
  return -1;
}

// Render-tick delay for the CPU renderer from its measured average frame cost: 1.3x the cost,
// clamped to 33..66 ms (30 fps ceiling, 15 fps floor).
static inline int afkCpuTickMs(double avgCostMs){ int w=(int)(avgCostMs*1.3+0.5); return w<33?33:(w>66?66:w); }

// The CPU renderer's frames are due on an ABSOLUTE grid: the next one is due one tick after the previous one was DUE, not one tick after it was
// finished, so a late wake-up or a slightly slow frame does not shift every later frame (that is what made 30 fps look uneven). Times in any one unit.
// `due` is when the frame that was just drawn was due (0 = none yet). A grid more than one tick behind (a stall, a pause, a mode switch) is restarted
// from `now` instead of being caught up with a burst of frames. Returns when the NEXT frame is due.
static inline int64_t afkGridNext(int64_t due,int64_t now,int64_t tick){
  if(due<=0||tick<=0) return now+(tick>0?tick:0);
  int64_t n=due+tick;
  if(n<now-tick) n=now;
  return n;
}

// ======================================================================================
// Vignette (camera-lens falloff)
// ======================================================================================

// 0 inside the focus circle, smoothstep up to 1 at outerR.
static inline float afkVignette01(float r,float focusR,float outerR){
  if(r<=focusR) return 0.f; float t=(r-focusR)/(outerR-focusR); if(t>1.f)t=1.f; return t*t*(3.f-2.f*t);
}
// CPU back-end: fills W*H premultiplied-ARGB pixels: black with alpha = maxAlpha01 * profile.
static inline void afkVignetteFill(uint8_t* px,int32_t stride,int W,int H,float maxAlpha01){
  float cx=(W-1)*0.5f, cy=(H-1)*0.5f; float m=(float)(W<H?W:H);
  float focusR=m*0.30f, outerR=sqrtf(cx*cx+cy*cy)*1.06f;
  for(int y=0;y<H;y++){ uint32_t* row=(uint32_t*)(px+(ptrdiff_t)y*stride); float dy=y-cy;
    for(int x=0;x<W;x++){ float dx=x-cx; float p=afkVignette01(sqrtf(dx*dx+dy*dy),focusR,outerR);
      uint32_t a=(uint32_t)(maxAlpha01*p*255.f+0.5f); if(a>255)a=255; row[x]=a<<24; } }
}
// GPU back-end: the same profile as stops of a radial gradient (centre (W-1)/2,(H-1)/2, radius = *outerR).
// Stop 0 is the centre, stop 1 the edge of the clear focus circle, the rest sample the smoothstep up to 1.
struct AfkVigStop{ float pos; float alpha; };
static inline int afkVignetteStops(int W,int H,float maxAlpha01,AfkVigStop* out,int n,float* outerROut=nullptr){
  float cx=(W-1)*0.5f, cy=(H-1)*0.5f, m=(float)(W<H?W:H); float focusR=m*0.30f, outerR=sqrtf(cx*cx+cy*cy)*1.06f;
  if(outerROut) *outerROut=outerR;
  if(n<4||outerR<=focusR) return 0;
  float p1=focusR/outerR; out[0].pos=0.f; out[0].alpha=0.f; out[1].pos=p1; out[1].alpha=0.f;
  for(int i=2;i<n;i++){ float t=(float)(i-1)/(float)(n-2); float pos=p1+(1.f-p1)*t;
    out[i].pos=pos; out[i].alpha=maxAlpha01*afkVignette01(pos*outerR,focusR,outerR); }
  return n;
}

// ======================================================================================
// CPU cross-fade for sparse frames and loop resync
// ======================================================================================

// At native frame rate the gap between published frames is small and every render tick already falls
// on-or-past the newest frame, so this returns 1.0 (opaque, zero extra blend cost) almost every time --
// it only produces a fractional blend once decode is slow enough that a hard cut would be visible.
static inline float afkBlendT(int64_t now,int64_t curT,int64_t prevT,int64_t minGapToBlend){
  int64_t gap=curT-prevT;
  if(gap<=minGapToBlend||gap<=0) return 1.0f;                 // native-rate path: no blending, no cost
  int64_t since=now-curT; if(since<=0) return 0.0f;           // just published: start of the cross-fade
  float t=(float)since/(float)gap; if(t>1.0f)t=1.0f; if(t<0.0f)t=0.0f; return t;
}

// ======================================================================================
// Lock-light frame hand-off (CPU back-end): decode thread -> render thread
// ======================================================================================

// The decode thread publishes frames; the render thread draws the two most recent (for cross-fading)
// without ever holding the lock while drawing, so the decoder is never stalled by a slow draw. "In use"
// is tracked by pointer identity, not by curr/prev role. A published frame is only freed/pooled after
// nothing outstanding references it. Finished frames are pooled and reused, avoiding a large allocation
// (~15MB at 2560x1440 RGB32) per video frame.
template<class Frame,class Lock> struct AfkFrameExchange{
  Lock lk; Frame* curr=nullptr; Frame* prev=nullptr; int64_t currT=0,prevT=0;
  Frame* inUseA=nullptr; Frame* inUseB=nullptr; std::vector<Frame*> deferred; std::vector<Frame*> pool;
  static const size_t POOL_MAX=3;
  struct G{ Lock& l; G(Lock& x):l(x){l.lock();} ~G(){l.unlock();} };
  bool inUse(Frame* f){ return f&&(f==inUseA||f==inUseB); }
  void recycleLocked(Frame* f){ if(!f) return; if(pool.size()<POOL_MAX) pool.push_back(f); else delete f; }
  // Returns the current frame and, if one exists, the previous frame too, along with both publish
  // timestamps so the caller can compute the blend factor itself. Both pointers stay valid (never
  // freed/reused) until releaseAfterDraw().
  void acquireForDraw(Frame*& c,Frame*& p,int64_t& ct,int64_t& pt){ G g(lk); inUseA=curr; inUseB=prev; c=curr; p=prev; ct=currT; pt=prevT; }
  void releaseAfterDraw(){ G g(lk); inUseA=nullptr; inUseB=nullptr;
    for(size_t i=0;i<deferred.size();i++) recycleLocked(deferred[i]); deferred.clear(); }
  template<class Maker> Frame* obtain(uint32_t w,uint32_t h,Maker mk){
    Frame* f=nullptr;
    { G g(lk); for(size_t i=0;i<pool.size();){ Frame* c=pool[i]; pool.erase(pool.begin()+i);
        if((uint32_t)c->GetWidth()==w&&(uint32_t)c->GetHeight()==h){ f=c; break; } delete c; } }
    return f?f:mk(w,h);
  }
  // nowT: the decode thread's own monotonic clock, used only for the cross-fade factor.
  void publish(Frame* nf,int64_t nowT=0){ G g(lk);
    Frame* retiring=prev;                 // about to fall out of the 2-deep window entirely
    prev=curr; prevT=currT; curr=nf; currT=nowT;
    if(retiring){ if(inUse(retiring)) deferred.push_back(retiring); else recycleLocked(retiring); }
  }
  void clear(){ G g(lk); Frame* a=curr; Frame* b=prev; curr=prev=nullptr; currT=prevT=0;
    if(a){ if(inUse(a)) deferred.push_back(a); else recycleLocked(a); }
    if(b && b!=a){ if(inUse(b)) deferred.push_back(b); else recycleLocked(b); } }
  void shutdown(){ G g(lk); if(curr){delete curr;curr=nullptr;} if(prev){delete prev;prev=nullptr;}
    for(size_t i=0;i<deferred.size();i++) delete deferred[i]; deferred.clear();
    for(size_t i=0;i<pool.size();i++) delete pool[i]; pool.clear(); }
};

// ======================================================================================
// CPU back-end: video + vignette composed in ONE pass (replaces two full-screen GDI+ draws)
// ======================================================================================
// A "gain map" holds one 32-bit pixel per screen pixel: B = G = R = 255 - vignetteAlpha, A = 255. Multiplying a frame by
// it is exactly "black with that alpha drawn over the frame", but costs one SSE2 pass instead of a GDI+ alpha blend.

static inline uint8_t afkMul255(uint32_t s,uint32_t g){ uint32_t t=s*g+128u; return (uint8_t)((t+(t>>8))>>8); }     // round(s*g/255), exact

// px: packed W*H 32-bit pixels (stride W*4).
static inline void afkGainMapFromVignette(uint32_t* px,int W,int H,float maxAlpha01){
  if(W<=0||H<=0) return;
  afkVignetteFill((uint8_t*)px,W*4,W,H,maxAlpha01);                    // pixel = alpha<<24 ...
  size_t n=(size_t)W*(size_t)H; for(size_t i=0;i<n;i++){ uint32_t g=255u-(px[i]>>24); px[i]=0xFF000000u|(g<<16)|(g<<8)|g; }
}

// dst = src * gain / 255 per colour channel, alpha forced to 255. Strides in bytes (may be negative for src / gain).
static inline void afkMulGainOpaque32(const uint8_t* src,int32_t srcStride,const uint8_t* gain,int32_t gainStride,uint8_t* dst,int32_t dstStride,uint32_t w,uint32_t h){
#if defined(__SSE2__) || defined(_M_X64)
  const __m128i zero=_mm_setzero_si128(), c128=_mm_set1_epi16(128), alpha=_mm_set1_epi32((int)0xFF000000u);
#endif
  for(uint32_t y=0;y<h;y++){
    const uint8_t* s=src+(ptrdiff_t)y*srcStride; const uint8_t* g=gain+(ptrdiff_t)y*gainStride; uint8_t* d=dst+(ptrdiff_t)y*dstStride; uint32_t x=0;
#if defined(__SSE2__) || defined(_M_X64)
    for(;x+4<=w;x+=4){
      __m128i sv=_mm_loadu_si128((const __m128i*)(s+(size_t)x*4)), gv=_mm_loadu_si128((const __m128i*)(g+(size_t)x*4));
      __m128i tl=_mm_add_epi16(_mm_mullo_epi16(_mm_unpacklo_epi8(sv,zero),_mm_unpacklo_epi8(gv,zero)),c128);
      __m128i th=_mm_add_epi16(_mm_mullo_epi16(_mm_unpackhi_epi8(sv,zero),_mm_unpackhi_epi8(gv,zero)),c128);
      tl=_mm_srli_epi16(_mm_add_epi16(tl,_mm_srli_epi16(tl,8)),8); th=_mm_srli_epi16(_mm_add_epi16(th,_mm_srli_epi16(th,8)),8);
      _mm_storeu_si128((__m128i*)(d+(size_t)x*4),_mm_or_si128(_mm_packus_epi16(tl,th),alpha)); }
#endif
    for(;x<w;x++){ const uint8_t* sp=s+(size_t)x*4; const uint8_t* gp=g+(size_t)x*4; uint8_t* dp=d+(size_t)x*4;
      dp[0]=afkMul255(sp[0],gp[0]); dp[1]=afkMul255(sp[1],gp[1]); dp[2]=afkMul255(sp[2],gp[2]); dp[3]=0xFF; }
  }
}

// dst = lerp(a, b, t128/128) * gain / 255 -- the cross-fade between two published frames plus the vignette, in one pass.
// a and b have the same size and stride. t128 is clamped to 0..128.
static inline void afkBlendGainOpaque32(const uint8_t* a,const uint8_t* b,int32_t abStride,int t128,const uint8_t* gain,int32_t gainStride,uint8_t* dst,int32_t dstStride,uint32_t w,uint32_t h){
  if(t128<0) t128=0; if(t128>128) t128=128;
#if defined(__SSE2__) || defined(_M_X64)
  const __m128i zero=_mm_setzero_si128(), c128=_mm_set1_epi16(128), alpha=_mm_set1_epi32((int)0xFF000000u), tv=_mm_set1_epi16((short)t128);
#endif
  for(uint32_t y=0;y<h;y++){
    const uint8_t* pa=a+(ptrdiff_t)y*abStride; const uint8_t* pb=b+(ptrdiff_t)y*abStride; const uint8_t* g=gain+(ptrdiff_t)y*gainStride; uint8_t* d=dst+(ptrdiff_t)y*dstStride; uint32_t x=0;
#if defined(__SSE2__) || defined(_M_X64)
    for(;x+4<=w;x+=4){
      __m128i av=_mm_loadu_si128((const __m128i*)(pa+(size_t)x*4)), bv=_mm_loadu_si128((const __m128i*)(pb+(size_t)x*4)), gv=_mm_loadu_si128((const __m128i*)(g+(size_t)x*4));
      __m128i al=_mm_unpacklo_epi8(av,zero), ah=_mm_unpackhi_epi8(av,zero), bl=_mm_unpacklo_epi8(bv,zero), bh=_mm_unpackhi_epi8(bv,zero);
      __m128i ml=_mm_add_epi16(al,_mm_srai_epi16(_mm_mullo_epi16(_mm_sub_epi16(bl,al),tv),7));       // a + (b-a)*t/128 (arithmetic shift: floor)
      __m128i mh=_mm_add_epi16(ah,_mm_srai_epi16(_mm_mullo_epi16(_mm_sub_epi16(bh,ah),tv),7));
      __m128i tl=_mm_add_epi16(_mm_mullo_epi16(ml,_mm_unpacklo_epi8(gv,zero)),c128), th=_mm_add_epi16(_mm_mullo_epi16(mh,_mm_unpackhi_epi8(gv,zero)),c128);
      tl=_mm_srli_epi16(_mm_add_epi16(tl,_mm_srli_epi16(tl,8)),8); th=_mm_srli_epi16(_mm_add_epi16(th,_mm_srli_epi16(th,8)),8);
      _mm_storeu_si128((__m128i*)(d+(size_t)x*4),_mm_or_si128(_mm_packus_epi16(tl,th),alpha)); }
#endif
    for(;x<w;x++){ const uint8_t* ap=pa+(size_t)x*4; const uint8_t* bp=pb+(size_t)x*4; const uint8_t* gp=g+(size_t)x*4; uint8_t* dp=d+(size_t)x*4;
      for(int c=0;c<3;c++){ int m=(int)ap[c]+((((int)bp[c]-(int)ap[c])*t128)>>7); dp[c]=afkMul255((uint32_t)m,gp[c]); }
      dp[3]=0xFF; }
  }
}

// Solid fill of a w*h block with one 32-bit pixel (stride in bytes).
static inline void afkFill32(uint8_t* dst,int32_t stride,uint32_t w,uint32_t h,uint32_t px){
  for(uint32_t y=0;y<h;y++){ uint32_t* r=(uint32_t*)(dst+(ptrdiff_t)y*stride); for(uint32_t x=0;x<w;x++) r[x]=px; } }

// ======================================================================================
// Fades and the staged reveal (shared by both back-ends)
// ======================================================================================

// Staged reveal after the screen goes dark (offsets from the moment AFK began, each phase's own fade-in):
//   0 ..  500 ms  screen to black (the global fade `af`)
// 500 .. 1300 ms  starfield     1300 .. 2100 ms  video     2100 .. 2800 ms  clock / quote / hint
// Once AFK is ending (afkNow false) everything snaps to fully revealed and the exit is one uniform
// fade-out driven by `af` alone.
static const uint64_t AFK_STAR_START=500,  AFK_STAR_DUR=800;
static const uint64_t AFK_VID_START=1300,  AFK_VID_DUR=800;
static const uint64_t AFK_TXT_START=2100,  AFK_TXT_DUR=700;
static const double   AFK_FADE_MS=500.0, AFK_QUOTE_FADE_MS=380.0;

static inline float afkStage(bool afkNow,uint64_t sinceEnterMs,uint64_t startMs,uint64_t durMs){
  if(!afkNow) return 1.0f;
  if(sinceEnterMs<=startMs) return 0.0f;
  uint64_t el=sinceEnterMs-startMs;
  if(durMs==0||el>=durMs) return 1.0f;
  return (float)el/(float)durMs;
}
// Global fade (0..1): a full sweep takes AFK_FADE_MS regardless of frame rate. dtMs is clamped so a
// thread that slept (or a window that was occluded) cannot jump the whole fade in one step.
static inline float afkFadeStep(float a,bool target,double dtMs){
  if(dtMs<0) dtMs=0; if(dtMs>250) dtMs=250;
  float st=(float)(dtMs/AFK_FADE_MS), tgt=target?1.0f:0.0f;
  if(a<tgt){ a+=st; if(a>tgt)a=tgt; } else if(a>tgt){ a-=st; if(a<tgt)a=tgt; }
  return a;
}
// Quote cross-fade: phase 0 stable, 1 fading out, 2 fading in (see AfkSnapshot).
static inline float afkQuoteAlpha(int phase,uint64_t nowMs,uint64_t phaseT0){
  if(phase!=1&&phase!=2) return 1.0f;
  double t=(nowMs>=phaseT0)?(double)(nowMs-phaseT0)/AFK_QUOTE_FADE_MS:0.0; if(t>1.0)t=1.0;
  return (float)(phase==1?1.0-t:t);
}
// Eases the video in from the moment its first frame exists, so it never pops in late.
static inline float afkVideoReadyFade(int64_t sinceFirstFrameMs){
  if(sinceFirstFrameMs<=0) return 0.0f; if(sinceFirstFrameMs>=400) return 1.0f; return (float)sinceFirstFrameMs/400.0f;
}
// The clip is revealed on the usual schedule, but never before the video check (AfkVideoGate) has seen it move: when that took longer than AFK_VID_START,
// the whole reveal is shifted by the difference (holdMs) so that it still fades in from nothing instead of jumping in half-way.
static inline uint64_t afkVideoHold(uint64_t gatePassedAtMs){ return gatePassedAtMs>AFK_VID_START?gatePassedAtMs-AFK_VID_START:0; }
static inline float afkVideoStage(bool afkNow,uint64_t sinceEnterMs,uint64_t holdMs){
  return afkStage(afkNow,sinceEnterMs>holdMs?sinceEnterMs-holdMs:0,AFK_VID_START,AFK_VID_DUR);
}

// ======================================================================================
// Starfield
// ======================================================================================

struct AfkRng{ uint32_t s=0x9E3779B9u;
  uint32_t next(){ s^=s<<13; s^=s>>17; s^=s<<5; return s; }
  float f01(){ return (float)(next()>>8)*(1.0f/16777216.0f); }
  int below(int n){ return n<=1?0:(int)(next()%(uint32_t)n); } };
struct AfkStar{ float x=0,y=0,vy=0; int a=0; };     // vy: pixels per 1/30 s at 1080p height; a: base alpha 70..239

static inline void afkStarsInit(std::vector<AfkStar>& v,int n,int W,int H,AfkRng& r){
  v.clear(); for(int i=0;i<n;i++){ AfkStar s; s.x=(float)r.below(W>0?W:1); s.y=(float)r.below(H>0?H:1);
    s.vy=0.10f+(float)r.below(40)/150.0f; s.a=70+r.below(170); v.push_back(s); } }
// Frame-rate independent drift (v13 moved a fixed distance per render call, which made the sky run at
// a speed set by the CPU). Stars re-enter at the bottom with a new x.
static inline void afkStarsStep(std::vector<AfkStar>& v,double dtSec,int W,int H,AfkRng& r){
  if(dtSec<0)dtSec=0; if(dtSec>0.25)dtSec=0.25;
  float k=(float)(dtSec*30.0)*(H>0?(float)H/1080.0f:1.0f);
  for(size_t i=0;i<v.size();i++){ AfkStar& s=v[i]; s.y-=s.vy*k; if(s.y<0){ s.y=(float)H; s.x=(float)r.below(W>0?W:1); } } }
// Stars are dimmed inside the lens focus circle (smoothstep, floor 6 %) and ramp up to full brightness
// outside it, where they fill in the vignette's dark edges.
static inline float afkStarCenterDim(float x,float y,int W,int H){
  float focusR=(float)(W<H?W:H)*0.30f; if(focusR<=0.f) return 1.0f;
  float dx=x-W*0.5f, dy=y-H*0.5f; float t=sqrtf(dx*dx+dy*dy)/focusR; if(t>1.f)t=1.f; if(t<0.f)t=0.f;
  float sh=t*t*(3.f-2.f*t); const float MINV=0.06f; return MINV+(1.0f-MINV)*sh; }

// ======================================================================================
// Geometry
// ======================================================================================

// The same fit as a destination rectangle {x,y,w,h} for a draw that scales the whole frame (may extend
// past W,H on the cropped axis).
static inline void afkCoverDest(float fw,float fh,float W,float H,float out[4]){
  float sc=(W/fw>H/fh)?(W/fw):(H/fh); float dw=fw*sc,dh=fh*sc; out[0]=(W-dw)/2.0f; out[1]=(H-dh)/2.0f; out[2]=dw; out[3]=dh; }

// Overlay layout, in physical pixels. s = UI scale (DPI scale, at least H/1080).
struct AfkLayout{ float gradY,clockY,quoteTop,quoteW,quoteAvail,hintY; };
static inline AfkLayout afkLayout(int W,int H,float s){
  AfkLayout L; L.gradY=H*0.42f; L.clockY=H*0.44f; L.quoteTop=H*0.58f; L.quoteW=W*0.70f; L.hintY=H-54.0f*s;
  L.quoteAvail=L.hintY-24.0f*s-L.quoteTop; return L; }
static inline float afkUiScale(float dpiScale,int H){ float r=(float)H/1080.0f; float s=dpiScale>r?dpiScale:r; return s<1.0f?1.0f:s; }
// Quote type ladder (regular size; bold runs are +4, line height 1.385x): largest step that fits wins.
static const float AFK_QUOTE_SIZES[6]={26.f,23.f,20.f,18.f,16.f,14.f};
static const int   AFK_QUOTE_STEPS=6;

// Present sync interval for a display refresh rate: 60 Hz -> 1, 120 -> 2, 144 -> 2, 240 -> 4 (about 60-75 fps).
static inline int afkSyncInterval(int hz){
  if(hz<30||hz>1000) return 1; int n=(int)floor(hz/60.0+0.4); return n<1?1:(n>4?4:n); }

// ======================================================================================
// Text helpers
// ======================================================================================

struct AfkTextRun{ size_t start,len; };
// "**bold**" markers and {user} -> plain text + bold ranges (in wchar_t units of `plain`).
static inline void afkParseQuote(const std::wstring& in,const std::wstring& user,std::wstring& plain,std::vector<AfkTextRun>& bold){
  std::wstring t=in; size_t from=0,up;
  while((up=t.find(L"{user}",from))!=std::wstring::npos){ t.replace(up,6,user); from=up+user.size(); }
  plain.clear(); bold.clear(); bool b=false; size_t bs=0;
  for(size_t i=0;i<t.size();){
    if(t.compare(i,2,L"**")==0){
      if(b){ if(plain.size()>bs){ AfkTextRun r={bs,plain.size()-bs}; bold.push_back(r); } b=false; } else { b=true; bs=plain.size(); }
      i+=2; }
    else plain+=t[i++]; }
  if(b&&plain.size()>bs){ AfkTextRun r={bs,plain.size()-bs}; bold.push_back(r); }
}
static inline void afkFmtClock(wchar_t out[9],int h,int m,int s){
  out[0]=(wchar_t)(L'0'+(h/10)%10); out[1]=(wchar_t)(L'0'+h%10); out[2]=L':';
  out[3]=(wchar_t)(L'0'+(m/10)%10); out[4]=(wchar_t)(L'0'+m%10); out[5]=L':';
  out[6]=(wchar_t)(L'0'+(s/10)%10); out[7]=(wchar_t)(L'0'+s%10); out[8]=0; }
// ======================================================================================
// Quality tiers and the watchdog
// ======================================================================================

// Tier ladder: 0 = 4K60 on the GPU, 1 = 1080p30 on the GPU, 2 = the GDI+ / software-decode renderer.
enum AfkTier{ AFK_TIER_GPU4K=0, AFK_TIER_GPULITE=1, AFK_TIER_CPU=2, AFK_TIER_NONE=3 };
// mode: 0 auto, 1 gpu-only, 2 cpu-only (same values as AfkModePref).
static inline int afkStartTier(int mode,int minTier,bool userClip){
  if(mode==2) return AFK_TIER_CPU;
  int t=minTier<0?0:(minTier>2?2:minTier);
  if(mode==1){ if(t>AFK_TIER_GPULITE) t=AFK_TIER_GPU4K; if(userClip) t=AFK_TIER_GPU4K; return t; }   // gpu-only ignores a stale CPU verdict
  if(userClip&&t==AFK_TIER_GPULITE) t=AFK_TIER_CPU;        // a clip the user dropped in has no lite variant
  return t;
}
static inline int afkNextTier(int cur,int mode,bool userClip){
  int n=cur+1; if(userClip&&n==AFK_TIER_GPULITE) n=AFK_TIER_CPU;
  if(mode==1&&n>AFK_TIER_GPULITE) return AFK_TIER_NONE;    // gpu-only never falls back to the CPU renderer
  if(mode==1&&userClip&&n>AFK_TIER_GPU4K) return AFK_TIER_NONE;
  return n>AFK_TIER_CPU?AFK_TIER_NONE:n;
}

// Judges playback health from the count of video frames actually delivered. Warm-up first (decoder start,
// first-frame latency), then fixed windows: a window below badFraction x clip fps is "bad"; badNeeded bad
// windows in a row means this tier cannot keep up on this machine. Reset whenever playback (re)starts.
struct AfkWatchdog{
  enum Verdict{ V_NONE=0, V_OK=1, V_BAD=2, V_DOWNGRADE=3 };
  int64_t warmupMs=3000, windowMs=2000; double badFraction=0.6; int badNeeded=2;
  bool started=false, warm=false; int64_t t0=0, winStart=0; uint64_t winFrames0=0; int badRun=0; double lastFps=0;
  void reset(){ started=false; warm=false; badRun=0; lastFps=0; }
  Verdict update(int64_t nowMs,uint64_t framesTotal,double clipFps){
    if(!started){ started=true; t0=nowMs; return V_NONE; }
    if(!warm){ if(nowMs-t0<warmupMs) return V_NONE; warm=true; winStart=nowMs; winFrames0=framesTotal; badRun=0; return V_NONE; }
    int64_t el=nowMs-winStart; if(el<windowMs) return V_NONE;
    double fps=(double)(framesTotal-winFrames0)*1000.0/(double)el; lastFps=fps;
    winStart=nowMs; winFrames0=framesTotal;
    double want=(clipFps>1.0?clipFps:30.0)*badFraction;
    if(fps<want){ badRun++; return badRun>=badNeeded?V_DOWNGRADE:V_BAD; }
    badRun=0; return V_OK; }
};

// Is the picture really moving? A player can load the clip, capture the first frame and still not play (Play() refused, a decoder that never starts, the audio
// device gone) without any error anywhere; only the count of NEW frames tells. Checked on every screen once its player is ready: the clip stays hidden until this
// passes (afkVideoStage), and a failure rebuilds the player (AfkRunner::videoCheckFailed). The clock starts at the first update after begin/reset.
struct AfkVideoGate{
  enum Verdict{ V_WAIT=0, V_PASS=1, V_FAIL=2 };
  int64_t maxMs=4500; uint64_t needFrames=4;        // 4 new frames: a fifteenth of a second of motion at 60 fps. A healthy player needs well under a second; the engine's
                                                    // "blind" mode (OnVideoStreamTick never signals) only starts counting after ~2 s, hence the room.
  bool open=false; int64_t t0=0; uint64_t f0=0, got=0;
  void reset(){ open=false; got=0; }
  Verdict update(int64_t nowMs,uint64_t framesTotal){
    if(!open){ open=true; t0=nowMs; f0=framesTotal; got=0; }
    got=framesTotal>=f0?framesTotal-f0:0;           // (a counter that restarted at zero belongs to a rebuilt player, which also resets the gate)
    if(got>=needFrames) return V_PASS;
    return nowMs-t0>=maxMs?V_FAIL:V_WAIT; }
};

// How long before the screen is due the next player is built (settings: afkIdleSec / the screen-saver wait): half the wait, at most 30 s. A build plus its first frame
// takes seconds -- on a slow GPU or a cold disk, many -- and a screen that came up before that showed the clock on black with no video.
static inline int64_t afkPreloadLeadMs(int64_t limitMs){ if(limitMs<=0) return 0; const int64_t l=limitMs/2; return l>30000?30000:l; }

// ======================================================================================
// What the app remembers about this machine between runs (persisted by settings.cpp)
// ======================================================================================
// A verdict must not outlive its cause: "the 4K tier was slow once" can be a game running in the background, so
//  * a slow verdict only becomes permanent after two sessions in a row (a 30 s run without trouble clears the count),
//  * a hard failure (the GPU player cannot start, repeated device loss) is remembered at once,
//  * and any remembered verdict -- a raised tier or a lone strike -- expires after a week, when the app simply tries the better tier again.
// Strikes belong to the tier a session STARTED on (afkJudgesTier): a swap down inside one session is the consequence of that verdict, not a second,
// independent one -- otherwise the swapped-to tier's good run would erase the strike, or its own slowness would count as a second one.
struct AfkTierMemory{
  int minTier=0;                 // lowest tier number worth starting at (0 = 4K GPU, 1 = lite GPU, 2 = CPU)
  int strikes=0;                 // consecutive slow sessions at the current tier
  int64_t minTierAtSec=0;        // unix time of the last verdict (a raised minTier or a strike); 0 = none
};
static const int64_t AFK_MINTIER_TTL_SEC=7LL*24*3600;
static const int     AFK_STRIKES_TO_PERSIST=2;
static const int64_t AFK_GOOD_RUN_MS=30000;

// Returns true if anything changed.
static inline bool afkMemExpire(AfkTierMemory& m,int64_t nowSec){
  bool ch=false;
  if(m.minTier<0||m.minTier>2){ m.minTier=0; ch=true; }
  if(m.strikes<0||m.strikes>9){ m.strikes=0; ch=true; }
  const bool live=(m.minTier>0||m.strikes>0);
  if(live && (m.minTierAtSec<=0 || nowSec<m.minTierAtSec || nowSec-m.minTierAtSec>AFK_MINTIER_TTL_SEC)){ m.minTier=0; m.strikes=0; m.minTierAtSec=0; ch=true; }   // too old, undated or from the future
  if(!live && m.minTierAtSec!=0){ m.minTierAtSec=0; ch=true; }
  return ch;
}
// A tier just failed and the controller is moving to `toTier`. hard: cannot start / device lost; otherwise it was too slow.
// Returns true if the memory changed (the caller saves it).
static inline bool afkMemOnDowngrade(AfkTierMemory& m,int toTier,bool hard,int64_t nowSec){
  if(toTier<=0||toTier>2) return false;                       // nothing worth remembering (already at the top / no tier left)
  if(hard){ if(toTier>m.minTier){ m.minTier=toTier; m.minTierAtSec=nowSec; } m.strikes=0; return true; }
  m.strikes++; m.minTierAtSec=nowSec;                         // a strike is dated too, so one from months ago cannot pair up with a new one
  if(m.strikes>=AFK_STRIKES_TO_PERSIST){ if(toTier>m.minTier) m.minTier=toTier; m.strikes=0; if(m.minTier==0) m.minTierAtSec=0; }
  return true;
}
static inline bool afkMemOnGoodRun(AfkTierMemory& m){ if(m.strikes>0){ m.strikes=0; if(m.minTier==0) m.minTierAtSec=0; return true; } return false; }
// Which tier's result the memory records: only the one the session started on.
static inline bool afkJudgesTier(int entryTier,int tier){ return entryTier==tier; }

// ======================================================================================
// settings.json "afkGpu": which GPU the video player uses on a PC with more than one
// ======================================================================================
// "default" lets Windows choose (it honours the per-app graphics preference in Settings and normally takes the GPU that drives the screen, which
// is the fastest choice on an Optimus laptop); the others pick the first hardware adapter of that vendor (MeConfig.preferredVendor).
enum AfkGpuPref{ AFKGPU_DEFAULT=0, AFKGPU_INTEL=1, AFKGPU_NVIDIA=2, AFKGPU_AMD=3 };
static inline int afkGpuPrefFromName(const std::string& s){
  std::string l; for(size_t i=0;i<s.size();i++){ char c=s[i]; if(c>='A'&&c<='Z') c=(char)(c-'A'+'a'); l+=c; }
  if(l=="intel") return AFKGPU_INTEL; if(l=="nvidia") return AFKGPU_NVIDIA; if(l=="amd"||l=="radeon") return AFKGPU_AMD;
  return AFKGPU_DEFAULT; }
static inline const char* afkGpuPrefName(int p){ switch(p){ case AFKGPU_INTEL: return "intel"; case AFKGPU_NVIDIA: return "nvidia"; case AFKGPU_AMD: return "amd"; default: return "default"; } }
static inline unsigned afkGpuVendorId(int p){ switch(p){ case AFKGPU_INTEL: return 0x8086u; case AFKGPU_NVIDIA: return 0x10DEu; case AFKGPU_AMD: return 0x1002u; default: return 0u; } }
