#!/usr/bin/env python3
"""Lay the brand mark into its PNG copies.

7.10: the mark is a painted Joshua tree (docs/brand/tree.png, 1024 px on nothing), in the naturalistic gouache style
of a wildlife album cover, replacing the crayon tree tools/gen/logo.py drew. Every copy below is that painting on the
page; it keeps its own colours on light and dark pages alike.

  docs/brand/glyph.png    1024x1024, the painting traced flat (bark ink, terracotta soil) for 32 px and under
  landing/favicon-32.png  32x32, that glyph, the tab icon
  landing/tree.png        400x400, the painting on nothing, the landing hero and footer seal
  landing/mark.png        280x280, the painting on paper, the large favicon and touch icon
  landing/badge.png       1024x1024, the painting on paper, for large artwork
  landing/og-image.png    1200x630 share card; this script only redraws the mark in the top
                          part, the title and tagline below it are kept from the file as it is
  docs/brand/mark-ink.png    480x480, the painting on nothing, what README shows on a light page
  docs/brand/mark-paper.png  480x480, the same, what README shows on a dark page
                             (README loads both from a GitHub release, which release.yml uploads them to)
  landing/nimbus-print-front.webp, landing/nimbus-print-parts.webp   1600 wide, the Nimbus front elevation and
                             exploded sheets as a brown print, for the landing's build section
  landing/brand/x-avatar.png 400x400, the painting on paper with room for X's round crop
  landing/brand/x-banner.png 1500x500, the footer desert (landing/footer-landscape.svg) along the bottom of paper
                             (release.yml sends both to @joshuatreeOS when they change)

From the repo root:  python3 tools/gen/gen_brand_art.py
Uses Pillow, and rsvg-convert for the footer desert.
"""
import io
import os
import subprocess

from PIL import Image, ImageChops, ImageDraw, ImageFilter

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
TREE = os.path.join(ROOT, "docs", "brand", "tree.png")
LANDSCAPE = os.path.join(ROOT, "landing", "footer-landscape.svg")
MARK_BG = (244, 243, 236)    # #f4f3ec, the page: the painting's own paper
OG_BG = (250, 248, 244)
BARK = (38, 26, 19)          # #261a13, the ink: the painting's darkest bark
GROUND = (181, 80, 44)       # #b5502c, terracotta: the painting's soil
OG_MARK = 324                # px the mark is rasterized at on the share card
OG_X, OG_Y = 438, 52         # top-left of that square, centred on the card


def render(px):
    """The painted tree on nothing, px square."""
    return Image.open(TREE).convert("RGBA").resize((px, px), Image.LANCZOS)


def glyph():
    """The small-size mark: the painting traced flat, the tree in bark ink and its soil in terracotta, for 32 px and
    under (favicon, menu bars), where the painting's brushwork turns to mud."""
    t = Image.open(TREE).convert("RGBA")
    r, g, _, a = t.split()
    on = lambda im, v: im.point(lambda x: 255 if x > v else 0)
    low = Image.new("L", t.size, 0)
    ImageDraw.Draw(low).rectangle((0, int(t.height * 0.86), t.width, t.height), fill=255)
    soil = ImageChops.multiply(ImageChops.multiply(on(a, 76), low), ImageChops.multiply(on(ImageChops.subtract(r, g), 35), on(r, 140)))
    tree = ImageChops.subtract(on(a, 89), soil)
    out = Image.new("RGBA", t.size)
    out.paste(GROUND + (255,), mask=on(soil.filter(ImageFilter.GaussianBlur(8)), 110))
    out.paste(BARK + (255,), mask=on(tree.filter(ImageFilter.GaussianBlur(2)), 110))
    return out


def brownprint(sheet, px):
    """A Nimbus blueprint sheet (docs/hardware/nimbus/pack/sheets, drawn from the CAD) as a Van Dyke brown print:
    cream lines on bark brown. Same lines, same numbers; only the colours change, so it stays true to the CAD."""
    import re
    svg = open(os.path.join(ROOT, "docs", "hardware", "nimbus", "pack", "sheets", sheet)).read()
    svg = re.sub(r'(<text[^>]*?)fill="#(1C1C1E|FAF8F4)"', r'\1fill="#F2E6D0"', svg)
    svg = svg.replace('stroke="#FAF8F4"', 'stroke="#C9B393"')
    for old, new in (('fill="#1C1C1E"', 'fill="#1E140D"'), ('stroke="#1C1C1E"', 'stroke="#F2E6D0"'), ("#8A857C", "#C9B393"),
                     ("#EFEBE4", "#46301F"), ("#FAF8F4", "#3A2618"), ("#B5502C", "#E08A5F")):
        svg = svg.replace(old, new)
    png = subprocess.run(["rsvg-convert", "-w", str(px), "-"], input=svg.encode(), check=True, capture_output=True).stdout
    return Image.open(io.BytesIO(png)).convert("RGB")


def flat(px, bg):
    im = Image.new("RGBA", (px, px), bg + (255,))
    im.alpha_composite(render(px))
    return im.convert("RGB")


def main():
    g = glyph()
    g.save(os.path.join(ROOT, "docs", "brand", "glyph.png"), optimize=True)
    g.resize((32, 32), Image.LANCZOS).save(os.path.join(ROOT, "landing", "favicon-32.png"), optimize=True)
    render(400).save(os.path.join(ROOT, "landing", "tree.png"), optimize=True)
    flat(280, MARK_BG).save(os.path.join(ROOT, "landing", "mark.png"), optimize=True)
    flat(1024, MARK_BG).save(os.path.join(ROOT, "landing", "badge.png"), optimize=True)
    render(480).save(os.path.join(ROOT, "docs", "brand", "mark-ink.png"), optimize=True)
    render(480).save(os.path.join(ROOT, "docs", "brand", "mark-paper.png"), optimize=True)   # the painting reads on dark too
    avatar = Image.new("RGB", (400, 400), MARK_BG)
    avatar.paste(flat(300, MARK_BG), (50, 50))   # the full crayon mark; X crops a circle, so keep it inside
    avatar.save(os.path.join(ROOT, "landing", "brand", "x-avatar.png"), optimize=True)
    brownprint("sheet1.svg", 1600).save(os.path.join(ROOT, "landing", "nimbus-print-front.webp"), "WEBP", quality=90)
    brownprint("sheet6.svg", 1600).save(os.path.join(ROOT, "landing", "nimbus-print-parts.webp"), "WEBP", quality=90)
    land = subprocess.run(["rsvg-convert", "-w", "1500", LANDSCAPE], check=True, capture_output=True).stdout
    land = Image.open(io.BytesIO(land)).convert("RGBA")
    banner = Image.new("RGBA", (1500, 500), MARK_BG + (255,))
    banner.alpha_composite(land, (0, 500 - land.height))
    banner.convert("RGB").save(os.path.join(ROOT, "landing", "brand", "x-banner.png"), optimize=True)
    og_path = os.path.join(ROOT, "landing", "og-image.png")
    og = Image.open(og_path).convert("RGB")
    og.paste(Image.new("RGB", (og.width, 400), OG_BG), (0, 0))   # wipe the old mark, keep the title below
    og.paste(flat(OG_MARK, OG_BG), (OG_X, OG_Y))
    og.save(og_path, optimize=True)
    print("wrote docs/brand/glyph.png, landing/tree.png, landing/favicon-32.png, landing/mark.png, landing/badge.png, landing/og-image.png, docs/brand/mark-ink.png, docs/brand/mark-paper.png, landing/nimbus-print-*.webp, landing/brand/x-avatar.png, landing/brand/x-banner.png")


if __name__ == "__main__":
    main()
