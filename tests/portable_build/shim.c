// shim.c -- test stand-ins for curl / tar / certutil / g++ / windres, so build_portable.bat and build.bat can be run end to end
// under Wine without a real Windows compiler or network. One program; what it does depends on the name it is started under.
// Not part of the product: it lives in the scratchpad only.
#include <windows.h>
#include <ctype.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char* envor(const char* n, const char* d) { const char* v = getenv(n); return (v && *v) ? v : d; }

static void logline(const char* fmt, ...) {
  FILE* f = fopen(envor("SHIM_LOG", "C:\\shim.log"), "ab");
  if (!f) return;
  va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
  fputc('\n', f); fclose(f);
}

static const char* baseName(const char* p) {
  const char* a = strrchr(p, '\\'); const char* b = strrchr(p, '/');
  const char* s = a > b ? a : b; return s ? s + 1 : p;
}
static int endsWith(const char* s, const char* suf) {
  size_t n = strlen(s), m = strlen(suf); return n >= m && _stricmp(s + n - m, suf) == 0;
}
static int isDir(const char* p) { DWORD a = GetFileAttributesA(p); return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY); }
static int isFile(const char* p) { DWORD a = GetFileAttributesA(p); return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY); }

static int writeFile(const char* path, const char* text) {
  FILE* f = fopen(path, "wb"); if (!f) return 0; fputs(text, f); fclose(f); return 1;
}

// ------------------------------------------------------------------------------------------------ SHA-256
typedef struct { uint32_t h[8]; uint64_t len; unsigned char buf[64]; size_t n; } Sha;
static const uint32_t K[64] = {
  0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,
  0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
  0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,
  0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
  0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,
  0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2 };
#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
static void shaInit(Sha* s) {
  static const uint32_t i[8] = { 0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19 };
  memcpy(s->h, i, sizeof i); s->len = 0; s->n = 0;
}
static void shaBlock(Sha* s, const unsigned char* p) {
  uint32_t w[64], a, b, c, d, e, f, g, h; int i;
  for (i = 0; i < 16; i++) w[i] = ((uint32_t)p[4*i] << 24) | ((uint32_t)p[4*i+1] << 16) | ((uint32_t)p[4*i+2] << 8) | p[4*i+3];
  for (i = 16; i < 64; i++) {
    uint32_t s0 = ROR(w[i-15], 7) ^ ROR(w[i-15], 18) ^ (w[i-15] >> 3), s1 = ROR(w[i-2], 17) ^ ROR(w[i-2], 19) ^ (w[i-2] >> 10);
    w[i] = w[i-16] + s0 + w[i-7] + s1;
  }
  a = s->h[0]; b = s->h[1]; c = s->h[2]; d = s->h[3]; e = s->h[4]; f = s->h[5]; g = s->h[6]; h = s->h[7];
  for (i = 0; i < 64; i++) {
    uint32_t S1 = ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25), ch = (e & f) ^ (~e & g), t1 = h + S1 + ch + K[i] + w[i];
    uint32_t S0 = ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22), mj = (a & b) ^ (a & c) ^ (b & c), t2 = S0 + mj;
    h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
  }
  s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d; s->h[4] += e; s->h[5] += f; s->h[6] += g; s->h[7] += h;
}
static void shaUpdate(Sha* s, const unsigned char* d, size_t n) {
  s->len += n;
  while (n) {
    size_t k = 64 - s->n; if (k > n) k = n;
    memcpy(s->buf + s->n, d, k); s->n += k; d += k; n -= k;
    if (s->n == 64) { shaBlock(s, s->buf); s->n = 0; }
  }
}
static void shaFinal(Sha* s, char out[65]) {
  uint64_t bits = s->len * 8; unsigned char pad = 0x80; int i;
  shaUpdate(s, &pad, 1); pad = 0; while (s->n != 56) shaUpdate(s, &pad, 1);
  unsigned char L[8]; for (i = 0; i < 8; i++) L[i] = (unsigned char)(bits >> (56 - 8 * i));
  shaUpdate(s, L, 8);
  for (i = 0; i < 8; i++) sprintf(out + 8 * i, "%08x", s->h[i]);
}

// ------------------------------------------------------------------------------------------------ helpers
static int copyTree(const char* src, const char* dst) {
  char pat[MAX_PATH * 2], a[MAX_PATH * 2], b[MAX_PATH * 2]; WIN32_FIND_DATAA fd; HANDLE h;
  CreateDirectoryA(dst, NULL);
  snprintf(pat, sizeof pat, "%s\\*", src);
  h = FindFirstFileA(pat, &fd); if (h == INVALID_HANDLE_VALUE) return 0;
  do {
    if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
    snprintf(a, sizeof a, "%s\\%s", src, fd.cFileName); snprintf(b, sizeof b, "%s\\%s", dst, fd.cFileName);
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { if (!copyTree(a, b)) { FindClose(h); return 0; } }
    else if (!CopyFileA(a, b, FALSE)) { FindClose(h); return 0; }
  } while (FindNextFileA(h, &fd));
  FindClose(h); return 1;
}

