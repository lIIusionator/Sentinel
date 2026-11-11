// eco.h -- "power saving when away": after ECO_AFTER_MS without keyboard / mouse input the PC runs on a power-saving setup, and goes back the moment the user returns.
//
// How: a temporary COPY of the active Windows power plan, with a few values lowered (CPU maximum, brightness, ...), becomes the active plan. Going back is "make the user's own
// plan active again, delete the copy": the user's plans are never edited. A marker file names both plans, so a crash, a kill or a shutdown while the copy was active is undone at
// the next start. Pure logic over a small interface (no Windows headers): tests/eco_tests.cpp. The Windows side (powrprof.dll, the marker file, the 1 s tick) is core/eco.cpp.
#pragma once
#include <string>
#include <stdint.h>

static const int64_t ECO_AFTER_MS  =30LL*60*1000;   // no input for this long -> power saving ("after 30 minutes AFK")
static const int     ECO_CPU_MAX_PCT=25;            // "Maximum processor state" while saving ("lower power to under 30 %")
static const int     ECO_BRIGHT_PCT =15;            // display brightness while saving (never raises a lower one)
static const int64_t ECO_RETRY_MS  =60*1000;        // a switch that failed is tried again after this long ...
static const int     ECO_MAX_FAILS =3;              // ... and given up after this many failures in a row, until the user has been back once
static const char* const ECO_BALANCED="381b4222-f694-41f0-9685-ff5bb260df2e";   // Windows' own "Balanced" plan: where to land if the user's plan has vanished
static const int     ECO_FALLBACK_AFTER=10;         // going back to the user's plan failed this many times in a row (once a second): it is gone, not busy -> Balanced
static_assert(ECO_CPU_MAX_PCT<30,"the request was: lower the CPU to under 30 %");

// What the OS has to offer. Plan ids are GUID text (lower case, no braces); "" = failed. The Windows one is core/eco.cpp, the test has a fake.
struct EcoSys{
  virtual ~EcoSys(){}
  virtual std::string activePlan()=0;
  virtual std::string makePlan(const std::string& copyOf)=0;     // a copy of that plan with the saving values applied (the CPU cap is required, the rest best effort)
  virtual bool activate(const std::string& plan)=0;
  virtual void remove(const std::string& plan)=0;
  virtual bool saveMarker(const std::string& user,const std::string& eco)=0;   // false: it could not be written
  virtual bool loadMarker(std::string& user,std::string& eco)=0;               // false: there is none
  virtual void clearMarker()=0;
  virtual void log(const std::string& line)=0;
};

class EcoMode{
 public:
  explicit EcoMode(EcoSys& s):m_s(s){}
  bool on() const{ return m_on; }
  // Called about once a second. away: no input for ECO_AFTER_MS. enabled: the switch on the Protection tab.
  void tick(int64_t nowMs,bool away,bool enabled){
    const bool want=away&&enabled;
    if(!want) m_fails=0;                                             // the user was back (or the switch is off): a fresh start next time
    if(want&&!m_on){ if(m_fails<ECO_MAX_FAILS&&nowMs>=m_nextTryMs) enter(nowMs); }
    else if(!want&&m_on) leave(); }
  void stop(){ if(m_on) leave(); }                                   // the app is closing, or the session is ending: one try (a failure is left to recover() at the next start)
  // At start: undo what a run that did not end cleanly left behind.
  void recover(){
    std::string user,eco; if(!m_s.loadMarker(user,eco)) return;
    const std::string cur=m_s.activePlan();
    m_s.log("power saving: the last run left a marker (your plan "+user+", the copy "+eco+"); the active plan is "+(cur.empty()?std::string("unknown"):cur));
    if(cur.empty()||cur==eco){                                       // the copy is still the active plan
      if(!m_s.activate(user)&&!m_s.activate(ECO_BALANCED)){
        m_s.log("power saving: left on by the last run, and cannot go back to plan "+user+" -- will try again at the next start"); return; }
      m_s.log("power saving: the last run ended while it was on -- back on plan "+user); }
    if(!eco.empty()) m_s.remove(eco);
    m_s.clearMarker(); }
 private:
  void enter(int64_t nowMs){
    auto fail=[&](const char* what){ m_fails++; m_nextTryMs=nowMs+ECO_RETRY_MS;
      m_s.log(std::string("power saving NOT started: ")+what+(m_fails>=ECO_MAX_FAILS?" -- giving up until you have been back":"")); };
    // shortcut: a copy left active by a run whose marker was deleted is not recognised (it would be copied as if it were the user's plan); add a sweep by plan name if that ever shows up.
    const std::string user=m_s.activePlan();  if(user.empty()){ fail("cannot read the active power plan"); return; }
    const std::string eco=m_s.makePlan(user); if(eco.empty()){ fail("cannot make the power-saving plan"); return; }
    if(!m_s.saveMarker(user,eco)){ m_s.remove(eco); fail("cannot write the recovery marker"); return; }
    if(!m_s.activate(eco)){ m_s.remove(eco); m_s.clearMarker(); fail("cannot switch to the power-saving plan"); return; }
    m_on=true; m_user=user; m_eco=eco; m_fails=0; m_leaveFails=0;
    m_s.log("power saving ON (no input for "+std::to_string(ECO_AFTER_MS/60000)+" min): plan "+eco+", a copy of "+user+", is active -- CPU max "+std::to_string(ECO_CPU_MAX_PCT)
      +" %, display brightness at most "+std::to_string(ECO_BRIGHT_PCT)+" %"); }
  void leave(){
    std::string landed=m_user; bool ok=m_s.activate(m_user);
    if(!ok&&++m_leaveFails>=ECO_FALLBACK_AFTER&&m_s.activate(ECO_BALANCED)){ ok=true; landed=std::string(ECO_BALANCED)+" (Balanced: plan "+m_user+" is gone)"; }
    if(!ok){ if(m_leaveFails==1||m_leaveFails%60==0) m_s.log("power saving: cannot go back to plan "+m_user+" -- trying again every second"); return; }
    m_s.remove(m_eco); m_s.clearMarker(); m_on=false; m_leaveFails=0;
    m_s.log("power saving OFF: back on plan "+landed); }
  EcoSys& m_s; bool m_on=false; std::string m_user,m_eco; int64_t m_nextTryMs=0; int m_fails=0, m_leaveFails=0;
};
