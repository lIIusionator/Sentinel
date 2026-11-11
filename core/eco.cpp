// eco.cpp -- Windows side of "power saving when away". The rules (when to start, when to stop, how a crash is undone) are shared/eco.h, tested by tests/eco_tests.cpp; this file
// is the power-plan calls (powrprof.dll), the recovery marker file and the once-a-second tick.
//
// The plan the user is on is never edited: it is COPIED (PowerDuplicateScheme), the copy gets the lowered values and becomes the active plan, and going back activates the
// user's plan again and deletes the copy. Everything here runs on the UI thread (the 1 s timer in ui/main.cpp, and its end-of-session window).
#include "sw_common.h"
#include "settings.h"
#include "watchers.h"
#include "eco.h"
#include <objbase.h>
#include <fstream>

std::atomic<bool> g_ecoOn{false};

namespace{
// powrprof.dll is loaded when the first power saving starts (no new static import; a PC without it just cannot save, and says so).
typedef DWORD (WINAPI *PFN_Get)(HKEY,GUID**);
typedef DWORD (WINAPI *PFN_Plan)(HKEY,const GUID*);                          // PowerSetActiveScheme, PowerDeleteScheme
typedef DWORD (WINAPI *PFN_Dup)(HKEY,const GUID*,GUID**);
typedef DWORD (WINAPI *PFN_Name)(HKEY,const GUID*,const GUID*,const GUID*,UCHAR*,DWORD);
typedef DWORD (WINAPI *PFN_Read)(HKEY,const GUID*,const GUID*,const GUID*,LPDWORD);
typedef DWORD (WINAPI *PFN_Write)(HKEY,const GUID*,const GUID*,const GUID*,DWORD);

const GUID SUB_PROCESSOR={0x54533251,0x82be,0x4824,{0x96,0xc1,0x47,0xb6,0x0b,0x74,0x0d,0x00}};
const GUID SET_CPU_MAX  ={0xbc5038f7,0x23e0,0x4960,{0x96,0xda,0x33,0xab,0xaf,0x59,0x35,0xec}};      // Maximum processor state (%)
const GUID SET_CPU_MIN  ={0x893dee8e,0x2bef,0x41e0,{0x89,0xc6,0xb5,0x5d,0x09,0x29,0x96,0x4c}};      // Minimum processor state (%)
const GUID SET_IDLE_DIS ={0x5d76a2ca,0xe8c0,0x402f,{0xa1,0x33,0x21,0x58,0x49,0x2d,0x58,0xad}};      // Processor idle disable (0: the CPU may sleep between tasks)
const GUID SET_BOOST    ={0xbe337238,0x0d82,0x4146,{0xa9,0x60,0x4f,0x37,0x49,0xd4,0x70,0xc7}};      // Processor performance boost mode (0: disabled)
const GUID SUB_VIDEO    ={0x7516b95f,0xf776,0x4464,{0x8c,0x53,0x06,0x16,0x7f,0x40,0xcc,0x99}};
const GUID SET_BRIGHT   ={0xaded5e82,0xb909,0x4619,{0x99,0x49,0xf5,0xd7,0x1d,0xac,0x0b,0xcb}};      // Display brightness (%)

std::string guidStr(const GUID& g){ wchar_t b[64]; if(StringFromGUID2(g,b,64)<3) return std::string();
  std::wstring w=lc(b); return toU8(w.substr(1,w.size()-2)); }                  // "{ABC...}" -> "abc..."
bool guidParse(const std::string& s,GUID& g){ return !s.empty()&&CLSIDFromString((L"{"+fromUtf8(s)+L"}").c_str(),&g)==S_OK; }
std::wstring markerPath(){ return appDir()+L"\\eco_active.txt"; }

struct EcoWin: EcoSys{
  HMODULE m=nullptr; bool tried=false;
  PFN_Get getActive=nullptr; PFN_Plan setActive=nullptr, del=nullptr; PFN_Dup dup=nullptr; PFN_Name name=nullptr; PFN_Read rAC=nullptr, rDC=nullptr; PFN_Write wAC=nullptr, wDC=nullptr;
#ifdef SW_TEST_HOOKS
  bool fake=false; std::string fakeActive="00000000-0000-0000-0000-00000000f00d"; int fakeN=0;    // SW_TEST_ECO: in-memory plans (Wine has no real power plans)
#endif
  void err(const char* what,DWORD r){ logLine(L"[ECO] "+fromUtf8(what)+L" failed, error "+std::to_wstring(r)); }
  bool load(){
    if(!tried){ tried=true;
      wchar_t sys[MAX_PATH]; const UINT n=GetSystemDirectoryW(sys,MAX_PATH);                // by full path: never a powrprof.dll lying next to the exe
      if(n&&n<MAX_PATH) m=LoadLibraryW((std::wstring(sys)+L"\\powrprof.dll").c_str());
      if(m){ getActive=(PFN_Get)(void*)GetProcAddress(m,"PowerGetActiveScheme"); setActive=(PFN_Plan)(void*)GetProcAddress(m,"PowerSetActiveScheme");
        del=(PFN_Plan)(void*)GetProcAddress(m,"PowerDeleteScheme"); dup=(PFN_Dup)(void*)GetProcAddress(m,"PowerDuplicateScheme");
        name=(PFN_Name)(void*)GetProcAddress(m,"PowerWriteFriendlyName");
        rAC=(PFN_Read)(void*)GetProcAddress(m,"PowerReadACValueIndex"); rDC=(PFN_Read)(void*)GetProcAddress(m,"PowerReadDCValueIndex");
        wAC=(PFN_Write)(void*)GetProcAddress(m,"PowerWriteACValueIndex"); wDC=(PFN_Write)(void*)GetProcAddress(m,"PowerWriteDCValueIndex"); }
      if(!(getActive&&setActive&&del&&dup&&name&&rAC&&rDC&&wAC&&wDC)) logLine(L"[ECO] powrprof.dll does not offer the power-plan functions: power saving is not available here"); }
    return getActive&&setActive&&del&&dup&&name&&rAC&&rDC&&wAC&&wDC; }

