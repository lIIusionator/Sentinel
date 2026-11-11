// idle_cpu_test.cpp -- the two pieces of the "idle CPU" fix that have logic of their own.
//
//   1. isSigned() (core/trust.cpp) asks WinVerifyTrust -- which hashes the whole file -- only when it has to: a "signed" answer is kept until the file
//      changes, a "not signed" answer is never kept. A stand-in replaces WinVerifyTrust (SW_TEST_HOOKS) so the calls can be counted.
//   2. cpuReport() (shared/sw_common.cpp) writes one "[CPU]" line naming the busy thread when the app used 3 % or more.
//
//   make idletest      builds idle_cpu_test.exe (cross: make CROSS=x86_64-w64-mingw32- idletest) -- run it on Windows or under Wine.
//   Exit code 0 = every check passed.
#include "sw_common.h"
#include "trust.h"
#include <cstdio>
#include <fstream>
#include <iterator>

extern bool (*g_swVerifyHook)(const std::wstring&);

static int g_checks=0, g_failed=0;
#define CHECK(c) do{ g_checks++; if(!(c)){ g_failed++; printf("  FAIL  line %d: %s\n",__LINE__,#c); } }while(0)

static int g_calls=0; static bool g_answer=true;
static bool fakeVerify(const std::wstring&){ g_calls++; return g_answer; }

static void cacheChecks(const std::wstring& dir){
  printf("isSigned cache\n");
  const std::wstring f=dir+L"\\Some App.exe";
  auto write=[&](const char* s){ std::ofstream o(wpath(f),std::ios::binary|std::ios::trunc); o<<s; };
  g_swVerifyHook=fakeVerify;
  write("one");
  g_answer=true;
  CHECK(isSigned(f)); CHECK(isSigned(f)); CHECK(isSigned(lc(f)));
  CHECK(g_calls==1);                                  // asked once; the same path in other letters is the same file
  write("a longer file now");
  CHECK(isSigned(f)); CHECK(g_calls==2);              // the file changed: asked again, then remembered again
  CHECK(isSigned(f)); CHECK(g_calls==2);
  g_answer=false; write("changed again!");
  CHECK(!isSigned(f)); CHECK(!isSigned(f));
  CHECK(g_calls==4);                                  // a "not signed" answer is never kept
  g_answer=true;
  CHECK(isSigned(f)); CHECK(g_calls==5);
  CHECK(!isSigned(L"")); CHECK(g_calls==5);           // no path: no question, no answer
  g_answer=true;
  CHECK(isSigned(dir+L"\\missing.exe")); CHECK(isSigned(dir+L"\\missing.exe")); CHECK(g_calls==7);   // a file that cannot be stat'ed is never cached either
  DeleteFileW(f.c_str());
  g_swVerifyHook=nullptr;
}

static HANDLE g_stop;
static DWORD WINAPI burner(LPVOID){
  cpuNameThread(L"burner");
  const ULONGLONG t=GetTickCount64(); volatile unsigned x=0;
  while(GetTickCount64()-t<1500) x=x+1;               // 1.5 s of CPU
  WaitForSingleObject(g_stop,30000);                  // stay alive: only live threads are listed
  return 0; }

static void cpuChecks(const std::wstring& dir){
  printf("cpuReport\n");
  SetEnvironmentVariableW(L"LOCALAPPDATA",dir.c_str());     // the log goes here
  DeleteFileW(logPath().c_str());
  SYSTEM_INFO si; GetSystemInfo(&si); const int n=(int)(si.dwNumberOfProcessors>0?si.dwNumberOfProcessors:1);
  g_stop=CreateEventW(nullptr,TRUE,FALSE,nullptr);
  cpuReport(1000);                                          // baseline
  std::vector<HANDLE> th; for(int i=0;i<n;i++) th.push_back(CreateThread(nullptr,0,burner,nullptr,0,nullptr));   // one per logical CPU: the app is ~90 % busy
  Sleep(1700);
  cpuReport(1000);
  SetEvent(g_stop); for(HANDLE h:th){ WaitForSingleObject(h,5000); CloseHandle(h); } CloseHandle(g_stop);
  std::ifstream in(wpath(logPath()),std::ios::binary); const std::string log((std::istreambuf_iterator<char>(in)),std::istreambuf_iterator<char>());
  printf("  log: %s",log.c_str());
  CHECK(log.find("[CPU] last ")!=std::string::npos && log.find(" s: app ")!=std::string::npos);
  CHECK(log.find("burner ")!=std::string::npos);            // the busy thread is named
  CHECK(log.find("burner ")<log.find('\n'));                // ... on the one line
  const size_t before=log.size();
  cpuReport(1000); Sleep(1100); cpuReport(1000);            // an idle second: no new line
  std::ifstream in2(wpath(logPath()),std::ios::binary); const std::string log2((std::istreambuf_iterator<char>(in2)),std::istreambuf_iterator<char>());
  CHECK(log2.size()==before);
}

int main(){
  wchar_t tmp[MAX_PATH]; GetTempPathW(MAX_PATH,tmp);
  std::wstring dir=std::wstring(tmp)+L"sw_idle_cpu_test"; CreateDirectoryW(dir.c_str(),nullptr);
  cacheChecks(dir);
  cpuChecks(dir);
  printf("%d checks, %d failed\n",g_checks,g_failed);
  return g_failed?1:0;
}
