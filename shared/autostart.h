// autostart.h -- "Start with Windows": the value written under HKCU Software\Microsoft\Windows\CurrentVersion\Run, and how the autorun watch recognises its own entry.
// Pure logic (no Windows headers): tests/autostart_tests.cpp.
#pragma once
#include <string>
#include <cwctype>

static const wchar_t* const AUTOSTART_NAME=L"SentinelWatch";
static const wchar_t* const AUTOSTART_FLAG=L"--background";            // "start hidden in the tray": the dashboard is not opened

inline std::wstring autostartValue(const std::wstring& exe){ return L"\""+exe+L"\" "+AUTOSTART_FLAG; }
inline std::wstring autostartLower(std::wstring s){ for(auto& c:s) c=(wchar_t)std::towlower(c); return s; }
// The autorun watch skips exactly this entry: our own name AND our own path (registry value names and Windows paths ignore case).
// The same name pointing anywhere else is somebody imitating us, and is still reported.
inline bool isOwnAutostart(const std::wstring& name,const std::wstring& value,const std::wstring& exe){
  return !exe.empty() && autostartLower(name)==autostartLower(AUTOSTART_NAME) && autostartLower(value)==autostartLower(autostartValue(exe)); }
