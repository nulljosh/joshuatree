#!/usr/bin/env python3
"""Authoring tool for the dock's icon artwork: writes the eleven dock icons
(plus Trash's full variant) and three Apps-folder-only icons (Calculator,
Search, Contacts -- APPS_ONLY below) in art/icons/ from the design tables
below, in one shared macOS Big Sur-style tile technique.

This is an authoring tool, not part of the build: gen_icon_art.py still
rasterizes whatever the SVGs say. It is kept in-tree so the technique's
numbers, and every dock glyph, live in one readable place instead of being
smeared across twelve hand-edited files. A tuning pass is an edit here plus
one re-run.

Why this replaced the glossy technique (owner feedback: "icons still look
too Windows or Linux"). The previous pass gave every tile a heavy three-stop
ramp, 42% toward white at the top to 40% toward black at the bottom (about
100 luminance top to bottom), a radial top sheen, and a rim stroke that went
black along the bottom edge. That is the Aqua/Vista-era glass look: a dark,
vignetted chip with a hard dark outline, and a small flat clip-art plate
floating in the middle of it. Big Sur and later do the opposite:

  * the tile is mostly its own colour, lit from the top by a few percent
    (about 20-30 luminance of spread, not 100), with no sheen;
  * a soft inner highlight along the top edge only, no outline anywhere
    and certainly no dark one;
  * a squircle (continuous-curvature corner), not a circular rounded rect;
  * the glyph is a material object that fills the tile: gradient fills lit
    from the same top light, and one gentle contact shadow under it.

Icons NOT in DOCK below (the fleet apps shown only in the Apps folder)
still carry the earlier glossy technique an older revision of this script
wrote; this one leaves those files alone rather than restyling artwork it
has no design for.

Usage: python3 tools/gen/restyle_icons.py && python3 tools/gen/gen_icon_art.py
"""
import math
import os
import sys

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
SVG_DIR = os.path.join(ROOT, "art", "icons")

# --- the shared technique's numbers ------------------------------------------
#
# Squircle: a superellipse |x|^n + |y|^n = 1 over the whole 128 canvas.
# n=4.8 puts the diagonal inset at 8.6 units, a hair past the old rx=28
# (21.9%) rounded rect's 8.2, but the curvature ramps in gradually from the
# flat side instead of switching on at a tangent point. That ramp is what
# makes the Apple tile read as one soft object instead of a rectangle with
# its corners cut off. Not 5: measured on a real capture, n=5 leaves ~6%
# coverage in the corner block iconhalo-check.py requires to be pure tray
# (it samples physical x 2..5 of the tile, not 0..3, since the check's slot
# origin sits one logical pixel right of where the tile really starts), and
# 4.8 clears it with the curve otherwise indistinguishable at dock size.
SQUIRCLE_N = 4.8
SQUIRCLE_PTS = 288

# Inner top highlight: the squircle's own outline, stroked white, clipped to
# the tile (so only the inner half shows) and faded out HL_FADE units down,
# so it is a lit top lip and never a frame round the sides or bottom.
HL_WIDTH, HL_ALPHA, HL_FADE = 5.0, 0.5, 22

# Glyph contact shadow: small offset, small blur, low alpha. A soft
# grounding under the object, not the old 0.42-alpha drop halo.
DROP_DY, DROP_BLUR, DROP_ALPHA = 1.8, 1.6, 0.30


def squircle_path():
    pts = []
    for i in range(SQUIRCLE_PTS):
        t = 2 * math.pi * i / SQUIRCLE_PTS
        c, s = math.cos(t), math.sin(t)
        x = 64 + 64 * math.copysign(abs(c) ** (2 / SQUIRCLE_N), c)
        y = 64 + 64 * math.copysign(abs(s) ** (2 / SQUIRCLE_N), s)
        pts.append("%.2f %.2f" % (x, y))
    return "M" + " L".join(pts) + " Z"