  // `v` for both the plugged-in (AC) and the battery (DC) value
  bool setBoth(const GUID& eco,const GUID& sub,const GUID& set,DWORD ac,DWORD dc,const char* what){
    const DWORD a=wAC(nullptr,&eco,&sub,&set,ac), d=wDC(nullptr,&eco,&sub,&set,dc);
    if(a||d){ err(what,a?a:d); return false; } return true; }
  // at most `cap`: a value the user already keeps lower stays as it is (a dimmer screen must not get brighter)
  bool lowerTo(const GUID& eco,const GUID& sub,const GUID& set,DWORD cap,const char* what){
    DWORD ac=cap, dc=cap, t=cap; if(rAC(nullptr,&eco,&sub,&set,&t)==0&&t<cap) ac=t;
    t=cap; if(rDC(nullptr,&eco,&sub,&set,&t)==0&&t<cap) dc=t;
    return setBoth(eco,sub,set,ac,dc,what); }

  std::string activePlan() override{
#ifdef SW_TEST_HOOKS
    if(fake) return fakeActive;
#endif
    if(!load()) return std::string();
    GUID* g=nullptr; const DWORD r=getActive(nullptr,&g); if(r||!g){ err("PowerGetActiveScheme",r); return std::string(); }
    const std::string s=guidStr(*g); LocalFree(g); return s; }
  std::string makePlan(const std::string& copyOf) override{
#ifdef SW_TEST_HOOKS
    if(fake) return "00000000-0000-0000-0000-0000000000e"+std::to_string(++fakeN%10);
#endif
    if(!load()) return std::string();
    GUID src; if(!guidParse(copyOf,src)){ logLine(L"[ECO] the active plan id is not a GUID: "+fromUtf8(copyOf)); return std::string(); }
    GUID* ng=nullptr; DWORD r=dup(nullptr,&src,&ng); if(r||!ng){ err("PowerDuplicateScheme",r); return std::string(); }
    const GUID eco=*ng; LocalFree(ng);
    static const wchar_t NAME[]=L"Sentinel Watch power saving (temporary)";
    name(nullptr,&eco,nullptr,nullptr,(UCHAR*)NAME,(DWORD)sizeof(NAME));          // so that a copy somebody finds in the power options says what it is (best effort)
    // the CPU cap is what was asked for: without it the copy is thrown away. The rest is best effort (a desktop has no brightness, a CPU may lack a setting).
    if(!lowerTo(eco,SUB_PROCESSOR,SET_CPU_MAX,ECO_CPU_MAX_PCT,"maximum processor state")){ del(nullptr,&eco); return std::string(); }
    lowerTo(eco,SUB_PROCESSOR,SET_CPU_MIN,5,"minimum processor state");           // the minimum must not sit above the new maximum
    setBoth(eco,SUB_PROCESSOR,SET_IDLE_DIS,0,0,"processor idle disable");        // idle states on: that is where an idle CPU saves its power
    setBoth(eco,SUB_PROCESSOR,SET_BOOST,0,0,"processor boost mode");
    lowerTo(eco,SUB_VIDEO,SET_BRIGHT,ECO_BRIGHT_PCT,"display brightness");
    return guidStr(eco); }
  bool activate(const std::string& plan) override{
#ifdef SW_TEST_HOOKS
    if(fake){ fakeActive=plan; return true; }
#endif
    GUID g; if(!load()||!guidParse(plan,g)) return false;
    const DWORD r=setActive(nullptr,&g); if(r){ err("PowerSetActiveScheme",r); return false; } return true; }
  void remove(const std::string& plan) override{
#ifdef SW_TEST_HOOKS
    if(fake) return;
#endif
    GUID g; if(!load()||!guidParse(plan,g)) return;
    const DWORD r=del(nullptr,&g); if(r) err("PowerDeleteScheme",r); }
  bool saveMarker(const std::string& user,const std::string& eco) override{
    std::ofstream f(wpath(markerPath()),std::ios::trunc|std::ios::binary); if(!f) return false;
    f<<user<<"\n"<<eco<<"\n"; f.flush(); return f.good(); }
  bool loadMarker(std::string& user,std::string& eco) override{
    std::ifstream f(wpath(markerPath())); if(!f) return false;
    std::getline(f,user); std::getline(f,eco);
    for(std::string* s:{&user,&eco}) while(!s->empty()&&(s->back()=='\r'||s->back()==' ')) s->pop_back();
    return true; }
  void clearMarker() override{ DeleteFileW(markerPath().c_str()); }
  void log(const std::string& line) override{ logLine(L"[ECO] "+fromUtf8(line)); }
};

EcoWin g_win;
EcoMode g_mode(g_win);
int64_t g_afterMs=ECO_AFTER_MS;
#ifdef SW_TEST_HOOKS
int64_t g_testAt=0, g_testFor=0;
#endif
}

