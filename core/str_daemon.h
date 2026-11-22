// str_daemon.h -- the answer of the Rust daemon (daemon/src/main.rs) as pure logic: how a StrReport is written and read back (no Windows headers: tests/str_scan_tests.cpp).
//
//   F <h|-><c|l> <text>        one line per finding shown: h = hot link, c = command / l = link
//   OK <links> <commands> <n>  the last line; n is the number of F lines before it
// The app reads this from a process it did not write the bytes of: anything that is not exactly this shape is refused, and the app scans inside itself instead.
#pragma once
#include "str_scan.h"
#include <stdio.h>

namespace strscan{

// the same bytes the daemon prints (tests/daemon_diff.py compares the two)
inline std::string formatAnswer(const StrReport& r){
  std::string o;
  for(const StrFinding& f:r.shown){ o+="F "; o+=f.hot?'h':'-'; o+=f.command?'c':'l'; o+=' '; o+=f.text; o+='\n'; }
  o+="OK "+std::to_string(r.links)+" "+std::to_string(r.commands)+" "+std::to_string(r.shown.size())+"\n";
  return o;
}

// false = not an answer (cut off, garbled, too many lines, counts that cannot be true); `r` is then to be ignored
inline bool parseAnswer(const std::string& out,StrReport& r){
  r=StrReport(); size_t pos=0, shownLinks=0, shownCmds=0;
  while(pos<out.size()){
    const size_t e=out.find('\n',pos); if(e==std::string::npos) return false;      // a killed daemon leaves a last line without its newline
    const std::string line=out.substr(pos,e-pos); pos=e+1;
    if(line.compare(0,2,"F ")==0){
      if(line.size()<6||line[4]!=' '||(line[2]!='h'&&line[2]!='-')||(line[3]!='c'&&line[3]!='l')) return false;
      StrFinding f; f.hot=line[2]=='h'; f.command=line[3]=='c'; f.text=line.substr(5);
      if(f.text.empty()||f.text.size()>MAX_TEXT||(f.command&&f.hot)) return false;
      for(unsigned char c:f.text) if(!printable(c)) return false;
      (f.command?shownCmds:shownLinks)++;
      if(shownLinks>MAX_LINKS||shownCmds>MAX_COMMANDS) return false;
      r.shown.push_back(f); }
    else if(line.compare(0,3,"OK ")==0){
      int links=-1,cmds=-1; unsigned n=0; char extra=0;
      if(sscanf(line.c_str()+3,"%d %d %u%c",&links,&cmds,&n,&extra)!=3) return false;   // exactly three numbers, nothing after them
      if(links<0||cmds<0||n!=r.shown.size()||pos!=out.size()) return false;           // OK is the last line
      if((size_t)links<shownLinks||(size_t)cmds<shownCmds) return false;               // it cannot have found less than it shows
      r.links=links; r.commands=cmds; return true; }
    else return false; }
  return false;                                                                         // no OK line
}

} // namespace strscan
