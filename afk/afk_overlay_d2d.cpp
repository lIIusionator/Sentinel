// afk_overlay_d2d.cpp -- Direct2D / DirectWrite HUD for the GPU back-end (see afk_overlay_d2d.h).
//
// Drawing order, bottom to top (same as the GDI+ renderer, so both paths look alike):
//   video (already in the back buffer) -> fade to background colour while the video is still revealing
//   -> vignette -> starfield (dimmed inside the lens circle) -> bottom gradient -> clock -> quote -> hint
// d2d1.dll / dwrite.dll are loaded at run time: if either is missing the video keeps playing without the HUD.
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include "afk_overlay_d2d.h"
#include "com_ptr.h"
#include <d3d11.h>
#include <dxgi.h>
#include <d2d1_1.h>
#include <d2d1helper.h>
#include <d2derr.h>
#include <dwrite.h>
#include <vector>
#include <string>

typedef HRESULT (WINAPI *PFN_D2D1CreateFactory_t)(D2D1_FACTORY_TYPE,REFIID,const D2D1_FACTORY_OPTIONS*,void**);
typedef HRESULT (WINAPI *PFN_DWriteCreateFactory_t)(DWRITE_FACTORY_TYPE,REFIID,IUnknown**);

struct AfkOverlayD2D::Impl{
  bool failed=false;                 // permanent: the HUD stays off for this back-end (the video keeps playing)
  int softFails=0; ULONGLONG retryAt=0;   // transient failures in a row, and the GetTickCount64() before which nothing is retried
  void* devKey=nullptr;
  Com<ID2D1Factory1> factory; Com<ID2D1Device> d2dDev; Com<ID2D1DeviceContext> dc;
  Com<IDWriteFactory> dw;
  Com<ID2D1Bitmap1> target; void* targetKey=nullptr;
  Com<ID2D1SolidColorBrush> solid; Com<ID2D1RadialGradientBrush> vig; Com<ID2D1LinearGradientBrush> bottom;
  unsigned gradW=0,gradH=0;
  Com<IDWriteTextFormat> clockFmt,hintFmt; float fmtScale=0.f;
  Com<IDWriteTextLayout> quoteLayout; std::wstring quoteKey; float quoteH=0.f;
  std::vector<AfkStar> stars; AfkRng rng; unsigned starW=0,starH=0;

  // A failed step. Permanent causes (no d2d1 / dwrite on this PC) switch the HUD off for good; anything else drops every Direct2D object and tries again
  // after a short, growing pause, so a hiccup right after a resize or a device recovery does not cost the text for the rest of the session.
  void fail(const wchar_t* step,HRESULT hr,bool permanent=false){
    if(permanent||softFails<4) logLine(std::wstring(L"[AFK-GPU] HUD overlay: ")+step+L" failed, hr="+hrHex(hr)+(permanent?L" -- the video keeps playing without the HUD":L" -- will retry"));
    dropAll();
    if(permanent||++softFails>=6){ failed=true; if(!permanent) logLine(L"[AFK-GPU] HUD overlay disabled after repeated failures (the video keeps playing)"); return; }
    retryAt=GetTickCount64()+500ULL*(ULONGLONG)softFails; }

  void dropTargets(){ if(dc) dc->SetTarget(nullptr); target.reset(); targetKey=nullptr; }
  void dropAll(){ dropTargets(); quoteLayout.reset(); quoteKey.clear(); clockFmt.reset(); hintFmt.reset(); fmtScale=0.f;
    solid.reset(); vig.reset(); bottom.reset(); gradW=gradH=0; dc.reset(); d2dDev.reset(); factory.reset(); dw.reset(); devKey=nullptr; }

