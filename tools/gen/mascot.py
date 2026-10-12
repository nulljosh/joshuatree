# The Joshua Tree mascot: the mark itself, drawn as a kid would draw a friend.
#
# Same hand as the logo (tools/gen/logo.py: its shaky lines, looping tufts, blades and crayon grain), on a chubbier
# tree that leans 4 degrees instead of 8, with a belly, shorter arms and a face low on the trunk: two dot eyes, a small
# smile and two terracotta-pink cheeks. Ink on paper, the terracotta ground, the cheeks the one extra colour. Seeded,
# so the file is the same on every run. Writes landing/mascot.svg; tools/gen/gen_brand_art.py rasterizes
# docs/brand/mascot.png from it. Used where the OS wants a face: splash, login, empty states.
import math
import random

import logo as L


def lean(p): return L.tilt(p, deg=4.0, cx=50, cy=86)
TRUNK = lean([(50, 86), (50.3, 80), (50.4, 74), (50.3, 68), (50, 62)])
LEFT  = lean([(45, 65), (37, 62), (30, 56), (27, 47.5)])
MID   = lean([(50, 62), (50.7, 52), (50.2, 41.5)])
RIGHT = lean([(55, 64.5), (63, 62), (69.5, 56.5), (72.5, 48.5)])
ARMS = (LEFT, MID, RIGHT)
CL, CR = lean([(47.4, 61.5)])[0], lean([(52.8, 61.5)])[0]
TUFTS = ((LEFT, 8.0, 5, 3, 3.4), (MID, 9.4, 6, 4, 3.6), (RIGHT, 8.2, 5, 3, 3.2))
def offw(line, ws, side):
    out = []
    for i, (x, y) in enumerate(line):
        a = line[max(i - 1, 0)]; b = line[min(i + 1, len(line) - 1)]
        dx, dy = b[0] - a[0], b[1] - a[1]; d = math.hypot(dx, dy)
        out.append((x - side * dy / d * ws[i], y + side * dx / d * ws[i]))
    return out
BELLY = (11.6, 13.6, 13.4, 12.0, 9.6)
def layers():
    rnd = random.Random(2026)
    ground = L.shaky(rnd, [(18, 88.2), (36, 88.8), (54, 88.6), (70, 87.8), (81, 87.0)], 0.3) + [(82, 86.2)]
    tl, tr = offw(TRUNK, BELLY, -1), offw(TRUNK, BELLY, +1)
    tl = [(tl[0][0] - 0.4, tl[0][1] + 2.6)] + tl; tr = [(tr[0][0] + 0.4, tr[0][1] + 2.2)] + tr
    le, me, re = ((L.offset(a, 3.6, 2.8, -1), L.offset(a, 3.6, 2.8, +1)) for a in ARMS)
    outer_l = L.shaky(rnd, tl[:-1] + le[0][1:], 0.35)
    outer_r = L.shaky(rnd, list(reversed(re[1][1:])) + list(reversed(tr[:-1])), 0.35)
    crotch_l = L.shaky(rnd, list(reversed(le[1][1:])) + [CL] + me[0][1:], 0.35)
    crotch_r = L.shaky(rnd, list(reversed(me[1][1:])) + [CR] + re[0][1:], 0.35)
    knots, blades = [], []
    for arm, r, loops, count, reach in TUFTS:
        cx, cy, up = L.tip(arm, 4.5)
        knots.append(L.scribble(rnd, cx, cy, r, loops, 0.6, (0.8, 0.66)))
        blades += L.flicks(rnd, cx, cy, up, r, count, reach)
    # the face, low on the chubby trunk: two dot eyes, a small smile, crayon cheeks
    ex, ey = lean([(45.6, 72.5)])[0], lean([(54.6, 72.5)])[0]
    eyes = [[(ex[0] - 0.3, ex[1] - 0.4), (ex[0] + 0.2, ex[1] + 0.5)], [(ey[0] - 0.3, ey[1] - 0.4), (ey[0] + 0.2, ey[1] + 0.5)]]
    smile = L.shaky(rnd, lean([(47.4, 77.0), (50.1, 79.0), (52.8, 77.0)]), 0.12, steps=1)
    def cheek(c):
        x, y = lean([c])[0]
        return [(x - 1.6, y + 0.3), (x + 1.4, y - 0.4), (x - 1.2, y + 0.9), (x + 1.6, y + 0.2)]
    cheeks = [cheek((42.2, 76.6)), cheek((58.0, 76.6))]
    return [(2.6, L.accent, [ground]), (3.0, "#e79a78", cheeks), (1.8, None, [outer_l, outer_r, crotch_l, crotch_r]),
            (1.6, None, knots), (2.0, None, blades), (4.0, None, eyes), (1.5, None, [smile])]


def main():
    L.write("landing/mascot.svg", L.svg(L.paths(layers()), "0 0 100 100", grain=True))


if __name__ == "__main__":
    main()
