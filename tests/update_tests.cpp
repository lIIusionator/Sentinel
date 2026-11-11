// update_tests.cpp -- native unit tests for shared/update.h (no Windows or network needed): version rules, SHA-256, the JSON reader, choosing the files of a release.
//
//   g++ -std=c++17 -O1 -g -Wall -Wextra -I shared tests/update_tests.cpp -o update_tests && ./update_tests
#include "update.h"
#include <cstdio>

static int g_fail=0, g_checks=0;
#define CHECK(c) do{ g_checks++; if(!(c)){ g_fail++; printf("  FAIL %s:%d  %s\n",__FILE__,__LINE__,#c); } }while(0)

static void testVersions(){
  CHECK(verNewer("v1.0","v1.0.1"));  CHECK(!verNewer("v1.0","v1.0"));  CHECK(!verNewer("v1.0.0","v1.0"));  CHECK(!verNewer("v1.0.1","v1.0"));
  CHECK(verNewer("v1.9","v1.10"));                                   // numbers, not text
  CHECK(!verNewer("v2.0","v1.9.9"));  CHECK(verNewer("v1.9.9","v2.0"));  CHECK(verNewer("1.0","V1.0.1"));          // with or without the v
  CHECK(!verNewer("v1.0","latest"));  CHECK(!verNewer("v1.0","v1.1-beta"));  CHECK(!verNewer("v1.0","v1.0.0.1"));  CHECK(!verNewer("v1.0","v"));
  CHECK(!verNewer("v1.0",""));  CHECK(!verNewer("v1.0","v1."));  CHECK(!verNewer("v1.0","v1.0."));  CHECK(!verNewer("v1.0","v1.0.0."));  CHECK(!verNewer("v1.0","v1..2"));
  CHECK(!verNewer("dev","v2.0"));  CHECK(!verNewer("","v2.0"));                                       // an exe that does not know its own version never updates
  CHECK(!verNewer("v1.0","v1.9999999"));                                                               // 7 digits: not a release number
  SwVer v=verParse("v12.34.56"); CHECK(v.ok&&v.a==12&&v.b==34&&v.c==56);  v=verParse("v7"); CHECK(v.ok&&v.a==7&&v.b==0&&v.c==0);
  CHECK(!verParse("1 .0").ok&&!verParse("1.0 ").ok&&!verParse(" 1.0").ok);
}

