#!/bin/sh
# Fuzzes drivers/http.c, drivers/json.c and drivers/fat.c on the host under
# ASan+UBSan: all three parse untrusted bytes (an HTTP server's reply, a
# weather/chat JSON body, a mounted FAT16 disk image), so hostile input
# matters the same way it does for the PNG/JPEG decoders. Driver in
# tools/fuzz-host/fuzz_parsers.c: truncates every seed sample at every
# length (or ~200 spaced lengths for the FAT image), runs ~2000
# deterministic fixed-seed byte mutations per sample, and a few handcrafted
# nasties (truncated/malformed status lines, unterminated JSON strings, a
# boot sector with impossible geometry). No QEMU, seconds.
#
# drivers/net.c is deliberately NOT linked in: fuzz_parsers.c stubs its two
# functions http.c actually calls (dns_resolve, tcp_get_timeout) so the
# real network/NIC state machine never has to run on the host; the stub
# tcp_get_timeout hands the fuzzed bytes straight to http.c's own
# status-line/header parsing, which is the real target. drivers/fat.c is
# fuzzed against a small in-memory disk image served through the real
# drivers/blockdev.c registration API, the same one drivers/ramdisk.c uses.
set -e
cd "$(dirname "$0")/../.."

mkdir -p tools/checks/parser-fuzz-regress
rm -f tools/checks/parser-fuzz-regress/*.bin

clang -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined \
    -Wall -Wextra \
    -Idrivers -Itools/parsers-host \
    -o /tmp/jt-parser-fuzz \
    tools/fuzz-host/fuzz_parsers.c drivers/http.c drivers/json.c drivers/fat.c drivers/blockdev.c

# Per-input hangs are caught inside the harness via alarm(1) per case, so a
# stuck input fails loudly instead of hanging the whole run. (No `timeout`
# binary on stock macOS; the per-input alarm is the real safety net here.)
/tmp/jt-parser-fuzz
