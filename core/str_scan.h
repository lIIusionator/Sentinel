// str_scan.h -- the "strings" check of a flagged program, as pure logic (no Windows headers: tests/str_scan_tests.cpp runs it on any host).
//
// It does what `strings -a file` plus a careful eye would do: collect the printable ASCII runs and the UTF-16LE runs of at least 6 characters
// and keep what is worth a look --
//   links     http / https / ftp / ws / wss URLs and ip:port pairs: where a dropper downloads from, where a stealer sends what it took
//   commands  command lines malware is known for (encoded PowerShell, certutil -urlcache, schtasks /create, shadow-copy deletion, ...;
//             on macOS: osascript password prompts, curl | bash, keychain dumps, xattr quarantine removal, ...)
// A link to a host that stealers and droppers like (Discord / Telegram webhooks, paste and file-sharing sites, ...) is `hot` and listed first.
// A hit is a lead, not a verdict: honest programs contain links too. core/str_scan.cpp only runs this on unsigned programs in user folders;
// mac/sentinel-strings.cpp runs it on whatever file or folder a Mac user names.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <string>
#include <vector>
#include <set>
#include <algorithm>

struct StrFinding{ bool command=false, hot=false; std::string text; };   // command=false: a link
struct StrReport{ std::vector<StrFinding> shown; int links=0, commands=0; };   // `shown` is capped; the counts cover everything found

