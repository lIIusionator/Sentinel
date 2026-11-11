// file_watch.cpp -- new-file watcher (ReadDirectoryChangesW) for AppData, with credential-staging detection.
#include "watchers.h"
#include "events.h"
#include "settings.h"
#include "trust.h"

DWORD WINAPI DirThread(LPVOID param){
  cpuNameThread(L"dir");
  std::wstring dir=*(std::wstring*)param; delete (std::wstring*)param;
  HANDLE h=CreateFileW(dir.c_str(),FILE_LIST_DIRECTORY,
    FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,0,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS,0);
  if(h==INVALID_HANDLE_VALUE)return 0; std::vector<BYTE> buf(64*1024);
  while(g_running.load()){ DWORD ret=0;
    if(!ReadDirectoryChangesW(h,buf.data(),(DWORD)buf.size(),TRUE,
      FILE_NOTIFY_CHANGE_FILE_NAME|FILE_NOTIFY_CHANGE_CREATION,&ret,0,0)){ Sleep(1000); continue; }   // a failing watch must not spin a core
    if(!ret)continue;                                       // the buffer overflowed: nothing to read, watch again
    if(!g_tFile.load())continue;
    for(BYTE* p=buf.data();;){ auto* fni=(FILE_NOTIFY_INFORMATION*)p;
      if(fni->Action==FILE_ACTION_ADDED||fni->Action==FILE_ACTION_RENAMED_NEW_NAME){
        std::wstring rel(fni->FileName,fni->FileNameLength/2), full=dir+L"\\"+rel;
        bool exec=isExecExt(full);
        bool cred=g_tTokens.load()&&isCredArtifactName(full);
        if((g_tAllFiles.load()||exec||cred) && lc(full).find(L"\\sentinelwatch\\")==std::wstring::npos){ g_fileCount++;
          if(cred&&isStagingDir(full)){                 // a copy of a login/token store, dropped where it shouldn't be
            g_tokenAlerts.fetch_add(1);
            pushEv(L"WARN",L"token file staged: "+rel,full);
            showOverlay(L"\u26A0 Credential file copied",rel+L"\n"+full+L"\n(possible token-theft staging)",ACC_RED);
          } else if(!cred || exec){                      // fall back to normal exec handling
            bool susp=exec&&suspiciousExe(full);
            pushEv(susp?L"WARN":L"FILE",rel,full);
            if(susp){ showOverlay(L"\u26A0 New unsigned executable",rel+L"\n"+full,ACC_RED); stringsScanAsync(full); }
          }
        }
      }
      if(!fni->NextEntryOffset)break; p+=fni->NextEntryOffset; }
  }
  CloseHandle(h); return 0;
}
