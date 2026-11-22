#!/usr/bin/env python3
"""daemon_diff.py -- the Rust daemon (daemon/) must say exactly what the C++ engine (core/str_scan.h) says.

  python3 tests/daemon_diff.py [build dir]          (run from the project root; `make tests` does)

1. the three lists (NOISE, HOT, CMDS) of core/str_scan.h and daemon/src/scan.rs are the same, in the same order;
2. both programs scan a few thousand files and print the same bytes: generated files made of the pieces the engine knows (links, hot hosts, noise hosts,
   ip:port, commands, wide strings, punctuation, runs of 399 / 400 / 401 characters, duplicates, high bytes), real Windows programs and libraries that happen to be
   on this machine (Wine's DLLs: real UTF-16 text), and files above 16 MB (head, tail and the skipped middle).
Needs g++ and cargo (the host ones). Exit code 0 = identical."""
import os, random, re, subprocess, sys, tempfile, glob

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, sys.argv[1] if len(sys.argv) > 1 else "build")
os.makedirs(BUILD, exist_ok=True)


def sh(*cmd, **kw):
    return subprocess.run(cmd, capture_output=True, **kw)


# ---- 1. the lists ----------------------------------------------------------------------------------------------------------------------------------
def lists_cpp():
    t = open(os.path.join(ROOT, "core/str_scan.h"), encoding="utf-8").read()
    def block(name):
        m = re.search(name + r"\[\]=\{(.*?)\};", t, re.S)
        return m.group(1)
    def strings(s):       # a string or a // comment, whichever comes first: "//aka.ms/" is a string, not a comment
        return [bytes(m.group(1), "utf-8").decode("unicode_escape") for m in re.finditer(r'"((?:[^"\\]|\\.)*)"|//[^\n]*', s) if m.group(1) is not None]
    return strings(block("NOISE")), strings(block("HOT")), strings(block("CMDS"))

def lists_rust():
    t = open(os.path.join(ROOT, "daemon/src/scan.rs"), encoding="utf-8").read()
    def block(name):
        m = re.search(r"const " + name + r": &\[[^\]]*?\] = &\[(.*?)\n\];", t, re.S)
        return m.group(1)
    def strings(s):
        return [bytes(x, "utf-8").decode("unicode_escape") for x in re.findall(r'"((?:[^"\\]|\\.)*)"', s)]
    return strings(block("NOISE")), strings(block("HOT")), strings(block("CMDS"))

fails = 0
cn, ch, cc = lists_cpp()
rn, rh, rc = lists_rust()
for name, a, b in (("NOISE", cn, rn), ("HOT", ch, rh), ("CMDS", cc, rc)):
    if a != b or not a:
        fails += 1
        print("FAIL: %s differs between core/str_scan.h and daemon/src/scan.rs\n  C++ : %r\n  Rust: %r" % (name, a, b))
    else:
        print("  ok  %s: %d entries the same" % (name, len(a)))
assert len(cc) % 2 == 0
CMDS = list(zip(cc[0::2], cc[1::2]))

# ---- 2. the two programs ----------------------------------------------------------------------------------------------------------------------------
r = sh("g++", "-std=c++17", "-O1", "-I" + os.path.join(ROOT, "core"), "-I" + os.path.join(ROOT, "mac"), os.path.join(ROOT, "tests/str_scan_dump.cpp"), "-o", os.path.join(BUILD, "str_scan_dump"))
if r.returncode: sys.exit("cannot build tests/str_scan_dump.cpp:\n" + r.stderr.decode())
CPP = os.path.join(BUILD, "str_scan_dump")
tgt = os.path.join(BUILD, "daemon-target")
r = sh("cargo", "build", "--release", "--quiet", "--manifest-path", os.path.join(ROOT, "daemon/Cargo.toml"), "--target-dir", tgt)
if r.returncode: sys.exit("cannot build the daemon:\n" + r.stderr.decode())
RUST = os.path.join(tgt, "release", "sentinel-daemon")

