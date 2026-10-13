# SPDX-License-Identifier: CC-BY-NC-SA-4.0
# Copyright (c) 2026 Joshua Trommel. Hardware design, see docs/hardware/LICENSE-NOTICE.md
# A flat retail box mock for Nimbus: a cream box, the tree mark centred, "Joshua Tree" small beneath, nothing else. 1600 px wide JPEG.
#   python3 docs/hardware/nimbus_box.py [out.jpg]      (needs Pillow and rsvg-convert)
import os, sys, subprocess, tempfile
from PIL import Image, ImageDraw, ImageFilter, ImageFont, ImageChops
HERE = os.path.dirname(os.path.abspath(__file__))
OUT = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "nimbus-box.jpg")
W, H = 1600, 1100
CREAM, INK, SWEEP = (0xEF, 0xEB, 0xE4), (0x1C, 0x1C, 0x1E), (0xC9, 0xC4, 0xBB)
tmp = tempfile.mkdtemp(); mk = os.path.join(tmp, "mark.png")
subprocess.run(["rsvg-convert", "-h", "480", "-o", mk, os.path.join(HERE, "..", "..", "landing", "mark-tree.svg")], check=True)      # the current brand mark (tools/gen/logo.py), ink with its terracotta ground
bg = Image.new("RGB", (W, H), SWEEP); px = bg.load(); d = ImageDraw.Draw(bg)
for y in range(H):                      # a soft vertical falloff, lighter toward the top like the renders
    t = y / H; c = tuple(int(a + (b - a) * t) for a, b in zip((0xDD, 0xD8, 0xCF), (0xC4, 0xBF, 0xB6))); d.line([(0, y), (W, y)], fill=c)
bw, bh, top = 920, 700, 150          # the box front
x0, y0 = (W - bw) // 2, 250
sh = Image.new("L", (W, H), 0); ImageDraw.Draw(sh).rectangle([x0 - 10, y0 + bh - 20, x0 + bw + 10, y0 + bh + 40], fill=90)
sh = sh.filter(ImageFilter.GaussianBlur(38)); bg.paste((120, 114, 104), (0, 0), sh)
lid = Image.new("RGB", (bw, bh), CREAM); dl = ImageDraw.Draw(lid)
for y in range(bh):                      # one soft top light: a touch brighter at the top, a touch deeper at the foot
    k = y / bh; c = tuple(int(v * (1.02 - 0.06 * k)) if v * (1.02 - 0.06 * k) < 255 else 255 for v in CREAM); dl.line([(0, y), (bw, y)], fill=c)
bg.paste(lid, (x0, y0))
d = ImageDraw.Draw(bg)
d.rectangle([x0, y0 - 26, x0 + bw, y0], fill=(0xF6, 0xF2, 0xEB))              # the lid's top face, seen a little from above
d.line([(x0, y0), (x0 + bw, y0)], fill=(0xD9, 0xD3, 0xC8), width=2)
d.line([(x0, y0 + bh - 1), (x0 + bw, y0 + bh - 1)], fill=(0xD2, 0xCC, 0xC0), width=3)
m = Image.open(mk).convert("RGBA"); a = m.split()[3]
ink = m
mx, my = x0 + (bw - m.width) // 2, y0 + 40
bg.paste(ink, (mx, my), ink)
f = ImageFont.truetype("/System/Library/Fonts/Helvetica.ttc", 34)
txt, sp = "Joshua Tree", 5
tw = sum(d.textlength(ch, font=f) + sp for ch in txt) - sp
cx = x0 + (bw - tw) / 2; ty = my + int(m.height * 0.93) + 40
for ch in txt:
    d.text((cx, ty), ch, font=f, fill=(0x5A, 0x56, 0x4F)); cx += d.textlength(ch, font=f) + sp
bg.save(OUT, quality=92)
print("wrote", OUT)
