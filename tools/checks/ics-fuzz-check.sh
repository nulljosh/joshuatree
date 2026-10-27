#!/bin/sh
# Fuzzes arch/arm64/ics.c's iCalendar and CalDAV parsers on the host under ASan and UBSan (tools/fuzz-host/fuzz_ics.c):
# they read what any feed or CalDAV server sends. Every truncation and 9000 fixed-seed mutations; a sanitizer report or a
# hang fails it. Seconds, no QEMU.
set -e
cd "$(dirname "$0")/../.."
dir=$(mktemp -d); trap 'rm -rf "$dir"' EXIT
clang -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined -Wall -Wextra -Wno-unused-function \
    -Iarch/arm64 -o "$dir/ics-fuzz" tools/fuzz-host/fuzz_ics.c
"$dir/ics-fuzz"
