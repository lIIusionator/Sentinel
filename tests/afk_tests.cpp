// afk_tests.cpp -- native unit tests for afk/afk_helpers.h (no Windows needed).
//
//   g++ -std=c++17 -O1 -g -I afk -I media-engine/src tests/afk_tests.cpp -o afk_tests -pthread            && ./afk_tests
//   g++ -std=c++17 -O1 -g -fsanitize=address,undefined ...                            (memory / UB)
//   g++ -std=c++17 -O1 -g -fsanitize=thread ...                                       (frame exchange races)
#include "afk_helpers.h"
#include "me_util.h"
#include <cstdio>
#include <cstdlib>
#include <atomic>
#include <mutex>
#include <thread>
#include <chrono>
#include <algorithm>

static int g_fail=0, g_checks=0;
#define CHECK(c) do{ g_checks++; if(!(c)){ g_fail++; printf("  FAIL %s:%d  %s\n",__FILE__,__LINE__,#c); } }while(0)
#define NEAR(a,b,eps) do{ g_checks++; double _a=(a),_b=(b); if(!(fabs(_a-_b)<=(eps))){ g_fail++; printf("  FAIL %s:%d  %s=%g vs %s=%g\n",__FILE__,__LINE__,#a,_a,#b,_b); } }while(0)
static void section(const char* s){ printf("[%s]\n",s); }

// ------------------------------------------------------------------ copy / content
static void scalarCopy(const uint8_t* srcTop,int32_t ss,uint8_t* dstTop,int32_t ds,uint32_t w,uint32_t h){
  for(uint32_t y=0;y<h;y++) for(uint32_t x=0;x<w;x++){ const uint8_t* s=srcTop+(ptrdiff_t)y*ss+x*4; uint8_t* d=dstTop+(ptrdiff_t)y*ds+x*4;
    d[0]=s[0]; d[1]=s[1]; d[2]=s[2]; d[3]=0xFF; } }
static void testCopy(){
  section("afkCopyOpaque32");
  for(uint32_t w : {1u,2u,3u,4u,5u,7u,8u,16u,17u,63u,64u,101u}) for(uint32_t h : {1u,2u,5u}){
    std::vector<uint8_t> src((size_t)w*h*4+64), a((size_t)w*h*4+64,0xAA), b((size_t)w*h*4+64,0xAA);
    for(size_t i=0;i<src.size();i++) src[i]=(uint8_t)(i*31+7);
    afkCopyOpaque32(src.data(),(int32_t)(w*4),a.data(),(int32_t)(w*4),w,h);
    scalarCopy(src.data(),(int32_t)(w*4),b.data(),(int32_t)(w*4),w,h);
    CHECK(memcmp(a.data(),b.data(),(size_t)w*h*4)==0);
    CHECK(a[(size_t)w*h*4]==0xAA);                       // nothing written past the end
    for(uint32_t i=0;i<w*h;i++) CHECK(a[i*4+3]==0xFF);
  }
  // bottom-up source: srcTop is the last row in memory, stride negative
  { const uint32_t w=13,h=6; std::vector<uint8_t> src(w*h*4); for(size_t i=0;i<src.size();i++) src[i]=(uint8_t)(i*13+1);
    std::vector<uint8_t> a(w*h*4,0), b(w*h*4,0); const uint8_t* top=src.data()+(size_t)(h-1)*w*4;
    afkCopyOpaque32(top,-(int32_t)(w*4),a.data(),(int32_t)(w*4),w,h); scalarCopy(top,-(int32_t)(w*4),b.data(),(int32_t)(w*4),w,h);
    CHECK(a==b); CHECK(a[0]==src[(size_t)(h-1)*w*4]); }
  // destination stride wider than the row
  { const uint32_t w=9,h=3; int32_t ds=(int32_t)(w*4+20); std::vector<uint8_t> src(w*h*4,0x11), a((size_t)ds*h,0x77);
    afkCopyOpaque32(src.data(),(int32_t)(w*4),a.data(),ds,w,h);
    CHECK(a[w*4]==0x77); CHECK(a[(size_t)ds+w*4+3]==0x77); CHECK(a[3]==0xFF); }
  section("afkHasContent");
  { const uint32_t w=64,h=48; std::vector<uint8_t> f(w*h*4,0);
    CHECK(!afkHasContent(f.data(),w,h));                                            // black
    for(size_t i=0;i<f.size();i++) f[i]=100; CHECK(!afkHasContent(f.data(),w,h));   // uniform grey
    for(size_t i=0;i<f.size();i++) f[i]=255; CHECK(!afkHasContent(f.data(),w,h));   // uniform white
    for(uint32_t y=0;y<h;y++) for(uint32_t x=0;x<w;x++){ uint8_t v=(uint8_t)(x*4); f[((size_t)y*w+x)*4]=v; f[((size_t)y*w+x)*4+1]=v; f[((size_t)y*w+x)*4+2]=v; }
    CHECK(afkHasContent(f.data(),w,h));                                              // gradient
    for(size_t i=0;i<f.size();i++) f[i]=(uint8_t)(i%5==0?10:12); CHECK(!afkHasContent(f.data(),w,h));   // dim noise
    CHECK(!afkHasContent(f.data(),0,10)); CHECK(!afkHasContent(f.data(),10,0));
    std::vector<uint8_t> one(4,255); CHECK(!afkHasContent(one.data(),1,1)); }
}

// ------------------------------------------------------------------ pacer / pick
static void testPacer(){
  section("AfkPacer");
  AfkPacer p; bool sl=true;
  CHECK(p.schedule(0,1000,&sl)==0); CHECK(!sl);                            // first frame anchors
  CHECK(p.schedule(333333,1000,&sl)==333333); CHECK(!sl);                  // on time: wait the whole gap
  CHECK(p.schedule(333333,1000+333333-5000,&sl)==5000);                    // 0.5 ms early
  CHECK(p.schedule(666666,1000+700000,&sl)==0); CHECK(!sl);                // slightly late, within SLIP: no wait, no slip
  int64_t late=1000+666666+AfkPacer::SLIP+50000; CHECK(p.schedule(666666,late,&sl)==0); CHECK(sl);   // very late -> slip
  // after slipping, the next frame is paced from the new anchor, not instantly
  int64_t w=p.schedule(666666+333333,late,&sl); CHECK(w>300000&&w<=333333); CHECK(!sl);
  // a timestamp discontinuity (loop restart) re-anchors instead of sleeping for ages
  AfkPacer q; q.schedule(5000000,0); int64_t w2=q.schedule(0,100000); CHECK(w2==0 || w2<=100000);
  q.reset(); CHECK(q.schedule(123,456)==0);
  AfkPacer r; r.schedule(0,0); CHECK(r.schedule(300000000,1000)==0);       // 30 s ahead -> re-anchor, no 30 s sleep
  section("afkPickBest");
  { double f[4]={20,30,29,0}; bool ok[4]={true,true,true,false}; CHECK(afkPickBest(f,ok,4)==1);
    double g[3]={27.5,30,10}; bool o2[3]={true,true,true}; CHECK(afkPickBest(g,o2,3)==0);   // within 10% -> earliest
    double h[2]={0,0}; bool o3[2]={false,false}; CHECK(afkPickBest(h,o3,2)==-1);
    double k[2]={50,100}; bool o4[2]={true,true}; CHECK(afkPickBest(k,o4,2)==1);
    double m[2]={90,100}; CHECK(afkPickBest(m,o4,2)==0); double m2[2]={89.9,100}; CHECK(afkPickBest(m2,o4,2)==1); }
  section("afkCpuTickMs");
  CHECK(afkCpuTickMs(5)==33); CHECK(afkCpuTickMs(30)==39); CHECK(afkCpuTickMs(100)==66); CHECK(afkCpuTickMs(0)==33);
}

