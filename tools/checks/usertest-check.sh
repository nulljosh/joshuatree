#!/bin/bash
# The regression test for the v1 syscall ABI (docs/SYSCALL-ABI.md).
#
# What it proves, and why it is the test this repo needed most: user/hello.c
# is a real external consumer of the kernel. It is compiled separately,
# against user/jtsys.h and no kernel header, linked flat at the window
# boot/linker.ld reserves, written to the VFS, read back off the VFS, and
# run as a genuine ring-3 task with its own page directory and IOPL 0.
# Every line it prints below arrived through int 0x80. If a syscall number,
# an argument register, a return convention or the fd numbering changes,
# this fails -- which is exactly what a MAJOR version is supposed to protect,
# and the reason 1.0.0 can mean anything here.
#
# The boot is deliberately disk-less (no -drive), the same condition
# tools/checks/ramfs-demo-check.sh reproduces and the same one the browser
# embed runs under: kmain falls back to ramfs, and `usertest` seeds both
# HELLO.BIN and HELLO.TXT through the VFS before exec'ing. So the test
# depends on no disk image and no host state.
#
# Assertions are read off the serial port, not VGA memory: sys_write
# mirrors every ring-3 write there, so the bytes asserted on are the
# program's own output rather than a rendering of it.
#
# Proven discriminating, see the bottom of this file for the exact revert
# used and what it printed.
set -e
cd "$(dirname "$0")/../.."
make -s kernel.elf >/dev/null

# The one thing a flat binary cannot survive is being linked at a
# different address than it is loaded at, and that address lives in three
# places by necessity (a linker script cannot include a C header). Check
# they still agree before spending a boot on it.
base_c=$(grep -o '0xC0500000' kernel/exec.h | head -1)
base_u=$(grep -o '0xC0500000' user/hello.ld | head -1)
base_l=$(grep -o '0xC0500000' boot/linker.ld | head -1)
if [ "$base_c" != "0xC0500000" ] || [ "$base_u" != "0xC0500000" ] || [ "$base_l" != "0xC0500000" ]; then
    echo "FAIL: JT_USER_BASE disagrees across kernel/exec.h, user/hello.ld and boot/linker.ld"
    exit 1
fi

WORKDIR=$(mktemp -d /tmp/jt-usertest-XXXX)
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
    send "usertest"; sleep 4
    echo quit
) | qemu-system-i386 -kernel kernel.elf -display none \
      -name jt-usertest-check \
      -serial "file:$SERIAL" -monitor stdio >/dev/null 2>&1

serial=$(cat "$SERIAL" 2>/dev/null || true)

fail() { echo "FAIL: $1"; echo "--- serial ---"; echo "$serial"; exit 1; }
have() { echo "$serial" | grep -q "$1"; }

# open(5) + read(3): the file's real bytes made the round trip out of the
# kernel into a user buffer and back in through write(4).
have "joshua tree user space" || fail "the program never echoed HELLO.TXT's contents back through write(); open/read did not work"
have "jt-hello: read=23"      || fail "read() returned the wrong byte count for HELLO.TXT (expected 23)"
# The per-fd offset is real: a second read is end of file, not the same bytes.
have "jt-hello: eof=0"        || fail "a second read() did not report end of file; the per-fd offset is not advancing"
# open() hands out descriptors starting at 3, per the contract.
have "jt-hello: fd=3"         || fail "open() did not return fd 3 as the first descriptor"
# close(6) really releases: hello.c exits 3 or 4 if close failed or a
# double close succeeded, so reaching the later lines at all proves both.
have "jt-hello: pid="         || fail "getpid() line missing; close()/double-close checks failed before it"
have "jt-hello: time ok"      || fail "time() did not return a plausible epoch, or did not write through its pointer"
have "jt-hello: yielded"      || fail "sched_yield() did not return 0"
have "jt-hello: nosys=-38"    || fail "an unassigned syscall number did not return -ENOSYS"
# The security shape, tested from ring 3 where it counts. 0xC0000000 is
# mapped but supervisor-only, so a check that only rejected unmapped
# addresses would pass these; -14 is -EFAULT.
have "jt-hello: openfault=-14"  || fail "open() accepted a kernel pointer as a path instead of returning -EFAULT"
have "jt-hello: readfault=-14"  || fail "read() accepted a kernel destination buffer instead of returning -EFAULT"
have "jt-hello: writefault=-14" || fail "write() accepted an unmapped source buffer instead of returning -EFAULT"
have "jt-hello: timefault=-14"  || fail "time() wrote through a kernel pointer instead of returning -EFAULT"
have "jt-hello: badfd=-9"       || fail "read() on an unopened descriptor did not return -EBADF"
have "jt-hello: done"         || fail "the program did not reach its last line"
# And the exit status the kernel itself observed, not something the
# program printed about itself.
have "usertest: exit code 9"  || fail "the kernel did not observe exit code 9 from the ring-3 task"
have "usertest: ok"           || fail "usertest reported failure"

echo "PASS: ring-3 reference program ran the whole v1 syscall set off the VFS and exited 9"

# Proven discriminating (run by hand, not part of CI):
#   1. Break one syscall, e.g. in kernel/syscall.c make sys_read on a file
#      ignore the per-fd offset:   f->pos += n;   ->   (nothing)
#      make -s kernel.elf && ./tools/checks/usertest-check.sh
#      -> FAIL: "a second read() did not report end of file; the per-fd
#         offset is not advancing"
#   2. Restore the line, rebuild, rerun -> PASS again.
#   3. Same again for the security shape: delete sys_read's
#      `if (!paging_user_range_ok(buf, len)) return -EFAULT;`
#      -> FAIL: "read() accepted a kernel destination buffer instead of
#         returning -EFAULT"
#      Restore it -> PASS again.
# All four halves were actually run when this check was written, not
# assumed, and both lines are restored in this tree.
