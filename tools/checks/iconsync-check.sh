#!/bin/sh
# The brand mark exists in three real places and they drift: landing/icon.svg
# is the source, icon.svg at the repo root is what README.md renders, and
# menubar/JoshuaTree.app's icon.icns is generated from the source by
# menubar/build-app-icon.sh.
#
# Real drift caught this the hard way: an icon rebuild updated only
# landing/icon.svg, so the app and the landing page showed the new mark
# while GitHub kept rendering the old gradient-heavy one from the root copy.
# Same class versionsync-check.sh guards for VERSION and landing/version.txt.
set -e
cd "$(dirname "$0")/../.."

if ! cmp -s landing/icon.svg icon.svg; then
  echo "FAIL: icon.svg at the repo root (what README renders) differs from landing/icon.svg"
  echo "  fix: cp landing/icon.svg icon.svg"
  exit 1
fi

if [ ! -f menubar/JoshuaTree.app/Contents/Resources/icon.icns ]; then
  echo "FAIL: the app bundle has no icon.icns"
  exit 1
fi

# The .icns is a binary render, so it cannot be compared byte for byte
# against the SVG. Mtime is the honest check available: if the source is
# newer than the render, the render is stale.
if [ landing/icon.svg -nt menubar/JoshuaTree.app/Contents/Resources/icon.icns ]; then
  echo "FAIL: icon.icns is older than landing/icon.svg, the app icon is stale"
  echo "  fix: menubar/build-app-icon.sh"
  exit 1
fi

echo "PASS: all three copies of the brand mark are in sync"
