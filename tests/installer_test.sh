#!/bin/bash
# The Windows setup under Wine, silent (/S): it installs over a running copy and leftovers of an update, writes the start menu link and the uninstall entry,
# and the uninstaller takes all of it away again (and leaves the data folder alone, because a silent uninstall never asks).
#
#   usage: tests/installer_test.sh
# Needs makensis, mingw-w64 (x86_64-w64-mingw32-gcc), Wine with 32-bit support (wine64 and wine32: the setup is a 32-bit program) and Xvfb. It builds two tiny stand-in programs and a setup around the second one, so the real exe is not needed.
# Not covered: the setup's pages and the "delete the data folder?" question (nobody clicks here), SmartScreen, a real Windows start menu.
set -u
HERE=$(cd "$(dirname "$0")" && pwd); ROOT=$(dirname "$HERE")
WINE=${WINE:-/usr/lib/wine/wine64}; WINE32=${WINE32:-/usr/lib/wine/wine}
[ -x "$WINE32" ] || { echo "needs wine32 (the 32-bit Wine): the setup is a 32-bit program"; exit 2; }
T=$(mktemp -d)
export WINEPREFIX=$T/prefix WINEDEBUG=-all LANG=C.UTF-8 LC_ALL=C.UTF-8 DISPLAY=:77
fail=0; XPID=""
ok()  { echo "  ok    $*"; }
bad() { echo "  FAIL  $*"; fail=1; }
wait_for() { local n=$1; shift; for ((i=0;i<n*2;i++)); do if eval "$@"; then return 0; fi; sleep 0.5; done; return 1; }
running() { ps -eo comm= | grep -c '^SentinelWatch'; }
trap '"${WINESERVER:-/usr/lib/wine/wineserver}" -k >/dev/null 2>&1; [ -n "$XPID" ] && kill "$XPID" 2>/dev/null; rm -rf "$T"' EXIT

[ -e /tmp/.X11-unix/X77 ] || { Xvfb :77 -screen 0 1280x800x24 >/dev/null 2>&1 & XPID=$!; sleep 2; }
for v in A B; do printf '#include <windows.h>\nint main(void){ Sleep(120000+%d); return 0; }\n' "$([ $v = A ] && echo 0 || echo 1)" > "$T/$v.c"; x86_64-w64-mingw32-gcc -O1 -s -o "$T/$v.exe" "$T/$v.c" || { echo "cannot build the stand-ins"; exit 2; }; done
makensis -V2 -DEXE="$T/B.exe" -DVERSION=1.0 -DOUT="$T/SentinelWatch-Setup.exe" "$ROOT/installer/windows/sentinel.nsi" || { echo "makensis failed"; exit 2; }
mkdir -p "$T/prefix"; "$WINE" cmd /c exit >/dev/null 2>&1
L=$(echo "$WINEPREFIX"/drive_c/users/*/AppData/Local); D=$L/Programs/SentinelWatch; DATA=$L/SentinelWatch
SM=$(echo "$WINEPREFIX"/drive_c/users/*/AppData/Roaming/Microsoft/Windows/Start\ Menu/Programs)
UKEY='HKCU\Software\Microsoft\Windows\CurrentVersion\Uninstall\SentinelWatch'; RKEY='HKCU\Software\Microsoft\Windows\CurrentVersion\Run'

echo "== an older copy runs, with the leftovers of an update next to it"
mkdir -p "$D" "$DATA"; cp "$T/A.exe" "$D/SentinelWatch.exe"; cp "$T/A.exe" "$D/SentinelWatch.exe.old"; cp "$T/A.exe" "$D/SentinelWatch.exe.new"; echo '{}' > "$DATA/settings.json"
"$WINE" reg add "$RKEY" /v SentinelWatch /d '"x" --background' /f >/dev/null 2>&1
"$WINE" "$D/SentinelWatch.exe" >/dev/null 2>&1 & wait_for 20 '[ "$(running)" -ge 1 ]' && ok "the old copy is running" || bad "the stand-in did not start"

echo "== install"
"$WINE32" "$T/SentinelWatch-Setup.exe" /S >/dev/null 2>&1
cmp -s "$D/SentinelWatch.exe" "$T/B.exe" && ok "the program file is the one in the setup" || bad "the program file was not replaced"
[ "$(running)" -eq 0 ] && ok "the running copy was closed" || bad "the old copy still runs"
[ ! -e "$D/SentinelWatch.exe.old" ] && [ ! -e "$D/SentinelWatch.exe.new" ] && ok "the leftovers of the update are gone" || bad ".old / .new are still there"
[ -f "$D/Uninstall.exe" ] && ok "Uninstall.exe written" || bad "no Uninstall.exe"
[ -f "$SM/Sentinel Watch.lnk" ] && ok "start menu link written" || bad "no start menu link"
"$WINE" reg query "$UKEY" /v DisplayVersion 2>/dev/null | grep -q '1\.0' && ok "uninstall entry has the version" || bad "uninstall entry missing"
"$WINE" reg query "$UKEY" /v QuietUninstallString 2>/dev/null | grep -qi 'Uninstall.exe.* /S' && ok "quiet uninstall string set" || bad "no quiet uninstall string"
[ "$("$WINE" reg query "$RKEY" /v SentinelWatch >/dev/null 2>&1; echo $?)" = 0 ] && ok "the setup leaves the Run value alone (the program manages it)" || bad "the setup removed the Run value"

echo "== uninstall"
"$WINE32" "$D/Uninstall.exe" /S >/dev/null 2>&1
wait_for 30 '[ ! -e "$D" ]' && ok "program folder removed" || bad "program folder is still there: $(ls "$D" 2>&1 | tr '\n' ' ')"
[ ! -f "$SM/Sentinel Watch.lnk" ] && ok "start menu link removed" || bad "link still there"
"$WINE" reg query "$UKEY" >/dev/null 2>&1 && bad "uninstall entry still there" || ok "uninstall entry removed"
"$WINE" reg query "$RKEY" /v SentinelWatch >/dev/null 2>&1 && bad "the Run value is still there" || ok "the Run value (start with Windows) removed"
[ -f "$DATA/settings.json" ] && ok "data folder kept (silent uninstall does not ask)" || bad "data folder was deleted without asking"

echo; [ $fail = 0 ] && echo "installer test: all checks passed" || echo "installer test: FAILED"
exit $fail
