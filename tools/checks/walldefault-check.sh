#!/bin/bash
# v0.76.7: proves the compiled-in wallpaper default is really WALL_SAT (4),
# not just that the #define exists somewhere in the source. Reads the real
# `wall_theme` global straight out of a freshly-booted kernel's memory via
# the QEMU monitor, before any shell command or Settings click could have
# changed it -- the same "read the symbol, don't trust the source" standard
# editor_qa.py's Machine.integer() already established for this repo.
#
# Direct root cause this guards: Satellite became real and buildable in
# v0.73.1, but the static initializer stayed WALL_WARM until v0.76.7, so
# every fresh boot (and the browser demo's every idle-tour lap, which wipes
# state via a ramfs reset) kept showing the Warm map by default regardless
# of how correct the Satellite fetch/decode path itself was.
set -e
cd "$(dirname "$0")/../.."
make -s kernel.elf

# v0.76.9: plain `nm` (binutils), not `llvm-nm`. The first version of this
# script fell into the exact same portability trap editor_qa.py's llvm-nm
# path already got caught in once this session: it worked here because this
# dev container happens to have the separate `llvm` apt package installed,
# but CI's own runner only installs clang/lld (see check.yml's own apt line),
# which does NOT pull in llvm-nm -- CI failed with "No such file or
# directory" on the macOS-only fallback path the first version had. `nm`
# ships with `binutils`, already present on every Ubuntu image and every
# macOS Xcode CLT install with no extra apt line needed, and reads this
# freestanding ELF's symbol table identically (verified: same address/type/
# name fields for wall_theme from both llvm-nm and nm on this exact binary).
ADDR=$(nm kernel.elf | awk '$3 == "wall_theme" { print $1 }')
if [ -z "$ADDR" ]; then
    echo "FAIL: wall_theme symbol not found in kernel.elf"
    exit 1
fi
PHYS=$(printf '0x%x' $((0x$ADDR - 0xC0000000)))

WORKDIR=$(mktemp -d /tmp/jt-walldefault-XXXX)
trap 'rm -rf "$WORKDIR"' EXIT
LOG="$WORKDIR/log"

(
    sleep 3
    echo "xp /4xb $PHYS"
    sleep 1
    echo quit
) | qemu-system-i386 -kernel kernel.elf -display none -monitor stdio > "$LOG" 2>&1

# wall_theme is a 4-byte little-endian int; only the low byte matters here
# since the real range is 0..4.
BYTE0=$(grep -A1 "xp /4xb" "$LOG" | tail -1 | awk '{print $2}')

if [ "$BYTE0" = "0x04" ]; then
    echo "PASS: wall_theme defaults to WALL_SAT (4) on a fresh boot"
    exit 0
else
    echo "FAIL: wall_theme defaulted to $BYTE0, expected 0x04 (WALL_SAT)"
    cat "$LOG"
    exit 1
fi
