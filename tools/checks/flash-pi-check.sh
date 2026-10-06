#!/bin/bash
# tools/flash-pi.sh against a temp dir standing in for the card, with fake
# firmware files so no network is needed. Proves: the kernel, the five
# firmware files and tools/pi-config.txt land on the card, and an existing
# kernel8.img and config.txt are kept as .bak first.
set -euo pipefail
cd "$(dirname "$0")/../.."
command -v ld.lld >/dev/null && command -v clang >/dev/null || { echo "SKIP: clang or ld.lld not installed"; exit 0; }
t=$(mktemp -d); trap 'rm -rf "$t"' EXIT
mkdir -p "$t/fw/overlays" "$t/card"
for f in bootcode.bin start4.elf fixup4.dat bcm2711-rpi-4-b.dtb overlays/disable-bt.dtbo; do echo "fake $f" > "$t/fw/$f"; done
echo old-kernel > "$t/card/kernel8.img"; echo old-config > "$t/card/config.txt"

FLASH_PI_FW="$t/fw" FLASH_PI_NO_EJECT=1 tools/flash-pi.sh "$t/card" >/dev/null

fail=0
for f in bootcode.bin start4.elf fixup4.dat bcm2711-rpi-4-b.dtb overlays/disable-bt.dtbo; do
    cmp -s "$t/fw/$f" "$t/card/$f" || { echo "FAIL: $f not copied"; fail=1; }
done
cmp -s arch/arm64/kernel8.img "$t/card/kernel8.img" || { echo "FAIL: kernel8.img is not the fresh build"; fail=1; }
cmp -s tools/pi-config.txt "$t/card/config.txt" || { echo "FAIL: config.txt is not tools/pi-config.txt"; fail=1; }
grep -qx 'dtoverlay=disable-bt' "$t/card/config.txt" || { echo "FAIL: config.txt lost disable-bt"; fail=1; }
[ "$(cat "$t/card/kernel8.img.bak")" = old-kernel ] || { echo "FAIL: old kernel8.img not kept as .bak"; fail=1; }
[ "$(cat "$t/card/config.txt.bak")" = old-config ] || { echo "FAIL: old config.txt not kept as .bak"; fail=1; }
[ "$fail" = 0 ] && echo "PASS: flash-pi.sh writes the kernel, firmware and config, and backs up what was there"
exit "$fail"
