#!/bin/sh
# The brand mark exists in two real places and they drift: landing/icon.svg
# is the source, icon.svg at the repo root is what README.md renders.
# (The Mac app and its .icns live in nulljosh/joshuatree-monitor now.)
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

echo "PASS: both copies of the brand mark are in sync"
