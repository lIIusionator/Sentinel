// eco_tests.cpp -- native unit tests for shared/eco.h (no Windows needed): when power saving starts and stops, and that the user's own power plan always comes back.
//
//   g++ -std=c++17 -O1 -g -Wall -Wextra -I shared tests/eco_tests.cpp -o eco_tests && ./eco_tests
#include "eco.h"
#include <cstdio>
#include <set>
#include <vector>

static int g_fail=0, g_checks=0;
#define CHECK(c) do{ g_checks++; if(!(c)){ g_fail++; printf("  FAIL %s:%d  %s\n",__FILE__,__LINE__,#c); } }while(0)

// A fake operating system: a set of plans, one of them active, a marker, and switches that make single calls fail.
struct Fake: EcoSys{
  std::set<std::string> plans{"user",ECO_BALANCED}; std::string active="user";
  bool hasMarker=false; std::string mUser,mEco; int made=0;
  bool failActive=false, failMake=false, failMarker=false, failActivateEco=false, failActivateUser=false, failActivateAll=false;
  std::vector<std::string> logs;
  std::string activePlan() override{ return failActive?std::string():active; }
  std::string makePlan(const std::string& copyOf) override{ if(failMake||!plans.count(copyOf)) return ""; std::string id="eco"+std::to_string(++made); plans.insert(id); return id; }
  bool activate(const std::string& p) override{
    if(failActivateAll||!plans.count(p)||(failActivateEco&&p.rfind("eco",0)==0)||(failActivateUser&&p=="user")) return false; active=p; return true; }
  void remove(const std::string& p) override{ if(p!=active) plans.erase(p); }       // an active plan cannot be deleted, as in Windows
  bool saveMarker(const std::string& u,const std::string& e) override{ if(failMarker) return false; hasMarker=true; mUser=u; mEco=e; return true; }
  bool loadMarker(std::string& u,std::string& e) override{ if(!hasMarker) return false; u=mUser; e=mEco; return true; }
  void clearMarker() override{ hasMarker=false; }
  void log(const std::string& l) override{ logs.push_back(l); }
  int ecoPlans() const{ int n=0; for(auto& p:plans) if(p.rfind("eco",0)==0) n++; return n; }
  bool logged(const char* part) const{ for(auto& l:logs) if(l.find(part)!=std::string::npos) return true; return false; }
};

static void testCycle(){
  Fake f; EcoMode m(f); int64_t t=1000;
  m.tick(t,false,true); CHECK(!m.on()&&f.active=="user"&&!f.hasMarker);                     // not away: nothing
  m.tick(t+=1000,true,false); CHECK(!m.on()&&f.active=="user");                              // away, switch off: nothing
  m.tick(t+=1000,true,true);                                                                 // away, switch on: power saving
  CHECK(m.on()&&f.active=="eco1"&&f.hasMarker&&f.mUser=="user"&&f.mEco=="eco1"&&f.logged("power saving ON")&&f.logged("a copy of user"));
  m.tick(t+=1000,true,true); m.tick(t+=1000,true,true); CHECK(f.made==1&&f.active=="eco1");  // still away: nothing more happens
  m.tick(t+=1000,false,true);                                                                // the user is back
  CHECK(!m.on()&&f.active=="user"&&!f.hasMarker&&f.ecoPlans()==0&&f.logged("power saving OFF: back on plan user"));
  m.tick(t+=1000,false,true); CHECK(f.active=="user"&&f.made==1);                            // and nothing changes afterwards
  m.tick(t+=1000,true,true); CHECK(m.on()&&f.active=="eco2");                                // away again: a fresh copy
  m.tick(t+=1000,true,false); CHECK(!m.on()&&f.active=="user"&&!f.hasMarker&&f.ecoPlans()==0);   // the switch turned off while saving: back at once
  Fake g; EcoMode n(g); n.stop(); CHECK(g.active=="user"&&g.logs.empty());                  // stop() while not saving does nothing
  n.tick(1,true,true); n.stop(); CHECK(!n.on()&&g.active=="user"&&!g.hasMarker&&g.ecoPlans()==0);   // stop() while saving puts everything back
}

