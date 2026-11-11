// content.cpp -- quotes / fun facts, AFK detection and the quote cross-fade state machine.
#include "content.h"
#include "settings.h"
#include "afk_state.h"
#include "afk_helpers.h"        // afkPreloadLeadMs (pure logic, no Windows headers)
#include <shlobj.h>
#define SECURITY_WIN32
#include <secext.h>
#include <fstream>
#include <cstdlib>

std::vector<std::wstring> g_facts,g_quotes,g_afk;
static std::vector<std::wstring> g_normal,g_special;   // facts+quotes / afk-special
static std::wstring g_pendingQuote;
std::wstring g_curQuote;
bool g_afkNow=false;
ULONGLONG g_afkEnterT0=0;
int g_quoteAlpha=255;
ULONGLONG g_quoteChangeT0=0;
static int g_fadePhase=2;                 // 0 stable, 1 fade-out, 2 fade-in
static ULONGLONG g_fadeT0=0, g_qT0=0;

static void writeIfMissing(const std::wstring& path,const std::vector<std::wstring>& lines){
  if(GetFileAttributesW(path.c_str())!=INVALID_FILE_ATTRIBUTES)return;
  std::ofstream f(wpath(path),std::ios::binary);
  for(auto&l:lines){ std::string u=toU8(l); f.write(u.data(),u.size()); f.put('\n'); }
}
static std::vector<std::wstring> readLines(const std::wstring& path){
  std::vector<std::wstring> v; std::ifstream f(wpath(path)); std::string ln;
  while(std::getline(f,ln)){ if(!ln.empty()&&ln.back()=='\r')ln.pop_back();
    if(!ln.empty()&&ln[0]!='#')v.push_back(fromUtf8(ln)); } return v;
}

