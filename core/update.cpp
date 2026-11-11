// update.cpp -- Windows side of "Automatic updates". What counts as a newer release, a valid release and a good download is shared/update.h (tests/update_tests.cpp); this
// file is the HTTPS calls, the worker thread and the swap of the program file.
//
//   * winhttp.dll is loaded, by full path, when the worker first needs it (no new static import; a PC without it just cannot check, and says so).
//   * The program file is never edited in place. The new exe is downloaded next to the running one as <exe>.new and checked against SHA256SUMS.txt. When nothing is going
//     on (no AFK screen, dashboard closed, no keyboard or mouse input for a minute) the running exe is renamed to <exe>.old -- Windows allows that for a running program --
//     <exe>.new takes its name, the new copy is started with --after-update, and this copy exits. The new copy waits for the single-instance mutex (ui/main.cpp) and then
//     deletes <exe>.old. Any failure on the way puts the old file back.
//   * One worker thread (below normal priority) does all the network work and sleeps in between. Everything that touches windows or ends the process runs on the UI thread.
#include "sw_common.h"
#include "sw_lock.h"
#include "settings.h"
#include "watchers.h"
#include "events.h"
#include "autostart.h"
#include "update.h"
#include <winhttp.h>        // types and constants only: the functions are fetched from winhttp.dll at run time
#include <fstream>
#include <cstdlib>

