// trust.h -- "is this executable / path suspicious" helpers used by every watcher.
#pragma once
#include "sw_common.h"

bool isExecExt(const std::wstring& p);
bool suspPath(const std::wstring& p);
bool isSystemProc(const std::wstring& p);
bool userWritable(const std::wstring& p);
bool isSigned(const std::wstring& path);          // Authenticode, verified offline
bool suspiciousExe(const std::wstring& path);     // user-writable location AND not validly signed
bool isCredArtifactName(const std::wstring& p);   // cookies / login data / leveldb ... file names
bool isStagingDir(const std::wstring& p);         // temp / downloads / desktop
