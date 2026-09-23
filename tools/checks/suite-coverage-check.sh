#!/usr/bin/env bash
# Drift insurance for tools/checks/ itself, the same idea check-refs.sh
# applies to docs: every *-check.* script must be accounted for, either
# wired into ci-suite.sh's manifest or carrying a MANUAL/HELPER marker
# explaining why it isn't. Without this, a new check can land, never get
# wired, and just silently sit there forever, unrun, which is exactly the
# ~41-orphan drift this guard was written to close (see the checks-triage
# branch that added it).
#
# A file is "accounted for" if:
#   - its basename appears in ci-suite.sh's manifest (WIRE), or
#   - its own first few lines contain a "MANUAL:" or "HELPER:" marker
#     comment explaining why it's intentionally not wired, or
#   - it's genuinely called by another check (grep across tools/checks/),
#     which makes it a HELPER even without its own marker comment.
#
# Only files matching *-check.* are covered: apptest.sh/guitest.sh-style
# one-off names aren't the convention this repo wires checks under, and
# forcing every stray script into this scheme would just invite a marker
# comment for its own sake. Deleting a dead check is still the right call
# when one turns up (see checks-triage's own report for what got deleted
# and why); this guard only stops new ones from being silently orphaned.
set -euo pipefail
cd "$(dirname "$0")/../.."

fail=0
for f in tools/checks/*-check.*; do
    [ -e "$f" ] || continue
    b=$(basename "$f")

    if grep -rqs "$b" tools/checks/ci-suite.sh tools/hooks .github; then
        continue
    fi
    if grep -qE "^\s*(#|//|\"\"\")\s*(MANUAL|HELPER):" "$f"; then
        continue
    fi
    if grep -rl "$b" tools/checks --include='*.sh' --include='*.py' --include='*.mjs' 2>/dev/null \
        | grep -qv "^tools/checks/$b\$"; then
        continue
    fi

    echo "FAIL: $b is neither in ci-suite.sh's manifest nor marked MANUAL/HELPER"
    fail=1
done

if [ "$fail" -eq 0 ]; then
    echo "PASS: every *-check.* file in tools/checks/ is in ci-suite.sh or says why not"
fi
exit $fail
