// settings.h -- protection toggles, window size and AFK-screen preferences, persisted as JSON.
#pragma once
#include "sw_common.h"

struct Toggle{ std::atomic<bool>* v; const char* key; const wchar_t* name; const wchar_t* desc; };
static const int TOG_N=11;
extern Toggle g_toggles[TOG_N];
extern std::atomic<bool> g_tAutorun,g_tProc,g_tFile,g_tAllFiles,g_tNotify,g_tBeep,g_tIgnoreSys,g_tTokens,g_tStrings,g_tAutostart,g_tEco;

static const int DASH_W=940, DASH_H=600;        // default / initial dashboard size
static const int DASH_MINW=780, DASH_MINH=560;  // smallest the layout stays intact at
extern int g_dashW,g_dashH;                     // current size (resizable + persisted)
extern int g_splashMs;

// AFK screen. "afkMode" in settings.json: "auto" (default) | "gpu" | "cpu".
//   auto : 4K GPU player -> 1080p GPU player -> CPU renderer, falling down the ladder if a tier fails here
//   gpu  : never use the CPU renderer     cpu : force the old GDI+ renderer
// "afkMinTier" remembers the best tier that has not failed on this machine (0 = 4K GPU, 1 = lite GPU, 2 = CPU).
// "afkStrikes" / "afkMinTierAt" are the bookkeeping behind it (see AfkTierMemory in afk_helpers.h): a slow verdict needs two
// sessions in a row, a hard failure counts at once, and any verdict expires after a week.
// Everything resets whenever the build changes ("afkBuild"), so a fixed build gets a fresh chance.
enum AfkModePref { AFK_AUTO=0, AFK_GPU=1, AFK_CPU=2 };
extern std::atomic<int> g_afkModePref;
// "afkIdleSec" in settings.json: 0 (default) = go AFK after the Windows screen-saver wait if one is enabled, else 5 minutes;
// 5..3600 = go AFK after that many idle seconds (handy for testing: 10 shows the screen after ten quiet seconds).
extern std::atomic<int> g_afkIdleSec;
// "afkGpu" in settings.json: "default" | "intel" | "nvidia" | "amd" (AfkGpuPref in afk_helpers.h). On a PC with two GPUs (Optimus laptops) this picks the
// adapter the video player runs on; "default" lets Windows choose. Changing it makes the app forget its remembered tier verdicts and try every tier again.
extern std::atomic<int> g_afkGpuPref;
extern std::atomic<int> g_afkMinTier;
extern std::atomic<int> g_afkStrikes;              // consecutive slow sessions at the current tier (two make a verdict permanent)
extern std::atomic<long long> g_afkMinTierAt;      // unix time g_afkMinTier was last raised; the verdict expires after a week

std::wstring baseFile();        // autorun baseline
std::wstring settingsPath();
void saveSettings();            // thread-safe
void loadSettings();
