@echo off
rem ======================================================================================================================
rem  build_portable.bat -- builds sentinel.exe on a PC that has NO C++ compiler. Nothing is installed.
rem
rem  1. unpacks sentinel-watch-src.zip (keep the zip next to this file), or, when this file sits inside the unpacked
rem     sentinel-watch folder, builds that folder
rem  2. takes the two AFK video clips from a sentinel.exe that has run once on this PC (they are not in the zip)
rem  3. downloads a portable MinGW-w64 compiler ONCE (WinLibs GCC 16.1.0, a 264 MB zip from GitHub) unless a suitable
rem     g++ is already on PATH
rem  4. runs build.bat and puts sentinel.exe next to this file
rem
rem  Everything is kept in C:\SentinelBuild (about 2 GB). Delete that folder and nothing is left: no installer, no registry
rem  entry, no PATH change. Needs Windows 10/11 (curl.exe and tar.exe come with Windows) and internet on the first run.
rem
rem  Optional environment variables:   SW_BUILD_ROOT=D:\folder   work folder instead of C:\SentinelBuild
rem                                    SW_FORCE_PORTABLE=1       use the portable compiler even if a g++ is installed
rem                                    SW_NOPAUSE=1              do not wait for a key at the end
rem ======================================================================================================================
setlocal EnableExtensions
title Sentinel Watch - build without a compiler

rem ---- the portable compiler: WinLibs GCC 16.1.0, POSIX threads, SEH, MSVCRT (the family this project is built with) ----
set "TC_TAG=16.1.0posix-14.0.0-msvcrt-r4"
set "TC_ZIP=winlibs-x86_64-posix-seh-gcc-16.1.0-mingw-w64msvcrt-14.0.0-r4.zip"
set "TC_URL=https://github.com/brechtsanders/winlibs_mingw/releases/download/%TC_TAG%/%TC_ZIP%"

set "SYS=%SystemRoot%\System32"
set "HERE=%~dp0"
set "ROOT=%SystemDrive%\SentinelBuild"
if defined SW_BUILD_ROOT set "ROOT=%SW_BUILD_ROOT%"
set "ROOT_TRIED="

echo.
echo  Sentinel Watch -- build without installing a compiler
echo  -----------------------------------------------------
echo  Work folder: %ROOT%   ^(delete it to remove everything this script made^)
echo.

rem ---------------------------------------------------------------------------------------------------------- work folder
:make_root
if not exist "%ROOT%\" mkdir "%ROOT%" >nul 2>nul
echo ok> "%ROOT%\write_test.tmp" 2>nul
if exist "%ROOT%\write_test.tmp" goto :root_ok
if defined ROOT_TRIED goto :root_fail
set "ROOT_TRIED=1"
echo Cannot write to %ROOT% -- trying %PUBLIC%\SentinelBuild instead.
set "ROOT=%PUBLIC%\SentinelBuild"
goto :make_root
:root_fail
echo.
echo Cannot create a work folder. Set SW_BUILD_ROOT to a folder you can write to ^(plain letters, no spaces is best^), then run this again.
goto :end_fail
:root_ok
del "%ROOT%\write_test.tmp" >nul 2>nul
set "SRC=%ROOT%\sentinel-watch"

rem ---------------------------------------------------------------------------------------------------------- 1. the source
if exist "%HERE%res\sentinel.rc" if exist "%HERE%build.bat" goto :src_from_folder
set "SRCZIP="
if exist "%HERE%sentinel-watch-src.zip" set "SRCZIP=%HERE%sentinel-watch-src.zip"
if not defined SRCZIP for %%z in ("%HERE%sentinel-watch*.zip") do if not defined SRCZIP set "SRCZIP=%%~fz"
if not defined SRCZIP goto :src_missing
echo [1/5] Unpacking %SRCZIP%
"%SYS%\tar.exe" -xf "%SRCZIP%" -C "%ROOT%"
if errorlevel 1 goto :src_unzip_fail
goto :src_unpacked

:src_from_folder
echo [1/5] Taking the sources from %HERE%
if /i "%HERE%"=="%SRC%\" goto :src_unpacked
if not exist "%SRC%\" mkdir "%SRC%"
for %%d in (afk core media-engine shared third_party tests tools ui res) do if exist "%HERE%%%d\" xcopy "%HERE%%%d" "%SRC%\%%d\" /E /I /Y /Q /D >nul
for %%f in (build.bat Makefile build.sh README.md CLAUDE.md) do if exist "%HERE%%%f" copy /y "%HERE%%%f" "%SRC%\%%f" >nul

:src_unpacked
if not exist "%SRC%\build.bat" goto :src_layout_fail
if not exist "%SRC%\res\sentinel.rc" goto :src_layout_fail