// ------------------------------------------------------------------ vignette
static void testVignette(){
  section("vignette");
  NEAR(afkVignette01(0,100,300),0,1e-6); NEAR(afkVignette01(100,100,300),0,1e-6); NEAR(afkVignette01(300,100,300),1,1e-6);
  NEAR(afkVignette01(400,100,300),1,1e-6); NEAR(afkVignette01(200,100,300),0.5,1e-6);
  { float prev=-1; for(float r=0;r<400;r+=1){ float v=afkVignette01(r,100,300); CHECK(v>=prev-1e-6f); prev=v; } }   // monotonic
  const int W=320,H=180; std::vector<uint32_t> px((size_t)W*H);
  afkVignetteFill((uint8_t*)px.data(),W*4,W,H,200.0f/255.0f);
  CHECK((px[(size_t)(H/2)*W+W/2]>>24)==0);                                   // centre fully clear
  CHECK((px[0]>>24)>=190 && (px[0]>>24)<=200);                               // corner ~ max alpha
  for(size_t i=0;i<px.size();i++) CHECK((px[i]&0x00FFFFFF)==0);              // black, premultiplied
  AfkVigStop st[32]; float outerR=0; int n=afkVignetteStops(W,H,200.0f/255.0f,st,32,&outerR);
  CHECK(n==32); CHECK(st[0].pos==0&&st[0].alpha==0); CHECK(st[1].alpha==0); NEAR(st[31].pos,1,1e-5);
  for(int i=1;i<n;i++) CHECK(st[i].pos>st[i-1].pos);
  for(int i=1;i<n;i++) CHECK(st[i].alpha>=st[i-1].alpha-1e-6f);
  // linear interpolation between stops stays close to the exact profile
  float cx=(W-1)*0.5f,cy=(H-1)*0.5f; double worst=0;
  for(int y=0;y<H;y+=7) for(int x=0;x<W;x+=7){ float r=sqrtf((x-cx)*(x-cx)+(y-cy)*(y-cy)); float pos=r/outerR; if(pos>1)pos=1;
    float a=0; for(int i=1;i<n;i++) if(pos<=st[i].pos){ float t=(pos-st[i-1].pos)/(st[i].pos-st[i-1].pos); a=st[i-1].alpha+(st[i].alpha-st[i-1].alpha)*t; break; }
    float exact=(200.0f/255.0f)*afkVignette01(r,(float)H*0.30f,outerR); worst=std::max(worst,(double)fabsf(a-exact)); }
  CHECK(worst<0.01);
  CHECK(afkVignetteStops(W,H,1.0f,st,2)==0);
}

// ------------------------------------------------------------------ blend / loop
static void testBlendLoop(){
  section("afkBlendT");
  CHECK(afkBlendT(100,50,0,600000)==1.0f);                 // gap <= min: no blend
  CHECK(afkBlendT(100,50,50,600000)==1.0f);                // zero gap
  CHECK(afkBlendT(100,50,60,600000)==1.0f);                // negative gap
  CHECK(afkBlendT(1000000,1000000,0,600000)==0.0f);        // just published
  NEAR(afkBlendT(1500000,1000000,0,600000),0.5,1e-6);
  CHECK(afkBlendT(9000000,1000000,0,600000)==1.0f);        // clamped
  CHECK(afkBlendT(500,1000000,0,600000)==0.0f);            // now earlier than publish
}

// ------------------------------------------------------------------ frame layout + render grid
static void testFrameLayoutGrid(){
  section("afkFrameLayoutOk");
  CHECK(afkFrameLayoutOk(1920*4,1920,1080,1920ull*4*1080));            // tight top-down
  CHECK(afkFrameLayoutOk(-1920*4,1920,1080,1920ull*4*1080));           // tight bottom-up
  CHECK(afkFrameLayoutOk(1920*4+64,1920,1080,(1079ull*(1920*4+64))+1920*4));   // padded rows; the last row needs no padding
  CHECK(!afkFrameLayoutOk(1920*4+64,1920,1080,1080ull*1920*4));        // padded rows in a buffer sized for tight ones
  CHECK(!afkFrameLayoutOk(1920*4,1920,1080,1920ull*4*1080-1));         // one byte short
  CHECK(!afkFrameLayoutOk(0,1920,1080,1ull<<40));                       // the hole found in review: stride 0 passed a size check made from the stride
  CHECK(!afkFrameLayoutOk(0,64,64,0));
  CHECK(!afkFrameLayoutOk(64*4-4,64,64,1ull<<30));                      // rows would overlap
  CHECK(!afkFrameLayoutOk(-(64*4-4),64,64,1ull<<30));
  CHECK(!afkFrameLayoutOk(256,0,64,1ull<<20)); CHECK(!afkFrameLayoutOk(256,64,0,1ull<<20));
  CHECK(!afkFrameLayoutOk(256,40000,64,1ull<<40));                      // absurd sizes
  CHECK(afkFrameLayoutOk(4,1,1,4)); CHECK(!afkFrameLayoutOk(4,1,1,3));  // a single pixel
  CHECK(afkFrameLayoutOk(INT64_MIN+1,64,2,1ull<<20)==false);            // |stride| huge: must not overflow into "fits"
  CHECK(afkFrameLayoutOk(INT32_MIN,64,2,1ull<<20)==false);

  section("afkGridNext");
  { const int64_t tick=330000;                                          // 33 ms in 100 ns units
    CHECK(afkGridNext(0,1000,tick)==1000+tick);                         // first frame: the grid starts now
    CHECK(afkGridNext(5000,5400,tick)==5000+tick);                      // normal: one tick after the previous was DUE, not after it ended
    CHECK(afkGridNext(5000,5000+tick,tick)==5000+tick);                 // just on time
    CHECK(afkGridNext(5000,5000+tick+tick/2,tick)==5000+tick);          // late by half a tick: the grid is kept, the frame is simply due already
    CHECK(afkGridNext(5000,5000+3*tick,tick)==5000+3*tick);             // more than a tick behind: restart from now, no burst
    CHECK(afkGridNext(5000,99,0)==99);                                  // degenerate tick
    // 600 simulated frames whose wake-ups are up to ~1.4 ms off, plus one 40 ms stall: the due times never leave the grid the first frame set up
    int64_t due=0, off=0; const int64_t t0=1000000; bool onGrid=true;
    for(int i=0;i<600;i++){
      const int64_t jitter=((i*7919)%2000)-1000+(i%2?0:1500)+(i==300?400000:0);
      const int64_t now=t0+(int64_t)i*tick+jitter;
      due=afkGridNext(due,now,tick);
      if(i==0) off=due-(t0+tick); else if(due!=t0+(int64_t)(i+1)*tick+off) onGrid=false; }
    CHECK(onGrid);
    // a long stall (the display was off for a minute): ONE restart, then the grid goes on from there
    int64_t d2=afkGridNext(due,t0+600*tick+60*10000000LL,tick); CHECK(d2==t0+600*tick+60*10000000LL);
    CHECK(afkGridNext(d2,d2+100,tick)==d2+tick); } }

// ------------------------------------------------------------------ frame exchange
struct TFrame{ uint32_t w,h; std::atomic<int>* live; uint32_t magic=0xC0FFEE; int id;
  TFrame(uint32_t W,uint32_t H,std::atomic<int>* l,int i):w(W),h(H),live(l),id(i){ (*live)++; }
  ~TFrame(){ magic=0xDEAD; (*live)--; } int GetWidth()const{ return (int)w; } int GetHeight()const{ return (int)h; } };
