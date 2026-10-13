#!/usr/bin/env python3
"""Pull each fleet app's real icon.svg into art/icons/ so the dock shows the
icon the project actually ships with, not a stand-in. The project art is
nested untouched inside a 128 box and clipped to the exact same squircle
every other authored icon uses (imported from restyle_icons.py, the one
place that shape is defined), so its corner curvature and top highlight
match the rest of the dock and Apps folder instead of standing out as a
plain rounded rect with no light on it. Run from the repo root with the
other repos checked out beside it, then run gen_icon_art.py.

ponytail: imported tiles still skip restyle_icons.py's shared gloss/tint
on their own artwork -- that stays the project's own branding, on purpose.
Only the tile's outer silhouette and the same soft top highlight line
every other icon has are shared, both boundary treatment, not colour."""
import os, re, sys
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
CODE = os.path.dirname(ROOT) if os.path.isdir(os.path.join(os.path.dirname(ROOT), "epiphany")) else os.path.expanduser("~/Documents/Code")
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from restyle_icons import squircle_path, HL_WIDTH, HL_ALPHA, HL_FADE  # single source of truth for the shared shape/light

# art/icons name -> repo folder
FLEET = {"epiphany": "epiphany", "curbfind": "curbfind", "bookrank": "bookrank", "tonchi": "tonchi", "hikko": "hikko",
         "quotes": "quotestreak", "keyrate": "keyrate", "toroid": "conway", "fieldbook": "fieldbook", "windgate": "breathe",
         "chat": "turing:web/mascot.svg"}   # Samantha: her mascot, Turing's app icon since v5.12.4 (a flat portrait: copper hair, tortoise-shell glasses). A "repo:path" value names the file when it is not repo/icon.svg.

# Dock tile color per app. Most projects ship a near-black tile, so eight of
# them side by side read as one black strip; the dock repaints each one's
# full-size background rect here instead. Only the tile, never the glyph,
# and only in this OS: the project's own icon.svg is untouched.
TILE = {"epiphany": "#D6DEE6", "curbfind": "#C8644F", "bookrank": "#3E5C86", "tonchi": "#2E86DE",
        "hikko": "#8A5A3C", "quotes": "#F5F5F8", "keyrate": "#5A5A5E"}  # toroid stays black: Life is white-on-black



# Art that is a bare shape, not a tile: the colour laid under it.
GROUND = {"chat": ("#F5F5F8", "#E0E1E6")}   # top -> bottom, the light-tile gradient (quotes, settings); a flat cream ground vanished into the tray
GROUND_FIT = 0.6875   # 88 of 128 units: 20 clear above and below, the bust stays inside the 16 top / 20 bottom margins


def retile(inner, box, color):
    """Swap the fill of the first rect that covers the whole viewBox (the tile)."""
    w = box.split()[2]
    m = re.search(r'<rect\b(?=[^>]*\bwidth="%s")[^>]*?\bfill="[^"]*"' % re.escape(w), inner)
    if not m: sys.exit("no full-size tile rect to recolor")
    return inner[:m.start()] + re.sub(r'\bfill="[^"]*"', 'fill="%s"' % color, m.group(0)) + inner[m.end():]


def wrap(repo, box, inner, tile=""):
    sq = squircle_path()
    return ('<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" width="128" height="128" viewBox="0 0 128 128">\n'
            '  <!-- imported from %s/icon.svg by tools/gen/import_fleet_icons.py, do not hand edit.\n'
            '       Tile silhouette and top highlight match every other authored icon\n'
            '       (see restyle_icons.py); the artwork itself is the project\'s own.%s -->\n'
            '  <defs>\n'
            '    <clipPath id="jt-tile"><path d="%s"/></clipPath>\n'
            '    <linearGradient id="hl" gradientUnits="userSpaceOnUse" x1="0" y1="0" x2="0" y2="%d">\n'
            '      <stop offset="0" stop-color="#FFFFFF" stop-opacity="%g"/><stop offset="1" stop-color="#FFFFFF" stop-opacity="0"/>\n'
            '    </linearGradient>\n'
            '  </defs>\n'
            '  <g clip-path="url(#jt-tile)">\n'
            '    <svg x="0" y="0" width="128" height="128" viewBox="%s">%s</svg>\n'
            '    <path d="%s" fill="none" stroke="url(#hl)" stroke-width="%g"/>\n'
            '  </g>\n</svg>\n') % (repo, tile, sq, HL_FADE, HL_ALPHA, box, inner, sq, HL_WIDTH)


def main():
    for name, repo in FLEET.items():
        repo, _, rel = repo.partition(":")
        src = os.path.join(CODE, repo, rel or "icon.svg")
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
        if name in TILE: inner = retile(inner, box, TILE[name])
        if name in GROUND:   # art with no tile of its own (a round portrait): a full-size rect under it fills the squircle's corners
            w, h = box.split()[2:4]
            cx, cy = float(w) / 2, float(h) / 2   # the disc shrinks to GROUND_FIT of the tile so the portrait keeps out of the top and bottom glyph margins (DESIGN.md Icons)
            top, bot = GROUND[name]
            inner = ('<linearGradient id="jt-ground" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="%s"/><stop offset="1" stop-color="%s"/></linearGradient>' % (top, bot)
                     + '<rect x="0" y="0" width="%s" height="%s" fill="url(#jt-ground)"/>' % (w, h)
                     + '<g transform="translate(%g %g) scale(%g) translate(%g %g)">%s</g>' % (cx, cy, GROUND_FIT, -cx, -cy, inner))
        open(os.path.join(ROOT, "art", "icons", name + ".svg"), "w").write(wrap(repo, box, inner, "\n       Tile %s -> %s." % GROUND[name] if name in GROUND else ""))
        print("imported", name)


if __name__ == "__main__":
    main()
