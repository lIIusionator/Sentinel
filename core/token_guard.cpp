// token_guard.cpp -- Token Guard: flags unsigned user-folder processes holding open handles to login/token stores.
#include "watchers.h"
#include "events.h"
#include "settings.h"
#include "trust.h"
#include <tlhelp32.h>

// ---------- Token Guard: catch credential/token theft ----------
// Stealers read the small files where Discord and browsers keep your logged-in
// session tokens. This watcher looks for an UNSIGNED process running from a
// user-writable spot that currently holds an open handle to one of those
// stores -- i.e. reading your tokens right now. It only ever inspects processes
// already flagged suspicious, so when nothing shady is running it costs almost
// nothing. It never kills anything: it raises a WARN (which the Events tab can
// one-click firewall-block) and a toast. The staged-copy case is handled in
// DirThread above.
//
// SystemExtendedHandleInformation is not in the public headers, so the few
// shapes we need are declared here and NtQuerySystemInformation is resolved
// from ntdll at runtime.
typedef struct _SNT_HANDLE_ENTRY {
  PVOID Object; ULONG_PTR UniqueProcessId; ULONG_PTR HandleValue;
  ULONG GrantedAccess; USHORT CreatorBackTraceIndex; USHORT ObjectTypeIndex;
  ULONG HandleAttributes; ULONG Reserved;
} SNT_HANDLE_ENTRY;
typedef struct _SNT_HANDLE_INFO {
  ULONG_PTR NumberOfHandles; ULONG_PTR Reserved; SNT_HANDLE_ENTRY Handles[1];
} SNT_HANDLE_INFO;
typedef LONG (WINAPI* PFN_NTQSI)(ULONG,PVOID,ULONG,PULONG);
static const ULONG SNT_ExtendedHandleInformation=64;
static const LONG  SNT_STATUS_INFO_LEN_MISMATCH=(LONG)0xC0000004L;

