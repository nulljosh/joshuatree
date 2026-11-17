#!/usr/bin/env python3
"""The Pi's default wallpaper (8.9.0, Joshua's ask): Joshua Tree National Park at dusk. Drawn at 480 x 270 in 16 colours, every
blend an ordered (Bayer 4x4) dither between two neighbouring colours, then shown 4x on the Pi's 1080p screen with every fourth
row dimmed, a scanline look. The sun sits low behind the far ridge, so the trees and the granite piles are backlit. The middle
of the bottom is kept quiet for the dock and the line above it.
Since the refresh it is drawn at K = 2, 960 x 540, the same scene at twice the detail, shown 2x.
Writes arch/arm64/park_wall.h (4-bit indexed, two pixels a byte, 259,200 bytes) and the 1920 x 1080 preview the brand kit
shows, docs/brand/park-wall.png (or the path given).
Usage: tools/gen/gen_park_wall.py [preview.png]"""
import math, random, sys
from pathlib import Path

K = 2   # 14.x refresh: the same scene drawn at twice the detail, 960 x 540, so the Pi scales it 2x, not 4x
W, H = 480 * K, 270 * K
root = Path(__file__).resolve().parents[2]
PAL = [
    # 13.9.5, Joshua's art brief: the dusk of a 90s cel, indigo through violet and magenta into hot pink, coral and gold
    (0x1a, 0x14, 0x42),  # 0 indigo, the top of the sky
    (0x3a, 0x2a, 0x78),  # 1 violet
    (0x7b, 0x3c, 0x98),  # 2 magenta violet, the cloud's shadow side
    (0xc9, 0x50, 0x8f),  # 3 hot pink
    (0xff, 0xd9, 0xa8),  # 4 pale peach, the stars and the glints' halo
    (0xf6, 0xb7, 0x5a),  # 5 gold, the lit rims
    (0xef, 0x7d, 0x5e),  # 6 coral
    (0xff, 0xf1, 0xc9),  # 7 cream, the sun and the brightest light
    (0x9b, 0x5f, 0x9e),  # 8 far ridge, mauve
    (0x5e, 0x3a, 0x7c),  # 9 near ridge, dusk purple
    (0x34, 0x22, 0x4f),  # 10 ridge shadow
    (0xc9, 0x93, 0x66),  # 11 sand
    (0x9c, 0x68, 0x47),  # 12 sand in shadow, the granite
    (0x6c, 0x45, 0x34),  # 13 near ground
    (0xb5, 0x50, 0x2c),  # 14 terracotta, the brand accent: the brightest rocks
    (0x26, 0x1a, 0x13),  # 15 bark ink, the trees
]
BAYER = [[0, 8, 2, 10], [12, 4, 14, 6], [3, 11, 1, 9], [15, 7, 13, 5]]
idx = [[0] * W for _ in range(H)]

def ramp(stops, t, x, y, seam=1.0):   # t along (position, palette index) stops: the two neighbours, dithered by the Bayer cell.
    # seam < 1 keeps each colour solid and dithers only the middle `seam` of the step, so bands read crisp with a dithered edge
    if t <= stops[0][0]: return stops[0][1]
    for (t0, a), (t1, b) in zip(stops, stops[1:]):
        if t <= t1:
            u = min(1.0, max(0.0, ((t - t0) / (t1 - t0) - (1 - seam) / 2) / seam))
            return b if u * 16 > BAYER[y & 3][x & 3] + 0.5 else a
    return stops[-1][1]

SUN_X, SUN_Y, SUN_R = 318, 150, 17
SKY = [(0.0, 0), (0.2, 1), (0.4, 2), (0.58, 3), (0.74, 6), (0.88, 5), (1.0, 4)]
horizon = 168
for Y in range(H):
    for X in range(W):
        x, y = X / K, Y / K   # the scene is laid out in 480 x 270 units; every pixel is K x K finer
        t = y / horizon
        d = math.hypot(x - SUN_X, (y - SUN_Y) * 1.6)
        t += max(0, 0.2 - math.hypot((x - SUN_X) * 0.55, (y - SUN_Y) * 2.0) / 420)   # the sky warms around the sun, spread along the horizon
        idx[Y][X] = ramp(SKY, t, X, Y, 0.45)
        if d < SUN_R: idx[Y][X] = 7 if d < SUN_R - 1.5 / K or BAYER[Y & 3][X & 3] > 6 else 5
