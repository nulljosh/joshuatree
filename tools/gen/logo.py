# The Joshua tree mark, 3.0: the crayon tree, tightened.
#
# The 2.25 marker scribble drawn again with a surer hand: steadier, heavier lines for the trunk and the three arms,
# a trunk that swells where the marker pressed, a dense knot of loops at the end of every arm with paper showing
# through it, a few short blades over the top of each knot, and a light crayon grain. The tree leans 8 degrees like
# a real one. The ground is the one colour: a single terracotta stroke (#b5502c) under the tree. Everything else is
# ink #161513 on paper #faf8f4, no fill, no gradients, no text. Every point is placed by hand below and then shaken
# a little by a seeded hand, so the line wavers like a real pen but the file is the same on every run
# (tools/checks/logo-check.sh rebuilds it byte for byte).
#
# Every copy of the mark is written from here:
#   landing/logo.svg        the mark with a light crayon grain (an SVG displacement filter): landing seal, share
#                           card, README, boot splash
#   landing/mark-tree.svg   the same drawing with no grain, the name the landing CSS masks use
#   landing/mark-bold.svg   no grain, the leaning tree drawn down its middle in heavy strokes on a tight box, with
#                           the same terracotta ground, for 16 to 48 px (favicon, menu bar, About)
#   landing/icon.svg, icon.svg   the mark on an off-white square (the repo-root copy is what README renders)
# The ground is painted first and the tree over it, so the trunk's feet stand on the terracotta. The ground is its
# own path with its own stroke colour, which is how the copies downstream find it: the boot splash draws it last.
# The PNG copies (landing/mark.png, landing/badge.png, landing/og-image.png, docs/brand/) come from
# tools/gen/gen_brand_art.py, the kernel's copies from tools/gen/gen_boot_mark.py.
import math
import os
import random

ink = "#161513"
paper = "#faf8f4"
accent = "#b5502c"
ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")


def tilt(pts, deg=8.0, cx=48.8, cy=84.0):
    """Lean points over to the right about the trunk's foot."""
    c, s = math.cos(math.radians(deg)), math.sin(math.radians(deg))
    return [(cx + (x - cx) * c - (y - cy) * s, cy + (x - cx) * s + (y - cy) * c) for x, y in pts]


# Centre lines, base to tip, placed upright and then leaned 8 degrees. Lopsided on purpose, like a real tree: the
# trunk has a small kink where it grew round something, the left arm reaches longer and lower, the right one stays
# short and forks off the trunk a little higher than it would if it were tidy.
TRUNK = tilt([(48.8, 84.0), (49.6, 74.5), (50.9, 70.5), (51.8, 62.0)])
LEFT = tilt([(49.0, 64.5), (39.0, 62.0), (30.0, 56.5), (23.5, 48.5), (22.0, 41.0)])
MID = tilt([(52.0, 60.0), (53.8, 47.0), (53.0, 34.0)])
RIGHT = tilt([(54.0, 59.5), (63.5, 59.5), (70.0, 53.5), (72.0, 47.5)])
# 4.0, Many windows: a fourth, smaller tuft, forking up off the long left arm, so the tree has four windows open
# instead of three. (It first hung off the foot of the trunk, which at avatar size read as something else
# entirely.) Same hand, same ground.
LOW = tilt([(37.5, 61.0), (36.0, 56.5), (34.5, 52.5), (33.5, 48.0)])
ARMS = (LEFT, MID, RIGHT, LOW)
CROTCH_L, CROTCH_R = tilt([(50.0, 59.0)])[0], tilt([(55.0, 57.0)])[0]
# Each tuft its own size: (arm, knot radius, loops, blades, blade reach). The bold copy takes SMALL_R.
TUFTS = ((LEFT, 7.2, 5, 3, 3.6), (MID, 9.4, 6, 4, 3.4), (RIGHT, 8.0, 5, 3, 3.2), (LOW, 5.4, 4, 3, 2.6))
SMALL_R = (6.6, 8.2, 7.0, 4.8)
# The bold copy's ground, flat so it lands on one pixel row at 16 px (bold_box puts it there).
SMALL_GROUND_Y = 87.8


def f(v):
    return f"{v:.2f}".rstrip("0").rstrip(".")


def smooth(pts):
    """Catmull-Rom through the points, as one SVG subpath."""
    d = [f"M{f(pts[0][0])} {f(pts[0][1])}"]
    for i in range(1, len(pts)):
        p0 = pts[max(i - 2, 0)]; p1 = pts[i - 1]; p2 = pts[i]; p3 = pts[min(i + 1, len(pts) - 1)]
        c1 = (p1[0] + (p2[0] - p0[0]) / 6, p1[1] + (p2[1] - p0[1]) / 6)
        c2 = (p2[0] - (p3[0] - p1[0]) / 6, p2[1] - (p3[1] - p1[1]) / 6)
        d.append(f"C{f(c1[0])} {f(c1[1])} {f(c2[0])} {f(c2[1])} {f(p2[0])} {f(p2[1])}")
    return "".join(d)


