// autorun_watch.cpp -- autorun enumeration (Run keys, Startup folders, Tasks) and the "quick scan".
#include "watchers.h"
#include "events.h"
#include "settings.h"
#include "autostart.h"
#include <shlobj.h>
#include <knownfolders.h>
#include <fstream>

std::set<std::wstring> g_known;

static std::wstring ownExe(){ wchar_t p[MAX_PATH]; DWORD n=GetModuleFileNameW(nullptr,p,MAX_PATH); return (n&&n<MAX_PATH)?std::wstring(p,n):std::wstring(); }

static void enumReg(HKEY r,const wchar_t* sub,const wchar_t* lab,std::set<std::wstring>& o){
  HKEY h; if(RegOpenKeyExW(r,sub,0,KEY_READ,&h)!=ERROR_SUCCESS)return;
  wchar_t nm[16384]; BYTE d[16384];
  for(DWORD i=0;;++i){ DWORD nl=16384/2,dl=sizeof(d),ty=0;
    if(RegEnumValueW(h,i,nm,&nl,0,&ty,d,&dl)!=ERROR_SUCCESS)break;
    std::wstring v; if(ty==REG_SZ||ty==REG_EXPAND_SZ)v=(wchar_t*)d;
    if(isOwnAutostart(nm,v,ownExe())) continue;                  // "Start with Windows" is ours, not a new autorun
    o.insert(std::wstring(L"REG ")+lab+L" | "+nm+L" = "+v); }
  RegCloseKey(h);
}
static void enumDir(const std::wstring& p,const wchar_t* lab,std::set<std::wstring>& o){
  if(p.empty())return; WIN32_FIND_DATAW fd; HANDLE h=FindFirstFileW((p+L"\\*").c_str(),&fd);
  if(h==INVALID_HANDLE_VALUE)return;
  do{ std::wstring f=fd.cFileName; if(f==L"."||f==L".."||f==L"desktop.ini")continue;
    o.insert(std::wstring(L"DIR ")+lab+L" | "+f);}while(FindNextFileW(h,&fd)); FindClose(h);
}
static std::wstring kf(REFKNOWNFOLDERID id){ PWSTR p=0; std::wstring o;
  if(SUCCEEDED(SHGetKnownFolderPath(id,0,0,&p))&&p)o=p; if(p)CoTaskMemFree(p); return o; }
std::set<std::wstring> scanAutoruns(){ std::set<std::wstring> s;
  enumReg(HKEY_CURRENT_USER,L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",L"HKCU Run",s);
  enumReg(HKEY_CURRENT_USER,L"Software\\Microsoft\\Windows\\CurrentVersion\\RunOnce",L"HKCU RunOnce",s);
  enumReg(HKEY_LOCAL_MACHINE,L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",L"HKLM Run",s);
  enumDir(kf(FOLDERID_Startup),L"Startup",s); enumDir(kf(FOLDERID_CommonStartup),L"Startup*",s);
  std::wstring sr=envW(L"SystemRoot"); if(!sr.empty())enumDir(sr+L"\\System32\\Tasks",L"Tasks",s);
  return s; }
// "Start with Windows": the HKCU Run value follows the g_tAutostart toggle. Called at start (that also heals the path after the exe was moved) and after every toggle click.
void autostartApply(){
  static const wchar_t* const RUN=L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
  HKEY h; if(RegCreateKeyExW(HKEY_CURRENT_USER,RUN,0,nullptr,0,KEY_QUERY_VALUE|KEY_SET_VALUE,nullptr,&h,nullptr)!=ERROR_SUCCESS){ logLine(L"[AUTOSTART] cannot open the Run key"); return; }
  const std::wstring exe=ownExe();
  if(g_tAutostart.load()&&!exe.empty()){
    const std::wstring want=autostartValue(exe);
    wchar_t cur[2048]={}; DWORD sz=sizeof(cur)-sizeof(wchar_t), ty=0;
    const bool same=RegQueryValueExW(h,AUTOSTART_NAME,nullptr,&ty,(BYTE*)cur,&sz)==ERROR_SUCCESS&&ty==REG_SZ&&autostartLower(cur)==autostartLower(want);
    if(!same){ const LONG r=RegSetValueExW(h,AUTOSTART_NAME,0,REG_SZ,(const BYTE*)want.c_str(),(DWORD)((want.size()+1)*sizeof(wchar_t)));
      logLine(r==ERROR_SUCCESS?L"[AUTOSTART] on: "+want:L"[AUTOSTART] could not write the Run value, error "+std::to_wstring(r)); } }
  else if(RegDeleteValueW(h,AUTOSTART_NAME)==ERROR_SUCCESS) logLine(L"[AUTOSTART] off: Run value removed");
  RegCloseKey(h);
}
void doScan(bool announce){
  { HANDLE pt=CreateThread(0,0,ProcScanThread,0,0,0); if(pt){ SetThreadPriority(pt,THREAD_PRIORITY_BELOW_NORMAL); CloseHandle(pt);} }  // background scan burst -- yields CPU to the video thread
  if(!g_tAutorun.load()){ pushEv(L"SYS",L"scan skipped",L"autorun watch off"); return; }
  auto cur=scanAutoruns(); int added=0;
  for(auto&e:cur)if(!g_known.count(e)){ pushEv(L"WARN",L"new autorun",e); added++; }
  if(added&&announce) showOverlay(L"\u26A0 New startup entry",std::to_wstring(added)+L" new autorun entry(s)",ACC_RED);
  if(added){ g_known=cur; std::ofstream f(wpath(baseFile()),std::ios::trunc|std::ios::binary);
    for(auto&e:cur){ std::string u=toU8(e); f.write(u.data(),u.size()); f.put('\n'); } }
  if(!added) pushEv(L"SYS",L"manual scan",L"autoruns re-swept \u2014 no changes");
}
