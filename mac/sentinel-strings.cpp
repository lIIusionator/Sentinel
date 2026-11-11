// sentinel-strings.cpp -- Sentinel Watch's strings check (core/str_scan.h) as a macOS command-line tool, Apple Silicon or Intel.
//
//   sentinel-strings <file-or-folder> [more ...]
//
// Lists the links (webhooks, paste / file hosts, ip:port ...) and suspicious commands (osascript password prompts, curl | bash, keychain dumps ...)
// found in the strings of a program. A file named on the command line is always checked; a folder is searched for programs only (Mach-O files and
// #! scripts; symbolic links are not followed). It only reads: nothing is run, nothing is fetched.
// A hit is a lead, not a verdict. Exit code: 0 nothing found, 1 something found, 2 a file or folder could not be read.
// shortcut: no signature check (the Windows app skips signed programs, this checks whatever it is given) and no watching of a folder;
// upgrade: SecStaticCodeCheckValidity / codesign for the first, FSEvents for the second.
//
// Build on a Mac (Xcode command line tools):   clang++ -std=c++17 -O2 -Icore -Imac mac/sentinel-strings.cpp -o sentinel-strings   (or: make mac-tool)
// Cross build for Apple Silicon (Linux):       make macarm
#include "scanfile.h"
#include <cstdio>
namespace fs=std::filesystem;

static int g_checked=0, g_hits=0, g_bad=0;

static void check(const fs::path& p,bool sayClean){
  std::vector<uint8_t> buf;
  if(!readCapped(p,buf)){ printf("%s: cannot read it\n",p.c_str()); g_bad=1; return; }
  g_checked++;
  const StrReport r=strscan::scan(buf.data(),buf.size());
  if(r.shown.empty()){ if(sayClean) printf("%s: no links or suspicious commands in its strings\n",p.c_str()); return; }
  g_hits++; printf("%s: %d link(s), %d command(s)\n",p.c_str(),r.links,r.commands);
  for(const StrFinding& f:r.shown) printf("  %s%s%s\n",f.hot?"[!] ":"",f.command?"cmd: ":"",f.text.c_str());
  const int more=r.links+r.commands-(int)r.shown.size(); if(more>0) printf("  (+%d more not shown)\n",more);
}

int main(int argc,char** argv){
  if(argc<2){ fprintf(stderr,"usage: %s <file-or-folder> [more ...]\n  Lists the links and suspicious commands inside a program's strings. It only reads: nothing is run or fetched.\n",argv[0]); return 2; }
  std::error_code ec, ec2;
  for(int i=1;i<argc;i++){
    const fs::path p=argv[i];
    if(fs::is_directory(p,ec)){
      fs::recursive_directory_iterator it(p,fs::directory_options::skip_permission_denied,ec), end;
      for(;!ec&&it!=end;it.increment(ec)){
        if(it->is_symlink(ec2)||!it->is_regular_file(ec2)) continue;      // (a file that vanished meanwhile is skipped, not an error)
        uint8_t head[8]={0}; size_t got=0;
        { std::ifstream f(it->path(),std::ios::binary); f.read((char*)head,8); got=(size_t)f.gcount(); }
        if(strscan::isMacProgram(head,got)) check(it->path(),false); }
      if(ec){ printf("%s: stopped searching (%s)\n",argv[i],ec.message().c_str()); g_bad=1; } }
    else if(fs::is_regular_file(p,ec)) check(p,true);
    else{ printf("%s: not a file or folder\n",argv[i]); g_bad=1; } }
  printf("checked %d file(s), %d with links or suspicious commands\n",g_checked,g_hits);
  return g_bad?2:(g_hits?1:0);
}
