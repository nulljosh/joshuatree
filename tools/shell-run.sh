#!/bin/bash
# v74: generic headless shell driver. Boots kernel.elf under -display none,
# escapes out of the GUI, types each argument as a shell command through
# the monitor's sendkey path (regress.sh's proven mechanism), waits, then
# prints the VGA text screen as plain text plus everything the kernel
# wrote to serial. For eyeballing a test's real output, or root-causing a
# regress.sh failure, without a cocoa window.
#   tools/shell-run.sh heaptest "ring3test" killtest
# Env: JT_WAIT (seconds after each command, default 2), JT_KERNEL (elf).
set -e
cd "$(dirname "$0")/.."
KERNEL="${JT_KERNEL:-kernel.elf}"
WAIT="${JT_WAIT:-2}"
[ "$KERNEL" = kernel.elf ] && make -s kernel.elf

WORKDIR=$(mktemp -d /tmp/jt-shell-XXXX)
trap 'rm -rf "$WORKDIR"' EXIT
LOG="$WORKDIR/log"; SER="$WORKDIR/serial"

send() {
    local s="$1" i c
    for (( i=0; i<${#s}; i++ )); do
        c="${s:$i:1}"
        case "$c" in
            " ") echo "sendkey spc" ;;
            ".") echo "sendkey dot" ;;
            "-") echo "sendkey minus" ;;
            "/") echo "sendkey slash" ;;
            *)   echo "sendkey $c" ;;
        esac
    done
    echo "sendkey ret"
}

(
    sleep 3
    echo 'sendkey esc'; sleep 1
    for cmd in "$@"; do send "$cmd"; sleep "$WAIT"; done
    echo 'xp /4000xb 0x000b8000'
    sleep 1
    echo quit
) | qemu-system-i386 -kernel "$KERNEL" -display none -monitor stdio -serial "file:$SER" > "$LOG" 2>&1

python3 - "$LOG" <<'PYEOF'
import re, sys
bytes_ = []
for l in open(sys.argv[1]).readlines():
    m = re.match(r'^[0-9a-f]{8}:\s+(.*)', l.strip())
    if m: bytes_.extend(int(x, 16) for x in m.group(1).split())
chars = bytes_[0::2]
text = ''.join(chr(c) if 32 <= c < 127 else '.' for c in chars)
print('--- VGA text ---')
for i in range(0, len(text), 80):
    row = text[i:i+80].rstrip('. ')
    if row: print(row)
PYEOF
echo '--- serial ---'
cat "$SER"
