#!/usr/bin/env bash
# Proves tools/bench.sh still works end to end: the kernel runs every
# benchmark and reports a nonzero number for each. Values themselves are
# host-dependent and not asserted.
set -euo pipefail
cd "$(dirname "$0")/../.."
out=$(./tools/bench.sh)
fail=0
for name in boot_to_shell heap_alloc_free memcpy context_switch disk_read; do
  v=$(echo "$out" | awk -v n="$name" '$2==n {print $4}')
  if [ -z "$v" ] || [ "$v" = "0" ]; then echo "bench-check: $name missing or zero"; fail=1; fi
done
echo "$out"
exit $fail
