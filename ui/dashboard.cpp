// dashboard.cpp -- the GDI+ control panel: splash, Dashboard / Protection / Events / Updates tabs.
#include "dashboard.h"
#include "gfx.h"
#include "settings.h"
#include "events.h"
#include "watchers.h"
#include "content.h"
#include "afk_screen.h"
#include <shellapi.h>
#include <shlobj.h>
#include <wincrypt.h>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <algorithm>

static const int SIDE_W=210, BAR_H=46;

HWND g_dash=nullptr;
static int g_tab=0;              // 0 dash 1 protect 2 events 3 credits
static bool g_splash=true; static ULONGLONG g_splashT0=0;
static Bitmap* g_avatar=nullptr;
static Bitmap* g_back=nullptr; static int g_bw=0,g_bh=0;
static int g_mx=-1,g_my=-1;                 // mouse (hover)
static ULONGLONG g_tabT0=0;                 // tab-switch time (scramble)
static ULONGLONG g_flashT0=0; static RECT g_flashR={0,0,0,0}; // press flash
static RECT g_rScanTop={0,0,0,0};
static ULONGLONG g_sessionStart=0;
static UINT g_frameMs=33;                   // current render interval
static const ULONGLONG IDLE_FRAME_MS=200;   // repaint interval while nothing changes (5 fps: clock, radar and dots only) ...
static const ULONGLONG IDLE_BG_MS=1000;     // ... and while another window has the focus (nobody is watching the motion: the clock is all that matters)
struct Part{ float x,y,vy; int a; };
static std::vector<Part> g_parts;
static float g_creatorT=0; static RECT g_rCreator={0,0,0,0};   // Creator hover-zoom
static int g_evScroll=0;
static RECT g_evRowR[40]; static std::wstring g_evRowPath[40]; static bool g_evRowWarn[40]={false}; static int g_evRowCount=0;
static RECT g_rFw={0,0,0,0};
static std::wstring g_devHost,g_devCPU,g_devRAM,g_devRes;      // device info
struct Note{ const wchar_t* ver; const wchar_t* kind; const wchar_t* text; };
static Note g_notes[]={
  {L"v1.7",L"FEATURE",L"Start with Windows: hidden in the tray at sign-in (Protection tab)"},
  {L"v1.7",L"FEATURE",L"Strings scan: links + commands inside flagged unsigned apps"},
  {L"v1.7",L"FIX",    L"Low idle CPU: signature cache, dashboard repaints on change"},
  {L"v1.7",L"FEATURE",L"AFK screen: GPU-decoded 4K 60 fps video on its own render thread"},
  {L"v1.7",L"IMPROVE",L"Source split into core / ui / afk / media-engine modules"},
  {L"v1.7",L"FIX",    L"AFK screen no longer starved by the dashboard timer (was ~9 fps)"},
  {L"v1.6",L"FEATURE",L"Token Guard \u2014 warns when unsigned apps read your logins/tokens"},
  {L"v1.6",L"FEATURE",L"Resizable / scalable window (drag any edge or corner)"},
  {L"v1.6",L"FIX",    L"Sharper AFK screensaver video (high-quality bicubic upscaling)"},
  {L"v1.5",L"FEATURE",L"Blue/purple theme, moon icon, hover + press animations"},
  {L"v1.5",L"FEATURE",L"Scramble tab titles, quote & fun-fact engine w/ config files"},
  {L"v1.5",L"FIX",    L"Session timer format (no more lu:lu:lu)"},
  {L"v1.5",L"FIX",    L"Event text no longer bleeds past its card"},
  {L"v1.5",L"IMPROVE",L"Scan now lists already-running apps (Roblox, Explorer)"},
  {L"v1.5",L"IMPROVE",L"Signature-based suspicion; signed apps are not flagged"},
  {L"v1.4",L"FIX",    L"Fixed a render recursion that froze the app"},
  {L"v1.4",L"FIX",    L"AppData watch storm no longer hangs the UI"},
  {L"v1.3",L"FEATURE",L"Tabbed dashboard, protection toggles, JSON settings"},
  {L"v1.2",L"FEATURE",L"AppData file watch, auto-start, cache folder"},
  {L"v1.1",L"FEATURE",L"Live process watch via WMI event feed"},
  {L"v1.0",L"FEATURE",L"Autorun watch (Run keys, Startup, Tasks)"},
};
static RECT g_rNav[4], g_rTog[TOG_N], g_rScan, g_rMin, g_rClose, g_rSoc[3];
// Fullscreen fills the WORK AREA (screen minus taskbar), not the whole monitor -- distinct from the
// AFK screen's own full-monitor takeover, which deliberately does cover the taskbar.
static bool g_fullscreen=false; static RECT g_preFullRect={0,0,0,0};
static const wchar_t* g_socUrl[3]={
  L"https://www.youtube.com/@lIIusionator",
  L"https://www.roblox.com/users/10679736474/profile",
  L""}; // discord = copy


static bool hov(RECT r){ return inR(r,g_mx,g_my); }
static void gatherDevice(){
  wchar_t cn[256]; DWORD cs=256; if(GetComputerNameW(cn,&cs)) g_devHost=cn;
  SYSTEM_INFO si; GetSystemInfo(&si); g_devCPU=std::to_wstring((int)si.dwNumberOfProcessors)+L" cores";
  MEMORYSTATUSEX ms; ms.dwLength=sizeof(ms);
  if(GlobalMemoryStatusEx(&ms)) g_devRAM=std::to_wstring((int)((ms.ullTotalPhys+536870912ull)/1073741824ull))+L" GB";
  g_devRes=std::to_wstring(GetSystemMetrics(SM_CXSCREEN))+L" x "+std::to_wstring(GetSystemMetrics(SM_CYSCREEN));
}

