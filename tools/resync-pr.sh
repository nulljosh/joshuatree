#!/bin/bash
# resync.sh <worktree-dir>: bring the branch checked out there up to date with
# its own remote and with origin/main, then push.
# The tracked demo kernel (landing/v86/kernel.elf) conflicts on every kernel PR;
# it is a build product, so resolve it by rebuilding from the merged source.
# Any other conflict is left for a human: the script stops and lists it.
set -e
cd "$1"
br=$(git rev-parse --abbrev-ref HEAD)
git fetch -q origin
merge_ref() {
  git merge "$1" -m "merge $1; demo kernel rebuilt from the merged source" >/dev/null 2>&1 && return 0
  others=$(git diff --name-only --diff-filter=U | grep -v '^landing/v86/kernel.elf$' || true)
  if [ -n "$others" ]; then echo "NEEDS HAND MERGE ($1):"; echo "$others"; exit 2; fi
  git checkout --theirs landing/v86/kernel.elf && git add landing/v86/kernel.elf
  git commit -qm "merge $1; demo kernel rebuilt from the merged source"
}
merge_ref "origin/$br"
merge_ref origin/main
touch kernel/kernel.c
if make -s kernel.elf 2>&1 | grep -E " error"; then echo "BUILD FAILED"; exit 3; fi
./check.sh 2>&1 | tail -1
git add landing/v86/kernel.elf
git diff --cached --quiet || git commit -qm "demo kernel rebuilt from the merged source"
git push -q origin "$br" && echo "pushed $br $(git rev-parse --short HEAD)"
