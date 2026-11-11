// proc_watch.cpp -- running-process scan and live WMI process-creation watcher.
#include "watchers.h"
#include "events.h"
#include "settings.h"
#include "trust.h"
#include <tlhelp32.h>
#include <wbemidl.h>
#include <oleauto.h>

// Enumerate CURRENTLY running processes (catches apps started before us:
// Roblox, Explorer, etc.), deduped by name. Runs on its own thread.
DWORD WINAPI ProcScanThread(LPVOID){
  cpuNameThread(L"scan");
  HANDLE snap=CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0);
  if(snap==INVALID_HANDLE_VALUE)return 0;
  PROCESSENTRY32W pe; pe.dwSize=sizeof(pe);
  std::set<std::wstring> seen; int count=0;
  if(Process32FirstW(snap,&pe)){ do{
    count++; std::wstring name=pe.szExeFile, key=lc(name);
    if(seen.count(key))continue; seen.insert(key);
    std::wstring path; HANDLE ph=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pe.th32ProcessID);
    if(ph){ wchar_t buf[MAX_PATH]; DWORD sz=MAX_PATH; if(QueryFullProcessImageNameW(ph,0,buf,&sz))path=buf; CloseHandle(ph); }
    bool susp=suspiciousExe(path);
    pushEvRaw(susp?L"WARN":L"PROC",name,path.empty()?L"(running)":path);
    if(susp) stringsScanAsync(path);
  }while(Process32NextW(snap,&pe)); }
  CloseHandle(snap); g_procCount.store(count); return 0;
}

// ---------- WMI process watch ----------
static std::wstring wStr(IWbemClassObject* o,const wchar_t* p){ VARIANT v; VariantInit(&v);
  std::wstring r; if(o->Get(p,0,&v,0,0)==WBEM_S_NO_ERROR&&v.vt==VT_BSTR&&v.bstrVal)r=v.bstrVal;
  VariantClear(&v); return r; }
DWORD WINAPI WmiThread(LPVOID){
  cpuNameThread(L"wmi");
  if(FAILED(CoInitializeEx(0,COINIT_MULTITHREADED)))return 0;
  IWbemLocator* loc=0;
  if(FAILED(CoCreateInstance(CLSID_WbemLocator,0,CLSCTX_INPROC_SERVER,IID_IWbemLocator,(void**)&loc))){CoUninitialize();return 0;}
  IWbemServices* svc=0; BSTR ns=SysAllocString(L"ROOT\\CIMV2");
  HRESULT hr=loc->ConnectServer(ns,0,0,0,0,0,0,&svc); SysFreeString(ns);
  if(FAILED(hr)){loc->Release();CoUninitialize();return 0;}
  CoSetProxyBlanket(svc,RPC_C_AUTHN_WINNT,RPC_C_AUTHZ_NONE,0,RPC_C_AUTHN_LEVEL_CALL,RPC_C_IMP_LEVEL_IMPERSONATE,0,EOAC_NONE);
  IEnumWbemClassObject* en=0; BSTR lang=SysAllocString(L"WQL");
  BSTR q=SysAllocString(L"SELECT * FROM __InstanceCreationEvent WITHIN 1 WHERE TargetInstance ISA 'Win32_Process'");
  hr=svc->ExecNotificationQuery(lang,q,WBEM_FLAG_FORWARD_ONLY|WBEM_FLAG_RETURN_IMMEDIATELY,0,&en);
  SysFreeString(lang);SysFreeString(q);
  if(FAILED(hr)){svc->Release();loc->Release();CoUninitialize();return 0;}
  bool broken=false;
  while(g_running.load()){ IWbemClassObject* ev=0; ULONG ret=0;
    hr=en->Next(1000,1,&ev,&ret);
    if(FAILED(hr)){ if(!broken){ broken=true; logLine(L"[WMI] process watcher: Next() failed, hr="+hrHex(hr)+L" -- retrying once a second"); }
      Sleep(1000); continue; }                          // a dead subscription answers at once: without the pause this loop would spin a core
    if(hr==WBEM_S_TIMEDOUT||!ret||!ev){ if(ev)ev->Release(); continue; }
    if(g_tProc.load()){ VARIANT vt; VariantInit(&vt);
      if(ev->Get(L"TargetInstance",0,&vt,0,0)==WBEM_S_NO_ERROR&&vt.vt==VT_UNKNOWN&&vt.punkVal){
        IWbemClassObject* ti=0; vt.punkVal->QueryInterface(IID_IWbemClassObject,(void**)&ti);
        if(ti){ std::wstring name=wStr(ti,L"Name"),path=wStr(ti,L"ExecutablePath");
          bool sys=isSystemProc(path);
          if(!(sys&&g_tIgnoreSys.load())){ g_procCount++;
            bool susp=suspiciousExe(path);
            std::wstring shown=path.empty()?L"(no path reported)":path;
            pushEv(susp?L"WARN":L"PROC",name,shown);
            if(susp){ showOverlay(L"\u26A0 Unsigned app in user folder",name+L"\n"+shown,ACC_RED); stringsScanAsync(path); }
          }
          ti->Release();
        } }
      VariantClear(&vt); }
    ev->Release();
  }
  en->Release();svc->Release();loc->Release();CoUninitialize(); return 0;
}
