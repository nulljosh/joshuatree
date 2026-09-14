#!/bin/sh
# Boots the kernel and asserts it reached the end of kmain's boot sequence.
# ponytail: one check. Used to assert on the VGA text banner at 0xB8000
# directly, until the kernel started booting straight into gui_run() by
# default: that switches the VGA card into a real graphics mode, which
# reprograms the Graphics Controller's memory-map select register and
# changes what physical address 0xB8000 even means, confirmed directly (a
# real `xp` read moments after boot showed 0xffff, not the banner, not
# assumed from the video-mode docs alone). A plain RAM marker written just
# before the mode switch survives it untouched.
set -e
make -s kernel.elf
out=$( (sleep 2; echo 'xp /1xw 0x9000'; sleep 1; echo quit) \
       | qemu-system-i386 -kernel kernel.elf -display none -monitor stdio 2>&1 | tr '\r' '\n' )
echo "$out" | grep -qi '0xb007c0de' \
  && echo "PASS: booted, reached gui_run" \
  || { echo "FAIL: boot marker not found"; exit 1; }
