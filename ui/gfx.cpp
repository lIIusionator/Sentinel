// gfx.cpp -- GDI+ helpers and fonts.
#include "gfx.h"
#include <cmath>
#include <cstdlib>

FontFamily *famUI=nullptr,*famMono=nullptr;
Font *fTitle=nullptr,*fH=nullptr,*fLbl=nullptr,*fBody=nullptr,*fBodyB=nullptr,
  *fBig=nullptr,*fMono=nullptr,*fMonoBig=nullptr;
std::vector<RichFont> g_ladderSide;

void gfxInit(){
  famUI=new FontFamily(L"Segoe UI"); famMono=new FontFamily(L"Consolas");
  fTitle=new Font(famUI,22,FontStyleBold,UnitPixel);
  fBig  =new Font(famUI,30,FontStyleBold,UnitPixel);
  fH    =new Font(famUI,15,FontStyleBold,UnitPixel);
  fBody =new Font(famUI,14,FontStyleRegular,UnitPixel);
  fLbl  =new Font(famMono,10,FontStyleRegular,UnitPixel);
  fMono =new Font(famMono,11,FontStyleRegular,UnitPixel);
  fMonoBig=new Font(famMono,26,FontStyleBold,UnitPixel);
  fBodyB=new Font(famUI,17,FontStyleBold,UnitPixel);   // bigger for **emphasis**
  // Bold runs sit a few px larger than body; line height ~1.93x the regular size.
  for(REAL sz : {14.0f,13.0f,12.0f,11.0f,10.0f,9.0f})
    g_ladderSide.push_back({ new Font(famUI,sz,FontStyleRegular,UnitPixel),
                             new Font(famUI,sz+3,FontStyleBold,UnitPixel), sz*1.93f });
}
void gfxShutdown(){
  for(auto& r:g_ladderSide){ delete r.n; delete r.b; } g_ladderSide.clear();
  delete fTitle; delete fBig; delete fH; delete fBody; delete fLbl; delete fMono; delete fMonoBig; delete fBodyB;
  fTitle=fBig=fH=fBody=fLbl=fMono=fMonoBig=fBodyB=nullptr;
  delete famUI; delete famMono; famUI=famMono=nullptr;
}

void FRect(Graphics&g,const Brush*b,REAL x,REAL y,REAL w,REAL h){ g.FillRectangle(b,x,y,w,h); }
void FEll(Graphics&g,const Brush*b,REAL x,REAL y,REAL w,REAL h){ g.FillEllipse(b,x,y,w,h); }
void DEll(Graphics&g,const Pen*pn,REAL x,REAL y,REAL w,REAL h){ g.DrawEllipse(pn,x,y,w,h); }
void DLine(Graphics&g,const Pen*pn,REAL x1,REAL y1,REAL x2,REAL y2){ g.DrawLine(pn,x1,y1,x2,y2); }
void DImg(Graphics&g,Image*im,REAL x,REAL y,REAL w,REAL h){ g.DrawImage(im,x,y,w,h); }

void addRound(GraphicsPath& gp,REAL x,REAL y,REAL w,REAL h,REAL r){
  gp.AddArc(x,y,r*2,r*2,180,90); gp.AddArc(x+w-r*2,y,r*2,r*2,270,90);
  gp.AddArc(x+w-r*2,y+h-r*2,r*2,r*2,0,90); gp.AddArc(x,y+h-r*2,r*2,r*2,90,90); gp.CloseFigure(); }
void fillRound(Graphics& g,REAL x,REAL y,REAL w,REAL h,REAL r,Color c){
  GraphicsPath gp; addRound(gp,x,y,w,h,r); SolidBrush b(c); g.FillPath(&b,&gp); }
void strokeRound(Graphics& g,REAL x,REAL y,REAL w,REAL h,REAL r,Color c,REAL t){
  GraphicsPath gp; addRound(gp,x,y,w,h,r); Pen p(c,t); g.DrawPath(&p,&gp); }
void drawStr(Graphics& g,const wchar_t* s,Font* f,Color c,REAL x,REAL y){
  SolidBrush b(c); g.DrawString(s,-1,f,PointF(x,y),&b); }
