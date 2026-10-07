# The Joshua tree mark, 2.25: a crayon scribble.
#
# Drawn like a quick marker sketch: thin, sure lines for the trunk, the three arms and the ground, and a dense
# knot of looping scribble at the end of every arm for the spiky tufts, with a few short strokes flicking out of
# each knot for the blades. The tree is lopsided like a real one, and the trunk and ground swell where the marker
# pressed harder and overshoot their ends a hair, as if drawn in one quick pass. Black on off-white, no fill, no shading, round caps. Every point is placed by hand
# below and then shaken a little by a seeded hand, so the line wavers like a real pen but the file is the same on
# every run (tools/checks/logo-check.sh rebuilds it byte for byte).
#
# Every copy of the mark is written from here:
#   landing/logo.svg        the mark with a light crayon grain (an SVG displacement filter): landing seal, share
#                           card, README, boot splash
#   landing/mark-tree.svg   the same drawing with no grain, the name the landing CSS masks use
#   landing/mark-bold.svg   no grain, heavier strokes on a tight box, for 16 to 48 px (favicon, menu bar, About)
#   landing/icon.svg, icon.svg   the mark on an off-white square (the repo-root copy is what README renders)
# The PNG copies (landing/mark.png, landing/badge.png, landing/og-image.png, docs/brand/) come from
# tools/gen/gen_brand_art.py, the kernel's copies from tools/gen/gen_boot_mark.py.
import math
import os
import random

ink = "#161513"
paper = "#faf8f4"
ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")

# Centre lines, base to tip. Lopsided on purpose, like a real tree: the trunk leans right a few degrees with a
# small kink where it grew round something, the left arm reaches longer and lower, the right one stays short.
TRUNK = [(48.8, 84.0), (49.6, 74.5), (50.9, 70.5), (51.8, 62.0)]
LEFT = [(49.0, 64.5), (39.0, 62.0), (30.0, 56.5), (23.5, 48.5), (22.0, 41.0)]
MID = [(52.0, 60.0), (53.8, 47.0), (53.0, 34.0)]
RIGHT = [(54.0, 63.0), (63.5, 59.5), (70.0, 53.5), (72.0, 47.5)]
# Each tuft its own size and density: (knot radius, loops, stray loops, loop size); the bold copy only takes the radius.
TUFTS = ((LEFT, 7.4, 3, 3, 1.1), (MID, 9.8, 5, 2, 0.75), (RIGHT, 7.8, 4, 2, 0.95))
SMALL_R = (6.3, 7.8, 6.7)
ARMS = (LEFT, MID, RIGHT)


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


def scribble(rnd, cx, cy, r, loops, amt):
    """A knot of looping scribble: small loops whose centre wanders round inside a circle of radius r."""
    pts = []
    n = loops * 9
    phase = rnd.uniform(0, 6.3)
    for i in range(n + 1):
        t = i / n
        wander = 2 * math.pi * t * 2.3 + phase
        mx = cx + math.cos(wander) * r * 0.5 + math.sin(wander * 2.7) * r * 0.12
        my = cy + math.sin(wander) * r * 0.42 + math.cos(wander * 1.9) * r * 0.1
        loop = 2 * math.pi * i / 9 + phase
        lr = r * rnd.uniform(0.4, 0.95)
        pts.append((mx + math.cos(loop) * lr + rnd.uniform(-amt, amt), my + math.sin(loop) * lr * 0.9 + rnd.uniform(-amt, amt)))
    return pts


def part(stroke, a, b):
    """The stretch of a stroke between fractions a and b of its points: where the marker pressed harder."""
    n = len(stroke) - 1
    return stroke[round(n * a):round(n * b) + 1]


def scale(pts, k, cx=50.0, cy=52.0):
    return [(cx + (x - cx) * k, cy + (y - cy) * k) for x, y in pts]


