#!/bin/sh
# A Pi release image carries no relay token: arch/arm64/pi-ask.o built without JT_WIFI_DEV=1 has no trace of the
# token file's bytes, and the same object built with JT_WIFI_DEV=1 (the dev card, tools/flash-pi.sh) does.
# Host-only: compiles one object twice against a throwaway token file. Fails if the Makefile stops passing
# -DCLAUDE_RELEASE or ask.c stops honouring it.
set -eu
cd "$(dirname "$0")/../.."
command -v clang >/dev/null || { echo "SKIP: no clang"; exit 0; }
tmp=$(mktemp -d); trap 'rm -rf "$tmp"; make -C arch/arm64 clean >/dev/null 2>&1' EXIT
tok=pi-release-check-token-0123456789abcdef
printf '%s\n' "$tok" > "$tmp/token"
export CLAUDE_RELAY_TOKEN_FILE=$tmp/token CLAUDE_RELAY_HOST=10.0.2.2 CLAUDE_RELAY_PORT=8765
rm -f arch/arm64/pi-ask.o arch/arm64/claude_cfg.h
make -C arch/arm64 pi-ask.o >/dev/null 2>&1
if LC_ALL=C grep -q "$tok" arch/arm64/pi-ask.o; then echo "FAIL: a Pi build without JT_WIFI_DEV=1 still carries the relay token"; exit 1; fi
rm -f arch/arm64/pi-ask.o
JT_WIFI_DEV=1 make -C arch/arm64 pi-ask.o >/dev/null 2>&1
if ! LC_ALL=C grep -q "$tok" arch/arm64/pi-ask.o; then echo "FAIL: the dev build (JT_WIFI_DEV=1) lost the relay token, so the control is broken"; exit 1; fi
echo "PASS: a Pi release build carries no relay token; the dev card build (JT_WIFI_DEV=1) does"