namespace strscan{

static const size_t MIN_RUN=6, MAX_RUN=400, MAX_TEXT=160, MAX_CANDIDATES=2000, MAX_LINKS=16, MAX_COMMANDS=8;
static const size_t READ_HALF=8u<<20;   // a file is read whole up to 2*READ_HALF; a bigger one by its first and last READ_HALF bytes (droppers keep their payload at the end)

// Links that every .NET / Office / signed program carries and that say nothing (XML namespaces, certificate services, loop-back).
static const char* const NOISE[]={"schemas.microsoft.com","www.w3.org","schemas.xmlsoap.org","schemas.openxmlformats.org","purl.org","ns.adobe.com",
  "www.microsoft.com/pki","crl.microsoft.com","go.microsoft.com/fwlink","//aka.ms/","//ocsp.","//crl.","www.verisign.com","www.digicert.com","www.apache.org/licenses",
  "www.apple.com/dtds","www.apple.com/appleca","www.apple.com/certificateauthority",   // Info.plist DTD, Apple's code-signing certificates
  "127.0.0.1"};                                                             // (a host without a dot, "localhost", is rejected before this list)
// Where droppers fetch from and stealers post to: webhooks, bot APIs, paste sites, anonymous file hosts, tunnels, URL shorteners.
static const char* const HOT[]={"discord.com/api/webhooks","discordapp.com/api/webhooks","api.telegram.org/bot","//t.me/","pastebin.com/raw","//paste.ee","hastebin.com",
  "raw.githubusercontent.com","cdn.discordapp.com/attachments","transfer.sh","gofile.io","anonfiles","files.catbox.moe","pixeldrain.com","//temp.sh","ngrok",
  "trycloudflare.com",".onion","//bit.ly/","tinyurl.com","//is.gd/"};
// A run is a command when it holds BOTH words (lower case).
struct Cmd{ const char* a; const char* b; };
static const Cmd CMDS[]={{"powershell","-enc"},{"powershell","hidden"},{"powershell","bypass"},{"powershell","iex"},{"powershell","downloadstring"},
  {"powershell","downloadfile"},{"powershell","-nop"},{"certutil","urlcache"},{"certutil","-decode"},{"bitsadmin","/transfer"},{"mshta","http"},{"mshta","script:"},
  {"regsvr32","scrobj"},{"regsvr32","/i:http"},{"rundll32","javascript:"},{"schtasks","/create"},{"vssadmin","shadows"},{"wmic","shadowcopy"},
  {"bcdedit","recoveryenabled"},{"mppreference","exclusion"},{"mppreference","disablerealtime"},{"net user","/add"},{"reg add","currentversion\\run"},
  // macOS: password phishing, run-what-was-downloaded, keychain theft, Gatekeeper / privacy-database tampering, persistence
  {"osascript","hidden answer"},{"osascript","administrator privileges"},{"osascript","do shell script"},
  {"curl","| sh"},{"curl","| bash"},{"curl","| zsh"},{"curl","sh -c"},{"base64","| sh"},{"base64","| bash"},{"base64","| zsh"},   // "sh -c" is also bash -c / zsh -c
  {"security","find-generic-password"},{"security","find-internet-password"},{"security","dump-keychain"},
  {"xattr","quarantine"},{"xattr","-cr"},{"spctl","master-disable"},{"tcc.db","insert"},{"launchctl","load"},{"launchctl","bootstrap"}};

inline bool printable(unsigned char c){ return (c>=0x20&&c<0x7F)||c=='\t'; }
inline std::string lower(std::string s){ for(auto& c:s) if(c>='A'&&c<='Z') c=(char)(c+32); return s; }
inline bool has(const std::string& l,const char* n){ return l.find(n)!=std::string::npos; }
inline bool isDigit(char c){ return c>='0'&&c<='9'; }
inline bool urlChar(unsigned char c){ return c>' '&&c<0x7F&&!strchr("\"'<>)]}\\|^`",c); }

// ASCII runs (what `strings -a` prints) and UTF-16LE runs (`strings -el`), at any alignment. f(run) for each run of at least MIN_RUN characters;
// a run longer than MAX_RUN is handed over in pieces.
template<class F> inline void asciiRuns(const uint8_t* p,size_t n,F f){
  size_t len=0;
  for(size_t i=0;i<=n;i++){
    if(i<n&&printable(p[i])){ if(++len==MAX_RUN){ f(std::string((const char*)p+i+1-len,len)); len=0; } }
    else{ if(len>=MIN_RUN) f(std::string((const char*)p+i-len,len)); len=0; } }
}
template<class F> inline void wideRuns(const uint8_t* p,size_t n,F f){
  for(size_t i=0;i+1<n;){
    if(printable(p[i])&&p[i+1]==0){ std::string r; size_t j=i;
      while(j+1<n&&printable(p[j])&&p[j+1]==0&&r.size()<MAX_RUN){ r.push_back((char)p[j]); j+=2; }
      if(r.size()>=MIN_RUN) f(r);
      i=j; }
    else i++; }
}

// A Mach-O program (thin 32 / 64-bit, either byte order, or a universal "fat" file) or a #! script: what a folder search of the Mac tool looks at.
// A fat header starts like a Java class file (CAFEBABE); a small architecture count tells them apart (a class file has its version, 45 or more, there).
inline bool isMacProgram(const uint8_t* p,size_t n){
  if(n>=2&&p[0]=='#'&&p[1]=='!') return true;
  if(n<4) return false;
  const uint32_t m=((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];
  if(m==0xFEEDFACEu||m==0xFEEDFACFu||m==0xCEFAEDFEu||m==0xCFFAEDFEu) return true;
  if((m==0xCAFEBABEu||m==0xCAFEBABFu)&&n>=8){ const uint32_t archs=((uint32_t)p[4]<<24)|((uint32_t)p[5]<<16)|((uint32_t)p[6]<<8)|p[7]; return archs>=1&&archs<20; }
  return false;
}

// a.b.c.d:port at s[i]: four octets 0..255, a port 1..65535, not 0.x.x.x and not loop-back. A bare address (a version number, say) is not reported.
inline bool ipPortAt(const std::string& s,size_t i,size_t& end){
  size_t p=i; int first=0;
  for(int k=0;k<4;k++){ size_t st=p; int v=0;
    while(p<s.size()&&p-st<3&&isDigit(s[p])){ v=v*10+(s[p]-'0'); p++; }
    if(p==st||v>255) return false;
    if(k==0) first=v;
    if(k<3){ if(p>=s.size()||s[p]!='.') return false; p++; } }
  if(p>=s.size()||s[p]!=':') return false;
  p++;
  size_t st=p; int port=0; while(p<s.size()&&p-st<5&&isDigit(s[p])){ port=port*10+(s[p]-'0'); p++; }
  if(p==st||port<1||port>65535||(p<s.size()&&isDigit(s[p]))) return false;
  if(first==0||first==127) return false;
  end=p; return true;
}

// add(isCommand,isHot,text) for everything worth reporting in one run
template<class Add> inline void analyze(const std::string& r,Add add){
  const std::string l=lower(r);
  if(has(l,"://")){
    static const char* const SCHEME[]={"http://","https://","ftp://","ws://","wss://"};
    for(size_t pos=0;;){
      size_t at=std::string::npos, sl=0;
      for(const char* s:SCHEME){ size_t k=l.find(s,pos); if(k!=std::string::npos&&k<at){ at=k; sl=strlen(s); } }
      if(at==std::string::npos) break;
      size_t e=at+sl; while(e<r.size()&&urlChar((unsigned char)r[e])) e++;
      pos=e;
      while(e>at+sl&&strchr(".,;:!?",r[e-1])) e--;                         // the punctuation of the sentence around it
      size_t h=at+sl, he=h; while(he<e&&r[he]!='/'&&r[he]!='?'&&r[he]!='#'&&r[he]!=':') he++;
      bool ok=he-h>=3&&l.find('.',h)<he;                                    // a real host name: "http://%s:%d/x" and a bare "http://" are not links
      for(size_t k=h;ok&&k<he;k++){ const char c=l[k]; if(!((c>='a'&&c<='z')||isDigit(c)||c=='.'||c=='-')) ok=false; }
      if(!ok) continue;
      const std::string lu=l.substr(at,e-at); bool noise=false, hot=false;
      for(const char* n:NOISE) if(has(lu,n)){ noise=true; break; }
      if(noise) continue;
      for(const char* n:HOT) if(has(lu,n)){ hot=true; break; }
      add(false,hot,r.substr(at,e-at)); } }
  if(has(l,":")&&has(l,"."))
    for(size_t i=0,e=0;i<r.size();i++){
      if(!isDigit(r[i])||(i&&(isDigit(r[i-1])||r[i-1]=='.'))) continue;
      if(ipPortAt(r,i,e)){ add(false,false,r.substr(i,e-i)); i=e-1; } }
  for(const Cmd& c:CMDS) if(has(l,c.a)&&has(l,c.b)){ add(true,false,r); break; }
}

// The whole check on one buffer (the file, or its head and tail). Hot links first, then the other links, then commands; each text is cut at MAX_TEXT.
inline StrReport scan(const uint8_t* p,size_t n){
  std::vector<StrFinding> all; std::set<std::string> seen;
  auto add=[&](bool cmd,bool hot,const std::string& t0){
    if(all.size()>=MAX_CANDIDATES) return;                                  // a file made of nothing but URLs must not eat the memory
    std::string t=t0; if(t.size()>MAX_TEXT){ t.resize(MAX_TEXT-3); t+="..."; }
    if(seen.insert(std::string(cmd?"c|":"l|")+t).second){ StrFinding f; f.command=cmd; f.hot=hot; f.text=t; all.push_back(f); } };
  auto each=[&](const std::string& run){ analyze(run,add); };
  asciiRuns(p,n,each); wideRuns(p,n,each);
  StrReport rep; for(const StrFinding& f:all) (f.command?rep.commands:rep.links)++;
  std::stable_sort(all.begin(),all.end(),[](const StrFinding& a,const StrFinding& b){ if(a.hot!=b.hot) return a.hot; return a.command<b.command; });
  size_t nl=0,nc=0; for(const StrFinding& f:all) if(f.command?nc++<MAX_COMMANDS:nl++<MAX_LINKS) rep.shown.push_back(f);
  return rep;
}

} // namespace strscan
