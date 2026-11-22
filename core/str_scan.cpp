// str_scan.cpp -- runs the strings check (str_scan.h) on a flagged program, off the watcher threads.
//
// The watchers call stringsScanAsync() whenever they flag a program (unsigned, in a user folder: new file, new process, quick scan, token access).
// One low-priority thread, alive only while there is something queued, reads the programs one at a time -- at most 16 MB each (the first and the last 8 MB
// of a bigger file: droppers keep their payload and settings at the end) -- and reports links and suspicious commands in events.log, the events list
// and a toast. Nothing is fetched or run: it only reads the file.
// The reading and the parsing of those bytes is done by the Rust daemon (daemon/, a program of its own inside this exe), so a hostile file is never parsed in
// this process; if the daemon is missing, crashes, hangs or says something that is not an answer, the same check runs inside the app (str_scan.h) and the log says why.
// shortcut: a burst of more than 32 flagged files drops the rest, and a file rewritten to the same size and time is not read again until restart;
// upgrade: persist the queue / key the "already read" memory on a file hash.
#include "watchers.h"
#include "events.h"
#include "settings.h"
#include "sw_lock.h"
#include "str_scan.h"
#include "str_daemon.h"
#include "resource_ids.h"
#include <algorithm>
#include <deque>
#include <unordered_map>

static const ULONGLONG HALF=strscan::READ_HALF;
static SwCs g_qCs;                                                          // guards everything below
static std::deque<std::wstring> g_q;
static bool g_workerOn=false;
static std::unordered_map<std::wstring,std::pair<ULONGLONG,ULONGLONG>> g_seen;   // lower-case path -> size and time of the version already read

static bool stamp(const std::wstring& p,ULONGLONG& size,ULONGLONG& mtime){
  WIN32_FILE_ATTRIBUTE_DATA a; if(!GetFileAttributesExW(p.c_str(),GetFileExInfoStandard,&a)) return false;
  size=((ULONGLONG)a.nFileSizeHigh<<32)|a.nFileSizeLow; mtime=((ULONGLONG)a.ftLastWriteTime.dwHighDateTime<<32)|a.ftLastWriteTime.dwLowDateTime; return true; }

// appends len bytes of the file, from offset off, to out
static bool readAt(HANDLE h,ULONGLONG off,ULONGLONG len,std::vector<uint8_t>& out){
  LARGE_INTEGER li; li.QuadPart=(LONGLONG)off; if(!SetFilePointerEx(h,li,nullptr,FILE_BEGIN)) return false;
  const size_t at=out.size(); out.resize(at+(size_t)len);
  size_t got=0;
  while(got<len){ DWORD n=0; if(!ReadFile(h,out.data()+at+got,(DWORD)std::min<ULONGLONG>(len-got,1u<<20),&n,nullptr)||!n) break; got+=n; }
  out.resize(at+got); return got>0; }

// ---- the daemon ---------------------------------------------------------------------------------------------------------------------------------
static const wchar_t* const NO_DAEMON=L"this exe was built without it";
static const DWORD DAEMON_WAIT_MS=60000;                        // a 16 MB file takes well under a second
static const SIZE_T DAEMON_MEM_MAX=512u<<20;                     // the daemon needs ~40 MB at most
static std::wstring winErr(const wchar_t* what){ return std::wstring(what)+L" (error "+std::to_wstring(GetLastError())+L")"; }

