#!/bin/bash
# v0.76.12: direct request, "add version number after Joshua Tree header
# text, should be dynamic." landing/index.html now fetches
# landing/version.txt at page load and shows it next to the header,
# rather than hand-typing a version into the HTML (the same drift risk
# that left kernel.c's own About panel showing "Version 0.42.1" for
# 30+ real releases before this same pass found and fixed it). Real
# "dynamic" here means version.txt is a real static file the page really
# fetches, kept in sync with the repo-root VERSION the same way
# landing/v86/kernel.elf is already kept in sync with the repo-root
# kernel.elf after every kernel change -- a plain `cp`, not generated at
# request time. This is the guard that keeps that resync from silently
# drifting the same way the kernel-side one did.
set -e
cd "$(dirname "$0")/../.."

if [ ! -f landing/version.txt ]; then
    echo "FAIL: landing/version.txt does not exist"
    exit 1
fi

if diff -q VERSION landing/version.txt > /dev/null; then
    echo "PASS: landing/version.txt matches the real VERSION file ($(cat VERSION))"
    exit 0
else
    echo "FAIL: landing/version.txt ($(cat landing/version.txt)) does not match VERSION ($(cat VERSION)) -- run: cp VERSION landing/version.txt"
    exit 1
fi