  bool ensureDevice(void* d3dDevice){
    if(dc && devKey==d3dDevice) return true;
    dropAll();
    HMODULE d2d=LoadLibraryW(L"d2d1.dll"), dwl=LoadLibraryW(L"dwrite.dll");
    if(!d2d||!dwl){ fail(L"LoadLibrary(d2d1/dwrite)",HRESULT_FROM_WIN32(GetLastError()),true); return false; }
    PFN_D2D1CreateFactory_t cf=(PFN_D2D1CreateFactory_t)(void*)GetProcAddress(d2d,"D2D1CreateFactory");
    PFN_DWriteCreateFactory_t wf=(PFN_DWriteCreateFactory_t)(void*)GetProcAddress(dwl,"DWriteCreateFactory");
    if(!cf||!wf){ fail(L"resolve D2D1CreateFactory/DWriteCreateFactory",E_NOINTERFACE,true); return false; }
    D2D1_FACTORY_OPTIONS fo={}; fo.debugLevel=D2D1_DEBUG_LEVEL_NONE;
    // Multi-threaded: the D3D device is shared with Media Foundation's decoder threads.
    HRESULT hr=cf(D2D1_FACTORY_TYPE_MULTI_THREADED,__uuidof(ID2D1Factory1),&fo,(void**)factory.put());
    if(FAILED(hr)||!factory){ fail(L"D2D1CreateFactory",hr); return false; }
    Com<IDXGIDevice> dxgi; hr=((ID3D11Device*)d3dDevice)->QueryInterface(__uuidof(IDXGIDevice),(void**)dxgi.put());
    if(FAILED(hr)){ fail(L"QueryInterface(IDXGIDevice)",hr); return false; }
    hr=factory->CreateDevice(dxgi.get(),d2dDev.put()); if(FAILED(hr)||!d2dDev){ fail(L"ID2D1Factory1::CreateDevice",hr); return false; }
    hr=d2dDev->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE,dc.put()); if(FAILED(hr)||!dc){ fail(L"CreateDeviceContext",hr); return false; }
    dc->SetDpi(96.f,96.f);
    hr=wf(DWRITE_FACTORY_TYPE_SHARED,__uuidof(IDWriteFactory),(IUnknown**)dw.put()); if(FAILED(hr)||!dw){ fail(L"DWriteCreateFactory",hr); return false; }
    hr=dc->CreateSolidColorBrush(D2D1::ColorF(0,0,0,1),solid.put()); if(FAILED(hr)){ fail(L"CreateSolidColorBrush",hr); return false; }
    devKey=d3dDevice; return true; }

  bool ensureTarget(void* backBuffer){
    if(target && targetKey==backBuffer) return true;
    dropTargets();
    Com<IDXGISurface> surf; HRESULT hr=((ID3D11Texture2D*)backBuffer)->QueryInterface(__uuidof(IDXGISurface),(void**)surf.put());
    if(FAILED(hr)){ fail(L"QueryInterface(IDXGISurface)",hr); return false; }
    D2D1_BITMAP_PROPERTIES1 bp={}; bp.pixelFormat.format=DXGI_FORMAT_B8G8R8A8_UNORM; bp.dpiX=96.f; bp.dpiY=96.f;
    bp.bitmapOptions=D2D1_BITMAP_OPTIONS_TARGET|D2D1_BITMAP_OPTIONS_CANNOT_DRAW;
    // The swap chain ignores alpha, and the video processor's alpha is whatever the driver wrote: treat it as opaque.
    bp.pixelFormat.alphaMode=D2D1_ALPHA_MODE_IGNORE;
    hr=dc->CreateBitmapFromDxgiSurface(surf.get(),&bp,target.put());
    if(FAILED(hr)){ bp.pixelFormat.alphaMode=D2D1_ALPHA_MODE_PREMULTIPLIED; hr=dc->CreateBitmapFromDxgiSurface(surf.get(),&bp,target.put()); }
    if(FAILED(hr)||!target){ fail(L"CreateBitmapFromDxgiSurface",hr); return false; }
    targetKey=backBuffer; return true; }

  bool ensureBrushes(unsigned W,unsigned H){
    if(gradW==W && gradH==H) return true;        // tried for this size: a brush that failed stays absent instead of being retried (and logged) every frame
    vig.reset(); bottom.reset(); gradW=W; gradH=H;
    AfkVigStop st[32]; float outerR=0.f; int n=afkVignetteStops((int)W,(int)H,200.0f/255.0f,st,32,&outerR);
    if(n>0){ D2D1_GRADIENT_STOP gs[32]; for(int i=0;i<n;i++){ gs[i].position=st[i].pos; gs[i].color=D2D1::ColorF(0,0,0,st[i].alpha); }
      Com<ID2D1GradientStopCollection> coll; HRESULT hr=dc->CreateGradientStopCollection(gs,(UINT32)n,D2D1_GAMMA_2_2,D2D1_EXTEND_MODE_CLAMP,coll.put());
      if(SUCCEEDED(hr)&&coll){ D2D1_RADIAL_GRADIENT_BRUSH_PROPERTIES rp={}; rp.center=D2D1::Point2F((W-1)*0.5f,(H-1)*0.5f); rp.radiusX=outerR; rp.radiusY=outerR;
        hr=dc->CreateRadialGradientBrush(rp,coll.get(),vig.put()); }
      if(FAILED(hr)) logLine(L"[AFK-GPU] vignette brush failed, hr="+hrHex(hr)); }
    { AfkLayout L=afkLayout((int)W,(int)H,1.0f);
      D2D1_GRADIENT_STOP gs[2]; gs[0].position=0.f; gs[0].color=D2D1::ColorF(0,0,0,0.f); gs[1].position=1.f; gs[1].color=D2D1::ColorF(0,0,0,215.0f/255.0f);
      Com<ID2D1GradientStopCollection> coll; HRESULT hr=dc->CreateGradientStopCollection(gs,2,D2D1_GAMMA_2_2,D2D1_EXTEND_MODE_CLAMP,coll.put());
      if(SUCCEEDED(hr)&&coll){ D2D1_LINEAR_GRADIENT_BRUSH_PROPERTIES lp={}; lp.startPoint=D2D1::Point2F(0.f,L.gradY); lp.endPoint=D2D1::Point2F(0.f,(float)H);
        hr=dc->CreateLinearGradientBrush(lp,coll.get(),bottom.put()); }
      if(FAILED(hr)) logLine(L"[AFK-GPU] bottom-gradient brush failed, hr="+hrHex(hr)); }
    return true; }

  bool ensureFormats(float s){
    if(clockFmt && hintFmt && fmtScale==s) return true;
    clockFmt.reset(); hintFmt.reset(); fmtScale=s;
    HRESULT hr=dw->CreateTextFormat(L"Consolas",nullptr,DWRITE_FONT_WEIGHT_BOLD,DWRITE_FONT_STYLE_NORMAL,DWRITE_FONT_STRETCH_NORMAL,46.f*s,L"en-us",clockFmt.put());
    if(FAILED(hr)){ fail(L"CreateTextFormat(clock)",hr); return false; }
    hr=dw->CreateTextFormat(L"Consolas",nullptr,DWRITE_FONT_WEIGHT_REGULAR,DWRITE_FONT_STYLE_NORMAL,DWRITE_FONT_STRETCH_NORMAL,11.f*s,L"en-us",hintFmt.put());
    if(FAILED(hr)){ fail(L"CreateTextFormat(hint)",hr); return false; }
    clockFmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER); clockFmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    hintFmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);  hintFmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    return true; }

  // Largest type-ladder step whose wrapped height fits the space between the clock and the hint line. Rebuilt only
  // when the text, the user name or the geometry changes.
  void buildQuote(const AfkFrameParams& p,const AfkLayout& L){
    std::wstring key=p.quote; key+=L'\x1f'; key+=p.user; key+=L'\x1f'; key+=std::to_wstring(p.W); key+=L'x'; key+=std::to_wstring(p.H);
    key+=L'@'; key+=std::to_wstring((int)(p.uiScale*100.0f+0.5f));
    if(key==quoteKey) return;
    quoteLayout.reset(); quoteKey=key; quoteH=0.f;
    std::wstring plain; std::vector<AfkTextRun> bold; afkParseQuote(p.quote,p.user,plain,bold);
    if(plain.empty()) return;
    for(int i=0;i<AFK_QUOTE_STEPS;i++){
      float sz=AFK_QUOTE_SIZES[i]*p.uiScale, lh=sz*1.385f;
      Com<IDWriteTextFormat> fmt;
      HRESULT qh=dw->CreateTextFormat(L"Segoe UI",nullptr,DWRITE_FONT_WEIGHT_REGULAR,DWRITE_FONT_STYLE_NORMAL,DWRITE_FONT_STRETCH_NORMAL,sz,L"en-us",fmt.put());
      if(FAILED(qh)||!fmt){ logLine(L"[AFK-GPU] HUD overlay: the quote's text format failed, hr="+hrHex(qh)+L" -- no quote is drawn"); return; }
      Com<IDWriteTextLayout> lay;
      qh=dw->CreateTextLayout(plain.c_str(),(UINT32)plain.size(),fmt.get(),L.quoteW,10000.0f,lay.put());
      if(FAILED(qh)||!lay){ logLine(L"[AFK-GPU] HUD overlay: the quote's text layout failed, hr="+hrHex(qh)+L" -- no quote is drawn"); return; }
      lay->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER); lay->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
      for(size_t k=0;k<bold.size();k++){ DWRITE_TEXT_RANGE tr={(UINT32)bold[k].start,(UINT32)bold[k].len};
        lay->SetFontWeight(DWRITE_FONT_WEIGHT_BOLD,tr); lay->SetFontSize(sz+4.0f*p.uiScale,tr); }
      lay->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM,lh,lh*0.8f);     // fixed line pitch even with larger bold runs
      DWRITE_TEXT_METRICS tm={}; lay->GetMetrics(&tm);
      quoteLayout.attach(lay.detach()); quoteH=tm.height;
      if(tm.height<=L.quoteAvail) break; } }
};

