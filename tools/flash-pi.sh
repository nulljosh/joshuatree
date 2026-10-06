#!/bin/bash
# Puts Joshua Tree on a microSD card for a Raspberry Pi 4.
#
# No Raspberry Pi OS needed. The Pi's firmware boots any kernel8.img it
# finds on a FAT32 card next to its own five boot files, so this script
# builds ours, fetches those five from the official raspberrypi/firmware
# repo, writes config.txt, and copies the lot onto the card. First done
# by hand on 2026-10-06; this is that session as a script.
#
# Usage: tools/flash-pi.sh [/Volumes/CARD]
# With no argument it looks for exactly one mounted FAT32 removable
# volume. It never formats anything: a fresh card is already FAT32, and
# for a used one `diskutil eraseDisk FAT32 PI MBRFormat diskN` first.
set -euo pipefail
cd "$(dirname "$0")/.."

CARD="${1:-}"
if [ -z "$CARD" ]; then
    # ponytail: macOS only for auto-detect; Linux passes the mount point.
    CARD=$(mount | awk '/\(msdos/ && /\/Volumes\// {sub(/^.* on /, ""); sub(/ \(msdos.*$/, ""); print}')
    [ "$(printf '%s\n' "$CARD" | grep -c .)" = 1 ] || { echo "pass the card's mount point, e.g. tools/flash-pi.sh \"/Volumes/NO NAME\"" >&2; exit 1; }
fi
[ -d "$CARD" ] || { echo "$CARD is not mounted" >&2; exit 1; }

make -C arch/arm64 pi >/dev/null

FW=build/pifw
B=https://github.com/raspberrypi/firmware/raw/stable/boot
mkdir -p "$FW/overlays"
for f in bootcode.bin start4.elf fixup4.dat bcm2711-rpi-4-b.dtb overlays/disable-bt.dtbo; do
    [ -s "$FW/$f" ] || curl -sSfL -o "$FW/$f" "$B/$f"
done

cp -R "$FW"/. "$CARD"/
cp arch/arm64/kernel8.img "$CARD"/kernel8.img
# disable-bt hands the good UART (PL011) to header pins 8 and 10.
# uart_2ndstage makes the firmware itself print first, so a blank
# serial line means wiring and a firmware-only line means our kernel.
printf 'arm_64bit=1\nkernel=kernel8.img\nenable_uart=1\ndtoverlay=disable-bt\nuart_2ndstage=1\n' > "$CARD"/config.txt

command -v dot_clean >/dev/null && dot_clean -m "$CARD"
if [ "$(uname -s)" = "Darwin" ]; then diskutil eject "$CARD"; else /bin/sync; fi
echo "flashed $(stat -f%z arch/arm64/kernel8.img 2>/dev/null || stat -c%s arch/arm64/kernel8.img) byte kernel8.img; card ejected"
