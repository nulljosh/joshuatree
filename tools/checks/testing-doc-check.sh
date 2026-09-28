#!/bin/sh
# docs/TESTING.md lists every check in ci-suite.sh; regenerate with tools/gen/testing-doc.py
exec python3 "$(dirname "$0")/../gen/testing-doc.py" --check
