#!/bin/sh
# Compiles libjt's string.c and stdlib.c natively (host clang, real host
# libc underneath) and runs tools/libjt-host/main.c, which calls each
# libjt function and the host's own libc function on the same inputs and
# diffs the results. Seconds, no QEMU -- the freestanding in-target build
# (user/libjt.a, wired into user/wc.bin by the Makefile) is the one that
# proves the same code works against the real syscalls; this is the fast
# inner loop for libjt work, same pattern as tools/checks/png-host-check.sh.
set -e
cd "$(dirname "$0")/../.."
# -fno-builtin: clang applies its own semantic knowledge to functions
# literally named malloc/free/calloc/realloc (e.g. that two malloc()
# results never alias), which is true of the host libc's allocator but
# not of libjt's own -- it reuses freed blocks on purpose -- and without
# this flag the optimizer silently "proved" a freed block's reuse false
# at -O1 and above, purely from the function names. Caught here, not in
# the kernel target build, because that one is -ffreestanding, which
# already implies -fno-builtin.
clang -O2 -Wall -Wextra -fno-builtin -Iuser -Iuser/libjt -o /tmp/jt-libjt-host \
    tools/libjt-host/main.c user/libjt/string.c user/libjt/stdlib.c
/tmp/jt-libjt-host