struct TLock{ std::mutex m; void lock(){ m.lock(); } void unlock(){ m.unlock(); } };
static void testExchange(){
  section("AfkFrameExchange");
  std::atomic<int> live{0}; int nextId=0;
  { AfkFrameExchange<TFrame,TLock> ex; auto mk=[&](uint32_t w,uint32_t h){ return new TFrame(w,h,&live,nextId++); };
    TFrame *c,*p; int64_t ct,pt; ex.acquireForDraw(c,p,ct,pt); CHECK(!c&&!p); ex.releaseAfterDraw();
    TFrame* a=ex.obtain(64,36,mk); ex.publish(a,10);
    ex.acquireForDraw(c,p,ct,pt); CHECK(c==a&&!p&&ct==10); ex.releaseAfterDraw();
    TFrame* b=ex.obtain(64,36,mk); ex.publish(b,20);
    ex.acquireForDraw(c,p,ct,pt); CHECK(c==b&&p==a&&ct==20&&pt==10);
    // while the "UI" holds a and b, publishing two more must NOT free them
    TFrame* d=ex.obtain(64,36,mk); ex.publish(d,30); TFrame* e=ex.obtain(64,36,mk); ex.publish(e,40);
    CHECK(a->magic==0xC0FFEE && b->magic==0xC0FFEE);
    ex.releaseAfterDraw();                                  // now they may be pooled
    TFrame* r=ex.obtain(64,36,mk); CHECK(r==a||r==b||r->magic==0xC0FFEE);   // reuse from the pool
    ex.publish(r,50);
    TFrame* odd=ex.obtain(32,18,mk); CHECK(odd->w==32);                      // different size -> not recycled from the pool
    ex.publish(odd,60); ex.clear(); ex.acquireForDraw(c,p,ct,pt); CHECK(!c&&!p); ex.releaseAfterDraw();
    ex.shutdown(); }
  CHECK(live.load()==0);
  // clear() while the UI holds frames must defer, not free
  { AfkFrameExchange<TFrame,TLock> ex; auto mk=[&](uint32_t w,uint32_t h){ return new TFrame(w,h,&live,nextId++); };
    TFrame* a=ex.obtain(8,8,mk); ex.publish(a,1); TFrame* b=ex.obtain(8,8,mk); ex.publish(b,2);
    TFrame *c,*p; int64_t ct,pt; ex.acquireForDraw(c,p,ct,pt); ex.clear(); CHECK(a->magic==0xC0FFEE&&b->magic==0xC0FFEE); ex.releaseAfterDraw(); ex.shutdown(); }
  CHECK(live.load()==0);
  // stress: one producer, one consumer, frames are verified while "drawn"
  { AfkFrameExchange<TFrame,TLock> ex; std::atomic<bool> stop{false}; std::atomic<long> drawn{0}, bad{0}, published{0};
    auto mk=[&](uint32_t w,uint32_t h){ return new TFrame(w,h,&live,nextId++); };
    std::thread prod([&]{ int64_t t=0; while(!stop.load()){ TFrame* f=ex.obtain(128,72,mk); ex.publish(f,++t); published++; if((t&63)==0) std::this_thread::yield(); } });
    std::thread cons([&]{ while(!stop.load()){ TFrame *c,*p; int64_t ct,pt; ex.acquireForDraw(c,p,ct,pt);
        if(c){ for(volatile int i=0;i<200;i++){ if(c->magic!=0xC0FFEE) bad++; if(p&&p->magic!=0xC0FFEE) bad++; } if(p&&pt>ct) bad++; drawn++; }
        ex.releaseAfterDraw(); } });
    std::this_thread::sleep_for(std::chrono::milliseconds(400)); stop=true; prod.join(); cons.join();
    CHECK(bad.load()==0); CHECK(drawn.load()>0); CHECK(published.load()>0); ex.shutdown(); }
  CHECK(live.load()==0);
}

// ------------------------------------------------------------------ fades / stages
static void testFades(){
  section("stage / fade / quote alpha");
  CHECK(afkStage(false,0,500,800)==1.0f);                                    // exiting: everything revealed
  CHECK(afkStage(true,0,500,800)==0.0f); CHECK(afkStage(true,500,500,800)==0.0f);
  NEAR(afkStage(true,900,500,800),0.5,1e-6); CHECK(afkStage(true,1300,500,800)==1.0f); CHECK(afkStage(true,99999,500,800)==1.0f);
  CHECK(afkStage(true,100,500,0)==0.0f); CHECK(afkStage(true,600,500,0)==1.0f);
  // the stages are ordered: stars, then video, then text
  for(uint64_t t=0;t<4000;t+=50){ float s=afkStage(true,t,AFK_STAR_START,AFK_STAR_DUR), v=afkStage(true,t,AFK_VID_START,AFK_VID_DUR), x=afkStage(true,t,AFK_TXT_START,AFK_TXT_DUR);
    CHECK(s>=v-1e-6f && v>=x-1e-6f); }
  // fade: full sweep takes ~500 ms at any frame rate
  for(double dt : {4.0,8.3,16.7,33.3,66.0,125.0}){ float a=0; double el=0; while(a<1.0f && el<2000){ a=afkFadeStep(a,true,dt); el+=dt; } NEAR(el,500,dt+1); CHECK(a==1.0f);
    a=1; el=0; while(a>0.0f && el<2000){ a=afkFadeStep(a,false,dt); el+=dt; } NEAR(el,500,dt+1); CHECK(a==0.0f); }
  CHECK(afkFadeStep(0,true,100000)<=0.5f+1e-6f);                              // a long stall cannot jump the whole fade
  CHECK(afkFadeStep(0.4f,true,-5)==0.4f); CHECK(afkFadeStep(1,true,16)==1.0f); CHECK(afkFadeStep(0,false,16)==0.0f);
  NEAR(afkQuoteAlpha(0,500,100),1,0); NEAR(afkQuoteAlpha(1,100,100),1,1e-6); NEAR(afkQuoteAlpha(1,290,100),0.5,1e-3); NEAR(afkQuoteAlpha(1,1000,100),0,1e-6);
  NEAR(afkQuoteAlpha(2,100,100),0,1e-6); NEAR(afkQuoteAlpha(2,290,100),0.5,1e-3); NEAR(afkQuoteAlpha(2,1000,100),1,1e-6); NEAR(afkQuoteAlpha(2,50,100),0,1e-6);
  CHECK(afkVideoReadyFade(-5)==0.0f && afkVideoReadyFade(0)==0.0f && afkVideoReadyFade(200)==0.5f && afkVideoReadyFade(400)==1.0f && afkVideoReadyFade(9999)==1.0f);
}

