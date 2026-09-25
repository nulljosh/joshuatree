#!/bin/sh
# Compiles drivers/ttf.c natively (host clang, real libm, kernel-heap shim)
# and compares glyph rasterization ("Ag" at 12, 48, 200px) against PIL's
# ImageFont rendering of the same DejaVuSans.ttf. Seconds, no QEMU.
# The in-kernel build (make kernel.elf) is what proves ttf.c links and runs
# freestanding; this is the fast inner loop that proves the rasterized
# shapes are actually right.
set -e
cd "$(dirname "$0")/../.."

clang -O2 -Wall -Wextra -DTTF_HOST_BUILD -Itools/ttf-host -Idrivers \
    -o /tmp/jt-ttf-host tools/ttf-host/main.c drivers/ttf.c -lm

status=0
for px in 12 48 200; do
    /tmp/jt-ttf-host "$px" /tmp/jt-ttf-A-$px.pgm /tmp/jt-ttf-g-$px.pgm
    python3 tools/ttf-host/compare.py "$px" tools/fonts/DejaVuSans-subset.ttf \
        /tmp/jt-ttf-A-$px.pgm /tmp/jt-ttf-g-$px.pgm || status=1
done
exit $status
