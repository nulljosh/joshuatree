#!/usr/bin/env bash
# Host test for the AVI/MJPEG reader: real JPEG frames through the real kernel decoder, chunk order,
# bad headers, truncation and a mutation fuzz, under ASan+UBSan. Seconds, no QEMU.
set -euo pipefail
cd "$(dirname "$0")/../.."
T="$(mktemp -d)"; trap 'rm -rf "$T"' EXIT
python3 tools/media/avigen.py "$T/fx"
clang -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=undefined -Wall -Wextra \
    -Itools/jpeg-host -Idrivers tools/media/avi-host.c user/libjt/avi.c drivers/jpeg.c -o "$T/avi-host"
"$T/avi-host" "$T/fx"
