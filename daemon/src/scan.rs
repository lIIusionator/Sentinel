//! The strings check, a line-by-line port of core/str_scan.h (read that file first: it is the reference, and tests/daemon_diff.py proves both give the same answer).
//!
//! It does what `strings -a file` plus a careful eye would do: collect the printable ASCII runs and the UTF-16LE runs of at least 6 characters and keep
//! what is worth a look: links (http / https / ftp / ws / wss and ip:port) and command lines malware is known for. A hit is a lead, not a verdict.
//! Everything here is bytes: a run only ever holds 0x20..0x7E and tab, so there is no encoding to get wrong.

pub const MIN_RUN: usize = 6;
pub const MAX_RUN: usize = 400;
pub const MAX_TEXT: usize = 160;
pub const MAX_CANDIDATES: usize = 2000;
pub const MAX_LINKS: usize = 16;
pub const MAX_COMMANDS: usize = 8;
pub const READ_HALF: usize = 8 << 20; // a file is read whole up to 2 * READ_HALF; a bigger one by its first and last READ_HALF bytes

// The three lists are copied from core/str_scan.h (same order). A change there needs the same change here: tests/daemon_diff.py checks the lists match.
const NOISE: &[&str] = &[
    "schemas.microsoft.com", "www.w3.org", "schemas.xmlsoap.org", "schemas.openxmlformats.org", "purl.org", "ns.adobe.com",
    "www.microsoft.com/pki", "crl.microsoft.com", "go.microsoft.com/fwlink", "//aka.ms/", "//ocsp.", "//crl.", "www.verisign.com", "www.digicert.com",
    "www.apache.org/licenses", "www.apple.com/dtds", "www.apple.com/appleca", "www.apple.com/certificateauthority", "127.0.0.1",
];
const HOT: &[&str] = &[
    "discord.com/api/webhooks", "discordapp.com/api/webhooks", "api.telegram.org/bot", "//t.me/", "pastebin.com/raw", "//paste.ee", "hastebin.com",
    "raw.githubusercontent.com", "cdn.discordapp.com/attachments", "transfer.sh", "gofile.io", "anonfiles", "files.catbox.moe", "pixeldrain.com", "//temp.sh", "ngrok",
    "trycloudflare.com", ".onion", "//bit.ly/", "tinyurl.com", "//is.gd/",
];
/// A run is a command when it holds BOTH words (lower case).
const CMDS: &[(&str, &str)] = &[
    ("powershell", "-enc"), ("powershell", "hidden"), ("powershell", "bypass"), ("powershell", "iex"), ("powershell", "downloadstring"),
    ("powershell", "downloadfile"), ("powershell", "-nop"), ("certutil", "urlcache"), ("certutil", "-decode"), ("bitsadmin", "/transfer"), ("mshta", "http"), ("mshta", "script:"),
    ("regsvr32", "scrobj"), ("regsvr32", "/i:http"), ("rundll32", "javascript:"), ("schtasks", "/create"), ("vssadmin", "shadows"), ("wmic", "shadowcopy"),
    ("bcdedit", "recoveryenabled"), ("mppreference", "exclusion"), ("mppreference", "disablerealtime"), ("net user", "/add"), ("reg add", "currentversion\\run"),
    ("osascript", "hidden answer"), ("osascript", "administrator privileges"), ("osascript", "do shell script"),
    ("curl", "| sh"), ("curl", "| bash"), ("curl", "| zsh"), ("curl", "sh -c"), ("base64", "| sh"), ("base64", "| bash"), ("base64", "| zsh"),
    ("security", "find-generic-password"), ("security", "find-internet-password"), ("security", "dump-keychain"),
    ("xattr", "quarantine"), ("xattr", "-cr"), ("spctl", "master-disable"), ("tcc.db", "insert"), ("launchctl", "load"), ("launchctl", "bootstrap"),
];
const SCHEMES: &[&str] = &["http://", "https://", "ftp://", "ws://", "wss://"];

/// `command == false`: a link.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Finding {
    pub command: bool,
    pub hot: bool,
    pub text: Vec<u8>,
}

/// `shown` is capped; the counts cover everything found.
#[derive(Debug, Default)]
pub struct Report {
    pub shown: Vec<Finding>,
    pub links: usize,
    pub commands: usize,
}

fn printable(c: u8) -> bool {
    (0x20..0x7F).contains(&c) || c == b'\t'
}
fn lower(s: &[u8]) -> Vec<u8> {
    s.to_ascii_lowercase()
}
fn find(h: &[u8], n: &[u8], from: usize) -> Option<usize> {
    if from > h.len() || n.len() > h.len() - from {
        return None;
    }
    h[from..].windows(n.len()).position(|w| w == n).map(|p| p + from)
}
fn has(l: &[u8], n: &str) -> bool {
    find(l, n.as_bytes(), 0).is_some()
}
fn url_char(c: u8) -> bool {
    c > b' ' && c < 0x7F && !b"\"'<>)]}\\|^`".contains(&c)
}

/// ASCII runs (what `strings -a` prints) at any alignment: f(run) for each of at least MIN_RUN characters; a longer one is handed over in pieces of MAX_RUN.
fn ascii_runs(p: &[u8], mut f: impl FnMut(&[u8])) {
    let n = p.len();
    let mut len = 0usize;
    for i in 0..=n {
        if i < n && printable(p[i]) {
            len += 1;
            if len == MAX_RUN {
                f(&p[i + 1 - len..=i]);
                len = 0;
            }
        } else {
            if len >= MIN_RUN {
                f(&p[i - len..i]);
            }
            len = 0;
        }
    }
}