static std::string blob(size_t n){ std::string s(n,'\0'); for(size_t i=0;i<n;i++) s[i]=(char)(unsigned char)((i*131+(i>>8)*7)&0xFF); return s; }
static void testSha(){
  CHECK(sha256Hex("")=="e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  CHECK(sha256Hex("abc")=="ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  CHECK(sha256Hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")=="248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
  CHECK(sha256Hex(std::string(1000000,'a'))=="cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
  CHECK(sha256Hex(std::string(55,'x'))=="d5e285683cd4efc02d021a5c62014694958901005d6f71e89e0989fac77e4072");    // 55 / 56 / 64: the padding edges
  CHECK(sha256Hex(std::string(56,'x'))=="04c26261370ee7541549d16dee320c723e3fd14671e66a099afe0a377c16888e");
  CHECK(sha256Hex(std::string(64,'x'))=="7ce100971f64e7001e8fe5a51973ecdfe1ced42befe7ee8d5fd6219506b5393c");
  const std::string b=blob(100003); const char* want="cbdc57ea0fbca82290e90997cd7da1aeb5c4f420c0bc6589cec504bb45032f87";
  CHECK(sha256Hex(b)==want);
  for(size_t step:{(size_t)1,(size_t)3,(size_t)63,(size_t)64,(size_t)65,(size_t)1000,(size_t)4096}){        // however the download arrives in pieces
    Sha256 h; for(size_t i=0;i<b.size();i+=step) h.update(b.data()+i,std::min(step,b.size()-i)); CHECK(h.hex()==want); }
  Sha256 h; h.update("ab",2); std::string mid=h.hex(); h.update("c",1); CHECK(mid==sha256Hex("ab")&&h.hex()==sha256Hex("abc"));   // hex() does not disturb the running hash
}

static void testJson(){
  Json j;
  CHECK(jsonParse("{\"a\":1,\"b\":[true,false,null,\"x\"],\"c\":{\"d\":\"e\"},\"n\":-1.5e2}",j)&&j.t==Json::Obj);
  CHECK(j.get("a")&&j.get("a")->t==Json::Num&&j.get("a")->n==1);  CHECK(j.get("n")&&j.get("n")->n==-150);
  CHECK(j.get("b")&&j.get("b")->t==Json::Arr&&j.get("b")->a.size()==4&&j.get("b")->a[0].b&&!j.get("b")->a[1].b&&j.get("b")->a[2].t==Json::Null&&j.get("b")->a[3].s=="x");
  CHECK(j.get("c")&&j.get("c")->str("d")=="e"&&j.get("zzz")==nullptr&&j.str("zzz").empty()&&!j.flag("zzz"));
  CHECK(jsonParse("  {} ",j)&&j.t==Json::Obj&&j.o.empty());  CHECK(jsonParse("[]",j)&&j.t==Json::Arr);  CHECK(jsonParse(" [ 1 , 2 ] ",j)&&j.a.size()==2);
  CHECK(jsonParse("{\"s\":\"a\\\"b\\\\c\\/d\\n\\t\\u00e9\\u2014\\ud83d\\ude80\"}",j)&&j.str("s")=="a\"b\\c/d\n\t\xc3\xa9\xe2\x80\x94\xf0\x9f\x9a\x80");   // quote, backslash, slash, newline, tab, e-acute, em dash, rocket
  CHECK(jsonParse("{\"a\":1,\"a\":2}",j)&&j.get("a")->n==1);                                          // the first of a repeated key wins
  CHECK(!jsonParse("",j));  CHECK(!jsonParse("{",j));  CHECK(!jsonParse("{\"a\":1}x",j));  CHECK(!jsonParse("{\"a\":1,}",j));  CHECK(!jsonParse("[1,]",j));
  CHECK(!jsonParse("{\"a\":\"x",j));  CHECK(!jsonParse("{\"a\":\"\\q\"}",j));  CHECK(!jsonParse("{\"a\":\"\\ud83d\"}",j));  CHECK(!jsonParse("{\"a\":\"\\ude80\"}",j));
  CHECK(!jsonParse("{\"a\":\"x\ny\"}",j));  CHECK(!jsonParse("{a:1}",j));  CHECK(!jsonParse("{\"a\" 1}",j));  CHECK(!jsonParse("tru",j));  CHECK(!jsonParse("{\"a\":-}",j));
  CHECK(!jsonParse(std::string(40,'[')+std::string(40,']'),j));                                       // nesting is limited
  CHECK(jsonParse(std::string(30,'[')+std::string(30,']'),j));
}

// What GitHub's API sends for a release (trimmed to the fields that matter plus a few that do not, in the order GitHub uses).
static std::string release(const std::string& tag,bool draft,bool pre,const std::string& exeUrl,const std::string& sumsUrl,bool withExe=true,bool withSums=true){
  std::string s="{\"url\":\"https://api.github.com/repos/lIIusionator/Sentinel/releases/1\",\"html_url\":\"https://github.com/lIIusionator/Sentinel/releases/tag/"+tag+"\",\"id\":1,"
    "\"author\":{\"login\":\"lIIusionator\",\"id\":2,\"type\":\"User\"},\"tag_name\":\""+tag+"\",\"name\":\"Sentinel Watch "+tag+"\",\"draft\":"+(draft?"true":"false")+",\"prerelease\":"+(pre?"true":"false")+","
    "\"assets\":[{\"url\":\"https://api.github.com/repos/lIIusionator/Sentinel/releases/assets/9\",\"id\":9,\"name\":\"SentinelWatch-Setup.exe\",\"label\":null,\"uploader\":{\"login\":\"github-actions[bot]\"},"
    "\"size\":1,\"browser_download_url\":\"https://github.com/lIIusionator/Sentinel/releases/download/"+tag+"/SentinelWatch-Setup.exe\"}";
  if(withExe) s+=",{\"id\":10,\"name\":\"SentinelWatch.exe\",\"label\":null,\"uploader\":{\"login\":\"github-actions[bot]\"},\"size\":81745504,\"browser_download_url\":\""+exeUrl+"\"}";
  if(withSums) s+=",{\"id\":11,\"name\":\"SHA256SUMS.txt\",\"size\":300,\"browser_download_url\":\""+sumsUrl+"\"}";
  s+="],\"body\":\"Notes\\r\\n\\u2014 first release \\ud83d\\ude80 with \\\"quotes\\\"\"}";
  return s; }
static void testRelease(){
  const std::string repo="lIIusionator/Sentinel", base="https://github.com/lIIusionator/Sentinel/releases/download/v1.1/";
  UpdateRelease r; std::string err;
  CHECK(updParseRelease(release("v1.1",false,false,base+"SentinelWatch.exe",base+"SHA256SUMS.txt"),repo,r,err)&&r.tag=="v1.1"&&r.exeUrl==base+"SentinelWatch.exe"&&r.sumsUrl==base+"SHA256SUMS.txt");
  CHECK(updParseRelease(release("v1.1",false,false,base+"SentinelWatch.exe",base+"SHA256SUMS.txt"),"liiusionator/sentinel",r,err));                // GitHub is not case sensitive about owner / repo
  CHECK(!updParseRelease(release("v1.1",true,false,base+"SentinelWatch.exe",base+"SHA256SUMS.txt"),repo,r,err)&&err.find("draft")!=std::string::npos);
  CHECK(!updParseRelease(release("v1.1",false,true,base+"SentinelWatch.exe",base+"SHA256SUMS.txt"),repo,r,err)&&err.find("pre-release")!=std::string::npos);
  CHECK(!updParseRelease(release("latest",false,false,base+"SentinelWatch.exe",base+"SHA256SUMS.txt"),repo,r,err)&&err.find("plain version")!=std::string::npos);
  CHECK(!updParseRelease(release("v1.1-rc1",false,false,base+"SentinelWatch.exe",base+"SHA256SUMS.txt"),repo,r,err));
  CHECK(!updParseRelease(release("v1.1",false,false,base+"SentinelWatch.exe",base+"SHA256SUMS.txt",false,true),repo,r,err)&&err.find("no SentinelWatch.exe")!=std::string::npos);
  CHECK(!updParseRelease(release("v1.1",false,false,base+"SentinelWatch.exe",base+"SHA256SUMS.txt",true,false),repo,r,err));                   // no checksum list: no update
  CHECK(!updParseRelease(release("v1.1",false,false,"https://evil.example/SentinelWatch.exe",base+"SHA256SUMS.txt"),repo,r,err)&&err.find("does not start with")!=std::string::npos);
  CHECK(!updParseRelease(release("v1.1",false,false,base+"SentinelWatch.exe","https://github.com/someone/else/releases/download/v1.1/SHA256SUMS.txt"),repo,r,err));
  CHECK(!updParseRelease(release("v1.1",false,false,"http://github.com/lIIusionator/Sentinel/releases/download/v1.1/SentinelWatch.exe",base+"SHA256SUMS.txt"),repo,r,err));   // not https
  CHECK(!updParseRelease(release("v1.1",false,false,"https://github.com/lIIusionator/Sentinel/releases/download/../../x/SentinelWatch.exe ",base+"SHA256SUMS.txt"),repo,r,err));   // a space in the address
  CHECK(!updParseRelease(release("v1.1",false,false,"https://github.com/lIIusionator/Sentinel/releases/download/v1.1/a\\\\b.exe",base+"SHA256SUMS.txt"),repo,r,err));
  CHECK(updParseRelease(release("v1.1",false,false,"http://127.0.0.1:8099/SentinelWatch.exe","http://127.0.0.1:8099/SHA256SUMS.txt"),repo,r,err,"http://127.0.0.1:8099/"));   // the test servers' own prefix
  CHECK(!updParseRelease("not json",repo,r,err)&&!updParseRelease("[]",repo,r,err)&&!updParseRelease("{\"message\":\"Not Found\"}",repo,r,err));    // GitHub's 404 body is not a release
  CHECK(!updParseRelease("{\"tag_name\":\"v1.1\",\"assets\":{}}",repo,r,err));
}

static void testSums(){
  const std::string h1(64,'a'), h2="0123456789abcdef0123456789abcdef0123456789abcdef0123456789ABCDEF";
  std::string hex;
  CHECK(updSumFor(h1+"  SentinelWatch.exe\n"+h2+"  SentinelWatch-Setup.exe\n","SentinelWatch.exe",hex)&&hex==h1);
  CHECK(updSumFor(h1+"  SentinelWatch.exe\n"+h2+"  SentinelWatch-Setup.exe\n","SentinelWatch-Setup.exe",hex)&&hex=="0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");   // upper case is lowered
  CHECK(updSumFor(h1+"  SentinelWatch.exe\r\n","SentinelWatch.exe",hex)&&hex==h1);                       // CRLF
  CHECK(updSumFor(h1+" *SentinelWatch.exe","SentinelWatch.exe",hex)&&hex==h1);                        // binary-mode marker, no final newline
  CHECK(updSumFor("# a comment\n\n"+h1+"  SentinelWatch.exe  \n","SentinelWatch.exe",hex));         // blank lines, comments, trailing spaces
  CHECK(!updSumFor(h1+"  SentinelWatch.exe.old\n","SentinelWatch.exe",hex));                          // a longer name is another file
  CHECK(!updSumFor(h1+"  xSentinelWatch.exe\n","SentinelWatch.exe",hex));
  CHECK(!updSumFor(h1+"SentinelWatch.exe\n","SentinelWatch.exe",hex));                               // no separator
  CHECK(!updSumFor(std::string(63,'a')+"  SentinelWatch.exe\n","SentinelWatch.exe",hex));           // a short hash
  CHECK(!updSumFor(std::string(63,'a')+"g  SentinelWatch.exe\n","SentinelWatch.exe",hex));          // not hex
  CHECK(!updSumFor("","SentinelWatch.exe",hex));
  CHECK(updSumFor(std::string(63,'g')+"x  SentinelWatch.exe\n"+h1+"  SentinelWatch.exe\n","SentinelWatch.exe",hex)&&hex==h1);   // a bad line does not hide a good one after it
  CHECK(updAlreadyApplied("v1.1","v1.1")&&!updAlreadyApplied("v1.1","v1.2")&&!updAlreadyApplied("","v1.1"));
}

static void testUrl(){
  UpdUrl u;
  CHECK(updParseUrl("https://api.github.com/repos/a/b/releases/latest",false,u)&&u.https&&u.host=="api.github.com"&&u.port==443&&u.path=="/repos/a/b/releases/latest");
  CHECK(updParseUrl("https://github.com",false,u)&&u.path=="/"&&u.port==443);                           // no path at all
  CHECK(updParseUrl("https://github.com?x=1",false,u)&&u.path=="/?x=1");                                // a query straight after the host
  CHECK(updParseUrl("https://objects.githubusercontent.com:8443/x/y?z=1&w=2",false,u)&&u.port==8443&&u.path=="/x/y?z=1&w=2"&&u.host=="objects.githubusercontent.com");
  CHECK(!updParseUrl("http://github.com/x",false,u));                                                   // plain http is refused ...
  CHECK(updParseUrl("http://127.0.0.1:8099/latest.json",true,u)&&!u.https&&u.port==8099&&u.host=="127.0.0.1");   // ... except for the test builds
  CHECK(updParseUrl("http://localhost/x",true,u)&&u.port==80);
  CHECK(!updParseUrl("ftp://github.com/x",true,u)&&!updParseUrl("github.com/x",true,u)&&!updParseUrl("",true,u)&&!updParseUrl("https://",false,u));
  CHECK(!updParseUrl("https://user@github.com/x",false,u)&&!updParseUrl("https://[::1]/x",false,u));    // no credentials, no ipv6 literals
  CHECK(!updParseUrl("https://github.com:/x",false,u)&&!updParseUrl("https://github.com:0/x",false,u)&&!updParseUrl("https://github.com:65536/x",false,u)&&!updParseUrl("https://github.com:80a/x",false,u));
  CHECK(!updParseUrl("https://github.com/a b",false,u)&&!updParseUrl("https://github.com/a\tb",false,u)&&!updParseUrl("https://github.com/a\\b",false,u));
  CHECK(!updParseUrl("https://github.com/a#frag",false,u)&&!updParseUrl("https://g\xc3\xb6thub.com/x",false,u));
  CHECK(!updParseUrl("https://git hub.com/x",false,u)&&!updParseUrl("https://github.com:80:90/x",false,u));
}

int main(){
  testVersions(); testSha(); testJson(); testRelease(); testSums(); testUrl();
  printf("\n%d checks, %d failed\n",g_checks,g_fail);
  return g_fail?2:0;
}
