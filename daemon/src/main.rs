//! sentinel-daemon: the strings check of core/str_scan.h, in its own process.
//!
//!   sentinel-daemon scan <file>
//!
//! The Windows app (core/str_scan.cpp) starts one of these for each flagged program and reads what it prints, so the bytes of a hostile file are parsed by
//! a separate, memory-safe program: if it ever crashes or hangs, the app notices, scans inside itself instead, and keeps running. The answer goes to stdout:
//!
//!   F <h|-><c|l> <text>        one line per finding shown: h = hot link, c = command / l = link, then the text (printable ASCII and tabs only)
//!   OK <links> <commands> <n>  the last line: how many were found (all of them), and how many F lines came before it (n)
//!   ERR <why>                  instead of all of that when the file cannot be read (exit code 2)
//!
//! A reader treats anything that does not end in a matching OK line as a failed run (a killed daemon leaves a cut-off answer).
//! shortcut: one process per file and no pipe protocol of its own (a flagged file is rare, starting a process costs milliseconds); upgrade: a long-lived
//! daemon with a named pipe when something else, the process watcher say, needs to ask it many questions a second.
mod scan;

use std::fs::File;
use std::io::{self, Read, Seek, SeekFrom, Write};
use std::path::Path;

/// The file as the app reads it: whole up to 16 MB, else its first and last 8 MB with 8 zero bytes between (droppers keep their payload at the end).
fn read_capped(path: &Path) -> io::Result<Vec<u8>> {
    let mut f = File::open(path)?; // std opens with FILE_SHARE_READ | WRITE | DELETE on Windows: a file that is still being written can be read
    let size = f.metadata()?.len();
    let half = scan::READ_HALF as u64;
    let mut buf = Vec::new();
    if size <= 2 * half {
        f.take(2 * half).read_to_end(&mut buf)?;
    } else {
        Read::by_ref(&mut f).take(half).read_to_end(&mut buf)?;
        buf.extend_from_slice(&[0u8; 8]);
        f.seek(SeekFrom::Start(size - half))?;
        Read::by_ref(&mut f).take(half).read_to_end(&mut buf)?;
    }
    Ok(buf)
}

fn render(r: &scan::Report) -> Vec<u8> {
    let mut out = Vec::new();
    for f in &r.shown {
        out.extend_from_slice(if f.hot { b"F h" } else { b"F -" });
        out.extend_from_slice(if f.command { b"c " } else { b"l " });
        out.extend_from_slice(&f.text);
        out.push(b'\n');
    }
    out.extend_from_slice(format!("OK {} {} {}\n", r.links, r.commands, r.shown.len()).as_bytes());
    out
}

fn main() {
    let args: Vec<_> = std::env::args_os().collect();
    if args.len() != 3 || args[1] != "scan" {
        eprintln!("usage: sentinel-daemon scan <file>");
        std::process::exit(2);
    }
    let (out, code) = match read_capped(Path::new(&args[2])) {
        Ok(bytes) => (render(&scan::scan(&bytes)), 0),
        Err(e) => (format!("ERR {}\n", e.to_string().replace(['\r', '\n'], " ")).into_bytes(), 2),
    };
    if io::stdout().write_all(&out).and_then(|_| io::stdout().flush()).is_err() {
        std::process::exit(2);
    }
    std::process::exit(code);
}

#[cfg(test)]
mod tests {
    use super::*;

    fn temp(name: &str, bytes: &[u8]) -> std::path::PathBuf {
        let p = std::env::temp_dir().join(format!("sentinel-daemon-test-{}-{}", std::process::id(), name));
        std::fs::write(&p, bytes).unwrap();
        p
    }

    #[test]
    fn small_file_is_read_whole() {
        let p = temp("small", b"abc\0http://a.example.com/x\0");
        assert_eq!(read_capped(&p).unwrap(), b"abc\0http://a.example.com/x\0");
        std::fs::remove_file(p).unwrap();
    }

    #[test]
    fn big_file_is_head_zeros_tail() {
        let half = scan::READ_HALF;
        let mut data = vec![b'.'; 2 * half + 1000]; // one byte of 1000 in the middle band must not be read
        data[..5].copy_from_slice(b"HEAD:");
        data[half + 500] = b'M'; // in the part that is skipped
        let n = data.len();
        data[n - 5..].copy_from_slice(b":TAIL");
        let p = temp("big", &data);
        let got = read_capped(&p).unwrap();
        assert_eq!(got.len(), 2 * half + 8);
        assert_eq!(&got[..5], b"HEAD:");
        assert_eq!(&got[half..half + 8], &[0u8; 8]);
        assert_eq!(&got[got.len() - 5..], b":TAIL");
        assert!(!got.contains(&b'M'));
        std::fs::remove_file(p).unwrap();
    }

    #[test]
    fn exactly_two_halves_is_whole() {
        let p = temp("two", &vec![b'x'; 2 * scan::READ_HALF]);
        assert_eq!(read_capped(&p).unwrap().len(), 2 * scan::READ_HALF);
        std::fs::remove_file(p).unwrap();
    }

    #[test]
    fn missing_file_is_an_error() {
        assert!(read_capped(Path::new("/no/such/file/here")).is_err());
    }

    #[test]
    fn answer_format() {
        let r = scan::scan(b"https://discord.com/api/webhooks/1/abc\0schtasks /create /tn x\0http://a.example.org/z\0");
        assert_eq!(
            String::from_utf8(render(&r)).unwrap(),
            "F hl https://discord.com/api/webhooks/1/abc\nF -l http://a.example.org/z\nF -c schtasks /create /tn x\nOK 2 1 3\n"
        );
        assert_eq!(render(&scan::scan(b"nothing to see here at all")), b"OK 0 0 0\n");
    }

    #[test]
    fn answer_stays_small() {
        // the app reads the answer from a 64 KB pipe after the process ended: the caps (16 links + 8 commands, 160 characters each) keep it far below that
        let mut data = Vec::new();
        for i in 0..5000 {
            data.extend_from_slice(format!("http://h{}.example.com/{}\0schtasks /create /tn {}\0", i, "a".repeat(300), "b".repeat(300)).as_bytes());
        }
        let out = render(&scan::scan(&data));
        assert!(out.len() < 8 * 1024, "{}", out.len());
    }
}
