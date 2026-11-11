// afk_assets.cpp -- see afk_assets.h
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include "afk_assets.h"
#include "resource_ids.h"
#include <shlobj.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <propvarutil.h>
#include <algorithm>

static const wchar_t* BUILTIN_4K   = L"sentinel_afk_4k60.mp4";
static const wchar_t* BUILTIN_LITE = L"sentinel_afk_1080p30.mp4";
static const ULONGLONG LEGACY_V13_CLIP_BYTES = 41240486ULL;     // afk_bg.mp4 unpacked by builds up to v13

static std::wstring clipDir(){ std::wstring d=appDir()+L"\\content\\afk"; SHCreateDirectoryExW(0,d.c_str(),0); return d; }

static bool fileSize(const std::wstring& p,ULONGLONG& sz){
  WIN32_FILE_ATTRIBUTE_DATA fa={}; if(!GetFileAttributesExW(p.c_str(),GetFileExInfoStandard,&fa)) return false;
  sz=((ULONGLONG)fa.nFileSizeHigh<<32)|fa.nFileSizeLow; return true; }

// Writes the RCDATA resource `res` to `dest` (skipped when a file of the right size is already there). Written as
// <dest>.part and renamed, so an interrupted write can never be mistaken for the real file.
static bool unpackResource(const wchar_t* res,const std::wstring& dest,const wchar_t* label){
  HRSRC r=FindResourceW(g_hInst,res,RT_RCDATA);
  if(!r){ logLine(std::wstring(L"[AFK] built-in clip ")+label+L" is not in this exe (resource "+res+L" missing)"); return false; }
  DWORD sz=SizeofResource(g_hInst,r); HGLOBAL h=LoadResource(g_hInst,r); const BYTE* p=h?(const BYTE*)LockResource(h):nullptr;
  if(!sz||!p){ logLine(std::wstring(L"[AFK] built-in clip ")+label+L": resource is empty / cannot be locked"); return false; }
  ULONGLONG have=0; if(fileSize(dest,have)&&have==sz) return true;                 // already unpacked
  // enough room? (clip + a little slack)
  ULARGE_INTEGER freeB={}; std::wstring dir=dest.substr(0,dest.find_last_of(L'\\'));
  if(GetDiskFreeSpaceExW(dir.c_str(),&freeB,nullptr,nullptr) && freeB.QuadPart < (ULONGLONG)sz+(32ULL<<20)){
    logLine(std::wstring(L"[AFK] not enough free disk space to unpack ")+label+L" ("+fmt1(sz/1048576.0)+L" MB needed, "+fmt1((double)freeB.QuadPart/1048576.0)+L" MB free)"); return false; }
  logLine(std::wstring(L"[AFK] unpacking built-in clip ")+label+L" ("+fmt1(sz/1048576.0)+L" MB) -> "+dest);
  std::wstring part=dest+L".part"; int64_t t0=hnsNow();
  HANDLE f=CreateFileW(part.c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_SEQUENTIAL_SCAN,nullptr);
  if(f==INVALID_HANDLE_VALUE){ logLine(L"[AFK] cannot create "+part+L", error "+std::to_wstring(GetLastError())); return false; }
  bool ok=true; DWORD off=0;
  while(off<sz){ DWORD n=sz-off; if(n>(8u<<20)) n=8u<<20; DWORD wr=0;
    if(!WriteFile(f,p+off,n,&wr,nullptr)||wr!=n){ logLine(L"[AFK] write failed at "+std::to_wstring(off)+L", error "+std::to_wstring(GetLastError())); ok=false; break; }
    off+=n; }
  if(ok) FlushFileBuffers(f);
  CloseHandle(f);
  if(!ok){ DeleteFileW(part.c_str()); return false; }
  if(!MoveFileExW(part.c_str(),dest.c_str(),MOVEFILE_REPLACE_EXISTING)){
    logLine(L"[AFK] cannot move "+part+L" into place, error "+std::to_wstring(GetLastError())); DeleteFileW(part.c_str()); return false; }
  logLine(std::wstring(L"[AFK] unpacked ")+label+L" in "+fmt1((hnsNow()-t0)/10000.0)+L" ms");
  return true; }

// Opens the file with a source reader (no decoding) to learn its size / frame rate / codec. Also proves Media Foundation
// can parse the container at all.
static bool probeClip(AfkClip& c,std::wstring& why){
  IMFSourceReader* rd=nullptr; HRESULT hr=MFCreateSourceReaderFromURL(c.path.c_str(),nullptr,&rd);
  if(FAILED(hr)||!rd){ why=L"MFCreateSourceReaderFromURL failed, hr="+hrHex(hr); return false; }
  IMFMediaType* nat=nullptr; hr=rd->GetNativeMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM,0,&nat);
  if(FAILED(hr)||!nat){ rd->Release(); why=L"no video stream (GetNativeMediaType hr="+hrHex(hr)+L")"; return false; }
  MFGetAttributeSize(nat,MF_MT_FRAME_SIZE,&c.w,&c.h);
  UINT32 n=0,d=0; if(SUCCEEDED(MFGetAttributeRatio(nat,MF_MT_FRAME_RATE,&n,&d))&&n&&d) c.fps=(double)n/(double)d;
  GUID sub=GUID_NULL; if(SUCCEEDED(nat->GetGUID(MF_MT_SUBTYPE,&sub))){ wchar_t cc[5]={0}; for(int i=0;i<4;i++){ unsigned ch=(sub.Data1>>(8*i))&0xFF; cc[i]=(ch>=32&&ch<127)?(wchar_t)ch:L'?'; } c.codec=cc; }
  nat->Release();
  PROPVARIANT pv; PropVariantInit(&pv);
  if(SUCCEEDED(rd->GetPresentationAttribute((DWORD)MF_SOURCE_READER_MEDIASOURCE,MF_PD_DURATION,&pv))&&pv.vt==VT_UI8) c.durationSec=(double)pv.uhVal.QuadPart/1e7;
  PropVariantClear(&pv); rd->Release();
  return c.w&&c.h; }

