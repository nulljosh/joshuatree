#!/usr/bin/env python3
"""One-shot restyler: rewrite every art/icons/*.svg into the glossy tile
technique (rich base gradient + soft top sheen + glyph top-light and drop
shadow), keeping each icon's own glyph geometry and base hue.

This is an authoring tool, not part of the build: gen_icon_art.py still
rasterizes whatever the SVGs say. Kept in-tree so the technique's numbers
live in one readable place instead of being smeared across 24 hand-edited
files, and so a future tuning pass is an edit here plus one re-run rather
than 24 careful search-and-replaces.

Usage: python3 tools/gen/restyle_icons.py
"""
import os
import re
import sys

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
SVG_DIR = os.path.join(ROOT, "art", "icons")

# --- the technique's numbers, all in one place -------------------------------
#
# Base tile gradient. The old art ran a single two-stop ramp of roughly
# +22% white to -18% black around the app's own hue, ~50 luminance top to
# bottom, which reads as a flat chip. This widens it to a real convex-body
# ramp and puts a third stop just past the midpoint so the top half falls
# slowly (the lit face) and the bottom half falls fast (the turn away from
# the light), instead of one straight line.
TOP_WHITE = 0.42      # top stop: this far from the hue toward white
MID_WHITE = 0.10      # stop at MID_AT: still a touch above the hue itself
MID_AT = 0.58         # where that stop sits: past halfway, so the lit face is
                      # the larger share and the turn into shade is the smaller
BOT_BLACK = 0.40      # bottom stop: this far from the hue toward black

# Top sheen. A radial, not a clipped ellipse, deliberately: an ellipse
# filled with a vertical gradient has a real edge wherever its own outline
# crosses the tile while the gradient is still above zero, and at 128px
# that edge survives the downsample as a faint seam. A radial centred above
# the tile falls off in every direction at once, so the highlight is
# brightest at the top centre, dimmer at the top corners and gone by the
# middle, with no geometry anywhere.
SHEEN = [(0.0, 0.14), (0.5, 0.06), (1.0, 0.0)]
SHEEN_CY, SHEEN_R = -18, 106

# Edge light. The top edge of a glossy body catches the most light; the
# bottom edge is in its own shade. This is the one hard, bright line in the
# icon and it is what iconart-check.py's rim-falloff oracle measures.
RIM_TOP, RIM_TOP_END, RIM_BOT = 0.95, 0.30, 0.26

# Glyph top-light. Masked to the glyph's own alpha so it lights the symbol
# and nothing else. White-only on purpose, with no black term at the
# bottom: a darkening term here would drag the Trash can's base band (a
# deliberately flat fill that iconedge-check.py asserts stays >= 240) down
# through that threshold, and weakening a real assertion to make a
# decoration work is the wrong trade.
LIFT = [(0.0, 0.32), (0.5, 0.07), (1.0, 0.0)]
LIFT_Y0, LIFT_Y1 = 16, 102

# Glyph drop shadow: what makes the symbol sit above the surface rather
# than be printed on it.
DROP_DY, DROP_BLUR, DROP_ALPHA = 2.6, 2.9, 0.42


def parse(c):
    return tuple(int(c[i:i + 2], 16) for i in (1, 3, 5))


def fmt(t):
    return "#%02X%02X%02X" % tuple(max(0, min(255, int(round(v)))) for v in t)


def toward(c, t, target):
    return tuple(v + (target - v) * t for v in c)


def stops(items, color="#ffffff"):
    return "".join(
        '<stop offset="%g" stop-color="%s" stop-opacity="%g"/>' % (o, color, a)
        for o, a in items)