AfkOverlayD2D::AfkOverlayD2D():m(new Impl()){}
AfkOverlayD2D::~AfkOverlayD2D(){ releaseAll(); delete m; }
bool AfkOverlayD2D::usable() const { return !m->failed; }
void AfkOverlayD2D::releaseTargets(){ m->dropTargets(); }
void AfkOverlayD2D::releaseAll(){ m->dropAll(); }

bool AfkOverlayD2D::draw(void* d3dDevice,void* backBuffer,unsigned W,unsigned H,bool haveVideo,float videoFade,const AfkFrameParams& p){
  Impl& s=*m; if(s.failed||!d3dDevice||!backBuffer||!W||!H) return false;
  if(s.retryAt){ if(GetTickCount64()<s.retryAt) return false; s.retryAt=0; }       // backing off after a failure
  if(!s.ensureDevice(d3dDevice)||!s.ensureTarget(backBuffer)||!s.ensureFormats(p.uiScale)) return false;
  s.ensureBrushes(W,H);
  const float Wf=(float)W,Hf=(float)H,sc=p.uiScale; const AfkLayout L=afkLayout((int)W,(int)H,sc);
  if(s.starW!=W||s.starH!=H||s.stars.empty()){ afkStarsInit(s.stars,90,(int)W,(int)H,s.rng); s.starW=W; s.starH=H; }
  afkStarsStep(s.stars,p.dtSec,(int)W,(int)H,s.rng);
  s.buildQuote(p,L);

  ID2D1DeviceContext* dc=s.dc.get(); ID2D1SolidColorBrush* b=s.solid.get();
  dc->SetTarget(s.target.get());
  dc->BeginDraw();
  dc->SetTransform(D2D1::Matrix3x2F::Identity());
  dc->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
  dc->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);      // text fades: ClearType would fringe on a changing alpha
  auto fill=[&](float x,float y,float w,float h,float r,float g,float bl,float a){ if(a<=0.001f) return; b->SetColor(D2D1::ColorF(r,g,bl,a)); dc->FillRectangle(D2D1::RectF(x,y,x+w,y+h),b); };

  // Video reveal: the video is already in the back buffer, so it fades in by laying the background colour over it.
  float fade=videoFade; if(fade>1.f)fade=1.f; if(fade<0.f)fade=0.f;
  if(!haveVideo) fade=0.f;
  if(fade<0.995f) fill(0,0,Wf,Hf,6/255.f,8/255.f,16/255.f,1.0f-fade);
  // Camera-lens vignette over the video only (the stars stay bright on top of it).
  if(haveVideo && s.vig && fade>0.001f){ s.vig->SetOpacity(fade); dc->FillRectangle(D2D1::RectF(0,0,Wf,Hf),s.vig.get()); }

  // Starfield: dimmed in the focus circle, full brightness outside it.
  { const float k=sc;
    for(size_t i=0;i<s.stars.size();i++){ const AfkStar& st=s.stars[i];
      float dim=afkStarCenterDim(st.x,st.y,(int)W,(int)H); float sa=st.a*p.af*p.starF*dim;   // 0..255
      if(sa<2.f) continue;
      b->SetColor(D2D1::ColorF(170/255.f,205/255.f,255/255.f,(sa/3.0f)/255.f));
      dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(st.x+1.5f*k,st.y+1.5f*k),3.5f*k,3.5f*k),b);
      b->SetColor(D2D1::ColorF(225/255.f,238/255.f,255/255.f,sa/255.f));
      dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(st.x+1.3f*k,st.y+1.3f*k),1.3f*k,1.3f*k),b); } }

  // Darkness rising from the bottom behind the text.
  if(s.bottom){ float o=p.af*p.txtF; if(o>0.001f){ s.bottom->SetOpacity(o); dc->FillRectangle(D2D1::RectF(0,L.gradY,Wf,Hf),s.bottom.get()); } }

  // Clock.
  { wchar_t clk[9]; afkFmtClock(clk,p.hh,p.mm,p.ss); float a=p.af*p.txtF;
    if(a>0.001f){ b->SetColor(D2D1::ColorF(220/255.f,230/255.f,245/255.f,a));
      dc->DrawText(clk,8,s.clockFmt.get(),D2D1::RectF(0,L.clockY,Wf,L.clockY+80.f*sc),b); } }

  // Quote, vertically centred in the space between the clock and the hint line.
  if(s.quoteLayout){ float a=p.af*p.quoteA*p.txtF;
    if(a>0.001f){ float y=L.quoteTop+(L.quoteAvail-s.quoteH)*0.5f; if(y<L.quoteTop) y=L.quoteTop;
      b->SetColor(D2D1::ColorF(236/255.f,239/255.f,249/255.f,a));
      dc->DrawTextLayout(D2D1::Point2F((Wf-L.quoteW)*0.5f,y),s.quoteLayout.get(),b); } }

  // Hint.
  { float a=0.7f*p.af*p.txtF;
    if(a>0.001f){ static const wchar_t hint[]=L"AFK  ·  move or press a key to resume";
      b->SetColor(D2D1::ColorF(150/255.f,160/255.f,190/255.f,a));
      dc->DrawText(hint,(UINT32)(sizeof(hint)/sizeof(wchar_t)-1),s.hintFmt.get(),D2D1::RectF(0,L.hintY,Wf,L.hintY+30.f*sc),b); } }

  HRESULT hr=dc->EndDraw();
  dc->SetTarget(nullptr);
  if(hr==(HRESULT)D2DERR_RECREATE_TARGET){ s.fail(L"EndDraw (the render target was lost)",hr); return false; }   // the engine reports a lost device itself; we rebuild after a pause
  if(FAILED(hr)){ s.fail(L"EndDraw",hr); return false; }
  s.softFails=0;
  return true;
}
