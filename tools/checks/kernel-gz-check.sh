#!/bin/bash
# The landing demo downloads the kernel once, gzipped: the build writes kernel.elf.gz, embed.js fetches it
# and hands v86 the buffer, and nothing fetches the raw kernel.elf a second time on the happy path.
set -e
cd "$(git rev-parse --show-toplevel)"
fail() { echo "FAIL: $1"; exit 1; }
grep -q 'gzip -9nc kernel.elf > landing/v86/kernel.elf.gz' Makefile || fail "Makefile no longer writes landing/v86/kernel.elf.gz"
grep -q 'fetch("v86/kernel.elf.gz")' landing/v86/embed.js || fail "embed.js no longer fetches the gzipped kernel"
grep -q 'DecompressionStream("gzip")' landing/v86/embed.js || fail "embed.js no longer decompresses in the browser"
grep -q 'multiboot: buf ? { buffer: buf.slice(0) }' landing/v86/embed.js || fail "v86 is no longer handed the fetched buffer (the kernel would download twice)"
grep -qx 'landing/v86/kernel.elf.gz' .gitignore || fail "kernel.elf.gz is not gitignored"
echo "kernel-gz-check: OK, one gzipped kernel download, handed straight to the emulator"