// ------------------------------------------------------------------------------------------------ the tools
static int doCurl(int argc, char** argv) {
  const char* out = NULL; const char* url = NULL; int i;
  for (i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "-o") && i + 1 < argc) out = argv[++i];
    else if (!strcmp(argv[i], "-C") && i + 1 < argc) i++;
    else if (!strcmp(argv[i], "--retry") || !strcmp(argv[i], "--retry-delay") || !strcmp(argv[i], "--connect-timeout") ||
             !strcmp(argv[i], "--speed-limit") || !strcmp(argv[i], "--speed-time")) i++;
    else if (argv[i][0] != '-') url = argv[i];
  }
  if (!out || !url) { fprintf(stderr, "curl: try 'curl --help'\n"); return 2; }
  int ex = atoi(envor("SHIM_CURL_EXIT", "0"));
  if (ex) { fprintf(stderr, "curl: (%d) simulated failure\n", ex); return ex; }
  char src[MAX_PATH * 2]; snprintf(src, sizeof src, "%s\\%s", envor("SHIM_WEB", "C:\\fakeweb"), baseName(url));
  if (!CopyFileA(src, out, FALSE)) { fprintf(stderr, "curl: (22) The requested URL returned error: 404 (%s)\n", baseName(url)); return 22; }
  fprintf(stderr, "shim curl: %s -> %s\n", baseName(url), out);
  return 0;
}

static int doTar(int argc, char** argv) {
  const char* zip = NULL; const char* dir = NULL; int i;
  for (i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "-xf") && i + 1 < argc) zip = argv[++i];
    else if (!strcmp(argv[i], "-C") && i + 1 < argc) dir = argv[++i];
  }
  if (!zip || !dir) { fprintf(stderr, "tar: bad usage\n"); return 1; }
  if (atoi(envor("SHIM_TAR_EXIT", "0"))) { fprintf(stderr, "tar: simulated failure\n"); return 1; }
  if (!isFile(zip)) { fprintf(stderr, "tar: Error opening archive: Failed to open '%s'\n", zip); return 1; }
  if (!isDir(dir)) { fprintf(stderr, "tar: Can't chdir to '%s'\n", dir); return 1; }
  const char* b = baseName(zip); char dst[MAX_PATH * 2];
  if (!strncmp(b, "winlibs", 7) && atoi(envor("SHIM_TAR_EXIT_TC", "0"))) { fprintf(stderr, "tar: simulated toolchain failure\n"); return 1; }
  if (!strncmp(b, "winlibs", 7)) {
    snprintf(dst, sizeof dst, "%s\\mingw64", dir);
    if (!copyTree(envor("SHIM_FAKE_TC", "C:\\fake\\mingw64"), dst)) { fprintf(stderr, "tar: could not extract toolchain\n"); return 1; }
  } else if (!strncmp(b, "sentinel-watch", 14)) {
    snprintf(dst, sizeof dst, "%s\\sentinel-watch", dir);
    if (!copyTree(envor("SHIM_FAKE_SRC", "C:\\fake\\src\\sentinel-watch"), dst)) { fprintf(stderr, "tar: could not extract source\n"); return 1; }
  } else { fprintf(stderr, "tar: unknown archive %s\n", b); return 1; }
  return 0;
}

static int doCertutil(int argc, char** argv) {
  if (argc < 4 || _stricmp(argv[1], "-hashfile") || _stricmp(argv[3], "SHA256")) { fprintf(stderr, "certutil shim: only -hashfile <f> SHA256\n"); return 1; }
  FILE* f = fopen(argv[2], "rb"); if (!f) { printf("CertUtil: -hashfile command FAILED: 0x80070002\r\n"); return 2; }
  Sha s; shaInit(&s); unsigned char buf[65536]; size_t n;
  while ((n = fread(buf, 1, sizeof buf, f)) > 0) shaUpdate(&s, buf, n);
  fclose(f);
  char hex[65]; shaFinal(&s, hex);
  printf("SHA256 hash of %s:\r\n%s\r\nCertUtil: -hashfile command completed successfully.\r\n", argv[2], hex);
  return 0;
}

