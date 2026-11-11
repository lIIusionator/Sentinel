// watchers.h -- the background monitors. Each *Thread is a thread entry point started by main.
#pragma once
#include "sw_common.h"

DWORD WINAPI ProcScanThread(LPVOID);      // one-shot: lists processes that are already running
DWORD WINAPI WmiThread(LPVOID);           // live: new processes through the WMI event feed
DWORD WINAPI DirThread(LPVOID param);     // live: new files under a directory (param = new std::wstring(dir))
DWORD WINAPI TokenGuardThread(LPVOID);    // live: unsigned processes holding credential-store handles

extern std::set<std::wstring> g_known;    // autorun baseline
std::set<std::wstring> scanAutoruns();
void doScan(bool announce);               // quick scan: running processes + autorun diff
void autostartApply();                    // "Start with Windows": make the HKCU Run value match the toggle (core/autorun_watch.cpp)
void ecoRecover();                        // start: undo a power-saving plan that a run which did not end cleanly left active (core/eco.cpp)
void ecoTick();                           // about once a second: power saving after 30 min without input, back at the first input (core/eco.cpp)
void ecoStop();                           // closing / sign-out / shutdown: put the user's power plan back

// Automatic updates (core/update.cpp; the rules are shared/update.h). The worker thread does the network work; updateTick and the restart run on the UI thread.
static const wchar_t* const UPD_RESTART_FLAG=L"--after-update";   // an update started this copy: the old one may still be exiting (ui/main.cpp waits for it)
std::wstring ownExe();                    // full path of this exe
void updateStart();                       // start: the worker that looks for a newer release (first look 90 s after start, then twice a day)
void updateTidy();                        // start, once this copy owns the single-instance mutex: remove the leftovers of an update (<exe>.old, <exe>.new)
void updateTick(bool busy);               // about once a second: installs a downloaded update when nothing is in the way (busy: AFK screen or dashboard showing)
void updateCheckNow();                    // "Check now" button / Automatic updates switched on: look at once
std::wstring updateStatus();              // one line for the Updates tab

void blockInFirewall(const std::wstring& path);   // UAC-elevated netsh rule
void stringsScanAsync(const std::wstring& path);  // queue a flagged program for the strings check: links / commands inside it (core/str_scan.cpp)
