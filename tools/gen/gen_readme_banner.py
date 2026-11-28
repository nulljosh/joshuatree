#!/usr/bin/env python3
"""The README's one banner (issue #1156): the landing hero, as a still.

Same parts as landing/index.html's .hero-card, so the README and the site read as one brand: the park painting
(docs/brand/park-wall.png, anchored at its foot like `50% 100% / cover`), the soft night wash behind the words, the
traced glyph in one ink (the boot splash rule), "Joshua Tree" in Tree Sans SemiBold, the headline in Tree Sans Display
Bold at -0.035em with "from scratch." in the hero's gold #ffd98a, and the sub line at 86 percent white. Corners are
the hero card's 28 px radius, transparent, so it sits on GitHub's light and dark pages alike.

Fonts come from landing/fonts/*.woff2 (decompressed with fontTools; needs `pip install fonttools brotli`).
Usage: python3 tools/gen/gen_readme_banner.py   -> docs/brand/readme-banner.png, 1600 x 640 (drawn at 2x, then scaled down)
"""
import io, os
from PIL import Image, ImageDraw, ImageFilter, ImageFont
from fontTools.ttLib import TTFont

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
S = 2                                    # drawn at 2x for sharp text on retina screens
W, H, R = 1600 * S, 640 * S, 28 * S
GOLD, WHITE = (0xFF, 0xD9, 0x8A), (255, 255, 255)
HEAD_A, HEAD_B = "A computer, ", "from scratch."
SUB = "Free and open source. Runs on a Raspberry Pi, a PC, or your browser."


def font(name, px):
    t = TTFont(os.path.join(ROOT, "landing/fonts", name))
    t.flavor = None
    b = io.BytesIO(); t.save(b); b.seek(0)
    return ImageFont.truetype(b, px)


def tracked(draw, xy, text, f, fill, track):    # letter-spacing in px, as CSS letter-spacing does
    x, y = xy
    for ch in text:
        draw.text((x, y), ch, font=f, fill=fill)
        x += f.getlength(ch) + track
    return x


def width(text, f, track): return sum(f.getlength(c) + track for c in text) - track


wall = Image.open(os.path.join(ROOT, "docs/brand/park-wall.png")).convert("RGB")
k = max(W / wall.width, H / wall.height)                      # cover
wall = wall.resize((round(wall.width * k), round(wall.height * k)), Image.LANCZOS)
x0 = (wall.width - W) // 2
img = wall.crop((x0, wall.height - H, x0 + W, wall.height)).convert("RGBA")   # 50% 100%: the trees and the horizon stay

wash = Image.new("L", (W, H), 0)                              # radial-gradient(ellipse 60% 42% at 50% 28%, rgba(24,14,46,.38), transparent 75%)
ImageDraw.Draw(wash).ellipse((W * 0.5 - W * 0.6 * 0.75, H * 0.30 - H * 0.42 * 0.75, W * 0.5 + W * 0.6 * 0.75, H * 0.30 + H * 0.42 * 0.75), fill=int(255 * 0.38))
wash = wash.filter(ImageFilter.GaussianBlur(120 * S))
img = Image.composite(Image.new("RGBA", (W, H), (24, 14, 46, 255)), img, wash)

shadow = Image.new("RGBA", (W, H), (0, 0, 0, 0))              # text-shadow: 0 2px 24px rgba(20,10,40,.35)
text = Image.new("RGBA", (W, H), (0, 0, 0, 0))
ds, dt = ImageDraw.Draw(shadow), ImageDraw.Draw(text)

glyph = Image.open(os.path.join(ROOT, "docs/brand/glyph.png")).convert("RGBA")
glyph = glyph.crop(glyph.getbbox())
gh = 64 * S
glyph = glyph.resize((round(glyph.width * gh / glyph.height), gh), Image.LANCZOS)
ink = Image.new("RGBA", glyph.size, WHITE + (255,)); ink.putalpha(glyph.getchannel("A"))   # one ink

name_f = font("TreeSans-SemiBold.woff2", 30 * S)
name_w = width("Joshua Tree", name_f, 0)
gap = 14 * S
lx = (W - (glyph.width + gap + name_w)) // 2
ly = 58 * S
text.alpha_composite(ink, (int(lx), ly))
tracked(dt, (lx + glyph.width + gap, ly + gh // 2 - 20 * S), "Joshua Tree", name_f, WHITE, 0)

head_f = font("TreeSansDisplay-Bold.woff2", 92 * S)
tr = -0.035 * 92 * S
hw = width(HEAD_A + HEAD_B, head_f, tr) + tr
hx, hy = (W - hw) / 2, 140 * S
for d in (ds, dt):
    fill_a, fill_b = ((20, 10, 40, 90), (20, 10, 40, 90)) if d is ds else (WHITE, GOLD)
    off = 2 * S if d is ds else 0
    x = tracked(d, (hx, hy + off), HEAD_A, head_f, fill_a, tr)
    tracked(d, (x, hy + off), HEAD_B, head_f, fill_b, tr)

sub_f = font("TreeSans-Regular.woff2", 22 * S)
sw = width(SUB, sub_f, 0)
dt.text(((W - sw) / 2, 276 * S), SUB, font=sub_f, fill=(255, 255, 255, int(255 * 0.86)))

img.alpha_composite(shadow.filter(ImageFilter.GaussianBlur(12 * S)))
img.alpha_composite(text)

mask = Image.new("L", (W, H), 0)
ImageDraw.Draw(mask).rounded_rectangle((0, 0, W - 1, H - 1), R, fill=255)
img.putalpha(mask)
out = os.path.join(ROOT, "docs/brand/readme-banner.png")
img = img.resize((1600, 640), Image.LANCZOS)                  # 1600 wide is 2x the README column: sharp, and under 1 MB
a = img.getchannel("A")
img = img.convert("RGB").quantize(256, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.FLOYDSTEINBERG).convert("RGBA")   # 13.27.0: the painting is smooth now, so the palette is dithered
img.putalpha(a)
img.save(out, optimize=True)
print(f"saved {os.path.relpath(out, ROOT)} {img.size} {os.path.getsize(out)} bytes")
