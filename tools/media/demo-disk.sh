#!/usr/bin/env bash
# Builds a FAT disk with the three demo songs in MUSIC/: demo-disk.sh [IMG]. Boot it with
#   qemu-system-i386 -kernel kernel.elf -hda IMG -device sb16 ...
# The songs are generated, never committed, and never ride in the kernel image (its bss margin is small).
set -euo pipefail
cd "$(dirname "$0")/../.."
IMG="${1:-/tmp/jt-demo-music.img}"
T="$(mktemp -d)"; trap 'rm -rf "$T"' EXIT
python3 tools/media/demo-music.py "$T" >/dev/null
bash tools/mkdisk.sh "$IMG" >/dev/null
mmd -i "$IMG" ::MUSIC
mcopy -i "$IMG" "$T"/*.WAV ::MUSIC/
echo "$IMG"