def shaky(rnd, pts, amt, steps=2):
    """Put points between the given ones and nudge every one, so a long line drifts like a hand-held pen."""
    out = []
    for a, b in zip(pts, pts[1:]):
        for k in range(steps):
            t = k / steps
            out.append((a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t))
    out.append(pts[-1])
    return [(x + rnd.uniform(-amt, amt), y + rnd.uniform(-amt, amt)) for x, y in out]


def offset(line, w0, w1, side):
    """One edge of a limb: side -1 is the left of the direction of travel, +1 the right."""
    out = []
    n = len(line)
    for i, (x, y) in enumerate(line):
        a = line[max(i - 1, 0)]; b = line[min(i + 1, n - 1)]
        dx, dy = b[0] - a[0], b[1] - a[1]; d = math.hypot(dx, dy)
        w = w0 + (w1 - w0) * i / (n - 1)
        out.append((x - side * dy / d * w, y + side * dx / d * w))
    return out


def tip(arm, push):
    (ax, ay), (ex, ey) = arm[-2], arm[-1]
    a = math.atan2(ey - ay, ex - ax)
    return ex + math.cos(a) * push, ey + math.sin(a) * push, a


def scribble(rnd, cx, cy, r, loops, amt, orbit):
    """A knot of looping scribble: small loops whose centre wanders round inside a circle of radius r.
    orbit: how far out the loops ride, so the weight sits at the rim and paper shows in the middle."""
    pts = []
    n = loops * 9
    phase = rnd.uniform(0, 6.3)
    for i in range(n + 1):
        t = i / n
        wander = 2 * math.pi * t * 2.3 + phase
        mx = cx + math.cos(wander) * r * orbit[0] + math.sin(wander * 2.7) * r * 0.12
        my = cy + math.sin(wander) * r * orbit[1] + math.cos(wander * 1.9) * r * 0.1
        loop = 2 * math.pi * i / 9 + phase
        lr = r * rnd.uniform(0.33, 0.5)
        pts.append((mx + math.cos(loop) * lr + rnd.uniform(-amt, amt), my + math.sin(loop) * lr * 0.9 + rnd.uniform(-amt, amt)))
    return pts


def flicks(rnd, cx, cy, up, r, count, reach, spread=120):
    """A few short blades over the top of the knot only, the yucca's leaves."""
    out = []
    for k in range(count):
        a = up + math.radians(-spread / 2 + spread * (k + rnd.uniform(-0.2, 0.2)) / (count - 1))
        r0 = r * 0.8; r1 = r + reach * rnd.uniform(0.7, 1.0)
        out.append([(cx + math.cos(a) * r0, cy + math.sin(a) * r0),
                    (cx + math.cos(a + 0.05) * r1, cy + math.sin(a + 0.05) * r1)])
    return out


def part(stroke, a, b):
    """The stretch of a stroke between fractions a and b of its points: where the marker pressed harder."""
    n = len(stroke) - 1
    return stroke[round(n * a):round(n * b) + 1]


def big():
    """The full mark as (stroke width, colour or None for ink, subpaths) in paint order, ground first."""
    rnd = random.Random(300)
    # the ground, one quick stroke that sags a touch and flicks up past where it meant to stop
    ground = shaky(rnd, [(17, 87.6), (34, 88.2), (52, 88.0), (68, 87.0), (79.5, 86.0)], 0.3) + [(80.6, 85.2)]
    # the trunk and the arms: two edges each, one stroke up each outside of the tree and one for each crotch.
    # Each trunk edge starts a hair below the ground, the way a fast pen overshoots.
    tl, tr = offset(TRUNK, 5.4, 3.6, -1), offset(TRUNK, 5.4, 3.6, +1)
    tl = [(tl[0][0] - 0.3, tl[0][1] + 3.6)] + tl
    tr = [(tr[0][0] + 0.4, tr[0][1] + 3.0)] + tr
    le, me, re = ((offset(a, 3.2, 2.4, -1), offset(a, 3.2, 2.4, +1)) for a in (LEFT, MID, RIGHT))
    own = random.Random(40)   # its own pen, so the first three tufts stay exactly as 3.0 drew them
    lo = shaky(own, offset(LOW, 2.6, 1.8, -1), 0.3), shaky(own, offset(LOW, 2.6, 1.8, +1), 0.3)
    outer_l = shaky(rnd, tl[:-1] + le[0][1:], 0.4)
    outer_r = shaky(rnd, list(reversed(re[1][1:])) + list(reversed(tr[:-1])), 0.4)
    crotch_l = shaky(rnd, list(reversed(le[1][1:])) + [CROTCH_L] + me[0][1:], 0.4)
    crotch_r = shaky(rnd, list(reversed(me[1][1:])) + [CROTCH_R] + re[0][1:], 0.4)
    # the marker pressed harder down the trunk, so the line swells and thins: two heavier passes over its foot
    press = [part(outer_l, 0.02, 0.42), part(outer_r, 0.6, 0.97)]
    press2 = [part(outer_l, 0.04, 0.22), part(outer_r, 0.8, 0.97)]
    # the tufts: a mass of overlapping loops worked round each tip, with paper showing through, and the blades
    knots, blades = [], []
    for arm, r, loops, count, reach in TUFTS:
        cx, cy, up = tip(arm, 4.5)
        if arm is LOW: rnd = own
        knots.append(scribble(rnd, cx, cy, r, loops, 0.6, (0.8, 0.66)))
        blades += flicks(rnd, cx, cy, up, r, count, reach)
    return [(2.6, accent, [ground]), (1.8, None, [outer_l, outer_r, crotch_l, crotch_r, lo[0], lo[1]]), (2.7, None, press),
            (3.5, None, press2), (1.6, None, knots), (2.0, None, blades)]


