#!/bin/sh
# Boots the kernel and asserts the banner reached VGA memory. ponytail: one check.
set -e
make -s kernel.elf
out=$( (sleep 2; echo 'xp /8xh 0x000b8000'; sleep 1; echo quit) \
       | qemu-system-i386 -kernel kernel.elf -display none -monitor stdio 2>&1 | tr '\r' '\n' )
echo "$out" | grep -q '0x076a 0x076f 0x0773' \
  && echo "PASS: booted, banner in VGA memory" \
  || { echo "FAIL: no banner"; exit 1; }
