#!/usr/bin/env python3
"""The Pi's default wallpaper (8.9.0, Joshua's ask): Joshua Tree National Park at dusk. Drawn at 480 x 270 in 16 colours, every
blend an ordered (Bayer 4x4) dither between two neighbouring colours, then shown 4x on the Pi's 1080p screen with every fourth
row dimmed, a scanline look. The sun sits low behind the far ridge, so the trees and the granite piles are backlit. The middle
of the bottom is kept quiet for the dock and the line above it.
Writes arch/arm64/park_wall.h (4-bit indexed, two pixels a byte, 64,800 bytes) and the 1920 x 1080 preview the brand kit
shows, docs/brand/park-wall.png (or the path given).
Usage: tools/gen/gen_park_wall.py [preview.png]"""
import math, random, sys
from pathlib import Path

W, H = 480, 270
root = Path(__file__).resolve().parents[2]
PAL = [
    (0x14, 0x1c, 0x3e),  # 0 night, the top of the sky
    (0x22, 0x35, 0x68),  # 1 deep blue
    (0x3b, 0x6e, 0xa5),  # 2 the house blue (8.9.0's flat wallpaper)
    (0x72, 0x9c, 0xc9),  # 3 high sky
    (0xb8, 0xc6, 0xd8),  # 4 pale sky, the stars
    (0xf3, 0xd2, 0xa2),  # 5 peach glow
    (0xec, 0x9e, 0x76),  # 6 horizon pink-orange
    (0xfb, 0xec, 0xc6),  # 7 the sun, the lit rims
    (0x9a, 0x7c, 0xa8),  # 8 far ridge, lavender
    (0x66, 0x4f, 0x7d),  # 9 near ridge, dusk purple
    (0x40, 0x31, 0x55),  # 10 ridge shadow
    (0xc9, 0x93, 0x66),  # 11 sand
    (0x9c, 0x68, 0x47),  # 12 sand in shadow, the granite
    (0x6c, 0x45, 0x34),  # 13 near ground
    (0xb5, 0x50, 0x2c),  # 14 terracotta, the brand accent: the brightest rocks
    (0x26, 0x1a, 0x13),  # 15 bark ink, the trees
]
BAYER = [[0, 8, 2, 10], [12, 4, 14, 6], [3, 11, 1, 9], [15, 7, 13, 5]]
idx = [[0] * W for _ in range(H)]

def ramp(stops, t, x, y):   # t along a list of (position, palette index): the two neighbours, dithered by the Bayer cell
    if t <= stops[0][0]: return stops[0][1]
    for (t0, a), (t1, b) in zip(stops, stops[1:]):
        if t <= t1: return b if (t - t0) / (t1 - t0) * 16 > BAYER[y & 3][x & 3] + 0.5 else a
    return stops[-1][1]

SUN_X, SUN_Y, SUN_R = 318, 150, 17
SKY = [(0.0, 0), (0.16, 1), (0.36, 2), (0.54, 3), (0.68, 4), (0.80, 5), (0.92, 6)]
horizon = 168
for y in range(H):
    for x in range(W):
        t = y / horizon
        d = math.hypot(x - SUN_X, (y - SUN_Y) * 1.6)
        t += max(0, 0.22 - d / 600)   # the sky warms around the sun
        idx[y][x] = ramp(SKY, t, x, y)
        if d < SUN_R: idx[y][x] = 7 if d < SUN_R - 1.5 or BAYER[y & 3][x & 3] > 6 else 5