rem ---------------------------------------------------------------------------------------------------------- 2. the clips
set "CLIPS=%LOCALAPPDATA%\SentinelWatch\content\afk"
if not exist "%SRC%\res\afk_bg_4k60.mp4" if exist "%CLIPS%\sentinel_afk_4k60.mp4" copy /y "%CLIPS%\sentinel_afk_4k60.mp4" "%SRC%\res\afk_bg_4k60.mp4" >nul
if not exist "%SRC%\res\afk_bg_lite.mp4" if exist "%CLIPS%\sentinel_afk_1080p30.mp4" copy /y "%CLIPS%\sentinel_afk_1080p30.mp4" "%SRC%\res\afk_bg_lite.mp4" >nul
if not exist "%SRC%\res\afk_bg_4k60.mp4" goto :clips_missing
if not exist "%SRC%\res\afk_bg_lite.mp4" goto :clips_missing
echo [2/5] The two AFK video clips are in place.

rem ---------------------------------------------------------------------------------------------------------- 3. a compiler
set "TCROOT=%ROOT%\toolchain"
set "TCBIN=%TCROOT%\mingw64\bin"
if exist "%TCBIN%\g++.exe" if exist "%TCBIN%\windres.exe" goto :cc_portable
if defined SW_FORCE_PORTABLE goto :cc_get
call :try_system_gcc
if defined SYSGCC_OK goto :cc_ready
goto :cc_get

:try_system_gcc
set "SYSGCC_OK="
g++ --version >nul 2>nul || exit /b 1
windres --version >nul 2>nul || exit /b 1
set "GMACH="
set "GVER="
for /f "delims=" %%m in ('g++ -dumpmachine 2^>nul') do set "GMACH=%%m"
for /f "delims=." %%v in ('g++ -dumpversion 2^>nul') do set "GVER=%%v"
echo %GMACH%| "%SYS%\find.exe" "x86_64" >nul || exit /b 1
echo %GMACH%| "%SYS%\find.exe" "mingw" >nul || exit /b 1
if not defined GVER exit /b 1
set /a GVN=%GVER% 2>nul
if %GVN% LSS 10 exit /b 1
set "SYSGCC_OK=1"
exit /b 0

:cc_get
echo [3/5] Getting a portable MinGW-w64 compiler: WinLibs GCC 16.1.0, 264 MB, one time only ^(no usable 64-bit g++ was found^).
echo       ^(needs about 2 GB free on %ROOT:~0,2% -- and a few minutes^)
set "CURL=%SYS%\curl.exe"
set "TCZIP=%ROOT%\%TC_ZIP%"
if exist "%TCZIP%" goto :cc_have_zip
if not exist "%CURL%" goto :dl_powershell
echo       downloading from github.com ...
"%CURL%" -L --fail --ssl-no-revoke --connect-timeout 30 --speed-limit 1000 --speed-time 60 --retry 5 --retry-delay 3 -C - -o "%TCZIP%.part" "%TC_URL%"
if errorlevel 33 if not errorlevel 34 goto :dl_done
if errorlevel 1 goto :dl_fail
goto :dl_done
:dl_powershell
echo       curl.exe is not available -- using PowerShell instead ^(no progress display^) ...
"%SYS%\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -ExecutionPolicy Bypass -Command "$ProgressPreference='SilentlyContinue'; [Net.ServicePointManager]::SecurityProtocol=[Net.SecurityProtocolType]::Tls12; Invoke-WebRequest -UseBasicParsing -Uri '%TC_URL%' -OutFile '%TCZIP%.part'"
if errorlevel 1 goto :dl_fail
:dl_done
move /y "%TCZIP%.part" "%TCZIP%" >nul
if not exist "%TCZIP%" goto :dl_fail

rem ---- check the download against the .sha256 file that sits next to it on GitHub ----
:cc_have_zip
set "WANT="
set "GOT="
if exist "%TCZIP%.sha256" del "%TCZIP%.sha256" >nul 2>nul
if exist "%CURL%" "%CURL%" -L --fail -s --ssl-no-revoke --retry 3 -o "%TCZIP%.sha256" "%TC_URL%.sha256"
if exist "%TCZIP%.sha256" for /f "tokens=1" %%h in ('type "%TCZIP%.sha256"') do if not defined WANT set "WANT=%%h"
if not defined WANT goto :hash_skip
for /f "skip=1 tokens=*" %%h in ('%SYS%\certutil.exe -hashfile "%TCZIP%" SHA256 2^>nul') do if not defined GOT set "GOT=%%h"
if defined GOT set "GOT=%GOT: =%"
if not defined GOT goto :hash_skip
if /i "%GOT%"=="%WANT%" goto :hash_ok
echo.
echo The downloaded file is damaged ^(checksum differs^).
echo   expected %WANT%
echo   got      %GOT%
echo It was deleted. Run this file again to download it again.
del "%TCZIP%" >nul 2>nul
del "%TCZIP%.sha256" >nul 2>nul
goto :end_fail
:hash_skip
echo       ^(could not check the checksum -- continuing; the download came over https^)
goto :cc_unpack
:hash_ok
echo       checksum OK.

