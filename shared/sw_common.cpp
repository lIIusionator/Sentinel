// sw_common.cpp -- shared globals and small utilities.
#include "sw_common.h"
#include "sw_lock.h"
#include <shlobj.h>
#include <tlhelp32.h>
#include <algorithm>
#include <fstream>
#include <map>

HINSTANCE g_hInst=nullptr;
HWND g_msg=nullptr;
std::atomic<bool> g_running{true};
std::atomic<bool> g_dirty{true};
ULONGLONG g_start=0;
std::wstring g_userName=L"Lunaris";

std::wstring envW(const wchar_t* n){ wchar_t b[MAX_PATH];
  DWORD k=GetEnvironmentVariableW(n,b,MAX_PATH); return (k&&k<MAX_PATH)?std::wstring(b,k):L""; }
std::string toU8(const std::wstring& w){ if(w.empty())return{};
  int n=WideCharToMultiByte(CP_UTF8,0,w.data(),(int)w.size(),0,0,0,0);
  std::string s(n,0); WideCharToMultiByte(CP_UTF8,0,w.data(),(int)w.size(),s.data(),n,0,0); return s; }
std::wstring fromUtf8(const std::string& s){ if(s.empty())return{};
  int n=MultiByteToWideChar(CP_UTF8,0,s.data(),(int)s.size(),0,0); std::wstring w(n,0);
  MultiByteToWideChar(CP_UTF8,0,s.data(),(int)s.size(),w.data(),n); return w; }
std::wstring lc(std::wstring s){ for(auto&c:s)c=(wchar_t)towlower(c); return s; }
bool endsW(const std::wstring&s,const std::wstring&e){
  return s.size()>=e.size()&&lc(s.substr(s.size()-e.size()))==e; }

std::wstring appDir(){ std::wstring d=envW(L"LOCALAPPDATA"); if(d.empty())d=envW(L"TEMP"); if(d.empty())d=L".";
  d+=L"\\SentinelWatch"; SHCreateDirectoryExW(0,d.c_str(),0); return d; }
std::wstring cacheDir(){ std::wstring d=appDir()+L"\\cache"; SHCreateDirectoryExW(0,d.c_str(),0); return d; }
std::wstring logPath(){ return appDir()+L"\\events.log"; }

// The log is written from many threads (watchers, AFK render thread, decode threads). One lock keeps
// lines from interleaving. Constructed during static initialisation, i.e. before any thread exists.
static CRITICAL_SECTION g_logCs;
static struct LogLockInit{ LogLockInit(){ InitializeCriticalSection(&g_logCs); } } s_logLockInit;

void logLine(const std::wstring& s){
  SYSTEMTIME t; GetLocalTime(&t); wchar_t ts[64];
  wsprintfW(ts,L"[%02d:%02d:%02d] ",t.wHour,t.wMinute,t.wSecond);
  std::string u=toU8(std::wstring(ts)+s);
  EnterCriticalSection(&g_logCs);
  { std::ofstream f(wpath(logPath()),std::ios::app|std::ios::binary); f.write(u.data(),(std::streamsize)u.size()); f.put('\n'); }
  LeaveCriticalSection(&g_logCs);
}

void logRotate(){
  std::wstring p=logPath(); WIN32_FILE_ATTRIBUTE_DATA fa={};
  if(!GetFileAttributesExW(p.c_str(),GetFileExInfoStandard,&fa)) return;
  ULONGLONG sz=((ULONGLONG)fa.nFileSizeHigh<<32)|fa.nFileSizeLow;
  if(sz>4ull*1024*1024){ std::wstring old=appDir()+L"\\events.old.log"; MoveFileExW(p.c_str(),old.c_str(),MOVEFILE_REPLACE_EXISTING); }
}

// ---------- where the CPU goes ----------
//   [CPU] last 60 s: app 28.4 % of the CPU (4 logical) | tokenguard 24.9 | ui 2.1 | tid 4412 0.9      (a thread that never called cpuNameThread shows as "tid N")
// "app" is on the Task Manager scale (all logical CPUs = 100 %); only live threads are listed, so a thread that ended inside the minute is in "app" but not in the list.
struct CpuName{ DWORD tid; wchar_t name[16]; };
static CpuName g_cpuName[32]; static int g_cpuNames=0; static SwCs g_cpuCs;
void cpuNameThread(const wchar_t* name){
  SwGuard g(g_cpuCs); if(g_cpuNames>=32) return;            // (the AFK thread is respawned at most a few times: 32 names are plenty)
  g_cpuName[g_cpuNames].tid=GetCurrentThreadId(); lstrcpynW(g_cpuName[g_cpuNames].name,name,16); g_cpuNames++; }
