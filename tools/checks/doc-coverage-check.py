#!/usr/bin/env python3
"""Docs stay at 100 percent (6.59.1): every source file the progress graph counts has a row in docs/ARCHITECTURE.md.

Same rule as tools/gen/progress.sh's doc_pct_at, on the tracked tree: a unit is each .c and .S file, plus each .h with no
.c of the same stem beside it; it is documented when its basename appears in the doc. Lists the missing ones.
Discriminating: delete `dice.c` from the store row and this names store/dice.c.
"""
import subprocess, sys
from pathlib import Path

root = Path(__file__).resolve().parents[2]
# ponytail: mirrors progress.sh's filters by hand; that file is a bash heredoc and cannot be imported
EXCLUDE_DIRS = ("node_modules/", ".claude/", "landing/v86/", "third_party/")
EXCLUDE_BASENAMES = {"wallpaper.h", "editor_fonts.h", "vgafont.h", "icon_art.h"}
files = [f for f in subprocess.run(["git", "ls-files"], cwd=root, capture_output=True, text=True).stdout.split()
         if f.endswith((".c", ".h", ".S")) and not f.startswith(EXCLUDE_DIRS) and not f.startswith("drivers/app_")
         and f.rsplit("/", 1)[-1] not in EXCLUDE_BASENAMES]
stems = {f[:-2] for f in files if f.endswith(".c")}
units = [f for f in files if not f.endswith(".h") or f[:-2] not in stems]
arch = (root / "docs/ARCHITECTURE.md").read_text()
missing = [u for u in units if u.rsplit("/", 1)[-1] not in arch]
if missing:
    print("FAIL: %d of %d source files have no row in docs/ARCHITECTURE.md:\n  %s" % (len(missing), len(units), "\n  ".join(missing)))
    sys.exit(1)
print("PASS: all %d source files are documented in docs/ARCHITECTURE.md" % len(units))
