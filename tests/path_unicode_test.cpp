// path_unicode_test.cpp -- the app's data folder, log and settings must work when the Windows account name is not plain ASCII.
//
// A Vietnamese / Japanese / accented user name puts non-ASCII characters (and, for some emoji names, surrogate pairs) in %LOCALAPPDATA%. Anything
// that squeezes such a path through the ANSI code page (a narrow ofstream constructor, a char* path) silently writes to the wrong place or fails.
// This program points LOCALAPPDATA at such a folder from inside the process (Wine ignores a LOCALAPPDATA given from outside, and so would nobody
// but a test), runs the real shared/ code against it and then checks, with plain Win32 calls, that the files really exist under the right names.
//
//   make pathtest      builds path_unicode_test.exe (cross: make CROSS=x86_64-w64-mingw32- pathtest) -- run it on Windows or under Wine.
//   Exit code 0 = every check passed.
#include "sw_common.h"
#include "settings.h"
#include "afk_helpers.h"
#include <shlobj.h>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <iterator>

static int g_checks=0, g_failed=0;
#define CHECK(c) do{ g_checks++; if(!(c)){ g_failed++; printf("  FAIL  line %d: %s\n",__LINE__,#c); } }while(0)

static bool exists(const std::wstring& p){ return GetFileAttributesW(p.c_str())!=INVALID_FILE_ATTRIBUTES; }
static bool isDir(const std::wstring& p){ DWORD a=GetFileAttributesW(p.c_str()); return a!=INVALID_FILE_ATTRIBUTES&&(a&FILE_ATTRIBUTE_DIRECTORY); }

// The exact name the file system holds for `leaf` inside `dir` (so a mangled "T?st" would show up as a different string).
static std::wstring listedName(const std::wstring& dir,const std::wstring& leaf){
  WIN32_FIND_DATAW fd; HANDLE h=FindFirstFileW((dir+L"\\*").c_str(),&fd); std::wstring found;
  if(h==INVALID_HANDLE_VALUE) return found;
  do{ if(leaf==fd.cFileName){ found=fd.cFileName; break; } }while(FindNextFileW(h,&fd));
  FindClose(h); return found; }

static std::string slurp(const std::wstring& p){ std::ifstream f(wpath(p),std::ios::binary); return std::string((std::istreambuf_iterator<char>(f)),std::istreambuf_iterator<char>()); }

static void runCase(const wchar_t* title,const std::wstring& localAppData){
  printf("%ls\n",title);
  // wide-string basics first: the narrow round trip the app uses for log text must be lossless
  const std::wstring probe=L"T\u00e9st \u00f1 \u65e5\u672c \u0110\u1ed7 Th\u1ecb \U0001F600";
  CHECK(fromUtf8(toU8(probe))==probe);

  SetEnvironmentVariableW(L"LOCALAPPDATA",localAppData.c_str());
  const std::wstring dir=appDir();
  CHECK(dir==localAppData+L"\\SentinelWatch");
  CHECK(isDir(dir));
  CHECK(listedName(localAppData,L"SentinelWatch")==L"SentinelWatch");

  // --- the log ---
  logLine(L"[AFK] unicode test: "+probe);
  logLine(L"second line");
  const std::wstring log=logPath();
  CHECK(exists(log));
  const std::string logText=slurp(log);
  CHECK(logText.find(toU8(probe))!=std::string::npos);                 // the UTF-8 bytes of the text really made it in
  CHECK(logText.find("second line")!=std::string::npos);

  // --- settings: write, then read back through the same functions ---
  g_afkModePref.store(AFK_CPU); g_afkIdleSec.store(45); g_afkGpuPref.store(AFKGPU_DEFAULT);
  g_afkMinTier.store(1); g_afkStrikes.store(1); g_afkMinTierAt.store((long long)time(nullptr));
  g_dashW=1000; g_dashH=640; g_splashMs=1234;
  saveSettings();
  CHECK(exists(settingsPath()));
  CHECK(!exists(settingsPath()+L".tmp"));                              // the temporary file was moved over, not left behind
  const std::string js=slurp(settingsPath());
  CHECK(js.find("\"afkMode\": \"cpu\"")!=std::string::npos);
  CHECK(js.find("\"afkIdleSec\": 45")!=std::string::npos);
  g_afkModePref.store(AFK_AUTO); g_afkIdleSec.store(0); g_afkMinTier.store(0); g_afkStrikes.store(0); g_dashW=0; g_dashH=0; g_splashMs=0;
  loadSettings();
  CHECK(g_afkModePref.load()==AFK_CPU); CHECK(g_afkIdleSec.load()==45);
  CHECK(g_afkMinTier.load()==1); CHECK(g_afkStrikes.load()==1);
  CHECK(g_dashW==1000); CHECK(g_dashH==640); CHECK(g_splashMs==1234);

  // --- a user's own clip with a non-ASCII name next to the built-in ones (FindFirstFileW + wide paths all the way) ---
  const std::wstring afkDir=dir+L"\\content\\afk"; SHCreateDirectoryExW(0,afkDir.c_str(),0);
  const std::wstring clipName=L"n\u1ec1n \u2014 \u65e5\u672c \U0001F600.mp4";
  { std::ofstream f(wpath(afkDir+L"\\"+clipName),std::ios::binary); f<<"x"; }
  CHECK(listedName(afkDir,clipName)==clipName);

  // --- log rotation moves the file by its wide name ---
  { std::ofstream f(wpath(log),std::ios::app|std::ios::binary); std::string pad(4*1024*1024+64,'x'); f.write(pad.data(),(std::streamsize)pad.size()); }
  logRotate();
  CHECK(exists(dir+L"\\events.old.log"));
  logLine(L"after rotation");
  CHECK(slurp(log).find("after rotation")!=std::string::npos);
}

int main(){
  // the base folder is created by the test (under the temp folder of whoever runs it) and removed again at the end
  wchar_t tmp[MAX_PATH]; DWORD n=GetTempPathW(MAX_PATH,tmp); std::wstring base=(n&&n<MAX_PATH)?std::wstring(tmp,n):L"C:\\";
  if(!base.empty()&&base.back()==L'\\') base.pop_back();
  base+=L"\\sw_unicode_test_"+std::to_wstring(GetCurrentProcessId());

  const std::wstring cases[][2]={
    { L"accents + CJK",                         L"T\u00e9st \u00f1 \u65e5\u672c" },
    { L"Vietnamese (what a user here may have)", L"Nguy\u1ec5n V\u0103n \u0110\u1ee9c" },
    { L"outside the BMP (a surrogate pair)",    L"Emoji \U0001F600 user" },
    { L"plain ASCII with spaces (control)",     L"Plain User Name" },
  };
  for(auto& c:cases) runCase(c[0].c_str(),base+L"\\"+c[1]);

  printf("\n%d checks, %d failed\n",g_checks,g_failed);
  // tidy up: best effort (SHFileOperation needs a double-NUL-terminated list)
  std::wstring list=base; list.push_back(L'\0'); list.push_back(L'\0');
  SHFILEOPSTRUCTW op={}; op.wFunc=FO_DELETE; op.pFrom=list.c_str(); op.fFlags=FOF_NO_UI|FOF_NOCONFIRMATION; SHFileOperationW(&op);
  return g_failed?1:0;
}
