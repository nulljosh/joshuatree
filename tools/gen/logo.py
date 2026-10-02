# The Joshua tree mark, 2.0 (second pass): one solid silhouette from a few primitives.
# Four tapered arms (quadratic arcs, one weight narrowing into the trunk), a flared trunk,
# a hub, and a crisp fan of nine tapered blades on a shared arc at each tip, hub circle included.
# Settlers named the tree for Joshua raising his hands to the sky. Run from the repo root.
import math
ink = "#161513"
def arm(p0, c, p1, w0, w1, n=24):
    pts = [((1-t)**2*p0[0]+2*(1-t)*t*c[0]+t*t*p1[0], (1-t)**2*p0[1]+2*(1-t)*t*c[1]+t*t*p1[1]) for t in (i/n for i in range(n+1))]
    l, r = [], []
    for i, (x, y) in enumerate(pts):
        a, b = pts[max(i-1, 0)], pts[min(i+1, n)]
        dx, dy = b[0]-a[0], b[1]-a[1]; d = math.hypot(dx, dy)
        h = (w0 + (w1-w0)*i/n) / 2
        l.append((x - dy/d*h, y + dx/d*h)); r.append((x + dy/d*h, y - dx/d*h))
    return "M" + "L".join(f"{x:.2f} {y:.2f}" for x, y in l + r[::-1]) + "Z"
def tuft(x, y, R, tilt):
    o = []
    n = 9
    for i in range(n):
        a = math.radians(tilt - 90 + (i - (n-1)/2) * 24)  # fan of blades on one arc
        L = R * (1.0 if abs(i-(n-1)/2) < 3.5 else 0.9)
        px, py = -math.sin(a)*2.3, math.cos(a)*2.3
        o.append(f'M{x+px:.2f} {y+py:.2f}L{x+math.cos(a)*L:.2f} {y+math.sin(a)*L:.2f}L{x-px:.2f} {y-py:.2f}Z')
    return "".join(o)
def build(name):
    o = ['<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">']
    o.append(f'<path d="M37 96C44 91 45 76 45.5 62L54.5 62C55 76 56 91 63 96Z" fill="{ink}"/>')
    tips = [((50,66),(30,66),(22,50),7,4.2,-35), ((49,62),(40,46),(35,31),7,4.2,-12),
            ((51,62),(60,46),(65,31),7,4.2,12), ((50,66),(70,66),(78,50),7,4.2,35)]
    for p0, c, p1, w0, w1, tilt in tips:
        o.append(f'<path d="{arm(p0, c, p1, w0, w1)}" fill="{ink}"/>')
        o.append(f'<path d="{tuft(p1[0], p1[1], 14, tilt)}" fill="{ink}"/><circle cx="{p1[0]}" cy="{p1[1]}" r="4.6" fill="{ink}"/>')
    o.append(f'<circle cx="50" cy="65" r="5.6" fill="{ink}"/>')
    o.append('</svg>'); open(name, "w").write("".join(o))
build("landing/logo.svg")
