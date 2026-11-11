// gfx.h -- GDI+ palette, fonts and small drawing helpers used by the dashboard, toast and CPU AFK renderer.
#pragma once
#include "sw_common.h"
#include <gdiplus.h>
using namespace Gdiplus;

// ---------- palette ----------
static const Color C_BG(255,10,14,26), C_BG2(255,14,18,34),
  C_PANEL(255,20,26,46), C_PANEL2(255,25,32,56), C_LINE(255,40,48,82),
  C_LINE2(255,54,66,108), C_VIOLET(255,120,196,255), C_VDIM(255,150,124,246),
  C_GREEN(255,92,230,164), C_RED(255,248,116,140),
  C_TX(255,231,234,244), C_TXD(255,140,150,178), C_TXM(255,92,100,134);

// ---------- fonts (created once in gfxInit; GDI+ must already be started) ----------
extern FontFamily *famUI,*famMono;
extern Font *fTitle,*fH,*fLbl,*fBody,*fBodyB,*fBig,*fMono,*fMonoBig;
// Type ladder for auto-fitting quotes into fixed boxes: the text picks the largest step that fits
// instead of overflowing and being clipped mid-line.
struct RichFont{ Font* n; Font* b; REAL lh; };
extern std::vector<RichFont> g_ladderSide;     // dashboard sidebar quote box, largest first
void gfxInit();
void gfxShutdown();

// ---------- typed wrappers (avoid int/REAL overload ambiguity) ----------
void FRect(Graphics&g,const Brush*b,REAL x,REAL y,REAL w,REAL h);
void FEll(Graphics&g,const Brush*b,REAL x,REAL y,REAL w,REAL h);
void DEll(Graphics&g,const Pen*pn,REAL x,REAL y,REAL w,REAL h);
void DLine(Graphics&g,const Pen*pn,REAL x1,REAL y1,REAL x2,REAL y2);
void DImg(Graphics&g,Image*im,REAL x,REAL y,REAL w,REAL h);

// ---------- shapes / text ----------
void addRound(GraphicsPath& gp,REAL x,REAL y,REAL w,REAL h,REAL r);
void fillRound(Graphics& g,REAL x,REAL y,REAL w,REAL h,REAL r,Color c);
void strokeRound(Graphics& g,REAL x,REAL y,REAL w,REAL h,REAL r,Color c,REAL t=1);
void drawStr(Graphics& g,const wchar_t* s,Font* f,Color c,REAL x,REAL y);
void drawStrR(Graphics& g,const std::wstring& s,Font* f,Color c,REAL right,REAL y);
bool inR(RECT r,int x,int y);
std::wstring scramble(const std::wstring& t,double el,double per);
void card(Graphics& g,REAL x,REAL y,REAL w,REAL h);
void label(Graphics& g,const wchar_t* s,REAL x,REAL y);

// Rich text: **bold** emphasis, {user} substitution, word-wrap. Returns the height used; measureOnly draws nothing.
REAL drawRich(Graphics& g,std::wstring text,REAL x,REAL y,REAL maxW,Color c,double scrEl=-1,double scrPer=0.03,
              Font* fnR=nullptr,Font* fnB=nullptr,REAL lh=27,bool measureOnly=false);
const RichFont* fitRich(Graphics& g,const std::wstring& text,REAL maxW,REAL maxH,const std::vector<RichFont>& ladder);