HEAD = """<svg xmlns="http://www.w3.org/2000/svg" width="128" height="128" viewBox="0 0 128 128">
  <!-- hue %(hue)s : this icon's own base colour, the one thing here that is
       per-icon. Everything below is the shared tile technique, written by
       tools/gen/restyle_icons.py; re-running it reads this line back, so
       tuning the technique never drifts the colour. -->
  <defs>
    <!-- The tile's own body: lit face on top, turning away from the light
         toward the bottom. Three stops, not two, so the falloff is a curve. -->
    <linearGradient id="base" x1="0" y1="0" x2="0" y2="1">
      <stop offset="0" stop-color="%(top)s"/><stop offset="%(midat)g" stop-color="%(mid)s"/><stop offset="1" stop-color="%(bot)s"/>
    </linearGradient>
    <!-- Top sheen, radial so it has no outline of its own anywhere. -->
    <radialGradient id="gloss" gradientUnits="userSpaceOnUse" cx="64" cy="%(scy)d" r="%(sr)d">
      %(sheen)s
    </radialGradient>
    <!-- Edge light: bright along the top edge, shaded along the bottom. -->
    <linearGradient id="rim" x1="0" y1="0" x2="0" y2="1">
      <stop offset="0" stop-color="#ffffff" stop-opacity="%(rimtop)g"/>
      <stop offset="%(rimend)g" stop-color="#ffffff" stop-opacity="0"/>
      <stop offset="1" stop-color="#000000" stop-opacity="%(rimbot)g"/>
    </linearGradient>
    <linearGradient id="paper" x1="0" y1="0" x2="0" y2="1">
      <stop offset="0" stop-color="#ffffff"/><stop offset="1" stop-color="#E9E2DC"/>
    </linearGradient>
    <!-- The glyph's own top-light, painted through the glyph's alpha. -->
    <linearGradient id="lift" gradientUnits="userSpaceOnUse" x1="0" y1="%(lifty0)d" x2="0" y2="%(lifty1)d">
      %(lift)s
    </linearGradient>
    <filter id="drop" x="-40%%" y="-40%%" width="180%%" height="190%%">
      <feDropShadow dx="0" dy="%(dy)g" stdDeviation="%(blur)g" flood-color="#000000" flood-opacity="%(alpha)g"/>
    </filter>
    <!-- Flattens whatever it is given to solid white, keeping alpha, so the
         glyph can be its own mask without a hand-drawn duplicate of it. -->
    <filter id="flat" x="-20%%" y="-20%%" width="140%%" height="140%%" color-interpolation-filters="sRGB">
      <feColorMatrix type="matrix" values="0 0 0 0 1  0 0 0 0 1  0 0 0 0 1  0 0 0 1 0"/>
    </filter>
    <clipPath id="tile"><rect x="0" y="0" width="128" height="128" rx="28"/></clipPath>
    <g id="glyph">%(body)s</g>
    <mask id="gmask" maskUnits="userSpaceOnUse" x="0" y="0" width="128" height="128">
      <use href="#glyph" filter="url(#flat)"/>
    </mask>
  </defs>
  <rect x="0" y="0" width="128" height="128" rx="28" fill="url(#base)"/>
  <g clip-path="url(#tile)">
    <rect x="0" y="0" width="128" height="128" fill="url(#gloss)"/>
    <use href="#glyph" filter="url(#drop)"/>
    <rect x="0" y="0" width="128" height="128" fill="url(#lift)" mask="url(#gmask)"/>
  </g>
  <rect x="0.7" y="0.7" width="126.6" height="126.6" rx="27.3" fill="none" stroke="url(#rim)" stroke-width="1.4"/>
</svg>
"""

BASE_RE = re.compile(
    r'<linearGradient id="base".*?stop-color="(#[0-9A-Fa-f]{6})".*?'
    r'stop-color="(#[0-9A-Fa-f]{6})".*?</linearGradient>', re.S)
HUE_RE = re.compile(r'<!-- hue (#[0-9A-Fa-f]{6})')
GLOSS_RE = re.compile(r'<ellipse cx="64" cy="6" rx="86" ry="54" fill="url\(#gloss\)"/>')
TAIL_RE = re.compile(r'\n\s*</g>\s*\n\s*<rect x="0\.7"')
GLYPH_RE = re.compile(r'<g id="glyph">(.*)</g>\s*\n\s*<mask id="gmask"', re.S)


def read(path):
    """(hue, glyph body) from a file in either the old or the restyled form.

    Re-runnable on purpose. Tuning the numbers at the top of this file and
    re-running is the whole point of keeping it in-tree, and a tool that
    only works once on pristine input is not that.
    """
    src = open(path).read()
    g = GLYPH_RE.search(src)
    if g:
        # Already restyled. The hue is read back from the marker the last run
        # wrote, not reverse-engineered out of a gradient stop: deriving it
        # from a stop means every tuning pass re-derives it through whatever
        # the constants happen to be now, and the colour walks.
        h = HUE_RE.search(src)
        if not h:
            raise SystemExit("%s: restyled but carries no hue marker" % path)
        return parse(h.group(1)), g.group(1)

    m = BASE_RE.search(src)
    if not m:
        raise SystemExit("%s: no #base gradient to read the hue from" % path)
    hue = tuple((a + b) / 2.0 for a, b in zip(parse(m.group(1)), parse(m.group(2))))
    a, b = GLOSS_RE.search(src), TAIL_RE.search(src)
    if not a or not b:
        raise SystemExit("%s: could not find the glyph block" % path)
    body = src[a.end():b.start()]
    # Each glyph used to carry the drop filter itself, sometimes on two
    # separate elements (Terminal), which double-shadowed where they
    # overlapped. One filtered <use> of the whole glyph replaces all of it.
    body = body.replace(' filter="url(#drop)"', "")
    return hue, "\n".join("  " + ln if ln.strip() else ln for ln in body.split("\n"))


def restyle(path):
    hue, body = read(path)

    out = HEAD % dict(
        hue=fmt(hue),
        top=fmt(toward(hue, TOP_WHITE, 255)),
        mid=fmt(toward(hue, MID_WHITE, 255)),
        bot=fmt(toward(hue, BOT_BLACK, 0)),
        midat=MID_AT, scy=SHEEN_CY, sr=SHEEN_R, sheen=stops(SHEEN),
        rimtop=RIM_TOP, rimend=RIM_TOP_END, rimbot=RIM_BOT,
        lifty0=LIFT_Y0, lifty1=LIFT_Y1, lift=stops(LIFT),
        dy=DROP_DY, blur=DROP_BLUR, alpha=DROP_ALPHA,
        body=body)
    open(path, "w").write(out)
    return fmt(toward(hue, TOP_WHITE, 255)), fmt(toward(hue, BOT_BLACK, 0))


def main():
    names = sorted(n for n in os.listdir(SVG_DIR) if n.endswith(".svg"))
    for n in names:
        top, bot = restyle(os.path.join(SVG_DIR, n))
        print("%-14s %s -> %s" % (n[:-4], top, bot))
    print("restyled %d icons" % len(names))
    return 0


if __name__ == "__main__":
    sys.exit(main())
