#!/bin/sh
# Fuzzes drivers/dns.h's DNS reply reader on the host under ASan and UBSan (tools/fuzz-host/fuzz_dns.c): any host on the
# network can answer a query. It must still read the good reply's address. Seconds, no QEMU.
set -e
cd "$(dirname "$0")/../.."
dir=$(mktemp -d); trap 'rm -rf "$dir"' EXIT
clang -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined -Wall -Wextra -Wno-unused-function -Idrivers -o "$dir/dns-fuzz" tools/fuzz-host/fuzz_dns.c
"$dir/dns-fuzz"