rnd = random.Random(20261115)
SCHEMES = ["http://", "https://", "ftp://", "ws://", "wss://", "HTTP://", "Https://", "hxxp://", "http:/", "://", "ab://"]
HOSTS = ["evil.example.com", "a.b.c", "x", "localhost", "127.0.0.1", "10.1.2.3", "cdn.discordapp.com", "discord.com", "pastebin.com", "api.telegram.org", "t.me", "best.me",
         "schemas.microsoft.com", "www.w3.org", "crl.microsoft.com", "syncrl.example.com", "ocsp.digicert.com", "www.apple.com", "bit.ly", "orbit.ly", "foo-bar.example.org",
         "UPPER.Example.COM", "a_b.example.com", "%s", "%s.example.com", "ngrok.io", "abc.onion", "files.catbox.moe", "x.y"]
PATHS = ["", "/", "/a", "/payload.exe", "/api/webhooks/1/abc", "/raw/abc", "/bot123:abc/sendMessage", "/" + "a" * 150, "/" + "b" * 400, "/x?y=1#z", "/winfx/2006/xaml",
         "/DTDs/PropertyList-1.0.dtd", "/appleca/", "/certificateauthority/", "/fwlink/?x", "/x\\y", "/q|r", "/a)b", "/c.", "/d,", "/e;", "/f!", "/g?", "/h:"]
PORTS = ["", ":80", ":8080", ":0", ":99999", ":65535", ":65536", ":4444x", ":"]
IPS = ["185.199.108.153", "203.0.113.9", "127.0.0.1", "0.0.0.0", "10.0.0.1", "999.1.1.1", "1.2.3.4.5", "6.1.7601.17514", "255.255.255.255", "1.2.3", "01.02.03.04", "192.168.1.1"]
PUNCT = list(" .,;:!?\"'<>)]}\\|^`\t-_/=&%$#@*+~()[]{}")
WORDS = ["hello", "world", "powershell", "cmd", "certutil", "curl", "base64", "security", "xattr", "launchctl", "osascript", "-enc", "hidden", "bypass", "-urlcache", "/create",
         "| sh", "| bash", "| zsh", "sh -c", "|bash", "net user", "/add", "reg add", "CurrentVersion\\Run", "TCC.db", "INSERT", "mshta", "http", "script:", "iex", "-nop"]

def url():
    return rnd.choice(SCHEMES) + rnd.choice(HOSTS) + rnd.choice(PORTS) + rnd.choice(PATHS)
def ipport():
    return rnd.choice(IPS) + rnd.choice(PORTS)
def command():
    a, b = rnd.choice(CMDS)
    parts = [a, b, rnd.choice(WORDS)]
    rnd.shuffle(parts) if rnd.random() < 0.3 else None
    return (" ".join(parts)) if rnd.random() < 0.8 else a + rnd.choice(PUNCT) + b
def soup():
    return "".join(rnd.choice(PUNCT + list("abcXYZ019")) for _ in range(rnd.randint(1, 30)))
def piece():
    k = rnd.random()
    if k < 0.30: s = url()
    elif k < 0.40: s = ipport()
    elif k < 0.55: s = command()
    elif k < 0.65: s = rnd.choice(WORDS)
    elif k < 0.75: s = soup()
    elif k < 0.82: s = "X" * rnd.choice([398, 399, 400, 401, 402, 799, 800, 801]) + url()
    elif k < 0.88: s = url() + "Y" * rnd.choice([350, 399, 400, 401, 600])
    elif k < 0.93: s = " ".join(url() for _ in range(rnd.randint(2, 6)))
    else: s = url() + " " + command() + " " + ipport()
    if rnd.random() < 0.15: s = s[: rnd.randint(0, len(s))]
    return s

def encode(s):
    b = s.encode("latin-1", "replace")
    return b"".join(bytes([c, 0]) for c in b) if rnd.random() < 0.3 else b
