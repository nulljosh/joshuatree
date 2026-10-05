#!/usr/bin/env python3
"""Rasterize the brand mark (landing/logo.svg, drawn by tools/gen/logo.py) into the PNG copies of it.

  landing/mark.png        280x280, the mark on paper (#f4f0e6), the PNG fallback favicon
  landing/og-image.png    1200x630 share card; this script only redraws the mark in the top
                          part, the title and tagline below it are kept from the file as it is
  docs/brand/mark-ink.png    480x480 transparent, ink on nothing, what README shows on a light page
  docs/brand/mark-paper.png  480x480 transparent, paper ink, what README shows on a dark page

Run after tools/gen/logo.py, from the repo root:  python3 tools/gen/gen_brand_art.py
Uses rsvg-convert (ImageMagick draws this SVG blank on this box) and Pillow.
"""
import io
import os
import subprocess

from PIL import Image

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
SVG = os.path.join(ROOT, "landing", "logo.svg")
INK = (22, 21, 19)          # #161513, the ink the mark is drawn in
PAPER_INK = (245, 245, 243)  # the ink on a dark page
MARK_BG = (244, 240, 230)
OG_BG = (250, 248, 244)
OG_MARK = 324               # px the mark is rasterized at on the share card
OG_X, OG_Y = 438, 52        # top-left of that square, centred on the card


def alpha(px):
    png = subprocess.run(["rsvg-convert", "-w", str(px), "-h", str(px), SVG], check=True, capture_output=True).stdout
    return Image.open(io.BytesIO(png)).convert("RGBA").split()[3]


def flat(px, ink, bg):
    im = Image.new("RGB", (px, px), bg)
    im.paste(Image.new("RGB", (px, px), ink), (0, 0), alpha(px))
    return im


def transparent(px, ink):
    im = Image.new("RGBA", (px, px), ink + (0,))
    im.putalpha(alpha(px))
    return im


def main():
    flat(280, INK, MARK_BG).save(os.path.join(ROOT, "landing", "mark.png"), optimize=True)
    transparent(480, INK).save(os.path.join(ROOT, "docs", "brand", "mark-ink.png"), optimize=True)
    transparent(480, PAPER_INK).save(os.path.join(ROOT, "docs", "brand", "mark-paper.png"), optimize=True)
    og_path = os.path.join(ROOT, "landing", "og-image.png")
    og = Image.open(og_path).convert("RGB")
    og.paste(Image.new("RGB", (og.width, 400), OG_BG), (0, 0))   # wipe the old mark, keep the title below
    og.paste(Image.new("RGB", (OG_MARK, OG_MARK), INK), (OG_X, OG_Y), alpha(OG_MARK))
    og.save(og_path, optimize=True)
    print("wrote landing/mark.png, landing/og-image.png, docs/brand/mark-ink.png, docs/brand/mark-paper.png")


if __name__ == "__main__":
    main()
