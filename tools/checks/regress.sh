#!/bin/bash
# Real regression suite, beyond check.sh's boot-only banner check. Exercises
# heap coalescing, preemptive scheduling, and FAT16 subdirectories/writes
# through real keystrokes into a real QEMU instance with a real disk image,
# then inspects actual VGA memory for the expected output, catching a
# regression check.sh's crude banner check can't see. This formalizes the
# same verification pattern used by hand throughout this project's own
# history (real artifacts, not assumptions) into something repeatable.
# Run after any change that touches memory, tasks, or the filesystem, not
# just before a release.
set -e
cd "$(dirname "$0")"
make -s kernel.elf

WORKDIR=$(mktemp -d /tmp/jt-regress-XXXX)
trap 'rm -rf "$WORKDIR"' EXIT
TMPDISK="$WORKDIR/disk.img"
LOG="$WORKDIR/log"

dd if=/dev/zero of="$TMPDISK" bs=1m count=16 >/dev/null 2>&1
DEV=$(hdiutil attach -nomount "$TMPDISK" | grep -o '/dev/disk[0-9]*' | head -1)
newfs_msdos -F 16 -v REGRESS "$DEV" >/dev/null 2>&1
hdiutil detach "$DEV" >/dev/null 2>&1

send() {
    local s="$1" i c
    for (( i=0; i<${#s}; i++ )); do
        c="${s:$i:1}"
        case "$c" in
            " ") echo "sendkey spc" ;;
            ".") echo "sendkey dot" ;;
            *)   echo "sendkey $c" ;;
        esac
    done
    echo "sendkey ret"
}

(
    sleep 2
    # The kernel now boots straight into the GUI (gui_run()) instead of the
    # text shell; esc is gui_run()'s own real exit path (the same key a
    # real visitor presses), so this drops back to the shell prompt before
    # any of the existing text-mode checks below run.
    echo 'sendkey esc'; sleep 1
    send "heaptest";              sleep 1
    send "tasktest";               sleep 1
    send "preempttest";           sleep 2
    send "mkdir regressdir";      sleep 1
    send "cd regressdir";         sleep 1
    send "write t.txt regression content"; sleep 1
    send "cat t.txt";             sleep 1
    send "cd ..";                 sleep 1
    echo 'xp /4000xb 0x000b8000'
    sleep 1
    echo quit
) | qemu-system-i386 -kernel kernel.elf -hda "$TMPDISK" -display none -monitor stdio > "$LOG" 2>&1

python3 - "$LOG" <<'PYEOF'
import re, sys

lines = open(sys.argv[1]).readlines()
bytes_ = []
for l in lines:
    m = re.match(r'^[0-9a-f]{8}:\s+(.*)', l.strip())
    if m:
        bytes_.extend(int(x, 16) for x in m.group(1).split())
chars = bytes_[0::2]
text = ''.join(chr(c) if 32 <= c < 127 else '.' for c in chars)

checks = [
    ("reused freed block: ok", "heaptest: free-list reuse"),
    ("coalesced adjacent free blocks: ok", "heaptest: coalescing"),
    ("ABABABABABABABABABAB", "tasktest: cooperative interleaving"),
    ("preempted without yield: ok", "preempttest: preemptive scheduling"),
    ("created", "fat: mkdir"),
    ("written", "fat: write"),
    ("regression content", "fat: cat readback matches"),
]
failed = [name for needle, name in checks if needle not in text]
if failed:
    print("FAIL:", ", ".join(failed))
    sys.exit(1)
print(f"PASS: all {len(checks)} checks ({', '.join(name for _, name in checks)})")
PYEOF
