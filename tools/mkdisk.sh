#!/bin/bash
# Creates a fresh, empty, real FAT16 disk image for app-interact-check.py
# to boot with, same mechanism sync_dotfiles.sh already uses (hdiutil +
# newfs_msdos), just blank instead of pre-seeded, so a real save/read
# round trip through this kernel's own fat.c/vfs.c is what proves a
# file's content, not this script inspecting RAM or trusting the UI.
set -euo pipefail
DISK="${1:-/tmp/jt-qa-test.img}"
rm -f "$DISK"
dd if=/dev/zero of="$DISK" bs=1m count=16 >/dev/null 2>&1
DEV=$(hdiutil attach -nomount "$DISK" | awk '/\/dev\/disk[0-9]+/ {print $1; exit}')
[[ -n "$DEV" ]] || { echo "could not attach $DISK" >&2; exit 1; }
newfs_msdos -F 16 -v JTQATEST "$DEV" >/dev/null
hdiutil detach "$DEV" >/dev/null
echo "created $DISK"
