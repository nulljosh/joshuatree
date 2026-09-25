#!/usr/bin/env bash
# 1.5.10: runs the same thing .github/workflows/check.yml runs, locally,
# before a draft PR goes ready. Direct request from Joshua, furious about
# a CI failure email landing on every single push while a PR is still
# being iterated on -- check.yml now skips every job on a draft PR (see
# its own comment), so this script is the replacement feedback loop: run
# it yourself, and only flip the PR ready once it's green.
#
# What it runs, matching check.yml job for job:
#   - `changes`/`suite` job: build kernel.elf, then all 4 shards of
#     tools/checks/ci-suite.sh, each in its own QEMU (-display none,
#     already how ci-suite.sh's own checks invoke QEMU), run in PARALLEL
#     rather than CI's 4 separate runners.
#   - `check-refs` job's three scripts: check-refs.sh, versionsync-check.sh,
#     version-bump-check.sh.
#   - `demo` job: demochat-check.mjs and cursorglide-check.mjs.
#
# Deliberately NOT run: the `network` job (continue-on-error in CI, talks
# to real internet hosts, never gates a merge -- see check.yml's own
# comment on it) and landing-roadmap-check.py (a pure-Python unittest
# already covered by ci-suite.sh's own checks reading the same generator).
#
# Needed tools (all available via Homebrew on macOS):
#   brew install qemu llvm lld node python3
#   python3 -m pip install pillow
# clang/lld: Apple's stock Xcode clang works too (this repo's own Makefile
# targets `-target i386-unknown-none`, no cross-toolchain needed -- see
# CLAUDE.md). qemu: `qemu-system-i386` on PATH. node: for the two
# Playwright checks (`npm install` pulls Playwright itself; Chromium comes
# down the first time `npx playwright install chromium` runs, this script
# does that once and reuses it). python3 + Pillow: several checks decode
# PNG/JPEG framebuffer dumps and diff them pixel-for-pixel.
#
# What this does NOT catch: CI runs on Linux (ubuntu-24.04), this runs on
# macOS. A Linux-only build difference -- the classic example here is a
# libm symbol (`-lm`) that Apple's linker resolves implicitly and lld on
# Linux does not -- will build clean locally and still fail in CI. This
# script is a fast, high-confidence local gate, not a full substitute for
# the real CI run; a genuinely Linux-specific regression is still CI's job
# to catch, and always will be until this repo runs Linux natively too.
set -uo pipefail
cd "$(dirname "$0")/.."
ROOT="$(pwd)"
START=$(date +%s)

PASS=0
FAIL=0
FAILED_NAMES=()

run_named() {
  local name="$1"; shift
  local start=$(date +%s)
  if "$@" > "/tmp/jt-ci-local-$$-$(echo "$name" | tr -c 'a-zA-Z0-9' '_').log" 2>&1; then
    local dur=$(( $(date +%s) - start ))
    echo "PASS ($dur s): $name"
    PASS=$((PASS + 1))
  else
    local dur=$(( $(date +%s) - start ))
    echo "FAIL ($dur s): $name"
    FAIL=$((FAIL + 1))
    FAILED_NAMES+=("$name")
  fi
}

echo "== tools/ci-local.sh: mirroring .github/workflows/check.yml locally =="
echo

echo "-- build --"
run_named "make kernel.elf" make kernel.elf
echo

echo "-- check-refs job --"
run_named "check-refs.sh" ./tools/checks/check-refs.sh
run_named "versionsync-check.sh" ./tools/checks/versionsync-check.sh
run_named "version-bump-check.sh (vs origin/main)" ./tools/checks/version-bump-check.sh origin/main
echo

echo "-- suite job: 4 shards in parallel, each its own QEMU (-display none) --"
SUITE_PIDS=()
SUITE_LOGS=()
for shard in 0 1 2 3; do
  log="/tmp/jt-ci-local-$$-shard${shard}.log"
  SUITE_LOGS+=("$log")
  ( SHARD=$shard SHARDS=4 ./tools/checks/ci-suite.sh > "$log" 2>&1 ) &
  SUITE_PIDS+=($!)
done
suite_start=$(date +%s)
suite_ok=1
for i in 0 1 2 3; do
  pid=${SUITE_PIDS[$i]}
  if wait "$pid"; then
    :
  else
    suite_ok=0
  fi
done
suite_dur=$(( $(date +%s) - suite_start ))
for i in 0 1 2 3; do
  echo "  shard $i log: ${SUITE_LOGS[$i]}"
  tail -n 3 "${SUITE_LOGS[$i]}" | sed 's/^/    /'
done
if [ "$suite_ok" = 1 ]; then
  echo "PASS ($suite_dur s): all 4 suite shards"
  PASS=$((PASS + 1))
else
  echo "FAIL ($suite_dur s): one or more suite shards"
  FAIL=$((FAIL + 1))
  FAILED_NAMES+=("suite shards (see logs above)")
fi
echo

echo "-- demo job --"
if [ ! -d node_modules ]; then
  run_named "npm install" npm install --no-audit --no-fund
fi
run_named "npx playwright install chromium" npx playwright install chromium
run_named "demochat-check.mjs" node tools/checks/demochat-check.mjs
run_named "cursorglide-check.mjs" node tools/checks/cursorglide-check.mjs
echo

TOTAL=$(( $(date +%s) - START ))
echo "== summary: $PASS passed, $FAIL failed, ${TOTAL}s wall time =="
if [ "$FAIL" -gt 0 ]; then
  echo "Failed:"
  for n in "${FAILED_NAMES[@]}"; do echo "  - $n"; done
  echo
  echo "Reminder: this ran on macOS. A Linux-only difference (e.g. a"
  echo "missing -lm that Apple's linker resolves implicitly) will still"
  echo "only show up in the real CI run."
  exit 1
fi
echo "All local checks passed. Real CI still runs on ready_for_review; this"
echo "is a fast, high-confidence local gate, not a substitute for it."
exit 0