for Y in range(int((SUN_Y - SUN_R * 2.6) * K), int((SUN_Y + SUN_R) * K)):   # the halo: pale gold dithered thinner outward
    for X in range(int((SUN_X - SUN_R * 4) * K), int((SUN_X + SUN_R * 4) * K)):
        d = math.hypot(X / K - SUN_X, (Y / K - SUN_Y) * 1.6)
        if SUN_R <= d < SUN_R * 2.4:
            f = 1 - (d - SUN_R) / (SUN_R * 1.4)
            if f * 16 > BAYER[Y & 3][X & 3] + 0.5: idx[Y][X] = 7 if f > 0.7 and idx[Y][X] in (5, 7) else 5 if idx[Y][X] in (4, 5, 6) else idx[Y][X]
star = random.Random(3)
for _ in range(70):   # stars in the dark top only, some twinkling as a plus
    x, y = star.randrange(W), star.randrange(int(horizon * 0.30 * K))
    if abs(x - W // 2) < 70 * K and y < 14 * K: continue
    idx[y][x] = 7
    if star.random() < 0.12:
        for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
            if 0 <= x + dx < W and 0 <= y + dy < H: idx[y + dy][x + dx] = 4

def streak(cx, cy, length, core, edge):   # one thin cloud: a solid core line, a dithered underside, tapered dithered ends
    for X in range(max(0, int((cx - length / 2) * K)), min(W, int((cx + length / 2) * K))):   # clipped to the picture
        u = abs(X / K - cx) / (length / 2)   # 0 at the middle, 1 at the tips
        y0 = int((cy + 0.0006 * (X / K - cx) ** 2) * K)   # a slight droop
        for dy in range(K * 2):
            Y = y0 + dy
            if not 0 <= Y < H: continue
            keep = (1 - u) * 16 > BAYER[Y & 3][X & 3] + 0.5 if u > 0.55 else True
            if not keep: continue
            idx[Y][X] = core if dy < K else (edge if BAYER[Y & 3][X & 3] < 8 else idx[Y][X])
for cx, cy, L, core, edge in ((96, 140, 170, 3, 5), (178, 118, 120, 2, 6), (64, 84, 96, 1, 3), (420, 134, 150, 3, 5), (452, 104, 84, 2, 6),
                             (300, 60, 70, 1, 3), (250, 150, 60, 3, 5), (20, 112, 60, 2, 6)):   # banks lit gold from the sun below them
    for k, (ox, oy, f) in enumerate(((0, 0, 1.0), (-0.18, -1.6, 0.55), (0.22, 1.4, 0.6), (0.05, -2.8, 0.3))):   # a bank, not a line
        streak(cx + ox * L, cy + oy, L * f, core if k != 3 else edge, edge)

LIGHTER = {1: 2, 2: 3, 3: 6, 6: 5, 5: 7, 4: 7}
for Y in range(0, int(horizon * K)):   # rays: alternate wedges out of the sun lit one stop, dithered thinner with distance
    for X in range(W):
        x, y = X / K, Y / K
        a = math.atan2(y - SUN_Y, x - SUN_X); d = math.hypot(x - SUN_X, y - SUN_Y)
        if int((a + math.pi) * 26 / math.pi) % 2 or d < SUN_R * 2.4 or d > 260: continue
        f = 1 - d / 260
        if f * 5 > BAYER[Y & 3][X & 3] + 0.5 and idx[Y][X] in LIGHTER: idx[Y][X] = LIGHTER[idx[Y][X]]

def HATCH(X, Y, k):   # a dry pencil's diagonal hatching, k of 4 marks, with a wobble so it reads drawn, not ruled
    return ((X + Y + ((Y >> 3) * 7 + (X >> 4) * 3) % 3) % 4) < k
def cumulus(cx, base, scale, seed):   # a towering cloud of lumps, lit from the sun's side: gold rims, pink body, violet shade
    rr = random.Random(seed); lumps = []
    for layer, n in enumerate((6, 5, 5, 4, 3, 3, 2)):   # a column that billows: wide foot, a cauliflower crown, ragged sides
        for i in range(n):
            w = (n - 1) / 2
            lumps.append((cx + (i - w) * 13 * scale * (1 - layer * 0.05) + rr.uniform(-6, 6) * scale,
                          base - layer * 14 * scale - rr.uniform(0, 7) * scale, (11 + rr.uniform(-1, 7) + (3 if layer >= 4 else 0)) * scale))
        if layer and rr.random() < 0.8:   # a puff bulging out of one side, so the outline is never a straight slope
            side = rr.choice((-1, 1))
            lumps.append((cx + side * (n / 2 + 0.6) * 13 * scale, base - layer * 14 * scale + 4 * scale, 8 * scale))
    sx = 1 if SUN_X > cx else -1
    x0 = int(min(l[0] - l[2] for l in lumps) * K); x1 = int(max(l[0] + l[2] for l in lumps) * K)
    y0 = int(min(l[1] - l[2] for l in lumps) * K); y1 = int(base * K)
    for Y in range(max(0, y0), min(H, y1)):
        for X in range(max(0, x0), min(W, x1)):
            x, y = X / K, Y / K; best = None
            for lx, ly, lr in lumps:   # the lump nearest the viewer here is the one whose surface is highest
                e = ((x - lx) ** 2 + (y - ly) ** 2) / (lr * lr)
                if e < 1 and (best is None or ly - lr * math.sqrt(1 - e) < best[0]): best = (ly - lr * math.sqrt(1 - e), lx, ly, lr, e)
            if not best: continue
            _, lx, ly, lr, e = best
            # one mass, not balls: light comes mostly from where the point sits in the whole cloud (sun side, height), and
            # only a little from its own lump's bulge, which gives the edges their roundness
            ccx = (x0 + x1) / 2 / K; span = (x1 - x0) / 2 / K
            lit = ((x - ccx) * sx / span) * 0.5 + (base - y) / (100 * scale) * 0.45 + ((x - lx) * sx * 0.6 - (y - ly) * 0.55) / lr * 0.2 - 0.1
            # cel shading, as a 90s background painter did it: flat bands of colour, each billow round by its own light,
            # a narrow dithered seam between bands and a hot rim on the sun's side (it was diagonal hatching, which read as facets)
            c = ramp([(-0.55, 1), (-0.2, 2), (0.15, 3), (0.45, 6), (0.72, 5)], lit, X, Y, seam=0.3)
            if lit > 0.55 and e > 0.8: c = 7 if BAYER[Y & 3][X & 3] < 10 else 5   # the rim only at the outline
            idx[Y][X] = c
# no cloud tower: a lumpy tower at this size read as a brain, not a cloud (Joshua, 2026-11-17). Long banks lit from below instead.

def ridge(base, amp, seed, freq):
    r = random.Random(seed); ph = [r.random() * 6.28 for _ in range(4)]
    return [K * (base - amp * (0.5 * math.sin(x * freq + ph[0]) + 0.3 * math.sin(x * freq * 2.7 + ph[1]) + 0.2 * abs(math.sin(x * freq * 6.1 + ph[2])))) for x in (X / K for X in range(W))]
far, near = ridge(158, 22, 11, 0.011), ridge(176, 14, 5, 0.017)
for x in range(W):
    for y in range(int(far[x]), H): idx[y][x] = ramp([(0, 8), (1, 9)], (y - far[x]) / (26 * K), x, y, 0.5)
    for y in range(int(near[x]), H): idx[y][x] = ramp([(0, 9), (1, 10)], (y - near[x]) / (16 * K), x, y, 0.5)
    if far[x] < near[x] and abs(x - SUN_X * K) < 60 * K:
        for k in range(K): idx[int(far[x]) + k][x] = 6   # the far ridge's rim, lit from behind
floor = 188 * K
for y in range(floor, H):   # the desert floor: sand at the horizon, darker toward us
    for x in range(W): idx[y][x] = ramp([(0, 11), (0.45, 12), (1, 13)], (y - floor) / (H - floor), x, y, 0.5)
rnd = random.Random(21)
for _ in range(260):   # creosote and grass tufts: a dark speck with a lighter top
    x, y = rnd.randrange(W // K) * K, rnd.randrange(floor // K + 2, H // K) * K
    if abs(x - W // 2) < 150 * K and y > H - 60 * K: continue   # under the dock stays quiet
    for dx in range(K):
        for dy in range(K): idx[y + dy][x + dx] = 12 if y < floor + 40 * K else 13
        if y - 1 >= floor: idx[y - 1][x + dx] = 11 if y < floor + 40 * K else 12

# the near dune: a dark rise across the bottom, its crest rim-lit gold by the sun behind it. It gives the empty sand a
# foreground and depth, and a calm dark floor under the dock.
for X in range(W):
    x = X / K
    crest = 232 + 5 * math.sin(x / 61 + 0.8) + 3 * math.sin(x / 23 + 2.1) + 1.2 * math.sin(x / 7.3)
    for Y in range(int(crest * K), H):
        d = Y / K - crest
        near = max(0.0, 1 - abs(x - SUN_X) / 260)                        # the crest is lit hardest where the sun is behind it
        if d < 0.5 + 1.2 * near: idx[Y][X] = 5 if near > 0.55 and BAYER[Y & 3][X & 3] < 12 else 6
        elif d < 1.6 + 2.5 * near: idx[Y][X] = 6 if BAYER[Y & 3][X & 3] < 4 + 6 * near else 12   # the glow runs down the slope
        elif d < 9: idx[Y][X] = 12 if BAYER[Y & 3][X & 3] < 16 - (d - 1.6) * 2 else 13             # it turns from the light
        else: idx[Y][X] = 13 if d < 26 or BAYER[Y & 3][X & 3] < 15 - min(14, int(d - 26)) else 15
SHADE = {11: 12, 12: 13}   # the darkest ground takes no shadow: a purple pool there reads as water
def shadow(x, base, length, half):   # the low sun is behind: a shadow pools at the foot and reaches toward the viewer
    cx, cy = (x + (x - SUN_X) * 0.02) * K, (base + length * 0.1) * K   # flattened by the ground's tilt, leaning out from the sun
    rx, ry = half * 1.15 * K, max(1.5, length * 0.22) * K
    for Y in range(int(cy - ry), int(cy + ry) + 1):
        for X in range(int(cx - rx), int(cx + rx) + 1):
            if not (0 <= X < W and floor <= Y < H) or idx[Y][X] not in SHADE: continue
            e = ((X - cx) / rx) ** 2 + ((Y - cy) / ry) ** 2
            if e < 1 and (1 - e) * 22 > BAYER[Y & 3][X & 3] + 0.5: idx[Y][X] = SHADE[idx[Y][X]]   # solid in the middle, dithered rim
for x, b, L, hw in ((118, 200, 40, 16), (214, 192, 22, 8), (392, 198, 34, 13), (452, 194, 18, 7), (30, 196, 24, 10),
                    (178, 190, 14, 1.5), (262, 189, 12, 1.2), (345, 190, 16, 1.6), (430, 192, 18, 1.8), (70, 194, 20, 2), (58, 262, 70, 5), (418, 252, 58, 4)):
    shadow(x, b, L, hw)

def blob(cx, cy, rx, ry, rim_right):   # one granite boulder: shadow body, a lit rim on the sun's side, terracotta warmth
    cx, cy, rx, ry = cx * K, cy * K, rx * K, ry * K
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
    x1, y1, x2, y2, w = x1 * K, y1 * K, x2 * K, y2 * K, w * K
    n = int(max(abs(x2 - x1), abs(y2 - y1)) * 2) + 1
    for i in range(n + 1):
        x, y = x1 + (x2 - x1) * i / n, y1 + (y2 - y1) * i / n
        for dy in range(-int(w), int(w) + 1):
            for dx in range(-int(w), int(w) + 1):
                if dx * dx + dy * dy <= w * w + 0.5 and 0 <= int(x + dx) < W and 0 <= int(y + dy) < H: idx[int(y + dy)][int(x + dx)] = c
def rosette(x, y, s, r):   # the spiky leaf ball at every arm's tip: stiff blades in every direction, longest pointing up
    if s > 0.8:   # a big tree's ball is dense: a dark core the blades grow out of
        for dy in range(-2, 3):
            for dx in range(-2, 3):
                if dx * dx + dy * dy <= 5: stroke(x + dx * 0.45 * s, y + dy * 0.45 * s, x + dx * 0.45 * s, y + dy * 0.45 * s, 0.5 * s)
    n = 17 if s > 0.8 else 7
    for k in range(n):
        a = -math.pi / 2 + (k - (n - 1) / 2) * (2.2 * math.pi / 2 / (n - 1)) + r.uniform(-0.08, 0.08)   # a fan, up and out, none straight down
        L = (3.2 + 1.8 * max(0, -math.sin(a))) * s * r.uniform(0.85, 1.1)
        stroke(x, y, x + math.cos(a) * L, y + math.sin(a) * L * 1.1, 0.35 * s if s > 0.8 else 0)
    stroke(x, y + 1, x, y + 1 + 2.2 * s, max(0.6, 0.8 * s))   # the dead-leaf skirt under it
def arm(x, y, side, L, w, r, s, fork, spread=(22, 48)):   # out from the trunk, an elbow, then up: the Joshua tree's crooked arm
    a = math.radians(r.uniform(*spread)) * side
    ex, ey = x + math.sin(a) * L * 0.55, y - math.cos(a) * L * 0.55
    stroke(x, y, ex, ey, w)
    up = math.radians(r.uniform(-12, 12))
    mx, my = ex + math.sin(up) * L * 0.3, ey - math.cos(up) * L * 0.3
    stroke(ex, ey, mx, my, w * 0.9)
    up += math.radians(r.uniform(8, 22)) * side * (-1 if r.random() < 0.4 else 1)   # crooked: a second bend on the way up
    tx, ty = mx + math.sin(up) * L * 0.32, my - math.cos(up) * L * 0.32
    stroke(mx, my, tx, ty, w * 0.8)
    if s > 0.8:   # the arms are shaggy with old leaves too, hanging off the underside
        for k in range(int(5 * s)):
            t = r.uniform(0.1, 0.9); px, py = ex + (tx - ex) * t, ey + (ty - ey) * t
            stroke(px, py, px + r.uniform(-1.2, 1.2) * s, py + r.uniform(1.4, 2.6) * s, 0)
    rosette(tx, ty, s, r)
    if fork:   # a second, shorter arm from the elbow
        arm(ex, ey, -side if r.random() < 0.5 else side, L * 0.6, w * 0.75, r, s * 0.9, False)
def tree(x, base, s, seed):
    r = random.Random(seed)
    lean = r.uniform(-0.1, 0.1)
    h = r.uniform(22, 28) * s
    tx, ty = x + math.sin(lean) * h, base - h
    stroke(x, base, tx, ty, (1.9 if s > 1.5 else 1.5) * s)   # the framing trees get the thick old trunks of the real ones
    for k in range(int(14 * s)):   # the shaggy trunk: dead leaves hanging off it
        yy = base - r.uniform(2, h / s - 1) * s; xx = x + (tx - x) * (base - yy) / (base - ty)
        side = r.choice((-1, 1))
        stroke(xx + side * 1.6 * s, yy, xx + side * 3.0 * s, yy + 1.8 * s, 0)
    stroke(x - 3 * s, base, x + 3 * s, base, 0.7)
    n = r.choice((4, 5)) if s > 0.8 else 2
    rosette(tx, ty, s * 0.9, r)   # the trunk ends in its own leaf ball
    for i in range(n):
        side = (-1, 1)[i % 2]
        at = ty + (r.uniform(0.04, 0.45) * h if s > 0.8 else r.uniform(0, h * 0.25))
        big = s > 0.8   # the big ones spread wide and low, so every arm reads on its own against the sky
        arm(x + (tx - x) * (base - at) / (base - ty), at, side, r.uniform(13, 19) * s * (1 + 0.35 * big * (1 - i / n)), (1.45 if big else 1.3) * s,
            r, s, big and r.random() < 0.7, (30 + 16 * (1 - i / n), 50 + 10 * (1 - i / n)) if big else (22, 48))
def bush(x, base, s, seed):   # creosote: a low dark dome of twigs, backlit like the trees, its shadow toward the viewer
    r = random.Random(seed)
    for k in range(int(10 * s) + 4):
        a = math.radians(r.uniform(-160, -20)); L = r.uniform(2.5, 5.5) * s
        stroke(x, base, x + math.cos(a) * L, base + math.sin(a) * L * 0.8, 0)
    stroke(x - 3 * s, base, x + 3 * s, base, 0.6)
for x, b, s, seed in ((150, 214, 1.0, 21), (238, 206, 0.7, 22), (318, 210, 0.9, 23), (372, 222, 1.1, 24), (98, 222, 0.8, 25), (280, 224, 0.8, 26), (470, 214, 0.9, 27), (12, 212, 0.8, 28)):
    shadow(x, b, 14 * s, 7 * s); bush(x, b, s * 1.9, seed)
# far small trees on the horizon, two big ones framing the left and right; none in the dock's middle
for x, b, s, seed in ((178, 190, 0.38, 8), (262, 189, 0.3, 9), (345, 190, 0.42, 10), (430, 192, 0.5, 12), (70, 194, 0.55, 13)):
    tree(x, b, s, seed)
tree(58, 262, 2.3, 1); tree(418, 252, 1.8, 4)   # the two framing trees, big enough to read as the park's own

for Y in range(H):   # backlit: a bark pixel with sky on its sun side gets a warm rim
    for X in range(1, W - 1):
        if idx[Y][X] == 15:
            n = idx[Y][X + 1] if X < SUN_X * K else idx[Y][X - 1]
            if n <= 7 and n != 15 and Y < floor: idx[Y][X] = 6 if n in (5, 6, 7) else 9   # only against the sky: the lit dune crest is not light behind the tree
            elif n != 15 and idx[Y][X - (1 if X < SUN_X * K else -1)] == 15 and BAYER[Y & 3][X & 3] < 8: idx[Y][X] = 13   # bark catching light

def glint(x, y, size):   # a four-point sparkle: a cream cross with long arms and a peach halo between them
    X0, Y0 = int(x * K), int(y * K)
    for k in range(-size, size + 1):
        for (X, Y) in ((X0 + k, Y0), (X0, Y0 + k)):
            if 0 <= X < W and 0 <= Y < H: idx[Y][X] = 7 if abs(k) < size * 0.6 else 4
    for dx, dy in ((1, 1), (1, -1), (-1, 1), (-1, -1)):
        if 0 <= X0 + dx < W and 0 <= Y0 + dy < H: idx[Y0 + dy][X0 + dx] = 4
for gx, gy, gs in ((SUN_X + 26, SUN_Y - 22, 9), (SUN_X - 30, SUN_Y - 12, 5), (171, 92, 6), (126, 190, 4), (398, 189, 4), (300, 60, 3), (360, 40, 4)):
    glint(gx, gy, gs)

out = root / "arch/arm64/park_wall.h"
packed = bytes(idx[y][x] << 4 | idx[y][x + 1] for y in range(H) for x in range(0, W, 2))
rows = ["/* Generated by tools/gen/gen_park_wall.py, do not edit: the Pi's default wallpaper, Joshua Tree at dusk, %d x %d," % (W, H),
        "   16 colours, two pixels a byte (high nibble first). wall.c draws it scaled, every other screen row dimmed. */",
        "#define PARK_W %d" % W, "#define PARK_H %d" % H,
        "static const unsigned park_pal[16] = { %s };" % ", ".join("0x00%02X%02X%02Xu" % c for c in PAL),
        "static const unsigned char park_px[%d] = {" % len(packed)]
for i in range(0, len(packed), 160): rows.append("    " + ",".join(str(b) for b in packed[i:i + 160]) + ",")   # 160 a row keeps the 2x picture under the 2000-line ceiling
rows.append("};")
out.write_text("\n".join(rows) + "\n")

if True:   # the preview, as the Pi shows it at 1080p: 4x, every fourth row at 80 percent
    from PIL import Image
    small = Image.new("RGB", (W, H)); small.putdata([PAL[idx[y][x]] for y in range(H) for x in range(W)])
    big = small.resize((1920, 1080), Image.NEAREST); px = big.load()
    for y in range(1, 1080, 2):
        for x in range(1920): px[x, y] = tuple(v * 4 // 5 for v in px[x, y])
    big.save(sys.argv[1] if len(sys.argv) > 1 else root / "docs/brand/park-wall.png")
print("wrote", out, len(packed), "bytes")