static ULONGLONG ft64(const FILETIME& f){ return ((ULONGLONG)f.dwHighDateTime<<32)|f.dwLowDateTime; }
static void cpuSample(std::map<DWORD,ULONGLONG>& out){      // thread id -> CPU time used so far (100 ns), live threads of this process
  HANDLE s=CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD,0); if(s==INVALID_HANDLE_VALUE) return;
  THREADENTRY32 te; te.dwSize=sizeof(te); const DWORD me=GetCurrentProcessId();
  for(BOOL ok=Thread32First(s,&te);ok;ok=Thread32Next(s,&te)){ if(te.th32OwnerProcessID!=me) continue;
    HANDLE h=OpenThread(THREAD_QUERY_LIMITED_INFORMATION,FALSE,te.th32ThreadID); if(!h) continue;
    FILETIME c,e,k,u; if(GetThreadTimes(h,&c,&e,&k,&u)) out[te.th32ThreadID]=ft64(k)+ft64(u);
    CloseHandle(h); }
  CloseHandle(s); }
void cpuReport(ULONGLONG everyMs){
  static ULONGLONG t0=0,p0=0; static std::map<DWORD,ULONGLONG> last;
  const ULONGLONG now=GetTickCount64();
  if(t0&&now-t0<everyMs) return;
  FILETIME c,e,k,u; GetProcessTimes(GetCurrentProcess(),&c,&e,&k,&u); const ULONGLONG p=ft64(k)+ft64(u);
  std::map<DWORD,ULONGLONG> cur; cpuSample(cur);
  if(t0){
    SYSTEM_INFO si; GetSystemInfo(&si); const DWORD ncpu=si.dwNumberOfProcessors?si.dwNumberOfProcessors:1;
    const double wall=(double)(now-t0)*10000.0*ncpu;         // 100 ns units x logical CPUs
    const double app=100.0*(double)(p-p0)/wall;
    if(app>=3.0){
      std::vector<std::pair<double,DWORD>> top;
      for(auto& kv:cur){ auto it=last.find(kv.first); const double pc=100.0*(double)(kv.second-(it==last.end()?0:it->second))/wall; if(pc>=0.5) top.push_back({pc,kv.first}); }
      std::sort(top.begin(),top.end(),[](const std::pair<double,DWORD>& a,const std::pair<double,DWORD>& b){ return a.first>b.first; });
      std::wstring l=L"[CPU] last "+std::to_wstring((now-t0)/1000)+L" s: app "+fmt1(app)+L" % of the CPU ("+std::to_wstring(ncpu)+L" logical)";
      for(size_t i=0;i<top.size()&&i<6;i++){ std::wstring nm=L"tid "+std::to_wstring(top[i].second);
        { SwGuard g(g_cpuCs); for(int j=0;j<g_cpuNames;j++) if(g_cpuName[j].tid==top[i].second) nm=g_cpuName[j].name; }
        l+=L" | "+nm+L" "+fmt1(top[i].first); }
      logLine(l); } }
  t0=now; p0=p; last.swap(cur);
}

std::wstring hrHex(HRESULT hr){ wchar_t b[16]; wsprintfW(b,L"0x%08X",(unsigned)hr); return b; }
std::wstring fmt1(double v){ int x=(int)(v*10.0+(v<0?-0.5:0.5)); return std::to_wstring(x/10)+L"."+std::to_wstring(x<0?-(x%10):x%10); }
int64_t hnsNow(){ static LARGE_INTEGER f={}; if(!f.QuadPart) QueryPerformanceFrequency(&f);
  LARGE_INTEGER c; QueryPerformanceCounter(&c);
  return (int64_t)(c.QuadPart/f.QuadPart)*10000000LL + (int64_t)((c.QuadPart%f.QuadPart)*10000000LL/f.QuadPart); }
