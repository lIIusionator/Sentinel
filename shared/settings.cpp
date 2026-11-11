// settings.cpp -- JSON settings (hand-rolled: the file is tiny and flat, so no parser dependency).
#include "settings.h"
#include "afk_helpers.h"
#include <ctime>
#include <fstream>
#include <iterator>
#include <cstdlib>

std::atomic<bool> g_tAutorun{true},g_tProc{true},g_tFile{true},
  g_tAllFiles{false},g_tNotify{true},g_tBeep{true},g_tIgnoreSys{true},g_tTokens{true},g_tStrings{true},g_tAutostart{true},g_tEco{true};

Toggle g_toggles[TOG_N]={
  {&g_tAutorun ,"autorun",  L"Autorun watch",                  L"Run keys, Startup folders, Tasks"},
  {&g_tProc    ,"proc",     L"Live process watch",             L"Every new process via the WMI feed"},
  {&g_tFile    ,"file",     L"File watch",                     L"New files in AppData + folders"},
  {&g_tAllFiles,"allfiles", L"Watch all file types",           L"Not just executables (noisier)"},
  {&g_tTokens  ,"tokens",   L"Token Guard",                    L"Warn if unsigned apps read your logins/tokens"},
  {&g_tStrings ,"strings",  L"Strings scan",                   L"Links + commands inside flagged unsigned apps"},
  {&g_tNotify  ,"notify",   L"Notifications",                  L"Pop a toast on suspicious activity"},
  {&g_tBeep    ,"beep",     L"Beep on suspicious",             L"Audible even behind a game"},
  {&g_tIgnoreSys,"ignoresys",L"Ignore signed system processes",L"Silences sppsvc.exe-style noise"},
  {&g_tAutostart,"autostart",L"Start with Windows",            L"Starts hidden in the tray when you sign in"},
  {&g_tEco     ,"eco",      L"Power saving when away",         L"After 30 min idle: CPU max 25 %, screen dimmed"},   // the numbers: shared/eco.h
};
int g_dashW=DASH_W, g_dashH=DASH_H, g_splashMs=3000;
std::atomic<int> g_afkModePref{AFK_AUTO};
std::atomic<int> g_afkIdleSec{0};
std::atomic<int> g_afkGpuPref{AFKGPU_DEFAULT};
std::atomic<int> g_afkMinTier{0};
std::atomic<int> g_afkStrikes{0};
std::atomic<long long> g_afkMinTierAt{0};

std::wstring baseFile(){ return appDir()+L"\\baseline.txt"; }
std::wstring settingsPath(){ return appDir()+L"\\settings.json"; }

static CRITICAL_SECTION g_setCs;
static struct SetLockInit{ SetLockInit(){ InitializeCriticalSection(&g_setCs); } } s_setLockInit;

// What "afkBuild" in settings.json holds: the build id plus the GPU choice. The remembered tier verdicts are only valid for that pair -- a new build, or
// another "afkGpu", gets a fresh try of every tier.
static std::string memoryKey(int gpu){ return std::string(SW_BUILD_ID)+"|"+afkGpuPrefName(gpu); }

