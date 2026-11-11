// update.h -- "Automatic updates": the pure rules, with no Windows or Apple headers (tests/update_tests.cpp). The Windows side (download, swap, restart) is core/update.cpp,
// the Mac side (look, tell, open the release page) is mac/core.cpp.
//
// The app asks GitHub for the LATEST release of one repository and takes a newer version only when
//   1. the release is neither a draft nor a pre-release and its tag is a plain version number (v1.2 / v1.2.3),
//   2. the addresses of the two files it needs lie under that repository's own releases/download/ path, over https,
//   3. the SHA-256 of the downloaded file is the one the release lists for it in SHA256SUMS.txt.
// 3 catches a damaged or cut-off download; it does not make the release itself trustworthy (that rests on the GitHub account and https; there is no code signature yet).
#pragma once
#include <string>
#include <vector>
#include <utility>
#include <cstdint>
#include <cstring>
#include <cstdlib>

static const char* const UPD_REPO      ="lIIusionator/Sentinel";   // owner/name of the repository whose releases are followed
static const char* const UPD_EXE_ASSET ="SentinelWatch.exe";       // the file an update replaces the running exe with
static const char* const UPD_SUMS_ASSET="SHA256SUMS.txt";          // "<sha256>  <file name>" per line, like `sha256sum` prints
static const int64_t UPD_FIRST_CHECK_MS=90LL*1000;                 // first look: a minute and a half after start (nothing competes with start-up)
static const int64_t UPD_EVERY_MS      =12LL*3600*1000;            // then twice a day
static const int64_t UPD_RETRY_MS      =30LL*60*1000;              // after a failed look (no network, GitHub busy)
static const size_t  UPD_MAX_JSON      =512*1024;                  // the release description is read up to this many bytes
static const size_t  UPD_MAX_EXE       =400u*1024*1024;            // a download larger than this is refused

// ---- versions: "v1.0", "1.0.0", "V2.3.4" -> numbers. Anything else ("1.2-beta", "1.2.3.4", "latest") is not a release number and never updates. ----
struct SwVer{ int a=0,b=0,c=0; bool ok=false; };
inline SwVer verParse(const std::string& s){
  SwVer v; size_t i=0; if(i<s.size()&&(s[i]=='v'||s[i]=='V')) i++;
  int part[3]={0,0,0}; int n=0;
  for(;;){
    if(i>=s.size()||s[i]<'0'||s[i]>'9') return v;
    long x=0; int digits=0; while(i<s.size()&&s[i]>='0'&&s[i]<='9'){ x=x*10+(s[i]-'0'); if(++digits>6) return v; i++; }
    part[n++]=(int)x;
    if(n<3&&i<s.size()&&s[i]=='.'){ i++; continue; }
    break; }
  if(i!=s.size()) return v;
  v.a=part[0]; v.b=part[1]; v.c=part[2]; v.ok=true; return v; }
inline bool verNewer(const std::string& current,const std::string& candidate){
  const SwVer c=verParse(current), n=verParse(candidate); if(!c.ok||!n.ok) return false;
  if(n.a!=c.a) return n.a>c.a; if(n.b!=c.b) return n.b>c.b; return n.c>c.c; }

