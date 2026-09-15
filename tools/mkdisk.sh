#!/bin/bash
# Creates a fresh, empty, real FAT16 disk image for app-interact-check.py
# to boot with, same mechanism sync_dotfiles.sh already uses on macOS
# (hdiutil + newfs_msdos), just blank instead of pre-seeded, so a real
# save/read round trip through this kernel's own fat.c/vfs.c is what
# proves a file's content, not this script inspecting RAM or trusting
# the UI.
#
# v0.76.2: also runs on Linux (mkfs.vfat -F 16, dosfstools/mtools), a
# real CI-coverage gap closed -- this script only ever had a macOS path
# before, so app-interact-check.py and fatcyclehang-check.sh could never
# run in GitHub Actions (ubuntu-latest) or any other Linux box. Picks the
# real tool per `uname`, not a hardcoded guess, so this stays the same
# script Joshua's own Mac already runs, not a fork of it.
set -euo pipefail
DISK="${1:-/tmp/jt-qa-test.img}"
rm -f "$DISK"
dd if=/dev/zero of="$DISK" bs=1M count=16 >/dev/null 2>&1

if [ "$(uname -s)" = "Darwin" ]; then
    DEV=$(hdiutil attach -nomount "$DISK" | awk '/\/dev\/disk[0-9]+/ {print $1; exit}')
    [[ -n "$DEV" ]] || { echo "could not attach $DISK" >&2; exit 1; }
    newfs_msdos -F 16 -v JTQATEST "$DEV" >/dev/null
    hdiutil detach "$DEV" >/dev/null
else
    mkfs.vfat -F 16 -n JTQATEST "$DISK" >/dev/null
fi
echo "created $DISK"
