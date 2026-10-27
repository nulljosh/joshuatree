#!/bin/sh
# Put an app in the store (6.55.0): build FILE.c with tools/mkapp.sh, sign it with tools/signapp.sh (Joshua's key), and copy
# name.app and name.sig into landing/apps/, which the landing serves at joshuatree.jaybulb.com/apps/ once main deploys. The
# app's first comment line becomes its line in landing/apps/index.txt. On the Pi, `install NAME` fetches it.
# Usage: tools/store-add.sh store/NAME.c [KEY.pem]
set -e
src=$1
[ -f "$src" ] || { echo "usage: tools/store-add.sh FILE.c [KEY.pem]"; exit 1; }
root=$(cd "$(dirname "$0")/.." && pwd)
name=$(basename "$src" .c | tr A-Z a-z)
up=$(echo "$name" | tr a-z A-Z)
dir=$(mktemp -d); trap 'rm -rf "$dir"' EXIT
"$root/tools/mkapp.sh" "$src" "$dir/$up.APP" >/dev/null
"$root/tools/signapp.sh" "$dir/$up.APP" ${2:+"$2"} >/dev/null
mkdir -p "$root/landing/apps"
cp "$dir/$up.APP" "$root/landing/apps/$name.app"
cp "$dir/$up.SIG" "$root/landing/apps/$name.sig"
about=$(sed -n '1s|^/\* *||; 1s| *\*/ *$||; 1s|^[a-z0-9_-]*: *||; 1p' "$src")
idx=$root/landing/apps/index.txt
{ [ -f "$idx" ] && grep -v "^$name " "$idx"; echo "$name $about"; } | sort > "$dir/idx" && mv "$dir/idx" "$idx"
echo "store: $name is in landing/apps ($(wc -c < "$root/landing/apps/$name.app" | tr -d ' ') bytes, signed); it goes live when main deploys"