// The daemon's exe is a resource of this exe (`make daemon`, then `make`). It is written to cache\sentinel-daemon.exe, and its bytes are compared with the resource before
// EVERY run: an update brings a new daemon, and a file somebody replaced in the meantime is overwritten, never run. (rule 3: nothing new is linked; it is a separate exe.)
// shortcut: a replacement between this check and the start is not closed; upgrade: start it from a handle opened with sharing that denies writing.
static bool daemonFile(std::wstring& exe,std::wstring& why){
  HRSRC r=FindResourceW(nullptr,SW_RES_DAEMON,RT_RCDATA);
  if(!r){ why=NO_DAEMON; return false; }
  HGLOBAL h=LoadResource(nullptr,r); const BYTE* p=h?(const BYTE*)LockResource(h):nullptr; const DWORD sz=SizeofResource(nullptr,r);
  if(!p||!sz){ why=L"its resource is empty"; return false; }
  exe=daemonExePath();
  HANDLE f=CreateFileW(exe.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);
  if(f!=INVALID_HANDLE_VALUE){ bool same=false; LARGE_INTEGER n;
    if(GetFileSizeEx(f,&n)&&n.QuadPart==(LONGLONG)sz){ std::vector<BYTE> b(sz); DWORD got=0; same=ReadFile(f,b.data(),sz,&got,nullptr)&&got==sz&&memcmp(b.data(),p,sz)==0; }
    CloseHandle(f); if(same) return true; }
  const std::wstring part=exe+L".part";
  f=CreateFileW(part.c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
  if(f==INVALID_HANDLE_VALUE){ why=winErr(L"cannot write the daemon"); return false; }
  DWORD wr=0; bool ok=WriteFile(f,p,sz,&wr,nullptr)&&wr==sz; CloseHandle(f);
  if(!ok||!MoveFileExW(part.c_str(),exe.c_str(),MOVEFILE_REPLACE_EXISTING)){ why=winErr(L"cannot put the daemon in place"); DeleteFileW(part.c_str()); return false; }
  return true;
}

// Runs `daemon scan <file>` and collects what it prints. The daemon lives in a job object while it reads the hostile bytes: at most ONE process (it cannot start another),
// 512 MB, and it dies with the app (KILL_ON_JOB_CLOSE: no orphan if this process is killed in the middle). It is created suspended and put in the job before its first instruction,
// has no console and below-normal priority, and is killed after 60 s. Its answer is small (<= 24 lines), so the 64 KB pipe never fills up while it runs.
// shortcut: it inherits every inheritable handle of this process (there are none that matter today); upgrade: PROC_THREAD_ATTRIBUTE_HANDLE_LIST.
static bool runDaemon(const std::wstring& exe,const std::wstring& file,std::string& out,std::wstring& why){
  SECURITY_ATTRIBUTES sa={sizeof(sa),nullptr,TRUE};
  HANDLE rd=nullptr,wr=nullptr;
  if(!CreatePipe(&rd,&wr,&sa,1u<<16)){ why=winErr(L"no pipe"); return false; }
  SetHandleInformation(rd,HANDLE_FLAG_INHERIT,0);
  HANDLE nul=CreateFileW(L"NUL",GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,&sa,OPEN_EXISTING,0,nullptr);
  HANDLE job=CreateJobObjectW(nullptr,nullptr);
  PROCESS_INFORMATION pi={}; bool started=false, ok=false;
  if(!job||nul==INVALID_HANDLE_VALUE) why=winErr(L"no job object");
  else{
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION li={};
    li.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE|JOB_OBJECT_LIMIT_ACTIVE_PROCESS|JOB_OBJECT_LIMIT_JOB_MEMORY;
    li.BasicLimitInformation.ActiveProcessLimit=1; li.JobMemoryLimit=DAEMON_MEM_MAX;
    STARTUPINFOW si={}; si.cb=sizeof(si); si.dwFlags=STARTF_USESTDHANDLES; si.hStdInput=nul; si.hStdOutput=wr; si.hStdError=wr;
    std::wstring cmd=L"\""+exe+L"\" scan \""+file+L"\"";
    if(!SetInformationJobObject(job,JobObjectExtendedLimitInformation,&li,sizeof(li))) why=winErr(L"cannot limit the job");
    else if(!CreateProcessW(nullptr,&cmd[0],nullptr,nullptr,TRUE,CREATE_SUSPENDED|DETACHED_PROCESS|BELOW_NORMAL_PRIORITY_CLASS,nullptr,nullptr,&si,&pi)) why=winErr(L"cannot start it");
    else{ started=true;
      if(!AssignProcessToJobObject(job,pi.hProcess)) why=winErr(L"cannot put it in the job");
      else if(ResumeThread(pi.hThread)==(DWORD)-1) why=winErr(L"cannot resume it");
      else ok=true; } }
  CloseHandle(wr); if(nul!=INVALID_HANDLE_VALUE) CloseHandle(nul);                      // the write end must be closed here or the read below never ends
  if(started&&!ok) TerminateProcess(pi.hProcess,1);
  if(ok){
    DWORD code=1, waitMs=DAEMON_WAIT_MS;
#ifdef SW_TEST_HOOKS
    { const std::wstring w=envW(L"SW_TEST_DAEMON_WAIT"); if(!w.empty()) waitMs=(DWORD)_wtoi(w.c_str()); }       // tests: do not wait a minute for a hung stand-in
#endif
    if(WaitForSingleObject(pi.hProcess,waitMs)!=WAIT_OBJECT_0){ TerminateJobObject(job,1); why=L"it did not answer in time ("+std::to_wstring(waitMs/1000)+L" s)"; ok=false; }
    else if(!GetExitCodeProcess(pi.hProcess,&code)||code!=0) ok=false;
    if(ok||code==2){ char b[4096]; DWORD n=0; while(out.size()<(1u<<16)&&ReadFile(rd,b,sizeof b,&n,nullptr)&&n) out.append(b,n); }
    if(!ok&&why.empty()){ why=L"it ended with code "+std::to_wstring(code); if(out.compare(0,4,"ERR ")==0) why+=L": "+fromUtf8(out.substr(4,out.find('\n')-4)); } }
  if(started){ CloseHandle(pi.hThread); CloseHandle(pi.hProcess); }
  CloseHandle(rd); if(job) CloseHandle(job);
  return ok;
}

static bool daemonScan(const std::wstring& path,StrReport& r,std::wstring& why){
  std::wstring exe;
#ifdef SW_TEST_HOOKS
  exe=envW(L"SW_TEST_DAEMON");                                                         // "off" = no daemon; a path = that program stands in for it
  if(exe==L"off"){ why=L"switched off (SW_TEST_DAEMON)"; return false; }
  if(exe.empty()&&!daemonFile(exe,why)) return false;
#else
  if(!daemonFile(exe,why)) return false;
#endif
  std::string out; if(!runDaemon(exe,path,out,why)) return false;
  if(!strscan::parseAnswer(out,r)){ why=L"its answer was not valid"; return false; }
  return true;
}

// The check inside the app: reads the file (the first and last 8 MB of a big one) and runs strscan::scan on it.
static bool scanInProcess(const std::wstring& path,ULONGLONG size,StrReport& r){
  HANDLE h=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_SEQUENTIAL_SCAN,nullptr);
  if(h==INVALID_HANDLE_VALUE){ logLine(L"[STR] cannot read "+path+L" (error "+std::to_wstring(GetLastError())+L")"); return false; }
  std::vector<uint8_t> buf; bool ok;
  if(size<=2*HALF) ok=readAt(h,0,size,buf);
  else{ ok=readAt(h,0,HALF,buf); buf.insert(buf.end(),8,0); ok=readAt(h,size-HALF,HALF,buf)&&ok; }
  CloseHandle(h);
  if(!ok){ logLine(L"[STR] could not read "+path); return false; }
  r=strscan::scan(buf.data(),buf.size()); return true;
}