namespace{
typedef HINTERNET (WINAPI *PFN_Open)(LPCWSTR,DWORD,LPCWSTR,LPCWSTR,DWORD);
typedef HINTERNET (WINAPI *PFN_Connect)(HINTERNET,LPCWSTR,INTERNET_PORT,DWORD);
typedef HINTERNET (WINAPI *PFN_OpenReq)(HINTERNET,LPCWSTR,LPCWSTR,LPCWSTR,LPCWSTR,LPCWSTR*,DWORD);
typedef BOOL (WINAPI *PFN_Send)(HINTERNET,LPCWSTR,DWORD,LPVOID,DWORD,DWORD,DWORD_PTR);
typedef BOOL (WINAPI *PFN_Recv)(HINTERNET,LPVOID);
typedef BOOL (WINAPI *PFN_Query)(HINTERNET,DWORD,LPCWSTR,LPVOID,LPDWORD,LPDWORD);
typedef BOOL (WINAPI *PFN_Read)(HINTERNET,LPVOID,DWORD,LPDWORD);
typedef BOOL (WINAPI *PFN_Close)(HINTERNET);
typedef BOOL (WINAPI *PFN_Time)(HINTERNET,int,int,int,int);

#ifdef SW_TEST_HOOKS
const bool ALLOW_HTTP=true;       // test builds talk to a server on this PC (SW_TEST_UPDATE_URL) ...
#else
const bool ALLOW_HTTP=false;      // ... a release build never speaks plain http
#endif

SwCs g_updCs;                                    // guards g_status and g_readyTag
std::wstring g_status;                        // the one line the Updates tab shows
std::string g_readyTag;                       // the version of the exe that waits as <exe>.new
std::atomic<bool> g_ready{false};             // a downloaded, checked exe waits as <exe>.new
std::atomic<bool> g_force{false};             // "Check now" / the switch turned on: look even if the schedule says not yet
std::atomic<bool> g_applying{false};          // the swap is done and the exit is on its way
std::atomic<int>  g_phase{0};                 // 0 idle, 1 asking GitHub, 2 downloading
std::atomic<long long> g_got{0}, g_total{0};  // download progress in bytes (total 0: not known)
HANDLE g_wake=nullptr;                        // auto-reset event: wakes the worker for a forced look

void setStatus(const std::wstring& s){ { SwGuard g(g_updCs); g_status=s; } g_dirty.store(true); }
std::wstring W(const std::string& s){ return fromUtf8(s); }
std::wstring clockNow(){ SYSTEMTIME t; GetLocalTime(&t); wchar_t b[16]; wsprintfW(b,L"%02u:%02u",(unsigned)t.wHour,(unsigned)t.wMinute); return b; }
std::wstring appliedPath(){ return appDir()+L"\\update_applied.txt"; }
std::string readApplied(){ std::ifstream f(wpath(appliedPath())); std::string s; std::getline(f,s); while(!s.empty()&&(s.back()=='\r'||s.back()==' ')) s.pop_back(); return s; }
void writeApplied(const std::string& tag){ std::ofstream f(wpath(appliedPath()),std::ios::trunc|std::ios::binary); f<<tag<<"\n"; }

// What the test builds can change from the environment: where GitHub is, how soon the first look is, how long the PC must have been idle.
int64_t firstDelayMs(){
#ifdef SW_TEST_HOOKS
  wchar_t b[16]; if(GetEnvironmentVariableW(L"SW_TEST_UPDATE_FIRST",b,16)) return _wtoi(b);
#endif
  return UPD_FIRST_CHECK_MS; }
int64_t idleNeededMs(){
#ifdef SW_TEST_HOOKS
  wchar_t b[16]; if(GetEnvironmentVariableW(L"SW_TEST_UPDATE_IDLE",b,16)) return (int64_t)_wtoi(b)*1000;
#endif
  return 60*1000; }
std::string apiUrl(std::string& assetPrefix){      // assetPrefix: "" = the real one (https://github.com/<repo>/releases/download/), else what the test server's addresses start with
  assetPrefix.clear();
#ifdef SW_TEST_HOOKS
  char b[600]; const DWORD n=GetEnvironmentVariableA("SW_TEST_UPDATE_URL",b,sizeof(b));
  if(n&&n<sizeof(b)){ const std::string u(b,n); const size_t s=u.find("//"), e=s==std::string::npos?s:u.find('/',s+2); if(e!=std::string::npos){ assetPrefix=u.substr(0,e+1); return u; } }
#endif
  return std::string("https://api.github.com/repos/")+UPD_REPO+"/releases/latest"; }

struct Http{
  HMODULE m=nullptr; bool tried=false; HINTERNET ses=nullptr;
  PFN_Open pOpen=nullptr; PFN_Connect pConnect=nullptr; PFN_OpenReq pOpenReq=nullptr; PFN_Send pSend=nullptr; PFN_Recv pRecv=nullptr;
  PFN_Query pQuery=nullptr; PFN_Read pRead=nullptr; PFN_Close pClose=nullptr; PFN_Time pTime=nullptr;
  ~Http(){ if(ses&&pClose) pClose(ses); }
  bool ready(std::string& err){
    if(!tried){ tried=true;
      wchar_t sys[MAX_PATH]; const UINT n=GetSystemDirectoryW(sys,MAX_PATH);                // by full path: never a winhttp.dll lying next to the exe
      if(n&&n<MAX_PATH) m=LoadLibraryW((std::wstring(sys)+L"\\winhttp.dll").c_str());
      if(m){ pOpen=(PFN_Open)(void*)GetProcAddress(m,"WinHttpOpen"); pConnect=(PFN_Connect)(void*)GetProcAddress(m,"WinHttpConnect");
        pOpenReq=(PFN_OpenReq)(void*)GetProcAddress(m,"WinHttpOpenRequest"); pSend=(PFN_Send)(void*)GetProcAddress(m,"WinHttpSendRequest");
        pRecv=(PFN_Recv)(void*)GetProcAddress(m,"WinHttpReceiveResponse"); pQuery=(PFN_Query)(void*)GetProcAddress(m,"WinHttpQueryHeaders");
        pRead=(PFN_Read)(void*)GetProcAddress(m,"WinHttpReadData"); pClose=(PFN_Close)(void*)GetProcAddress(m,"WinHttpCloseHandle");
        pTime=(PFN_Time)(void*)GetProcAddress(m,"WinHttpSetTimeouts"); } }
    if(!(pOpen&&pConnect&&pOpenReq&&pSend&&pRecv&&pQuery&&pRead&&pClose&&pTime)){ err="winhttp.dll is not available on this PC"; return false; }
    if(!ses){ const std::wstring ua=std::wstring(L"SentinelWatch/")+SW_VERSION_W;
      ses=pOpen(ua.c_str(),WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,nullptr,nullptr,0);           // the proxy settings of the user / the network (Windows 8.1 and later) ...
      if(!ses) ses=pOpen(ua.c_str(),WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,nullptr,nullptr,0);    // ... or the machine-wide ones
      if(!ses){ err="WinHttpOpen failed, error "+std::to_string(GetLastError()); return false; }
      pTime(ses,15000,15000,30000,60000); }
    return true; }

