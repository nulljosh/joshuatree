#!/bin/sh
# Compiles drivers/jpeg.c natively (host clang, shim heap/libc) and runs
# the tolerance + fault-injection harness in tools/jpeg-host/main.c.
# Seconds, no QEMU. The in-kernel `jpegtest` (tools/checks/jpeg-check.sh)
# is the one that proves the same code works inside the freestanding
# build; this is the fast inner loop for decoder work, and the one that
# checks correctness against an independent decoder (PIL), within the
# documented lossy tolerance (see tools/jpeg-host/main.c).
set -e
cd "$(dirname "$0")/../.."
python3 tools/gen/gen_jpeg_testdata.py >/dev/null
clang -O2 -Wall -Wextra -Itools/jpeg-host -Idrivers -o /tmp/jt-jpeg-host tools/jpeg-host/main.c drivers/jpeg.c
/tmp/jt-jpeg-host
