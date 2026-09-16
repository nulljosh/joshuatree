#!/bin/sh
# Verify that the landing page's h1 banner is in sync with roadmap.md's **Latest** field,
# and that it actually changes when the field changes.
set -e

cd "$(dirname "$0")/../.."

# Read the current Latest line using awk
original_latest=$(awk '/^\*\*Latest\*\*:/ {print; exit}' roadmap.md)
if [ -z "$original_latest" ]; then
  echo "FAIL: No **Latest**: line found in roadmap.md"
  exit 1
fi

# Extract the expected headline
expected=$(echo "$original_latest" | awk -F '\\*\\*Latest\\*\\*: ' '{print $2}' | sed 's/\.$//')
expected_heading="Introducing ${expected}."

# Read what's currently in landing/index.html
current_h1=$(grep -o '<h1>Introducing [^<]*</h1>' landing/index.html | head -1)
if [ -z "$current_h1" ]; then
  echo "FAIL: No <h1>Introducing...</h1> found in landing/index.html"
  exit 1
fi

# Check it matches
if [ "$current_h1" != "<h1>${expected_heading}</h1>" ]; then
  echo "FAIL: landing h1 mismatch"
  echo "  Expected: <h1>${expected_heading}</h1>"
  echo "  Got: $current_h1"
  exit 1
fi

# Now test that it changes when we change the field
test_headline="Improved icons and weather display."

# Make a backup
cp roadmap.md roadmap.md.bak

# Temporarily modify roadmap.md - replace line 8 which has the Latest line
awk -v new="**Latest**: ${test_headline}" '
  NR == 8 {print new; next}
  {print}
' roadmap.md.bak > roadmap.md

trap "mv roadmap.md.bak roadmap.md" EXIT

# Regenerate the landing page
./tools/gen/inject-landing-headline.sh >/dev/null 2>&1

# Verify it changed
new_h1=$(grep -o '<h1>Introducing [^<]*</h1>' landing/index.html | head -1)
expected_new="Introducing ${test_headline}"
if [ "$new_h1" != "<h1>${expected_new}</h1>" ]; then
  echo "FAIL: banner did not change when Latest field changed"
  echo "  Expected: <h1>${expected_new}</h1>"
  echo "  Got: $new_h1"
  exit 1
fi

# Restore and regenerate again
mv roadmap.md.bak roadmap.md
trap - EXIT  # Clear the trap

./tools/gen/inject-landing-headline.sh >/dev/null 2>&1

# Verify it changed back
restored_h1=$(grep -o '<h1>Introducing [^<]*</h1>' landing/index.html | head -1)
if [ "$restored_h1" != "<h1>${expected_heading}</h1>" ]; then
  echo "FAIL: banner did not restore when Latest field restored"
  echo "  Expected: <h1>${expected_heading}</h1>"
  echo "  Got: $restored_h1"
  exit 1
fi

echo "PASS: landing page banner tracks roadmap Latest field"
