#!/bin/sh
# Fetches the three CYW43455 firmware files the Pi 4's Wi-Fi chip needs into build/wifi-fw/ (gitignored), pinned
# to one RPi-Distro/firmware-nonfree commit so the bytes never drift under us. All four downloads land in a temp
# dir and move in together, like flash-pi.sh does for the boot firmware: a dropped download never leaves a half
# file behind. The files are Cypress's, redistributable under the terms in `copyright` next to them, and not ours:
# they are never committed. Used by `make -C arch/arm64 wifi-fw` (the `pi` target depends on it) and flash-pi.sh.
set -eu
D=${1:-build/wifi-fw}
C=c91cd2804cf7463aab913e7247c176049f16bbd6
R=https://raw.githubusercontent.com/RPi-Distro/firmware-nonfree/$C
P=debian/config/brcm80211
# dest name : path at that commit : minimum sane size. At $C the Pi 4 B NVRAM
# (brcmfmac43455-sdio.raspberrypi,4-model-b.txt) is a symlink to brcmfmac43455-sdio.txt, so fetch the target.
SET="brcmfmac43455-sdio.bin:$P/cypress/cyfmac43455-sdio-standard.bin:500000
brcmfmac43455-sdio.txt:$P/brcm/brcmfmac43455-sdio.txt:1500
brcmfmac43455-sdio.clm_blob:$P/cypress/cyfmac43455-sdio.clm_blob:2000
copyright:debian/copyright:100000"
ok=1; for s in $SET; do [ -s "$D/${s%%:*}" ] || ok=0; done
[ "$ok" = 1 ] && exit 0
tmp=$(mktemp -d)
for s in $SET; do
    n=${s%%:*}; rest=${s#*:}; src=${rest%:*}; min=${rest##*:}
    curl -sSfL -o "$tmp/$n" "$R/$src"
    sz=$(wc -c < "$tmp/$n")
    if [ "$sz" -lt "$min" ]; then echo "wifi-fw: $n is $sz bytes, expected at least $min (wrong path at $C?)" >&2; rm -rf "$tmp"; exit 1; fi
done
rm -rf "$D"; mkdir -p "$(dirname "$D")"; mv "$tmp" "$D"
echo "wifi-fw: $(wc -c < "$D/brcmfmac43455-sdio.bin") byte firmware in $D (RPi-Distro/firmware-nonfree @ ${C%${C#???????}})"
