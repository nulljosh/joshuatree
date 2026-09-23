#!/usr/bin/env python3
"""Pull each fleet app's real icon.svg into art/icons/ so the dock shows the
icon the project actually ships with, not a stand-in. The project art is
nested untouched inside a 128 box and clipped to the same rx=28 tile every
authored icon uses, so it sits in the dock like the rest. Run from the repo
root with the other repos checked out beside it, then run gen_icon_art.py.
ponytail: imported tiles skip restyle_icons.py's shared gloss; they are the
project's own artwork on purpose."""
import os, re, sys
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
CODE = os.path.dirname(ROOT) if os.path.isdir(os.path.join(os.path.dirname(ROOT), "epiphany")) else os.path.expanduser("~/Documents/Code")
# art/icons name -> repo folder
FLEET = {"epiphany": "epiphany", "curbfind": "curbfind", "bookrank": "bookrank", "lexly": "lexly", "sparkjar": "sparkjar",
         "quotes": "quotestreak", "keyrate": "keyrate", "toroid": "conway", "homeqi": "homeqi", "fieldbook": "fieldbook",
         "plan": "plan"}  # Weather is a system app, it keeps the restyled icon from restyle_icons.py
for name, repo in FLEET.items():
    src = os.path.join(CODE, repo, "icon.svg")
    if not os.path.isfile(src):
        print("skip %s: no %s" % (name, src)); continue
    svg = open(src).read()
    m = re.search(r"<svg\b[^>]*>", svg)
    vb = re.search(r'viewBox="([^"]+)"', m.group(0))
    if not vb:
        w = re.search(r'\bwidth="([\d.]+)', m.group(0)); h = re.search(r'\bheight="([\d.]+)', m.group(0))
        if not (w and h): sys.exit("%s: no viewBox or size" % src)
        box = "0 0 %s %s" % (w.group(1), h.group(1))
    else: box = vb.group(1)
    inner = svg[m.end():svg.rindex("</svg>")]
    out = ('<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" width="128" height="128" viewBox="0 0 128 128">\n'
           '  <!-- imported from %s/icon.svg by tools/gen/import_fleet_icons.py, do not hand edit -->\n'
           '  <defs><clipPath id="jt-tile"><rect x="0" y="0" width="128" height="128" rx="28"/></clipPath></defs>\n'
           '  <g clip-path="url(#jt-tile)"><svg x="0" y="0" width="128" height="128" viewBox="%s">%s</svg></g>\n</svg>\n') % (repo, box, inner)
    open(os.path.join(ROOT, "art", "icons", name + ".svg"), "w").write(out)
    print("imported", name)