def lg(id_, *stops, x2=0, y2=1):
    """A linear gradient, top-to-bottom by default (the shared light)."""
    s = "".join('<stop offset="%g" stop-color="%s"/>' % (o, c) for o, c in stops)
    return '<linearGradient id="%s" x1="0" y1="0" x2="%g" y2="%g">%s</linearGradient>' % (id_, x2, y2, s)


# --- the dock designs ----------------------------------------------------------
#
# Each entry: tile top colour, tile bottom colour, extra <defs>, glyph body.
# Every glyph keeps out of the top 16 and bottom 20 units of the canvas: that
# is the tile's own lit band and shaded band, which iconlight-check.py
# measures, and a glyph crossing them would make the check measure the glyph.
# Stroke weights are shared: 8 units for primary strokes, 5 for secondary.

TRASH_DEFS = (
    lg("metal", (0, "#C4C7CE"), (1, "#9A9EA7"))
    + lg("can", (0, "#FFFFFF"), (0.55, "#F4F5F7"), (1, "#C9CCD3"), x2=1, y2=0)
)
TRASH_CAN = """
      <rect x="52" y="22" width="24" height="9" rx="3.5" fill="url(#metal)"/>
      <rect x="29" y="31" width="70" height="11" rx="5" fill="url(#metal)"/>
      <path d="M36 46 H92 L86 90 H42 Z" fill="url(#can)"/>
      <g fill="#8A8F99">
        <rect x="50.5" y="54" width="5" height="28" rx="2.5"/>
        <rect x="61.5" y="54" width="5" height="28" rx="2.5"/>
        <rect x="72.5" y="54" width="5" height="28" rx="2.5"/>
      </g>
      <path d="M41.6 87 H86.4 L85.3 95.4 A3.4 3.4 0 0 1 81.9 98.4 H46.1 A3.4 3.4 0 0 1 42.7 95.4 Z" fill="#FFFFFF"/>"""

