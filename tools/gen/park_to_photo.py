#!/usr/bin/env python3
"""The PC build's picture wallpaper from the Pi's: decodes arch/arm64/park_wall.h (Joshua Tree at dusk, 960 x 540,
256 colours a byte a pixel since 13.27.0, the soft painting; 16 colours with a scanline before), then packs it
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
assert len(pal) == 256 and len(px) == W * H, (len(pal), len(px))   # 13.27.0: 256 colours, a byte a pixel, no scanline

lut = [bytes((c >> 16 & 255, c >> 8 & 255, c & 255)) for c in pal]
out = b"".join(lut[b] for b in px)
wallraw.write_header(bytes(out), W, H)
print("wrote drivers/wallpaper.h from arch/arm64/park_wall.h (%d x %d)" % (W, H))