// ------------------------------------------------------------------ stars
static void testStars(){
  section("stars");
  AfkRng r1,r2; std::vector<AfkStar> a,b; afkStarsInit(a,90,1920,1080,r1); afkStarsInit(b,90,1920,1080,r2);
  CHECK(a.size()==90); for(size_t i=0;i<a.size();i++){ CHECK(a[i].x==b[i].x&&a[i].y==b[i].y&&a[i].vy==b[i].vy&&a[i].a==b[i].a);
    CHECK(a[i].x>=0&&a[i].x<1920&&a[i].y>=0&&a[i].y<1080); CHECK(a[i].vy>=0.10f&&a[i].vy<0.10f+40/150.0f+1e-4f); CHECK(a[i].a>=70&&a[i].a<240); }
  // dt independence: 2 x 8 ms == 1 x 16 ms (away from the wrap)
  std::vector<AfkStar> s1(1),s2(1); s1[0].x=s2[0].x=100; s1[0].y=s2[0].y=500; s1[0].vy=s2[0].vy=0.3f; AfkRng r;
  afkStarsStep(s1,0.016,1920,1080,r); afkStarsStep(s2,0.008,1920,1080,r); afkStarsStep(s2,0.008,1920,1080,r); NEAR(s1[0].y,s2[0].y,1e-3);
  // resolution scaling: same fraction of the screen per second at 4K as at 1080p
  std::vector<AfkStar> k1(1),k2(1); k1[0].y=500; k2[0].y=1000; k1[0].vy=k2[0].vy=0.2f; k1[0].x=k2[0].x=1; afkStarsStep(k1,0.1,1920,1080,r); afkStarsStep(k2,0.1,3840,2160,r);
  NEAR((500-k1[0].y)/1080.0,(1000-k2[0].y)/2160.0,1e-5);
  // huge dt is clamped; wrap re-enters at the bottom
  std::vector<AfkStar> w(1); w[0].y=0.01f; w[0].vy=1; w[0].x=5; afkStarsStep(w,10.0,1920,1080,r); CHECK(w[0].y>1000&&w[0].y<=1080); CHECK(w[0].x>=0&&w[0].x<1920);
  for(int i=0;i<100000;i++) afkStarsStep(a,0.016,1920,1080,r); for(auto&s:a) CHECK(s.y>=0&&s.y<=1080&&s.x>=0&&s.x<1920);
  // centre dimming
  NEAR(afkStarCenterDim(960,540,1920,1080),0.06,1e-6); NEAR(afkStarCenterDim(960+324,540,1920,1080),1.0,1e-5); NEAR(afkStarCenterDim(0,0,1920,1080),1.0,1e-6);
  { float prev=0; for(float d=0;d<=324;d+=4){ float v=afkStarCenterDim(960+d,540,1920,1080); CHECK(v>=prev-1e-6f); prev=v; } }
  CHECK(afkStarCenterDim(1,1,0,0)==1.0f);
}

// ------------------------------------------------------------------ geometry
static void testGeometry(){
  section("cover / layout / sync");
  float c[4]; me_coverSrcNorm(3840,2160,1920,1080,c); CHECK(c[0]==0&&c[1]==0&&c[2]==1&&c[3]==1);          // same aspect: no crop
  me_coverSrcNorm(3840,2160,1920,1200,c); NEAR(c[0],(1-(1920.0/1200)/(3840.0/2160))/2,1e-6); CHECK(c[1]==0&&c[3]==1); NEAR(c[2],1-c[0],1e-6);   // 16:10 -> crop the sides
  me_coverSrcNorm(3840,2160,2560,1080,c); CHECK(c[0]==0&&c[2]==1); NEAR(c[1],(1-(3840.0/2160)/(2560.0/1080))/2,1e-6); NEAR(c[3],1-c[1],1e-6);   // ultrawide -> crop top/bottom
  me_coverSrcNorm(0,0,10,10,c); CHECK(c[0]==0&&c[2]==1);
  for(int i=0;i<200;i++){ uint32_t vw=100+rand()%4000, vh=100+rand()%3000, W=200+rand()%3000, H=200+rand()%2000; me_coverSrcNorm(vw,vh,W,H,c);
    CHECK(c[0]>=0&&c[1]>=0&&c[2]<=1+1e-6f&&c[3]<=1+1e-6f&&c[0]<c[2]&&c[1]<c[3]);
    double sa=((c[2]-c[0])*vw)/((c[3]-c[1])*vh); NEAR(sa,(double)W/H,1e-3*(double)W/H); }                       // cropped region has the target's aspect
  float d[4]; afkCoverDest(3840,2160,1920,1200,d); CHECK(d[0]<=0.001f&&d[1]<=0.001f&&d[0]+d[2]>=1919.99f&&d[1]+d[3]>=1199.99f);
  for(int i=0;i<200;i++){ float fw=100+rand()%4000,fh=100+rand()%3000,W=200+rand()%3000,H=200+rand()%2000; afkCoverDest(fw,fh,W,H,d);
    CHECK(d[0]<=1e-3f&&d[1]<=1e-3f&&d[0]+d[2]>=W-1e-2f&&d[1]+d[3]>=H-1e-2f); NEAR(d[2]/d[3],fw/fh,1e-3*fw/fh); }
  for(int H : {768,1080,1440,2160}){ int W=H*16/9; float s=afkUiScale(1.0f,H); AfkLayout L=afkLayout(W,H,s); CHECK(L.quoteAvail>H*0.08f); CHECK(L.hintY<H&&L.hintY>L.quoteTop); CHECK(L.clockY<L.quoteTop); NEAR(L.quoteW,W*0.7,1e-3); }
  NEAR(afkUiScale(1.25f,1080),1.25,1e-6); NEAR(afkUiScale(1.0f,2160),2.0,1e-6); NEAR(afkUiScale(1.0f,768),1.0,1e-6); NEAR(afkUiScale(1.5f,2160),2.0,1e-6);
  CHECK(afkSyncInterval(60)==1&&afkSyncInterval(59)==1&&afkSyncInterval(75)==1&&afkSyncInterval(90)==1&&afkSyncInterval(100)==2&&afkSyncInterval(120)==2&&afkSyncInterval(144)==2&&afkSyncInterval(165)==3&&afkSyncInterval(240)==4&&afkSyncInterval(360)==4);
  CHECK(afkSyncInterval(0)==1&&afkSyncInterval(1)==1&&afkSyncInterval(29)==1&&afkSyncInterval(5000)==1&&afkSyncInterval(30)==1);
}

// ------------------------------------------------------------------ text
static void testText(){
  section("text helpers");
  std::wstring p; std::vector<AfkTextRun> b;
  afkParseQuote(L"Hello **big** world",L"Y",p,b); CHECK(p==L"Hello big world"); CHECK(b.size()==1&&b[0].start==6&&b[0].len==3);
  afkParseQuote(L"Itterasshai, {user}.",L"Lunaris",p,b); CHECK(p==L"Itterasshai, Lunaris."); CHECK(b.empty());
  afkParseQuote(L"**a** b **c d** {user}",L"U",p,b); CHECK(p==L"a b c d U"); CHECK(b.size()==2&&b[0].start==0&&b[0].len==1&&b[1].start==4&&b[1].len==3);
  afkParseQuote(L"open **never closed",L"U",p,b); CHECK(p==L"open never closed"); CHECK(b.size()==1&&b[0].start==5&&b[0].len==12);
  afkParseQuote(L"****",L"U",p,b); CHECK(p.empty()&&b.empty());
  afkParseQuote(L"x {user} y",L"{user}",p,b); CHECK(p==L"x {user} y");                       // no re-substitution loop
  afkParseQuote(L"",L"U",p,b); CHECK(p.empty()&&b.empty());
  afkParseQuote(L"{user}{user}",L"ab",p,b); CHECK(p==L"abab");
  afkParseQuote(L"a * b ** c",L"U",p,b); CHECK(p==L"a * b  c" || p==L"a * b c"); CHECK(b.size()==1 || b.empty());
  wchar_t c[9]; afkFmtClock(c,7,5,9); CHECK(std::wstring(c)==L"07:05:09"); afkFmtClock(c,23,59,59); CHECK(std::wstring(c)==L"23:59:59"); afkFmtClock(c,0,0,0); CHECK(std::wstring(c)==L"00:00:00");
  CHECK(me_fileUrl(L"C:\\Users\\Yaccho\\content\\afk\\a b.mp4")==L"file:///C:/Users/Yaccho/content/afk/a%20b.mp4");
  CHECK(me_fileUrl(L"D:\\x#y%z.mp4")==L"file:///D:/x%23y%25z.mp4");
  CHECK(me_fileUrl(L"C:\\\u00e9.mp4")==L"file:///C:/%C3%A9.mp4");
  CHECK(me_fileUrl(L"C:\\\u4e2d.mp4")==L"file:///C:/%E4%B8%AD.mp4");
  if(sizeof(wchar_t)==4){ std::wstring s=L"C:\\"; s+=(wchar_t)0x1F600; s+=L".mp4"; CHECK(me_fileUrl(s)==L"file:///C:/%F0%9F%98%80.mp4"); }
  else { std::wstring s=L"C:\\"; s+=(wchar_t)0xD83D; s+=(wchar_t)0xDE00; s+=L".mp4"; CHECK(me_fileUrl(s)==L"file:///C:/%F0%9F%98%80.mp4"); }
}

