// core.cpp -- see core.h. Plain C++17 + POSIX; the OS is reached only through sw::g_sys.
#include "core.h"
#include "platform.h"
#include "version.h"
#include "scanfile.h"
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <random>
#include <thread>
#include <unordered_map>
#include <fcntl.h>
#include <pwd.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#if SW_OS_MAC
#include <pthread.h>
#include <sys/sysctl.h>
#endif
namespace fs=std::filesystem;
using strscan::lower;

namespace sw{

// ================= clock, small helpers =================
static int64_t steadyMs(){ return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
int64_t (*g_nowMs)()=steadyMs;
std::string g_selfExe, g_fsRoot;
Sys* g_sys=nullptr;
static std::atomic<bool> g_quit{false};
std::atomic<unsigned> g_rev{1};
static void bump(){ g_rev.fetch_add(1,std::memory_order_relaxed); }

static bool endsWithI(const std::string& s,const char* suf){ const size_t n=strlen(suf); return s.size()>=n&&lower(s.substr(s.size()-n))==suf; }   // suf in lower case
static bool hasI(const std::string& l,const char* m){ return l.find(m)!=std::string::npos; }                                                       // l already lower case
static std::string baseName(const std::string& p){ const size_t k=p.find_last_of('/'); return k==std::string::npos?p:p.substr(k+1); }
static std::string hhmmss(){ time_t t=time(nullptr); struct tm lt; localtime_r(&t,&lt); char b[16]; snprintf(b,sizeof b,"%02d:%02d:%02d",lt.tm_hour,lt.tm_min,lt.tm_sec); return b; }
// Background threads ask for the efficiency cores: the watchers must cost next to nothing ("idle means idle").
static void qosBackground(){
#if SW_OS_MAC
  pthread_set_qos_class_self_np(QOS_CLASS_BACKGROUND,0);
#endif
}
static bool statReg(const std::string& p,uint64_t& size,int64_t& mt){ struct stat st; if(stat(p.c_str(),&st)!=0||!S_ISREG(st.st_mode)) return false; size=(uint64_t)st.st_size; mt=(int64_t)st.st_mtime; return true; }
static std::string readWhole(const std::string& p){ std::ifstream f(p,std::ios::binary); return f?std::string((std::istreambuf_iterator<char>(f)),std::istreambuf_iterator<char>()):std::string(); }
// A temporary file moved over the old one: being killed in mid-write cannot leave an empty settings.json (that would lose every choice in it).
static bool writeFileAtomic(const std::string& path,const std::string& data){
  const std::string tmp=path+".tmp"; bool ok=false;
  { std::ofstream f(tmp,std::ios::binary|std::ios::trunc); if(f){ f.write(data.data(),(std::streamsize)data.size()); f.flush(); ok=f.good(); } }
  if(ok) ok=std::rename(tmp.c_str(),path.c_str())==0;
  if(!ok){ std::remove(tmp.c_str()); std::ofstream f(path,std::ios::binary|std::ios::trunc); if(f){ f.write(data.data(),(std::streamsize)data.size()); ok=f.good(); } }
  return ok; }

std::string utf8Clean(const std::string& s){
  std::string o; o.reserve(s.size());
  for(size_t i=0;i<s.size();){
    const unsigned char c=(unsigned char)s[i]; size_t n=0; uint32_t cp=0;
    if(c<0x80){ o+=(char)c; i++; continue; }
    if(c>=0xC2&&c<=0xDF){ n=2; cp=c&0x1Fu; } else if(c>=0xE0&&c<=0xEF){ n=3; cp=c&0x0Fu; } else if(c>=0xF0&&c<=0xF4){ n=4; cp=c&0x07u; }
    bool ok=n&&i+n<=s.size();
    for(size_t k=1;ok&&k<n;k++){ const unsigned char d=(unsigned char)s[i+k]; if((d&0xC0)!=0x80) ok=false; else cp=(cp<<6)|(d&0x3Fu); }
    if(ok&&((n==3&&(cp<0x800||(cp>=0xD800&&cp<=0xDFFF)))||(n==4&&(cp<0x10000||cp>0x10FFFF)))) ok=false;      // overlong, surrogate, beyond Unicode
    if(ok){ o.append(s,i,n); i+=n; } else{ o+="\xEF\xBF\xBD"; i++; } }
  return o; }

std::string jsonStr(const std::string& raw){
  const std::string s=utf8Clean(raw); std::string o="\""; o.reserve(s.size()+2);
  for(size_t i=0;i<s.size();i++){ const unsigned char c=(unsigned char)s[i];
    if(c=='"') o+="\\\""; else if(c=='\\') o+="\\\\"; else if(c=='\n') o+="\\n"; else if(c=='\r') o+="\\r"; else if(c=='\t') o+="\\t";
    else if(c<0x20){ char b[8]; snprintf(b,sizeof b,"\\u%04x",c); o+=b; }
    else if(c==0xE2&&i+2<s.size()&&(unsigned char)s[i+1]==0x80&&((unsigned char)s[i+2]==0xA8||(unsigned char)s[i+2]==0xA9)){ o+=((unsigned char)s[i+2]==0xA8)?"\\u2028":"\\u2029"; i+=2; }   // line / paragraph separators end a JavaScript string
    else o+=(char)c; }
  return o+"\""; }

// ================= paths, log =================
static std::string g_data; static std::mutex g_dataMx, g_logMx;
std::string homeDir(){ const char* h=getenv("HOME"); if(h&&*h) return h; struct passwd* pw=getpwuid(getuid()); return (pw&&pw->pw_dir)?pw->pw_dir:"/tmp"; }
void setDataDir(const std::string& d){ { std::lock_guard<std::mutex> g(g_dataMx); g_data=d; } std::error_code ec; fs::create_directories(d,ec); }
std::string dataDir(){
  { std::lock_guard<std::mutex> g(g_dataMx); if(!g_data.empty()) return g_data; }
  const char* e=getenv("SW_DATA_DIR"); setDataDir(e&&*e?std::string(e):homeDir()+"/Library/Application Support/SentinelWatch");
  std::lock_guard<std::mutex> g(g_dataMx); return g_data; }
void logLine(const std::string& s){
  const std::string line="["+hhmmss()+"] "+utf8Clean(s)+"\n", p=dataDir()+"/events.log";
  std::lock_guard<std::mutex> g(g_logMx); std::ofstream f(p,std::ios::app|std::ios::binary); f.write(line.data(),(std::streamsize)line.size()); }
void logRotate(){
  const std::string p=dataDir()+"/events.log"; struct stat st; if(stat(p.c_str(),&st)!=0||st.st_size<=4*1024*1024) return;
  std::lock_guard<std::mutex> g(g_logMx); std::rename(p.c_str(),(dataDir()+"/events.old.log").c_str()); }

// ================= settings =================
std::atomic<bool> g_tAutorun{true},g_tProc{true},g_tFile{true},g_tAllFiles{false},g_tNotify{true},g_tBeep{true},g_tIgnoreSys{true},g_tTokens{true},g_tStrings{true},g_tAutostart{true};
Toggle g_toggles[TOG_N]={
  {&g_tAutorun ,"autorun",  "Autorun watch",                  "Login items: LaunchAgents and LaunchDaemons"},
  {&g_tProc    ,"proc",     "Live process watch",             "Every new process (looked for every 2 s)"},
  {&g_tFile    ,"file",     "File watch",                     "New files in Library, Downloads and temp folders"},
  {&g_tAllFiles,"allfiles", "Watch all file types",           "Not just programs (noisier)"},
  {&g_tTokens  ,"tokens",   "Token Guard",                    "Warn if unsigned apps read your logins/tokens"},
  {&g_tStrings ,"strings",  "Strings scan",                   "Links + commands inside flagged unsigned apps"},
  {&g_tNotify  ,"notify",   "Notifications",                  "Pop a toast on suspicious activity"},
  {&g_tBeep    ,"beep",     "Beep on suspicious",             "Audible even behind a full-screen app"},
  {&g_tIgnoreSys,"ignoresys","Ignore Apple system processes", "Silences launchd-style noise"},
  {&g_tAutostart,"autostart","Start at login",                "Starts hidden in the menu bar when you log in"},
};
std::atomic<int> g_afkIdleSec{0};
int g_dashW=DASH_W, g_dashH=DASH_H;
static std::mutex g_setMx;
static std::string settingsPath(){ return dataDir()+"/settings.json"; }

void saveSettings(){
  std::lock_guard<std::mutex> g(g_setMx);        // build AND write under one lock: two writers can never leave an older snapshot on top of a newer one
  std::string j="{\n";
  for(int i=0;i<TOG_N;i++){ j+="  \""; j+=g_toggles[i].key; j+="\": "; j+=g_toggles[i].v->load()?"true":"false"; j+=",\n"; }
  j+="  \"afkIdleSec\": "+std::to_string(g_afkIdleSec.load())+",\n";
  j+="  \"dashW\": "+std::to_string(g_dashW)+",\n";
  j+="  \"dashH\": "+std::to_string(g_dashH)+"\n}\n";
  writeFileAtomic(settingsPath(),j);
}
void loadSettings(){
  const std::string s=readWhole(settingsPath()); if(s.empty()) return;
  for(int i=0;i<TOG_N;i++){ const size_t k=s.find("\""+std::string(g_toggles[i].key)+"\""); if(k==std::string::npos) continue;
    size_t c=s.find(':',k); if(c==std::string::npos) continue; c++; while(c<s.size()&&(s[c]==' '||s[c]=='\t')) c++;
    g_toggles[i].v->store(s.compare(c,4,"true")==0); }
  auto readInt=[&](const char* key,int lo,int hi,int& out){ const size_t k=s.find(std::string("\"")+key+"\""); if(k==std::string::npos) return;
    const size_t c=s.find(':',k); if(c==std::string::npos) return; const int v=atoi(s.c_str()+c+1); if(v>=lo&&v<=hi) out=v; };
  readInt("dashW",DASH_MINW,10000,g_dashW); readInt("dashH",DASH_MINH,10000,g_dashH);
  int idle=0; readInt("afkIdleSec",0,3600,idle); if(idle>0&&idle<5) idle=5; g_afkIdleSec.store(idle);
}

// ================= events =================
static std::deque<Ev> g_events; static std::mutex g_evMx; static int64_t g_secWin=0; static int g_secCnt=0;
std::atomic<int> g_procCount{0},g_fileCount{0},g_tokenAlerts{0};
std::function<void(const std::string&,const std::string&,bool,bool)> g_toastHook;

void pushEv(const std::string& tag,const std::string& name,const std::string& path){
  bool drop=false; const std::string tm=hhmmss(); const int64_t now=g_nowMs();
  { std::lock_guard<std::mutex> g(g_evMx);
    if(now-g_secWin>=1000){ g_secWin=now; g_secCnt=0; }
    if(g_secCnt>=30) drop=true;                 // storm guard: 30 events a second at most
    else{ g_secCnt++; g_events.push_front({tag,name,path,tm}); if(g_events.size()>80) g_events.pop_back(); } }
  if(!drop){ bump(); logLine(tag+" "+name+" | "+path); }
}
void pushEvRaw(const std::string& tag,const std::string& name,const std::string& path){
  { std::lock_guard<std::mutex> g(g_evMx); g_events.push_front({tag,name,path,hhmmss()}); if(g_events.size()>80) g_events.pop_back(); }
  bump(); }
std::vector<Ev> eventsCopy(){ std::lock_guard<std::mutex> g(g_evMx); return std::vector<Ev>(g_events.begin(),g_events.end()); }
void showOverlay(const std::string& title,const std::string& text,bool red){ if(g_tNotify.load()&&g_toastHook) g_toastHook(title,text,red,g_tBeep.load()); }

// ================= trust rules =================
bool isExecExt(const std::string& p){
  static const char* const ex[]={".app",".command",".sh",".zsh",".bash",".py",".pl",".rb",".scpt",".applescript",".workflow",".dylib",".jar",".terminal"};
  for(const char* e:ex) if(endsWithI(p,e)) return true;
  if(endsWithI(p,".plist")){ const std::string l=lower(p); return hasI(l,"/library/launchagents/")||hasI(l,"/library/launchdaemons/"); }   // a new launchd job runs something at login
  return false; }
bool userWritable(const std::string& p){
  if(p.empty()) return false; const std::string l=lower(p);
  for(const char* m:{"/library/application support/","/library/caches/","/library/launchagents/","/downloads/","/private/tmp/","/private/var/tmp/","/private/var/folders/","/users/shared/","/tmp/","/var/tmp/","/var/folders/"})
    if(hasI(l,m)) return true;
  return false; }
bool isSystemProc(const std::string& p){ const std::string l=lower(p); for(const char* m:{"/system/","/usr/","/bin/","/sbin/","/library/apple/"}) if(l.rfind(m,0)==0) return true; return false; }
std::string signTarget(const std::string& p){ const size_t k=lower(p).find(".app/"); return k==std::string::npos?p:p.substr(0,k+4); }
std::string mainExeOf(const std::string& bundle){
  std::error_code ec; const fs::path d=fs::path(bundle)/"Contents"/"MacOS"; std::vector<std::string> names;
  for(fs::directory_iterator it(d,ec),end; !ec&&it!=end; it.increment(ec)){ std::error_code e2; if(it->is_regular_file(e2)) names.push_back(it->path().filename().string()); }
  if(names.empty()) return ""; std::sort(names.begin(),names.end());
  std::string stem=baseName(bundle); if(endsWithI(stem,".app")) stem.resize(stem.size()-4);
  for(const std::string& n:names) if(lower(n)==lower(stem)) return (d/n).string();
  return (d/names[0]).string(); }
bool looksLikeProgram(const std::string& path){ std::ifstream f(path,std::ios::binary); if(!f) return false; uint8_t h[8]={0}; f.read((char*)h,8); return strscan::isMacProgram(h,(size_t)f.gcount()); }

// codesign takes a while on a big app (it reads every file in the bundle), and Token Guard asks about every program in a user folder every 4 s: the answer is
// kept until the file changes. A bundle is judged by its Info.plist stamp.
// shortcut: a file rewritten to the same size within the same second keeps its old verdict, and a bundle's resources are not part of the stamp; upgrade: key the cache on a hash.
struct SigRes{ uint64_t size; int64_t mt; bool ok; };
static std::mutex g_sigMx; static std::unordered_map<std::string,SigRes> g_sig; static std::set<std::string> g_sigWarned;
std::atomic<bool> g_signUsable{true};
static bool stampOf(const std::string& target,uint64_t& size,int64_t& mt){
  struct stat st; if(stat(target.c_str(),&st)!=0) return false;
  if(S_ISDIR(st.st_mode)){ const std::string pl=target+"/Contents/Info.plist"; if(stat(pl.c_str(),&st)!=0) return false; }
  size=(uint64_t)st.st_size; mt=(int64_t)st.st_mtime; return true; }
bool isSigned(const std::string& path){
  if(path.empty()) return false;
  if(!g_signUsable.load()||!g_sys) return true;                // codesign does not work here (signSelfTest said so): nothing is called unsigned
  const std::string target=signTarget(path), key=lower(target); uint64_t sz=0; int64_t mt=0; const bool have=stampOf(target,sz,mt);
  if(have){ std::lock_guard<std::mutex> g(g_sigMx); auto it=g_sig.find(key); if(it!=g_sig.end()&&it->second.size==sz&&it->second.mt==mt) return it->second.ok; }
  const int r=g_sys->codesign(target);
  if(r<0){ bool first; { std::lock_guard<std::mutex> g(g_sigMx); first=g_sigWarned.size()<20&&g_sigWarned.insert(key).second; }
    if(first) logLine("[SIGN] could not check "+target+" (taken as signed)"); return true; }
  const bool ok=(r==0);
  if(have){ std::lock_guard<std::mutex> g(g_sigMx); if(g_sig.size()>=4096) g_sig.clear(); g_sig[key]={sz,mt,ok}; }
  return ok; }
bool suspiciousExe(const std::string& path){ if(path.empty()||!userWritable(path)) return false; return !isSigned(path); }
bool isCredArtifactName(const std::string& p){
  const std::string l=lower(p);
  for(const char* m:{"cookies","login data","web data","key4.db","key3.db","logins.json","places.sqlite",".ldb","leveldb","local state","login.keychain"}) if(hasI(l,m)) return true;
  return false; }
bool isStagingDir(const std::string& p){ const std::string l=lower(p); for(const char* m:{"/tmp/","/var/tmp/","/downloads/","/desktop/"}) if(hasI(l,m)) return true; return false; }

void signSelfTest(){
  if(!g_sys) return;
  const std::string probe=dataDir()+"/signcheck.txt", copy=dataDir()+"/signcheck.bin"; writeFileAtomic(probe,"not code\n");
  const int apple=g_sys->codesign("/bin/ls"), plain=g_sys->codesign(probe);
  // A program re-signed ad hoc is validly signed, but not by Apple. On Apple Silicon every program is at least that, so this is the control that matters: it proves
  // the "Apple-anchored" requirement is really being tested (-2: the control could not be made; the other two still have to pass).
  int adhoc=-2; if(g_sys->adhocSignedCopy("/bin/ls",copy)) adhoc=g_sys->codesign(copy);
  std::remove(probe.c_str()); std::remove(copy.c_str());
  const bool ok=(apple==0&&plain==1&&(adhoc==1||adhoc==-2)); g_signUsable.store(ok);
  if(ok) logLine(std::string("[SIGN] the signature check works (an Apple program: signed, a plain file: not, an ad-hoc signed copy: ")+(adhoc==1?"not)":"could not be made)"));
  else{ logLine("[SIGN] the signature check does not work here (/bin/ls gave "+std::to_string(apple)+", a plain file gave "+std::to_string(plain)+", an ad-hoc signed copy gave "+std::to_string(adhoc)+"): unsigned-program warnings are off");
    pushEv("SYS","signature check unavailable","unsigned-program warnings are off"); }
}

// ================= processes =================
// shortcut: a process is new when its pid was not there at the last look (pids wrap only after 99,999 starts), and one that starts and ends within 2 s is missed;
// upgrade: Endpoint Security (needs an Apple entitlement).
static std::mutex g_pcMx; static std::unordered_map<int,ProcInfo> g_pc;           // processes seen: pid -> what they are
static bool procInfoOf(int pid,ProcInfo& pi){ if(!g_sys->procInfo(pid,pi)) return false; pi.pid=pid; if(pi.name.empty()) pi.name=pi.path.empty()?"(pid "+std::to_string(pid)+")":baseName(pi.path); return true; }
static bool suspiciousProc(const ProcInfo& pi){ return pi.pid!=(int)getpid()&&suspiciousExe(pi.path); }

void procScan(){
  if(!g_sys) return;
  const std::vector<int> ids=g_sys->pids(); std::unordered_map<int,ProcInfo> now; std::set<std::string> seen;
  for(int pid:ids){ ProcInfo pi; if(!procInfoOf(pid,pi)) continue; now[pid]=pi;
    if(!seen.insert(lower(pi.name)).second) continue;
    const bool susp=suspiciousProc(pi);
    pushEvRaw(susp?"WARN":"PROC",pi.name,pi.path.empty()?"(running)":pi.path);
    if(susp) stringsScanAsync(pi.path); }
  g_procCount.store((int)ids.size()); { std::lock_guard<std::mutex> g(g_pcMx); g_pc.swap(now); } bump(); }
void procTick(){
  if(!g_sys) return;
  const std::vector<int> ids=g_sys->pids(); const std::set<int> alive(ids.begin(),ids.end()); std::vector<ProcInfo> fresh;
  { std::lock_guard<std::mutex> g(g_pcMx); for(auto it=g_pc.begin();it!=g_pc.end();){ if(!alive.count(it->first)) it=g_pc.erase(it); else ++it; } }
  for(int pid:ids){ { std::lock_guard<std::mutex> g(g_pcMx); if(g_pc.count(pid)) continue; }
    ProcInfo pi; if(!procInfoOf(pid,pi)) continue; { std::lock_guard<std::mutex> g(g_pcMx); g_pc[pid]=pi; } fresh.push_back(pi); }
  if(!g_tProc.load()) return;
  for(const ProcInfo& pi:fresh){
    if(isSystemProc(pi.path)&&g_tIgnoreSys.load()) continue;
    g_procCount++;
    const bool susp=suspiciousProc(pi); const std::string shown=pi.path.empty()?"(no path reported)":pi.path;
    pushEv(susp?"WARN":"PROC",pi.name,shown);
    if(susp){ showOverlay("\xE2\x9A\xA0 Unsigned app in user folder",pi.name+"\n"+shown,true); stringsScanAsync(pi.path); } }
}

// ================= login items =================
std::set<std::string> g_known; static std::mutex g_scanMx; bool g_syncScan=false;
static const char* const AGENT_LABEL="com.sentinelwatch.agent";
bool isOwnAutostart(const std::string& plistName,const std::string& program,const std::string& exe){
  return !exe.empty()&&lower(plistName)==std::string(AGENT_LABEL)+".plist"&&lower(program)==lower(exe); }
std::set<std::string> scanAutoruns(){
  std::set<std::string> out; if(!g_sys) return out; const std::string exe=g_selfExe;
  const struct{ std::string dir; const char* label; } ds[]={{homeDir()+"/Library/LaunchAgents","LaunchAgents"},{g_fsRoot+"/Library/LaunchAgents","LaunchAgents*"},{g_fsRoot+"/Library/LaunchDaemons","LaunchDaemons"}};
  for(const auto& d:ds){ std::error_code ec;
    for(fs::directory_iterator it(d.dir,ec),end; !ec&&it!=end; it.increment(ec)){
      const std::string f=it->path().filename().string(); if(!endsWithI(f,".plist")) continue;
      std::error_code e2; if(!it->is_regular_file(e2)&&!it->is_symlink(e2)) continue;
      const std::string prog=g_sys->plistProgram(it->path().string());
      if(isOwnAutostart(f,prog,exe)) continue;                                  // "Start at login" is ours, not a new login item
      out.insert("PLIST "+std::string(d.label)+" | "+f+(prog.empty()?"":" = "+prog)); } }
  return out; }
static std::string baselinePath(){ return dataDir()+"/baseline.txt"; }
static void writeBaseline(){ std::string t; for(const std::string& e:g_known){ t+=e; t+='\n'; } writeFileAtomic(baselinePath(),t); }
static void loadBaseline(){                       // the baseline is written at the first start too, so a login item added while the app was off is reported
  std::lock_guard<std::mutex> g(g_scanMx); std::ifstream f(baselinePath()); const bool have=(bool)f; std::string ln;
  while(std::getline(f,ln)){ if(!ln.empty()&&ln.back()=='\r') ln.pop_back(); if(!ln.empty()) g_known.insert(ln); }
  if(!have){ g_known=scanAutoruns(); writeBaseline(); } }
void doScan(bool announce){
  if(g_syncScan) procScan(); else{ try{ std::thread([]{ qosBackground(); procScan(); }).detach(); }catch(...){} }
  if(!g_tAutorun.load()){ pushEv("SYS","scan skipped","autorun watch off"); return; }
  std::lock_guard<std::mutex> g(g_scanMx);
  const std::set<std::string> cur=scanAutoruns(); int added=0;
  for(const std::string& e:cur) if(!g_known.count(e)){ pushEv("WARN","new autorun",e); added++; }
  if(added&&announce) showOverlay("\xE2\x9A\xA0 New login item",std::to_string(added)+" new login item(s)",true);
  if(added){ g_known=cur; writeBaseline(); }
  else if(announce) pushEv("SYS","manual scan","login items re-swept \xE2\x80\x94 no changes");
}

// ================= token guard =================
static std::vector<std::string> credDirs(){
  const std::string a=lower(homeDir())+"/library/application support/", out[]={
    a+"discord/local storage/leveldb",a+"discordptb/local storage/leveldb",a+"discordcanary/local storage/leveldb",
    a+"google/chrome",a+"microsoft edge",a+"bravesoftware/brave-browser",a+"vivaldi",a+"yandex/yandexbrowser",
    a+"com.operasoftware.opera",a+"com.operasoftware.operagx",a+"firefox/profiles",
    lower(homeDir())+"/library/keychains/",lower(homeDir())+"/library/cookies/"};
  return std::vector<std::string>(std::begin(out),std::end(out)); }
static std::string credLabel(const std::string& full){
  const size_t k=full.rfind("/library/"); const std::string d=k==std::string::npos?full:full.substr(k);
  const std::pair<const char*,const char*> t[]={{"discord","Discord"},{"chrome","Chrome"},{"edge","Edge"},{"brave","Brave"},{"vivaldi","Vivaldi"},{"yandex","Yandex"},{"opera","Opera"},{"firefox","Firefox"},{"keychains","Keychain"},{"cookies","Safari"}};
  for(const auto& x:t) if(hasI(d,x.first)) return x.second;
  return "credential store"; }
static std::set<int> g_alerted;
void tokenGuardTick(){
  if(!g_tTokens.load()||!g_sys) return;
  std::vector<ProcInfo> all; { std::lock_guard<std::mutex> g(g_pcMx); for(const auto& kv:g_pc) all.push_back(kv.second); }
  std::vector<ProcInfo> susp; std::set<int> alive;
  for(const ProcInfo& p:all){ alive.insert(p.pid); if(!p.path.empty()&&suspiciousProc(p)) susp.push_back(p); }     // unsigned + user-writable: normally none
  for(auto it=g_alerted.begin();it!=g_alerted.end();){ if(!alive.count(*it)) it=g_alerted.erase(it); else ++it; }
  if(susp.empty()) return;
  const std::vector<std::string> dirs=credDirs();
  for(const ProcInfo& sp:susp){ if(g_alerted.count(sp.pid)) continue;
    std::string store; bool hit=false;
    for(const std::string& f:g_sys->openFiles(sp.pid)){ const std::string l=lower(f); for(const std::string& d:dirs) if(l.rfind(d,0)==0){ store=d; hit=true; break; } if(hit) break; }
    if(!hit) continue;
    g_alerted.insert(sp.pid); g_tokenAlerts++; bump();
    const std::string lbl=credLabel(store);
    pushEv("WARN","token access ("+lbl+"): "+sp.name,sp.path);
    showOverlay("\xE2\x9A\xA0 Credential/token access",sp.name+"\n"+sp.path+"\nreading your "+lbl+" login data",true);
    stringsScanAsync(sp.path); }
}

// ================= files =================
// A new file is looked at 1.5 s after the last event about it (a program is still being written when its event arrives; nothing but its first bytes tell that it
// is a program, there is no .exe), and a .app bundle is one item: every event inside it re-arms the bundle's timer.
static std::mutex g_fpMx; static std::map<std::string,int64_t> g_fp;
static bool isOwnData(const std::string& p){ return hasI(lower(p),"/sentinelwatch/")||endsWithI(p,"com.sentinelwatch.agent.plist"); }
std::vector<std::string> watchedDirs(){
  const std::string h=homeDir(); std::vector<std::string> v; std::error_code ec;
  const std::vector<std::string> all={h+"/Library/Application Support",h+"/Library/LaunchAgents",h+"/Downloads","/private/tmp","/private/var/tmp","/Users/Shared"};
  for(const std::string& d:all) if(fs::is_directory(d,ec)) v.push_back(d);
  return v; }
void fileEvent(const std::string& path){
  if(!g_tFile.load()||path.empty()||isOwnData(path)) return;
  const std::string key=signTarget(path); std::lock_guard<std::mutex> g(g_fpMx); if(g_fp.size()<512||g_fp.count(key)) g_fp[key]=g_nowMs(); }
static void fileEvaluate(const std::string& key){
  struct stat st; if(lstat(key.c_str(),&st)!=0) return;                       // gone again: the old name of a rename, a temporary file
  const bool isDir=S_ISDIR(st.st_mode), isReg=S_ISREG(st.st_mode);
  if(!isDir&&!isReg) return;                                                  // links, sockets
  if(isDir&&!endsWithI(key,".app")) return;                                   // plain folders are not files
  const std::string name=baseName(key), l=lower(key), exe=isDir?mainExeOf(key):key;
  const bool cache=hasI(l,"/caches/")||hasI(l,"/cache/")||hasI(l,"/code cache/")||hasI(l,"/gpucache/");   // browsers write thousands of files here; only a script / .app name counts there
  const bool exec=isDir||isExecExt(key)||(!cache&&looksLikeProgram(key));
  const bool cred=g_tTokens.load()&&isCredArtifactName(key);
  if(!(g_tAllFiles.load()||exec||cred)) return;
  g_fileCount++; bump();
  if(cred&&isStagingDir(key)){                                                // a copy of a login / token store, dropped where it should not be
    g_tokenAlerts++; pushEv("WARN","token file staged: "+name,key);
    showOverlay("\xE2\x9A\xA0 Credential file copied",name+"\n"+key+"\n(possible token-theft staging)",true); }
  else if(!cred||exec){
    const std::string target=exe.empty()?key:exe; const bool susp=exec&&suspiciousExe(target);
    pushEv(susp?"WARN":"FILE",name,key);
    if(susp){ showOverlay("\xE2\x9A\xA0 New unsigned executable",name+"\n"+key,true); stringsScanAsync(target); } }
}
void fileTick(){
  std::vector<std::string> due; const int64_t now=g_nowMs();
  { std::lock_guard<std::mutex> g(g_fpMx); for(auto it=g_fp.begin();it!=g_fp.end();){ if(now-it->second>=1500){ due.push_back(it->first); it=g_fp.erase(it); } else ++it; } }
  if(!g_tFile.load()) return;
  size_t n=0; for(const std::string& p:due){ if(n++>=100) break; fileEvaluate(p); }        // (a flood of more than 100 in one second is dropped)
}

// ================= strings scan =================
// shortcut: a burst of more than 32 flagged files drops the rest, and a file rewritten to the same size within the same second is not read again until restart;
// upgrade: persist the queue / key the "already read" memory on a hash. The reading is the same as the Windows app's (scanfile.h).
int g_waitStableMs=1000;
static std::mutex g_sqMx; static std::deque<std::string> g_sq; static bool g_sWorker=false; static std::unordered_map<std::string,std::pair<uint64_t,int64_t>> g_sSeen;
static void scanOne(const std::string& path){
  uint64_t size=0; int64_t mt=0; if(!statReg(path,size,mt)||!size) return;
  for(int i=0;i<5&&g_waitStableMs>0;i++){                                    // a file that has just appeared may still be written: wait until its size stops changing (5 s at most)
    std::this_thread::sleep_for(std::chrono::milliseconds(g_waitStableMs)); uint64_t s2=0; int64_t m2=0; if(!statReg(path,s2,m2)) return;
    const bool same=(s2==size); size=s2; mt=m2; if(same) break; }
  const std::string key=lower(path);
  { std::lock_guard<std::mutex> g(g_sqMx); auto it=g_sSeen.find(key); if(it!=g_sSeen.end()&&it->second.first==size&&it->second.second==mt) return; }   // this version was read already
  std::vector<uint8_t> buf; if(!readCapped(path,buf)){ logLine("[STR] cannot read "+path); return; }
  { std::lock_guard<std::mutex> g(g_sqMx); if(g_sSeen.size()>=1024) g_sSeen.clear(); g_sSeen[key]=std::make_pair(size,mt); }
  const StrReport r=strscan::scan(buf.data(),buf.size());
  if(r.shown.empty()){ logLine("[STR] "+path+": no links or suspicious commands in its strings"); return; }
  pushEv("STR","strings: "+std::to_string(r.links)+" link(s), "+std::to_string(r.commands)+" command(s)",path);
  std::string toast=baseName(path); int n=0;
  for(const StrFinding& f:r.shown){ const std::string line=std::string(f.hot?"[!] ":"")+(f.command?"cmd: ":"")+f.text; logLine("[STR]   "+line);
    if(n++<2) toast+="\n"+(line.size()>52?line.substr(0,49)+"...":line); }
  if(r.shown.size()>2) toast+="\n+"+std::to_string(r.shown.size()-2)+" more in events.log";
  showOverlay("\xE2\x9A\xA0 Links / commands inside it",toast,true);
}
static void strWorker(){
  qosBackground();
  for(;;){ std::string p;
    { std::lock_guard<std::mutex> g(g_sqMx); if(g_sq.empty()||g_quit.load()){ g_sWorker=false; g_sq.clear(); return; } p=g_sq.front(); g_sq.pop_front(); }
    try{ scanOne(p); }catch(...){ logLine("[STR] the check of "+p+" failed (out of memory?)"); } }      // a thread must never end the app
}
void stringsScanAsync(const std::string& path){
  if(!g_tStrings.load()||path.empty()||path[0]=='('||isOwnData(path)||path==g_selfExe) return;       // off; "(running)" placeholders; our own data and program
  { std::lock_guard<std::mutex> g(g_sqMx); if(g_sq.size()>=32||std::find(g_sq.begin(),g_sq.end(),path)!=g_sq.end()) return; g_sq.push_back(path); if(g_sWorker) return; g_sWorker=true; }
  try{ std::thread(strWorker).detach(); }catch(...){ std::lock_guard<std::mutex> g(g_sqMx); g_sWorker=false; g_sq.clear(); }
}
bool stringsScanIdle(){ std::lock_guard<std::mutex> g(g_sqMx); return !g_sWorker&&g_sq.empty(); }

// ================= start at login =================
static std::string xmlEsc(const std::string& s){ std::string o; for(char c:s){ switch(c){ case '&': o+="&amp;"; break; case '<': o+="&lt;"; break; case '>': o+="&gt;"; break; case '"': o+="&quot;"; break; case '\'': o+="&apos;"; break; default: o+=c; } } return o; }
std::string launchAgentPlist(const std::string& exe){
  return std::string("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n<plist version=\"1.0\">\n<dict>\n")
    +"\t<key>Label</key>\n\t<string>"+AGENT_LABEL+"</string>\n"
    +"\t<key>ProgramArguments</key>\n\t<array>\n\t\t<string>"+xmlEsc(exe)+"</string>\n\t\t<string>--background</string>\n\t</array>\n"
    +"\t<key>RunAtLoad</key>\n\t<true/>\n\t<key>LimitLoadToSessionType</key>\n\t<string>Aqua</string>\n</dict>\n</plist>\n"; }
void autostartApply(){
  const std::string path=homeDir()+"/Library/LaunchAgents/"+AGENT_LABEL+".plist", exe=g_selfExe;
  if(!g_tAutostart.load()){ if(std::remove(path.c_str())==0) logLine("[AUTOSTART] off: LaunchAgent removed"); return; }   // (launchd keeps nothing running: the agent only starts at login)
  if(exe.empty()) return;
  const std::string want=launchAgentPlist(exe);
  if(readWhole(path)==want) return;
  std::error_code ec; fs::create_directories(fs::path(path).parent_path(),ec);
  logLine(writeFileAtomic(path,want)?"[AUTOSTART] on: "+exe:"[AUTOSTART] could not write "+path);
}

// ================= quotes, AFK =================
static std::vector<std::string> g_normal,g_special; static std::string g_user="Lunaris", g_curRaw;
static Afk g_afk; static int64_t g_qT0=0; static std::mutex g_qMx; static std::mt19937 g_rng{std::random_device{}()};
static std::vector<std::string> readLines(const std::string& p){ std::vector<std::string> v; std::ifstream f(p); std::string ln;
  while(std::getline(f,ln)){ if(!ln.empty()&&ln.back()=='\r') ln.pop_back(); if(!ln.empty()&&ln[0]!='#') v.push_back(ln); } return v; }
static void pickQuote(){                                                       // g_qMx held
  const std::vector<std::string>& pool=g_afk.on?g_special:g_normal; if(pool.empty()) return;
  size_t i=g_rng()%pool.size(); const auto cur=std::find(pool.begin(),pool.end(),g_curRaw);
  if(pool.size()>1&&cur!=pool.end()){ const size_t c=(size_t)(cur-pool.begin()); i=g_rng()%(pool.size()-1); if(i>=c) i++; }        // never the quote that is showing
  g_curRaw=pool[i]; std::string q=g_curRaw;
  for(size_t k=q.find("{user}");k!=std::string::npos;k=q.find("{user}",k+g_user.size())) q.replace(k,6,g_user);
  g_afk.quote=q; g_afk.seq++; }
void loadContent(const std::string& bundleDir){
  const std::string cd=dataDir()+"/content"; std::error_code ec; fs::create_directories(cd+"/afk",ec);
  { std::ofstream f(cd+"/afk/README.txt",std::ios::binary|std::ios::trunc);       // rewritten at every start: it is documentation, not user data
    f<<"SENTINEL - AFK screen background\n\nThe app brings its own looping clips (afk_bg_4k60.mp4, afk_bg_lite.mp4). Want your own? Drop a video (.mp4 / .m4v / .mov) in this folder:\n"
       "it outranks the built-in clips; delete it and they come back. No video at all: the AFK screen shows the clock and the quote on black.\n\n"
       "Tuning: ~/Library/Application Support/SentinelWatch/settings.json\n  \"afkIdleSec\": 0   (0 = after 5 minutes without keyboard or mouse; 5..3600 = after that many seconds, 10 is handy for testing)\n"
       "Lines: ../facts.txt, ../quotes.txt, ../afk.txt (one per line, **bold**, {user}); delete one to get the built-in list back at the next start.\n"; }
  for(const char* n:{"facts.txt","quotes.txt","afk.txt"}){ const std::string dst=cd+"/"+n, src=bundleDir+"/content/"+n; if(!fs::exists(dst,ec)&&fs::exists(src,ec)) fs::copy_file(src,dst,ec); }
  std::lock_guard<std::mutex> g(g_qMx);
  g_normal=readLines(cd+"/facts.txt"); { const auto q=readLines(cd+"/quotes.txt"); g_normal.insert(g_normal.end(),q.begin(),q.end()); } g_special=readLines(cd+"/afk.txt");
  struct passwd* pw=getpwuid(getuid());
  if(pw){ std::string n=pw->pw_gecos?pw->pw_gecos:""; n=n.substr(0,n.find(',')); if(n.empty()&&pw->pw_name) n=pw->pw_name; if(!n.empty()) g_user=utf8Clean(n); }
  g_afk=Afk(); g_curRaw.clear(); g_qT0=g_nowMs(); pickQuote(); }
bool quoteTick(double idleSec){
  std::lock_guard<std::mutex> g(g_qMx); const int64_t now=g_nowMs(); bool changed=false;
  const bool afk=idleSec>=(g_afkIdleSec.load()>0?g_afkIdleSec.load():300);
  if(afk!=g_afk.on){ g_afk.on=afk; pickQuote(); g_qT0=now; changed=true; }
  else if(now-g_qT0>=120000){ pickQuote(); g_qT0=now; changed=true; }
  if(changed) bump();
  return changed; }
Afk afkState(){ std::lock_guard<std::mutex> g(g_qMx); return g_afk; }
std::string pickAfkVideo(const std::string& resDir){
  std::error_code ec; std::vector<std::string> own;
  for(fs::directory_iterator it(dataDir()+"/content/afk",ec),end; !ec&&it!=end; it.increment(ec)){ const std::string f=it->path().filename().string();
    if(f[0]!='.'&&(endsWithI(f,".mp4")||endsWithI(f,".m4v")||endsWithI(f,".mov"))) own.push_back(it->path().string()); }
  if(!own.empty()){ std::sort(own.begin(),own.end()); return own[0]; }
  for(const char* n:{"/afk_bg_4k60.mp4","/afk_bg_lite.mp4"}) if(fs::exists(resDir+n,ec)) return resDir+n;
  return ""; }

// ================= dashboard =================
struct Device{ std::string host,cpu,mem; };
static Device g_dev; static int64_t g_t0=0;
static std::string sysctlStr(const char* name){
#if SW_OS_MAC
  size_t n=0; if(sysctlbyname(name,nullptr,&n,nullptr,0)!=0||!n) return ""; std::string s(n,'\0'); if(sysctlbyname(name,&s[0],&n,nullptr,0)!=0) return "";
  while(!s.empty()&&s.back()=='\0') s.pop_back(); return s;
#else
  (void)name; return "";
#endif
}
static void gatherDevice(){
  char h[256]={0}; gethostname(h,sizeof h-1); g_dev.host=h;
  const long cores=sysconf(_SC_NPROCESSORS_ONLN); const std::string brand=sysctlStr("machdep.cpu.brand_string");
  g_dev.cpu=(brand.empty()?"":brand+" \xC2\xB7 ")+std::to_string(cores>0?cores:1)+" cores";
  const long long mem=(long long)sysconf(_SC_PHYS_PAGES)*(long long)sysconf(_SC_PAGESIZE);
  g_dev.mem=std::to_string((mem+(1LL<<29))>>30)+" GB"; }
static long uptimeMin(){
#if SW_OS_MAC
  struct timeval bt; size_t n=sizeof bt; if(sysctlbyname("kern.boottime",&bt,&n,nullptr,0)!=0) return 0; return (long)((time(nullptr)-bt.tv_sec)/60);
#else
  double up=0; std::ifstream f("/proc/uptime"); f>>up; return (long)(up/60);
#endif
}
std::string stateJson(const std::string& display){
  const Afk a=afkState(); const std::vector<Ev> ev=eventsCopy(); size_t known; { std::lock_guard<std::mutex> g(g_scanMx); known=g_known.size(); }
  auto b=[](bool v){ return v?"true":"false"; };
  std::string s="{\"version\":"+jsonStr(SW_VERSION)+",\"build\":"+jsonStr(SW_BUILD_ID)+",\"afk\":"+b(a.on)+",\"quote\":"+jsonStr(a.quote)+",\"quoteSeq\":"+std::to_string(a.seq)
    +",\"user\":"+jsonStr(g_user)+",\"sessionMs\":"+std::to_string(g_nowMs()-g_t0)+",\"procs\":"+std::to_string(g_procCount.load())+",\"files\":"+std::to_string(g_fileCount.load())
    +",\"tokenAlerts\":"+std::to_string(g_tokenAlerts.load())+",\"autoruns\":"+std::to_string(known)+",\"tokenGuard\":"+b(g_tTokens.load())+",\"signOk\":"+b(g_signUsable.load())+",\"toggles\":[";
  for(int i=0;i<TOG_N;i++){ if(i) s+=","; s+=std::string("{\"key\":")+jsonStr(g_toggles[i].key)+",\"name\":"+jsonStr(g_toggles[i].name)+",\"desc\":"+jsonStr(g_toggles[i].desc)+",\"on\":"+b(g_toggles[i].v->load())+"}"; }
  s+="],\"events\":[";
  for(size_t i=0;i<ev.size();i++){ if(i) s+=","; s+="{\"tag\":"+jsonStr(ev[i].tag)+",\"name\":"+jsonStr(ev[i].name)+",\"path\":"+jsonStr(ev[i].path)+",\"tm\":"+jsonStr(ev[i].tm)+"}"; }
  s+="],\"device\":{\"host\":"+jsonStr(g_dev.host)+",\"cpu\":"+jsonStr(g_dev.cpu)+",\"mem\":"+jsonStr(g_dev.mem)+",\"display\":"+jsonStr(display)+",\"uptimeMin\":"+std::to_string(uptimeMin())+"}}";
  return s; }
bool cmdToggle(const std::string& key){
  for(int i=0;i<TOG_N;i++) if(key==g_toggles[i].key){ g_toggles[i].v->store(!g_toggles[i].v->load()); saveSettings(); if(g_toggles[i].v==&g_tAutostart) autostartApply(); bump(); return true; }
  return false; }
void cmdScan(){ doScan(true); }
bool cmdRevealOk(const std::string& path){ for(const Ev& e:eventsCopy()) if(e.path==path) return true; return false; }

// ================= life cycle =================
static int g_lockFd=-1;
bool acquireInstanceLock(){
  const int fd=open((dataDir()+"/instance.lock").c_str(),O_RDWR|O_CREAT|O_CLOEXEC,0600); if(fd<0) return true;     // cannot even make the file: run anyway
  if(flock(fd,LOCK_EX|LOCK_NB)!=0){ close(fd); return false; }
  g_lockFd=fd; return true; }                                                                                     // (the lock dies with the process, crash or not)
void releaseInstanceLock(){ if(g_lockFd>=0){ close(g_lockFd); g_lockFd=-1; } }
void appInit(const std::string& bundleDir,bool background){
  g_quit.store(false); logRotate(); loadSettings(); saveSettings(); gatherDevice(); g_t0=g_nowMs();
  logLine(std::string("[APP] Sentinel ")+SW_VERSION+" build "+SW_BUILD_ID+" starting on "+SW_OS_NAME+(background?" (hidden in the menu bar)":""));
  autostartApply(); loadContent(bundleDir); loadBaseline(); }
static std::thread g_eng; static std::mutex g_engMx; static std::condition_variable g_engCv;
static void engineLoop(){
#if SW_OS_MAC
  pthread_set_qos_class_self_np(QOS_CLASS_UTILITY,0);
#endif
  try{ signSelfTest(); procScan(); }catch(...){ logLine("[ENGINE] the first scan threw"); }
  for(unsigned n=1;!g_quit.load();n++){
    { std::unique_lock<std::mutex> lk(g_engMx); g_engCv.wait_for(lk,std::chrono::seconds(1),[]{ return g_quit.load(); }); }
    if(g_quit.load()) break;
    try{ if(n%2==0) procTick(); if(n%4==0) tokenGuardTick(); fileTick(); if(n%300==0) doScan(false); }
    catch(...){ logLine("[ENGINE] a watcher threw; going on"); } }            // a thread must never end the app
}
void engineStart(){ g_quit.store(false); g_eng=std::thread(engineLoop); }
void engineStop(){ g_quit.store(true); g_engCv.notify_all(); if(g_eng.joinable()) g_eng.join(); }

// tests/mac_logic_tests.cpp: everything back to a fresh start (no strings-scan thread may be running)
void testReset(){
  { std::lock_guard<std::mutex> g(g_evMx); g_events.clear(); g_secWin=0; g_secCnt=0; }
  g_procCount=0; g_fileCount=0; g_tokenAlerts=0; g_toastHook=nullptr; g_signUsable=true; g_syncScan=true; g_waitStableMs=1000; g_afkIdleSec=0; g_dashW=DASH_W; g_dashH=DASH_H;
  static const bool defs[TOG_N]={true,true,true,false,true,true,true,true,true,true}; for(int i=0;i<TOG_N;i++) g_toggles[i].v->store(defs[i]);
  { std::lock_guard<std::mutex> g(g_sigMx); g_sig.clear(); g_sigWarned.clear(); }
  { std::lock_guard<std::mutex> g(g_pcMx); g_pc.clear(); } { std::lock_guard<std::mutex> g(g_scanMx); g_known.clear(); }
  { std::lock_guard<std::mutex> g(g_fpMx); g_fp.clear(); } { std::lock_guard<std::mutex> g(g_sqMx); g_sq.clear(); g_sSeen.clear(); }
  g_alerted.clear(); { std::lock_guard<std::mutex> g(g_qMx); g_normal.clear(); g_special.clear(); g_afk=Afk(); g_curRaw.clear(); g_qT0=g_nowMs(); }
  g_selfExe.clear(); g_fsRoot.clear(); }

}
