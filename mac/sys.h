// sys.h -- what the Mac app needs from the operating system, as one interface. The portable logic (core.cpp) only ever talks to this, so it runs and is tested
// on any host against a fake (tests/mac_logic_tests.cpp); sys_mac.cpp (libproc, posix_spawn) and apple.mm (CoreGraphics, Foundation) are the real one.
#pragma once
#include <stdint.h>
#include <string>
#include <vector>

namespace sw{

struct ProcInfo{ int pid=0; std::string name, path; };          // path is "" when the OS would not tell

struct Sys{
  virtual ~Sys(){}
  virtual std::vector<int> pids()=0;                              // every process id
  virtual bool procInfo(int pid,ProcInfo& out)=0;                 // false when the process is gone; a process the OS will not describe comes back with an empty path
  virtual std::vector<std::string> openFiles(int pid)=0;          // files the process holds open right now ({} when it may not be asked)
  virtual int codesign(const std::string& path)=0;                // 0 = signed by an Apple-anchored identity (Developer ID, App Store, Apple), 1 = not, -1 = could not be checked
  virtual bool adhocSignedCopy(const std::string& src,const std::string& dst)=0;   // makes dst, a copy of src signed ad hoc (valid, but not by Apple): the control for codesign()
  virtual std::string plistProgram(const std::string& plist)=0;   // the program a launchd plist starts ("" if unreadable)
  virtual double idleSeconds()=0;                                 // since the last keyboard / mouse input
  virtual bool screenLocked()=0;
};
extern Sys* g_sys;                                                // set once at start-up; never null while the watchers run
Sys* newMacSys();                                                 // the real one (sys_mac.cpp + apple.mm; Mac builds only); the tests set g_sys to a fake instead

}
