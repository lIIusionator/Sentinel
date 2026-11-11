// core.h -- the portable part of the Sentinel Watch Mac app: settings, the event feed, the trust rules, the watchers (processes, files, login items, token guard),
// the strings scan, "Start at login", the quote / AFK state and the JSON the dashboard shows. Plain C++17 + POSIX: nothing here includes an Apple header, and
// everything that needs the OS goes through sw::Sys (sys.h), so tests/mac_logic_tests.cpp runs all of it on any host against a fake.
// The shell (app.mm: menu bar, windows, toasts, AFK screen) and apple.mm (CoreGraphics / Foundation / FSEvents) sit on top; README section 12 has the map.
// Threads: the shell's main thread; one engine thread (engineStart) that ticks every watcher; one short-lived strings-scan thread; FSEvents callbacks.
#pragma once
#include "sys.h"
#include <atomic>
#include <deque>
#include <functional>
#include <set>
#include <string>
#include <vector>

namespace sw{

// ---- clock, paths ----
extern int64_t (*g_nowMs)();                       // monotonic milliseconds (a hook so that tests can move time)
extern std::string g_selfExe;                      // the running program; the shell sets it (the app's own LaunchAgent and the autorun watch use it)
extern std::string g_fsRoot;                       // "" ; tests point it at a scratch directory standing in for /Library
std::string homeDir();
std::string dataDir();                             // ~/Library/Application Support/SentinelWatch (made on demand); $SW_DATA_DIR replaces it
void setDataDir(const std::string& d);
std::string utf8Clean(const std::string& s);       // every invalid byte becomes U+FFFD (file names are not always valid UTF-8)
std::string jsonStr(const std::string& s);         // a JSON string literal with the quotes, safe to paste into JavaScript
void logLine(const std::string& s);                // "[HH:MM:SS] text" appended to events.log
void logRotate();                                  // events.log over 4 MB becomes events.old.log

// ---- settings.json (same keys as the Windows app where they apply) ----
struct Toggle{ std::atomic<bool>* v; const char* key; const char* name; const char* desc; };
static const int TOG_N=10;
extern Toggle g_toggles[TOG_N];
extern std::atomic<bool> g_tAutorun,g_tProc,g_tFile,g_tAllFiles,g_tNotify,g_tBeep,g_tIgnoreSys,g_tTokens,g_tStrings,g_tAutostart;
extern std::atomic<int> g_afkIdleSec;              // 0 = 5 minutes; 5..3600 = that many idle seconds
extern int g_dashW,g_dashH;                        // dashboard window size (persisted)
static const int DASH_W=940, DASH_H=600, DASH_MINW=780, DASH_MINH=560;
void saveSettings();
void loadSettings();

// ---- events ----
struct Ev{ std::string tag,name,path,tm; };
extern std::atomic<int> g_procCount,g_fileCount,g_tokenAlerts;
extern std::atomic<unsigned> g_rev;                // grows whenever something the dashboard shows has changed
void pushEv(const std::string& tag,const std::string& name,const std::string& path);      // rate-capped (30/s); the newest 80 are kept
void pushEvRaw(const std::string& tag,const std::string& name,const std::string& path);   // bypasses the cap (explicit scans)
std::vector<Ev> eventsCopy();
// The shell shows a toast. Called from any thread; red = a warning, else information.
extern std::function<void(const std::string& title,const std::string& text,bool red,bool beep)> g_toastHook;
void showOverlay(const std::string& title,const std::string& text,bool red);

// ---- trust rules ----
bool isExecExt(const std::string& p);
bool userWritable(const std::string& p);
bool isSystemProc(const std::string& p);
std::string signTarget(const std::string& p);      // the .app bundle a path lives in, else the path itself
std::string mainExeOf(const std::string& bundle);  // Contents/MacOS/<the program> of a .app ("" if none)
bool looksLikeProgram(const std::string& path);    // Mach-O or #! script, by its first bytes
bool isSigned(const std::string& path);            // Apple-anchored code signature (Developer ID, App Store, Apple); cached
bool suspiciousExe(const std::string& path);       // user-writable location AND not signed
bool isCredArtifactName(const std::string& p);
bool isStagingDir(const std::string& p);
void signSelfTest();                               // proves codesign works here (one Apple program, one plain file); if not, "unsigned" warnings switch off, loudly
extern std::atomic<bool> g_signUsable;

// ---- watchers ----
void procScan();                                   // every running process now (quick scan, start-up)
void procTick();                                   // processes that have started since the last call
std::set<std::string> scanAutoruns();              // launchd plists in ~/Library/LaunchAgents, /Library/LaunchAgents, /Library/LaunchDaemons
void doScan(bool announce);                        // quick scan: running processes + login-item diff
void tokenGuardTick();
std::vector<std::string> watchedDirs();            // folders FSEvents should watch (those that exist)
void fileEvent(const std::string& path);           // something new appeared at path (FSEvents; any thread)
void fileTick();                                   // handles file events that have been quiet for 1.5 s
void stringsScanAsync(const std::string& path);    // links / commands inside a flagged program (one low-priority thread while the queue is not empty)
bool stringsScanIdle();
extern int g_waitStableMs;                         // how long the strings scan waits for a new file to stop growing (1000; tests: 0)
extern bool g_syncScan;                            // tests: doScan runs the process scan inline
extern std::set<std::string> g_known;              // login-item baseline

// ---- start at login ----
std::string launchAgentPlist(const std::string& exe);      // the LaunchAgent that starts exe with --background
bool isOwnAutostart(const std::string& plistName,const std::string& program,const std::string& exe);
void autostartApply();                             // makes ~/Library/LaunchAgents/com.sentinelwatch.agent.plist follow the toggle (also heals the path after a move)

// ---- quotes and the AFK state ----
struct Afk{ bool on=false; std::string quote; unsigned seq=0; };
void loadContent(const std::string& bundleDir);    // facts / quotes / AFK lines: copied from the app bundle to <data>/content on first run, then read
bool quoteTick(double idleSec);                    // AFK on / off + the quote; true when either changed
Afk afkState();
std::string pickAfkVideo(const std::string& resDir);   // a clip of the user's own in <data>/content/afk, else the app's 4K, else the app's 1080p ("" = none)

// ---- dashboard ----
std::string stateJson(const std::string& display);     // everything the dashboard shows (display = "2560 x 1600", from the shell)
bool cmdToggle(const std::string& key);                // a switch was clicked
void cmdScan();                                        // "Quick scan" (menu or button)
bool cmdRevealOk(const std::string& path);             // path belongs to an event in the feed (so a page cannot ask for any file)

// ---- life cycle ----
bool acquireInstanceLock();                        // false when another copy is running
void releaseInstanceLock();
void appInit(const std::string& bundleDir,bool background);   // log, settings, start-up line, content, autostart, login-item baseline; call once
void engineStart();                                // signature self-test, first process scan, then the 1 s ticker thread
void engineStop();
void testReset();                                  // tests/mac_logic_tests.cpp only: everything back to a fresh start

}
