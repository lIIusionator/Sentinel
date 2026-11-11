tests/portable_build -- how build_portable.bat (and build.bat) were exercised without Windows, a network or a real compiler
==========================================================================================================================

Everything runs under Wine on Linux. Real Windows tools are replaced by one small program, shim.c, which behaves according to the
name it is started under:

  curl.exe      "downloads" by copying C:\fakeweb\<file name of the url> to the -o target (fails with 22 when the file is missing)
  tar.exe       "unpacks" by copying a prepared tree (the toolchain for winlibs*.zip, the project for sentinel-watch*.zip)
  certutil.exe  -hashfile <file> SHA256 with a real SHA-256 (so a damaged download is really detected)
  g++.exe       answers --version / -dumpmachine / -dumpversion; "compiles" by checking that the source file and every -I directory exist
                relative to the current directory, and writes a dummy .o; the link step checks the flags (-static -mwindows -municode)
                and that all 24 objects + sentinel.res.o exist (25 in total)
  windres.exe   checks res\sentinel.rc, "-I res" and that the two clips and the avatar are in res\

So the scripts' control flow AND the paths/flags build.bat passes are checked against the real project tree; the compiler itself is not.

Set-up (once):
  x86_64-w64-mingw32-gcc -O1 -static -o shim.exe shim.c
  prefix=/tmp/wineprefix                                            # any wine prefix; C: is $prefix/drive_c
  copy shim.exe to $prefix/drive_c/windows/system32/{curl,tar,certutil}.exe     (certutil needs WINEDLLOVERRIDES='certutil.exe=n',
                                                                                  otherwise Wine runs its own builtin certutil)
  copy shim.exe to C:\fake\mingw64\bin\{g++,gcc,windres}.exe
  C:\fakeweb\winlibs-x86_64-posix-seh-gcc-16.1.0-mingw-w64msvcrt-14.0.0-r4.zip  (any file) + the same name + ".sha256"
        containing "<sha256>  <file name>"
  C:\fake\src\sentinel-watch\   a copy of the clip-less source tree
  %LOCALAPPDATA%\SentinelWatch\content\afk\sentinel_afk_4k60.mp4 and sentinel_afk_1080p30.mp4   (any bytes)
  C:\t1\  holding build_portable.bat and sentinel-watch-src.zip

reset.sh / run.sh in this folder are the helpers that were used (they hard-code the scratch paths; edit the variables at the top).

Wine 9.0's cmd.exe quirk: in `a || b` it runs `b` even when `a` succeeded (check: `ver >nul || echo X` prints X). Windows does not.
tf.py rewrites every `a || goto x` / `a || exit /b n` line into `a` + `if errorlevel 1 goto x` for the Wine runs only; the shipped
scripts keep `||`. Wine's where.exe is also unreliable, so the scripts start the tools (`g++ --version`) instead of asking `where`.

Scenarios that were run (all behaved as intended): first run; re-run (no second download, sentinel_built.exe); damaged download;
no clips (stops before any download) / only one clip; no zip; suitable g++ already on PATH / 32-bit / too old / SW_FORCE_PORTABLE;
project folder with edits (synced into the work copy) and run from inside the work copy; work folder "C:\Sentinel Build (x86)";
tar failing on the source zip / on the toolchain zip; compile error (build.log shown); no curl.exe.

NOT covered: the real download from github.com, Windows' real tar.exe / certutil.exe / curl.exe on the real WinLibs zip, PowerShell fallbacks,
and a real GCC 16.1 compile.