// ------------------------------------------------------------------ tiers / watchdog
static void testTiers(){
  section("tier ladder");
  // auto, built-in clips: 0 -> 1 -> 2 -> none
  CHECK(afkStartTier(0,0,false)==0&&afkStartTier(0,1,false)==1&&afkStartTier(0,2,false)==2&&afkStartTier(0,9,false)==2&&afkStartTier(0,-4,false)==0);
  CHECK(afkNextTier(0,0,false)==1&&afkNextTier(1,0,false)==2&&afkNextTier(2,0,false)==AFK_TIER_NONE);
  // auto, user clip: the lite tier is skipped
  CHECK(afkStartTier(0,1,true)==2&&afkStartTier(0,0,true)==0&&afkNextTier(0,0,true)==2);
  // gpu-only: never the CPU renderer
  CHECK(afkStartTier(1,2,false)==0&&afkStartTier(1,1,false)==1&&afkNextTier(0,1,false)==1&&afkNextTier(1,1,false)==AFK_TIER_NONE&&afkNextTier(2,1,false)==AFK_TIER_NONE);
  CHECK(afkStartTier(1,1,true)==0&&afkNextTier(0,1,true)==AFK_TIER_NONE);
  // cpu-only
  CHECK(afkStartTier(2,0,false)==2&&afkStartTier(2,0,true)==2&&afkNextTier(2,2,false)==AFK_TIER_NONE);
  // every ladder terminates
  for(int mode=0;mode<3;mode++) for(int user=0;user<2;user++) for(int mt=0;mt<3;mt++){ int t=afkStartTier(mode,mt,user!=0),n=0; while(t!=AFK_TIER_NONE&&n<10){ t=afkNextTier(t,mode,user!=0); n++; } CHECK(t==AFK_TIER_NONE&&n<=3); }
  section("watchdog");
  { AfkWatchdog w; uint64_t frames=0; int64_t t=0; AfkWatchdog::Verdict v; bool down=false; int oks=0;
    // healthy 60 fps for 20 s
    for(t=0;t<=20000;t+=16){ frames=(uint64_t)(t*60/1000); v=w.update(t,frames,60); if(v==AfkWatchdog::V_DOWNGRADE) down=true; if(v==AfkWatchdog::V_OK) oks++; }
    CHECK(!down); CHECK(oks>=7); NEAR(w.lastFps,60,2); }
  { AfkWatchdog w; uint64_t frames=0; AfkWatchdog::Verdict v; int64_t downAt=-1;       // 20 fps against a 60 fps clip -> bad twice -> downgrade
    for(int64_t t=0;t<=20000&&downAt<0;t+=16){ frames=(uint64_t)(t*20/1000); v=w.update(t,frames,60); if(v==AfkWatchdog::V_DOWNGRADE) downAt=t; }
    CHECK(downAt>=3000+2*2000-40 && downAt<=3000+2*2000+80); }                             // warm-up 3 s + two 2 s windows
  { AfkWatchdog w; uint64_t frames=0; bool down=false;                                    // one bad window then recovery -> never downgrades
    for(int64_t t=0;t<=30000;t+=16){ double fps=(t>=5000&&t<7000)?5:60; static double acc; (void)acc; frames+=(uint64_t)(fps*16/1000.0+0.5); if(w.update(t,frames,60)==AfkWatchdog::V_DOWNGRADE) down=true; }
    CHECK(!down); }
  { AfkWatchdog w; bool down=false; for(int64_t t=0;t<=30000;t+=16) if(w.update(t,0,30)==AfkWatchdog::V_DOWNGRADE) down=true; CHECK(down); }   // frozen picture
  { AfkWatchdog w; w.update(0,0,60); w.update(1000,60,60); w.reset(); CHECK(w.update(100000,0,60)==AfkWatchdog::V_NONE && !w.warm); }
  { AfkWatchdog w; AfkWatchdog::Verdict v; int64_t t=0; uint64_t f=0; for(;t<3100;t+=10){ f=(uint64_t)(t*30/1000); w.update(t,f,30); }   // 30 fps clip at 30 fps: fine; 18 fps is the threshold
    bool down=false; for(;t<20000;t+=10){ f=(uint64_t)(t*30/1000); v=w.update(t,f,30); if(v==AfkWatchdog::V_DOWNGRADE) down=true; } CHECK(!down); }
  { AfkWatchdog w; bool down=false; uint64_t f=0; for(int64_t t=0;t<20000;t+=10){ f=(uint64_t)(t*17/1000); if(w.update(t,f,30)==AfkWatchdog::V_DOWNGRADE) down=true; } CHECK(down); }   // 17 < 18
  { AfkWatchdog w; w.update(5,0,0); w.update(3100,0,0); CHECK(w.warm); }                // clip fps unknown -> assume 30
}


static void testEngineUtil(){
  section("me_util");
  CHECK(me_ema(0,5)==5); NEAR(me_ema(10,20),11,1e-9);
  MeBlindCounter b; CHECK(b.advance(0.0,60)==0); unsigned long tot=0; for(int i=1;i<=100;i++) tot+=b.advance(i*0.0166667,60); CHECK(tot>=99&&tot<=100);   // ~1.67 s at 60 fps
  b.advance(0.5,60); unsigned long before=b.advance(2.0,60); CHECK(before>=80);                                                              // forward jump counted
  unsigned long loop=b.advance(0.1,60); CHECK(loop==0);                                                                                      // wrap: nothing added
  unsigned long next=b.advance(0.1166667,60); CHECK(next==1);                                                                                // then counting resumes from the new position
  MeBlindCounter z; z.advance(0,0); unsigned long t30=0; for(int i=1;i<=30;i++) t30+=z.advance(i/30.0,0); CHECK(t30>=29&&t30<=30);          // unknown fps assumed 30
}

// ------------------------------------------------------------------ one-pass compose (CPU back-end)
static uint8_t refMul(uint32_t s,uint32_t g){ return (uint8_t)((s*g+127)/255); }          // plain rounding, to cross-check afkMul255
static void refMulGain(const uint8_t* src,int32_t ss,const uint8_t* gain,int32_t gs,uint8_t* dst,int32_t ds,uint32_t w,uint32_t h){
  for(uint32_t y=0;y<h;y++) for(uint32_t x=0;x<w;x++){ const uint8_t* s=src+(ptrdiff_t)y*ss+x*4; const uint8_t* g=gain+(ptrdiff_t)y*gs+x*4; uint8_t* d=dst+(ptrdiff_t)y*ds+x*4;
    for(int c=0;c<3;c++) d[c]=afkMul255(s[c],g[c]); d[3]=0xFF; } }
