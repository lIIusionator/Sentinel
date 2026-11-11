// autostart_tests.cpp -- native unit tests for shared/autostart.h (no Windows needed).
//
//   g++ -std=c++17 -O1 -g -Wall -I shared tests/autostart_tests.cpp -o autostart_tests && ./autostart_tests
#include "autostart.h"
#include <cstdio>

static int g_fail=0, g_checks=0;
#define CHECK(c) do{ g_checks++; if(!(c)){ g_fail++; printf("  FAIL %s:%d  %s\n",__FILE__,__LINE__,#c); } }while(0)

int main(){
  const std::wstring exe=L"C:\\Users\\Nguy\u1ec5n\\Apps\\Sentinel\\sentinel.exe";
  const std::wstring val=autostartValue(exe);
  CHECK(val==L"\"C:\\Users\\Nguy\u1ec5n\\Apps\\Sentinel\\sentinel.exe\" --background");        // quoted: a path with spaces must survive the Run key
  CHECK(autostartValue(L"C:\\a b\\s.exe")==L"\"C:\\a b\\s.exe\" --background");
  // our own entry: same name, same path (case does not matter in the registry or in Windows paths)
  CHECK(isOwnAutostart(L"SentinelWatch",val,exe));
  CHECK(isOwnAutostart(L"sentinelwatch",autostartLower(val),exe));
  CHECK(isOwnAutostart(L"SENTINELWATCH",val,L"c:\\users\\nguy\u1ec5n\\apps\\sentinel\\SENTINEL.EXE"));
  // anything else is still reported
  CHECK(!isOwnAutostart(L"SentinelWatch",L"\"C:\\Temp\\evil.exe\" --background",exe));         // our name, somebody else's program
  CHECK(!isOwnAutostart(L"SentinelWatch",L"\"C:\\Users\\Nguy\u1ec5n\\Apps\\Sentinel\\sentinel.exe\"",exe));   // no flag: not the value we write
  CHECK(!isOwnAutostart(L"SentinelWatch2",val,exe));
  CHECK(!isOwnAutostart(L"Updater",val,exe));
  CHECK(!isOwnAutostart(L"SentinelWatch",val,L"C:\\other\\sentinel.exe"));                     // we run from somewhere else now
  CHECK(!isOwnAutostart(L"SentinelWatch",L"",exe));
  CHECK(!isOwnAutostart(L"SentinelWatch",val,L""));                                             // exe path unknown: never exempt anything
  CHECK(!isOwnAutostart(L"",L"",L""));
  printf("\n%d checks, %d failed\n",g_checks,g_fail);
  return g_fail?2:0;
}
