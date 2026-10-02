# The Joshua tree mark, 2.0: four kinked arms that taper toward the tips, dense engraved
# tufts (tapered spikes, two rings, longest upward), a flared trunk and a ground line.
# Settlers named the tree for Joshua raising his hands to the sky. Run from the repo root.
import math, random
ink = "#161513"
def tuft(x, y, r, rng):
    o = []
    n = 34
    for i in range(n):
        a = math.radians(i * 360 / n - 90 + rng.uniform(-3, 3))
        up = -math.sin(a)
        L = r * (0.8 + 0.4 * max(up, 0)) if up > -0.3 else r * 0.55
        L *= rng.uniform(0.82, 1.12)
        hw = 0.95
        px, py = -math.sin(a) * hw, math.cos(a) * hw
        bx, by = x + math.cos(a) * 2.5, y - up * 2.5
        o.append(f'M{bx+px:.2f} {by-py:.2f}L{x+math.cos(a)*L:.2f} {y-up*L:.2f}L{bx-px:.2f} {by+py:.2f}Z')
    for i in range(17):  # shorter inner ring of tapered spikes fills the gaps
        a = math.radians(i * 360 / 17 - 80)
        up = -math.sin(a); L = r * 0.62 * (1 if up > -0.3 else 0.7)
        px, py = -math.sin(a) * 0.8, math.cos(a) * 0.8
        o.append(f'M{x+px:.2f} {y-py:.2f}L{x+math.cos(a)*L:.2f} {y-up*L:.2f}L{x-px:.2f} {y+py:.2f}Z')
    return o
def build(arms, trunk, name, tuft_r):
    rng = random.Random(20)
    o = ['<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">', f'<path d="{trunk}" fill="{ink}"/>']
    for pts, w in arms:
        n = len(pts) - 1
        for i in range(n):
            (x0, y0), (x1, y1) = pts[i], pts[i + 1]
            sw = w
            o.append(f'<path d="M{x0} {y0}L{x1} {y1}" fill="none" stroke="{ink}" stroke-width="{sw:.2f}" stroke-linecap="round" stroke-linejoin="round"/>')
    for pts, w in arms:
        x, y = pts[-1]
        o.append(f'<path d="{"".join(tuft(x, y, tuft_r, rng))}" fill="{ink}"/>')
        o.append(f'<circle cx="{x}" cy="{y}" r="3.4" fill="{ink}"/>')
    o.append(f'<path d="M20 97.5H80" stroke="{ink}" stroke-width="1.3" stroke-linecap="round"/>')
    o.append('</svg>'); open(name, "w").write("".join(o))
trunk = "M41 96.5C45 90 46 72 47 62L54 62C54 72 55 90 59 96.5Z"
build([([(50,68),(36,64),(30,52),(20,48)],6), ([(49,62),(44,48),(36,40),(34,26)],6), ([(51,60),(56,44),(62,34),(60,20)],6), ([(51,66),(66,62),(74,50),(84,46)],6)], trunk, "landing/logo.svg", 12.5)