:cc_unpack
echo       unpacking the compiler -- this takes a few minutes, please wait ...
if not exist "%TCROOT%\" mkdir "%TCROOT%"
if exist "%SYS%\tar.exe" "%SYS%\tar.exe" -xf "%TCZIP%" -C "%TCROOT%"
if exist "%SYS%\tar.exe" if not errorlevel 1 goto :unpack_done
echo       tar did not work -- trying PowerShell ...
"%SYS%\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -ExecutionPolicy Bypass -Command "Expand-Archive -LiteralPath '%TCZIP%' -DestinationPath '%TCROOT%' -Force"
:unpack_done
if exist "%TCBIN%\g++.exe" goto :unpack_ok
rem the archive layout is not the usual one: look for g++.exe anywhere below the toolchain folder
set "GXX="
for /f "delims=" %%g in ('dir /s /b "%TCROOT%\g++.exe" 2^>nul') do if not defined GXX set "GXX=%%g"
if not defined GXX goto :unpack_fail
for %%g in ("%GXX%") do set "TCBIN=%%~dpg"
:unpack_ok
del "%TCZIP%" >nul 2>nul
del "%TCZIP%.sha256" >nul 2>nul

:cc_portable
set "PATH=%TCBIN%;%PATH%"
:cc_ready
g++ --version >nul 2>nul || goto :cc_broken
windres --version >nul 2>nul || goto :cc_broken
set "GVL="
for /f "delims=" %%v in ('g++ --version 2^>nul') do if not defined GVL set "GVL=%%v"
if not defined GVL goto :cc_broken
echo [3/5] Compiler: %GVL%

rem ---------------------------------------------------------------------------------------------------------- 4. build
echo [4/5] Building sentinel.exe -- a few minutes; the screen stays quiet while the 77 MB of video is embedded.
pushd "%SRC%"
if exist sentinel.exe del sentinel.exe
call build.bat
popd
if not exist "%SRC%\sentinel.exe" goto :build_fail

rem ---------------------------------------------------------------------------------------------------------- 5. result
set "OUTEXE=%HERE%sentinel.exe"
if exist "%OUTEXE%" set "OUTEXE=%HERE%sentinel_built.exe"
copy /y "%SRC%\sentinel.exe" "%OUTEXE%" >nul
if errorlevel 1 goto :copy_fail
echo.
echo [5/5] DONE.
for %%f in ("%OUTEXE%") do echo       %%~ff   ^(%%~zf bytes^)
echo       The sources and the compiler stay in %ROOT% so the next build is quick.
echo       To remove everything: delete that folder.
goto :end_ok

rem ---------------------------------------------------------------------------------------------------------- problems
:src_missing
echo.
echo Cannot find sentinel-watch-src.zip. Put it in the same folder as this file:
echo   %HERE%
echo ^(or run this file from inside the unpacked sentinel-watch folder^).
goto :end_fail
:src_unzip_fail
echo.
echo Could not unpack %SRCZIP%
echo Windows 10/11 has tar.exe built in; on an older Windows unpack the zip yourself and run this file from the sentinel-watch folder.
goto :end_fail
:src_layout_fail
echo.
echo %SRC% does not look like the Sentinel Watch sources ^(build.bat or res\sentinel.rc missing^).
goto :end_fail
:clips_missing
echo.
echo The two AFK video clips are missing. They are built INTO sentinel.exe but are not inside the source zip ^(74 MB^).
echo.
echo   Easiest: start sentinel.exe once and wait about 30 seconds -- it unpacks the clips to
echo            %CLIPS%
echo            Close it, then run this file again. It copies them from there by itself.
echo   Or:      put your own afk_bg_4k60.mp4 and afk_bg_lite.mp4 into %SRC%\res
echo            ^(tools\make_clips.bat makes them from any video; see README.md, "The two clips"^).
goto :end_fail
:dl_fail
echo.
echo The download did not work. Download this file in your browser instead:
echo   %TC_URL%
echo and save it, without renaming it, as:
echo   %TCZIP%
echo Then run this file again -- it uses that file instead of downloading.
goto :end_fail
:unpack_fail
echo.
echo Could not unpack the compiler into %TCROOT%. The disk may be full ^(it needs about 2 GB^).
echo The downloaded file was kept: %TCZIP%
echo Free some space, delete %TCROOT% ^(and that zip if it keeps failing^) and run this file again.
goto :end_fail
:cc_broken
echo.
echo The compiler in %TCBIN% does not start. An antivirus may have quarantined part of it.
echo Delete %TCROOT% and run this file again.
goto :end_fail
:build_fail
echo.
echo The build failed. The compiler's messages are above and in
echo   %SRC%\build\build.log
echo Send me that file and I will fix it.
goto :end_fail
:copy_fail
echo.
echo Built, but could not copy sentinel.exe next to this file. It is here:
echo   %SRC%\sentinel.exe
goto :end_fail

:end_ok
if not defined SW_NOPAUSE pause
exit /b 0
:end_fail
if not defined SW_NOPAUSE pause
exit /b 1
