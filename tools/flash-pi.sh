#!/bin/bash
# Puts Joshua Tree on a microSD card for a Raspberry Pi 4.
#
# No Raspberry Pi OS needed. The Pi's firmware boots any kernel8.img it
# finds on a FAT32 card next to its own five boot files, so this script
# builds ours, fetches those five from the official raspberrypi/firmware
# repo, copies tools/pi-config.txt over as config.txt, and copies the lot
# onto the card. First done by hand on 2026-10-06; this is that session
# as a script. tools/checks/flash-pi-check.sh covers it.
#
# Usage: tools/flash-pi.sh [/Volumes/CARD]
# With no argument it looks for exactly one mounted FAT volume on an
# external disk. It never formats anything: a fresh card is already
# FAT32, and for a used one `diskutil eraseDisk FAT32 PI MBRFormat diskN`
# first. An existing kernel8.img or config.txt on the card is kept as
# .bak before it is replaced.
#
# FLASH_PI_FW=dir uses firmware files already in dir (the check uses
# this, no network). FLASH_PI_NO_EJECT=1 leaves the card mounted.
set -euo pipefail
cd "$(dirname "$0")/.."

CARD="${1:-}"
if [ -z "$CARD" ]; then
    # ponytail: macOS only for auto-detect; Linux passes the mount point.
    found=()
    for v in /Volumes/*; do
        info=$(diskutil info "$v" 2>/dev/null) || continue
        echo "$info" | grep -q 'File System Personality: *.*FAT' || continue
        echo "$info" | grep -q 'Device Location: *External' || continue
        found+=("$v")
    done
    [ "${#found[@]}" = 1 ] || { echo "found ${#found[@]} external FAT volumes; pass the card's mount point, e.g. tools/flash-pi.sh \"/Volumes/NO NAME\"" >&2; exit 1; }
    CARD="${found[0]}"
fi
[ -d "$CARD" ] || { echo "$CARD is not mounted" >&2; exit 1; }
echo "flashing $CARD"

JT_WIFI_DEV=1 make -B -C arch/arm64 pi >/dev/null   # JT_WIFI_DEV: this card joins our network; release builds never carry it   # -B: the Makefile does not track headers

FW="${FLASH_PI_FW:-build/pifw}"
FILES="bootcode.bin start4.elf fixup4.dat bcm2711-rpi-4-b.dtb overlays/disable-bt.dtbo"
missing=0; for f in $FILES; do [ -s "$FW/$f" ] || missing=1; done
if [ "$missing" = 1 ]; then
    # All five from one fetch, into a temp dir, moved in only when every one
    # landed: a dropped download never leaves a half file in the cache, and
    # start4.elf and fixup4.dat always come from the same firmware release.
    B=https://github.com/raspberrypi/firmware/raw/stable/boot
    tmp=$(mktemp -d); mkdir -p "$tmp/overlays"
    for f in $FILES; do curl -sSfL -o "$tmp/$f" "$B/$f"; done
    rm -rf "$FW"; mkdir -p "$(dirname "$FW")"; mv "$tmp" "$FW"
fi

tools/wifi-fw.sh   # CYW43455 firmware into build/wifi-fw/, baked into kernel8.img by the pi target
for f in kernel8.img config.txt; do [ -f "$CARD/$f" ] && cp "$CARD/$f" "$CARD/$f.bak"; done
cp -R "$FW"/. "$CARD"/
cp arch/arm64/kernel8.img "$CARD"/kernel8.img
cp tools/pi-config.txt "$CARD"/config.txt

command -v dot_clean >/dev/null && dot_clean -m "$CARD"
if [ "${FLASH_PI_NO_EJECT:-}" = 1 ]; then :
elif [ "$(uname -s)" = "Darwin" ]; then diskutil eject "$CARD"; else /bin/sync; fi
echo "flashed $(wc -c < arch/arm64/kernel8.img | tr -d ' ') byte kernel8.img"
