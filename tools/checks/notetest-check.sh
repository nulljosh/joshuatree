#!/bin/bash
# The regression test for the v2 syscall ABI (docs/SYSCALL-ABI.md).
#
# v1 gave a ring-3 program a stable interface it could only look through.
# v2 is the half that lets it change something: open() flags, write() to a
# descriptor from open(), a real lseek, and argv. user/note.c is the
# reference program, and unlike user/hello.c it is a program a person
# would run rather than an ABI self-test:
#
#     note FILE              print it
#     note FILE text...      append a line, creating the file
#     note FILE @N text...   overwrite the bytes at offset N
#
# The `notetest` shell command runs it three times, with three different
# argv vectors, against one file, and then reads that file back through
# the VFS itself and compares the bytes. That last step is the assertion
# that carries the weight: everything the program prints is the program's
# claim about what it did, while vfs_read_file() is the filesystem's
# answer. A write path that printed the right numbers and stored nothing
# would satisfy every printed line below and still fail.
#
# It then runs the same program once more through the shell's own `exec`
# dispatcher, so the words a person would type really do arrive as argv.
#
# The boot is deliberately disk-less (no -drive), same as
# tools/checks/usertest-check.sh and tools/checks/ramfs-demo-check.sh:
# kmain falls back to ramfs and `notetest` seeds NOTE.BIN through the VFS,
# so the test depends on no disk image and no host state.
#
# Proven discriminating, see the bottom of this file for the exact reverts
# used and what each one printed.
set -e
cd "$(dirname "$0")/../.."
make -s kernel.elf >/dev/null

# A flat binary only works at the address it was linked at, and that
# address lives in four places now (a linker script cannot include a C
# header). user/hello.ld is checked by usertest-check.sh; user/note.ld is
# this one's job.
base_c=$(grep -o '0xC0500000' kernel/exec.h | head -1)
base_u=$(grep -o '0xC0500000' user/note.ld | head -1)
base_l=$(grep -o '0xC0500000' boot/linker.ld | head -1)
if [ "$base_c" != "0xC0500000" ] || [ "$base_u" != "0xC0500000" ] || [ "$base_l" != "0xC0500000" ]; then
    echo "FAIL: JT_USER_BASE disagrees across kernel/exec.h, user/note.ld and boot/linker.ld"
    exit 1
fi

WORKDIR=$(mktemp -d /tmp/jt-notetest-XXXX)
trap 'rm -rf "$WORKDIR"' EXIT
SERIAL="$WORKDIR/serial"

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
    send "notetest"; sleep 8
    echo quit
) | qemu-system-i386 -kernel kernel.elf -display none \
      -name jt-notetest-check \
      -serial "file:$SERIAL" -monitor stdio >/dev/null 2>&1

serial=$(cat "$SERIAL" 2>/dev/null || true)

fail() { echo "FAIL: $1"; echo "--- serial ---"; echo "$serial"; exit 1; }
have() { echo "$serial" | grep -q "$1"; }

# --- argv ------------------------------------------------------------------
# Every line below depends on argv working at all: with no arguments the
# program prints its usage to stderr and exits 2. These two are the ones
# that pin down the values rather than merely their presence: 9 is the
# length of "buy milk\n", which is argv[2] and argv[3] joined with a
# space, so a program that got the wrong words would print a different
# number.
have "note: added 9"       || fail "the first run did not append 9 bytes; argv did not arrive, or O_CREAT|O_APPEND did not write"
have "note: bytes=9"       || fail "lseek(SEEK_END) did not see a 9-byte file after the first close"

# --- write, append, and the fact that close is what stores ----------------
have "note: bytes=18"      || fail "the second run did not append to the file; O_APPEND overwrote instead of adding, or close did not flush"

# --- seek ------------------------------------------------------------------
have "note: patched at 4"  || fail "lseek(SEEK_SET, 4) did not return 4"
have "buy MILK"            || fail "the seek-then-write did not land at offset 4; the file does not read back patched"

# --- the kernel's own answer, not the program's ---------------------------
have "notetest: exit codes 0 0 0" || fail "one of the three runs exited non-zero"
have "notetest: ok"        || fail "the kernel read NOTE.TXT back off the VFS and it was not what the program wrote"

# --- the shell's own argv splitting ---------------------------------------
have "note: added 13"      || fail "\`exec NOTE.BIN NOTE.TXT and one more\` did not reach the program with its four words"
have "note: bytes=31"      || fail "the shell-driven run did not append to the existing file"
have "notetest argv: ok"   || fail "after the shell-driven run the file was not what those typed words should have produced"

echo "PASS: ring-3 program created, appended to, seek-patched and re-read a real file, with real argv"

# Proven discriminating (run by hand, not part of CI). All three halves of
# each were actually run when this check was written, not assumed, and
# every line is restored in this tree.
#
#   1. The write path. In kernel/syscall.c, write_file_fd(), delete
#         f->dirty = 1;
#      so the bytes land in the descriptor's buffer and close() never
#      writes them back.
#      make -s kernel.elf && ./tools/checks/notetest-check.sh
#      -> FAIL: "lseek(SEEK_END) did not see a 9-byte file after the first
#         close"
#      Restore it -> PASS again.
#
#   2. The seek path. In kernel/syscall.c, sys_lseek(), replace
#         f->pos = (u32)np;
#      with nothing, so seeking reports the right number and moves
#      nothing.
#      -> FAIL: "the seek-then-write did not land at offset 4; the file
#         does not read back patched"
#      Restore it -> PASS again.
#
#   3. The argv path. In kernel/exec.c, build_user_stack(), return
#      JT_USER_STACK_TOP immediately, which is exactly what v1 did: a
#      program starts on an empty stack with no arguments above it.
#      -> FAIL: "the first run did not append 9 bytes; argv did not
#         arrive, or O_CREAT|O_APPEND did not write"
#      Restore it -> PASS again.
