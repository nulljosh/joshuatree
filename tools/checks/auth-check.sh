#!/bin/sh
# Compiles kernel/auth.h natively (host clang, a tiny in-memory VFS + GUI
# stand-ins in tools/auth-host/main.c) and runs a real, discriminating
# test suite against it. Same fast-inner-loop relationship
# tools/png-host-check.sh has to drivers/png.c: seconds, no QEMU, but the
# header this compiles is the exact one kernel/kernel.c includes and
# wires into gui_run() via auth_gate(), not a separate copy.
#
# What's actually proven here, not just asserted:
#   - PBKDF2-HMAC-SHA256 (kernel/auth_kdf.c, RFC 8018 loop over BearSSL's
#     br_hmac) matches the RFC 7914 section 11 vectors and the RFC 6070
#     set recomputed for SHA-256, byte for byte.
#   - A pre-1.7.9 untagged record logs in, is rewritten as a tagged PBKDF2
#     record on that login only, and verifies again from the rewritten
#     line; unknown tags and out-of-range iteration counts are skipped.
#   - SHA-256 itself is correct: the three real FIPS 180-4 published test
#     vectors (empty string, "abc", and a 56-byte multi-block message, so
#     both the single-block and the pad-into-a-second-block paths are
#     covered), byte for byte.
#   - A wrong password is really rejected and the right one really
#     accepted, including a same-length-prefix password (proves it's not
#     doing a substring/prefix compare by accident) and an unknown
#     username.
#   - The salted hash really round-trips through USERS.TXT's own hex
#     format (reload from a fresh "disk" read, not just the in-memory
#     struct that just created it).
#   - Password change: wrong current password refused, right one
#     succeeds, and the OLD password stops working afterward (proves the
#     salt was actually rotated and re-saved, not left stale).
#   - USERS.TXT bounds/parse hardening: three deliberately malformed
#     lines (missing a field, non-hex salt, a one-char-short hash) are
#     all skipped rather than crashing the parser or being trusted
#     partially.
#   - Constant-time compare actually distinguishes equal from
#     one-byte-different input, not just a name that claims it does.
set -e
cd "$(dirname "$0")/../.."
# kernel/auth_kdf.c and the vendored BearSSL SHA-256/HMAC it sits on are
# compiled natively alongside, so the PBKDF2 bytes the vectors pin are the
# same ones the kernel links.
clang -O2 -Wall -Wextra -Itools/auth-host -Ikernel -Ithird_party/bearssl/inc -Ithird_party/bearssl/src \
    -o /tmp/jt-auth-host tools/auth-host/main.c kernel/auth_kdf.c \
    third_party/bearssl/src/sha2small.c third_party/bearssl/src/hmac.c \
    third_party/bearssl/src/dec32be.c third_party/bearssl/src/enc32be.c
/tmp/jt-auth-host
