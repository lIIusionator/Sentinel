#!/usr/bin/env python3
# Test-time transform: Wine 9.0's cmd.exe runs the right-hand side of `cmd1 || cmd2` even when cmd1 succeeded (verified: `ver >nul || echo X`
# prints X). Real Windows does not. For the Wine runs only, rewrite `A || B` as `A` + `if errorlevel 1 B`. Usage: tf.py in.bat out.bat (CRLF output)
import re, sys
src = open(sys.argv[1], newline='').read().replace('\r', '')
out, n = [], 0
for line in src.split('\n'):
    m = re.match(r'^(\s*)(.*?\S)\s*\|\|\s*(goto \S+|exit /b \d+)\s*$', line)
    if m and not line.lstrip().lower().startswith('rem '):
        out.append(m.group(1) + m.group(2)); out.append(m.group(1) + 'if errorlevel 1 ' + m.group(3)); n += 1
    else:
        out.append(line)
open(sys.argv[2], 'w', newline='').write('\r\n'.join(out).replace('\r\n\r\n\r\n', '\r\n\r\n'))
print(f'{sys.argv[1]}: {n} "||" lines rewritten for Wine')