void loadContent(){
  std::wstring cd=appDir()+L"\\content"; SHCreateDirectoryExW(0,cd.c_str(),0);
  std::wstring afkDir=cd+L"\\afk"; SHCreateDirectoryExW(0,afkDir.c_str(),0);
  // Rewritten on every launch (it is documentation, not user data).
  { std::ofstream f(wpath(afkDir+L"\\README.txt"),std::ios::binary|std::ios::trunc);
    const wchar_t* lines[]={
      L"SENTINEL - AFK screensaver background",
      L"",
      L"Two clips are built into sentinel.exe and unpack themselves here when first needed:",
      L"  sentinel_afk_4k60.mp4      3840x2160 @ 60 fps, H.264 + AAC. Played by the GPU video engine.",
      L"  sentinel_afk_1080p30.mp4   1920x1080 @ 30 fps. Used automatically if the GPU engine cannot",
      L"                             keep up (or cannot run) on this PC; the CPU renderer plays it too.",
      L"Both are H.264 on purpose: it plays everywhere, whereas HEVC needs a Windows extension",
      L"that many PCs (and slimmed-down Windows builds) do not have.",
      L"",
      L"Want your own looping background? Drop any video in this folder",
      L"(.mp4 / .m4v / .mov / .mkv / .webm / .avi / .wmv). A video of yours outranks the built-in",
      L"clips and is used on every tier (there is no lighter copy of it, so pick one your PC",
      L"can decode); delete it and the built-in ones come back. Built-in clips that are deleted",
      L"or damaged are unpacked again automatically.",
      L"",
      L"No video at all? Put numbered images here (frame_0001.png, frame_0002.png, ...):",
      L"they play in filename order at ~24 fps (up to 240 frames).",
      L"",
      L"Tuning: %LOCALAPPDATA%\\SentinelWatch\\settings.json",
      L"  \"afkMode\": \"auto\" | \"gpu\" | \"cpu\"   (auto: 4K GPU -> 1080p GPU -> CPU renderer)",
      L"  \"afkIdleSec\": 0   (0 = after the Windows screen-saver wait if one is set, else 5 minutes;",
      L"                      5..3600 = go AFK after that many idle seconds -- 10 is handy for testing)",
      L"  \"afkGpu\": \"default\" | \"intel\" | \"nvidia\" | \"amd\"   (which graphics chip plays the video on a PC with two;",
      L"                      \"default\" lets Windows choose, which is normally the right one)",
      L"  \"afkMinTier\": 0   (0 = try everything; the app raises this by itself when a tier fails,",
      L"                      remembers it for a week or until the next build, then tries again)",
      L"Everything the AFK screen does is logged to events.log with the tags [AFK] and [AFKVID].",
    };
    for(auto l:lines){ std::string u=toU8(l); f.write(u.data(),(std::streamsize)u.size()); f.put('\n'); } }
  // Built-in lines. They are written to facts.txt / quotes.txt / afk.txt only when those files do not exist yet (the files are the user's to edit), so a
  // changed list reaches an existing install only after the three files are deleted once. "**bold**" = emphasis, {user} = the display name. Keep a line under
  // ~130 characters: the sidebar box is 174 px wide and shrinks the font to fit.
  std::vector<std::wstring> facts={
    // about Lunaris
    L"Do you know Lunaris enjoys eating food, but having a **cold drink** nearby is easily the top tier preference?",
    L"Do you know Lunaris spends most of his time **alone**, just staying completely in his own space?",
    L"Do you know despite Lunaris having nearsighted vision, his eyes instantly spot any **tiny movement** in the room?",
    L"Do you know Lunaris picks up new skills **insanely fast** when he approaches them from a unique angle?",
    L"Do you know Lunaris loves **drawing** simply out of a deep appreciation for art itself?",
    L"Do you know Lunaris plays competitive games **solo**, even though his original goal was just having fun with friends?",
    L"Do you know Lunaris finds making **major life decisions** alone almost impossible without a clear guide?",
    L"Do you know Lunaris feels sharing too much personal stuff is like handing someone a **weapon** to hold against him?",
    L"Do you know Lunaris will favor his favorite people, until **raw logic** forces him to the opposite side?",
    L"Do you know Lunaris finds reorganizing code into its **simplest, cleanest** form deeply satisfying?",
    L"Do you know Lunaris plays **competitive Roblox football** seriously, with scrims, league matches and tryouts?",
    L"Do you know Lunaris runs through a **pre-scrim checklist** so nothing is left to luck?",
    L"Do you know Lunaris has been following the anime movie **Cosmic Princess Kaguya!** and its characters?",
    L"Do you know Lunaris likes making his desktop feel **personal**, with Rainmeter skins and wallpapers?",
    L"Do you know Lunaris built a **loading screen** with a cinematic first-person camera that drifts and breathes?",
    L"Do you know Lunaris has dug into **hitbox synchronization**, from client prediction to server validation?",
    L"Do you know Lunaris wrote his own tiny **music player**: no window, no hotkeys, just shuffle and a low volume?",
    L"Do you know Lunaris made his own **screen recorder** with AutoHotkey and FFmpeg, and **F9** starts and stops it?",
    L"Do you know Lunaris uses **AutoHotkey** to manage his game's process priority, so it always gets the CPU first?",
    L"Do you know Lunaris built **Sentinel** himself, a tool that watches file changes to make spotting viruses easier?",
    L"Do you know Lunaris scripted an **anime-style hair accessory** in Blender with Python?",
    // everyday curiosities
    L"Do you know honey **never truly spoils**, and edible jars have been found in ancient Egyptian tombs?",
    L"Do you know sound travels about **four times faster** through water than through air?",
    L"Do you know a **day on Venus** is actually longer than an entire year on Venus?",
    L"Do you know an octopus has **three hearts**, and the main one stops beating while it swims?",
    L"Do you know **bananas are berries**, but strawberries are not?",
    L"Do you know the Eiffel Tower grows **up to 15 cm taller** in summer, because heat expands the iron?",
    L"Do you know lightning can be about **five times hotter** than the surface of the Sun?",
    L"Do you know sunsets on Mars look **blue**?",
    L"Do you know wombats make **cube-shaped** droppings?",
    L"Do you know the Moon drifts **3.8 cm farther** from Earth every year?",
    L"Do you know there are more possible **chess games** than atoms in the observable universe?",
    L"Do you know sharks have been around **longer than trees**?",
    L"Do you know your brain is about 2% of your body weight but uses about **20% of its energy**?",
    L"Do you know **Saturn would float** in water, if you could find a bathtub big enough?",
    L"Do you know the first **literal** computer bug was a real moth, found inside a Harvard computer in 1947?",
    L"Do you know the first computer mouse had a **wooden shell**?",
    L"Do you know sunlight takes about **8 minutes** to reach the Earth?",
    L"Do you know one **day on Mercury**, from sunrise to sunrise, lasts about 176 Earth days?",
    L"Do you know a **tardigrade** can survive in the vacuum of space?",
    L"Do you know dolphins call each other with **signature whistles**, a bit like names?",
    L"Do you know the smell of rain has a name: **petrichor**?",
    L"Do you know **komorebi** is the Japanese word for sunlight filtering through the leaves of trees?",
    L"Do you know **tsundoku** is the Japanese word for buying books and letting them pile up unread?",
    L"Do you know **Antarctica** is technically the largest desert on Earth?",
    L"Do you know sea otters **hold hands** while they sleep so they don't drift apart?",
    L"Do you know koalas have **fingerprints** almost identical to human ones?",
    L"Do you know a group of flamingos is called a **flamboyance**?"};
  std::vector<std::wstring> quotes={
    L"Everything feels heavy right now, but I still like to think **tomorrow** might be a little quieter.",
    L"I don't know where I'm going, but **standing still** forever isn't an option either.",
    L"People can hurt you if you let them too close, but **isolation gets old**, so we keep trying.",
    L"It's hard to trust the process when you're lost in the noise, but the **clarity** always comes back.",
    L"My head is everywhere at once, but maybe that means **something good** is being built underneath.",
    L"The floor might feel unstable today, but I'm still **curious** how the story turns out.",
    L"And just like fine wine, our **memories** will only get better with time.",
    L"Keep **smiling**, even if life is hard.",
    L"Keep **moving**, so that you can't think about it.",
    L"Some days you only have to **get through**, and that is more than enough.",
    L"You don't need the whole map, only the **next step** in the light you have.",
    L"It's okay to be quiet for a while. Even the moon takes its time between **phases**.",
    L"Not every storm is here to break you. Some are only passing through to **clear the air**.",
    L"Small progress is still **progress**.",
    L"I'm not behind. I'm on a **different road**, and it has its own timing.",
    L"The night is long, but it's the only place the **stars** can show up.",
    L"Be gentle with yourself today. You're doing **better** than you think.",
    L"Rest is not giving up. It's how you **keep going**.",
    L"Some things take time to make sense. Keep the questions, the **answers** will find you.",
    L"Quiet doesn't mean empty. Sometimes it's just **room to grow**.",
    L"You've survived every bad day so far. That's a **perfect record**.",
    L"A calm mind isn't one without noise. It's one that knows **which noise to ignore**.",
    L"Even slow steps leave **footprints**.",
    L"It's fine to start over. A clean file is just a **fresh chance** to write it better.",
    L"Every clean version starts as a **messy first draft**.",
    L"Hold on to what feels **warm** and let the rest drift by.",
    L"Not everything has to be solved tonight. Some answers like to **sleep on it** first.",
    L"Stay curious. Almost every good thing started as a **question**.",
    L"You're allowed to take up **space**, even on the days you feel small.",
    L"The hard days teach you. The good days remind you why it was **worth it**.",
    L"Keep your circle **small** and your peace big."};
  std::vector<std::wstring> afk={
    L"You are you, only you can do it, because I know you. It will be fine; that is **Lunaris's guarantee**.",
    L"Itterasshai, {user}.",
    L"It is a cycle, one of a **destiny** that we cannot escape.",
    L"Take your time, {user}. I'll **keep watch** while you're away.",
    L"Rest well, {user}. I'll say **okaeri** when you're back.",
    L"Otsukaresama, {user}. You did **enough** today.",
    L"Put it all down for a minute, {user}. I'll **hold it** until you're back.",
    L"Breathe, {user}. The world will still be there in **five minutes**.",
    L"The stars don't hurry, and **neither do you**, {user}.",
    L"Watch the stars for a moment, {user}. **Nothing** needs you right now.",
    L"Lunaris believes in you, {user}. That is the **whole message**.",
    L"Whenever you are ready, {user}, **we will continue**.",
    L"Dream a little, {user}. The **next task** can wait.",
    L"Ganbatte, {user}. Not today though. **Today you rest**.",
    L"Everything that matters will be **right here** when you get back, {user}.",
    L"It is fine to do **nothing** for a while, {user}.",
    L"Cycles end and begin again, {user}. **Rest** is part of the loop."};
  writeIfMissing(cd+L"\\facts.txt",facts);
  writeIfMissing(cd+L"\\quotes.txt",quotes);
  writeIfMissing(cd+L"\\afk.txt",afk);
  g_facts=readLines(cd+L"\\facts.txt"); g_quotes=readLines(cd+L"\\quotes.txt"); g_afk=readLines(cd+L"\\afk.txt");
  g_normal.clear(); for(auto&x:g_facts)g_normal.push_back(x); for(auto&x:g_quotes)g_normal.push_back(x);
  g_special=g_afk;
  if(!g_normal.empty()) g_curQuote=g_normal[rand()%(int)g_normal.size()];
  g_fadePhase=2; g_fadeT0=GetTickCount64();
  { wchar_t dn[256]; ULONG dl=256;
    if(GetUserNameExW(NameDisplay,dn,&dl)&&dl>0) g_userName=dn;
    else { wchar_t un[256]; DWORD us=256; if(GetUserNameW(un,&us)&&us>1) g_userName=un; } }
}

