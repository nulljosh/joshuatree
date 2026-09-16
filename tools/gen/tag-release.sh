#!/bin/sh
# Tag + push + gh-release the current HEAD if its commit subject is a version bump ("vX.Y.Z: ...").
# Run this manually after a version-bump commit. No auto-trigger, per house no-background-automation rule.
set -e

msg=$(git log -1 --format=%s)
ver=$(echo "$msg" | grep -oE '^v[0-9]+\.[0-9]+\.[0-9]+' | head -1)
if [ -z "$ver" ]; then
  echo "HEAD commit isn't a version bump (\"$msg\"), nothing to tag."
  exit 1
fi

if git rev-parse "$ver" >/dev/null 2>&1; then
  echo "$ver already tagged, skipping."
  exit 0
fi

git tag -a "$ver" -m "$msg"
git push origin "$ver"

if command -v gh >/dev/null 2>&1; then
  gh release create "$ver" --title "$ver" --notes "$msg"
else
  echo "gh not found, tag pushed but no release created."
fi

echo "Tagged and released $ver."
