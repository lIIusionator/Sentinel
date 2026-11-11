// trust.cpp -- executable / path heuristics and Authenticode check.
#include "trust.h"
#include "sw_lock.h"
#include <unordered_map>
#include <wincrypt.h>
#include <wintrust.h>
#include <softpub.h>

bool isExecExt(const std::wstring& p){
  static const wchar_t* ex[]={L".exe",L".dll",L".pyd",L".scr",L".bat",L".cmd",
    L".js",L".vbs",L".ps1",L".msi",L".com",L".lnk",L".jar"};
  for(auto e:ex) if(endsW(p,e))return true; return false; }
bool suspPath(const std::wstring& p){ if(p.empty())return true; std::wstring l=lc(p);
  for(auto m:{L"\\appdata\\",L"\\temp\\",L"\\downloads\\",L"\\programdata\\",L"\\users\\public\\"})
    if(l.find(m)!=std::wstring::npos)return true; return false; }
bool isSystemProc(const std::wstring& p){ std::wstring l=lc(p);
  std::wstring win=lc(envW(L"SystemRoot"));
  return (!win.empty()&&l.rfind(win,0)==0); }
bool userWritable(const std::wstring& p){ if(p.empty())return false; std::wstring l=lc(p);
  for(auto m:{L"\\appdata\\",L"\\temp\\",L"\\downloads\\",L"\\programdata\\",L"\\users\\public\\"})
    if(l.find(m)!=std::wstring::npos)return true; return false; }
// Authenticode check: legit apps are signed; unsigned exes in user-writable
// spots are the real red flag. Verified offline (no revocation network call).
static bool verifyTrust(const std::wstring& path){
  WINTRUST_FILE_INFO fi; ZeroMemory(&fi,sizeof(fi)); fi.cbStruct=sizeof(fi); fi.pcwszFilePath=path.c_str();
  GUID gv=WINTRUST_ACTION_GENERIC_VERIFY_V2;
  WINTRUST_DATA wd; ZeroMemory(&wd,sizeof(wd)); wd.cbStruct=sizeof(wd);
  wd.dwUIChoice=WTD_UI_NONE; wd.fdwRevocationChecks=WTD_REVOKE_NONE;
  wd.dwUnionChoice=WTD_CHOICE_FILE; wd.pFile=&fi;
  wd.dwStateAction=WTD_STATEACTION_VERIFY; wd.dwProvFlags=WTD_CACHE_ONLY_URL_RETRIEVAL;
  LONG st=WinVerifyTrust((HWND)INVALID_HANDLE_VALUE,&gv,&wd);
  wd.dwStateAction=WTD_STATEACTION_CLOSE; WinVerifyTrust((HWND)INVALID_HANDLE_VALUE,&gv,&wd);
  return st==0;
}
#ifdef SW_TEST_HOOKS
bool (*g_swVerifyHook)(const std::wstring&)=nullptr;     // tests/idle_cpu_test.cpp: stands in for WinVerifyTrust
#endif

// WinVerifyTrust reads and hashes the WHOLE file (Discord, Roblox, VS Code are 100+ MB), and Token Guard asks about every running app in a user
// folder every 4 s (the process / file / WMI watchers ask again for each event). That alone kept a core busy: a "signed" answer is now kept
// until the file changes.
// shortcut: only "signed" is kept (an unsigned verdict can be a file that was locked a moment ago, so it is asked again), and a file rewritten
// to the same size and time keeps its old verdict until restart. Upgrade: key the cache on a file hash.
struct SigStamp{ ULONGLONG size,mtime; };
static SwCs g_sigCs;
static std::unordered_map<std::wstring,SigStamp> g_sigOk;        // lower-case path -> the file as it was when it verified
static bool stampOf(const std::wstring& p,SigStamp& s){
  WIN32_FILE_ATTRIBUTE_DATA a; if(!GetFileAttributesExW(p.c_str(),GetFileExInfoStandard,&a)) return false;
  s.size=((ULONGLONG)a.nFileSizeHigh<<32)|a.nFileSizeLow; s.mtime=((ULONGLONG)a.ftLastWriteTime.dwHighDateTime<<32)|a.ftLastWriteTime.dwLowDateTime; return true; }
bool isSigned(const std::wstring& path){
  if(path.empty())return false;
  SigStamp st; const bool have=stampOf(path,st); const std::wstring key=lc(path);
  if(have){ SwGuard g(g_sigCs); auto it=g_sigOk.find(key); if(it!=g_sigOk.end()&&it->second.size==st.size&&it->second.mtime==st.mtime) return true; }
#ifdef SW_TEST_HOOKS
  const bool ok=g_swVerifyHook?g_swVerifyHook(path):verifyTrust(path);
#else
  const bool ok=verifyTrust(path);
#endif
  if(ok&&have){ SwGuard g(g_sigCs); if(g_sigOk.size()>=4096) g_sigOk.clear(); g_sigOk[key]=st; }
  return ok;
}
// suspicious = runs from a user-writable location AND is not validly signed.
// Program Files / Windows, or anything properly signed, is treated as normal.
bool suspiciousExe(const std::wstring& path){
  if(path.empty())return false;
  if(!userWritable(path))return false;
  return !isSigned(path);
}
// Filenames a token/credential stealer copies out of Discord / browsers. Seeing
// one of these appear somewhere it does not belong is a strong theft signal.
bool isCredArtifactName(const std::wstring& p){
  std::wstring l=lc(p);
  for(auto m:{L"cookies",L"login data",L"web data",L"key4.db",L"key3.db",
              L"logins.json",L"places.sqlite",L".ldb",L"leveldb",L"local state"})
    if(l.find(m)!=std::wstring::npos)return true;
  return false;
}
// "staging" spots where an exfil copy of the above would land, but the real
// stores never live (so a credential file here is not just Discord/Chrome
// touching its own data).
bool isStagingDir(const std::wstring& p){ std::wstring l=lc(p);
  for(auto m:{L"\\temp\\",L"\\downloads\\",L"\\desktop\\"})
    if(l.find(m)!=std::wstring::npos)return true; return false; }
