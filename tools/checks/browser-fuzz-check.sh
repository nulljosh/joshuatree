#!/bin/sh
# Fuzzes arch/arm64/browser.c's page parser (to_text: tags, entities, <style> rules, forms and their fields) and its
# Set-Cookie reader on the host under ASan and UBSan (tools/fuzz-host/fuzz_browser.c): both read what any site sends.
# Every truncation and 8000 fixed-seed mutations; a sanitizer report or a hang fails it. Seconds, no QEMU.
set -e
cd "$(dirname "$0")/../.."
dir=$(mktemp -d); trap 'rm -rf "$dir"' EXIT
clang -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined -Wall -Wextra -Wno-unused-function \
    -Wno-unused-parameter -Iarch/arm64 -o "$dir/browser-fuzz" tools/fuzz-host/fuzz_browser.c
"$dir/browser-fuzz"
