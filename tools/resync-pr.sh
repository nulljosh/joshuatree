#!/bin/bash
# resync-pr.sh <worktree-dir>: bring the branch checked out there up to date
# with its own remote and with origin/main, run the checks, then push.
# Any conflict is left for a human: the script stops and lists it.
set -e
cd "$1"
br=$(git rev-parse --abbrev-ref HEAD)
git fetch -q origin
for ref in "origin/$br" origin/main; do
  git merge "$ref" -m "merge $ref" >/dev/null 2>&1 && continue
  # the demo kernel used to be tracked; main deleted it, so drop our copy
  git rm -q --cached landing/v86/kernel.elf 2>/dev/null || true
  others=$(git diff --name-only --diff-filter=U)
  if [ -n "$others" ]; then echo "NEEDS HAND MERGE ($ref):"; echo "$others"; exit 2; fi
  git commit -qm "merge $ref"
done
if make -s kernel.elf 2>&1 | grep -E " error"; then echo "BUILD FAILED"; exit 3; fi
./check.sh 2>&1 | tail -1
git push -q origin "$br" && echo "pushed $br $(git rev-parse --short HEAD)"
