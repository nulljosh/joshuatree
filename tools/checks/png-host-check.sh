#!/bin/sh
# Compiles drivers/png.c natively (host clang, shim heap/libc) and runs the
# pixel-for-pixel + fault-injection harness in tools/png-host/main.c.
# Seconds, no QEMU. The in-kernel `pngtest` (tools/checks/png-check.sh) is the one
# that proves the same code works inside the freestanding build; this is
# the fast inner loop for decoder work.
set -e
cd "$(dirname "$0")/.."
python3 tools/gen/gen_png_testdata.py >/dev/null
clang -O2 -Wall -Wextra -Itools/png-host -Idrivers -o /tmp/jt-png-host tools/png-host/main.c drivers/png.c
/tmp/jt-png-host
