#!/usr/bin/env python3
"""The PC build's picture wallpaper from the Pi's: decodes arch/arm64/park_wall.h (Joshua Tree at dusk, 960 x 540,
16 colours, two pixels a byte) with the same scanline the Pi draws (every other row at four fifths), then packs it
into drivers/wallpaper.h through wallraw.write_header. 13.22.0: the PC's photo was a tree photo while the Pi showed
the painting, so the landing's hero (filmed on the PC) and its Pi clips showed two different desktops.

Usage: tools/gen/park_to_photo.py   (from anywhere)"""
import os, re, sys
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import wallraw

src = open(os.path.join(ROOT, "arch", "arm64", "park_wall.h")).read()
W = int(re.search(r"#define PARK_W (\d+)", src).group(1))
H = int(re.search(r"#define PARK_H (\d+)", src).group(1))
pal = [int(t, 16) for t in re.findall(r"0x([0-9A-Fa-f]{8})u", src[src.index("park_pal"):src.index("park_px")])]
body = src[src.index("park_px"):]
px = bytes(int(t) for t in re.findall(r"\d+", body[body.index("{"):body.rindex("}")]))
assert len(pal) == 16 and len(px) == W * H // 2, (len(pal), len(px))

def rgb(c, dim):
    r, g, b = c >> 16 & 255, c >> 8 & 255, c & 255
    return bytes((r * 4 // 5, g * 4 // 5, b * 4 // 5)) if dim else bytes((r, g, b))

lut = [[rgb(c, d) for c in pal] for d in (0, 1)]
out = bytearray()
for y in range(H):
    row, l = px[y * (W // 2):(y + 1) * (W // 2)], lut[y & 1]
    for b in row:
        out += l[b >> 4] + l[b & 15]
wallraw.write_header(bytes(out), W, H)
print("wrote drivers/wallpaper.h from arch/arm64/park_wall.h (%d x %d)" % (W, H))
