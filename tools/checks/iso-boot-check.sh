#!/bin/sh
# Proves `make iso` produces a joshuatree.iso that boots this exact
# kernel to the GUI two ways: as a CD-ROM (the ISO's El Torito boot
# catalog, BIOS path) and as a raw disk (the same file, isohybrid MBR,
# the path a real USB stick written with `dd` takes). Real hardware and
# UEFI firmware are NOT covered here (see README); this only proves the
# BIOS boot path QEMU emulates, headless, the same way check.sh proves
# the -kernel path.
#
# Same marker check.sh already uses: a plain RAM address (0x9000) the
# kernel writes right before switching the VGA card into graphics mode,
# read back over the QEMU monitor instead of trusted from a screendump
# (screendump is broken headless, see CLAUDE.md).
set -e
cd "$(dirname "$0")/../.."
make -s iso

boot_and_check() {
    label="$1"; shift
    out=$( (sleep 3; echo 'xp /1xw 0x9000'; sleep 1; echo quit) \
           | qemu-system-i386 "$@" -display none -vga std -monitor stdio 2>&1 | tr '\r' '\n')
    if echo "$out" | grep -qi '0xb007c0de'; then
        echo "PASS: $label reached gui_run"
    else
        echo "FAIL: $label boot marker not found"
        echo "$out" | tail -20
        exit 1
    fi
}

boot_and_check "CD-ROM boot (-cdrom)" -cdrom joshuatree.iso
boot_and_check "USB/raw-disk boot (-drive format=raw, same file, isohybrid MBR)" \
    -drive file=joshuatree.iso,format=raw

# A video card that is NOT the Bochs adapter, the stand-in for a real PC's GPU:
# the kernel cannot set a mode there itself, so the desktop only appears if it
# picked up the framebuffer Limine set (multiboot info, flag bit 12). "present"
# is klog's line for a frame actually reaching the screen.
log=$(mktemp)
(sleep 20; echo quit) | qemu-system-i386 -cdrom joshuatree.iso -display none -vga virtio \
    -serial "file:$log" -monitor stdio >/dev/null 2>&1
if grep -q '^present' "$log"; then
    echo "PASS: non-Bochs video (-vga virtio) drew the desktop from the bootloader's framebuffer"
else
    echo "FAIL: non-Bochs video never presented a frame"; tail -5 "$log"; rm -f "$log"; exit 1
fi
rm -f "$log"