static void testStartFailures(){
  { Fake f; EcoMode m(f); f.failActive=true; m.tick(0,true,true); CHECK(!m.on()&&f.made==0&&!f.hasMarker&&f.logged("NOT started")); }
  { Fake f; EcoMode m(f); f.failMake=true; m.tick(0,true,true); CHECK(!m.on()&&f.active=="user"&&!f.hasMarker&&f.ecoPlans()==0); }
  { Fake f; EcoMode m(f); f.failMarker=true; m.tick(0,true,true);                            // no marker, no power saving: a crash could not be undone
    CHECK(!m.on()&&f.active=="user"&&f.ecoPlans()==0&&f.logged("recovery marker")); }
  { Fake f; EcoMode m(f); f.failActivateEco=true; m.tick(0,true,true);
    CHECK(!m.on()&&f.active=="user"&&f.ecoPlans()==0&&!f.hasMarker&&f.logged("cannot switch")); }
  // a failure is retried after ECO_RETRY_MS, three failures in a row are the limit, and the user coming back starts over
  Fake f; EcoMode m(f); f.failMake=true; int64_t t=0;
  m.tick(t,true,true); CHECK(f.logs.size()==1);
  m.tick(t+=1000,true,true); m.tick(t+=ECO_RETRY_MS-2000,true,true); CHECK(f.logs.size()==1);  // too early: no new try (and no new log line)
  m.tick(t+=1000,true,true); CHECK(f.logs.size()==2);                                         // after the wait: second try
  f.failMake=false; m.tick(t+=ECO_RETRY_MS,true,true); CHECK(m.on());                         // it works now
  m.tick(t+=1000,false,true); CHECK(!m.on());
  Fake h; EcoMode k(h); h.failMake=true; t=0;
  for(int i=0;i<ECO_MAX_FAILS;i++){ k.tick(t,true,true); t+=ECO_RETRY_MS; }
  CHECK(h.logs.size()==(size_t)ECO_MAX_FAILS&&h.logged("giving up"));
  h.failMake=false; k.tick(t+=ECO_RETRY_MS,true,true); CHECK(!k.on());                       // gave up: stays off however long the user is away
  k.tick(t+=1000,false,true); k.tick(t+=1000,true,true); CHECK(k.on());                       // back once, away again: a new attempt
}

static void testLeaveFailures(){
  { Fake f; EcoMode m(f); m.tick(0,true,true); f.failActivateUser=true;                      // the user's plan cannot be activated: stay on and keep trying (a busy power service) ...
    for(int i=1;i<ECO_FALLBACK_AFTER;i++) m.tick(i*1000,false,true);
    CHECK(m.on()&&f.active=="eco1"&&f.hasMarker);
    m.tick(ECO_FALLBACK_AFTER*1000,false,true);                                               // ... and only after ECO_FALLBACK_AFTER failures in a row call it gone and land on Balanced
    CHECK(!m.on()&&f.active==ECO_BALANCED&&!f.hasMarker&&f.ecoPlans()==0&&f.logged("Balanced")); }
  { Fake f; EcoMode m(f); m.tick(0,true,true); f.failActivateUser=true; for(int i=1;i<5;i++) m.tick(i*1000,false,true);   // it comes back before the limit: the user's plan, not Balanced
    f.failActivateUser=false; m.tick(6000,false,true); CHECK(!m.on()&&f.active=="user"&&!f.hasMarker&&!f.logged("Balanced")); }
  { Fake f; EcoMode m(f); m.tick(0,true,true); f.failActivateAll=true; int64_t t=0;
    for(int i=0;i<130;i++) m.tick(t+=1000,false,true);
    CHECK(m.on()&&f.active=="eco1"&&f.hasMarker&&f.logged("cannot go back"));
    size_t n=0; for(auto& l:f.logs) if(l.find("cannot go back")!=std::string::npos) n++; CHECK(n==3);   // logged on the 1st, 60th and 120th failure, not every second
    f.failActivateAll=false; m.tick(t+=1000,false,true); CHECK(!m.on()&&f.active=="user"&&!f.hasMarker&&f.ecoPlans()==0); }
  { Fake f; EcoMode m(f); m.tick(0,true,true); f.plans.erase("user");                         // the user's plan is gone: Balanced after the wait
    for(int i=1;i<=ECO_FALLBACK_AFTER;i++) m.tick(i*1000,false,true);
    CHECK(!m.on()&&f.active==ECO_BALANCED&&!f.hasMarker&&f.ecoPlans()==0&&f.logged("Balanced")); }
  { Fake f; EcoMode m(f); m.tick(0,true,true); f.plans.erase("user"); m.stop();               // closing the app is one try: no fallback, the marker stays for the next start
    CHECK(m.on()&&f.active=="eco1"&&f.hasMarker); EcoMode next(f); next.recover(); CHECK(f.active==ECO_BALANCED&&!f.hasMarker&&f.ecoPlans()==0); }
}

