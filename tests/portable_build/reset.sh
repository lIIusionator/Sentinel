#!/bin/bash
# reset.sh -- fresh state for one scenario: wipes work folders/logs, rewrites the Wine-test copies of the two scripts
SP=/tmp/claude-0/-home-claude/addb7cd1-cad3-5aca-aa5d-af9b82ae52d8/scratchpad; C=/tmp/wineprefix/drive_c
rm -rf $C/SentinelBuild "$C/Sentinel Build (x86)" $C/shim.log $C/t2 $C/t3
python3 $SP/shim/tf.py /home/claude/sentinel-watch/build.bat $C/fake/src/sentinel-watch/build.bat >/dev/null
mkdir -p $C/t1 && python3 $SP/shim/tf.py $SP/bp/build_portable.bat $C/t1/build_portable.bat >/dev/null
cp $SP/out/send/sentinel-watch-src.zip $C/t1/ 2>/dev/null
rm -f $C/t1/sentinel.exe $C/t1/sentinel_built.exe
# clips left by a first run of sentinel.exe, web files, shims
A=$C/users/root/AppData/Local/SentinelWatch/content/afk; mkdir -p $A
[ -f $A/sentinel_afk_4k60.mp4 ] || head -c 1500000 /dev/urandom > $A/sentinel_afk_4k60.mp4
[ -f $A/sentinel_afk_1080p30.mp4 ] || head -c 500000 /dev/urandom > $A/sentinel_afk_1080p30.mp4
Z=$C/fakeweb/winlibs-x86_64-posix-seh-gcc-16.1.0-mingw-w64msvcrt-14.0.0-r4.zip
printf '%s  %s\n' "$(sha256sum $Z | cut -d' ' -f1)" "$(basename $Z)" > $Z.sha256