DOCK = {
    # Launchpad-style grid: nine colour chips on a light tile.
    "apps": ("#F3F3F6", "#DADBE0",
             lg("chip", (0, "#FFFFFF"), (1, "#000000")),
             "".join(
                 '<rect x="%d" y="%d" width="20" height="20" rx="5.5" fill="%s"/>'
                 '<rect x="%d" y="%d" width="20" height="20" rx="5.5" fill="url(#chip)" opacity="0.16"/>'
                 % (26 + 28 * (i % 3), 26 + 28 * (i // 3), c, 26 + 28 * (i % 3), 26 + 28 * (i // 3))
                 for i, c in enumerate(["#FF5F57", "#FF9F0A", "#FFD60A",
                                        "#32D74B", "#40C8E0", "#0A84FF",
                                        "#5E5CE6", "#BF5AF2", "#FF375F"]))),

    # Burrow (the file browser): a kit fox peeking out of its burrow. Warm
    # sand tile, dark 3px outline like Samantha's face, big ears and eyes.
    "burrow": ("#FBEBD0", "#F0CFA0",
               lg("fur", (0, "#F5A04A"), (1, "#E07F2E"))
               + lg("dirt", (0, "#C08A52"), (1, "#8F5E33"))
               + lg("cream", (0, "#FFF8EA"), (1, "#F6E6C8")),
               """
      <g stroke="#2A1A10" stroke-linecap="round" stroke-linejoin="round" transform="translate(64 18) scale(0.80) translate(-64 -14)">
        <ellipse cx="64" cy="98" rx="50" ry="22" fill="url(#dirt)" stroke-width="3"/>
        <ellipse cx="64" cy="94" rx="38" ry="13" fill="#3A2414" stroke="none"/>
        <path d="M33 54 L27 15 Q49 18 57 40 Z" fill="url(#fur)" stroke-width="3"/>
        <path d="M95 54 L101 15 Q79 18 71 40 Z" fill="url(#fur)" stroke-width="3"/>
        <path d="M35 47 L32 26 Q44 29 50 41 Z" fill="#F7B8A8" stroke="none"/>
        <path d="M93 47 L96 26 Q84 29 78 41 Z" fill="#F7B8A8" stroke="none"/>
        <path d="M27 15 Q38 16 45 24 L32 30 Z" fill="#2A1A10" stroke="none"/>
        <path d="M101 15 Q90 16 83 24 L96 30 Z" fill="#2A1A10" stroke="none"/>
        <ellipse cx="64" cy="64" rx="35" ry="29" fill="url(#fur)" stroke-width="3"/>
        <path d="M30 70 Q44 62 57 71 Q64 76 71 71 Q84 62 98 70 Q94 92 64 93 Q34 92 30 70 Z" fill="url(#cream)" stroke="none"/>
        <ellipse cx="51" cy="60" rx="6" ry="7.5" fill="#2A1A10" stroke="none"/>
        <ellipse cx="77" cy="60" rx="6" ry="7.5" fill="#2A1A10" stroke="none"/>
        <g fill="#FFFFFF" stroke="none"><circle cx="53" cy="57" r="2.4"/><circle cx="79" cy="57" r="2.4"/><circle cx="49.6" cy="63" r="1.1"/><circle cx="75.6" cy="63" r="1.1"/></g>
        <ellipse cx="64" cy="73" rx="4.6" ry="3.4" fill="#2A1A10" stroke="none"/>
        <path d="M57 79 Q60.5 83 64 79.5 Q67.5 83 71 79" fill="none" stroke-width="2.4"/>
        <g fill="#F08C7A" stroke="none" opacity="0.6"><ellipse cx="40" cy="74" rx="5" ry="3.2"/><ellipse cx="88" cy="74" rx="5" ry="3.2"/></g>
        <path d="M14 101 Q64 84 114 101 Q110 118 64 120 Q18 118 14 101 Z" fill="url(#dirt)" stroke-width="3"/>
        <g fill="#2A1A10" stroke="none" opacity="0.35"><ellipse cx="34" cy="108" rx="4" ry="2.2"/><ellipse cx="92" cy="110" rx="5" ry="2.4"/><ellipse cx="62" cy="113" rx="3.5" ry="2"/></g>
      </g>"""),

    # Mail: a white envelope on a blue tile.
    "mail": ("#34A6FF", "#157FF3",
             lg("env", (0, "#FFFFFF"), (1, "#E4EAF3"))
             + lg("flap", (0, "#F6F8FB"), (1, "#D7DFEA"))
             + '<clipPath id="envclip"><rect x="18" y="34" width="92" height="62" rx="8"/></clipPath>',
             """
      <rect x="18" y="34" width="92" height="62" rx="8" fill="url(#env)"/>
      <g clip-path="url(#envclip)">
        <path d="M18 96 L56 64 M110 96 L72 64" stroke="#CBD5E3" stroke-width="2.4" fill="none"/>
        <path d="M14 32 L64 72 L114 32 Z" fill="url(#flap)"/>
        <path d="M18 36 L64 72.5 L110 36" stroke="#B8C5D8" stroke-width="2" fill="none" stroke-linejoin="round" opacity="0.8"/>
      </g>"""),

    # Calendar: a plain white tile, no baked date. v0.89.x: the month/day
    # used to be hand-drawn strokes here ("SEP 17", fixed forever, not
    # <text> since rsvg would pick whatever font the host has and
    # gen_icon_art.py --check would then differ machine to machine); now
    # the real current date is drawn at runtime instead, by
    # gui_calendar_draw_date in kernel/kernel.c, directly on top of this
    # tile's white/shadow art on every icon draw, off the same RTC read
    # the menu bar clock already trusts. Nothing left to bake in: this
    # tile is just the squircle, its top highlight and its drop shadow,
    # same shared technique every other tile below uses, with no glyph
    # body of its own.
    "calendar": ("#F5F5F8", "#E0E1E6", "", ""),

    # Notes: a yellow pencil over ruled paper.
    "notes": ("#F6F6F8", "#E0E1E6",
              lg("wood", (0, "#FFE27A"), (0.5, "#FFC928"), (1, "#E9A400"))
              + lg("ferrule", (0, "#F2F3F5"), (0.5, "#C3C6CC"), (1, "#8E929A"))
              + lg("eraser", (0, "#FFA9B6"), (1, "#EE6A82"))
              + lg("cone", (0, "#F9E2BE"), (1, "#E2BD88")),
              """
      <g stroke="#D5D6DC" stroke-width="3" stroke-linecap="round">
        <path d="M24 38 H104 M24 54 H104 M24 70 H104 M24 86 H104"/>
      </g>
      <g transform="rotate(-45 64 64)">
        <path d="M31 56 L15 64 L31 72 Z" fill="url(#cone)"/>
        <path d="M20.6 61.2 L15 64 L20.6 66.8 Z" fill="#3A3A3E"/>
        <rect x="31" y="56" width="56" height="16" fill="url(#wood)"/>
        <rect x="31" y="61.3" width="56" height="1.6" fill="#FFFFFF" opacity="0.35"/>
        <rect x="87" y="56" width="9" height="16" fill="url(#ferrule)"/>
        <path d="M96 56 H102 A5 5 0 0 1 107 61 V67 A5 5 0 0 1 102 72 H96 Z" fill="url(#eraser)"/>
      </g>"""),

    # Reminders: three coloured rings and their list lines, on a white tile.
    "reminders": ("#F5F5F8", "#E0E1E6", "",
                  "".join(
                      '<circle cx="36" cy="%d" r="9.5" fill="none" stroke="%s" stroke-width="3.2"/>'
                      '<circle cx="36" cy="%d" r="5.2" fill="%s"/>'
                      '<rect x="54" y="%d" width="50" height="5" rx="2.5" fill="#C7C8CE"/>'
                      % (y, c, y, c, y - 2.5)
                      for y, c in ((38, "#0A84FF"), (64, "#FF453A"), (90, "#FF9F0A")))),

    # Terminal: a dark screen with a white prompt, set in an aluminium tile.
    "terminal": ("#EEEEF1", "#D2D3D8",
                 lg("screen", (0, "#3A3A40"), (1, "#1A1A1D")),
                 """
      <rect x="16" y="20" width="96" height="80" rx="11" fill="url(#screen)"/>
      <rect x="18" y="21" width="92" height="1.6" rx="0.8" fill="#FFFFFF" opacity="0.16"/>
      <g fill="none" stroke="#FFFFFF" stroke-width="8" stroke-linecap="round" stroke-linejoin="round">
        <path d="M36 46 L52 59 L36 72"/>
        <path d="M61 76 H84"/>
      </g>"""),

    # Chat (Samantha): her real app icon from the turing repo, the oval
    # portrait she drew, on its own flat paper. art/samantha-icon.svg is a
    # vendored copy of turing/icon.svg; its own rounded rect is dropped so
    # the tile below stays the dock's squircle. Re-copy it when turing's
    # icon changes.
    "chat": ("#ECE8DF", "#ECE8DF", "",
             """<svg x="0" y="0" width="128" height="128" viewBox="0 0 200 200">"""
             + "\n".join(l for l in open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "art", "samantha-icon.svg")).read().split("\n") if not l.lstrip().startswith(("<svg", "</svg>", "<rect")))
             + """</svg>"""),

    # Weather: a sun half behind a cloud, on a sky-blue tile.
    "weather": ("#47A8F8", "#2A86EC",
                lg("sun", (0, "#FFE96E"), (1, "#FFAE1F"))
                + lg("cloud", (0, "#FFFFFF"), (1, "#DCE6F3")),
                """
      <circle cx="50" cy="50" r="23" fill="url(#sun)"/>
      <g fill="url(#cloud)">
        <circle cx="56" cy="78" r="16"/>
        <circle cx="77" cy="69" r="20"/>
        <circle cx="96" cy="81" r="13"/>
        <rect x="38" y="76" width="70" height="18" rx="9"/>
      </g>"""),

    # Stocks: a green trend line over a faint grid, on a graphite tile.
    "stocks": ("#3B3B40", "#1E1E22",
               '<linearGradient id="area" x1="0" y1="0" x2="0" y2="1">'
               '<stop offset="0" stop-color="#32D74B" stop-opacity="0.42"/>'
               '<stop offset="1" stop-color="#32D74B" stop-opacity="0"/></linearGradient>',
               """
      <g stroke="#FFFFFF" stroke-opacity="0.10" stroke-width="2">
        <path d="M18 40 H110 M18 60 H110 M18 80 H110"/>
      </g>
      <path d="M18 86 L36 72 L50 79 L66 55 L80 63 L96 38 L110 45 V100 H18 Z" fill="url(#area)"/>
      <path d="M18 86 L36 72 L50 79 L66 55 L80 63 L96 38 L110 45" fill="none" stroke="#34DA4F" stroke-width="6" stroke-linecap="round" stroke-linejoin="round"/>"""),

    # Trash: a white can with grey lid and ribs, on a light metal tile. The
    # flat white base band at y 87-98 is load-bearing: iconedge-check.py
    # asserts every pixel of it stays >= 240 luminance (the v71.9 rib-stub
    # regression), so it is deliberately one flat fill, not the can gradient.
    "trash": ("#ECEDF0", "#D3D5DB", TRASH_DEFS, TRASH_CAN),
    "trash_full": ("#ECEDF0", "#D3D5DB",
                   TRASH_DEFS + lg("paper", (0, "#FFFFFF"), (1, "#E3DED6")),
                   """
      <circle cx="51" cy="26" r="10" fill="url(#paper)"/>
      <circle cx="73" cy="22" r="11" fill="url(#paper)"/>""" + TRASH_CAN.replace(
                       '<rect x="52" y="22" width="24" height="9" rx="3.5" fill="url(#metal)"/>', "")),
}

