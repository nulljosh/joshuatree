#!/bin/bash
# The landing logo is generated, never hand-edited: tools/gen/logo.py must rebuild landing/logo.svg byte for byte.
set -e
cd "$(git rev-parse --show-toplevel)"
before=$(mktemp); cp landing/logo.svg "$before"
python3 tools/gen/logo.py
if cmp -s landing/logo.svg "$before"; then echo "logo-check: OK, tools/gen/logo.py rebuilds landing/logo.svg exactly"; rm "$before"
else cp "$before" landing/logo.svg; rm "$before"; echo "FAIL: landing/logo.svg differs from what tools/gen/logo.py draws (hand-edited?)"; exit 1; fi
# The mascot is drawn by the same hand and held to the same rule.
before=$(mktemp); cp landing/mascot.svg "$before"
python3 tools/gen/mascot.py
if cmp -s landing/mascot.svg "$before"; then echo "logo-check: OK, tools/gen/mascot.py rebuilds landing/mascot.svg exactly"; rm "$before"
else cp "$before" landing/mascot.svg; rm "$before"; echo "FAIL: landing/mascot.svg differs from what tools/gen/mascot.py draws (hand-edited?)"; exit 1; fi
