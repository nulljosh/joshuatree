#!/bin/sh
# Writes bt_fw.S (gitignored): .incbin of the Bluetooth patch firmware named by $1 (default ../../build/bt-fw/BCM4345C0.hcd,
# which tools/bt-fw.sh downloads from RPi-Distro/bluez-firmware, LICENSE.cypress alongside) when it is there, else an
# empty one, so every build links and bt.c says the radio runs its own firmware. The file is Cypress's, redistributable
# but not ours: never committed. Rewrites bt_fw.S only when its text changes, so an unchanged build never relinks.
F=${1:-../../build/bt-fw/BCM4345C0.hcd}; O=bt_fw.S; T=bt_fw.S.tmp
{
  printf '.section .rodata\n.balign 4\n.globl bt_fw_hcd\nbt_fw_hcd:\n'
  if [ -f "$F" ]; then printf '.incbin "%s/%s"\n' "$(cd "$(dirname "$F")" && pwd)" "$(basename "$F")"; fi
  printf '1:\n.balign 4\n.globl bt_fw_hcd_len\nbt_fw_hcd_len: .word 1b - bt_fw_hcd\n'
} > $T
if cmp -s $T $O; then rm -f $T; else mv $T $O; fi
