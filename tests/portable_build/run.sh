#!/bin/bash
# usage: run.sh <test-dir-in-prefix, e.g. t1> [env assignments...]   -- runs build_portable.bat there under Wine
export WINEPREFIX=/tmp/wineprefix WINEDEBUG=-all LANG=C.UTF-8 LC_ALL=C.UTF-8 WINEDLLOVERRIDES='certutil.exe=n'
export SW_NOPAUSE=1 SHIM_WEB='C:\fakeweb' SHIM_FAKE_TC='C:\fake\mingw64' SHIM_FAKE_SRC='C:\fake\src\sentinel-watch' SHIM_LOG='C:\shim.log'
D=$1; shift
for kv in "$@"; do export "$kv"; done
cd /tmp/wineprefix/drive_c/$D || exit 9
timeout 300 /usr/lib/wine/wine64 cmd /c "C:\\$D\\build_portable.bat" 2>&1 | tr -d '\r'
echo "[exit code: ${PIPESTATUS[0]}]"
/usr/lib/wine/wineserver -k 2>/dev/null
exit 0
