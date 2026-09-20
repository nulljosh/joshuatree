#!/bin/sh
# Injects the latest feature headline from roadmap.md into landing/index.html.
# Run this before wrangler deploy to keep the landing page's banner in sync.
set -e

# Extract the Latest line from roadmap.md using awk to avoid sed regex issues
latest_line=$(awk '/^\*\*Latest\*\*:/ {print; exit}' roadmap.md)
if [ -z "$latest_line" ]; then
  echo "Error: No **Latest**: line found in roadmap.md"
  exit 1
fi

# Extract the headline text using awk (everything after "**Latest**: ")
headline=$(echo "$latest_line" | awk -F '\\*\\*Latest\\*\\*: ' '{print $2}')
if [ -z "$headline" ]; then
  echo "Error: **Latest**: line is empty"
  exit 1
fi

# Remove trailing period if present
headline_clean=$(echo "$headline" | sed 's/\.$//')

# The announcement rides the eyebrow's data-latest attribute, not the H1.
# The H1 is the brand line and stays put: injecting the announcement there
# meant every visitor watched it render and then get replaced a second later
# by the demo tour's own reset, which is the swap this moved to fix.
tmp_file=$(mktemp)
trap "rm -f $tmp_file" EXIT

awk -v latest="$headline_clean" '
  /data-latest="[^"]*"/ {
    sub(/data-latest="[^"]*"/, "data-latest=\"" latest "\"")
  }
  {print}
' landing/index.html > "$tmp_file"

mv "$tmp_file" landing/index.html

if ! grep -q "data-latest=\"${headline_clean}\"" landing/index.html; then
  echo "Error: could not find the eyebrow's data-latest attribute to update"
  exit 1
fi

echo "Updated landing/index.html eyebrow data-latest to: $headline_clean"