def small():
    """The tree drawn down its middle for 16 to 48 px: one heavy line per limb, a tight knot per tuft, and the ground."""
    rnd = random.Random(1932)
    lines = [TRUNK] + [[TRUNK[-1]] + a[1:] for a in ARMS[:3]] + [LOW]   # the fourth arm leaves the long left arm
    knots = []
    for arm, r in zip(ARMS, SMALL_R):
        cx, cy, _ = tip(arm, 3.0)
        knots.append(scribble(rnd, cx, cy, r, 3, 0.2, (0.6, 0.5)))
    ground = [(26, SMALL_GROUND_Y), (50, SMALL_GROUND_Y), (75, SMALL_GROUND_Y)]
    return [(6.0, accent, [ground]), (6.0, None, lines), (5.2, None, knots)]


def paths(layers):
    out = []
    for w, colour, subs in layers:
        stroke = f' stroke="{colour}"' if colour else ""
        out.append(f'<path d="{"".join(smooth(p) for p in subs)}"{stroke} stroke-width="{f(w)}"/>')
    return "".join(out)


GRAIN = ('<filter id="crayon" x="-5%" y="-5%" width="110%" height="110%">'
         '<feTurbulence type="fractalNoise" baseFrequency="1.1" numOctaves="2" seed="7"/>'
         '<feDisplacementMap in="SourceGraphic" scale="1.2" xChannelSelector="R" yChannelSelector="G"/></filter>')


def svg(body, box, grain=False, theme=False):
    """theme: the ink turns to paper on a dark page (the favicon); the terracotta ground keeps its own colour."""
    style = (f'<style>g{{stroke:{ink}}}@media (prefers-color-scheme:dark){{g{{stroke:{paper}}}}}</style>'
             if theme else "")
    defs = f"<defs>{GRAIN}</defs>" if grain else ""
    filt = ' filter="url(#crayon)"' if grain else ""
    return (f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="{box}" width="100" height="100">{style}{defs}'
            f'<g fill="none" stroke="{ink}" stroke-linecap="round" stroke-linejoin="round"{filt}>{body}</g></svg>\n')


def bold_box():
    """A tight square round the bold copy: a little paper over the top knot, centred side to side, and deep enough
    below that the ground's centre line falls on the middle of the second row up at 16 px, so the ground is one
    solid terracotta row there instead of two half-tone ones."""
    pts = [p for _, _, subs in small() for s in subs for p in s]
    xs = [p[0] for p in pts]
    top = min(p[1] for p in pts) - 3.2
    side = (SMALL_GROUND_Y - top) / (1 - 1.5 / 16)
    cx = (min(xs) + max(xs)) / 2
    return f"{f(cx - side / 2)} {f(top)} {f(side)} {f(side)}"


def write(path, text):
    with open(os.path.join(ROOT, path), "w") as fh:
        fh.write(text)


def main():
    mark = paths(big())
    write("landing/logo.svg", svg(mark, "0 0 100 100", grain=True))
    write("landing/mark-tree.svg", svg(mark, "0 0 100 100"))
    write("landing/mark-bold.svg", svg(paths(small()), bold_box(), theme=True))
    tile = ('<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 200 200" width="200" height="200">\n'
            '  <!-- The 3.0 mark (logo.svg), the crayon tree on a terracotta ground, on an off-white square. -->\n'
            f'  <defs>{GRAIN}</defs>\n'
            f'  <rect width="200" height="200" rx="44" fill="{paper}"/>\n'
            f'  <g transform="scale(2)" fill="none" stroke="{ink}" stroke-linecap="round" stroke-linejoin="round" '
            f'filter="url(#crayon)">{mark}</g>\n'
            '</svg>\n')
    write("landing/icon.svg", tile)
    write("icon.svg", tile)


if __name__ == "__main__":
    main()