void saveSettings(){
  EnterCriticalSection(&g_setCs);      // build AND write under one lock: two writers can never leave a stale snapshot on top of a newer one
  const char* modeName = g_afkModePref.load()==AFK_GPU ? "gpu" : g_afkModePref.load()==AFK_CPU ? "cpu" : "auto";
  std::string j="{\n";
  for(int i=0;i<TOG_N;i++){ j+="  \""; j+=g_toggles[i].key; j+="\": ";
    j+=(g_toggles[i].v->load()?"true":"false"); j+=",\n"; }
  j+="  \"splashMs\": "+std::to_string(g_splashMs)+",\n";
  j+="  \"afkMode\": \""+std::string(modeName)+"\",\n";
  j+="  \"afkIdleSec\": "+std::to_string(g_afkIdleSec.load())+",\n";
  j+="  \"afkGpu\": \""+std::string(afkGpuPrefName(g_afkGpuPref.load()))+"\",\n";
  j+="  \"afkMinTier\": "+std::to_string(g_afkMinTier.load())+",\n";
  j+="  \"afkStrikes\": "+std::to_string(g_afkStrikes.load())+",\n";
  j+="  \"afkMinTierAt\": "+std::to_string(g_afkMinTierAt.load())+",\n";
  j+="  \"afkBuild\": \""+memoryKey(g_afkGpuPref.load())+"\",\n";
  j+="  \"dashW\": "+std::to_string(g_dashW)+",\n";
  j+="  \"dashH\": "+std::to_string(g_dashH)+"\n}\n";
  // Written to a temporary file and moved over the old one, so that being killed in mid-write cannot leave an empty settings.json (which
  // would lose every choice in it).
  const std::wstring path=settingsPath(), tmp=path+L".tmp";
  bool ok=false;
  { std::ofstream f(wpath(tmp),std::ios::trunc|std::ios::binary);
    if(f){ f.write(j.data(),(std::streamsize)j.size()); f.flush(); ok=f.good(); } }
  if(ok) ok=MoveFileExW(tmp.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=FALSE;
  if(!ok){ DeleteFileW(tmp.c_str());                                           // the plain overwrite as a fallback (a locked target, an odd file system)
    std::ofstream f(wpath(path),std::ios::trunc|std::ios::binary); f.write(j.data(),(std::streamsize)j.size()); }
  LeaveCriticalSection(&g_setCs);
}

void loadSettings(){
  std::ifstream f(wpath(settingsPath())); if(!f)return;
  std::string s((std::istreambuf_iterator<char>(f)),std::istreambuf_iterator<char>());
  for(int i=0;i<TOG_N;i++){ std::string key="\""+std::string(g_toggles[i].key)+"\"";
    size_t k=s.find(key); if(k==std::string::npos)continue;
    size_t c=s.find(':',k); if(c==std::string::npos)continue;
    size_t q=c+1; while(q<s.size()&&(s[q]==' '||s[q]=='\t'))q++;
    g_toggles[i].v->store(s.compare(q,4,"true")==0); }
  size_t k=s.find("\"splashMs\"");
  if(k!=std::string::npos){ size_t c=s.find(':',k);
    if(c!=std::string::npos){ int v=atoi(s.c_str()+c+1); if(v>=0&&v<=10000)g_splashMs=v; } }
  auto readInt=[&](const char* key,int lo,int hi,int& out){
    size_t kk=s.find(std::string("\"")+key+"\""); if(kk==std::string::npos)return;
    size_t cc=s.find(':',kk); if(cc==std::string::npos)return;
    int v=atoi(s.c_str()+cc+1); if(v>=lo&&v<=hi)out=v; };
  auto readStr=[&](const char* key,std::string& out){
    size_t kk=s.find(std::string("\"")+key+"\""); if(kk==std::string::npos)return;
    size_t cc=s.find(':',kk); if(cc==std::string::npos)return;
    size_t q1=s.find('"',cc); if(q1==std::string::npos)return;
    size_t q2=s.find('"',q1+1); if(q2==std::string::npos)return;
    out=s.substr(q1+1,q2-q1-1); };
  readInt("dashW",DASH_MINW,10000,g_dashW);
  readInt("dashH",DASH_MINH,10000,g_dashH);
  std::string mode; readStr("afkMode",mode);
  if(mode=="gpu") g_afkModePref.store(AFK_GPU); else if(mode=="cpu") g_afkModePref.store(AFK_CPU); else g_afkModePref.store(AFK_AUTO);
  { int idle=0; readInt("afkIdleSec",0,3600,idle); if(idle>0&&idle<5) idle=5; g_afkIdleSec.store(idle); }
  { std::string gpu; readStr("afkGpu",gpu); g_afkGpuPref.store(afkGpuPrefFromName(gpu)); }
  int tier=0; readInt("afkMinTier",0,2,tier);
  int strikes=0; readInt("afkStrikes",0,9,strikes);
  long long at=0; { size_t kk=s.find("\"afkMinTierAt\""); if(kk!=std::string::npos){ size_t cc=s.find(':',kk); if(cc!=std::string::npos) at=atoll(s.c_str()+cc+1); } }
  std::string build; readStr("afkBuild",build);
  AfkTierMemory mem; mem.minTier=tier; mem.strikes=strikes; mem.minTierAtSec=at;
  if(build!=memoryKey(g_afkGpuPref.load())) mem=AfkTierMemory();   // a new build, or another GPU choice, re-tries every tier
  else afkMemExpire(mem,(int64_t)time(nullptr));              // an old verdict is not trusted either
  g_afkMinTier.store(mem.minTier); g_afkStrikes.store(mem.strikes); g_afkMinTierAt.store((long long)mem.minTierAtSec);
}