static void testRecover(){
  { Fake f; EcoMode m(f); m.recover(); CHECK(f.active=="user"&&f.logs.empty()); }                                   // no marker: nothing
  { Fake f; f.plans.insert("eco9"); f.active="eco9"; f.hasMarker=true; f.mUser="user"; f.mEco="eco9"; EcoMode m(f);   // the copy was left active
    m.recover(); CHECK(f.active=="user"&&!f.hasMarker&&f.ecoPlans()==0&&f.logged("left a marker")&&f.logged("back on plan user")); }
  { Fake f; f.plans.insert("eco9"); f.plans.insert("mine"); f.active="mine"; f.hasMarker=true; f.mUser="user"; f.mEco="eco9"; EcoMode m(f);   // the user chose another plan since
    m.recover(); CHECK(f.active=="mine"&&!f.hasMarker&&f.ecoPlans()==0); }
  { Fake f; f.plans.insert("eco9"); f.plans.erase("user"); f.active="eco9"; f.hasMarker=true; f.mUser="user"; f.mEco="eco9"; EcoMode m(f);   // their plan is gone
    m.recover(); CHECK(f.active==ECO_BALANCED&&!f.hasMarker&&f.ecoPlans()==0); }
  { Fake f; f.plans.insert("eco9"); f.active="eco9"; f.hasMarker=true; f.mUser="user"; f.mEco="eco9"; f.failActivateAll=true; EcoMode m(f);   // cannot go back: keep the marker for the next start
    m.recover(); CHECK(f.active=="eco9"&&f.hasMarker&&f.plans.count("eco9")&&f.logged("next start")); }
  { Fake f; f.plans.insert("eco9"); f.active="eco9"; f.hasMarker=true; f.mUser=""; f.mEco="eco9"; EcoMode m(f);   // a damaged marker still ends on a real plan
    m.recover(); CHECK(f.active==ECO_BALANCED&&!f.hasMarker); }
  { Fake f; f.plans.insert("eco9"); f.active="eco9"; f.failActive=true; f.hasMarker=true; f.mUser="user"; f.mEco="eco9"; EcoMode m(f);   // the active plan cannot be read
    m.recover(); CHECK(f.active=="user"&&!f.hasMarker); }
  { Fake f; EcoMode m(f); m.tick(0,true,true); CHECK(f.hasMarker);                                                  // the full story: on, the app dies, the next start undoes it
    EcoMode next(f); next.recover(); CHECK(f.active=="user"&&!f.hasMarker&&f.ecoPlans()==0&&!next.on()); }
}

int main(){
  CHECK(ECO_AFTER_MS==30*60*1000); CHECK(ECO_CPU_MAX_PCT<30&&ECO_CPU_MAX_PCT>0); CHECK(ECO_BRIGHT_PCT>0&&ECO_BRIGHT_PCT<50);
  testCycle(); testStartFailures(); testLeaveFailures(); testRecover();
  printf("\n%d checks, %d failed\n",g_checks,g_fail);
  return g_fail?2:0;
}
