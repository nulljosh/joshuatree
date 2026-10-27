#!/bin/sh
# Fuzzes arch/arm64/uimg.h's app image check on the host under ASan and UBSan (tools/fuzz-host/fuzz_uimg.c): since 6.51.0
# an app can come off the card, so the image is untrusted. It must still pass a good image. Seconds, no QEMU.
set -e
cd "$(dirname "$0")/../.."
dir=$(mktemp -d); trap 'rm -rf "$dir"' EXIT
clang -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined -Wall -Wextra -Wno-unused-function -Iarch/arm64 -o "$dir/uimg-fuzz" tools/fuzz-host/fuzz_uimg.c
"$dir/uimg-fuzz"
