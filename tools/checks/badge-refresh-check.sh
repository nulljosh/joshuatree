#!/bin/sh
# Enforces the standing rule: every MINOR version bump refreshes the
# engraved landing badge (landing/badge.svg / landing/badge.png). Fails
# when VERSION's minor number differs from the previous minor release
# tag's minor number, while the badge asset is unchanged since that tag.
#
# Tags are bare "x.y.z" (e.g. "1.2.0", "1.3.0"). PATCH bumps (z changes,
# x.y stays put) are exempt: the rule is minor-only.
set -eu
cd "$(dirname "$0")/../.."

VERSION="$(tr -d '[:space:]' < VERSION)"
MINOR="$(echo "$VERSION" | cut -d. -f1,2)"

# Most recent tag whose x.y differs from the current one: that is the
# "previous minor" baseline we compare the badge against.
PREV_TAG=""
for t in $(git tag --list '*.*.*' --sort=-v:refname); do
  case "$t" in
    [0-9]*.[0-9]*.[0-9]*) ;;
    *) continue ;;
  esac
  t_minor="$(echo "$t" | cut -d. -f1,2)"
  if [ "$t_minor" != "$MINOR" ]; then
    PREV_TAG="$t"
    break
  fi
done

if [ -z "$PREV_TAG" ]; then
  echo "badge-refresh-check: no previous minor tag found, nothing to compare, OK"
  exit 0
fi

PREV_MINOR="$(echo "$PREV_TAG" | cut -d. -f1,2)"
if [ "$PREV_MINOR" = "$MINOR" ]; then
  echo "badge-refresh-check: VERSION ($VERSION) is still on minor $MINOR, OK"
  exit 0
fi

# VERSION has moved to a new minor since $PREV_TAG. The badge assets must
# have changed since that tag.
CHANGED="$(git diff --name-only "$PREV_TAG" -- landing/badge.svg landing/badge.png 2>/dev/null || true)"

if [ -z "$CHANGED" ]; then
  echo "badge-refresh-check: FAIL"
  echo "  VERSION is $VERSION (minor $MINOR), previous minor tag is $PREV_TAG (minor $PREV_MINOR),"
  echo "  but landing/badge.svg and landing/badge.png are unchanged since $PREV_TAG."
  echo "  Every minor bump refreshes the engraved badge. See docs/BADGE.md."
  exit 1
fi

echo "badge-refresh-check: badge refreshed since $PREV_TAG for minor $MINOR, OK"
