#!/bin/sh
# The app SDK (6.52.0): one C file into an app for the card. The file includes "jtsys.h" for the system calls (and any of
# libjt's headers, "libjt/string.h" say), and puts _start in .text.start; arch/arm64/Makefile builds it as it builds the
# built-in apps. Copy the result to JT/APPS/ on the card and `userrun NAME` runs it. See docs/GUIDE.md, Write an app.
# Usage: tools/mkapp.sh FILE.c [OUT.APP]   (OUT defaults to the file's name, upper case, in this folder)
set -e
src=$1
[ -f "$src" ] || { echo "usage: tools/mkapp.sh FILE.c [OUT.APP]"; exit 1; }
out=${2:-$(basename "$src" .c | tr a-z A-Z).APP}
name=$(basename "$out" .APP)
case "$name" in ''|?????????*|*[!A-Z0-9_-]*) echo "mkapp: $name.APP: the name must be 1 to 8 of A-Z, 0-9, - and _"; exit 1;; esac
root=$(cd "$(dirname "$0")/.." && pwd)
abs=$(cd "$(dirname "$src")" && pwd)/$(basename "$src")
make -s -C "$root/arch/arm64" uapp_sdk.bin SRC="$abs"
cp "$root/arch/arm64/uapp_sdk.bin" "$out"
rm -f "$root/arch/arm64/uapp_sdk.o" "$root/arch/arm64/uapp_sdk.bin"
echo "mkapp: made $out, $(wc -c < "$out" | tr -d ' ') bytes; copy it to JT/APPS/ on the card, then: userrun $(echo "$name" | tr A-Z a-z)"