static void refBlendGain(const uint8_t* a,const uint8_t* b,int32_t abs_,int t,const uint8_t* gain,int32_t gs,uint8_t* dst,int32_t ds,uint32_t w,uint32_t h){
  if(t<0)t=0; if(t>128)t=128;
  for(uint32_t y=0;y<h;y++) for(uint32_t x=0;x<w;x++){ const uint8_t* pa=a+(ptrdiff_t)y*abs_+x*4; const uint8_t* pb=b+(ptrdiff_t)y*abs_+x*4; const uint8_t* g=gain+(ptrdiff_t)y*gs+x*4; uint8_t* d=dst+(ptrdiff_t)y*ds+x*4;
    for(int c=0;c<3;c++){ int m=pa[c]+(((pb[c]-pa[c])*t)>>7); d[c]=afkMul255((uint32_t)m,g[c]); } d[3]=0xFF; } }
static void testCompose(){
  section("afkMul255");
  { int bad=0; for(uint32_t s=0;s<=255;s++) for(uint32_t g=0;g<=255;g++){ double e=s*g/255.0; int r=afkMul255(s,g); if(fabs(r-e)>0.5001) bad++; if(r!=(int)refMul(s,g)&&fabs(r-e)>0.5001) bad++; } CHECK(bad==0); }   // exactly round(s*g/255)
  CHECK(afkMul255(255,255)==255); CHECK(afkMul255(0,255)==0); CHECK(afkMul255(255,0)==0); CHECK(afkMul255(128,255)==128); CHECK(afkMul255(255,128)==128);
  section("afkMulGainOpaque32");
  srand(1234);
  for(uint32_t w : {1u,2u,3u,4u,5u,7u,8u,9u,16u,17u,31u,64u,101u}) for(uint32_t h : {1u,3u}){
    std::vector<uint8_t> src((size_t)w*h*4+64), gain((size_t)w*h*4+64), a((size_t)w*h*4+64,0x5A), b((size_t)w*h*4+64,0x5A);
    for(auto& v:src) v=(uint8_t)rand(); for(auto& v:gain) v=(uint8_t)rand();
    afkMulGainOpaque32(src.data(),(int32_t)(w*4),gain.data(),(int32_t)(w*4),a.data(),(int32_t)(w*4),w,h);
    refMulGain(src.data(),(int32_t)(w*4),gain.data(),(int32_t)(w*4),b.data(),(int32_t)(w*4),w,h);
    CHECK(memcmp(a.data(),b.data(),(size_t)w*h*4)==0); CHECK(a[(size_t)w*h*4]==0x5A); }
  { const uint32_t w=21,h=5; std::vector<uint8_t> src(w*h*4),gain(w*h*4); for(auto& v:src) v=(uint8_t)rand(); for(auto& v:gain) v=(uint8_t)rand();   // bottom-up source, wider destination stride
    std::vector<uint8_t> a((size_t)(w*4+16)*h,0x33), b((size_t)(w*4+16)*h,0x33); const uint8_t* top=src.data()+(size_t)(h-1)*w*4;
    afkMulGainOpaque32(top,-(int32_t)(w*4),gain.data(),(int32_t)(w*4),a.data(),(int32_t)(w*4+16),w,h); refMulGain(top,-(int32_t)(w*4),gain.data(),(int32_t)(w*4),b.data(),(int32_t)(w*4+16),w,h);
    CHECK(a==b); CHECK(a[w*4]==0x33); }
  { std::vector<uint8_t> s(16,200),g(16,255),d(16,0); afkMulGainOpaque32(s.data(),16,g.data(),16,d.data(),16,4,1); for(int i=0;i<16;i++) CHECK(d[i]==((i%4==3)?255:200));   // unit gain keeps colour, forces alpha
    std::fill(g.begin(),g.end(),0); afkMulGainOpaque32(s.data(),16,g.data(),16,d.data(),16,4,1); for(int i=0;i<16;i++) CHECK(d[i]==((i%4==3)?255:0)); }
  section("afkBlendGainOpaque32");
  for(uint32_t w : {1u,3u,4u,5u,8u,13u,32u,67u}) for(int t : {-5,0,1,31,64,100,127,128,200}){ const uint32_t h=3;
    std::vector<uint8_t> pa((size_t)w*h*4+32), pb((size_t)w*h*4+32), gain((size_t)w*h*4+32), a((size_t)w*h*4+32,0x11), b((size_t)w*h*4+32,0x11);
    for(auto& v:pa) v=(uint8_t)rand(); for(auto& v:pb) v=(uint8_t)rand(); for(auto& v:gain) v=(uint8_t)rand();
    afkBlendGainOpaque32(pa.data(),pb.data(),(int32_t)(w*4),t,gain.data(),(int32_t)(w*4),a.data(),(int32_t)(w*4),w,h);
    refBlendGain(pa.data(),pb.data(),(int32_t)(w*4),t,gain.data(),(int32_t)(w*4),b.data(),(int32_t)(w*4),w,h);
    CHECK(memcmp(a.data(),b.data(),(size_t)w*h*4)==0); CHECK(a[(size_t)w*h*4]==0x11); }
  { const uint32_t w=8,h=2; std::vector<uint8_t> pa(w*h*4,10), pb(w*h*4,250), g(w*h*4,255), d(w*h*4);                                   // endpoints
    afkBlendGainOpaque32(pa.data(),pb.data(),w*4,0,g.data(),w*4,d.data(),w*4,w,h); CHECK(d[0]==10&&d[1]==10&&d[3]==255);
    afkBlendGainOpaque32(pa.data(),pb.data(),w*4,128,g.data(),w*4,d.data(),w*4,w,h); CHECK(d[0]==250&&d[2]==250);
    afkBlendGainOpaque32(pa.data(),pb.data(),w*4,64,g.data(),w*4,d.data(),w*4,w,h); NEAR(d[0],130,1); }
  { const uint32_t w=6,h=2; std::vector<uint8_t> pa(w*h*4,255), pb(w*h*4,0), g(w*h*4,255), d(w*h*4);                                    // falling brightness never wraps
    for(int t=0;t<=128;t++){ afkBlendGainOpaque32(pa.data(),pb.data(),w*4,t,g.data(),w*4,d.data(),w*4,w,h); int exp=255+(((0-255)*t)>>7); CHECK(d[0]==exp&&exp>=0&&exp<=255); } }
  section("afkFill32 / gain map");
  { std::vector<uint8_t> d((size_t)(7*4+8)*3,0x00); afkFill32(d.data(),7*4+8,7,3,0xFF060810u); uint32_t v0,v1; memcpy(&v0,d.data(),4); memcpy(&v1,d.data()+(7*4+8)*2+24,4); CHECK(v0==0xFF060810u&&v1==0xFF060810u&&d[7*4]==0); }
  { const int W=97,H=61; std::vector<uint32_t> gm((size_t)W*H), al((size_t)W*H); afkGainMapFromVignette(gm.data(),W,H,200.0f/255.0f); afkVignetteFill((uint8_t*)al.data(),W*4,W,H,200.0f/255.0f);
    bool same=true; for(size_t i=0;i<gm.size();i++){ uint32_t a=al[i]>>24, g=255u-a; if(gm[i]!=(0xFF000000u|(g<<16)|(g<<8)|g)) same=false; } CHECK(same);          // gain = 255 - vignette alpha, replicated into B,G,R
    CHECK((gm[(size_t)(H/2)*W+W/2]&0xFF)==255);                                                                                                                          // clear centre
    CHECK((gm[0]&0xFF)<255 && (gm[0]&0xFF)>=55);                                                                                                                         // darkest at the corner, never darker than the 200/255 cap
    uint32_t prev=0; bool mono=true; for(int i=0;i<std::min(W,H)/2;i++){ uint32_t g=gm[(size_t)i*W+i]&0xFF; if(g<prev) mono=false; prev=g; } CHECK(mono); }       // corner -> centre: the gain only ever rises (falls off towards the corner)
  { uint32_t dummy=0; afkGainMapFromVignette(&dummy,0,0,0.5f); afkGainMapFromVignette(&dummy,-3,5,0.5f); CHECK(dummy==0); }                                               // degenerate sizes are harmless
  { // vignetted frame == video with black alpha drawn over it (the premultiplied "over" the GDI+ path performs)
    const int W=64,H=40; std::vector<uint32_t> gm((size_t)W*H), al((size_t)W*H); afkGainMapFromVignette(gm.data(),W,H,200.0f/255.0f); afkVignetteFill((uint8_t*)al.data(),W*4,W,H,200.0f/255.0f);
    std::vector<uint8_t> src((size_t)W*H*4), out((size_t)W*H*4); for(auto& v:src) v=(uint8_t)rand();
    afkMulGainOpaque32(src.data(),W*4,(const uint8_t*)gm.data(),W*4,out.data(),W*4,W,H); int bad=0;
    for(int i=0;i<W*H;i++){ uint32_t a=al[i]>>24; for(int c=0;c<3;c++){ int over=(int)(src[(size_t)i*4+c]*(255-a)/255.0+0.5); if(abs(over-(int)out[(size_t)i*4+c])>1) bad++; } } CHECK(bad==0); }
}

