#!/bin/bash
# Regenerates JoshuaTree.app/Contents/Resources/icon.icns from the real
# landing/icon.svg, so the app's Dock icon stays in sync with the actual
# brand mark instead of going stale. Run whenever icon.svg changes.
set -e
cd "$(dirname "$0")/.."

ICONSET=$(mktemp -d)/JoshuaTree.iconset
mkdir -p "$ICONSET"
for size in 16 32 128 256 512; do
    rsvg-convert -w "$size" -h "$size" landing/icon.svg -o "$ICONSET/icon_${size}x${size}.png"
    double=$((size * 2))
    rsvg-convert -w "$double" -h "$double" landing/icon.svg -o "$ICONSET/icon_${size}x${size}@2x.png"
done
iconutil -c icns "$ICONSET" -o menubar/JoshuaTree.app/Contents/Resources/icon.icns
rm -rf "$(dirname "$ICONSET")"

touch menubar/JoshuaTree.app
xattr -cr menubar/JoshuaTree.app
echo "icon.icns regenerated from landing/icon.svg"
