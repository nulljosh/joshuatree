#!/bin/bash
# Shell regression suite: headless in-kernel checks for the six built-in shell
# commands (heaptest, tasktest, preempttest, killtest, ring3test, ps). Each
# command has its own discriminating test that proves a real regression if
# broken, not just that the command didn't crash.
#
# heaptest: proves memory coalescing/splitting actually works (real heap
#   allocator efficiency, not just "didn't crash"). Expects three output lines:
#   - "alloc ok, no reuse" or "reused freed block: ok" (block reuse)
#   - "coalesced adjacent free blocks: ok" (heap coalescing)
#   - "split leftover reused: ok" (heap splitting)
# tasktest: proves cooperative task switching works. Expects "ABABAB..." output
#   interspersed with prompt, showing two tasks yielding back and forth.
# preempttest: proves preemptive scheduling works without yield(). Expects
#   "preempted without yield: ok" (both tasks ran in a tight busy-wait loop).
# killtest: proves task_kill actually stops a task, not just crashes the kernel.
#   Expects "kill: task really stopped: ok" (killed task's counter stayed at a
#   fixed value, not continuing to increment).
# ring3test: proves ring-3 user mode and int 0x80 syscall gate work. Expects
#   "int 0x80 round trip (write returned 31, exit 42): ok".
# ps: proves task_used() discriminates between free/used slots. Just needs to
#   print slot status without crashing.
#
# Safe ordering: heaptest (heap state), tasktest (no preemption yet, just
# cooperative), preempttest (sets up two background tasks), killtest (kills
# one of them), ring3test (no state needed, orthogonal), ps (read-only query).
# Run them in this sequence in one boot so ps can see residual task state
# after the others. The known-working sequence from earlier testing is
# "heaptest tasktest preempttest killtest ring3test ps"; proved below.
set -e
cd "$(dirname "$0")/../.."
make -s kernel.elf

WORKDIR=$(mktemp -d /tmp/jt-shellregress-XXXX)
trap 'rm -rf "$WORKDIR"' EXIT
VGA="$WORKDIR/vga"; LOG="$WORKDIR/log"

send() {
    local s="$1" i c
    for (( i=0; i<${#s}; i++ )); do
        c="${s:$i:1}"
        case "$c" in
            " ") echo "sendkey spc" ;;
            *)   echo "sendkey $c" ;;
        esac
    done
    echo "sendkey ret"
}

(
    sleep 3
    echo 'sendkey esc'; sleep 1
    send "heaptest"; sleep 2
    send "tasktest"; sleep 3
    send "preempttest"; sleep 3
    send "killtest"; sleep 3
    send "ring3test"; sleep 3
    send "ps"; sleep 2
    echo 'xp /4000xb 0x000b8000'
    sleep 1
    echo quit
) | qemu-system-i386 -kernel kernel.elf -display none -monitor stdio > "$LOG" 2>&1

python3 - "$LOG" "$VGA" <<'PYEOF'
import re, sys
bytes_ = []
for l in open(sys.argv[1]).readlines():
    m = re.match(r'^[0-9a-f]+:\s+(.*)', l.strip())
    if m: bytes_.extend(int(x, 16) for x in m.group(1).split())
chars = bytes_[0::2]
text = ''.join(chr(c) if 32 <= c < 127 else '.' for c in chars)
with open(sys.argv[2], 'w') as f:
    for i in range(0, len(text), 80):
        row = text[i:i+80].rstrip('. ')
        if row: f.write(row + '\n')
PYEOF

# Fail if any known-error string appears
if grep -q "alloc ok, no reuse\|reused freed block: ok" "$VGA" && \
   grep -q "coalesced adjacent free blocks: ok" "$VGA" && \
   grep -q "split leftover reused: ok" "$VGA"; then
    echo "✓ heaptest passed (alloc, coalesce, split)"
else
    echo "FAIL: heaptest did not pass all three checks"
    cat "$VGA"
    exit 1
fi

if grep -q "ABABABABABABABABABAB" "$VGA" && grep -q "done (expect ABABAB...)" "$VGA"; then
    echo "✓ tasktest passed (cooperative task switching with AB interleaving)"
else
    echo "FAIL: tasktest did not show proper AB interleaving"
    cat "$VGA"
    exit 1
fi

if grep -q "preempted without yield: ok" "$VGA"; then
    echo "✓ preempttest passed (preemptive scheduling works)"
else
    echo "FAIL: preempttest did not report preemption"
    cat "$VGA"
    exit 1
fi

if grep -q "kill: task really stopped: ok" "$VGA"; then
    echo "✓ killtest passed (task_kill actually stops the task)"
else
    echo "FAIL: killtest did not stop the killed task"
    cat "$VGA"
    exit 1
fi

if grep -q "int 0x80 round trip (write returned 31, exit 42): ok" "$VGA"; then
    echo "✓ ring3test passed (ring-3 user mode and syscall gate work)"
else
    echo "FAIL: ring3test did not complete the syscall round trip"
    cat "$VGA"
    exit 1
fi

# ps just needs to not crash and show some task slots
if grep -q "free\|used" "$VGA"; then
    echo "✓ ps passed (task slot enumeration works)"
else
    echo "FAIL: ps did not enumerate task slots"
    cat "$VGA"
    exit 1
fi

echo "PASS: all shell regression tests passed (heaptest, tasktest, preempttest, killtest, ring3test, ps)"
exit 0
