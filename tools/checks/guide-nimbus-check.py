#!/usr/bin/env python3
"""8.14.0: the Nimbus build guide (docs/GUIDE.md, "Build a Nimbus, step by step") stays true to the repo.

1. The chapter is there, with its seven numbered steps in order.
2. Every file it names in backticks exists (a bare part name means the print pack, docs/hardware/nimbus/pack/), and every
   command it tells you to run is a script in the repo.
3. Every part in docs/hardware/nimbus/pack/bom.csv is named in the chapter or in the Shopping list section.
4. Every screen size the chapter promises is one tools/checks/arm64-screens-check.py boots.
Fails on a renamed STL, a moved script, a BOM row nobody mentions, or a screen size nothing checks.
Usage: tools/checks/guide-nimbus-check.py   (from the repo root)
"""
import csv, re, sys
from pathlib import Path

root = Path(__file__).resolve().parents[2]
guide = (root / "docs/GUIDE.md").read_text()
fails = []
def check(name, ok, detail=""):
    print(("  ok: " if ok else "  FAIL: ") + name + ("" if ok else "  " + str(detail)[:400]))
    if not ok: fails.append(name)

def section(title):
    i = guide.find(title)
    if i < 0: return ""
    j = re.search(r"\n#{2,3} ", guide[i + len(title):])
    return guide[i: i + len(title) + (j.start() if j else len(guide))]

ch = section("### Build a Nimbus, step by step")
check("the chapter is in docs/GUIDE.md", bool(ch))
steps = [int(n) for n in re.findall(r"^(\d+)\. \*\*", ch, re.M)]
check("seven numbered steps, in order (%s)" % steps, steps == list(range(1, 8)), steps)

PACK = "docs/hardware/nimbus/pack/"
missing = []
for t in re.findall(r"`([^`]+)`", ch):
    word = t.split()[0]
    if word.startswith("brew") or word.startswith("/Volumes"): continue
    if "/" in word or re.search(r"\.(stl|csv|pdf|py|sh|json|svg)$", word):
        path = word if "/" in word else PACK + word
        if not (root / path).exists(): missing.append(path)
check("every file and command the chapter names is in the repo", not missing, missing)

shop = section("### Shopping list")
text = (ch + shop).lower()
STOP = {"with", "from", "into", "panel", "short", "cable", "sheet", "thick", "wide", "inch", "class", "wanted", "touch", "model"}
unnamed = []
with open(root / PACK / "bom.csv") as f:
    for row in csv.DictReader(f):
        item = row["item"]
        words = [w for w in re.findall(r"[a-z0-9.\-]+", item.lower()) if len(w) >= 3 and w not in STOP]
        if not any(w in text for w in words): unnamed.append(item)
check("every part in bom.csv is named in the chapter or the Shopping list", not unnamed, unnamed)

promised = set(re.findall(r"\b(\d{3,4})x(\d{3,4})\b", ch))
screens = (root / "tools/checks/arm64-screens-check.py").read_text()
booted = set(re.findall(r"\((\d{3,4}), (\d{3,4})\)", screens))
check("every screen size the chapter promises is booted by arm64-screens-check.py (%s)" % sorted(promised), promised and promised <= booted, sorted(promised - booted))

if fails: print("FAIL: %d of the Nimbus guide checks" % len(fails)); sys.exit(1)
print("PASS: the Nimbus build guide names only real files and scripts, covers every part in the BOM, and promises only screens a check boots")
