// response.cpp -- the one active response the suite offers: block an app in Windows Firewall (UAC prompt).
#include "watchers.h"
#include <shellapi.h>

void blockInFirewall(const std::wstring& path){
  if(path.empty()||path==L"(running)"||path==L"(no path reported)")return;
  std::wstring pr=L"/c netsh advfirewall firewall add rule name=\"Sentinel Block Out\" dir=out action=block program=\""+path+L"\" enable=yes"
                 L" & netsh advfirewall firewall add rule name=\"Sentinel Block In\" dir=in action=block program=\""+path+L"\" enable=yes";
  ShellExecuteW(0,L"runas",L"cmd.exe",pr.c_str(),0,SW_HIDE);   // UAC prompt -> adds real firewall rules
}