// ---------- AFK detection / quote state ----------
// Desktop idle -> AFK after the system screensaver timeout (or 5 min). Called from the dashboard's render
// tick (so the sidebar quote keeps cross-fading while the dashboard is open) and from the always-on
// message-window timer (so AFK detection keeps working with the dashboard closed). Both run on the UI
// thread; the AFK render thread never touches these globals -- it reads the snapshot published below.
bool isAfk(DWORD ms){ LASTINPUTINFO li; li.cbSize=sizeof(li);
  if(!GetLastInputInfo(&li))return false; return (GetTickCount()-li.dwTime)>=ms; }
// "afkIdleSec" from settings.json if set, else the system idle timeout (screensaver setting) if one is enabled, else 5 minutes
DWORD afkThresholdMs(){
  const int forced=g_afkIdleSec.load(); if(forced>0) return (DWORD)forced*1000;
  BOOL ssActive=FALSE; int ssTimeout=0;
  if(SystemParametersInfoW(SPI_GETSCREENSAVEACTIVE,0,&ssActive,0) && ssActive
     && SystemParametersInfoW(SPI_GETSCREENSAVETIMEOUT,0,&ssTimeout,0) && ssTimeout>0)
    return (DWORD)ssTimeout*1000;
  return 300000; // 5 minutes
}
static void pickPending(){
  std::vector<std::wstring>& pool = g_afkNow ? g_special : g_normal;
  if(pool.empty()){ g_pendingQuote.clear(); return; }
  int idx=rand()%(int)pool.size();
  if(pool.size()>1){ int guard=0; while(pool[idx]==g_curQuote && guard++<8) idx=rand()%(int)pool.size(); }
  g_pendingQuote=pool[idx];
}
void triggerQuoteChange(){ pickPending(); g_fadePhase=1; g_fadeT0=GetTickCount64(); }