def drawing(small):
    """(line subpaths, scribble subpaths, (width factor, subpaths) marker presses). small: the tree drawn down
    its middle for 16 to 48 px, one even weight."""
    rnd = random.Random(1932)
    K = 0.82   # the drawing sits in the middle of the box, lots of paper round it
    lines, knots = [], []
    if small:
        lines.append([(30.5, 85.4), (49.0, 84.0), (68.5, 83.6)])
        lines.append(TRUNK)
        for arm in ARMS:
            lines.append([TRUNK[-1]] + arm[1:])
        for arm, r in zip(ARMS, SMALL_R):
            cx, cy, _ = tip(arm, 3.0)
            knots.append(scribble(rnd, cx, cy, r, 3, 0.3))
        return [scale(p, K) for p in lines], [scale(p, K) for p in knots], []
    # the ground, one quick stroke that runs a touch downhill, sags, and flicks up past where it meant to stop
    ground = shaky(rnd, [(26.5, 87.2), (38.0, 87.9), (52.0, 88.1), (66.0, 87.0), (72.6, 86.0)], 0.45) + [(73.8, 85.1)]
    lines.append(ground)
    # the trunk and the arms: two thin edges each, one stroke up the outside of the tree, one for each crotch
    tl, tr = offset(TRUNK, 5.6, 3.9, -1), offset(TRUNK, 5.6, 3.9, +1)
    # each edge starts a hair past the ground, the way a fast pen overshoots
    tl = [(tl[0][0] - 0.5, tl[0][1] + 4.4)] + tl
    tr = [(tr[0][0] + 0.7, tr[0][1] + 3.3)] + tr
    edges = {id(a): (offset(a, 3.3, 2.6, -1), offset(a, 3.3, 2.6, +1)) for a in ARMS}
    outer_l = shaky(rnd, tl[:-1] + edges[id(LEFT)][0][1:], 0.8)
    outer_r = shaky(rnd, list(reversed(edges[id(RIGHT)][1][1:])) + list(reversed(tr[:-1])), 0.8)
    lines.append(outer_l)
    lines.append(shaky(rnd, list(reversed(edges[id(LEFT)][1][1:])) + [(50.0, 59.0)] + edges[id(MID)][0][1:], 0.8))
    lines.append(shaky(rnd, list(reversed(edges[id(MID)][1][1:])) + [(55.0, 60.6)] + edges[id(RIGHT)][0][1:], 0.8))
    lines.append(outer_r)
    # the marker pressed harder down the trunk and through the middle of the ground, so the width swells and thins
    press = [(1.35, [part(outer_l, 0.04, 0.42), part(outer_r, 0.6, 0.97), part(ground, 0.12, 0.62)]),
             (1.7, [part(outer_l, 0.12, 0.28), part(outer_r, 0.74, 0.9), part(ground, 0.25, 0.45)])]
    # the tufts: a mass of overlapping loops worked back and forth at each tip, a ragged edge, and a few loose
    # loops trailing off it, each tuft its own size and density
    for arm, r, loops, strays, size in TUFTS:
        cx, cy, up = tip(arm, 4.5)
        knots.append(scribble(rnd, cx, cy, r, loops, 0.9))
        knots.append(scribble(rnd, cx - 0.8, cy + 0.5, r * 0.72 * size, loops + 1, 0.6))
        knots.append(scribble(rnd, cx + 0.6, cy - 0.4, r * 0.4, max(loops - 1, 1), 0.4))
        for k in range(strays):
            a = up + rnd.uniform(-1.9, 1.9)
            d = r * rnd.uniform(0.95, 1.2)
            knots.append(scribble(rnd, cx + math.cos(a) * d, cy + math.sin(a) * d, r * 0.3 * size, 1, 0.3))
    return ([scale(p, K) for p in lines], [scale(p, K) for p in knots],
            [(w, [scale(p, K) for p in group]) for w, group in press])


def paths(small, line_w, knot_w):
    lines, knots, press = drawing(small)
    a = f'<path d="{"".join(smooth(p) for p in lines)}" stroke-width="{line_w}"/>'
    b = f'<path d="{"".join(smooth(p) for p in knots)}" stroke-width="{knot_w}"/>'
    c = "".join(f'<path d="{"".join(smooth(p) for p in group)}" stroke-width="{f(line_w * w)}"/>' for w, group in press)
    return a + c + b


GRAIN = ('<filter id="crayon" x="-5%" y="-5%" width="110%" height="110%">'
         '<feTurbulence type="fractalNoise" baseFrequency="1.1" numOctaves="2" seed="7"/>'
         '<feDisplacementMap in="SourceGraphic" scale="1.1" xChannelSelector="R" yChannelSelector="G"/></filter>')


def svg(body, box, grain=False, theme=False):
    style = (f'<style>g{{stroke:{ink}}}@media (prefers-color-scheme:dark){{g{{stroke:{paper}}}}}</style>'
             if theme else "")
    defs = f"<defs>{GRAIN}</defs>" if grain else ""
    filt = ' filter="url(#crayon)"' if grain else ""
    return (f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="{box}" width="100" height="100">{style}{defs}'
            f'<g fill="none" stroke="{ink}" stroke-linecap="round" stroke-linejoin="round"{filt}>{body}</g></svg>\n')


def bold_box():
    lines, knots, _ = drawing(True)
    xs = [p[0] for s in lines + knots for p in s]; ys = [p[1] for s in lines + knots for p in s]
    cx, cy = (min(xs) + max(xs)) / 2, (min(ys) + max(ys)) / 2
    side = max(max(xs) - min(xs), max(ys) - min(ys)) + 8
    return f"{f(cx - side / 2)} {f(cy - side / 2)} {f(side)} {f(side)}"


def write(path, text):
    with open(os.path.join(ROOT, path), "w") as fh:
        fh.write(text)


def main():
    big = paths(False, 2.3, 1.7)
    write("landing/logo.svg", svg(big, "0 0 100 100", grain=True))
    write("landing/mark-tree.svg", svg(big, "0 0 100 100"))
    write("landing/mark-bold.svg", svg(paths(True, 5.0, 4.2), bold_box(), theme=True))
    tile = ('<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 200 200" width="200" height="200">\n'
            '  <!-- The 2.25 mark (logo.svg), a crayon scribble on an off-white square. -->\n'
            f'  <defs>{GRAIN}</defs>\n'
            f'  <rect width="200" height="200" rx="44" fill="{paper}"/>\n'
            f'  <g transform="scale(2)" fill="none" stroke="{ink}" stroke-linecap="round" stroke-linejoin="round" '
            f'filter="url(#crayon)">{big}</g>\n'
            '</svg>\n')
    write("landing/icon.svg", tile)
    write("icon.svg", tile)


if __name__ == "__main__":
    main()