// ------------------------------------------------------------------ tier memory
static void testTierMemory(){
  section("AfkTierMemory");
  const int64_t DAY=86400, T0=1800000000LL; AfkTierMemory m;
  CHECK(!afkMemOnDowngrade(m,0,false,T0)); CHECK(!afkMemOnDowngrade(m,3,true,T0)); CHECK(m.minTier==0&&m.strikes==0);      // nothing to remember
  // one slow session: strike only; the second in a row makes it permanent
  CHECK(afkMemOnDowngrade(m,1,false,T0)); CHECK(m.minTier==0&&m.strikes==1);
  CHECK(afkMemOnDowngrade(m,1,false,T0+10)); CHECK(m.minTier==1&&m.strikes==0&&m.minTierAtSec==T0+10);
  // a good run in between clears the strike
  AfkTierMemory a; afkMemOnDowngrade(a,1,false,T0); CHECK(afkMemOnGoodRun(a)&&a.strikes==0); CHECK(!afkMemOnGoodRun(a)); afkMemOnDowngrade(a,1,false,T0+5); CHECK(a.minTier==0&&a.strikes==1);
  // a hard failure is remembered at once, and never lowers an existing verdict
  AfkTierMemory h; CHECK(afkMemOnDowngrade(h,2,true,T0)); CHECK(h.minTier==2&&h.strikes==0&&h.minTierAtSec==T0); CHECK(afkMemOnDowngrade(h,1,true,T0+9)); CHECK(h.minTier==2&&h.minTierAtSec==T0);
  // expiry after a week; clock going backwards also clears it; sanitising garbage from settings.json
  AfkTierMemory e; e.minTier=2; e.minTierAtSec=T0; e.strikes=1;
  CHECK(!afkMemExpire(e,T0+6*DAY)&&e.minTier==2); CHECK(afkMemExpire(e,T0+8*DAY)&&e.minTier==0&&e.strikes==0&&e.minTierAtSec==0);
  AfkTierMemory f; f.minTier=1; f.minTierAtSec=T0; CHECK(afkMemExpire(f,T0-100)&&f.minTier==0);
  AfkTierMemory g; g.minTier=1; g.minTierAtSec=0; CHECK(afkMemExpire(g,T0)&&g.minTier==0);                                     // a verdict without a date cannot be trusted
  AfkTierMemory z; z.minTier=7; z.strikes=-2; CHECK(afkMemExpire(z,T0)&&z.minTier==0&&z.strikes==0);
  AfkTierMemory ok; CHECK(!afkMemExpire(ok,T0));
  // the ladder honours it
  AfkTierMemory r; afkMemOnDowngrade(r,1,true,T0); CHECK(afkStartTier(0,r.minTier,false)==1); afkMemExpire(r,T0+9*DAY); CHECK(afkStartTier(0,r.minTier,false)==0);
  // a lone strike is dated and expires as well; one without a date (older settings) is not trusted
  AfkTierMemory s; afkMemOnDowngrade(s,1,false,T0); CHECK(s.strikes==1&&s.minTier==0&&s.minTierAtSec==T0);
  CHECK(!afkMemExpire(s,T0+6*DAY)&&s.strikes==1); CHECK(afkMemExpire(s,T0+8*DAY)&&s.strikes==0&&s.minTierAtSec==0);
  AfkTierMemory u; u.strikes=1; u.minTierAtSec=0; CHECK(afkMemExpire(u,T0)&&u.strikes==0);
  AfkTierMemory gr; afkMemOnDowngrade(gr,1,false,T0); afkMemOnGoodRun(gr); CHECK(gr.strikes==0&&gr.minTierAtSec==0&&!afkMemExpire(gr,T0+1));   // clean state after a good run
  AfkTierMemory k; afkMemOnDowngrade(k,2,true,T0); afkMemOnDowngrade(k,3,false,T0+1); CHECK(k.minTier==2&&k.minTierAtSec==T0);              // nothing below the CPU tier to remember
  AfkTierMemory rs; afkMemOnDowngrade(rs,2,true,T0); afkMemOnDowngrade(rs,2,false,T0+5); afkMemOnDowngrade(rs,2,false,T0+6);
  CHECK(rs.minTier==2&&rs.strikes==0);                                                                                                          // strikes toward a tier that is already the floor never lower it
}

// A whole machine, session by session, with the policy the controller applies: only the tier a session started on is judged (afkJudgesTier). `firstFast` is the
// lowest tier number that keeps up on the simulated machine; every tier above it is too slow. The review of the controller proved that without this rule a machine
// whose 4K tier is slow but whose 1080p tier is fine never got the verdict, and that two slow tiers in one session wrote the CPU verdict at once.
static int simSessions(AfkTierMemory& mem,int firstFast,int sessions,int64_t t0,int* started=nullptr){
  int lastEntry=-1;
  for(int s=0;s<sessions;s++){
    const int64_t now=t0+s*3600; afkMemExpire(mem,now);
    const int entry=afkStartTier(0,mem.minTier,false); lastEntry=entry; if(started) started[s]=entry;
    int t=entry;
    while(t!=AFK_TIER_NONE&&t<firstFast){                          // too slow: the watchdog swaps one tier down
      const int next=afkNextTier(t,0,false);
      if(afkJudgesTier(entry,t)) afkMemOnDowngrade(mem,next,false,now);
      t=next; }
    if(t!=AFK_TIER_NONE&&afkJudgesTier(entry,t)) afkMemOnGoodRun(mem);   // the entry tier ran well for 30 s
  }
  return lastEntry;
}
static void testTierSessions(){
  section("tier memory over sessions");
  const int64_t T0=1800000000LL; int st[8];
  { AfkTierMemory m; simSessions(m,1,6,T0,st);                       // 4K slow, 1080p fine: two sessions to be sure, then 1080p from the start
    CHECK(st[0]==0&&st[1]==0&&st[2]==1&&st[3]==1&&st[5]==1); CHECK(m.minTier==1&&m.strikes==0); }
  { AfkTierMemory m; simSessions(m,0,8,T0,st);                       // everything fine: nothing is ever remembered
    CHECK(m.minTier==0&&m.strikes==0); for(int i=0;i<8;i++) CHECK(st[i]==0); }
  { AfkTierMemory m; simSessions(m,2,8,T0,st);                       // only the CPU renderer keeps up: one tier down per two sessions, never faster
    CHECK(st[0]==0&&st[1]==0&&st[2]==1&&st[3]==1&&st[4]==2&&st[5]==2); CHECK(m.minTier==2); }
  { AfkTierMemory m, bad;                                              // the old rule, for contrast: strikes counted for every tier, a fast tier's run cleared them
    afkMemOnDowngrade(bad,1,false,T0);                                // 4K slow -> strike 1, swap to 1080p
    afkMemOnGoodRun(bad);                                             // 1080p runs well -> the strike is gone
    CHECK(bad.minTier==0&&bad.strikes==0);                            // ...so session two starts on the slow 4K tier again: the bug the policy above prevents
    simSessions(m,1,2,T0,st); CHECK(m.minTier==1); }
  { AfkTierMemory m; m.minTier=1; m.minTierAtSec=T0; simSessions(m,1,3,T0,st); CHECK(m.minTier==1&&st[0]==1&&st[2]==1); }   // a remembered 1080p start that works stays put
  CHECK(afkJudgesTier(0,0)&&!afkJudgesTier(0,1)&&afkJudgesTier(2,2));
}

