#!/bin/sh
# Fuzzes arch/arm64/wifi_proto.h's scan-result and frame readers on the host under ASan and UBSan
# (tools/fuzz-host/fuzz_wifi.c): a nearby access point chooses its beacon's bytes. Every truncation and 10000 fixed-seed
# mutations; a sanitizer report fails it. Seconds, no QEMU.
set -e
cd "$(dirname "$0")/../.."
dir=$(mktemp -d); trap 'rm -rf "$dir"' EXIT
clang -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined -Wall -Wextra -Iarch/arm64 -o "$dir/wifi-fuzz" tools/fuzz-host/fuzz_wifi.c
"$dir/wifi-fuzz"
