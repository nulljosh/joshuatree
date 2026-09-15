#!/usr/bin/env bash
# Drift insurance, direct request ("work on drift insurance, so any
# references don't drift too much"). Scans roadmap.md and CLAUDE.md for
# every backtick-quoted file path and checks it still exists somewhere in
# the real tree, by basename, not just the exact written path: this
# codebase's own prose style routinely writes bare filenames as shorthand
# (`kheap.c`, not `kernel/kheap.c`), which is normal, not drift, so an
# exact-path-only check would false-positive on most of roadmap.md.
#
# Run this after any rename/move/delete, or periodically as part of the
# loop's own verification step; a real hit here means a doc genuinely
# points at something that no longer exists.
set -euo pipefail
cd "$(dirname "$0")/../.."

python3 << 'PYEOF'
import re, os, sys

FILES_TO_SCAN = ["roadmap.md", "CLAUDE.md", "docs/ARCHITECTURE.md"]
PATTERN = re.compile(r'`([A-Za-z0-9_./-]+\.(?:c|h|S|md|sh|py|js|html|svg|json|toml|tsv|txt|yml))`')
# node_modules/.claude are noise (thousands of irrelevant basenames);
# landing/v86/ (the vendored v86.wasm build) is real and indexed like
# everything else, a bare `libv86.js` reference legitimately means the one
# real file there, no reason to special-case it out.
SKIP_DIRS = ("node_modules/", ".claude/")

basenames = {}
for root, dirs, filenames in os.walk("."):
    rel_root = os.path.relpath(root, ".")
    norm = (rel_root + "/").replace("./", "", 1)
    if any(norm.startswith(e) for e in SKIP_DIRS) or ".git" in root.split(os.sep):
        dirs[:] = []
        continue
    for fn in filenames:
        basenames.setdefault(fn, []).append(os.path.join(rel_root, fn))

stale = []
checked = 0
for f in FILES_TO_SCAN:
    if not os.path.exists(f):
        continue
    text = open(f).read()
    for m in PATTERN.finditer(text):
        ref = m.group(1)
        checked += 1
        if os.path.exists(ref) or os.path.basename(ref) in basenames:
            continue
        stale.append((f, ref))

print(f"check-refs: {checked} file-path references checked across {FILES_TO_SCAN}")
if stale:
    print("STALE (doc points at something that no longer exists):")
    for f, ref in stale:
        print(f"  {f}: `{ref}`")
    sys.exit(1)
else:
    print("clean: every reference resolves somewhere in the real tree")
PYEOF
