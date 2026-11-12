#!/bin/bash
# The automatic update, end to end, under Wine: the exe looks for a release on a fake server on this PC (tests/fake_release_server.py), checks the download,
# swaps itself for it and restarts as the new version -- and, in the bad cases, leaves everything as it was.
#
#   usage: tests/update_e2e.sh <old.exe> <new.exe>
#   old.exe  a test build (make TEST=1) -- says v1.0
#   new.exe  the same sources built as v1.0.1 (README section 13):
#            CXXFLAGS='-O2 -DSW_VERSION_OVERRIDE=\"v1.0.1\"' make TEST=1 BUILD=build-test-b OUT=sentinel_test_b.exe
# Needs Wine (wine64), Xvfb and python3. Real HTTPS to GitHub is NOT covered: the fake server speaks plain http (only test builds allow that).
set -u
OLD=$(readlink -f "$1"); NEW=$(readlink -f "$2"); HERE=$(cd "$(dirname "$0")" && pwd)
WINE=${WINE:-/usr/lib/wine/wine64}
T=$(mktemp -d)
export WINEPREFIX=$T/prefix WINEDEBUG=-all LANG=C.UTF-8 LC_ALL=C.UTF-8 DISPLAY=:77
fail=0; XPID=""; SPID=""
ok()  { echo "  ok    $*"; }
bad() { echo "  FAIL  $*"; fail=1; }
sha() { sha256sum "$1" | cut -d' ' -f1; }
wait_for() { local n=$1; shift; for ((i=0;i<n*2;i++)); do if eval "$@"; then return 0; fi; sleep 0.5; done; return 1; }
stop_all() { "${WINESERVER:-/usr/lib/wine/wineserver}" -k >/dev/null 2>&1; [ -n "$SPID" ] && kill "$SPID" 2>/dev/null; SPID=""; sleep 1; }
trap 'stop_all; [ -n "$XPID" ] && kill "$XPID" 2>/dev/null; rm -rf "$T"' EXIT

[ -e /tmp/.X11-unix/X77 ] || { Xvfb :77 -screen 0 1280x800x24 >/dev/null 2>&1 & XPID=$!; sleep 2; }
mkdir -p "$T/prefix"; "$WINE" cmd /c exit >/dev/null 2>&1        # create the prefix once

scenario() {            # scenario <name> <server mode> [off]
  local name=$1 mode=$2 off=${3:-} port=$((8100+RANDOM%800)) app=$T/app srv=$T/srv
  echo "== $name"
  rm -rf "$app" "$srv" "$WINEPREFIX"/drive_c/users/*/AppData/Local/SentinelWatch; mkdir -p "$app" "$srv"
  cp "$OLD" "$app/SentinelWatch.exe"; cp "$NEW" "$srv/SentinelWatch.exe"
  if [ -n "$off" ]; then d=$(echo "$WINEPREFIX"/drive_c/users/*/AppData/Local); mkdir -p "$d/SentinelWatch"; echo '{"update": false}' > "$d/SentinelWatch/settings.json"; fi
  python3 -I "$HERE/fake_release_server.py" "$port" "$srv" "$mode" 2> "$T/server.log" & SPID=$!
  wait_for 10 "(exec 3<>/dev/tcp/127.0.0.1/$port) 2>/dev/null" || bad "the fake server did not start"
  local args=(--background); [ -n "${SHOW:-}" ] && args=()          # SHOW=1: a normal start, the dashboard opens
  SW_TEST_UPDATE_URL="http://127.0.0.1:$port/latest.json" SW_TEST_UPDATE_FIRST=1500 SW_TEST_UPDATE_IDLE=0 \
    "$WINE" "$app/SentinelWatch.exe" "${args[@]}" >/dev/null 2>&1 &
  LOG=; wait_for 20 'LOG=$(ls "$WINEPREFIX"/drive_c/users/*/AppData/Local/SentinelWatch/events.log 2>/dev/null | head -1); [ -n "$LOG" ]' || bad "no events.log: the exe did not start"
}
logs() { grep -cF -- "$1" "$LOG" 2>/dev/null; }
untouched() { [ "$(sha "$T/app/SentinelWatch.exe")" = "$(sha "$OLD")" ] && [ ! -e "$T/app/SentinelWatch.exe.new" ]; }

