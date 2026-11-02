#!/bin/sh
# Self-update's signature (9.3.0, arch/arm64/update.c). CI attaches kernel8.img and version.txt to each release but never
# holds the app key, so this runs on Joshua's Mac: it downloads the release's kernel8.img, signs SHA-256 of
# "JTAPP1\0os.image\0" IMAGE with the key (the name has a dot, so no signed app can pass as a kernel), and attaches
# kernel8.sig. A Pi refuses an update without it.
#   tools/update-sign.sh [TAG] [KEY.pem]   TAG defaults to the latest release, KEY to ~/.config/joshuatree/app-signing.pem
set -e
tag=${1:-$(gh release view --json tagName -q .tagName)}
key=${2:-$HOME/.config/joshuatree/app-signing.pem}
[ -f "$key" ] || { echo "update-sign: no key at $key"; exit 1; }
dir=$(mktemp -d); trap 'rm -rf "$dir"' EXIT
gh release download "$tag" -p kernel8.img -D "$dir"
{ printf 'JTAPP1\000os.image\000'; cat "$dir/kernel8.img"; } > "$dir/msg"
openssl dgst -sha256 -sign "$key" -out "$dir/kernel8.sig" "$dir/msg"
gh release upload "$tag" "$dir/kernel8.sig" --clobber
echo "update-sign: $tag's kernel8.img is signed; Pis on a release build can update to it"
