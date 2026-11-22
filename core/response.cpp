// response.cpp -- the active responses the suite offers, each one only after the user says yes in the dashboard: block an app in Windows Firewall (UAC prompt), hold a program's file.
#include "watchers.h"
#include "events.h"
#include <shellapi.h>

void blockInFirewall(const std::wstring& path){
  if(path.empty()||path==L"(running)"||path==L"(no path reported)")return;
  std::wstring pr=L"/c netsh advfirewall firewall add rule name=\"Sentinel Block Out\" dir=out action=block program=\""+path+L"\" enable=yes"
                 L" & netsh advfirewall firewall add rule name=\"Sentinel Block In\" dir=in action=block program=\""+path+L"\" enable=yes";
  ShellExecuteW(0,L"runas",L"cmd.exe",pr.c_str(),0,SW_HIDE);   // UAC prompt -> adds real firewall rules
}

// Hold: the Events tab offers it for a program the strings scan found links / commands in. The file is renamed to <name>.sentinel-held in its own folder: a double click, a Run
// entry or a scheduled task can no longer start it, and renaming it back undoes it. Nothing is deleted, copied or sent anywhere. A copy that is running keeps running (Windows lets a
// running program be renamed); the log line says so. shortcut: no list of released files, so if the file is renamed back and starts again, a new scan can offer to hold it again.
bool holdFile(const std::wstring& path){
  if(path.empty()||path[0]==L'('||GetFileAttributesW(path.c_str())==INVALID_FILE_ATTRIBUTES) return false;
  std::wstring to=path+L".sentinel-held";
  for(int i=2;i<100&&GetFileAttributesW(to.c_str())!=INVALID_FILE_ATTRIBUTES;i++) to=path+L".sentinel-held"+std::to_wstring(i);   // never over a file that is there
  const bool ok=MoveFileExW(path.c_str(),to.c_str(),0)!=0; const DWORD err=GetLastError();
  if(ok){ logLine(L"[HOLD] "+path+L" is held now: renamed to "+to+L" -- rename it back to let it run (a copy that is already running keeps running)");
          pushEv(L"SYS",L"held: "+path.substr(path.find_last_of(L'\\')+1),to); }
  else logLine(L"[HOLD] could not rename "+path+L" (error "+std::to_wstring(err)+L")");
  return ok;
}
