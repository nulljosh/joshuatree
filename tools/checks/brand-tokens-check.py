#!/usr/bin/env python3
"""docs/brand/tokens.css is the one place the brand's colours are written down, and the kernel and the map read theirs
elsewhere, so this fails when they drift: every desktop token must equal the kernel constant it names, the accent must be
the house terracotta from docs/ARCHITECTURE.md, the brand page must link the file and not carry its own :root, and
tools/gen/map.py must read it. Proof it discriminates: change one hex in tokens.css and it prints the pair.
"""
import os, re, sys

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
def read(p): return open(os.path.join(ROOT, p)).read()
tok = dict(re.findall(r"--([a-z-]+):\s*(#[0-9a-f]{6})", read("docs/brand/tokens.css").split("@media")[0]))
bad = []
# desktop colours: the kernel's constants, 0x00RRGGBB
src = read("kernel/dock_geom.h") + read("kernel/gui_paint.h") + read("arch/arm64/main.c") + read("arch/arm64/wall.c") + read("arch/arm64/settings.c")
for name, const in (("dock-tray", "DOCK_TRAY_COLOR"), ("dock-label", "DOCK_LABEL_BG"), ("menubar-rule", "MENUBAR_RULE")):
    m = re.search(r"#define %s\s+0x00([0-9A-Fa-f]{6})" % const, src)
    if not m: bad.append("no #define %s in the source" % const)
    elif tok.get(name, "").lower() != "#" + m.group(1).lower(): bad.append("--%s is %s, %s is #%s" % (name, tok.get(name), const, m.group(1).lower()))
m = re.search(r"\{ \"Grey\", 0x00([0-9A-Fa-f]{6})u \}", src)
if m and tok.get("wallpaper", "").lower() != "#" + m.group(1).lower(): bad.append("--wallpaper is %s, the Grey wallpaper is #%s" % (tok.get("wallpaper"), m.group(1).lower()))
m = re.search(r"`ACCENT = (#[0-9a-f]{6})`", read("docs/ARCHITECTURE.md"))
if not m or tok.get("accent") != m.group(1): bad.append("--accent is %s, ARCHITECTURE says ACCENT = %s" % (tok.get("accent"), m and m.group(1)))
page = read("docs/brand/index.html")
if 'href="tokens.css"' not in page: bad.append("docs/brand/index.html does not link tokens.css")
if re.search(r"^\s*:root\s*\{", page, re.M): bad.append("docs/brand/index.html carries its own :root; the tokens belong in tokens.css")
if "tokens.css" not in read("tools/gen/map.py"): bad.append("tools/gen/map.py does not read tokens.css")
for k in ("bg", "fg", "sub", "rule", "card", "accent", "window-ink"): 
    if k not in tok: bad.append("tokens.css has no --%s, which map.py needs" % k)
if bad:
    for b in bad: print("FAIL:", b)
    sys.exit(1)
print("PASS: docs/brand/tokens.css matches the kernel's colours, the accent, the brand page and the map (%d tokens)" % len(tok))
