#!/usr/bin/env bash
# Host test for the media decoders (WAV now, MP3 and AVI/MJPEG as they land): golden output,
# malformed input, and a mutation fuzz, under ASan+UBSan. No QEMU, a few seconds.
set -euo pipefail
cd "$(dirname "$0")/../.."
T="$(mktemp -d)"; trap 'rm -rf "$T"' EXIT
python3 tools/media/wavgen.py "$T/fx"
clang -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=undefined -Wall -Wextra \
    tools/media/wav-host.c user/libjt/wav.c -o "$T/wav-host"
"$T/wav-host" "$T/fx"

clang -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=undefined -Wall -Wextra \
    -Ithird_party/minimp3 -Iuser tools/media/mp3-host.c user/libjt/mp3.c -o "$T/mp3-host"
"$T/mp3-host" tools/media/fixtures
