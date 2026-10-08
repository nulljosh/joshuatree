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
    # Launchpad-style grid: nine flat colour chips on a light tile.
    "apps": ("#F3F3F6", "#DADBE0",
             "",
             "".join(
                 '<rect x="%d" y="%d" width="20" height="20" rx="5.5" fill="%s"/>'
                 % (26 + 28 * (i % 3), 26 + 28 * (i // 3), c)
                 for i, c in enumerate(["#FF5F57", "#FF9F0A", "#FFD60A",
                                        "#32D74B", "#5B9BD5", "#0A84FF",
                                        "#A87C5B", "#8A8F99", "#FF375F"]))),

    # Burrow (the file browser): the Finder idea done in the house colours.
    # The whole tile is the face, split left/right into a pale periwinkle
    # and a cobalt (blue at Joshua's ask, 2026-10-07, in a cobalt that is neither Finder's sky blue nor Mail's)
    # and the accent, with two dots and one smile across the split in the
    # deep tone. Full-bleed like Finder, one bold object like Mail or
    # Terminal, no mascot: the fox, the plain folder, the arch head and the
    # hill-with-a-hole all lost to this at 64 px. The tile is Samantha's lit
    # accent ramp, so the dark half keeps the one shared top light. The
    # full-bleed half is allow-listed in iconart, iconlight and iconinset
    # (FULL_BLEED there): they sample Burrow's dark half only.
    "burrow": ("#4F6FF2", "#2D47C4", "",
               """
      <path d="M0 0 H66 C64 40 60 52 60 64 C60 78 64 90 66 128 H0 Z" fill="#A9B9FF"/>
      <rect x="40" y="40" width="11" height="20" rx="5.5" fill="#070A12"/>
      <rect x="77" y="40" width="11" height="20" rx="5.5" fill="#070A12"/>
      <path d="M36 80 Q64 102 92 80" fill="none" stroke="#070A12" stroke-width="7" stroke-linecap="round"/>"""),

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

    # Chat (Samantha): a voice orb, not a speech bubble and not an engraving. She is voice first
    # (hold F2 to talk), so the icon is a cream disc with five rounded waveform bars, the tallest
    # in the middle. Tile is the landing design system's --accent #b5502c lit from the top, disc
    # and bars its --bg #faf8f4 and --accent. Her engraved portrait (art/samantha-icon.svg) is too
    # fine to read at dock size, so it stays in her window and the Turing mark.
    "chat": ("#C65E37", "#A24526", "",
             """
      <circle cx="64" cy="62" r="38" fill="#FAF8F4"/>
      <g stroke="#B5502C" stroke-width="8" stroke-linecap="round">
        <path d="M43 56 V68"/><path d="M53.5 46 V78"/><path d="M64 38 V86"/><path d="M74.5 46 V78"/><path d="M85 56 V68"/>
      </g>"""),

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

    # Activity: a graphite tile, one green pulse line. Flat, no gloss.
    "activity": ("#4A4F57", "#1C1F24", "",
                 """
      <polyline points="22,66 44,66 54,38 68,92 78,56 84,66 106,66" fill="none" stroke="#34C759" stroke-width="9" stroke-linecap="round" stroke-linejoin="round"/>"""),

    # Music: two beamed notes, cream on the design-system accent #b5502c. Flat: the tile ramp has
    # the same colour at both ends, so only the shared top lip and the contact shadow remain. No text.
    "music": ("#B5502C", "#B5502C", "",
              """
      <g fill="#FAF8F4">
        <circle cx="45" cy="92" r="14"/>
        <circle cx="83" cy="84" r="14"/>
        <rect x="55" y="34" width="7" height="58"/>
        <rect x="93" y="26" width="7" height="58"/>
        <polygon points="55,34 100,26 100,44 55,52"/>
      </g>"""),

    # Hamurabi: a stepped ziggurat with a shrine on top, cream on the design-system accent #b5502c. Flat, same ramp
    # as Music, so only the shared top lip and the contact shadow remain. The stair is the tile colour cut through the tiers. No text.
    "hamurabi": ("#B5502C", "#B5502C", "",
                 """
      <g fill="#F8F8F6">
        <rect x="14" y="84" width="100" height="20" rx="3"/>
        <rect x="27" y="63" width="74" height="19" rx="3"/>
        <rect x="40" y="42" width="48" height="19" rx="3"/>
        <rect x="52" y="22" width="24" height="18" rx="3"/>
      </g>
      <g fill="#B5502C">
        <rect x="60" y="29" width="8" height="11"/>
        <rect x="59" y="42" width="10" height="62"/>
      </g>"""),

    # Claude (2.14): an eight-spoke spark, cream on the design-system accent #b5502c. Flat, the same
    # ramp as Music, so only the shared top lip and the contact shadow remain. No text.
    "claude": ("#B5502C", "#B5502C", "",
               """
      <g stroke="#FAF8F4" stroke-width="11" stroke-linecap="round">
        <line x1="64" y1="28" x2="64" y2="96"/>
        <line x1="30" y1="62" x2="98" y2="62"/>
        <line x1="40" y1="38" x2="88" y2="86"/>
        <line x1="88" y1="38" x2="40" y2="86"/>
      </g>
      <circle cx="64" cy="62" r="12" fill="#FAF8F4"/>"""),

    # Mines (2.32): one cream mine on the accent #b5502c, flat like Claude and Hamurapi. A round
    # body, four bold spikes and a small accent glint. No text, no flag, no grid.
    "mines": ("#B5502C", "#B5502C", "",
              """
      <g stroke="#FAF8F4" stroke-width="10" stroke-linecap="round">
        <line x1="64" y1="26" x2="64" y2="98"/>
        <line x1="28" y1="62" x2="100" y2="62"/>
        <line x1="40" y1="38" x2="88" y2="86"/>
        <line x1="88" y1="38" x2="40" y2="86"/>
      </g>
      <circle cx="64" cy="62" r="25" fill="#FAF8F4"/>
      <circle cx="55" cy="53" r="6" fill="#B5502C"/>"""),

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