# 1. the good case: v1.0 -> v1.0.1 and the new copy runs
scenario "newer release is installed" ok
if wait_for 40 '[ "$(logs "Sentinel v1.0.1 build")" -ge 1 ]'; then ok "the restarted copy says v1.0.1"; else bad "no v1.0.1 start in the log"; fi
[ "$(sha "$T/app/SentinelWatch.exe")" = "$(sha "$NEW")" ] && ok "the exe on disk is the new one" || bad "the exe on disk was not replaced"
[ "$(logs "[UPD] installed v1.0.1")" -ge 1 ] && ok "[UPD] installed line logged" || bad "no [UPD] installed line"
[ "$(cat "$(dirname "$LOG")/update_applied.txt" 2>/dev/null | tr -d '\r\n')" = "v1.0.1" ] && ok "update_applied.txt says v1.0.1" || bad "update_applied.txt missing"
wait_for 20 '[ ! -e "$T/app/SentinelWatch.exe.old" ]' && ok "the .old file was removed by the new copy" || bad "SentinelWatch.exe.old is still there"
[ "$(grep -c 'Sentinel v1.0 build' "$LOG")" -eq 1 ] && ok "exactly one v1.0 start (no restart loop)" || bad "unexpected number of v1.0 starts"
stop_all

# 1b. the dashboard is open: the update is downloaded but must wait (nobody is restarted under a window they are looking at)
SHOW=1 scenario "dashboard open: downloaded, installed later" ok
wait_for 30 '[ "$(logs "SHA-256 matches")" -ge 1 ]' && ok "downloaded and checked" || bad "no download line"
sleep 10
[ "$(logs "[UPD] installed")" -eq 0 ] && [ "$(sha "$T/app/SentinelWatch.exe")" = "$(sha "$OLD")" ] && ok "still the old exe while the dashboard is open" || bad "installed under an open dashboard"
stop_all

# 2. the bad cases: nothing changes
scenario "wrong checksum is refused" badsum
wait_for 20 '[ "$(logs "does not match its checksum")" -ge 1 ]' && ok "checksum mismatch logged" || bad "no checksum line"
untouched && ok "exe unchanged, no .new left" || bad "exe changed or .new left behind"; stop_all

scenario "cut-off download is refused" truncate
wait_for 20 '[ "$(logs "check failed")" -ge 1 ]' && ok "failure logged" || bad "no failure line"
untouched && ok "exe unchanged, no .new left" || bad "exe changed or .new left behind"; stop_all

scenario "same version: nothing to do" same
wait_for 20 '[ "$(logs "up to date")" -ge 1 ]' && ok "up to date logged" || bad "no up-to-date line"
grep -q 'SentinelWatch.exe' "$T/server.log" && bad "the exe was downloaded anyway" || ok "no download"
untouched && ok "exe unchanged" || bad "exe changed"; stop_all

scenario "pre-release is ignored" prerelease
wait_for 20 '[ "$(logs "pre-release")" -ge 1 ]' && ok "pre-release refused" || bad "no pre-release line"
untouched && ok "exe unchanged" || bad "exe changed"; stop_all

scenario "repository without a release (404)" notfound
wait_for 20 '[ "$(logs "no published release")" -ge 1 ]' && ok "404 explained in the log" || bad "no 404 line"
untouched && ok "exe unchanged" || bad "exe changed"; stop_all

scenario "switch off: no request at all" ok off
sleep 8
grep -q 'GET' "$T/server.log" && bad "the exe asked the server although automatic updates are off" || ok "no request while off"
untouched && ok "exe unchanged" || bad "exe changed"; stop_all

echo; [ $fail = 0 ] && echo "update e2e: all scenarios passed" || echo "update e2e: FAILED"
exit $fail
