// toast.cpp -- always-on-top alert popup.
#include "toast.h"
#include "gfx.h"
#include "settings.h"

HWND g_ovl=nullptr;
static std::wstring g_ovlTitle,g_ovlText; static ArgbColor g_ovlAccent=ACC_VIOLET;

// Posted to the UI thread, so it is safe to call from any watcher thread.
void showOverlay(const std::wstring& title,const std::wstring& text,ArgbColor ac){
  if(!g_tNotify.load())return;
  PostMessageW(g_msg,WM_SHOWTOAST,0,(LPARAM)new Toast{title,text,ac});
}

LRESULT CALLBACK OvlProc(HWND h,UINT m,WPARAM w,LPARAM l){
  switch(m){
  case WM_PAINT:{ PAINTSTRUCT ps; HDC dc=BeginPaint(h,&ps); RECT rc; GetClientRect(h,&rc);
    Graphics g(dc); g.SetSmoothingMode(SmoothingModeAntiAlias); g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
    SolidBrush bg(Color(255,24,26,32)); FRect(g,&bg,0,0,rc.right,rc.bottom);
    SolidBrush acb(Color((ARGB)g_ovlAccent)); FRect(g,&acb,0,0,6,rc.bottom);
    SolidBrush t1(Color(255,240,240,245)),t2(Color(255,220,220,228));
    g.DrawString(g_ovlTitle.c_str(),-1,fH,PointF(20,12),&t1);
    RectF rb(20,44,(REAL)rc.right-34,(REAL)rc.bottom-54); StringFormat sf;
    g.DrawString(g_ovlText.c_str(),-1,fBody,rb,&sf,&t2);
    EndPaint(h,&ps); return 0; }
  case WM_TIMER: if(w==TMR_HIDEOVL){KillTimer(h,TMR_HIDEOVL);ShowWindow(h,SW_HIDE);} return 0;
  case WM_LBUTTONUP: KillTimer(h,TMR_HIDEOVL); ShowWindow(h,SW_HIDE); return 0;
  }
  return DefWindowProcW(h,m,w,l);
}

void toastDisplay(Toast* t){
  if(!t) return;
  if(g_tNotify.load()){
    g_ovlTitle=t->title; g_ovlText=t->text; g_ovlAccent=t->accent;
    RECT wa; SystemParametersInfoW(SPI_GETWORKAREA,0,&wa,0);
    SetWindowPos(g_ovl,HWND_TOPMOST,wa.right-416,wa.bottom-166,400,150,SWP_NOACTIVATE);
    ShowWindow(g_ovl,SW_SHOWNOACTIVATE); InvalidateRect(g_ovl,0,TRUE);
    SetTimer(g_ovl,TMR_HIDEOVL,5000,0);
    if(g_tBeep.load()){ Beep(1000,120); Beep(1400,120); }
  }
  delete t;
}