static std::vector<std::wstring> g_credDirs;      // lowercased store dir prefixes
static void buildCredDirs(){
  if(!g_credDirs.empty())return;
  std::wstring ad=lc(envW(L"APPDATA")), la=lc(envW(L"LOCALAPPDATA"));
  auto add=[&](const std::wstring& p){ if(!p.empty())g_credDirs.push_back(p); };
  if(!ad.empty()){
    add(ad+L"\\discord\\local storage\\leveldb");
    add(ad+L"\\discordptb\\local storage\\leveldb");
    add(ad+L"\\discordcanary\\local storage\\leveldb");
    add(ad+L"\\opera software\\opera stable");
    add(ad+L"\\opera software\\opera gx stable");
    add(ad+L"\\mozilla\\firefox\\profiles");
  }
  if(!la.empty()){
    add(la+L"\\google\\chrome\\user data");
    add(la+L"\\google\\chrome beta\\user data");
    add(la+L"\\microsoft\\edge\\user data");
    add(la+L"\\bravesoftware\\brave-browser\\user data");
    add(la+L"\\vivaldi\\user data");
    add(la+L"\\yandex\\yandexbrowser\\user data");
  }
}
static std::wstring credLabel(const std::wstring& d){
  auto has=[&](const wchar_t* s){ return d.find(s)!=std::wstring::npos; };
  if(has(L"discord"))return L"Discord";
  if(has(L"chrome")) return L"Chrome";
  if(has(L"edge"))   return L"Edge";
  if(has(L"brave"))  return L"Brave";
  if(has(L"vivaldi"))return L"Vivaldi";
  if(has(L"yandex")) return L"Yandex";
  if(has(L"opera"))  return L"Opera";
  if(has(L"firefox"))return L"Firefox";
  return L"credential store";
}
static bool pathUnderCredDir(const std::wstring& lpath,std::wstring& storeOut){
  for(auto& d:g_credDirs) if(lpath.rfind(d,0)==0){ storeOut=d; return true; }
  return false;
}
// Duplicate a handle out of another process and resolve it to a DOS path.
static bool resolveHandlePath(HANDLE proc,ULONG_PTR hval,std::wstring& out){
  HANDLE dup=nullptr;
  if(!DuplicateHandle(proc,(HANDLE)hval,GetCurrentProcess(),&dup,0,FALSE,DUPLICATE_SAME_ACCESS)||!dup)return false;
  wchar_t buf[1024];
  DWORD n=GetFinalPathNameByHandleW(dup,buf,1023,FILE_NAME_NORMALIZED|VOLUME_NAME_DOS);
  CloseHandle(dup);
  if(n==0||n>=1024)return false;
  std::wstring p(buf,n);
  if(p.rfind(L"\\\\?\\",0)==0)p=p.substr(4);      // strip \\?\ prefix
  out=lc(p); return true;
}
DWORD WINAPI TokenGuardThread(LPVOID){
  cpuNameThread(L"tokenguard");
  buildCredDirs();
  PFN_NTQSI NtQSI=(PFN_NTQSI)GetProcAddress(GetModuleHandleW(L"ntdll.dll"),"NtQuerySystemInformation");
  std::set<DWORD> alerted;                          // pids already warned about
  std::vector<BYTE> buf(1u<<20);
  while(g_running.load()){
    for(int i=0;i<40 && g_running.load();++i) Sleep(100);   // ~4s, but wakes fast on exit
    if(!g_running.load())break;
    if(!g_tTokens.load())continue;

    // 1) suspicious processes = unsigned + user-writable. Normally none.
    struct SP{ DWORD pid; std::wstring name,path; };
    std::vector<SP> susp; std::set<DWORD> alive;
    HANDLE snap=CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0);
    if(snap!=INVALID_HANDLE_VALUE){ PROCESSENTRY32W pe; pe.dwSize=sizeof(pe);
      if(Process32FirstW(snap,&pe)){ do{
        DWORD pid=pe.th32ProcessID; if(pid<=4)continue; alive.insert(pid);
        if(pid==GetCurrentProcessId())continue;
        HANDLE ph=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid);
        std::wstring path; if(ph){ wchar_t b[MAX_PATH]; DWORD sz=MAX_PATH;
          if(QueryFullProcessImageNameW(ph,0,b,&sz))path=b; CloseHandle(ph); }
        if(!path.empty() && suspiciousExe(path)) susp.push_back({pid,pe.szExeFile,path});
      }while(Process32NextW(snap,&pe)); }
      CloseHandle(snap);
    }
    for(auto it=alerted.begin();it!=alerted.end();){ if(!alive.count(*it)) it=alerted.erase(it); else ++it; }
    if(susp.empty()||!NtQSI)continue;

    // 2) one system-wide handle snapshot; a probe file lets us learn the File
    //    object-type index so we only try to resolve actual file handles.
    HANDLE probe=CreateFileW(settingsPath().c_str(),GENERIC_READ,
        FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,0,OPEN_EXISTING,0,0);
    if(probe==INVALID_HANDLE_VALUE) probe=CreateFileW(appDir().c_str(),0,
        FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,0,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS,0);
    ULONG need=0; LONG st=SNT_STATUS_INFO_LEN_MISMATCH;
    for(int tries=0;tries<6;tries++){
      st=NtQSI(SNT_ExtendedHandleInformation,buf.data(),(ULONG)buf.size(),&need);
      if(st==SNT_STATUS_INFO_LEN_MISMATCH||(need&&need>buf.size())){ buf.resize(need?need+65536:buf.size()*2); continue; }
      break;
    }
    if(st!=0){ if(probe!=INVALID_HANDLE_VALUE)CloseHandle(probe); continue; }
    auto* info=(SNT_HANDLE_INFO*)buf.data();
    ULONG_PTR cnt=info->NumberOfHandles;
    DWORD self=GetCurrentProcessId(); USHORT fileType=0xFFFF;
    if(probe!=INVALID_HANDLE_VALUE){
      for(ULONG_PTR i=0;i<cnt;i++){ auto& e=info->Handles[i];
        if((DWORD)e.UniqueProcessId==self && e.HandleValue==(ULONG_PTR)probe){ fileType=e.ObjectTypeIndex; break; } } }

    for(auto& sp:susp){
      if(alerted.count(sp.pid))continue;
      HANDLE tp=OpenProcess(PROCESS_DUP_HANDLE|PROCESS_QUERY_LIMITED_INFORMATION,FALSE,sp.pid);
      if(!tp)continue;                                  // usually means it is elevated; needs admin to inspect
      bool hit=false; std::wstring store;
      for(ULONG_PTR i=0;i<cnt && !hit;i++){ auto& e=info->Handles[i];
        if((DWORD)e.UniqueProcessId!=sp.pid)continue;
        if(fileType!=0xFFFF && e.ObjectTypeIndex!=fileType)continue;   // only file objects
        std::wstring p; if(!resolveHandlePath(tp,e.HandleValue,p))continue;
        if(pathUnderCredDir(p,store)) hit=true;
      }
      CloseHandle(tp);
      if(hit){
        alerted.insert(sp.pid); g_tokenAlerts.fetch_add(1);
        std::wstring lbl=credLabel(store);
        pushEv(L"WARN",L"token access ("+lbl+L"): "+sp.name,sp.path);
        showOverlay(L"\u26A0 Credential/token access",
          sp.name+L"\n"+sp.path+L"\nreading your "+lbl+L" login data",ACC_RED);
        stringsScanAsync(sp.path);
      }
    }
    if(probe!=INVALID_HANDLE_VALUE)CloseHandle(probe);
  }
  return 0;
}
