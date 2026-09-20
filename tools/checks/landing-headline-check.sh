#!/bin/sh
# Verify that the landing page's announcement is in sync with roadmap.md's
# **Latest** field, and that it actually changes when the field changes.
#
# The announcement lives on the eyebrow's data-latest attribute, not the H1.
# It used to be injected into the H1, which meant every visitor watched the
# announcement render and then get replaced a second later by the demo tour's
# own reset to the brand line. So this also asserts the H1 IS the brand line:
# if a future pass moves the announcement back into it, that swap comes back
# and this check fails.
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


# The H1 must be the brand line, nothing else.
current_h1=$(grep -o '<h1>[^<]*</h1>' landing/index.html | head -1)
if [ "$current_h1" != "<h1>Introducing Joshua Tree.</h1>" ]; then
  echo "FAIL: the H1 should be the brand line, not an announcement"
  echo "  Expected: <h1>Introducing Joshua Tree.</h1>"
  echo "  Got: $current_h1"
  exit 1
fi

current_latest=$(grep -o 'data-latest="[^"]*"' landing/index.html | head -1)
if [ "$current_latest" != "data-latest=\"${expected}\"" ]; then
  echo "FAIL: eyebrow announcement mismatch"
  echo "  Expected: data-latest=\"${expected}\""
  echo "  Got: $current_latest"
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
new_latest=$(grep -o 'data-latest="[^"]*"' landing/index.html | head -1)
expected_new=$(echo "$test_headline" | sed 's/\.$//')
if [ "$new_latest" != "data-latest=\"${expected_new}\"" ]; then
  echo "FAIL: announcement did not change when Latest field changed"
  echo "  Expected: data-latest=\"${expected_new}\""
  echo "  Got: $new_latest"
  exit 1
fi

# Restore and regenerate again
mv roadmap.md.bak roadmap.md
trap - EXIT  # Clear the trap

./tools/gen/inject-landing-headline.sh >/dev/null 2>&1

# Verify it changed back
restored_latest=$(grep -o 'data-latest="[^"]*"' landing/index.html | head -1)
if [ "$restored_latest" != "data-latest=\"${expected}\"" ]; then
  echo "FAIL: announcement did not restore when Latest field restored"
  echo "  Expected: data-latest=\"${expected}\""
  echo "  Got: $restored_latest"
  exit 1
fi

echo "PASS: the H1 is the brand line and the eyebrow announcement tracks roadmap Latest"
