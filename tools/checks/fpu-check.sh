#!/bin/bash
# fputest: two tasks keep distinct values on the x87 stack across task switches. Fails if schedule()
# stops saving per-task x87 state (the MP3 decoder is float code and shares the CPU with the desktop).
set -e
cd "$(dirname "$0")/../.."
make -s kernel.elf
WORKDIR=$(mktemp -d /tmp/jt-fpu-XXXX); trap 'rm -rf "$WORKDIR"' EXIT
send() { local s="$1" i; for (( i=0; i<${#s}; i++ )); do echo "sendkey ${s:$i:1}"; done; echo "sendkey ret"; }
( sleep 3; echo 'sendkey ctrl-alt-backspace'; sleep 1; send "fputest"; sleep 4; echo 'xp /4000xb 0x000b8000'; sleep 1; echo quit ) \
  | qemu-system-i386 -kernel kernel.elf -display none -monitor stdio > "$WORKDIR/log" 2>&1
python3 - "$WORKDIR/log" <<'PYEOF'
import re, sys
b = []
for l in open(sys.argv[1]):
    m = re.match(r'^[0-9a-f]+:\s+(.*)', l.strip())
    if m: b.extend(int(x, 16) for x in m.group(1).split())
t = ''.join(chr(c) if 32 <= c < 127 else '.' for c in b[0::2])
if 'x87 state kept per task: ok' in t: print('PASS: fputest'); sys.exit(0)
print('FAIL: fputest'); print('\n'.join(t[i:i+80].rstrip('. ') for i in range(0, len(t), 80) if t[i:i+80].strip('. '))); sys.exit(1)
PYEOF
