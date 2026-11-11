#!/bin/bash
# build.sh -- builds for the OS it runs on (the OS is checked first):
#   macOS  -> SentinelWatch.app, with Apple clang (Xcode command line tools: xcode-select --install)        see mac/mac.mk
#   Linux  -> sentinel.exe, cross-compiled with mingw-w64 (what the reference builds here used)
#   Windows has no bash: use build.bat, or mingw32-make.
cd "$(dirname "$0")" || exit 1
case "$(uname -s)" in
  Darwin) xcode-select -p >/dev/null 2>&1 || { echo "Apple clang is not installed. Run:  xcode-select --install   then run ./build.sh again."; exit 1; }
          exec make -j"$(sysctl -n hw.ncpu)" "$@" ;;
  Linux)  exec make CROSS=x86_64-w64-mingw32- -j"$(nproc)" "$@" ;;
  *)      echo "Unsupported OS: $(uname -s). Windows: build.bat or mingw32-make. macOS / Linux: ./build.sh"; exit 1 ;;
esac
