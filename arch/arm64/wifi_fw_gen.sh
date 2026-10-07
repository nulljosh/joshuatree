#!/bin/sh
# Writes wifi_fw.S (gitignored): .incbin of the three Cypress CYW43455 files from ../../build/wifi-fw/ when they are
# there (tools/flash-pi.sh downloads them, pinned to a raspberrypi/firmware-nonfree commit; the Cypress licence
# LICENCE.cypress sits alongside), else empty stubs so the Pi build still links and prints `wifi no firmware`.
# The firmware files are Cypress's, redistributable but not ours: never committed.
D=${1:-../../build/wifi-fw}; O=wifi_fw.S; : > $O
for s in bin:brcmfmac43455-sdio.bin nvram:brcmfmac43455-sdio.txt clm:brcmfmac43455-sdio.clm_blob; do
  n=${s%%:*}; f=$D/${s#*:}
  printf '.section .rodata\n.balign 64\n.globl wifi_fw_%s\nwifi_fw_%s:\n' $n $n >> $O
  if [ -f "$f" ]; then printf '.incbin "%s"\n1:\n.balign 4\n.globl wifi_fw_%s_len\nwifi_fw_%s_len: .word 1b - wifi_fw_%s\n' "$f" $n $n $n >> $O
  else printf '.byte 0\n.balign 4\n.globl wifi_fw_%s_len\nwifi_fw_%s_len: .word 0\n' $n $n >> $O; fi
done
