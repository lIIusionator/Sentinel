// sys_mac.cpp -- sw::Sys on a Mac, libSystem only: libproc (processes and the files they hold open) and posix_spawn + /usr/bin/codesign (signatures).
// Idle time, the lock screen and launchd plists need CoreGraphics / Foundation: apple.mm, reached through apple.h.
// Never run on a Mac by whoever wrote it: README section 12 lists what to look at in events.log on the first run.
#include "sys.h"
#include "apple.h"
#include <chrono>
#include <filesystem>
#include <thread>
#include <crt_externs.h>
#include <errno.h>
#include <fcntl.h>
#include <libproc.h>
#include <signal.h>
#include <spawn.h>
#include <string.h>
#include <sys/proc_info.h>
#include <sys/wait.h>
#include <unistd.h>

namespace sw{

// Runs /usr/bin/codesign with its output thrown away; its exit status, or -1 when it could not be run, did not end on its own within timeoutSec, or died on a signal.
static int runCodesign(const std::vector<std::string>& args,int timeoutSec){
  std::vector<char*> argv; std::string prog="codesign"; argv.push_back(&prog[0]);
  std::vector<std::string> a(args); for(std::string& s:a) argv.push_back(&s[0]); argv.push_back(nullptr);
  posix_spawn_file_actions_t fa; if(posix_spawn_file_actions_init(&fa)!=0) return -1;
  posix_spawn_file_actions_addopen(&fa,1,"/dev/null",O_WRONLY,0); posix_spawn_file_actions_addopen(&fa,2,"/dev/null",O_WRONLY,0);
  pid_t pid=0; const int rc=posix_spawn(&pid,"/usr/bin/codesign",&fa,nullptr,argv.data(),*_NSGetEnviron()); posix_spawn_file_actions_destroy(&fa);
  if(rc!=0) return -1;
  for(int i=0;i<timeoutSec*50;i++){
    int st=0; const pid_t r=waitpid(pid,&st,WNOHANG);
    if(r==pid) return WIFEXITED(st)?WEXITSTATUS(st):-1;
    if(r<0&&errno!=EINTR) return -1;
    std::this_thread::sleep_for(std::chrono::milliseconds(20)); }
  kill(pid,SIGKILL); int st=0; waitpid(pid,&st,0); return -1; }

struct MacSys: Sys{
  std::vector<int> pids() override{
    const int n0=proc_listallpids(nullptr,0); if(n0<=0) return {};
    std::vector<pid_t> v((size_t)n0+64);                                   // room for processes that start between the two calls
    const int n=proc_listallpids(v.data(),(int)(v.size()*sizeof(pid_t))); if(n<=0) return {};
    std::vector<int> out; for(int i=0;i<n&&i<(int)v.size();i++) if(v[(size_t)i]>0) out.push_back((int)v[(size_t)i]);
    return out; }
  bool procInfo(int pid,ProcInfo& o) override{
    char path[PROC_PIDPATHINFO_MAXSIZE]; const int pl=proc_pidpath(pid,path,sizeof path);
    o.pid=pid; o.path=pl>0?std::string(path,(size_t)pl):std::string(); o.name.clear();
    if(o.path.empty()){ char nm[64]; const int nl=proc_name(pid,nm,sizeof nm); if(nl>0) o.name.assign(nm,(size_t)nl); }   // the OS gave no path: the short name, if it gives one
    if(o.path.empty()&&o.name.empty()) return kill(pid,0)==0||errno==EPERM;                                         // told nothing: is it there at all?
    return true; }
  std::vector<std::string> openFiles(int pid) override{
    std::vector<std::string> out;
    const int bytes0=proc_pidinfo(pid,PROC_PIDLISTFDS,0,nullptr,0); if(bytes0<=0) return out;                    // not ours to ask (or gone)
    std::vector<char> buf((size_t)bytes0+8*sizeof(proc_fdinfo));
    const int bytes=proc_pidinfo(pid,PROC_PIDLISTFDS,0,buf.data(),(int)buf.size()); if(bytes<=0) return out;
    const proc_fdinfo* fds=(const proc_fdinfo*)buf.data(); const int n=bytes/(int)sizeof(proc_fdinfo);
    for(int i=0;i<n&&out.size()<2048;i++){
      if(fds[i].proc_fdtype!=PROX_FDTYPE_VNODE) continue;                                                           // files and folders only (not sockets, pipes, kqueues)
      vnode_fdinfowithpath vi; memset(&vi,0,sizeof vi);
      if(proc_pidfdinfo(pid,fds[i].proc_fd,PROC_PIDFDVNODEPATHINFO,&vi,(int)sizeof vi)!=(int)sizeof vi) continue;
      if(vi.pvip.vip_path[0]) out.push_back(vi.pvip.vip_path); }
    return out; }
  // codesign --verify --strict -R "=anchor apple generic" path: the signature is valid AND the certificate chain leads to Apple (Developer ID, App Store, Apple's own).
  // An ad-hoc or self-signed program fails the requirement, which is the point. Exit 0: yes; 1 (or 3): no; anything else: could not be checked (core.cpp's self-test
  // calls this on an Apple program, a plain file and an ad-hoc signed copy at start-up, and switches the warnings off, loudly, if the answers are not 0 / 1 / 1).
  int codesign(const std::string& path) override{
    const int r=runCodesign({"--verify","--strict","-R","=anchor apple generic",path},60);
    return r==0?0:((r==1||r==3)?1:-1); }
  bool adhocSignedCopy(const std::string& src,const std::string& dst) override{
    std::error_code ec; std::filesystem::copy_file(src,dst,std::filesystem::copy_options::overwrite_existing,ec); if(ec) return false;
    if(runCodesign({"--force","-s","-",dst},60)==0) return true;
    std::filesystem::remove(dst,ec); return false; }
  std::string plistProgram(const std::string& plist) override{ return applePlistProgram(plist); }
  double idleSeconds() override{ return appleIdleSeconds(); }
  bool screenLocked() override{ return appleScreenLocked(); }
};

Sys* newMacSys(){ return new MacSys(); }

}