static int doGxx(int argc, char** argv) {
  const char* mach = envor("SHIM_GXX_MACHINE", "x86_64-w64-mingw32"); const char* ver = envor("SHIM_GXX_VER", "16.1.0");
  int i;
  if (argc >= 2 && !strcmp(argv[1], "--version")) { printf("g++ (MinGW-W64 x86_64-posix-seh, built by shim) %s\r\nCopyright (C) shim\r\n", ver); return 0; }
  if (argc >= 2 && !strcmp(argv[1], "-dumpmachine")) { printf("%s\r\n", mach); return 0; }
  if (argc >= 2 && !strcmp(argv[1], "-dumpversion")) { printf("%s\r\n", ver); return 0; }
  int compile = 0; const char* out = NULL; const char* srcf = NULL;
  int hasStd = 0, hasUni = 0, hasWin = 0, hasStatic = 0;
  for (i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "-c")) compile = 1;
    else if (!strcmp(argv[i], "-o") && i + 1 < argc) out = argv[++i];
    else if (!strcmp(argv[i], "-std=c++17")) hasStd = 1;
    else if (!strcmp(argv[i], "-municode")) hasUni = 1;
    else if (!strcmp(argv[i], "-mwindows")) hasWin = 1;
    else if (!strcmp(argv[i], "-static")) hasStatic = 1;
    else if (!strncmp(argv[i], "-I", 2) && argv[i][2] && !isDir(argv[i] + 2)) { fprintf(stderr, "g++: error: include directory '%s' does not exist\n", argv[i] + 2); return 1; }
    else if (argv[i][0] != '-' && endsWith(argv[i], ".cpp")) srcf = argv[i];
  }
  if (!out) { fprintf(stderr, "g++: fatal error: no output file\n"); return 1; }
  if (compile) {
    if (!srcf || !isFile(srcf)) { fprintf(stderr, "g++: error: %s: No such file or directory\n", srcf ? srcf : "(none)"); return 1; }
    if (!hasStd) { fprintf(stderr, "g++: error: missing -std=c++17\n"); return 1; }
    const char* bad = getenv("SHIM_GXX_FAIL_ON");
    if (bad && *bad && strstr(srcf, bad)) { fprintf(stderr, "%s:12:5: error: simulated compile error in this file\n", srcf); return 1; }
    char text[MAX_PATH * 2]; snprintf(text, sizeof text, "OBJ of %s\n", srcf);
    if (!writeFile(out, text)) { fprintf(stderr, "g++: error: cannot write %s\n", out); return 1; }
    logline("[g++ compile] %s -> %s", srcf, out);
    return 0;
  }
  // link
  int objs = 0, res = 0;
  for (i = 1; i < argc; i++) if (endsWith(argv[i], ".o")) {
    if (!isFile(argv[i])) { fprintf(stderr, "g++: error: %s: No such file or directory\n", argv[i]); return 1; }
    objs++; if (strstr(argv[i], "sentinel.res.o")) res = 1;
  }
  if (!hasUni || !hasWin || !hasStatic || !res || objs != 25) { fprintf(stderr, "g++: error: link line wrong (municode=%d mwindows=%d static=%d res=%d objs=%d)\n", hasUni, hasWin, hasStatic, res, objs); return 1; }
  if (!writeFile(out, "MZ fake sentinel.exe built by the shim\n")) return 1;
  logline("[g++ link] %d objects -> %s", objs, out);
  return 0;
}

static int doWindres(int argc, char** argv) {
  const char* out = NULL; const char* in = NULL; int hasI = 0, i;
  if (argc >= 2 && !strcmp(argv[1], "--version")) { printf("GNU windres (shim) 2.46\r\n"); return 0; }
  for (i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "-o") && i + 1 < argc) out = argv[++i];
    else if (!strcmp(argv[i], "-I") && i + 1 < argc) { i++; hasI = 1; if (!isDir(argv[i])) { fprintf(stderr, "windres: include dir %s missing\n", argv[i]); return 1; } }
    else if (!strcmp(argv[i], "-O") && i + 1 < argc) i++;
    else if (argv[i][0] != '-') in = argv[i];
  }
  if (!out || !in || !hasI) { fprintf(stderr, "windres: bad usage\n"); return 1; }
  if (!isFile(in)) { fprintf(stderr, "windres: %s: No such file or directory\n", in); return 1; }
  const char* clipsOk[] = { "res\\afk_bg_4k60.mp4", "res\\afk_bg_lite.mp4", "res\\avatar.jpg" };
  for (i = 0; i < 3; i++) if (!isFile(clipsOk[i])) { fprintf(stderr, "windres: %s: No such file or directory\n", clipsOk[i]); return 1; }
  if (!writeFile(out, "RES fake\n")) return 1;
  logline("[windres] %s -> %s", in, out);
  return 0;
}

int main(int argc, char** argv) {
  char self[MAX_PATH]; GetModuleFileNameA(NULL, self, MAX_PATH);
  char name[64]; const char* b = baseName(self); size_t i = 0;
  for (; b[i] && b[i] != '.' && i + 1 < sizeof name; i++) name[i] = (char)tolower((unsigned char)b[i]);
  name[i] = 0;
  logline("[%s] %s", name, GetCommandLineA());
  if (!strcmp(name, "curl")) return doCurl(argc, argv);
  if (!strcmp(name, "tar")) return doTar(argc, argv);
  if (!strcmp(name, "certutil")) return doCertutil(argc, argv);
  if (!strcmp(name, "g++") || !strcmp(name, "gcc")) return doGxx(argc, argv);
  if (!strcmp(name, "windres")) return doWindres(argc, argv);
  fprintf(stderr, "shim: started under unknown name '%s'\n", name);
  return 99;
}