/// UTF-16LE runs (`strings -el`): a printable byte followed by a zero byte, at any alignment, at most MAX_RUN characters.
fn wide_runs(p: &[u8], mut f: impl FnMut(&[u8])) {
    let n = p.len();
    let mut i = 0usize;
    while i + 1 < n {
        if printable(p[i]) && p[i + 1] == 0 {
            let mut r: Vec<u8> = Vec::new();
            let mut j = i;
            while j + 1 < n && printable(p[j]) && p[j + 1] == 0 && r.len() < MAX_RUN {
                r.push(p[j]);
                j += 2;
            }
            if r.len() >= MIN_RUN {
                f(&r);
            }
            i = j;
        } else {
            i += 1;
        }
    }
}

/// a.b.c.d:port at s[i]: four octets 0..255, a port 1..65535, not 0.x.x.x and not loop-back. A bare address (a version number, say) is not reported.
fn ip_port_at(s: &[u8], i: usize) -> Option<usize> {
    let digit = |p: usize| p < s.len() && s[p].is_ascii_digit();
    let mut p = i;
    let mut first = 0u32;
    for k in 0..4 {
        let st = p;
        let mut v = 0u32;
        while p - st < 3 && digit(p) {
            v = v * 10 + (s[p] - b'0') as u32;
            p += 1;
        }
        if p == st || v > 255 {
            return None;
        }
        if k == 0 {
            first = v;
        }
        if k < 3 {
            if p >= s.len() || s[p] != b'.' {
                return None;
            }
            p += 1;
        }
    }
    if p >= s.len() || s[p] != b':' {
        return None;
    }
    p += 1;
    let st = p;
    let mut port = 0u32;
    while p - st < 5 && digit(p) {
        port = port * 10 + (s[p] - b'0') as u32;
        p += 1;
    }
    if p == st || !(1..=65535).contains(&port) || digit(p) {
        return None;
    }
    if first == 0 || first == 127 {
        return None;
    }
    Some(p)
}

/// add(is_command, is_hot, text) for everything worth reporting in one run
fn analyze(r: &[u8], mut add: impl FnMut(bool, bool, &[u8])) {
    let l = lower(r);
    if has(&l, "://") {
        let mut pos = 0usize;
        loop {
            let mut at = usize::MAX;
            let mut sl = 0usize;
            for s in SCHEMES {
                if let Some(k) = find(&l, s.as_bytes(), pos) {
                    if k < at {
                        at = k;
                        sl = s.len();
                    }
                }
            }
            if at == usize::MAX {
                break;
            }
            let mut e = at + sl;
            while e < r.len() && url_char(r[e]) {
                e += 1;
            }
            pos = e;
            while e > at + sl && b".,;:!?".contains(&r[e - 1]) {
                e -= 1; // the punctuation of the sentence around it
            }
            let h = at + sl;
            let mut he = h;
            while he < e && !matches!(r[he], b'/' | b'?' | b'#' | b':') {
                he += 1;
            }
            // a real host name: "http://%s:%d/x" and a bare "http://" are not links
            let mut ok = he - h >= 3 && l[h..].iter().position(|&c| c == b'.').is_some_and(|d| h + d < he);
            for &c in &l[h..he] {
                if !(c.is_ascii_lowercase() || c.is_ascii_digit() || c == b'.' || c == b'-') {
                    ok = false;
                    break;
                }
            }
            if !ok {
                continue;
            }
            let lu = &l[at..e];
            if NOISE.iter().any(|n| has(lu, n)) {
                continue;
            }
            let hot = HOT.iter().any(|n| has(lu, n));
            add(false, hot, &r[at..e]);
        }
    }
    if has(&l, ":") && has(&l, ".") {
        let mut i = 0usize;
        while i < r.len() {
            if r[i].is_ascii_digit() && !(i > 0 && (r[i - 1].is_ascii_digit() || r[i - 1] == b'.')) {
                if let Some(e) = ip_port_at(r, i) {
                    add(false, false, &r[i..e]);
                    i = e;
                    continue;
                }
            }
            i += 1;
        }
    }
    if CMDS.iter().any(|&(a, b)| has(&l, a) && has(&l, b)) {
        add(true, false, r);
    }
}

/// The whole check on one buffer (the file, or its head and tail). Hot links first, then the other links, then commands; each text is cut at MAX_TEXT.
pub fn scan(p: &[u8]) -> Report {
    use std::collections::HashSet;
    let mut all: Vec<Finding> = Vec::new();
    let mut seen: HashSet<(bool, Vec<u8>)> = HashSet::new();
    let mut add = |cmd: bool, hot: bool, t0: &[u8]| {
        if all.len() >= MAX_CANDIDATES {
            return; // a file made of nothing but URLs must not eat the memory
        }
        let mut t = t0.to_vec();
        if t.len() > MAX_TEXT {
            t.truncate(MAX_TEXT - 3);
            t.extend_from_slice(b"...");
        }
        if seen.insert((cmd, t.clone())) {
            all.push(Finding { command: cmd, hot, text: t });
        }
    };
    ascii_runs(p, |run| analyze(run, &mut add));
    wide_runs(p, |run| analyze(run, &mut add));
    let mut rep = Report::default();
    for f in &all {
        if f.command {
            rep.commands += 1;
        } else {
            rep.links += 1;
        }
    }
    all.sort_by_key(|f| (!f.hot, f.command)); // stable: hot first, then links before commands
    let (mut nl, mut nc) = (0usize, 0usize);
    for f in all {
        let (n, max) = if f.command { (&mut nc, MAX_COMMANDS) } else { (&mut nl, MAX_LINKS) };
        if *n < max {
            rep.shown.push(f);
        }
        *n += 1;
    }
    rep
}

#[cfg(test)]
#[path = "scan_tests.rs"]
mod tests;
