; sentinel.nsi -- the Windows setup of Sentinel Watch (NSIS 3; makensis runs on Linux and on Windows).
;
;   makensis -DEXE=<SentinelWatch.exe> -DVERSION=1.0 -DOUT=<SentinelWatch-Setup.exe> installer/windows/sentinel.nsi
;
; For the current user only: no administrator rights. The program goes to %LOCALAPPDATA%\Programs\SentinelWatch because it updates itself by replacing its own file
; (README section 13), and a folder under C:\Program Files would not let it.
; shortcut: one fixed folder, no folder or component page; add pages when somebody needs another folder.
; shortcut: the program has no icon resource yet, so the shortcut shows the plain exe icon.

Unicode true
ManifestDPIAware true
!include "MUI2.nsh"

!ifndef EXE
  !error "Pass -DEXE=<path of SentinelWatch.exe>"
!endif
!ifndef VERSION
  !define VERSION "dev"
!endif
!ifndef OUT
  !define OUT "SentinelWatch-Setup.exe"
!endif

!define NAME "Sentinel Watch"
!define APP  "SentinelWatch.exe"
!define KEY  "Software\Microsoft\Windows\CurrentVersion\Uninstall\SentinelWatch"
!define RUN  "Software\Microsoft\Windows\CurrentVersion\Run"      ; the "start with Windows" value the program writes itself (AUTOSTART_NAME in shared/autostart.h)

Name "${NAME}"
OutFile "${OUT}"
InstallDir "$LOCALAPPDATA\Programs\SentinelWatch"
RequestExecutionLevel user
BrandingText "${NAME} ${VERSION}"
; zlib (the default) on purpose: nearly all of the exe is two video clips, which do not compress, so lzma would only take longer

!define MUI_ABORTWARNING
!define MUI_FINISHPAGE_RUN "$INSTDIR\${APP}"
!define MUI_FINISHPAGE_RUN_TEXT "Start ${NAME} now"
!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"

Section "Install"
  SetOutPath "$INSTDIR"
  ; A running copy (also one the program started by itself after an update) keeps its file locked. taskkill fails when nothing runs: that is fine.
  ; shortcut: a fixed wait after taskkill; make it a loop if a slow PC still holds the file.
  nsExec::Exec 'taskkill /F /IM ${APP}'
  Pop $0
  Sleep 800
  Delete "$INSTDIR\${APP}.old"
  Delete "$INSTDIR\${APP}.new"
  File "/oname=${APP}" "${EXE}"
  WriteUninstaller "$INSTDIR\Uninstall.exe"
  CreateShortCut "$SMPROGRAMS\${NAME}.lnk" "$INSTDIR\${APP}"
  WriteRegStr HKCU "${KEY}" "DisplayName" "${NAME}"
  WriteRegStr HKCU "${KEY}" "DisplayVersion" "${VERSION}"
  WriteRegStr HKCU "${KEY}" "Publisher" "lIIusionator"
  WriteRegStr HKCU "${KEY}" "URLInfoAbout" "https://github.com/lIIusionator/Sentinel"
  WriteRegStr HKCU "${KEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKCU "${KEY}" "DisplayIcon" "$INSTDIR\${APP}"
  WriteRegStr HKCU "${KEY}" "UninstallString" '"$INSTDIR\Uninstall.exe"'
  WriteRegStr HKCU "${KEY}" "QuietUninstallString" '"$INSTDIR\Uninstall.exe" /S'
  WriteRegDWORD HKCU "${KEY}" "NoModify" 1
  WriteRegDWORD HKCU "${KEY}" "NoRepair" 1
SectionEnd

Section "Uninstall"
  nsExec::Exec 'taskkill /F /IM ${APP}'
  Pop $0
  Sleep 800
  DeleteRegValue HKCU "${RUN}" "SentinelWatch"
  Delete "$SMPROGRAMS\${NAME}.lnk"
  Delete "$INSTDIR\${APP}"
  Delete "$INSTDIR\${APP}.old"
  Delete "$INSTDIR\${APP}.new"
  Delete "$INSTDIR\Uninstall.exe"
  RMDir "$INSTDIR"                                   ; not /r: only when it is empty, whatever folder this was
  DeleteRegKey HKCU "${KEY}"
  ; The data folder holds settings.json, events.log and the two unpacked clips (78 MB): ask. A silent uninstall keeps it.
  IfSilent done
  MessageBox MB_YESNO|MB_ICONQUESTION "Also delete the settings, the log and the unpacked video clips in $LOCALAPPDATA\SentinelWatch?" IDNO done
  RMDir /r "$LOCALAPPDATA\SentinelWatch"
  done:
SectionEnd