static void testGpuPref(){
  section("afkGpu setting");
  CHECK(afkGpuPrefFromName("intel")==AFKGPU_INTEL&&afkGpuPrefFromName("NVIDIA")==AFKGPU_NVIDIA&&afkGpuPrefFromName("Amd")==AFKGPU_AMD&&afkGpuPrefFromName("radeon")==AFKGPU_AMD);
  CHECK(afkGpuPrefFromName("")==AFKGPU_DEFAULT&&afkGpuPrefFromName("default")==AFKGPU_DEFAULT&&afkGpuPrefFromName("auto")==AFKGPU_DEFAULT&&afkGpuPrefFromName("nvidia1")==AFKGPU_DEFAULT);
  CHECK(afkGpuVendorId(AFKGPU_INTEL)==0x8086&&afkGpuVendorId(AFKGPU_NVIDIA)==0x10DE&&afkGpuVendorId(AFKGPU_AMD)==0x1002&&afkGpuVendorId(AFKGPU_DEFAULT)==0&&afkGpuVendorId(77)==0);
  for(int p=0;p<4;p++) CHECK(afkGpuPrefFromName(afkGpuPrefName(p))==p);                      // the saved name reads back as the same choice
}

// ------------------------------------------------------------------ the video check, the reveal hold and the preload lead
static void testVideoGate(){
  section("video check (AfkVideoGate)");
  { AfkVideoGate g; int64_t passAt=-1; uint64_t frames=0;                      // a healthy 60 fps player whose first frame comes 120 ms after play
    for(int64_t t=1000;t<=1000+3000&&passAt<0;t+=16){ frames=t>=1120?(uint64_t)((t-1120)*60/1000)+1:0; if(g.update(t,frames)==AfkVideoGate::V_PASS) passAt=t; }
    CHECK(passAt>=0&&passAt-1000<400); CHECK(g.got>=4); }
  { AfkVideoGate g; AfkVideoGate::Verdict v=AfkVideoGate::V_WAIT; int64_t failAt=-1;   // frozen: not one new frame -> waits the full 4.5 s, then fails
    for(int64_t t=5000;t<=5000+9000&&failAt<0;t+=16){ v=g.update(t,777); if(v==AfkVideoGate::V_PASS) CHECK(false); if(v==AfkVideoGate::V_FAIL) failAt=t; }
    CHECK(failAt>=5000+4500&&failAt<5000+4500+16); CHECK(g.got==0); }
  { AfkVideoGate g; CHECK(g.update(0,10)==AfkVideoGate::V_WAIT);               // the clock starts at the first update, not at construction (a player that became ready late)
    CHECK(g.update(4499,10)==AfkVideoGate::V_WAIT); CHECK(g.update(4500,10)==AfkVideoGate::V_FAIL); }
  { AfkVideoGate g; g.update(0,100); CHECK(g.update(100,103)==AfkVideoGate::V_WAIT); CHECK(g.update(200,104)==AfkVideoGate::V_PASS); }   // 3 frames are not motion, 4 are
  { AfkVideoGate g; g.update(0,50); CHECK(g.update(10,60)==AfkVideoGate::V_PASS); }
  { AfkVideoGate g; g.update(0,5000); CHECK(g.update(100,3)==AfkVideoGate::V_WAIT&&g.got==0); }   // a counter that went back (rebuilt player) never underflows into a pass
  { AfkVideoGate g; g.update(0,0); g.update(500,2); g.reset(); CHECK(g.update(900,2)==AfkVideoGate::V_WAIT&&g.t0==900&&g.f0==2&&g.got==0); }   // reset: a new measurement
  { AfkVideoGate g; g.update(0,0); CHECK(g.update(4499,3)==AfkVideoGate::V_WAIT); CHECK(g.update(4500,4)==AfkVideoGate::V_PASS); }   // enough frames at the deadline is a pass, not a failure

  section("reveal hold");
  CHECK(afkVideoHold(0)==0&&afkVideoHold(500)==0&&afkVideoHold(AFK_VID_START)==0&&afkVideoHold(2000)==2000-AFK_VID_START);
  NEAR(afkVideoStage(true,AFK_VID_START,0),0.0,1e-6); NEAR(afkVideoStage(true,AFK_VID_START+AFK_VID_DUR,0),1.0,1e-6);   // no hold: the usual schedule
  NEAR(afkVideoStage(true,0,0),afkStage(true,0,AFK_VID_START,AFK_VID_DUR),1e-6);
  { const uint64_t hold=afkVideoHold(2000);                                    // gate passed at 2.0 s: nothing before, a full fade from there
    NEAR(afkVideoStage(true,1999,hold),0.0,1e-6); NEAR(afkVideoStage(true,2000,hold),0.0,1e-6);
    NEAR(afkVideoStage(true,2400,hold),0.5,1e-3); NEAR(afkVideoStage(true,2000+AFK_VID_DUR,hold),1.0,1e-6); NEAR(afkVideoStage(true,99999,hold),1.0,1e-6); }
  NEAR(afkVideoStage(false,10,5000),1.0,1e-6);                                   // the screen is ending: everything snaps to revealed

  section("preload lead");
  CHECK(afkPreloadLeadMs(0)==0&&afkPreloadLeadMs(-5)==0);
  CHECK(afkPreloadLeadMs(5000)==2500&&afkPreloadLeadMs(10000)==5000&&afkPreloadLeadMs(60000)==30000&&afkPreloadLeadMs(59000)==29500);
  CHECK(afkPreloadLeadMs(300000)==30000&&afkPreloadLeadMs(3600000)==30000);
  for(int64_t lim : {1000LL,5000LL,45000LL,60001LL,300000LL}) CHECK(afkPreloadLeadMs(lim)>0&&afkPreloadLeadMs(lim)<=lim/2+1&&afkPreloadLeadMs(lim)<=30000);   // never the whole wait
}

int main(){
  testVideoGate();
  testCopy(); testPacer(); testVignette(); testBlendLoop(); testFrameLayoutGrid(); testExchange(); testFades(); testStars(); testGeometry(); testText(); testTiers(); testCompose(); testTierMemory(); testTierSessions(); testGpuPref(); testEngineUtil();
  printf("\n%d checks, %d failed\n",g_checks,g_fail);
  return g_fail?1:0;
}
