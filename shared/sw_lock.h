// sw_lock.h -- a CRITICAL_SECTION wrapper.
// std::mutex / std::thread only exist in MinGW's "posix" thread model, and std::shared_ptr is not thread-safe in the "win32"
// model, so the suite uses Win32 primitives and std::atomic (which works in every model) for everything that crosses threads.
#pragma once
#include <windows.h>

struct SwCs{
  CRITICAL_SECTION cs;
  SwCs(){ InitializeCriticalSection(&cs); }
  ~SwCs(){ DeleteCriticalSection(&cs); }
  SwCs(const SwCs&)=delete; SwCs& operator=(const SwCs&)=delete;
  void lock(){ EnterCriticalSection(&cs); }
  void unlock(){ LeaveCriticalSection(&cs); }
};
struct SwGuard{
  SwCs& c;
  explicit SwGuard(SwCs& x):c(x){ c.lock(); }
  ~SwGuard(){ c.unlock(); }
  SwGuard(const SwGuard&)=delete; SwGuard& operator=(const SwGuard&)=delete;
};
