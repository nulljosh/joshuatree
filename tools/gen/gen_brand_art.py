#!/usr/bin/env python3
"""Rasterize the brand mark (landing/logo.svg, drawn by tools/gen/logo.py) into the PNG copies of it.

  landing/mark.png        280x280, the small weight (landing/mark-bold.svg, no grain) on paper, the PNG
                          fallback favicon and anything drawn small
  landing/badge.png       1024x1024, the crayon mark on paper, for large artwork
  landing/og-image.png    1200x630 share card; this script only redraws the mark in the top
                          part, the title and tagline below it are kept from the file as it is
  docs/brand/mark-ink.png    480x480 transparent, ink on nothing, what README shows on a light page
  docs/brand/mark-paper.png  480x480 transparent, paper ink, what README shows on a dark page
                             (README loads both from a GitHub release, which release.yml uploads them to)
  docs/brand/mascot.png      1024x1024, the mascot (landing/mascot.svg, tools/gen/mascot.py) on paper

3.0: the mark has two colours, ink and the terracotta ground, so every copy is the SVG's own colour render laid
on the page, not one ink poured through its alpha. On a dark page only the ink turns to paper.

Run after tools/gen/logo.py, from the repo root:  python3 tools/gen/gen_brand_art.py
Uses rsvg-convert (ImageMagick draws this SVG blank on this box) and Pillow.
"""
import io
import os
import subprocess

from PIL import Image

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
SVG = os.path.join(ROOT, "landing", "logo.svg")
BOLD = os.path.join(ROOT, "landing", "mark-bold.svg")
MASCOT = os.path.join(ROOT, "landing", "mascot.svg")
INK = "#161513"              # the ink the mark is drawn in
PAPER_INK = "#f5f5f3"        # the ink on a dark page
MARK_BG = (250, 248, 244)    # #faf8f4, the page
OG_BG = (250, 248, 244)
OG_MARK = 324                # px the mark is rasterized at on the share card
OG_X, OG_Y = 438, 52         # top-left of that square, centred on the card


def render(px, svg=SVG, ink=INK):
    """The mark in its own colours on nothing, with the ink swapped for `ink`."""
    txt = open(svg).read().replace(f'stroke="{INK}"', f'stroke="{ink}"').replace(f"stroke:{INK}", f"stroke:{ink}")
    png = subprocess.run(["rsvg-convert", "-w", str(px), "-h", str(px), "-"], input=txt.encode(), check=True,
                         capture_output=True).stdout
    return Image.open(io.BytesIO(png)).convert("RGBA")


def flat(px, bg, svg=SVG):
    im = Image.new("RGBA", (px, px), bg + (255,))
    im.alpha_composite(render(px, svg))
    return im.convert("RGB")


def main():
    flat(280, MARK_BG, BOLD).save(os.path.join(ROOT, "landing", "mark.png"), optimize=True)
    flat(1024, MARK_BG).save(os.path.join(ROOT, "landing", "badge.png"), optimize=True)
    render(480).save(os.path.join(ROOT, "docs", "brand", "mark-ink.png"), optimize=True)
    render(480, ink=PAPER_INK).save(os.path.join(ROOT, "docs", "brand", "mark-paper.png"), optimize=True)
    flat(1024, MARK_BG, MASCOT).save(os.path.join(ROOT, "docs", "brand", "mascot.png"), optimize=True)
    og_path = os.path.join(ROOT, "landing", "og-image.png")
    og = Image.open(og_path).convert("RGB")
    og.paste(Image.new("RGB", (og.width, 400), OG_BG), (0, 0))   # wipe the old mark, keep the title below
    og.paste(flat(OG_MARK, OG_BG), (OG_X, OG_Y))
    og.save(og_path, optimize=True)
    print("wrote landing/mark.png, landing/badge.png, landing/og-image.png, docs/brand/mark-ink.png, docs/brand/mark-paper.png, docs/brand/mascot.png")


if __name__ == "__main__":
    main()
