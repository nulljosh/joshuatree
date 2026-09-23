#!/bin/sh
# Fuzzes drivers/png.c and drivers/jpeg.c on the host under ASan+UBSan:
# these two decoders parse untrusted bytes off the network (wallpaper
# tiles, weather icons), so hostile input matters. Driver in
# tools/fuzz-host/fuzz_decoders.c: truncates every valid sample at every
# length (or ~200 spaced lengths for the larger JPEG fixtures), runs ~2000
# deterministic fixed-seed byte mutations per sample, and a few
# handcrafted nasties (huge/zero dimensions, bad chunk lengths, bogus
# Huffman/quant tables, a broken zlib stored block). No QEMU, seconds.
set -e
cd "$(dirname "$0")/../.."

python3 tools/gen/gen_png_testdata.py >/dev/null
python3 tools/gen/gen_jpeg_testdata.py >/dev/null

mkdir -p tools/checks/decoder-fuzz-regress
rm -f tools/checks/decoder-fuzz-regress/*.bin

clang -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined \
    -Wall -Wextra \
    -Idrivers -Itools/png-host \
    -o /tmp/jt-decoder-fuzz \
    tools/fuzz-host/fuzz_decoders.c drivers/png.c drivers/jpeg.c

# Per-input hangs are caught inside the harness via alarm(1) per case, so
# a stuck input fails loudly instead of hanging the whole run. (No `timeout`
# binary on stock macOS; the per-input alarm is the real safety net here.)
/tmp/jt-decoder-fuzz
