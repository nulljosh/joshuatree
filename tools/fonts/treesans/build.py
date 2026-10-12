#!/usr/bin/env python3
"""Builds Tree Sans, the Joshua Tree typeface, from Inter 4.1 (SIL OFL 1.1), the way Nimble Sans was made.

    python3 tools/fonts/treesans/build.py INTER_VARIABLE.ttf OUT_DIR

The constraints (docs/TYPE.md) are enforced here, not hoped for:
  * every vertical metric is a multiple of 1/16 em (128 units of 2048): ascent 15, descent 4, line gap 0, so the line box
    is 19/16 em and a size on the 4 px grid gives a line on the 4 px grid;
  * one face per optical size: Text is Inter's opsz 14 (open apertures, looser fit), Display is opsz 32 (tight, for 22 px and up);
  * one frozen set of alternates, decided once: tailed l (cv05) and serifed capital I (cv08) so I, l and 1 never read alike on a
    small screen, and round quotes and commas (ss03). Tabular digits stay one feature away (tnum), proportional by default;
  * renamed Tree Sans, as the OFL asks of a modified font; Inter's copyright and licence stay.
Outputs TTF and WOFF2 for Regular, Medium, SemiBold and Bold of Text, and SemiBold and Bold of Display.
"""
import os, subprocess, sys, tempfile
from fontTools.ttLib import TTFont
from fontTools.varLib import instancer
from fontTools import subset

UPM, U = 2048, 128                      # one grid unit is 1/16 em
ASC, DESC, GAP = 15 * U, 4 * U, 0       # 1920, 512, 0
FREEZE = "cv05,cv08,ss03"
KEEP_FEATURES = ["kern", "liga", "calt", "case", "tnum", "pnum", "frac", "ccmp", "locl", "zero"]
UNICODES = "U+0020-007E,U+00A0-00FF,U+2013,U+2014,U+2018,U+2019,U+201C,U+201D,U+2022,U+2026,U+2190-2193,U+2212,U+2248,U+2260,U+2264,U+2265,U+221A,U+221E,U+00D7,U+00F7"
FACES = [  # family, style, wght, opsz, weight class, bold
    ("Tree Sans", "Regular", 400, 14, 400), ("Tree Sans", "Medium", 500, 14, 500),
    ("Tree Sans", "SemiBold", 600, 14, 600), ("Tree Sans", "Bold", 700, 14, 700),
    ("Tree Sans Display", "SemiBold", 600, 32, 600), ("Tree Sans Display", "Bold", 700, 32, 700),
]


def rename(font, family, style, weight):
    ps = f"{family.replace(' ', '')}-{style}"
    full = f"{family} {style}"
    name = font["name"]
    keep = [n for n in name.names if n.nameID in (0, 13, 14)]       # copyright, licence text and URL stay
    name.names = keep
    cr = next((n.toUnicode() for n in keep if n.nameID == 0), "")
    for rec in [n for n in name.names if n.nameID == 0]: name.names.remove(rec)
    name.setName(cr + " Modified by Joshua Trommel as Tree Sans.", 0, 3, 1, 0x409)
    name.setName(family if style in ("Regular", "Bold") else f"{family} {style}", 1, 3, 1, 0x409)
    name.setName(style if style in ("Regular", "Bold") else "Regular", 2, 3, 1, 0x409)
    name.setName(f"{ps};{font['head'].fontRevision:.3f}", 3, 3, 1, 0x409)
    name.setName(full, 4, 3, 1, 0x409)
    name.setName("Version 1.000", 5, 3, 1, 0x409)
    name.setName(ps, 6, 3, 1, 0x409)
    name.setName(family, 16, 3, 1, 0x409)
    name.setName(style, 17, 3, 1, 0x409)
    for rec in list(name.names):                                    # drop Mac records so one set of names remains
        if rec.platformID == 1: name.names.remove(rec)
    os2 = font["OS/2"]
    os2.usWeightClass = weight
    os2.fsSelection = (os2.fsSelection & ~0b1100001) | (0b100000 if style == "Bold" else 0b1000000 if style == "Regular" else 0) | (1 << 7)
    font["head"].macStyle = 1 if style == "Bold" else 0
    if "STAT" in font: del font["STAT"]


def snap_metrics(font):
    """Vertical metrics on the 1/16 em grid; the Windows clip box still covers every glyph."""
    hhea, os2 = font["hhea"], font["OS/2"]
    hhea.ascent, hhea.descent, hhea.lineGap = ASC, -DESC, GAP
    os2.sTypoAscender, os2.sTypoDescender, os2.sTypoLineGap = ASC, -DESC, GAP
    ymax = max((g.yMax for g in (font["glyf"][n] for n in font.getGlyphOrder()) if g.numberOfContours), default=ASC)
    ymin = min((g.yMin for g in (font["glyf"][n] for n in font.getGlyphOrder()) if g.numberOfContours), default=-DESC)
    os2.usWinAscent, os2.usWinDescent = max(ASC, ymax), max(DESC, -ymin)
    os2.fsSelection |= 1 << 7                                       # USE_TYPO_METRICS
    font["post"].underlinePosition, font["post"].underlineThickness = -2 * U // 4, U // 4   # 1/32 em stroke, one value for the family


def build(src, out):
    os.makedirs(out, exist_ok=True)
    for family, style, wght, opsz, weight in FACES:
        var = TTFont(src)
        inst = instancer.instantiateVariableFont(var, {"wght": wght, "opsz": opsz}, inplace=False)
        with tempfile.TemporaryDirectory() as t:
            a, b = os.path.join(t, "a.ttf"), os.path.join(t, "b.ttf")
            inst.save(a)
            subprocess.run([sys.executable, "-m", "opentype_feature_freezer.cli", "-f", FREEZE, a, b], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            opts = subset.Options(); opts.layout_features = KEEP_FEATURES; opts.hinting = False; opts.notdef_outline = True
            opts.name_IDs = ["*"]; opts.drop_tables += ["DSIG"]
            f = TTFont(b)
            s = subset.Subsetter(opts); s.populate(unicodes=subset.parse_unicodes(UNICODES)); s.subset(f)
            snap_metrics(f); rename(f, family, style, weight)
            base = os.path.join(out, f"{family.replace(' ', '')}-{style}")
            f.save(base + ".ttf")
            f.flavor = "woff2"; f.save(base + ".woff2")
            print(f"{os.path.basename(base)}: {os.path.getsize(base + '.ttf')} ttf, {os.path.getsize(base + '.woff2')} woff2")


if __name__ == "__main__":
    build(sys.argv[1], sys.argv[2])