static void loadAvatar(){
  std::wstring path=cacheDir()+L"\\avatar.jpg";
  if(GetFileAttributesW(path.c_str())==INVALID_FILE_ATTRIBUTES){
    HRSRC r=FindResourceW(nullptr,L"AVATAR_JPG",RT_RCDATA);
    HGLOBAL h=r?LoadResource(nullptr,r):nullptr; const void* p=h?LockResource(h):nullptr; DWORD n=r?SizeofResource(nullptr,r):0;
    if(p&&n){ std::ofstream f(wpath(path),std::ios::binary); f.write((const char*)p,n); }
  }
  g_avatar=Bitmap::FromFile(path.c_str());
  if(g_avatar&&g_avatar->GetLastStatus()!=Ok){ delete g_avatar; g_avatar=nullptr; }
}
void dashInit(){ loadAvatar(); gatherDevice(); }
void dashShutdown(){ if(g_avatar){ delete g_avatar; g_avatar=nullptr; } if(g_back){ delete g_back; g_back=nullptr; } }

static void renderScene(HDC dc,int W,int H){
  if(W<=0||H<=0)return;
  if(!g_back||g_bw!=W||g_bh!=H){ if(g_back){delete g_back;g_back=nullptr;} g_back=new Bitmap(W,H,PixelFormat32bppPARGB); g_bw=W; g_bh=H; }
  if(!g_back||g_back->GetLastStatus()!=Ok)return;
  Graphics g(g_back);
  g.SetSmoothingMode(SmoothingModeAntiAlias);
  g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
  SolidBrush bg(C_BG); FRect(g,&bg,0,0,W,H);

  double now=(GetTickCount64()-g_start)/1000.0;
  std::vector<Ev> evs; { EnterCriticalSection(&g_cs); evs.assign(g_events.begin(),g_events.end()); LeaveCriticalSection(&g_cs); }

  // ---- splash / loading ----
  if(g_splash){
    g_dirty.store(true);                                  // the loading animation wants every frame
    double el=(GetTickCount64()-g_splashT0)/1000.0;
    int steps=19; int done=(int)(el/0.14); if(done>steps)done=steps;
    int pct=(int)(100.0*done/steps);
    REAL m=48;
    Pen fb(C_LINE2,2);
    DLine(g,&fb,m,m,m+22,m); DLine(g,&fb,m,m,m,m+22);
    DLine(g,&fb,W-m,m,W-m-22,m); DLine(g,&fb,W-m,m,W-m,m+22);
    DLine(g,&fb,m,H-m,m+22,H-m); DLine(g,&fb,m,H-m,m,H-m-22);
    DLine(g,&fb,W-m,H-m,W-m-22,H-m); DLine(g,&fb,W-m,H-m,W-m,H-m-22);
    SolidBrush dotb(C_VIOLET); FEll(g,&dotb,m+6,m+24,9,9);
    drawStr(g,L"SENTINEL",fH,C_TX,m+24,m+16);
    drawStr(g,L"WATCH SUITE",fMono,C_TXM,m+24,m+38);
    SYSTEMTIME st; GetLocalTime(&st); wchar_t ts[16]; wsprintfW(ts,L"%02d:%02d:%02d",st.wHour,st.wMinute,st.wSecond);
    drawStrR(g,ts,fMono,C_TXD,W-m-6,m+16);
    wchar_t cc[16]; wsprintfW(cc,L"%02d / %02d",done,steps); drawStrR(g,cc,fMono,C_TXM,W-m-6,m+36);
    std::wstring title=scramble(L"INITIALIZING",el,0.12);
    drawStr(g,title.c_str(),fBig,C_TX,m+6,(REAL)H/2-72);
    drawStr(g,L"\u203a\u203a\u203a",fBig,C_VDIM,m+250,(REAL)H/2-72);
    REAL rcx=W-m-92, rcy=(REAL)H/2-42, rr=54;
    Pen rp(C_LINE2,1);
    DEll(g,&rp,rcx-rr,rcy-rr,rr*2,rr*2); DEll(g,&rp,rcx-rr*0.6f,rcy-rr*0.6f,rr*1.2f,rr*1.2f);
    REAL ang=(REAL)fmod(el*140.0,360.0);
    GraphicsPath pie; pie.AddPie((REAL)(rcx-rr),(REAL)(rcy-rr),(REAL)(rr*2),(REAL)(rr*2),(REAL)ang,(REAL)70);
    SolidBrush pb(Color(85,150,124,246)); g.FillPath(&pb,&pie);
    DLine(g,&rp,rcx,rcy,rcx+rr,rcy);
    std::wstring sub=L"resolving \u00b7 "+std::to_wstring(done)+L" / "+std::to_wstring(steps);
    drawStr(g,sub.c_str(),fMono,C_VIOLET,m+8,(REAL)H/2-16);
    wchar_t pc[8]; wsprintfW(pc,L"%d%%",pct); drawStrR(g,pc,fH,C_TX,rcx+rr,(REAL)H/2-20);
    REAL bx=m+8, bw=(REAL)W-m*2-16, prog=(REAL)done/steps;
    strokeRound(g,bx,(REAL)H/2+8,bw,7,3,C_LINE2); fillRound(g,bx,(REAL)H/2+8,bw*prog,7,3,C_VIOLET);
    REAL wy=(REAL)H-m-46; int bars=40; REAL step=((REAL)W-m*2)/(REAL)bars;
    for(int i=0;i<bars;i++){ int hh=8+(rand()%22); SolidBrush wb(Color(140,150,124,246));
      FRect(g,&wb,m+i*step,wy+(26-hh),3,(REAL)hh); }
    std::wstring hex; static const wchar_t* HX=L"0123456789ABCDEF";
    for(int i=0;i<20;i++){ hex+=HX[rand()%16]; hex+=HX[rand()%16]; hex+=L' '; }
    drawStr(g,hex.c_str(),fMono,C_TXM,m+8,(REAL)H-m-14);
    drawStrR(g,L"SENTINEL",fMono,C_TXM,W-m-6,(REAL)H-m-14);
    if(el*1000.0>g_splashMs){ g_splash=false; g_sessionStart=GetTickCount64(); }
    for(int y=0;y<H;y+=3){ SolidBrush sl(Color(28,0,0,0)); FRect(g,&sl,0,(REAL)y,(REAL)W,1); }
    Graphics wg(dc); wg.DrawImage(g_back,0,0); return;
  }

  updateQuoteState();
  if(g_quoteAlpha<255) g_dirty.store(true);               // the quote cross-fade wants every frame

  // ---- background particles (subtle motion in empty space) ----
  // vy is pixels per 30 fps frame; the repaint rate is not constant any more, so the step follows the time since the last paint (capped after a long pause)
  static double lastNow=-1; const float k=lastNow<0?1.f:(float)std::min(6.0,(now-lastNow)*30.0); lastNow=now;
  if(g_parts.empty()){ for(int i=0;i<14;i++){ Part pt; pt.x=(float)(SIDE_W+(W>SIDE_W?rand()%(W-SIDE_W):0));
    pt.y=(float)(H>0?rand()%H:0); pt.vy=0.15f+(rand()%50)/120.0f; pt.a=18+rand()%40; g_parts.push_back(pt);} }
  for(auto&pt:g_parts){ pt.y-=pt.vy*k; if(pt.y<BAR_H){ pt.y=(float)H; pt.x=(float)(SIDE_W+(W>SIDE_W?rand()%(W-SIDE_W):0)); }
    SolidBrush pb(Color(pt.a,150,190,255)); FEll(g,&pb,pt.x,pt.y,2.5f,2.5f); }

  // ---- sidebar ----
  SolidBrush sb(C_BG2); FRect(g,&sb,0,0,SIDE_W,H);
  Pen edge(C_VDIM,2); DLine(g,&edge,SIDE_W-1,0,SIDE_W-1,H);
  SolidBrush dot(C_VIOLET); FEll(g,&dot,18,20,9,9);
  drawStr(g,L"SENTINEL",fH,C_TX,34,15);
  drawStr(g,L"WATCH SUITE",fMono,C_TXM,34,36);

  const wchar_t* nav[4]={L"Dashboard",L"Protection",L"Events",L"Updates"};
  for(int i=0;i<4;i++){ REAL y=78+i*46;
    g_rNav[i]={12,(LONG)y,SIDE_W-12,(LONG)y+40};
    if(i==g_tab){ fillRound(g,12,y,SIDE_W-24,40,9,Color(48,150,124,246));
      SolidBrush vb(C_VIOLET); FRect(g,&vb,12,y+6,3,28); }
    else if(hov(g_rNav[i])) fillRound(g,12,y,SIDE_W-24,40,9,Color(24,150,124,246));
    drawStr(g,nav[i],fBody,(i==g_tab||hov(g_rNav[i]))?C_TX:C_TXD,30,y+9);
  }
  { REAL qy=(REAL)H-214;
    drawStr(g, g_afkNow?L"\u25cf AFK":L"\u25cf LIVE", fMono, g_afkNow?C_VDIM:C_GREEN, 18, qy);
    if(!g_curQuote.empty()){
      const REAL qBoxH=150, qBoxW=(REAL)SIDE_W-36;
      RectF qc(16,qy+22,(REAL)SIDE_W-30,qBoxH); g.SetClip(qc);
      double sel=(double)(GetTickCount64()-g_quoteChangeT0)/1000.0;
      bool glitch=(!g_afkNow && sel<0.75);
      if(glitch) g_dirty.store(true);
      Color qcCol = g_afkNow?Color(g_quoteAlpha,198,206,226):Color(255,198,206,226);
      const RichFont* rf=fitRich(g,g_curQuote,qBoxW,qBoxH-8,g_ladderSide);   // -8 keeps a little breathing room
      if(rf) drawRich(g,g_curQuote,18,qy+26,qBoxW,qcCol, glitch?sel:-1, 0.03, rf->n,rf->b,rf->lh);
      else   drawRich(g,g_curQuote,18,qy+26,qBoxW,qcCol, glitch?sel:-1, 0.03);
      g.ResetClip(); } }
  { std::wstring gl; static const wchar_t* G=L"01<>/\\#*+=x"; for(int i=0;i<12;i++) gl+=G[rand()%10];
    drawStr(g,gl.c_str(),fMono,Color(70,120,160,210),18,H-42);
    drawStr(g,L"SENTINEL // ONLINE",fMono,C_TXM,18,H-24); }

  // ---- top bar ----
  REAL cx=SIDE_W+24;
  const wchar_t* titles[4]={L"DASHBOARD",L"PROTECTION",L"EVENTS",L"UPDATES"};
  double tel=(GetTickCount64()-g_tabT0)/1000.0;
  std::wstring tt = tel<0.6 ? scramble(titles[g_tab],tel,0.04) : std::wstring(titles[g_tab]);
  if(tel<0.6) g_dirty.store(true);
  drawStr(g,tt.c_str(),fTitle,C_TX,cx,16);
  g_rScanTop={(LONG)(cx+232),12,(LONG)(cx+348),38};
  fillRound(g,cx+232,12,116,26,7, hov(g_rScanTop)?C_VIOLET:C_VDIM);
  drawStr(g,L"Quick scan",fMono,Color(255,255,255,255),cx+252,16);
  drawStr(g,L"\u203a\u203a\u203a",fTitle,C_VDIM,cx+180,16);
  // clock
  SYSTEMTIME st; GetLocalTime(&st); wchar_t clk[16]; wsprintfW(clk,L"%02d:%02d:%02d",st.wHour,st.wMinute,st.wSecond);
  drawStrR(g,clk,fMono,C_TXD,W-150,20);
  // version chip + window buttons (fullscreen is F11 now, not a button -- see WM_KEYDOWN)
  fillRound(g,W-130,14,44,24,6,C_PANEL); drawStr(g,SW_VERSION_W,fMono,C_VIOLET,W-122,17);
  g_rMin={W-76,14,W-50,38}; g_rClose={W-44,14,W-18,38};
  fillRound(g,W-76,14,26,24,6,hov(g_rMin)?C_LINE2:C_PANEL); drawStr(g,L"\u2013",fBody,hov(g_rMin)?C_TX:C_TXD,W-68,15);
  fillRound(g,W-44,14,26,24,6,hov(g_rClose)?C_RED:C_PANEL); drawStr(g,L"\u00d7",fBody,hov(g_rClose)?Color(255,255,255,255):C_TXD,W-37,15);

  REAL top=BAR_H+14, W2=W-cx-24;

  auto seconds=[&](){ ULONGLONG base=g_sessionStart?g_sessionStart:g_start; unsigned long long s=(GetTickCount64()-base)/1000;
    wchar_t b[16]; wsprintfW(b,L"%02u:%02u:%02u",(unsigned)(s/3600),(unsigned)(s%3600/60),(unsigned)(s%60)); return std::wstring(b); };

  if(g_tab==0){ // DASHBOARD
    REAL cw=(W2-32)/3;
    struct{const wchar_t* l; std::wstring v; const wchar_t* h;} sc[3]={
      {L"SESSION",seconds(),L"this run"},
      {L"PROCESSES SEEN",std::to_wstring(g_procCount.load()),L"since launch"},
      {L"NEW FILES SEEN",std::to_wstring(g_fileCount.load()),L"watched folders"}};
    for(int i=0;i<3;i++){ REAL x=cx+i*(cw+16); card(g,x,top,cw,96); label(g,sc[i].l,x+16,top+14);
      drawStr(g,sc[i].v.c_str(),fBig,C_TX,x+16,top+34);
      drawStr(g,sc[i].h,fMono,C_TXM,x+16,top+74); }
    // events + side column
    REAL fy=top+112, fh=H-fy-20, fw=W2*0.62f;
    card(g,cx,fy,fw,fh); label(g,L"RECENT EVENTS",cx+16,fy+14);
    SolidBrush lg(C_GREEN); FEll(g,&lg,cx+fw-118,fy+16,7,7);
    drawStr(g,L"MONITORING",fMono,C_GREEN,cx+fw-104,fy+12);
    { REAL ry=fy+44; int n=0;
      for(auto&e:evs){ if(n++>=9)break;
        Color tc=C_VIOLET; if(e.tag==L"WARN")tc=C_RED; else if(e.tag==L"FILE")tc=C_GREEN; else if(e.tag==L"SYS")tc=C_TXM;
        strokeRound(g,cx+16,ry,52,18,4,tc); drawStr(g,e.tag.c_str(),fMono,tc,cx+22,ry+1);
        { RectF nc(cx+78,ry-4,fw-78-62,36); g.SetClip(nc);
          drawStr(g,e.name.c_str(),fBody,C_TX,cx+78,ry-2);
          drawStr(g,e.path.c_str(),fMono,C_TXM,cx+78,ry+14); g.ResetClip(); }
        drawStrR(g,e.tm,fMono,C_TXM,cx+fw-16,ry);
        ry+=42; }
    }
    // radar + autoruns
    REAL rx=cx+fw+16, rw=W2-fw-16;
    card(g,rx,fy,rw,fh*0.58f); label(g,L"WATCHERS",rx+16,fy+14);
    REAL ccx=rx+rw/2, ccy=fy+fh*0.30f, rad=44;
    Pen rp(C_LINE2,1); DEll(g,&rp,ccx-rad,ccy-rad,rad*2,rad*2);
    DEll(g,&rp,ccx-rad*0.6f,ccy-rad*0.6f,rad*1.2f,rad*1.2f);
    REAL ang=(REAL)fmod(now*90.0,360.0);
    GraphicsPath pie; pie.AddPie((REAL)(ccx-rad),(REAL)(ccy-rad),(REAL)(rad*2),(REAL)(rad*2),(REAL)ang,(REAL)60);
    SolidBrush pb(Color(85,150,124,246)); g.FillPath(&pb,&pie);
    drawStr(g,L"4 / 4 active",fBody,C_TX,rx+16,fy+fh*0.44f);
    card(g,rx,fy+fh*0.62f,rw,fh*0.38f); label(g,L"AUTORUNS",rx+16,fy+fh*0.62f+14);
    drawStr(g,std::to_wstring((int)g_known.size()).c_str(),fBig,C_TX,rx+16,fy+fh*0.62f+32);
  }
  else if(g_tab==1){ // PROTECTION
    card(g,cx,top,W2,H-top-72); label(g,L"WATCHERS",cx+16,top+14);
    { bool on=g_tTokens.load(); Color sc=on?C_GREEN:C_TXM;
      drawStrR(g,on?L"TOKEN GUARD \u25cf ON":L"TOKEN GUARD \u25cb OFF",fMono,sc,cx+W2-16,top+12);
      if(g_tokenAlerts.load()>0)
        drawStrR(g,(std::to_wstring(g_tokenAlerts.load())+L" token alert(s)").c_str(),fMono,C_RED,cx+W2-16,top+28); }
    const REAL rowH=std::max<REAL>(34,std::min<REAL>(46,(H-top-72-78)/(TOG_N-1)));      // the rows must still fit the card at the smallest window (11 rows: 35 px each at 780x560)
    for(int i=0;i<TOG_N;i++){ REAL y=top+44+i*rowH; bool on=g_toggles[i].v->load();
      drawStr(g,g_toggles[i].name,fBody,C_TX,cx+16,y);
      drawStr(g,g_toggles[i].desc,fMono,C_TXM,cx+16,y+18);
      REAL swx=cx+W2-80, swy=y+2;
      g_rTog[i]={(LONG)swx,(LONG)swy,(LONG)swx+46,(LONG)swy+26};
      fillRound(g,swx,swy,46,26,13, on?C_VDIM:Color(255,12,13,24));
      if(!on)strokeRound(g,swx,swy,46,26,13,C_LINE2);
      SolidBrush kn(on?Color(255,255,255,255):C_TXM);
      FEll(g,&kn, on?swx+22:swx+3, swy+3, 20,20);
      if(i<TOG_N-1){ const REAL ly=y+std::min<REAL>(38,rowH-1); Pen ln(C_LINE,1); DLine(g,&ln,cx+16,ly,cx+W2-16,ly); }
    }
    // scan button
    g_rScan={(LONG)cx,(LONG)(H-58),(LONG)cx+150,(LONG)(H-24)};
    fillRound(g,cx,H-58,150,34,9, hov(g_rScan)?C_VIOLET:C_VDIM); drawStr(g,L"Quick scan",fBody,Color(255,255,255,255),cx+38,H-52);
    g_rFw={(LONG)(cx+164),(LONG)(H-58),(LONG)(cx+164+210),(LONG)(H-24)};
    fillRound(g,cx+164,H-58,210,34,9, hov(g_rFw)?C_LINE2:C_PANEL); strokeRound(g,cx+164,H-58,210,34,9,C_LINE);
    drawStr(g,L"Open Windows Firewall",fBody,C_TX,cx+180,H-52);
  }
  else if(g_tab==2){ // EVENTS
    card(g,cx,top,W2,H-top-20); label(g,L"EVENT STREAM",cx+16,top+14);
    SolidBrush lg(C_GREEN); FEll(g,&lg,cx+W2-70,top+16,7,7); drawStr(g,L"LIVE",fMono,C_GREEN,cx+W2-56,top+12);
    int total=(int)evs.size(); int rows=(int)((H-top-20-44)/40); if(rows<1)rows=1;
    int maxScroll=total-rows; if(maxScroll<0)maxScroll=0; if(g_evScroll>maxScroll)g_evScroll=maxScroll; if(g_evScroll<0)g_evScroll=0;
    REAL ry=top+44; g_evRowCount=0;
    for(int i=g_evScroll;i<total && i<g_evScroll+rows;i++){ auto&e=evs[i]; bool warn=(e.tag==L"WARN");
      Color tc=C_VIOLET; if(warn)tc=C_RED; else if(e.tag==L"FILE")tc=C_GREEN; else if(e.tag==L"SYS")tc=C_TXM;
      if(g_evRowCount<40){ g_evRowR[g_evRowCount]={(LONG)(cx+12),(LONG)(ry-6),(LONG)(cx+W2-12),(LONG)(ry+32)};
        g_evRowPath[g_evRowCount]=e.path; g_evRowWarn[g_evRowCount]=warn; g_evRowCount++; }
      if(warn && hov({(LONG)(cx+12),(LONG)(ry-6),(LONG)(cx+W2-12),(LONG)(ry+32)}))
        fillRound(g,cx+12,ry-6,W2-24,38,7,Color(26,248,116,140));
      strokeRound(g,cx+16,ry,52,18,4,tc); drawStr(g,e.tag.c_str(),fMono,tc,cx+22,ry+1);
      { RectF nc(cx+78,ry-4,W2-78-72,36); g.SetClip(nc);
        drawStr(g,e.name.c_str(),fBody,C_TX,cx+78,ry-2);
        drawStr(g,e.path.c_str(),fMono,C_TXM,cx+78,ry+14); g.ResetClip(); }
      drawStrR(g,e.tm,fMono,C_TXM,cx+W2-16,ry); ry+=40; }
    if(total>rows){ REAL trackH=(REAL)(H-top-20-44); REAL th=trackH*rows/total; REAL ty=top+44+trackH*g_evScroll/total;
      fillRound(g,cx+W2-8,top+44,3,trackH,1,C_LINE); fillRound(g,cx+W2-8,ty,3,th,1,C_VDIM); }
    drawStr(g,L"scroll to see more · click a red WARN row to block it in Windows Firewall",fMono,C_TXM,cx+16,(REAL)H-30);
  }
  else { // UPDATES & CREDITS
    REAL logW=W2*0.60f, rx=cx+logW+16, rw=W2-logW-16;
    card(g,cx,top,logW,H-top-20); label(g,L"UPDATE LOG",cx+16,top+14);
    REAL ly=top+44;
    for(auto&nt:g_notes){ if(ly>H-38)break;
      Color kc=(wcscmp(nt.kind,L"FIX")==0)?C_GREEN:(wcscmp(nt.kind,L"IMPROVE")==0)?C_VIOLET:C_VDIM;
      strokeRound(g,cx+16,ly,66,18,4,kc); drawStr(g,nt.kind,fMono,kc,cx+21,ly+1);
      drawStr(g,nt.ver,fMono,C_TXM,cx+90,ly+1);
      { RectF tc(cx+138,ly-3,logW-138-16,22); g.SetClip(tc);
        drawStr(g,nt.text,fBody,C_TX,cx+138,ly-2); g.ResetClip(); }
      ly+=32; }
    // Creator (hover-zoom) grows while Device Info shrinks, smoothly
    REAL totalH=(REAL)H-top-20, gap=16;
    // Social row geometry. The card height is derived from these rather than
    // being its own constant -- that is what let the third row (Discord) fall
    // outside the card at rest: rows ended at top+166, the card at top+150.
    const REAL socY0=96, socY0G=24;      // first row offset; +G when hovered
    const REAL socStep=24, socStepG=8;   // row pitch;        +G when hovered
    const REAL socRowH=22, socPadB=16;   // row height, bottom padding
    auto creatorNeed=[&](REAL t){ return socY0+t*socY0G + 2*(socStep+t*socStepG) + socRowH + socPadB; };
    REAL base=creatorNeed(0.0f);                  // 182: last row + padding fits
    REAL grow=(creatorNeed(1.0f)+32.0f)-base;     // hovered keeps a little slack
    { REAL tgt=hov(g_rCreator)?1.0f:0.0f; g_creatorT+=(tgt-g_creatorT)*0.18f;
      if(g_creatorT<0.002f)g_creatorT=0; if(g_creatorT>0.998f)g_creatorT=1;
      if(g_creatorT!=tgt) g_dirty.store(true); }          // still easing toward the hover state
    REAL ch=base+g_creatorT*grow;
    g_rCreator={(LONG)rx,(LONG)top,(LONG)(rx+rw),(LONG)(top+ch)};
    card(g,rx,top,rw,ch); label(g,L"CREATOR",rx+16,top+14);
    REAL av=44+g_creatorT*20;
    if(g_avatar){ GraphicsPath cp; cp.AddEllipse((REAL)(rx+16),(REAL)(top+38),(REAL)av,(REAL)av); g.SetClip(&cp);
      DImg(g,g_avatar,(REAL)rx+16,(REAL)top+38,av,av); g.ResetClip();
      Pen ap(C_LINE2,1); DEll(g,&ap,rx+16,top+38,av,av); }
    drawStr(g,L"Lunaris",fH,C_TX,rx+72+g_creatorT*10,top+42);
    drawStr(g,L"OPERATOR",fMono,C_GREEN,rx+72+g_creatorT*10,top+66);
    const wchar_t* soc[3]={L"YouTube  @lIIusionator",L"Roblox  Profile",L"Discord  liiusionator"};
    for(int i=0;i<3;i++){ REAL y=top+socY0+g_creatorT*socY0G+i*(socStep+g_creatorT*socStepG);
      g_rSoc[i]={(LONG)(rx+16),(LONG)y,(LONG)(rx+rw-16),(LONG)(y+socRowH)};
      SolidBrush vb(C_VIOLET); FEll(g,&vb,rx+20,y+7,8,8);
      { RectF sc(rx+34,y,rw-40,socRowH); g.SetClip(sc); drawStr(g,soc[i],fMono,hov(g_rSoc[i])?C_TX:C_TXD,rx+34,y+2); g.ResetClip(); } }
    // Device Info (shrinks as Creator grows)
    REAL dy=top+ch+gap, dh=totalH-ch-gap;
    if(dh>44){ card(g,rx,dy,rw,dh); label(g,L"DEVICE INFO",rx+16,dy+14);
      ULONGLONG up=GetTickCount64()/1000; wchar_t ub[32]; wsprintfW(ub,L"%uh %um",(unsigned)(up/3600),(unsigned)(up%3600/60));
      struct{ const wchar_t* k; std::wstring v; } rows[]={
        {L"HOST",g_devHost},{L"USER",g_userName},{L"CPU",g_devCPU},{L"MEMORY",g_devRAM},{L"DISPLAY",g_devRes},{L"UPTIME",ub}};
      REAL ry2=dy+44;
      for(auto&r:rows){ if(ry2>dy+dh-16)break;
        drawStr(g,r.k,fMono,C_TXM,rx+16,ry2);
        { RectF vc(rx+92,ry2-2,rw-92-16,22); g.SetClip(vc); drawStrR(g,r.v,fBody,C_TX,rx+rw-16,ry2-2); g.ResetClip(); }
        ry2+=28; } }
  }

  // ---- press flash animation ----
  { double fel=(GetTickCount64()-g_flashT0)/1000.0;
    if(fel>=0&&fel<0.18&&g_flashR.right>g_flashR.left){
      int a=(int)(80*(1.0-fel/0.18));
      fillRound(g,(REAL)g_flashR.left,(REAL)g_flashR.top,(REAL)(g_flashR.right-g_flashR.left),(REAL)(g_flashR.bottom-g_flashR.top),8,Color(a,255,255,255));
      g_dirty.store(true); } }

  // ---- CRT scanlines ----
  for(int y=0;y<H;y+=3){ SolidBrush sl(Color(22,0,0,0)); FRect(g,&sl,0,y,W,1); }

  // AFK content no longer draws here -- it's a dedicated always-on-top
  // window now (g_afkWin, driven from g_msg's timer) so the screensaver keeps
  // working even with this window closed/unfocused/behind other windows.
  Graphics wg(dc); wg.DrawImage(g_back,0,0);
}

