#!/bin/bash
# The Rust daemon end to end under Wine: fake droppers are copied into the watched %APPDATA% (the real file-watch trigger), and the test reads events.log.
#   1. the daemon scans (unpacked from the exe, compared with the resource), and the findings are the right ones, in the right order, also for a >16 MB file
#   2. a damaged or replaced daemon file is overwritten before it runs
#   3. the same findings with the daemon switched off (the check inside the app) -- parity
#   4. a daemon that lies, crashes, hangs, starts a child or eats memory: the app notices, scans itself, and nothing is left running
#   5. "hold" (what the Events tab does after the user says yes): the file is renamed, nothing is deleted
#
#   usage: tests/daemon_e2e.sh <sentinel_test.exe built after `make daemon`>        (make TEST=1; the test hooks SW_TEST_DAEMON* / SW_TEST_HOLD are in it)
# Needs Wine (wine64), Xvfb, mingw-w64 (stand-in programs). Not covered: real Windows (job objects, DETACHED_PROCESS, antivirus), the click in the Events tab (nobody clicks here).
set -u
EXE=$(readlink -f "$1"); HERE=$(cd "$(dirname "$0")" && pwd); ROOT=$(dirname "$HERE")
WINE=${WINE:-/usr/lib/wine/wine64}; DAEMON=$ROOT/build/sentinel-daemon.exe
[ -f "$DAEMON" ] || { echo "run 'make daemon' first (build/sentinel-daemon.exe)"; exit 2; }
T=$(mktemp -d)
export WINEPREFIX=$T/prefix WINEDEBUG=-all LANG=C.UTF-8 LC_ALL=C.UTF-8 DISPLAY=:77
fail=0; XPID=""
ok()  { echo "  ok    $*"; }
bad() { echo "  FAIL  $*"; fail=1; }
wait_for() { local n=$1; shift; for ((i=0;i<n*2;i++)); do if eval "$@"; then return 0; fi; sleep 0.5; done; return 1; }
stop_all() { "${WINESERVER:-/usr/lib/wine/wineserver}" -k >/dev/null 2>&1; sleep 1; }
trap 'stop_all; [ -n "$XPID" ] && kill "$XPID" 2>/dev/null; rm -rf "$T"' EXIT

pgrep -x Xvfb >/dev/null || { rm -f /tmp/.X77-lock /tmp/.X11-unix/X77; Xvfb :77 -screen 0 1280x800x24 >/dev/null 2>&1 & XPID=$!; sleep 2; }
mkdir -p "$T/prefix" "$T/app"; "$WINE" cmd /c exit >/dev/null 2>&1
cp "$EXE" "$T/app/SentinelWatch.exe"
L=$(echo "$WINEPREFIX"/drive_c/users/*/AppData/Local); DATA=$L/SentinelWatch; ROAM=$(echo "$WINEPREFIX"/drive_c/users/*/AppData/Roaming); CACHED=$DATA/cache/sentinel-daemon.exe; LOG=$DATA/events.log

# ---- stand-in daemons (what a broken or hostile daemon could do) ----
mk() { printf '%s\n' "$2" > "$T/$1.c"; x86_64-w64-mingw32-gcc -O1 -s -o "$T/$1.exe" "$T/$1.c" || { echo "cannot build the stand-in $1"; exit 2; }; }
mk lies   '#include <stdio.h>
int main(void){ puts("hello, I am not a daemon"); return 0; }'
mk crash  '#include <stdio.h>
int main(void){ fputs("F -l http://half.example.org/", stdout); fflush(stdout); return 3; }'
mk hang   '#include <windows.h>
int main(void){ Sleep(120000); return 0; }'
mk child  '#include <windows.h>
#include <stdio.h>
#include <fcntl.h>
#include <io.h>
int main(void){ _setmode(1,_O_BINARY); STARTUPINFOA si={sizeof si}; PROCESS_INFORMATION pi; char c[]="cmd.exe /c exit 0"; BOOL ok=CreateProcessA(0,c,0,0,FALSE,CREATE_NO_WINDOW,0,0,&si,&pi);
  printf("F -l http://child-%s.example.org/x\nOK 1 0 1\n",ok?"STARTED":"BLOCKED"); return 0; }'
mk mem    '#include <windows.h>
#include <stdio.h>
#include <fcntl.h>
#include <io.h>
int main(void){ _setmode(1,_O_BINARY); void* p=VirtualAlloc(0,900u<<20,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE); if(p){ ZeroMemory(p,900u<<20); }
  printf("F -l http://memory-%s.example.org/x\nOK 1 0 1\n",p?"GRANTED":"REFUSED"); return 0; }'

