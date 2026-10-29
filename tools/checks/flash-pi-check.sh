#!/bin/bash
# tools/flash-pi.sh against a temp dir standing in for the card, with fake
# firmware files so no network is needed. Proves: the kernel, the four
# firmware files and tools/pi-config.txt land on the card, and an existing
# kernel8.img and config.txt are kept as .bak first. Since 2.51.0 the card has
# no disable-bt overlay: Bluetooth needs the PL011 and the console uses the mini
# UART, whose clock enable_uart=1 holds steady.
set -euo pipefail
cd "$(dirname "$0")/../.."
command -v ld.lld >/dev/null && command -v clang >/dev/null || { echo "SKIP: clang or ld.lld not installed"; exit 0; }
t=$(mktemp -d); trap 'rm -rf "$t"' EXIT
mkdir -p "$t/fw" "$t/card"
for f in bootcode.bin start4.elf fixup4.dat bcm2711-rpi-4-b.dtb; do echo "fake $f" > "$t/fw/$f"; done
echo old-kernel > "$t/card/kernel8.img"; echo old-config > "$t/card/config.txt"

FLASH_PI_FW="$t/fw" FLASH_PI_NO_EJECT=1 tools/flash-pi.sh "$t/card" >/dev/null

fail=0
for f in bootcode.bin start4.elf fixup4.dat bcm2711-rpi-4-b.dtb; do
    cmp -s "$t/fw/$f" "$t/card/$f" || { echo "FAIL: $f not copied"; fail=1; }
done
cmp -s arch/arm64/kernel8.img "$t/card/kernel8.img" || { echo "FAIL: kernel8.img is not the fresh build"; fail=1; }
cmp -s tools/pi-config.txt "$t/card/config.txt" || { echo "FAIL: config.txt is not tools/pi-config.txt"; fail=1; }
grep -q 'disable-bt' "$t/card/config.txt" && { echo "FAIL: config.txt still turns Bluetooth off (disable-bt)"; fail=1; }
grep -qx 'enable_uart=1' "$t/card/config.txt" || { echo "FAIL: config.txt lost enable_uart=1 (the mini UART console's steady clock)"; fail=1; }
[ "$(cat "$t/card/kernel8.img.bak")" = old-kernel ] || { echo "FAIL: old kernel8.img not kept as .bak"; fail=1; }
[ "$(cat "$t/card/config.txt.bak")" = old-config ] || { echo "FAIL: old config.txt not kept as .bak"; fail=1; }
[ -s "$t/card/JT/MUSIC/HELLO.WAV" ] || { echo "FAIL: an empty JT/MUSIC/ did not get the sample song"; fail=1; }
# a card that already has songs keeps exactly them
mkdir -p "$t/card2/JT/MUSIC"; echo mine > "$t/card2/JT/MUSIC/MINE.MP3"
# and is flashed as anyone's card but Joshua's: no signing key (7.1.0, landing/sdk.html)
JT_APP_KEY="$t/no-key.pem" FLASH_PI_FW="$t/fw" FLASH_PI_NO_EJECT=1 tools/flash-pi.sh "$t/card2" >/dev/null
[ "$(ls "$t/card2/JT/MUSIC")" = MINE.MP3 ] || { echo "FAIL: flash-pi.sh added a song next to Joshua's own"; fail=1; }
if [ -s "$t/card/JT/APPS/PLANKC.APP" ]; then   # Plank is here, so plankc was built
    [ -s "$t/card2/JT/APPS/PLANKC.APP" ] && [ -s "$t/card2/JT/PLANKC/RT.PK" ] && [ -s "$t/card2/JT/CODE.PK" ] \
        || { echo "FAIL: with no signing key the card went without plankc, so nobody else can build an app on the Pi"; fail=1; }
    [ -e "$t/card2/JT/APPS/PLANKC.SIG" ] && { echo "FAIL: plankc got a signature with no key"; fail=1; }
else
    echo "SKIP: no Plank here, so plankc on the card is not checked"
fi
[ "$fail" = 0 ] && echo "PASS: flash-pi.sh writes the kernel, firmware and config, and backs up what was there"
exit "$fail"