// Tell the AFK render thread about changes. It computes its own fade timing from these timestamps, so the
// UI thread only has to publish when something actually changed.
static bool g_afkWarm=false;      // idle for most of the wait (see afkPreloadLeadMs): the AFK thread builds its next player now, so the screen finds one ready
static void afkPublishState(){
  static bool have=false; static bool lAfk=false, lWarm=false; static int lPhase=-1; static ULONGLONG lPhaseT0=0; static ULONGLONG lEnter=0; static std::wstring lQuote,lUser;
  if(have && lAfk==g_afkNow && lWarm==g_afkWarm && lPhase==g_fadePhase && lPhaseT0==g_fadeT0 && lEnter==g_afkEnterT0 && lQuote==g_curQuote && lUser==g_userName) return;
  have=true; lAfk=g_afkNow; lWarm=g_afkWarm; lPhase=g_fadePhase; lPhaseT0=g_fadeT0; lEnter=g_afkEnterT0; lQuote=g_curQuote; lUser=g_userName;
  AfkSnapshot s; s.afkNow=g_afkNow; s.warm=g_afkWarm; s.enterT0=g_afkEnterT0; s.phase=g_fadePhase; s.phaseT0=g_fadeT0; s.quote=g_curQuote; s.user=g_userName;
  afkScreenPublish(s);
}

void updateQuoteState(){
  ULONGLONG now=GetTickCount64(); const DWORD lim=afkThresholdMs(); bool afk=isAfk(lim);
  g_afkWarm=afk||isAfk(lim-(DWORD)afkPreloadLeadMs((int64_t)lim));      // (the screen-saver wait can change while the app runs: read every tick, as the threshold itself is)
  if(afk!=g_afkNow){ g_afkNow=afk; triggerQuoteChange(); if(afk) g_afkEnterT0=now; }
  else if(g_fadePhase==0 && now-g_qT0>=120000) triggerQuoteChange();
  if(g_fadePhase==1){ double t=(double)(now-g_fadeT0)/380.0;
    if(t>=1){ g_curQuote=g_pendingQuote; g_fadePhase=2; g_fadeT0=now; g_quoteChangeT0=now; g_quoteAlpha=0; } else g_quoteAlpha=(int)(255*(1-t)); }
  else if(g_fadePhase==2){ double t=(double)(now-g_fadeT0)/380.0;
    if(t>=1){ g_fadePhase=0; g_qT0=now; g_quoteAlpha=255; } else g_quoteAlpha=(int)(255*t); }
  else g_quoteAlpha=255;
  afkPublishState();
}
