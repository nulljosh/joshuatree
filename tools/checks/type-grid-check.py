#!/usr/bin/env python3
"""The type system holds (docs/ARCHITECTURE.md): every font-size on the landing page is one of the nine scale steps, the token block in
landing/index.html and docs/ARCHITECTURE.md are what tools/gen/typetokens.py writes today, every scale step's line height sits on the
4 px grid, and the Tree Sans files the page loads exist and are real WOFF2.

Usage: python3 tools/checks/type-grid-check.py"""
import importlib.util, os, re, sys
ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
spec = importlib.util.spec_from_file_location("typetokens", os.path.join(ROOT, "tools", "gen", "typetokens.py"))
tt = importlib.util.module_from_spec(spec); spec.loader.exec_module(tt)
fails = []
html = open(os.path.join(ROOT, "landing", "index.html")).read()
body = re.sub(re.escape(tt.START) + r".*?" + re.escape(tt.END), "", html, flags=re.S)

raw = sorted({m.group(1) for m in re.finditer(r"font-size\s*:\s*([0-9.]+px)", body)})
if raw: fails.append(f"raw px font sizes outside the scale: {raw}; use var(--tN) (python3 tools/gen/typetokens.py --snap)")
for m in re.finditer(r"font-size\s*:\s*var\(--t(\d+)\)", body):
    if int(m.group(1)) not in tt.SIZES: fails.append(f"--t{m.group(1)} is not on the scale {tt.SIZES}")
if tt.START not in html or tt.css_block() not in html: fails.append("the token block in landing/index.html is stale: python3 tools/gen/typetokens.py")
sys.path.insert(0, os.path.join(ROOT, "tools", "gen"))
import docblock
if (docblock.current("type") or "").strip() != tt.type_md().strip(): fails.append("the type block in docs/ARCHITECTURE.md is stale: python3 tools/gen/typetokens.py")
for s in tt.SIZES:
    if tt.lh(s) % 4: fails.append(f"line height of {s}px is {tt.lh(s)}, off the 4 px grid")
for _, _, _, f in tt.FACES:
    p = os.path.join(ROOT, "landing", "fonts", f + ".woff2")
    if not os.path.exists(p) or open(p, "rb").read(4) != b"wOF2": fails.append(f"{p} is missing or not WOFF2")
if fails:
    print("FAIL:"); [print("  - " + x) for x in fails]; sys.exit(1)
print(f"PASS: type grid: every landing font size is on the {len(tt.SIZES)}-step scale, line heights sit on the 4 px grid, the token block and ARCHITECTURE.md are current, and all {len(tt.FACES)} Tree Sans files load")
