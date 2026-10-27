#!/bin/sh
# Fuzzes arch/arm64/mail.c's IMAP reply and MIME readers on the host under ASan and UBSan (tools/fuzz-host/fuzz_mail.c):
# a mail server's replies and every message's parts are chosen by whoever sends them. Every truncation and 8000
# fixed-seed mutations; a sanitizer report or a hang fails it. Seconds, no QEMU.
set -e
cd "$(dirname "$0")/../.."
dir=$(mktemp -d); trap 'rm -rf "$dir"' EXIT
clang -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined -Wall -Wextra -Wno-unused-function \
    -Wno-unused-parameter -Iarch/arm64 -o "$dir/mail-fuzz" tools/fuzz-host/fuzz_mail.c
"$dir/mail-fuzz"