// Work-area fullscreen toggle for the borderless dashboard window. Not a
// real Win32 "maximize" (this window has no native chrome/max button to
// begin with) -- just resizes to the work area and back, remembering the
// windowed rect to restore to.
static void toggleFullscreen(HWND h){
  if(!g_fullscreen){
    GetWindowRect(h,&g_preFullRect);
    RECT wa; SystemParametersInfoW(SPI_GETWORKAREA,0,&wa,0);
    SetWindowPos(h,0,wa.left,wa.top,wa.right-wa.left,wa.bottom-wa.top,SWP_NOZORDER);
    g_fullscreen=true;
  } else {
    int w=g_preFullRect.right-g_preFullRect.left, ht=g_preFullRect.bottom-g_preFullRect.top;
    if(w>0&&ht>0) SetWindowPos(h,0,g_preFullRect.left,g_preFullRect.top,w,ht,SWP_NOZORDER);
    g_fullscreen=false;
  }
}
LRESULT CALLBACK DashProc(HWND h,UINT m,WPARAM w,LPARAM l){
  switch(m){
  case WM_CREATE: g_splash=true; g_splashT0=GetTickCount64(); g_tabT0=GetTickCount64(); g_dirty.store(true);
    SetTimer(h,TMR_RENDER,33,0); return 0;
  case WM_TIMER: if(w==TMR_RENDER&&!IsIconic(h)&&!afkScreenVisible()){   // the AFK screen covers us: do not burn CPU drawing behind it
      // A GDI+ repaint of the whole window costs about a core at 30 fps. Repaint at once when something changed (mouse, click, a new event, or an
      // animation that keeps g_dirty set); otherwise only the ambient motion is left, and a few frames a second are plenty for that.
      static ULONGLONG lastDraw=0; const ULONGLONG t=GetTickCount64();
      if(!g_dirty.exchange(false) && t-lastDraw<(GetForegroundWindow()==h?IDLE_FRAME_MS:IDLE_BG_MS)) return 0;
      lastDraw=t;
      RECT rc; GetClientRect(h,&rc);
      if(rc.right>0&&rc.bottom>0){ HDC dc=GetDC(h); renderScene(dc,rc.right,rc.bottom); ReleaseDC(h,dc);} } return 0;
  case WM_GETMINMAXINFO:{ MINMAXINFO* mmi=(MINMAXINFO*)l;   // clamp how small the user can drag it
      mmi->ptMinTrackSize.x=DASH_MINW; mmi->ptMinTrackSize.y=DASH_MINH; return 0; }
  case WM_NCHITTEST:{ const LONG B=8;                       // 8px grab border -> resize a borderless popup
      POINT p={(LONG)(short)LOWORD(l),(LONG)(short)HIWORD(l)}; ScreenToClient(h,&p);
      RECT rc; GetClientRect(h,&rc);
      if(inR(g_rClose,p.x,p.y)||inR(g_rMin,p.x,p.y)||inR(g_rScanTop,p.x,p.y)) return HTCLIENT; // buttons win over the strip
      bool L_=p.x<B, R_=p.x>=rc.right-B, T_=p.y<B, D_=p.y>=rc.bottom-B;
      if(T_&&L_)return HTTOPLEFT;    if(T_&&R_)return HTTOPRIGHT;
      if(D_&&L_)return HTBOTTOMLEFT; if(D_&&R_)return HTBOTTOMRIGHT;
      if(L_)return HTLEFT; if(R_)return HTRIGHT; if(T_)return HTTOP; if(D_)return HTBOTTOM;
      return HTCLIENT; }
  case WM_ERASEBKGND: return 1;
  case WM_PAINT:{ PAINTSTRUCT ps; BeginPaint(h,&ps); RECT rc; GetClientRect(h,&rc);
      if(rc.right>0&&rc.bottom>0) renderScene(ps.hdc,rc.right,rc.bottom); EndPaint(h,&ps); return 0; }
  case WM_MOUSEWHEEL:{ int d=GET_WHEEL_DELTA_WPARAM(w); if(g_tab==2){ g_evScroll+=(d>0?-3:3); if(g_evScroll<0)g_evScroll=0; g_dirty.store(true);} return 0; }
  case WM_KEYDOWN: if(w==VK_F11){ toggleFullscreen(h); g_dirty.store(true); } return 0;
  case WM_MOUSEMOVE:{ g_mx=(int)(short)LOWORD(l); g_my=(int)(short)HIWORD(l); g_dirty.store(true);
    TRACKMOUSEEVENT tme={sizeof(tme),TME_LEAVE,h,0}; TrackMouseEvent(&tme); return 0; }
  case WM_MOUSELEAVE: g_mx=g_my=-1; g_dirty.store(true); return 0;
  case WM_LBUTTONDOWN:{ int x=LOWORD(l),y=HIWORD(l); g_dirty.store(true);
    auto flash=[&](RECT r){ g_flashR=r; g_flashT0=GetTickCount64(); };
    if(inR(g_rClose,x,y)){ DestroyWindow(h); return 0; }
    if(inR(g_rMin,x,y)){ flash(g_rMin); ShowWindow(h,SW_MINIMIZE); return 0; }
    if(inR(g_rScanTop,x,y)){ doScan(true); flash(g_rScanTop); return 0; }
    for(int i=0;i<4;i++) if(inR(g_rNav[i],x,y)){ g_tab=i; g_tabT0=GetTickCount64(); flash(g_rNav[i]); return 0; }
    if(g_tab==1){
      for(int i=0;i<TOG_N;i++) if(inR(g_rTog[i],x,y)){ g_toggles[i].v->store(!g_toggles[i].v->load()); saveSettings(); autostartApply(); flash(g_rTog[i]); return 0; }
      if(inR(g_rScan,x,y)){ doScan(true); flash(g_rScan); return 0; }
      if(inR(g_rFw,x,y)){ flash(g_rFw);
        // firewall.cpl's registered verb is "cplopen", not "open" -- asking for
        // "open" finds no matching verb and ShellExecute silently no-ops, which
        // is why the button looked like it did nothing. control.exe accepts the
        // cpl name as a plain parameter, sidestepping verb lookup entirely.
        HINSTANCE r=ShellExecuteW(0,L"open",L"control.exe",L"firewall.cpl",0,SW_SHOW);
        if((INT_PTR)r<=32) r=ShellExecuteW(0,L"open",L"rundll32.exe",L"shell32.dll,Control_RunDLL firewall.cpl",0,SW_SHOW);
        if((INT_PTR)r<=32) MessageBoxW(h,L"Could not open Windows Firewall settings.",L"Sentinel",MB_OK|MB_ICONWARNING);
        return 0; } }
    if(g_tab==2){ for(int i=0;i<g_evRowCount;i++) if(inR(g_evRowR[i],x,y) && g_evRowWarn[i] && !g_evRowPath[i].empty()){
      std::wstring m=L"Block this app's internet access using Windows Firewall?\n\n"+g_evRowPath[i]+L"\n\n(You'll get a UAC prompt to apply the rule.)";
      if(MessageBoxW(h,m.c_str(),L"Sentinel \u2014 Firewall",MB_YESNO|MB_ICONWARNING)==IDYES) blockInFirewall(g_evRowPath[i]);
      return 0; } }
    if(g_tab==3){ for(int i=0;i<3;i++) if(inR(g_rSoc[i],x,y)){ flash(g_rSoc[i]);
        if(i==2){ if(OpenClipboard(h)){ EmptyClipboard();
            const wchar_t* d=L"liiusionator"; HGLOBAL hm=GlobalAlloc(GMEM_MOVEABLE,(wcslen(d)+1)*2);
            memcpy(GlobalLock(hm),d,(wcslen(d)+1)*2); GlobalUnlock(hm);
            SetClipboardData(CF_UNICODETEXT,hm); CloseClipboard(); } }
        else ShellExecuteW(0,L"open",g_socUrl[i],0,0,SW_SHOW); return 0; } }
    // drag from top bar
    if(y<BAR_H){ ReleaseCapture(); SendMessageW(h,WM_NCLBUTTONDOWN,HTCAPTION,0); }
    return 0; }
  case WM_SIZE:
    if(w==SIZE_MINIMIZED){ SetTimer(h,TMR_RENDER,200,0); g_frameMs=200; }       // 5 fps minimized
    else { SetTimer(h,TMR_RENDER,33,0); g_frameMs=33; g_dirty.store(true);      // 30 fps active
      g_dashW=LOWORD(l); g_dashH=HIWORD(l); }        // borderless popup: client size == window size
    return 0;
  case WM_EXITSIZEMOVE: saveSettings(); return 0;     // write size to JSON once, after the drag ends
  case WM_CLOSE: DestroyWindow(h); return 0;                   // close -> headless tray
  case WM_DESTROY: KillTimer(h,TMR_RENDER); g_dash=nullptr; return 0;
  }
  return DefWindowProcW(h,m,w,l);
}
void openDash(){
  if(!g_dash){
    g_dash=CreateWindowExW(WS_EX_APPWINDOW,CLS_DASH,L"Sentinel Watch Suite",
      WS_POPUP|WS_MINIMIZEBOX,CW_USEDEFAULT,CW_USEDEFAULT,g_dashW,g_dashH,0,0,g_hInst,0);
    // center at the persisted size
    RECT wa; SystemParametersInfoW(SPI_GETWORKAREA,0,&wa,0);
    SetWindowPos(g_dash,0,(wa.right-g_dashW)/2,(wa.bottom-g_dashH)/2,0,0,SWP_NOSIZE|SWP_NOZORDER);
  }
  ShowWindow(g_dash,SW_SHOW); SetForegroundWindow(g_dash); UpdateWindow(g_dash);
}
