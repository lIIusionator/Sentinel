// str_scan.cpp -- runs the strings check (str_scan.h) on a flagged program, off the watcher threads.
//
// The watchers call stringsScanAsync() whenever they flag a program (unsigned, in a user folder: new file, new process, quick scan, token access).
// One low-priority thread, alive only while there is something queued, reads the programs one at a time -- at most 16 MB each (the first and the last 8 MB
// of a bigger file: droppers keep their payload and settings at the end) -- and reports links and suspicious commands in events.log, the events list
// and a toast. Nothing is fetched or run: it only reads the file.
// shortcut: a burst of more than 32 flagged files drops the rest, and a file rewritten to the same size and time is not read again until restart;
// upgrade: persist the queue / key the "already read" memory on a file hash.
#include "watchers.h"
#include "events.h"
#include "settings.h"
#include "sw_lock.h"
#include "str_scan.h"
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

static void scanOne(const std::wstring& path){
  ULONGLONG size=0,mt=0;
  // a file that has just appeared may still be being written: wait until its size stops changing (5 s at most)
  if(!stamp(path,size,mt)||!size) return;
  for(int i=0;i<5;i++){ Sleep(1000); ULONGLONG s2=0,m2=0; if(!stamp(path,s2,m2)) return; const bool same=(s2==size); size=s2; mt=m2; if(same) break; }
  const std::wstring key=lc(path);
  { SwGuard g(g_qCs); auto it=g_seen.find(key); if(it!=g_seen.end()&&it->second.first==size&&it->second.second==mt) return; }   // this version was read already
  HANDLE h=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_SEQUENTIAL_SCAN,nullptr);
  if(h==INVALID_HANDLE_VALUE){ logLine(L"[STR] cannot read "+path+L" (error "+std::to_wstring(GetLastError())+L")"); return; }
  std::vector<uint8_t> buf; bool ok;
  if(size<=2*HALF) ok=readAt(h,0,size,buf);
  else{ ok=readAt(h,0,HALF,buf); buf.insert(buf.end(),8,0); ok=readAt(h,size-HALF,HALF,buf)&&ok; }
  CloseHandle(h);
  if(!ok){ logLine(L"[STR] could not read "+path); return; }
  { SwGuard g(g_qCs); if(g_seen.size()>=1024) g_seen.clear(); g_seen[key]=std::make_pair(size,mt); }

  const StrReport r=strscan::scan(buf.data(),buf.size());
  if(r.shown.empty()){ logLine(L"[STR] "+path+L": no links or suspicious commands in its strings"); return; }
  pushEv(L"STR",L"strings: "+std::to_wstring(r.links)+L" link(s), "+std::to_wstring(r.commands)+L" command(s)",path);
  std::wstring toast=path.substr(path.find_last_of(L'\\')+1); int n=0;
  for(const StrFinding& f:r.shown){
    const std::wstring line=std::wstring(f.hot?L"[!] ":L"")+(f.command?L"cmd: ":L"")+fromUtf8(f.text);
    logLine(L"[STR]   "+line);
    if(n++<2) toast+=L"\n"+(line.size()>52?line.substr(0,49)+L"...":line); }
  if(r.shown.size()>2) toast+=L"\n+"+std::to_wstring(r.shown.size()-2)+L" more in events.log";
  showOverlay(L"⚠ Links / commands inside it",toast,ACC_RED);
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
