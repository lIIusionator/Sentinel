// me_util.h -- pure helpers of the media engine (no Windows headers, so tests/afk_tests.cpp can run them natively).
#pragma once
#include <stdint.h>
#include <string>

// Cover-fit crop as a normalised source rectangle {left,top,right,bottom} of the video frame, for a destination of
// W x H: the frame is scaled to fill, the overflow is cropped, nothing is stretched. Same-aspect -> {0,0,1,1}.
static inline void me_coverSrcNorm(uint32_t vw,uint32_t vh,uint32_t W,uint32_t H,float out[4]){
  out[0]=0.f; out[1]=0.f; out[2]=1.f; out[3]=1.f; if(!vw||!vh||!W||!H) return;
  double va=(double)vw/(double)vh, ta=(double)W/(double)H;
  if(va>ta+1e-6){ double vis=ta/va; out[0]=(float)((1.0-vis)/2.0); out[2]=(float)((1.0+vis)/2.0); }
  else if(va<ta-1e-6){ double vis=va/ta; out[1]=(float)((1.0-vis)/2.0); out[3]=(float)((1.0+vis)/2.0); }
}

// file:///C:/Users/x/My%20Videos/a.mp4 from a Windows path (UTF-8 percent-encoding; surrogate pairs handled).
static inline std::wstring me_fileUrl(const std::wstring& path){
  std::wstring o=L"file:///"; static const char* hex="0123456789ABCDEF";
  for(size_t i=0;i<path.size();i++){
    uint32_t cp=(uint32_t)path[i];
    if(sizeof(wchar_t)==2 && cp>=0xD800 && cp<0xDC00 && i+1<path.size()){
      uint32_t lo=(uint32_t)path[i+1]; if(lo>=0xDC00&&lo<0xE000){ cp=0x10000+((cp-0xD800)<<10)+(lo-0xDC00); i++; } }
    if(cp=='\\') cp='/';
    bool plain=(cp>='A'&&cp<='Z')||(cp>='a'&&cp<='z')||(cp>='0'&&cp<='9')||cp=='-'||cp=='_'||cp=='.'||cp=='~'||cp=='/'||cp==':';
    if(plain){ o+=(wchar_t)cp; continue; }
    unsigned char b[4]; int n=0;
    if(cp<0x80){ b[n++]=(unsigned char)cp; }
    else if(cp<0x800){ b[n++]=(unsigned char)(0xC0|(cp>>6)); b[n++]=(unsigned char)(0x80|(cp&0x3F)); }
    else if(cp<0x10000){ b[n++]=(unsigned char)(0xE0|(cp>>12)); b[n++]=(unsigned char)(0x80|((cp>>6)&0x3F)); b[n++]=(unsigned char)(0x80|(cp&0x3F)); }
    else { b[n++]=(unsigned char)(0xF0|(cp>>18)); b[n++]=(unsigned char)(0x80|((cp>>12)&0x3F)); b[n++]=(unsigned char)(0x80|((cp>>6)&0x3F)); b[n++]=(unsigned char)(0x80|(cp&0x3F)); }
    for(int k=0;k<n;k++){ o+=L'%'; o+=(wchar_t)hex[b[k]>>4]; o+=(wchar_t)hex[b[k]&15]; } }
  return o;
}

// Exponential moving average for the per-frame cost counters (first sample seeds it).
static inline double me_ema(double cur,double x){ return cur<=0.0 ? x : cur*0.9+x*0.1; }

// New-frame accounting for "blind" mode (OnVideoStreamTick never signals): derives frames delivered from the
// media clock. `acc` carries the fractional frame between calls; a backwards jump (loop restart) adds nothing.
struct MeBlindCounter{
  double last=0, acc=0;
  unsigned long advance(double mediaTimeSec,double fps){
    double cnt=mediaTimeSec*(fps>0?fps:30.0);
    if(cnt>last){ acc+=cnt-last; }
    last=cnt; unsigned long whole=(unsigned long)acc; acc-=(double)whole; return whole; }
};