# ---- fake droppers ----
mkdrop() {   # mkdrop <file> -- webhook + plain link + encoded PowerShell + a UTF-16 link
  { printf 'MZ\220\0\3\0\0\0noise noise\0https://discord.com/api/webhooks/1/abc\0http://plain.example.com/payload.exe\0powershell -nop -w hidden -enc JABzAD0A\0'
    printf 'h\0t\0t\0p\0:\0/\0/\0w\0i\0d\0e\0.\0e\0x\0a\0m\0p\0l\0e\0.\0o\0r\0g\0/\0z\0\0\0'; } > "$1"; }
mkclean() { printf 'MZ\220\0\3\0\0\0hello world, nothing to see here\0just some text\0' > "$1"; }
mkbig() {    # >16 MB: a link at the start, one at the end, and one in the middle that must not be seen
  head -c 20000000 /dev/zero | tr '\0' '.' > "$1"
  printf '\0http://head.example.net/a\0'  | dd of="$1" bs=1 seek=1000     conv=notrunc 2>/dev/null
  printf '\0http://middle.example.net/b\0' | dd of="$1" bs=1 seek=10000000 conv=notrunc 2>/dev/null
  printf '\0http://tail.example.net/c\0'  | dd of="$1" bs=1 seek=19990000 conv=notrunc 2>/dev/null; }

start_app() {   # start_app [VAR=value ...] -- a fresh data folder, the app in the tray
  stop_all; rm -rf "$DATA" "$ROAM/Drop"* ; mkdir -p "$ROAM"
  [ -n "${PRE:-}" ] && { mkdir -p "$DATA/cache"; printf 'this is not the daemon' > "$CACHED"; }
  env "$@" "$WINE" "$T/app/SentinelWatch.exe" --background >/dev/null 2>&1 &
  wait_for 30 '[ -f "$LOG" ]' || bad "no events.log: the exe did not start"
  sleep 4                                           # the directory watchers need a moment
}
drop() {        # drop <maker> <name>: copy a fresh fake program into the watched folder and wait for the answer in the log
  local mk=$1 name=$2 f="$T/$2"; mkdir -p "$ROAM/Drop"; $mk "$f"; cp "$f" "$ROAM/Drop/$name"
  if ! wait_for 40 'grep -qF "$name" <(grep "\[STR\]" "$LOG")'; then   # (late directory events were seen under Wine once: one retry)
    cp "$f" "$ROAM/Drop/again_$name"; name=again_$name; wait_for 30 'grep -qF "$name" <(grep "\[STR\]" "$LOG")' || return 1; fi
  sleep 1; }                                       # the findings are logged right after the line that names the file
strlines() { grep '\[STR\]' "$LOG" | sed 's/^.*\[STR\] *//' ; }   # what follows "[STR]"
has() { strlines | grep -qF -- "$1"; }
findings() { grep '\[STR\]   ' "$LOG" | sed 's/^.*\[STR\]   //'; }

# ============================================================
echo "== 1. the daemon scans"
start_app
drop mkdrop dropper1.exe
strlines | grep -q 'daemon: .*dropper1.exe scanned' && ok "the log says the daemon scanned it" || bad "no 'daemon: ... scanned' line: $(strlines | head -3)"
strlines | grep -q 'no daemon' && bad "the app fell back: $(strlines | grep 'no daemon')" || ok "no fallback"
cmp -s "$CACHED" "$DAEMON" && ok "cache\\sentinel-daemon.exe is the embedded daemon, byte for byte" || bad "the unpacked daemon differs from build/sentinel-daemon.exe"
EXPECT=$(printf '%s\n' '[!] https://discord.com/api/webhooks/1/abc' 'http://plain.example.com/payload.exe' 'http://wide.example.org/z' 'cmd: powershell -nop -w hidden -enc JABzAD0A')
[ "$(findings)" = "$EXPECT" ] && ok "findings and order: webhook (hot) first, links, then the command; the UTF-16 link found" || bad "findings differ:
$(findings)"
grep -q 'strings: 3 link(s), 1 command(s)' "$LOG" && ok "the STR event counts 3 links, 1 command" || bad "no STR event line"
drop mkclean clean1.exe
strlines | grep -q 'clean1.exe: no links or suspicious commands' && ok "a clean file: said so" || bad "no 'no links' line for the clean file"
drop mkbig big1.exe
strlines | grep -q 'big1.exe' && ok "the >16 MB file was scanned" || bad "no line for the big file"
findings | grep -q 'head.example.net' && findings | grep -q 'tail.example.net' && ok "big file: head and tail read" || bad "big file: head or tail link missing"
findings | grep -q 'middle.example.net' && bad "big file: the skipped middle was read" || ok "big file: the middle is skipped, as designed"
stop_all