// ---- SHA-256 (FIPS 180-4) ----
class Sha256{
 public:
  Sha256(){ reset(); }
  void reset(){ static const uint32_t i0[8]={0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    for(int k=0;k<8;k++) h[k]=i0[k]; len=0; fill=0; }
  void update(const void* p,size_t n){ const uint8_t* d=(const uint8_t*)p; len+=n;
    while(n){ size_t t=64-fill; if(t>n) t=n; memcpy(buf+fill,d,t); fill+=t; d+=t; n-=t; if(fill==64){ block(buf); fill=0; } } }
  std::string hex() const{                                  // the digest so far (the object itself stays usable)
    Sha256 c=*this; const uint64_t bits=(uint64_t)c.len*8; const uint8_t one=0x80, zero=0;
    c.update(&one,1); while(c.fill!=56) c.update(&zero,1);
    uint8_t L[8]; for(int k=0;k<8;k++) L[k]=(uint8_t)(bits>>(56-8*k)); c.update(L,8);
    static const char* hx="0123456789abcdef"; std::string o;
    for(int k=0;k<8;k++) for(int j=3;j>=0;j--){ const uint8_t b=(uint8_t)(c.h[k]>>(8*j)); o+=hx[b>>4]; o+=hx[b&15]; }
    return o; }
 private:
  static uint32_t rr(uint32_t x,int n){ return (x>>n)|(x<<(32-n)); }
  void block(const uint8_t* p){
    static const uint32_t K[64]={
      0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
      0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
      0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
      0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
    uint32_t w[64];
    for(int i=0;i<16;i++) w[i]=(uint32_t)p[4*i]<<24|(uint32_t)p[4*i+1]<<16|(uint32_t)p[4*i+2]<<8|(uint32_t)p[4*i+3];
    for(int i=16;i<64;i++){ const uint32_t s0=rr(w[i-15],7)^rr(w[i-15],18)^(w[i-15]>>3), s1=rr(w[i-2],17)^rr(w[i-2],19)^(w[i-2]>>10); w[i]=w[i-16]+s0+w[i-7]+s1; }
    uint32_t a=h[0],b=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],hh=h[7];
    for(int i=0;i<64;i++){
      const uint32_t S1=rr(e,6)^rr(e,11)^rr(e,25), ch=(e&f)^(~e&g), t1=hh+S1+ch+K[i]+w[i];
      const uint32_t S0=rr(a,2)^rr(a,13)^rr(a,22), mj=(a&b)^(a&c)^(b&c), t2=S0+mj;
      hh=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2; }
    h[0]+=a; h[1]+=b; h[2]+=c; h[3]+=d; h[4]+=e; h[5]+=f; h[6]+=g; h[7]+=hh; }
  uint32_t h[8]; uint8_t buf[64]; size_t fill; uint64_t len;
};
inline std::string sha256Hex(const std::string& s){ Sha256 h; h.update(s.data(),s.size()); return h.hex(); }

// ---- a small JSON reader (what GitHub's release description needs: objects, arrays, strings with escapes, numbers, true / false / null) ----
struct Json{
  enum T{Null,Bool,Num,Str,Arr,Obj} t=Null; bool b=false; double n=0; std::string s; std::vector<Json> a; std::vector<std::pair<std::string,Json>> o;
  const Json* get(const char* k) const{ if(t!=Obj) return nullptr; for(const auto& kv:o) if(kv.first==k) return &kv.second; return nullptr; }
  std::string str(const char* k) const{ const Json* j=get(k); return j&&j->t==Str?j->s:std::string(); }
  bool flag(const char* k) const{ const Json* j=get(k); return j&&j->t==Bool&&j->b; }
};
namespace updjson{
  struct P{ const std::string& s; size_t i=0; int depth=0; explicit P(const std::string& x):s(x){} };
  inline void ws(P& p){ while(p.i<p.s.size()&&(p.s[p.i]==' '||p.s[p.i]=='\t'||p.s[p.i]=='\r'||p.s[p.i]=='\n')) p.i++; }
  inline void utf8(std::string& o,unsigned cp){
    if(cp<0x80) o+=(char)cp;
    else if(cp<0x800){ o+=(char)(0xC0|(cp>>6)); o+=(char)(0x80|(cp&63)); }
    else if(cp<0x10000){ o+=(char)(0xE0|(cp>>12)); o+=(char)(0x80|((cp>>6)&63)); o+=(char)(0x80|(cp&63)); }
    else{ o+=(char)(0xF0|(cp>>18)); o+=(char)(0x80|((cp>>12)&63)); o+=(char)(0x80|((cp>>6)&63)); o+=(char)(0x80|(cp&63)); } }
  inline bool hex4(P& p,unsigned& v){ if(p.i+4>p.s.size()) return false; v=0;
    for(int k=0;k<4;k++){ const char c=p.s[p.i++]; v<<=4; if(c>='0'&&c<='9') v|=(unsigned)(c-'0'); else if(c>='a'&&c<='f') v|=(unsigned)(c-'a'+10); else if(c>='A'&&c<='F') v|=(unsigned)(c-'A'+10); else return false; }
    return true; }
  inline bool str(P& p,std::string& out){                 // p.i is on the opening quote
    p.i++; out.clear();
    while(p.i<p.s.size()){ const char c=p.s[p.i++];
      if(c=='"') return true;
      if((unsigned char)c<0x20) return false;
      if(c!='\\'){ out+=c; continue; }
      if(p.i>=p.s.size()) return false;
      const char e=p.s[p.i++];
      switch(e){ case '"': out+='"'; break; case '\\': out+='\\'; break; case '/': out+='/'; break; case 'b': out+='\b'; break; case 'f': out+='\f'; break;
        case 'n': out+='\n'; break; case 'r': out+='\r'; break; case 't': out+='\t'; break;
        case 'u':{ unsigned v; if(!hex4(p,v)) return false;
          if(v>=0xD800&&v<0xDC00){ unsigned lo; if(p.i+1<p.s.size()&&p.s[p.i]=='\\'&&p.s[p.i+1]=='u'){ p.i+=2; if(!hex4(p,lo)||lo<0xDC00||lo>=0xE000) return false; v=0x10000+((v-0xD800)<<10)+(lo-0xDC00); } else return false; }
          else if(v>=0xDC00&&v<0xE000) return false;
          utf8(out,v); break; }
        default: return false; } }
    return false; }
  inline bool val(P& p,Json& j){
    if(++p.depth>32) return false;
    ws(p); if(p.i>=p.s.size()) return false;
    const char c=p.s[p.i]; bool ok=false;
    if(c=='{'){ j.t=Json::Obj; p.i++; ws(p);
      if(p.i<p.s.size()&&p.s[p.i]=='}'){ p.i++; ok=true; }
      else for(;;){ ws(p); if(p.i>=p.s.size()||p.s[p.i]!='"') break; std::string k; if(!str(p,k)) break; ws(p);
        if(p.i>=p.s.size()||p.s[p.i]!=':') break; p.i++; Json v; if(!val(p,v)) break; j.o.emplace_back(k,std::move(v)); ws(p);
        if(p.i<p.s.size()&&p.s[p.i]==','){ p.i++; continue; } if(p.i<p.s.size()&&p.s[p.i]=='}'){ p.i++; ok=true; } break; } }
    else if(c=='['){ j.t=Json::Arr; p.i++; ws(p);
      if(p.i<p.s.size()&&p.s[p.i]==']'){ p.i++; ok=true; }
      else for(;;){ Json v; if(!val(p,v)) break; j.a.push_back(std::move(v)); ws(p);
        if(p.i<p.s.size()&&p.s[p.i]==','){ p.i++; continue; } if(p.i<p.s.size()&&p.s[p.i]==']'){ p.i++; ok=true; } break; } }
    else if(c=='"'){ j.t=Json::Str; ok=str(p,j.s); }
    else if(p.s.compare(p.i,4,"true")==0){ j.t=Json::Bool; j.b=true; p.i+=4; ok=true; }
    else if(p.s.compare(p.i,5,"false")==0){ j.t=Json::Bool; j.b=false; p.i+=5; ok=true; }
    else if(p.s.compare(p.i,4,"null")==0){ j.t=Json::Null; p.i+=4; ok=true; }
    else if(c=='-'||(c>='0'&&c<='9')){ const char* b=p.s.c_str()+p.i; char* e=nullptr; j.n=strtod(b,&e); if(e!=b){ j.t=Json::Num; p.i+=(size_t)(e-b); ok=true; } }
    p.depth--; return ok; }
}
inline bool jsonParse(const std::string& src,Json& out){ updjson::P p(src); out=Json(); if(!updjson::val(p,out)) return false; updjson::ws(p); return p.i==src.size(); }

// ---- the release ----
struct UpdateRelease{ std::string tag, exeUrl, sumsUrl; };
inline bool updStartsWithNoCase(const std::string& s,const std::string& pre){
  if(s.size()<pre.size()) return false;
  for(size_t i=0;i<pre.size();i++){ char a=s[i], b=pre[i]; if(a>='A'&&a<='Z') a=(char)(a-'A'+'a'); if(b>='A'&&b<='Z') b=(char)(b-'A'+'a'); if(a!=b) return false; }
  return true; }
// body = the reply of GET /repos/<repo>/releases/latest. urlPrefix: where asset addresses must start (default: https://github.com/<repo>/releases/download/; the tests use their own).
// false -> err says why, in a form fit for the log.
inline bool updParseRelease(const std::string& body,const std::string& repo,UpdateRelease& r,std::string& err,const std::string& urlPrefix=std::string()){
  r=UpdateRelease(); Json j;
  if(!jsonParse(body,j)||j.t!=Json::Obj){ err="the reply is not a JSON object"; return false; }
  if(j.flag("draft")||j.flag("prerelease")){ err="the latest release is a draft or a pre-release"; return false; }
  r.tag=j.str("tag_name");
  if(!verParse(r.tag).ok){ err="the tag \""+r.tag+"\" is not a plain version number"; return false; }
  const std::string prefix=urlPrefix.empty()?"https://github.com/"+repo+"/releases/download/":urlPrefix;
  const Json* as=j.get("assets"); if(!as||as->t!=Json::Arr){ err="the release has no list of files"; return false; }
  for(const Json& a:as->a){ const std::string name=a.str("name"), url=a.str("browser_download_url");
    if(name!=UPD_EXE_ASSET&&name!=UPD_SUMS_ASSET) continue;
    if(!updStartsWithNoCase(url,prefix)||url.find_first_of(" \t\r\n\"<>\\")!=std::string::npos){ err="the address of "+name+" does not start with "+prefix; return false; }
    (name==UPD_EXE_ASSET?r.exeUrl:r.sumsUrl)=url; }
  if(r.exeUrl.empty()||r.sumsUrl.empty()){ err=std::string("the release has no ")+UPD_EXE_ASSET+" or no "+UPD_SUMS_ASSET; return false; }
  return true; }
// The hash a SHA256SUMS.txt lists for one file name ("<64 hex>  name", "<64 hex> *name"; upper case and CRLF are fine).
inline bool updSumFor(const std::string& sums,const std::string& name,std::string& hex){
  size_t p=0;
  while(p<sums.size()){ size_t e=sums.find('\n',p); if(e==std::string::npos) e=sums.size();
    std::string ln=sums.substr(p,e-p); p=e+1;
    while(!ln.empty()&&(ln.back()=='\r'||ln.back()==' ')) ln.pop_back();
    if(ln.size()<66) continue;
    size_t q=64; while(q<ln.size()&&ln[q]==' ') q++;
    if(q==64) continue;
    if(q<ln.size()&&ln[q]=='*') q++;
    if(ln.compare(q,std::string::npos,name)!=0) continue;
    std::string h=ln.substr(0,64); bool good=true;
    for(char& c:h){ if(c>='A'&&c<='F') c=(char)(c-'A'+'a'); if(!((c>='0'&&c<='9')||(c>='a'&&c<='f'))) good=false; }
    if(good){ hex=h; return true; } }
  return false; }
// The one case in which a newer tag must NOT be fetched again: it was fetched and installed already, and this exe still calls itself older (a release whose files carry a
// lower version than its tag). Without this the app would download itself every few minutes.
inline bool updAlreadyApplied(const std::string& appliedTag,const std::string& latestTag){ return !appliedTag.empty()&&appliedTag==latestTag; }

// ---- an address, split for WinHTTP: https only (plain http only with allowHttp: the test builds talk to a server on this PC) ----
struct UpdUrl{ bool https=false; std::string host,path; int port=0; };
inline bool updParseUrl(const std::string& u,bool allowHttp,UpdUrl& o){
  o=UpdUrl(); size_t p;
  if(u.compare(0,8,"https://")==0){ o.https=true; p=8; } else if(allowHttp&&u.compare(0,7,"http://")==0) p=7; else return false;
  for(char c:u) if((unsigned char)c<=0x20||(unsigned char)c>=0x7f||c=='\\'||c=='#') return false;     // no spaces, control characters, non-ASCII, fragments
  const size_t e=u.find_first_of("/?",p); const std::string hp=u.substr(p,e==std::string::npos?std::string::npos:e-p);
  o.path=e==std::string::npos?std::string("/"):(u[e]=='?'?"/"+u.substr(e):u.substr(e));
  const size_t c=hp.find(':'); o.host=hp.substr(0,c); o.port=o.https?443:80;
  if(c!=std::string::npos){ const std::string ps=hp.substr(c+1); if(ps.empty()||ps.size()>5) return false;
    int v=0; for(char d:ps){ if(d<'0'||d>'9') return false; v=v*10+(d-'0'); } if(v<1||v>65535) return false; o.port=v; }
  if(o.host.empty()) return false;
  for(char d:o.host) if(!((d>='a'&&d<='z')||(d>='A'&&d<='Z')||(d>='0'&&d<='9')||d=='.'||d=='-')) return false;   // no user@host, no [ipv6]
  return true; }