def sep():
    k = rnd.random()
    if k < 0.5: return b"\0"
    if k < 0.65: return b"\n"
    if k < 0.75: return b" "
    if k < 0.85: return bytes(rnd.randrange(128, 256) for _ in range(rnd.randint(1, 3)))
    return b""
def generated():
    out = bytearray(rnd.randbytes(rnd.randint(0, 3)))      # odd and even alignment of the wide strings
    for _ in range(rnd.randint(1, 40)):
        out += encode(piece()) + sep()
        if rnd.random() < 0.1: out += rnd.randbytes(rnd.randint(1, 300))
    return bytes(out)

tmp = tempfile.mkdtemp(prefix="sw-diff-")
files = []
for i in range(1500):
    p = os.path.join(tmp, "g%04d.bin" % i)
    open(p, "wb").write(generated()); files.append(p)
# a lot of links: the cap on candidates (2000) and on what is shown
p = os.path.join(tmp, "many.bin")
open(p, "wb").write(b"\0".join(("http://h%d.example.com/%d" % (i, i)).encode() for i in range(3500)) + b"\0schtasks /create /x\0"); files.append(p)
# a file above 16 MB: the payload at the start, at the end, and one in the middle that must not be seen
big = bytearray(rnd.randbytes(40 << 20))
for off, s in ((1000, b"\0http://head.example.com/x\0"), (len(big) - 2000, b"\0https://discord.com/api/webhooks/9/tail\0"), (20 << 20, b"\0http://middle.example.com/skipped\0"),
               ((8 << 20) - 10, b"\0http://straddle.example.com/half\0")):
    big[off:off + len(s)] = s
p = os.path.join(tmp, "big.bin"); open(p, "wb").write(big); files.append(p)
big = big[: (16 << 20) + 5]; p = os.path.join(tmp, "just_over.bin"); open(p, "wb").write(big); files.append(p)
p = os.path.join(tmp, "empty.bin"); open(p, "wb").close(); files.append(p)
real = sorted(glob.glob("/usr/lib/wine/x86_64-windows/*.dll") + glob.glob("/usr/lib/wine/x86_64-windows/*.exe"))
rnd.shuffle(real); files += real[:150]
bins = sorted(glob.glob("/usr/bin/*"))
rnd.shuffle(bins); files += [f for f in bins if os.path.isfile(f) and os.path.getsize(f) < (60 << 20)][:60]

bad = 0; withhits = 0
for f in files:
    a = sh(CPP, f); b = sh(RUST, "scan", f)
    if a.stdout != b.stdout or a.returncode != b.returncode:
        bad += 1
        if bad <= 5: print("DIFFERENT on %s\n  C++ : %r\n  Rust: %r" % (f, a.stdout[:600], b.stdout[:600]))
    elif a.stdout != b"OK 0 0 0\n":
        withhits += 1
print("  %s  %d files compared (%d with findings), %d different" % ("ok " if not bad else "FAIL", len(files), withhits, bad))
fails += bad
# an unreadable file: the daemon says ERR and exits 2 (the C++ side is not compared here: it has its own wording)
r = sh(RUST, "scan", os.path.join(tmp, "no-such-file"))
if r.returncode != 2 or not r.stdout.startswith(b"ERR "): fails += 1; print("FAIL: a missing file must give 'ERR ...' and exit code 2, got %r %r" % (r.returncode, r.stdout))
else: print("  ok  a missing file: ERR, exit code 2")
r = sh(RUST); r2 = sh(RUST, "scan")
if r.returncode != 2 or r2.returncode != 2: fails += 1; print("FAIL: wrong arguments must exit with 2")
else: print("  ok  wrong arguments: exit code 2")
sh("rm", "-rf", tmp)
print("daemon_diff: all checks passed" if not fails else "daemon_diff: FAILED")
sys.exit(1 if fails else 0)