void drawStrR(Graphics& g,const std::wstring& s,Font* f,Color c,REAL right,REAL y){
  SolidBrush b(c); RectF box; g.MeasureString(s.c_str(),-1,f,PointF(0,0),&box);
  g.DrawString(s.c_str(),-1,f,PointF(right-box.Width,y),&b); }
bool inR(RECT r,int x,int y){ return x>=r.left&&x<r.right&&y>=r.top&&y<r.bottom; }

std::wstring scramble(const std::wstring& t,double el,double per){
  static const wchar_t* G=L"ABCDEFGHJKLMNPQRSTUVWXYZ0123456789#%&*<>/=+"; int gl=(int)wcslen(G);
  std::wstring o; for(size_t i=0;i<t.size();++i){ double rt=(i+1)*per;
    if(el>=rt||t[i]==L' ')o+=t[i]; else o+=G[rand()%gl]; } return o; }

void card(Graphics& g,REAL x,REAL y,REAL w,REAL h){
  fillRound(g,x,y,w,h,12,C_PANEL); strokeRound(g,x,y,w,h,12,C_LINE);
  // corner brackets
  Pen p(C_LINE2,2);
  DLine(g,&p,x+8,y+8,x+20,y+8); DLine(g,&p,x+8,y+8,x+8,y+20);
  DLine(g,&p,x+w-8,y+h-8,x+w-20,y+h-8); DLine(g,&p,x+w-8,y+h-8,x+w-8,y+h-20);
}
void label(Graphics& g,const wchar_t* s,REAL x,REAL y){
  SolidBrush v(C_VIOLET); FRect(g,&v,x,y+7,14,2);
  drawStr(g,s,fLbl,C_TXM,x+20,y); }

REAL drawRich(Graphics& g,std::wstring text,REAL x,REAL y,REAL maxW,Color c,double scrEl,double scrPer,
                     Font* fnR,Font* fnB,REAL lh,bool measureOnly){
  size_t up; while((up=text.find(L"{user}"))!=std::wstring::npos) text.replace(up,6,g_userName);
  struct Run{ std::wstring s; bool b; }; std::vector<Run> runs; bool b=false; std::wstring cur;
  for(size_t i=0;i<text.size();){ if(text.compare(i,2,L"**")==0){ if(!cur.empty()){runs.push_back({cur,b});cur.clear();} b=!b; i+=2; } else cur+=text[i++]; }
  if(!cur.empty())runs.push_back({cur,b});
  static const wchar_t* GL=L"ABCDEFGHJKLMNPQRSTUVWXYZ0123456789#%&*<>/=+"; int gln=(int)wcslen(GL); int ci=0;
  SolidBrush br(c); REAL cxp=x,cyp=y;
  Font* FR=fnR?fnR:fBody; Font* FB=fnB?fnB:fBodyB;
  for(auto&r:runs){ Font* fn=r.b?FB:FR; std::wstring word;
    auto flush=[&](){ if(word.empty())return; RectF bb; g.MeasureString(word.c_str(),-1,fn,PointF(0,0),&bb);
      if(cxp+bb.Width>x+maxW){ cxp=x; cyp+=lh; }
      if(!measureOnly){
        REAL wy=cyp+(lh-bb.Height)/2;               // center taller emphasis words on the line
        g.DrawString(word.c_str(),-1,fn,PointF(cxp,wy),&br); }
      cxp+=bb.Width; word.clear(); };
    for(wchar_t ch:r.s){ wchar_t oc=ch;
        if(ch!=L' '){ if(scrEl>=0 && scrEl < (ci+1)*scrPer) oc=GL[rand()%gln]; ci++; }
        word+=oc; if(oc==L' ') flush(); } flush(); }
  return cyp+lh-y;
}
// Largest ladder step whose wrapped height fits maxH; falls back to the smallest.
const RichFont* fitRich(Graphics& g,const std::wstring& text,REAL maxW,REAL maxH,
                               const std::vector<RichFont>& ladder){
  if(ladder.empty())return nullptr;
  for(auto&rf:ladder){
    if(drawRich(g,text,0,0,maxW,Color(0,0,0,0),-1,0.03,rf.n,rf.b,rf.lh,true)<=maxH) return &rf; }
  return &ladder.back();
}