star = random.Random(3)
for _ in range(70):   # stars in the dark top only, some twinkling as a plus
    x, y = star.randrange(W), star.randrange(int(horizon * 0.30))
    if abs(x - W // 2) < 70 and y < 14: continue
    idx[y][x] = 4
    if star.random() < 0.12:
        for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
            if 0 <= x + dx < W and 0 <= y + dy < H: idx[y + dy][x + dx] = 3

def ridge(base, amp, seed, freq):
    r = random.Random(seed); ph = [r.random() * 6.28 for _ in range(4)]
    return [base - amp * (0.5 * math.sin(x * freq + ph[0]) + 0.3 * math.sin(x * freq * 2.7 + ph[1]) + 0.2 * abs(math.sin(x * freq * 6.1 + ph[2]))) for x in range(W)]
far, near = ridge(158, 22, 11, 0.011), ridge(176, 14, 5, 0.017)
for x in range(W):
    for y in range(int(far[x]), H): idx[y][x] = ramp([(0, 8), (1, 9)], (y - far[x]) / 26, x, y)
    for y in range(int(near[x]), H): idx[y][x] = ramp([(0, 9), (1, 10)], (y - near[x]) / 16, x, y)
    if far[x] < near[x] and abs(x - SUN_X) < 60: idx[int(far[x])][x] = 6   # the far ridge's rim, lit from behind
floor = 188
for y in range(floor, H):   # the desert floor: sand at the horizon, darker toward us
    for x in range(W): idx[y][x] = ramp([(0, 11), (0.45, 12), (1, 13)], (y - floor) / (H - floor), x, y)
rnd = random.Random(21)
for _ in range(260):   # creosote and grass tufts: a dark speck with a lighter top
    x, y = rnd.randrange(W), rnd.randrange(floor + 2, H)
    if abs(x - W // 2) < 150 and y > H - 60: continue   # under the dock stays quiet
    idx[y][x] = 13 if y < floor + 40 else 10
    if y - 1 >= floor: idx[y - 1][x] = 12

def blob(cx, cy, rx, ry, rim_right):   # one granite boulder: shadow body, a lit rim on the sun's side, terracotta warmth
    for y in range(int(cy - ry), int(cy + ry) + 1):
        for x in range(int(cx - rx), int(cx + rx) + 1):
            if not (0 <= x < W and 0 <= y < H): continue
            e = ((x - cx) / rx) ** 2 + ((y - cy) / ry) ** 2
            if e > 1: continue
            lit = (x - cx) / rx * (1 if rim_right else -1) - (y - cy) / ry * 0.6
            idx[y][x] = ramp([(-1.2, 13), (-0.2, 12), (0.55, 14), (0.95, 11)], lit, x, y)
            if e > 0.82 and lit > 0.5: idx[y][x] = 5
def pile(cx, base, size, seed):
    r = random.Random(seed)
    stones = [(cx + r.uniform(-1.6, 1.6) * size, base - size * 0.5, size * r.uniform(1.0, 1.5), size * r.uniform(0.6, 0.8)) for _ in range(3)]
    stones += [(cx + r.uniform(-0.8, 0.8) * size, base - size * 1.25, size * r.uniform(0.7, 1.0), size * r.uniform(0.55, 0.7)) for _ in range(2)]
    stones += [(cx + r.uniform(-0.3, 0.3) * size, base - size * 1.85, size * 0.6, size * 0.45)]
    for s in stones: blob(*s, rim_right=s[0] < SUN_X)
pile(118, 200, 11, 1); pile(214, 192, 6, 2); pile(392, 198, 9, 3); pile(452, 194, 5, 4); pile(30, 196, 7, 5)

def stroke(x1, y1, x2, y2, w, c=15):
    n = int(max(abs(x2 - x1), abs(y2 - y1)) * 2) + 1
    for i in range(n + 1):
        x, y = x1 + (x2 - x1) * i / n, y1 + (y2 - y1) * i / n
        for dy in range(-int(w), int(w) + 1):
            for dx in range(-int(w), int(w) + 1):
                if dx * dx + dy * dy <= w * w + 0.5 and 0 <= int(x + dx) < W and 0 <= int(y + dy) < H: idx[int(y + dy)][int(x + dx)] = c
def rosette(x, y, s, r):   # the spiky leaf ball at every arm's tip: stiff blades in every direction, longest pointing up
    n = 9 if s > 0.8 else 7
    for k in range(n):
        a = -math.pi / 2 + (k - (n - 1) / 2) * (2.2 * math.pi / 2 / (n - 1)) + r.uniform(-0.08, 0.08)   # a fan, up and out, none straight down
        L = (3.2 + 1.8 * max(0, -math.sin(a))) * s * r.uniform(0.85, 1.1)
        stroke(x, y, x + math.cos(a) * L, y + math.sin(a) * L * 1.1, 0)
    stroke(x, y + 1, x, y + 1 + 2.2 * s, max(0.6, 0.8 * s))   # the dead-leaf skirt under it
def arm(x, y, side, L, w, r, s, fork):   # out from the trunk, an elbow, then up: the Joshua tree's crooked arm
    a = math.radians(r.uniform(22, 48)) * side
    ex, ey = x + math.sin(a) * L * 0.55, y - math.cos(a) * L * 0.55
    stroke(x, y, ex, ey, w)
    up = math.radians(r.uniform(-12, 12))
    tx, ty = ex + math.sin(up) * L * 0.6, ey - math.cos(up) * L * 0.6
    stroke(ex, ey, tx, ty, w * 0.85)
    rosette(tx, ty, s, r)
    if fork:   # a second, shorter arm from the elbow
        arm(ex, ey, -side if r.random() < 0.5 else side, L * 0.6, w * 0.75, r, s * 0.9, False)
def tree(x, base, s, seed):
    r = random.Random(seed)
    lean = r.uniform(-0.1, 0.1)
    h = r.uniform(22, 28) * s
    tx, ty = x + math.sin(lean) * h, base - h
    stroke(x, base, tx, ty, 2.0 * s)
    for k in range(int(6 * s)):   # the shaggy trunk: dead leaves hanging off it
        yy = base - r.uniform(2, h / s - 1) * s; xx = x + (tx - x) * (base - yy) / (base - ty)
        side = r.choice((-1, 1))
        stroke(xx + side * 1.6 * s, yy, xx + side * 3.0 * s, yy + 1.8 * s, 0)
    stroke(x - 3 * s, base, x + 3 * s, base, 0.7)
    n = r.choice((3, 4)) if s > 0.8 else 2
    for i in range(n):
        side = (-1, 1)[i % 2]
        at = ty + r.uniform(0, h * 0.25)
        arm(x + (tx - x) * (base - at) / (base - ty), at, side, r.uniform(13, 19) * s, 1.3 * s, r, s, s > 0.8 and r.random() < 0.6)
# far small trees on the horizon, two big ones framing the left and right; none in the dock's middle
for x, b, s, seed in ((178, 190, 0.38, 8), (262, 189, 0.3, 9), (345, 190, 0.42, 10), (430, 192, 0.5, 12), (70, 194, 0.55, 13)):
    tree(x, b, s, seed)
tree(64, 256, 1.6, 1); tree(412, 240, 1.25, 4)

out = root / "arch/arm64/park_wall.h"
packed = bytes(idx[y][x] << 4 | idx[y][x + 1] for y in range(H) for x in range(0, W, 2))
rows = ["/* Generated by tools/gen/gen_park_wall.py, do not edit: the Pi's default wallpaper, Joshua Tree at dusk, %d x %d," % (W, H),
        "   16 colours, two pixels a byte (high nibble first). wall.c draws it scaled, every fourth screen row dimmed. */",
        "#define PARK_W %d" % W, "#define PARK_H %d" % H,
        "static const unsigned park_pal[16] = { %s };" % ", ".join("0x00%02X%02X%02Xu" % c for c in PAL),
        "static const unsigned char park_px[%d] = {" % len(packed)]
for i in range(0, len(packed), 40): rows.append("    " + ",".join(str(b) for b in packed[i:i + 40]) + ",")
rows.append("};")
out.write_text("\n".join(rows) + "\n")

if True:   # the preview, as the Pi shows it at 1080p: 4x, every fourth row at 80 percent
    from PIL import Image
    small = Image.new("RGB", (W, H)); small.putdata([PAL[idx[y][x]] for y in range(H) for x in range(W)])
    big = small.resize((W * 4, H * 4), Image.NEAREST); px = big.load()
    for y in range(3, H * 4, 4):
        for x in range(W * 4): px[x, y] = tuple(v * 4 // 5 for v in px[x, y])
    big.save(sys.argv[1] if len(sys.argv) > 1 else root / "docs/brand/park-wall.png")
print("wrote", out, len(packed), "bytes")
