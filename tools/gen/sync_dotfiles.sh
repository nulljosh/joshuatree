#!/bin/bash
# Copy tracked, non-secret dotfiles into Joshua Tree's FAT16 disk. Fish and
# macOS tools cannot execute inside this kernel; these are readable files.
set -euo pipefail
cd "$(dirname "$0")"
SOURCE="${DOTFILES_REPO:-$HOME/Documents/Code/dotfiles}"
DISK="$PWD/dotfiles.img"
[[ -d "$SOURCE/.git" ]] || { echo "dotfiles checkout not found: $SOURCE" >&2; exit 1; }

if [[ ! -f "$DISK" ]]; then
    dd if=/dev/zero of="$DISK" bs=1m count=16 >/dev/null 2>&1
    DEV=$(hdiutil attach -nomount "$DISK" | awk '/\/dev\/disk[0-9]+/ {print $1; exit}')
    [[ -n "$DEV" ]] || { echo "could not attach dotfiles disk" >&2; exit 1; }
    newfs_msdos -F 16 -v JTDOT "$DEV" >/dev/null
    hdiutil detach "$DEV" >/dev/null
fi

MOUNT=$(mktemp -d /tmp/jt-dotfiles-XXXX)
cleanup() {
    hdiutil detach "$MOUNT" >/dev/null 2>&1 || true
    rmdir "$MOUNT" >/dev/null 2>&1 || true
}
trap cleanup EXIT
hdiutil attach -nobrowse -mountpoint "$MOUNT" "$DISK" >/dev/null

# FAT16 in this kernel understands 8.3 names. Keep an explicit allowlist so
# ~/.config/fish/secrets.fish and other untracked credentials never enter it.
cp "$SOURCE/fish/config.fish" "$MOUNT/FISH.CFG"
cp "$SOURCE/starship/starship.toml" "$MOUNT/STARSHIP.CFG"
cp "$SOURCE/ghostty/config" "$MOUNT/GHOSTTY.CFG"
cp "$SOURCE/cmux/cmux.json" "$MOUNT/CMUX.JSON"
cp "$SOURCE/git/.gitconfig" "$MOUNT/GIT.CFG"
cp "$SOURCE/README.md" "$MOUNT/ABOUT.TXT"
printf 'Source: nulljosh/dotfiles\nFiles are readable here; Fish and macOS apps do not run in Joshua Tree yet.\n' > "$MOUNT/START.TXT"
sync
echo "synced tracked dotfiles to $DISK"