echo "== 2. a damaged daemon file is overwritten before it runs"
PRE=1 start_app
drop mkdrop dropper2.exe
cmp -s "$CACHED" "$DAEMON" && ok "the garbage in cache\\sentinel-daemon.exe was replaced" || bad "the damaged file is still there"
strlines | grep -q 'daemon: .*dropper2.exe scanned' && ok "and the scan went through the daemon" || bad "no daemon scan after the repair"
stop_all

echo "== 3. the check inside the app gives the same findings (daemon switched off)"
start_app SW_TEST_DAEMON=off
drop mkdrop dropper3.exe
strlines | grep -q 'no daemon (switched off' && ok "the log says it scanned inside the app" || bad "no 'no daemon' line"
[ "$(findings)" = "$EXPECT" ] && ok "same findings, same order as with the daemon" || bad "findings differ without the daemon:
$(findings)"
drop mkclean clean3.exe; strlines | grep -q 'clean3.exe: no links' && ok "clean file: same answer" || bad "clean file: no answer"
stop_all

echo "== 4. a daemon that misbehaves"
for s in lies crash; do
  start_app "SW_TEST_DAEMON=Z:${T//\//\\}\\$s.exe"
  drop mkdrop "dropper_$s.exe"
  case $s in lies) why="its answer was not valid";; crash) why="it ended with code 3";; esac
  strlines | grep -q "no daemon ($why)" && ok "$s: noticed ($why)" || bad "$s: not noticed: $(strlines | head -3)"
  [ "$(findings)" = "$EXPECT" ] && ok "$s: the app scanned the file itself, same findings" || bad "$s: findings differ:
$(findings)"
  stop_all
done
start_app "SW_TEST_DAEMON=Z:${T//\//\\}\\hang.exe" SW_TEST_DAEMON_WAIT=3000
drop mkdrop dropper_hang.exe
strlines | grep -q 'no daemon (it did not answer in time' && ok "hang: given up after the wait" || bad "hang: not noticed: $(strlines | head -3)"
[ "$(findings)" = "$EXPECT" ] && ok "hang: the app scanned the file itself" || bad "hang: findings differ"
[ "$(ps -eo comm= | grep -c '^hang')" -eq 0 ] && ok "hang: the stuck daemon was killed (nothing left running)" || bad "hang: the stand-in is still running"
stop_all
start_app "SW_TEST_DAEMON=Z:${T//\//\\}\\child.exe"
drop mkdrop dropper_child.exe
if has 'child-BLOCKED'; then ok "job: a daemon cannot start another process"; elif has 'child-STARTED'; then echo "  NOTE  this Wine lets the daemon start a child (its job object does not enforce ActiveProcessLimit); Windows documents it, but it was not run on real Windows"; else bad "child: no answer: $(strlines | head -3)"; fi
stop_all
start_app "SW_TEST_DAEMON=Z:${T//\//\\}\\mem.exe"
drop mkdrop dropper_mem.exe
if has 'memory-REFUSED'; then ok "job: the memory limit holds (900 MB refused)"; elif has 'memory-GRANTED'; then echo "  NOTE  this Wine does not enforce the job's memory limit; Windows documents it, but it was not run on real Windows"; else bad "mem: no answer: $(strlines | head -3)"; fi
stop_all

echo "== 5. hold (what the Events tab does after a yes)"
start_app SW_TEST_HOLD=1
drop mkdrop held1.exe
wait_for 20 '[ ! -e "$ROAM/Drop/held1.exe" ]'
[ ! -e "$ROAM/Drop/held1.exe" ] && [ -f "$ROAM/Drop/held1.exe.sentinel-held" ] && ok "the program was renamed to .sentinel-held" || bad "not renamed: $(ls "$ROAM/Drop")"
cmp -s "$ROAM/Drop/held1.exe.sentinel-held" "$T/held1.exe" && ok "nothing in the file changed (it is the same bytes)" || bad "the held file differs from the original"
grep -q '\[HOLD\].*held1.exe is held now' "$LOG" && ok "[HOLD] line says how to undo" || bad "no [HOLD] line"
stop_all
start_app
drop mkdrop plain1.exe; sleep 2
[ -f "$ROAM/Drop/plain1.exe" ] && ok "without the switch nothing is held" || bad "a file was held by itself"

echo; [ $fail = 0 ] && echo "daemon e2e: all checks passed" || echo "daemon e2e: FAILED"
exit $fail
