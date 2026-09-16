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
full_heading="Introducing ${headline_clean}."

# Replace the h1 in landing/index.html using a portable approach
# Create a temporary file with the replacement
tmp_file=$(mktemp)
trap "rm -f $tmp_file" EXIT

awk -v heading="<h1>${full_heading}</h1>" '
  /<h1>Introducing [^<]*<\/h1>/ {
    print heading
    next
  }
  {print}
' landing/index.html > "$tmp_file"

mv "$tmp_file" landing/index.html

echo "Updated landing/index.html h1 to: $full_heading"
