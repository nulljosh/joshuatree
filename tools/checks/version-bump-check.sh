#!/bin/sh
# Every PR that changes code must bump VERSION, so every merge cuts a release
# (release.yml fires on VERSION). Five merges in a row shipped no release on
# 2026-09-23 because nobody bumped it. Prose-only PRs are exempt.
# Usage: tools/checks/version-bump-check.sh <base-ref>   (CI passes origin/main)
set -e
cd "$(dirname "$0")/../.."
base=${1:-origin/main}
# Two-dot on purpose: in CI HEAD is the PR merge commit, whose first parent
# is the base, so base..HEAD is exactly the PR. Three-dot needs a merge base
# a shallow clone may not have.
files=$(git diff --name-only "$base" HEAD)
[ -z "$files" ] && { echo "PASS: no changes"; exit 0; }
if ! echo "$files" | grep -qvE '\.md$'; then echo "PASS: prose-only change, no version bump needed"; exit 0; fi
old=$(git show "$base":VERSION | tr -d ' \n'); new=$(tr -d ' \n' < VERSION)
if [ "$old" = "$new" ]; then
    echo "FAIL: VERSION is still $old but this PR changes code; bump it so the merge cuts a release"
    exit 1
fi
if [ "$(printf '%s\n%s\n' "$old" "$new" | sort -V | tail -1)" != "$new" ]; then
    echo "FAIL: VERSION $new is not above $old"; exit 1
fi
echo "PASS: VERSION $old -> $new"
