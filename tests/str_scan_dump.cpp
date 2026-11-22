// str_scan_dump.cpp -- what the Rust daemon prints for `scan <file>` (daemon/src/main.rs), produced by the C++ engine (core/str_scan.h, core/str_daemon.h) instead.
// tests/daemon_diff.py runs both on thousands of files and compares the bytes: the daemon is a port, and this is how it is kept honest.
//   g++ -std=c++17 -O1 -Icore -Imac tests/str_scan_dump.cpp -o str_scan_dump && ./str_scan_dump <file>
#include "scanfile.h"
#include "str_daemon.h"
#include <cstdio>

int main(int argc,char** argv){
  if(argc!=2){ fprintf(stderr,"usage: str_scan_dump <file>\n"); return 2; }
  std::vector<uint8_t> buf;
  if(!readCapped(argv[1],buf)){ printf("ERR cannot read\n"); return 2; }
  const std::string a=strscan::formatAnswer(strscan::scan(buf.data(),buf.size()));
  fwrite(a.data(),1,a.size(),stdout);
  return 0;
}