# Apps-folder-only tiles that still carried the pre-Big-Sur "glossy" technique
# (commit 9db67b9, a plain rx=28 rect, a radial sheen and a rim stroke bright
# at the top and dark at the bottom): the exact look the Big Sur pass moved
# every DOCK tile away from on owner feedback ("still looks too Windows or
# Linux"). These three never got migrated because they only show in the Apps
# folder grid, not the dock, so DOCK above never touched them. Same shared
# template, same squircle, same soft top light, no dark rim; only the glyph
# bodies (unchanged shapes, ported byte-for-byte from the old files) and
# their own hue differ. Written by the same main() loop as DOCK, into the
# same art/icons/ files, just not shown in the dock tray.
APPS_ONLY = {
    "calculator": ("#A3ADB9", "#3A4450", "",
                   """
      <rect x="26" y="22" width="76" height="84" rx="10" fill="#ECEFF3"/>
      <rect x="34" y="30" width="60" height="18" rx="5" fill="#2E3A49"/>
      <g fill="#7C8899">
        <rect x="34" y="55" width="16" height="13" rx="4"/>
        <rect x="56" y="55" width="16" height="13" rx="4"/>
        <rect x="34" y="73" width="16" height="13" rx="4"/>
        <rect x="56" y="73" width="16" height="13" rx="4"/>
        <rect x="34" y="91" width="38" height="8" rx="4"/>
      </g>
      <rect x="78" y="55" width="16" height="44" rx="4" fill="#E8913C"/>"""),

    # Clock: a white face on a graphite tile, hands at 10:10, orange seconds.
    "clock": ("#4A4F57", "#1C1F24", "",
              """
      <circle cx="64" cy="64" r="40" fill="#F7F7F9"/>
      <g stroke="#1C1F24" stroke-width="3" stroke-linecap="round"><line x1="64.00" y1="31.00" x2="64.00" y2="40.00"/><line x1="80.50" y1="35.42" x2="77.50" y2="40.62"/><line x1="92.58" y1="47.50" x2="87.38" y2="50.50"/><line x1="97.00" y1="64.00" x2="88.00" y2="64.00"/><line x1="92.58" y1="80.50" x2="87.38" y2="77.50"/><line x1="80.50" y1="92.58" x2="77.50" y2="87.38"/><line x1="64.00" y1="97.00" x2="64.00" y2="88.00"/><line x1="47.50" y1="92.58" x2="50.50" y2="87.38"/><line x1="35.42" y1="80.50" x2="40.62" y2="77.50"/><line x1="31.00" y1="64.00" x2="40.00" y2="64.00"/><line x1="35.42" y1="47.50" x2="40.62" y2="50.50"/><line x1="47.50" y1="35.42" x2="50.50" y2="40.62"/></g>
      <g stroke="#1C1F24" stroke-linecap="round">
        <line x1="64" y1="64" x2="47.5" y2="54.5" stroke-width="5"/>
        <line x1="64" y1="64" x2="83" y2="43" stroke-width="3.6"/>
      </g>
      <line x1="64" y1="72" x2="64" y2="32" stroke="#F09A37" stroke-width="1.8" stroke-linecap="round"/>
      <circle cx="64" cy="64" r="3.4" fill="#F09A37"/>"""),

    "search": ("#9AA3B1", "#303A48", "",
               """
      <circle cx="52" cy="52" r="23" fill="none" stroke="#ECEFF3" stroke-width="11"/>
      <line x1="69" y1="69" x2="93" y2="93" stroke="#ECEFF3" stroke-width="12" stroke-linecap="round"/>"""),

    "contacts": ("#C9B4A5", "#614C3C",
                 lg("paper", (0, "#FFFFFF"), (1, "#E9E2DC")),
                 """
      <rect x="24" y="26" width="80" height="76" rx="10" fill="url(#paper)"/>
      <rect x="24" y="26" width="9" height="76" fill="#C7B39F"/>
      <circle cx="68" cy="55" r="14" fill="#A87C5B"/>
      <path d="M46 92 a22 22 0 0 1 44 0 z" fill="#A87C5B"/>"""),
}