static void scanOne(const std::wstring& path){
  ULONGLONG size=0,mt=0;
  // a file that has just appeared may still be being written: wait until its size stops changing (5 s at most)
  if(!stamp(path,size,mt)||!size) return;
  for(int i=0;i<5;i++){ Sleep(1000); ULONGLONG s2=0,m2=0; if(!stamp(path,s2,m2)) return; const bool same=(s2==size); size=s2; mt=m2; if(same) break; }
  const std::wstring key=lc(path);
  { SwGuard g(g_qCs); auto it=g_seen.find(key); if(it!=g_seen.end()&&it->second.first==size&&it->second.second==mt) return; }   // this version was read already
  StrReport r; std::wstring why; const ULONGLONG t0=GetTickCount64(); static bool noted=false;   // (only the one worker thread touches `noted`)
  if(daemonScan(path,r,why)) logLine(L"[STR] daemon: "+path+L" scanned in "+std::to_wstring(GetTickCount64()-t0)+L" ms");
  else{
    if(why!=NO_DAEMON||!noted) logLine(L"[STR] no daemon ("+why+L"): scanning "+path+L" inside the app");   // "built without it" is said once, not for every file
    noted|=(why==NO_DAEMON);
    if(!scanInProcess(path,size,r)) return; }
  { SwGuard g(g_qCs); if(g_seen.size()>=1024) g_seen.clear(); g_seen[key]=std::make_pair(size,mt); }

  if(r.shown.empty()){ logLine(L"[STR] "+path+L": no links or suspicious commands in its strings"); return; }
  pushEv(L"STR",L"strings: "+std::to_wstring(r.links)+L" link(s), "+std::to_wstring(r.commands)+L" command(s)",path);
  std::wstring toast=path.substr(path.find_last_of(L'\\')+1); int n=0;
  for(const StrFinding& f:r.shown){
    const std::wstring line=std::wstring(f.hot?L"[!] ":L"")+(f.command?L"cmd: ":L"")+fromUtf8(f.text);
    logLine(L"[STR]   "+line);
    if(n++<2) toast+=L"\n"+(line.size()>52?line.substr(0,49)+L"...":line); }
  if(r.shown.size()>2) toast+=L"\n+"+std::to_wstring(r.shown.size()-2)+L" more in events.log";
  showOverlay(L"⚠ Links / commands inside it",toast,ACC_RED);
#ifdef SW_TEST_HOOKS
  if(envW(L"SW_TEST_HOLD")==L"1") holdFile(path);                               // tests: what the Events tab does when the user says yes
#endif
}

static DWORD WINAPI worker(LPVOID){
  cpuNameThread(L"strings");
  for(;;){ std::wstring p;
    { SwGuard g(g_qCs); if(g_q.empty()||!g_running.load()){ g_workerOn=false; g_q.clear(); return 0; } p=g_q.front(); g_q.pop_front(); }
    try{ scanOne(p); } catch(...){ logLine(L"[STR] the check of "+p+L" failed (out of memory?)"); } }   // a thread must never end the app
}

void stringsScanAsync(const std::wstring& path){
  if(!g_tStrings.load()||path.empty()||path[0]==L'('||lc(path).find(L"\\sentinelwatch\\")!=std::wstring::npos) return;   // off; "(running)" placeholders; our own data
  wchar_t self[MAX_PATH]; const DWORD sl=GetModuleFileNameW(nullptr,self,MAX_PATH); if(sl&&sl<MAX_PATH&&lc(path)==lc(self)) return;   // not this exe
  { SwGuard g(g_qCs);
    if(g_q.size()>=32||std::find(g_q.begin(),g_q.end(),path)!=g_q.end()) return;
    g_q.push_back(path); if(g_workerOn) return; g_workerOn=true; }
  HANDLE t=CreateThread(nullptr,0,worker,nullptr,0,nullptr);
  if(t){ SetThreadPriority(t,THREAD_PRIORITY_BELOW_NORMAL); CloseHandle(t); }
  else{ SwGuard g(g_qCs); g_workerOn=false; g_q.clear(); }
}