  // The reply body goes to `mem` (at most `cap` bytes) or to `file` (hashed on the way through `sha`). 200 = good; anything else with `err` filled in.
  int body(HINTERNET req,uint64_t cap,std::string* mem,HANDLE file,Sha256* sha,std::string& err){
    DWORD cl=0,sl=sizeof(cl); const bool known=pQuery(req,WINHTTP_QUERY_CONTENT_LENGTH|WINHTTP_QUERY_FLAG_NUMBER,nullptr,&cl,&sl,nullptr)!=FALSE;
    if(known&&cl>cap){ err="the file is larger than allowed ("+std::to_string(cl)+" bytes)"; return 0; }
    g_total.store(known?(long long)cl:0); g_got.store(0);
    std::vector<BYTE> buf(64*1024); uint64_t got=0;
    for(;;){ DWORD n=0;
      if(!pRead(req,buf.data(),(DWORD)buf.size(),&n)){ err="the transfer broke off, error "+std::to_string(GetLastError()); return 0; }
      if(!n) break;
      got+=n; if(got>cap){ err="the file is larger than allowed"; return 0; }
      if(mem) mem->append((const char*)buf.data(),n);
      if(file){ DWORD w=0; if(!WriteFile(file,buf.data(),n,&w,nullptr)||w!=n){ err="cannot write the file, error "+std::to_string(GetLastError()); return 0; } }
      if(sha) sha->update(buf.data(),n);
      g_got.store((long long)got);
      if(!g_running.load()){ err="the program is closing"; return 0; } }
    if(known&&got!=cl){ err="the transfer ended early"; return 0; }
    return 200; }
  int get(const std::string& url,const wchar_t* accept,uint64_t cap,std::string* mem,HANDLE file,Sha256* sha,std::string& err){
    UpdUrl pu; if(!updParseUrl(url,ALLOW_HTTP,pu)){ err="not an https address: "+url.substr(0,100); return 0; }
    const std::wstring host=W(pu.host), path=W(pu.path), hdr=std::wstring(L"Accept: ")+accept+L"\r\n";
    HINTERNET con=pConnect(ses,host.c_str(),(INTERNET_PORT)pu.port,0); if(!con){ err="cannot reach "+pu.host+", error "+std::to_string(GetLastError()); return 0; }
    HINTERNET req=pOpenReq(con,L"GET",path.c_str(),nullptr,nullptr,nullptr,pu.https?WINHTTP_FLAG_SECURE:0);   // redirects (github.com -> its download servers) are followed by WinHTTP
    int code=0;
    if(!req||!pSend(req,hdr.c_str(),(DWORD)-1,nullptr,0,0,0)||!pRecv(req,nullptr)) err="no answer from "+pu.host+", error "+std::to_string(GetLastError());
    else{ DWORD sc=0,sl=sizeof(sc);
      if(!pQuery(req,WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,nullptr,&sc,&sl,nullptr)) err="no status line from "+pu.host;
      else if(sc!=200){ code=(int)sc; err="HTTP "+std::to_string(sc)+" from "+pu.host; }
      else code=body(req,cap,mem,file,sha,err); }
    if(req) pClose(req);
    pClose(con); return code; }
};

// One look at GitHub (worker thread). false: it failed in a way that is worth trying again soon (no network, GitHub busy, a bad download); true: it knows the answer.
bool checkOnce(Http& h){
  std::string err, prefix;
  auto fail=[&](const std::string& why)->bool{ logLine(L"[UPD] check failed: "+W(why)); setStatus(L"Could not check for updates ("+W(why)+L"), will try again"); return false; };
  if(g_ready.load()) return true;           // one downloaded update at a time: it installs first, the next release is seen after the restart
  if(!h.ready(err)) return fail(err);
  g_phase.store(1); setStatus(L"Checking GitHub...");
  std::string body; const std::string api=apiUrl(prefix);
  const int c1=h.get(api,L"application/vnd.github+json",UPD_MAX_JSON,&body,nullptr,nullptr,err);
  if(c1==404) return fail("GitHub has no published release for "+std::string(UPD_REPO)+" yet (HTTP 404; a private repository answers the same way)");
  if(c1!=200) return fail(err);
  UpdateRelease rel; if(!updParseRelease(body,UPD_REPO,rel,err,prefix)) return fail(err);
  const std::wstring cur=SW_VERSION_W, tag=W(rel.tag);
  if(!verNewer(SW_VERSION,rel.tag)){ logLine(L"[UPD] up to date: this is "+cur+L", the latest release is "+tag); setStatus(L"Up to date ("+cur+L"), checked "+clockNow()); return true; }
  if(updAlreadyApplied(readApplied(),rel.tag)){
    logLine(L"[UPD] "+tag+L" was installed before, but this program still calls itself "+cur+L": not downloading it again");
    setStatus(L"Installed "+tag+L", but the program still reports "+cur+L" (a faulty release?)"); return true; }
  std::string sums, want;
  if(h.get(rel.sumsUrl,L"text/plain, application/octet-stream",64*1024,&sums,nullptr,nullptr,err)!=200) return fail(err);
  if(!updSumFor(sums,UPD_EXE_ASSET,want)) return fail(std::string(UPD_SUMS_ASSET)+" does not list "+UPD_EXE_ASSET);
  const std::wstring exe=ownExe(), part=exe+L".new";
  if(exe.empty()) return fail("cannot find this program's own file");
  const HANDLE f=CreateFileW(part.c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
  if(f==INVALID_HANDLE_VALUE) return fail("cannot create "+toU8(part)+", error "+std::to_string(GetLastError())+" (is the program folder writable?)");
  logLine(L"[UPD] "+tag+L" is available (this is "+cur+L"): downloading");
  g_phase.store(2); setStatus(L"Downloading "+tag+L"...");
  Sha256 sha; const int code=h.get(rel.exeUrl,L"application/octet-stream",UPD_MAX_EXE,nullptr,f,&sha,err);
  CloseHandle(f); g_phase.store(0);
  if(code!=200){ DeleteFileW(part.c_str()); return fail(err); }
  const std::string got=sha.hex();
  if(got!=want){ DeleteFileW(part.c_str()); return fail("the download does not match its checksum (got "+got.substr(0,12)+"..., the release lists "+want.substr(0,12)+"...)"); }
  { SwGuard g(g_updCs); g_readyTag=rel.tag; } g_ready.store(true);
  logLine(L"[UPD] "+tag+L" downloaded, SHA-256 matches: it installs when the PC is idle");
  setStatus(tag+L" downloaded: installs when you are away from the PC");
  return true; }

DWORD WINAPI UpdThread(LPVOID){
  cpuNameThread(L"update");
  Http h; int64_t wait=firstDelayMs();
  while(g_running.load()){
    WaitForSingleObject(g_wake,(DWORD)(wait>0x7fffffffLL?0x7fffffffLL:wait));
    if(!g_running.load()) break;
    const bool forced=g_force.exchange(false);
    if(!forced&&!g_tUpdate.load()){ setStatus(L"Automatic updates are off"); wait=UPD_EVERY_MS; continue; }
    const bool ok=checkOnce(h); g_phase.store(0);
    wait=ok?UPD_EVERY_MS:UPD_RETRY_MS; }
  return 0; }

// The swap (UI thread). Every step that can fail puts the files back as they were; only a started new copy makes the old one leave.
void applyNow(){
  std::string tag; { SwGuard g(g_updCs); tag=g_readyTag; }
  const std::wstring exe=ownExe(), nw=exe+L".new", old=exe+L".old", wt=W(tag);
  auto fail=[&](const std::wstring& why){ logLine(L"[UPD] cannot install "+wt+L": "+why); setStatus(L"Could not install "+wt+L" ("+why+L")"); g_ready.store(false); DeleteFileW(nw.c_str()); };
  if(exe.empty()||GetFileAttributesW(nw.c_str())==INVALID_FILE_ATTRIBUTES) return fail(L"the downloaded file is missing");
  if(!MoveFileExW(exe.c_str(),old.c_str(),MOVEFILE_REPLACE_EXISTING)) return fail(L"cannot rename the running program, error "+std::to_wstring(GetLastError()));
  if(!MoveFileW(nw.c_str(),exe.c_str())){ const DWORD e=GetLastError(); MoveFileExW(old.c_str(),exe.c_str(),MOVEFILE_REPLACE_EXISTING); return fail(L"cannot put the new file in place, error "+std::to_wstring(e)); }
  std::wstring cmd=L"\""+exe+L"\" "+AUTOSTART_FLAG+L" "+UPD_RESTART_FLAG;      // hidden in the tray, like every start that nobody asked for
  STARTUPINFOW si={}; si.cb=sizeof(si); PROCESS_INFORMATION pi={};
  if(!CreateProcessW(exe.c_str(),&cmd[0],nullptr,nullptr,FALSE,0,nullptr,nullptr,&si,&pi)){ const DWORD e=GetLastError();
    MoveFileExW(exe.c_str(),nw.c_str(),MOVEFILE_REPLACE_EXISTING); MoveFileExW(old.c_str(),exe.c_str(),MOVEFILE_REPLACE_EXISTING);
    return fail(L"the new program does not start, error "+std::to_wstring(e)); }
  CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
  writeApplied(tag); g_applying.store(true);                                  // see updAlreadyApplied
  logLine(L"[UPD] installed "+wt+L" (was "+SW_VERSION_W+L"): restarting");
  PostMessageW(g_msg,WM_COMMAND,ID_EXIT,0); }
}   // namespace

void updateStart(){
  g_wake=CreateEventW(nullptr,FALSE,FALSE,nullptr); if(!g_wake){ setStatus(L"Updates are not available (no event object)"); return; }
  setStatus(g_tUpdate.load()?L"Will check GitHub shortly after start":L"Automatic updates are off");
  HANDLE t=CreateThread(nullptr,0,UpdThread,nullptr,0,nullptr); if(t){ SetThreadPriority(t,THREAD_PRIORITY_BELOW_NORMAL); CloseHandle(t); } }

void updateTidy(){ const std::wstring exe=ownExe(); if(exe.empty()) return; DeleteFileW((exe+L".new").c_str()); DeleteFileW((exe+L".old").c_str()); }   // fails quietly while the old copy is still exiting

void updateCheckNow(){ g_force.store(true); if(g_wake) SetEvent(g_wake); }

void updateTick(bool busy){
  if(!g_ready.load()||g_applying.load()||busy) return;
  LASTINPUTINFO li; li.cbSize=sizeof(li);
  if(!GetLastInputInfo(&li)||(int64_t)(DWORD)(GetTickCount()-li.dwTime)<idleNeededMs()) return;
  applyNow(); }

std::wstring updateStatus(){
  std::wstring s; { SwGuard g(g_updCs); s=g_status; }
  if(g_phase.load()==2&&g_total.load()>0) s+=L" "+std::to_wstring((int)(g_got.load()*100/g_total.load()))+L" %";
  return s; }