void ecoRecover(){
#ifdef SW_TEST_HOOKS
  // SW_TEST_ECO=<s>: power saving after <s> seconds without input instead of 30 minutes; SW_TEST_ECO=<at>,<for>: "away" from second <at> for <for> seconds after the start,
  // whatever the input does (so it can overlap an AFK screen: a window that appears counts as input under Wine). Plans are in-memory fakes (Wine has no real ones)
  // unless SW_TEST_ECO_REAL=1, which makes the real power calls.
  { wchar_t b[32]; if(GetEnvironmentVariableW(L"SW_TEST_ECO",b,32)){ const wchar_t* comma=wcschr(b,L','); g_afterMs=(int64_t)_wtoi(b)*1000;
      if(comma){ g_testAt=_wtoi(b); g_testFor=_wtoi(comma+1); }
      g_win.fake=!GetEnvironmentVariableW(L"SW_TEST_ECO_REAL",b,32);
      logLine(L"[TEST] power saving: "+(comma?L"away from "+std::to_wstring(g_testAt)+L" s for "+std::to_wstring(g_testFor)+L" s":L"after "+std::to_wstring(g_afterMs/1000)+L" s without input")
        +L", "+(g_win.fake?L"fake plans":L"real power calls")); } }
#endif
  g_mode.recover();
}

void ecoTick(){
  static ULONGLONG last=0; const ULONGLONG now=GetTickCount64(); if(now-last<1000) return;      // ui/main.cpp calls this every 50 ms while the AFK screen is up
  last=now;
  LASTINPUTINFO li; li.cbSize=sizeof(li);
  bool away=GetLastInputInfo(&li)&&(int64_t)(DWORD)(GetTickCount()-li.dwTime)>=g_afterMs;
#ifdef SW_TEST_HOOKS
  if(g_testFor>0){ const int64_t s=(int64_t)(now-g_start)/1000; away=s>=g_testAt&&s<g_testAt+g_testFor; }
#endif
  g_mode.tick((int64_t)now,away,g_tEco.load());
  g_ecoOn.store(g_mode.on());
}

void ecoStop(){ g_mode.stop(); g_ecoOn.store(g_mode.on()); }