TEMPLATE = """<svg xmlns="http://www.w3.org/2000/svg" width="128" height="128" viewBox="0 0 128 128">
  <!-- Written by tools/gen/restyle_icons.py (the "%(name)s" entry): edit
       that, not this file. Tile %(top)s -> %(bot)s, lit from the top. -->
  <defs>
    %(tilegrad)s
    <linearGradient id="hl" gradientUnits="userSpaceOnUse" x1="0" y1="0" x2="0" y2="%(fade)d">
      <stop offset="0" stop-color="#FFFFFF" stop-opacity="%(hla)g"/><stop offset="1" stop-color="#FFFFFF" stop-opacity="0"/>
    </linearGradient>
    <clipPath id="tile"><path d="%(sq)s"/></clipPath>
    <filter id="drop" x="-30%%" y="-30%%" width="160%%" height="170%%">
      <feDropShadow dx="0" dy="%(dy)g" stdDeviation="%(blur)g" flood-color="#000000" flood-opacity="%(alpha)g"/>
    </filter>
    %(defs)s
  </defs>
  <path d="%(sq)s" fill="url(#base)"/>
  <g clip-path="url(#tile)">
    <path d="%(sq)s" fill="none" stroke="url(#hl)" stroke-width="%(hlw)g"/>
    <g filter="url(#drop)">%(body)s
    </g>
  </g>
</svg>
"""


def main():
    sq = squircle_path()
    all_tiles = {**DOCK, **APPS_ONLY}
    for name, (top, bot, defs, body) in all_tiles.items():
        out = TEMPLATE % dict(
            name=name, top=top, bot=bot, sq=sq,
            tilegrad=lg("base", (0, top), (1, bot)),
            fade=HL_FADE, hla=HL_ALPHA, hlw=HL_WIDTH,
            dy=DROP_DY, blur=DROP_BLUR, alpha=DROP_ALPHA,
            defs=defs, body=body)
        open(os.path.join(SVG_DIR, name + ".svg"), "w").write(out)
        print("%-11s %s -> %s" % (name, top, bot))
    print("wrote %d icons (%d dock, %d apps-folder-only)" % (len(all_tiles), len(DOCK), len(APPS_ONLY)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
