#!/bin/sh
# Fuzzes arch/arm64/appsig.c, the app signature check (6.54.0), on the host under ASan and UBSan with the vendored BearSSL
# (tools/fuzz-host/fuzz_appsig.c): NAME.SIG is a file anyone can put on the card. A throwaway key signs a small image through
# tools/signapp.sh, so the signer and the kernel's check are proven to agree; then every app in the store (landing/apps) must be
# signed by the shipped key. Skips (exit 0) without openssl. Under a minute.
set -e
cd "$(dirname "$0")/../.."
command -v openssl >/dev/null || { echo "SKIP: openssl not installed"; exit 0; }
dir=$(mktemp -d); trap 'rm -rf "$dir"' EXIT
CC="clang -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined -DBR_INT128=0 -DBR_UMUL128=0"
for f in third_party/bearssl/src/*.c; do
    $CC -w -Ithird_party/bearssl/inc -Ithird_party/bearssl/src -c "$f" -o "$dir/$(basename "$f" .c).o" &
done
wait
ar rcs "$dir/libbearssl.a" "$dir"/*.o
tools/signapp.sh --new-key "$dir/key.pem" "$dir/key.h" >/dev/null
head -c 5000 /dev/urandom > "$dir/FUZZ.APP"
tools/signapp.sh "$dir/FUZZ.APP" "$dir/key.pem" >/dev/null
$CC -Wall -Wextra -Iarch/arm64 -Ithird_party/bearssl/inc -DAPP_KEY_H="\"$dir/key.h\"" -o "$dir/appsig-fuzz" tools/fuzz-host/fuzz_appsig.c arch/arm64/appsig.c "$dir/libbearssl.a"
"$dir/appsig-fuzz" "$dir/FUZZ.APP" "$dir/FUZZ.SIG" fuzz
# 6.55.0: every app in the store (landing/apps/) is signed by the key the kernel ships with, arch/arm64/app_key.h, and its
# signature stands up to the same cuts and mutations.
$CC -Wall -Wextra -Iarch/arm64 -Ithird_party/bearssl/inc -o "$dir/store-verify" tools/fuzz-host/fuzz_appsig.c arch/arm64/appsig.c "$dir/libbearssl.a"
for app in landing/apps/*.app; do
    [ -f "$app" ] || continue
    name=$(basename "$app" .app)
    "$dir/store-verify" "$app" "landing/apps/$name.sig" "$name" >/dev/null 2>&1 || { echo "FAIL: landing/apps/$name.app is not signed by arch/arm64/app_key.h's key (tools/store-add.sh signs it)"; exit 1; }
    grep -q "^$name " landing/apps/index.txt || { echo "FAIL: landing/apps/index.txt has no line for $name"; exit 1; }
    echo "store: $name signed by the shipped key"
done
