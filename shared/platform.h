// platform.h -- which OS this is being compiled for.
//
// The Windows build is shared/ core/ ui/ afk/ media-engine/ (MinGW-w64); the macOS build is mac/ (Apple clang). Each tree includes this header, so compiling a
// tree on the wrong OS stops with one clear message instead of a page of "windows.h not found". The Makefile and build.sh pick the right build the same way
// (Windows_NT / Darwin / Linux). Linux is only a host for the native unit tests of the pure logic and for the Windows cross compiler.
#pragma once
#if defined(_WIN32)
  #define SW_OS_WINDOWS 1
  #define SW_OS_MAC     0
  #define SW_OS_NAME    "Windows"
#elif defined(__APPLE__) && defined(__MACH__)
  #define SW_OS_WINDOWS 0
  #define SW_OS_MAC     1
  #define SW_OS_NAME    "macOS"
#elif defined(__linux__)
  #define SW_OS_WINDOWS 0
  #define SW_OS_MAC     0
  #define SW_OS_NAME    "Linux"
#else
  #error "Sentinel Watch builds on Windows (MinGW-w64) and macOS (Apple clang); this OS is not supported."
#endif
