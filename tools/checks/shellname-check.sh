#!/bin/bash
# 1.0.0: "a real shell that launches a program by name" (roadmap.md).
#
# Before this, `exec HELLO.BIN` needed the exact on-disk filename, shifted
# case and all. Typing `hello` did nothing but print the unknown-command
# error. Now an unknown command falls through to exec_resolve_name()
# (kernel/exec.c) before that error, case-insensitive and trying the real
# build extension, then runs through the same exec_user() `exec` itself
# uses -- see kernel/kernel.c's run().
#
# This check types real lowercase words through the QEMU monitor's
# sendkey, not the boot-time direct-call trick notetest() uses for its own
# internal exec test: that trick exists only because `exec HELLO.BIN`
# needs shifted (uppercase) keys sendkey cannot reliably produce. A bare
# `hello` or `note argvtest ...` needs no shift at all, so it is exactly
# the case a real keystroke-driven check can cover, and covering it for
# real is the point of this file.
#
# Three things proven:
#   1. `hello` (bare) runs HELLO.BIN, seeded moments earlier by `usertest`
#      -- proven by a second full run of its serial-mirrored output.
#   2. `note argvtest hello there` (bare) runs NOTE.BIN with the words
#      after the program name arriving as real argv, on a brand-new file
#      so the byte counts are unambiguous.
#   3. A nonsense name still gets the plain unknown-command error, read
#      off raw VGA memory (the same technique tools/checks/check.sh and
#      shellregress-check.sh use) since that error is a `puts()` typed to
#      the screen, not one of the serial-mirrored ring-3 syscall writes.
#
# The boot is deliberately disk-less (no -drive), same as
# usertest-check.sh and notetest-check.sh: kmain falls back to ramfs, and
# ramfs does no case-folding of its own (unlike FAT's to_fat_name), which
# makes it the harder of the two backends for exec_resolve_name() to get
# right -- exactly why these checks run against it.
set -e
cd "$(dirname "$0")/../.."
make -s kernel.elf >/dev/null

WORKDIR=$(mktemp -d /tmp/jt-shellname-XXXX)
trap 'rm -rf "$WORKDIR"' EXIT
SERIAL="$WORKDIR/serial"
LOG="$WORKDIR/log"
VGA="$WORKDIR/vga"

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
    echo 'sendkey esc'; sleep 1    # leave the GUI for the text shell
    send "usertest"; sleep 4                       # seeds + runs HELLO.BIN once
    send "hello"; sleep 4                           # bare name: fallthrough must run it again
    send "notetest"; sleep 8                        # seeds NOTE.BIN, its own internal checks
    send "note argvtest hello there"; sleep 3        # bare name with argv, fresh file
    send "zzzznosuch"; sleep 2                       # bare name, no such program
    echo 'xp /4000xb 0x000b8000'
    sleep 1
    echo quit
) | qemu-system-i386 -kernel kernel.elf -display none \
      -name jt-shellname-check \
      -serial "file:$SERIAL" -monitor stdio > "$LOG" 2>&1

serial=$(cat "$SERIAL" 2>/dev/null || true)

fail() { echo "FAIL: $1"; echo "--- serial ---"; echo "$serial"; exit 1; }
have() { echo "$serial" | grep -q "$1"; }
count() { echo "$serial" | grep -c "$1" || true; }

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
vga=$(cat "$VGA" 2>/dev/null || true)

# --- usertest seeded HELLO.BIN and ran it once -----------------------------
have "usertest: ok" || fail "usertest did not seed/run HELLO.BIN; nothing to resolve by name"

# --- `hello` by bare name really ran HELLO.BIN a second time --------------
n=$(count "jt-hello: done")
[ "$n" = "2" ] || fail "expected HELLO.BIN's output twice (usertest, then bare \"hello\"), saw $n; the fallthrough did not resolve/run it"
have "jt-hello: read=23" || fail "the bare \"hello\" run never produced HELLO.BIN's real output"

# --- `note argvtest hello there` by bare name really passed argv ----------
have "notetest: ok"       || fail "notetest did not seed NOTE.BIN"
have "notetest argv: ok"  || fail "notetest's own internal exec check regressed"
have "note: added 12"     || fail "bare \"note argvtest hello there\" did not append 12 bytes; the words after the program name did not arrive as argv"
have "note: bytes=12"     || fail "argvtest was not a fresh file after the bare-name run, or the append did not land"

# --- a real unknown name still errors, read off VGA since that error is --
# --- a puts() to the screen, not one of the serial-mirrored syscall writes
echo "$vga" | grep -q "? zzzznosuch" || fail "a real unknown name did not print the unknown-command error"

echo "PASS: the shell launches HELLO.BIN and NOTE.BIN by bare name, case-insensitively, with real argv, and still errors on a real unknown name"
