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

void blockInFirewall(const std::wstring& path);   // UAC-elevated netsh rule
void stringsScanAsync(const std::wstring& path);  // queue a flagged program for the strings check: links / commands inside it (core/str_scan.cpp)
