@echo off
rem build.bat -- builds sentinel.exe with MinGW-w64 (64-bit). Run it from this folder in a shell where g++ and windres are on PATH
rem (for example the MinGW-w64 / winlibs terminal). Needs about 1 GB of RAM for the resource step. "mingw32-make" does the same, faster, after the first build.
setlocal enabledelayedexpansion
set OUT=sentinel.exe
if "%1"=="test" ( set EXTRA=-DSW_TEST_HOOKS & set OUT=sentinel_test.exe )
if not exist res\afk_bg_4k60.mp4 goto :noclips
if not exist res\afk_bg_lite.mp4 goto :noclips
g++ --version >nul 2>nul || goto :nogcc
windres --version >nul 2>nul || goto :nogcc
set FLAGS=-std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter -Wno-cast-function-type -Wno-misleading-indentation -DUNICODE -D_UNICODE -DWINVER=0x0A00 -D_WIN32_WINNT=0x0A00 -Ishared -Icore -Iui -Iafk -Imedia-engine/include -Imedia-engine/src -Ithird_party -Ires %EXTRA%
set SRC=shared\sw_common.cpp shared\settings.cpp core\autorun_watch.cpp core\events.cpp core\file_watch.cpp core\proc_watch.cpp core\response.cpp core\str_scan.cpp core\token_guard.cpp core\trust.cpp ui\content.cpp ui\dashboard.cpp ui\gfx.cpp ui\main.cpp ui\toast.cpp afk\afk_assets.cpp afk\afk_cpu_backend.cpp afk\afk_gpu_backend.cpp afk\afk_loop.cpp afk\afk_overlay_d2d.cpp afk\afk_screen.cpp afk\afk_state.cpp afk\afk_tiers.cpp afk\afk_win.cpp media-engine\src\media_engine.cpp
set LIBS=-lgdiplus -lgdi32 -lole32 -loleaut32 -luuid -lmfplat -lmf -lmfreadwrite -lmfuuid -lwinmm -lshell32 -luser32 -ladvapi32 -lcrypt32 -lwintrust -lsecur32 -lwbemuuid
if not exist build mkdir build
rem everything the compiler says (warnings included) goes to build\build.log; it is shown here when the build fails
if exist build\build.log del build\build.log
set OBJS=
echo [1/3] resources (embeds avatar + the two AFK clips) ...
windres -I res -O coff res\sentinel.rc -o build\sentinel.res.o 2>>build\build.log || goto :fail
for %%f in (%SRC%) do (
  echo [2/3] %%f
  g++ %FLAGS% -c %%f -o build\%%~nf.o 2>>build\build.log || goto :fail
  set OBJS=!OBJS! build\%%~nf.o
)
echo [3/3] linking %OUT% ...
g++ -static -static-libgcc -static-libstdc++ -mwindows -municode -o %OUT% !OBJS! build\sentinel.res.o %LIBS% 2>>build\build.log || goto :fail
echo.
echo Built %OUT%
goto :eof
:nogcc
echo.
echo g++ and windres are not on PATH. Open a MinGW-w64 terminal, or run build_portable.bat, which fetches a portable compiler
echo by itself ^(nothing gets installed^).
exit /b 1
:noclips
echo.
echo The two AFK clips are missing: res\afk_bg_4k60.mp4 and res\afk_bg_lite.mp4.
echo They are not in the source zip (74 MB). See README.md, section "The two clips":
echo copy them out of a sentinel.exe that has run once (%%LOCALAPPDATA%%\SentinelWatch\content\afk) or make them with tools\make_clips.bat.
exit /b 1
:fail
echo.
echo BUILD FAILED. What the compiler said ^(also saved in build\build.log^):
echo.
if exist build\build.log type build\build.log
exit /b 1