static std::wstring describe(const AfkClip& c){
  return c.path+L" ("+std::to_wstring(c.w)+L"x"+std::to_wstring(c.h)+L" @ "+fmt1(c.fps)+L" fps, "+(c.codec.empty()?L"codec ?":c.codec)+L", "+fmt1(c.durationSec)+L" s)"; }

static bool isVideoName(const std::wstring& l){
  static const wchar_t* ext[]={L".mp4",L".mov",L".mkv",L".webm",L".avi",L".wmv",L".m4v"};
  for(const wchar_t* e:ext) if(endsW(l,e)) return true; return false; }

bool afkAssetsPrepare(AfkMedia& out){
  out=AfkMedia();
  std::wstring dir=clipDir();
  const std::wstring p4k=dir+L"\\"+BUILTIN_4K, plite=dir+L"\\"+BUILTIN_LITE;

  // housekeeping: the single clip older builds unpacked (here and in the cache folder), recognised by its exact size
  { const std::wstring old[2]={dir+L"\\afk_bg.mp4",cacheDir()+L"\\afk_bg.mp4"};
    for(int i=0;i<2;i++){ ULONGLONG sz=0; if(fileSize(old[i],sz)&&sz==LEGACY_V13_CLIP_BYTES&&DeleteFileW(old[i].c_str())) logLine(L"[AFK] removed the old built-in clip "+old[i]); } }
  { WIN32_FIND_DATAW fd; HANDLE h=FindFirstFileW((dir+L"\\*.part").c_str(),&fd);        // leftovers of an interrupted unpack
    if(h!=INVALID_HANDLE_VALUE){ do{ DeleteFileW((dir+L"\\"+fd.cFileName).c_str()); }while(FindNextFileW(h,&fd)); FindClose(h); } }

  HRESULT mf=MFStartup(MF_VERSION,MFSTARTUP_LITE);
  if(FAILED(mf)) logLine(L"[AFK] MFStartup failed while probing clips, hr="+hrHex(mf)+L" -- clip details unavailable");

  // a clip the user dropped in wins over the built-ins
  AfkClip user;
  { std::vector<std::wstring> names; WIN32_FIND_DATAW fd; HANDLE h=FindFirstFileW((dir+L"\\*").c_str(),&fd);
    if(h!=INVALID_HANDLE_VALUE){ do{ if(fd.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY) continue; std::wstring l=lc(fd.cFileName);
        if(l==lc(BUILTIN_4K)||l==lc(BUILTIN_LITE)) continue;
        if(isVideoName(l)) names.push_back(fd.cFileName); }while(FindNextFileW(h,&fd)); FindClose(h); }
    std::sort(names.begin(),names.end(),[](const std::wstring& a,const std::wstring& b){ return lc(a)<lc(b); });
    for(const std::wstring& nm:names){ AfkClip c; c.path=dir+L"\\"+nm; c.user=true; std::wstring why;
      if(SUCCEEDED(mf)&&!probeClip(c,why)){ logLine(L"[AFK] ignoring "+c.path+L": "+why); continue; }
      if(FAILED(mf)){ c.fps=30.0; }
      user=c; break; } }

  if(user.valid()){
    logLine(L"[AFK] using your clip: "+describe(user));
    if(user.codec==L"HEVC"||user.codec==L"hev1"||user.codec==L"hvc1") logLine(L"[AFK] note: this is an HEVC clip -- Windows only plays HEVC if the \"HEVC Video Extensions\" are installed; if the screen stays without video, re-encode it to H.264");
    out.userClip=true; out.big=user; out.lite=user;
  } else {
    bool ok4k=unpackResource(SW_RES_AFK_4K60,p4k,L"4K 60 fps"), okLite=unpackResource(SW_RES_AFK_LITE,plite,L"1080p 30 fps");
    if(ok4k){ AfkClip c; c.path=p4k; std::wstring why; if(SUCCEEDED(mf)&&!probeClip(c,why)){ logLine(L"[AFK] the built-in 4K clip cannot be opened by Media Foundation: "+why+
          L" (on Windows \"N\" editions install the Media Feature Pack)"); c.fps=60.0; } if(!c.fps) c.fps=60.0; out.big=c; logLine(L"[AFK] built-in 4K clip: "+describe(c)); }
    if(okLite){ AfkClip c; c.path=plite; std::wstring why; if(SUCCEEDED(mf)&&!probeClip(c,why)){ logLine(L"[AFK] the built-in 1080p clip cannot be opened by Media Foundation: "+why); c.fps=30.0; } if(!c.fps) c.fps=30.0; out.lite=c; logLine(L"[AFK] built-in 1080p clip: "+describe(c)); }
    if(!out.lite.valid()) out.lite=out.big;       // the 4K clip is better than nothing for the lite tiers
    if(!out.big.valid())  out.big=out.lite;
  }
  if(SUCCEEDED(mf)) MFShutdown();
  return out.big.valid()||out.lite.valid();
}
