# The Joshua tree mark, 2.12: one solid silhouette from a few primitives, engraved with a few hairline cuts.
# Four tapered arms (quadratic arcs narrowing to a flared tip), a flared trunk that stands on a short ground
# line, a hub, and a crisp fan of nine tapered blades at each tip. Settlers named the tree for Joshua raising
# his hands to the sky. One ink, no gradients. The hairlines are true holes in the ink, so the mark stays a
# single-colour mask that works on light and dark. Run from the repo root.
#
# 2.12 over 2.0 (one small step, same tree):
#   * the concave corners where the limbs meet the trunk and each other are filleted, so the forks read as
#     grown wood instead of stacked shapes;
#   * every arm flares into its crown, so the knob at each tuft base is gone;
#   * the blades alternate long and short and the two inner crowns lean apart, so the four crowns stay
#     separate down to 32 px;
#   * the trunk stands on a short tapered ground line (the flat cut at the bottom is gone);
#   * thin engraved grooves follow the trunk and each limb, lit from the upper left.
#
# Every copy of the mark is written from here so none can drift:
#   landing/logo.svg        the mark (favicon, landing seal, boot splash source, 3D cap source)
#   landing/mark-tree.svg   the same bytes, the name the landing CSS masks use
#   landing/icon.svg, icon.svg   the mark on a paper tile (the repo-root copy is what README renders)
# The PNG copies (landing/mark.png, landing/og-image.png, docs/brand/) come from tools/gen/gen_brand_art.py.
import math
import os

ink = "#161513"
paper = "#ece8df"
ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")


def smooth(x):
    x = min(max(x, 0.0), 1.0)
    return x * x * (3 - 2 * x)


def quad(p0, c, p1, t):
    return ((1-t)**2*p0[0] + 2*(1-t)*t*c[0] + t*t*p1[0], (1-t)**2*p0[1] + 2*(1-t)*t*c[1] + t*t*p1[1])


def cubic(p0, c1, c2, p1, t):
    w = ((1-t)**3, 3*t*(1-t)**2, 3*t*t*(1-t), t**3)
    return tuple(w[0]*p0[k] + w[1]*c1[k] + w[2]*c2[k] + w[3]*p1[k] for k in (0, 1))


def pts_d(pts):
    return "M" + "L".join(f"{x:.2f} {y:.2f}" for x, y in pts) + "Z"


def inside(pt, poly):
    x, y = pt; c = False
    for i in range(len(poly)):
        (x0, y0), (x1, y1) = poly[i], poly[(i+1) % len(poly)]
        if (y0 > y) != (y1 > y) and x < (x1-x0)*(y-y0)/(y1-y0) + x0:
            c = not c
    return c


def seg_hit(a, b, c, d):
    r = (b[0]-a[0], b[1]-a[1]); s = (d[0]-c[0], d[1]-c[1])
    den = r[0]*s[1] - r[1]*s[0]
    if abs(den) < 1e-9:
        return None
    t = ((c[0]-a[0])*s[1] - (c[1]-a[1])*s[0]) / den
    u = ((c[0]-a[0])*r[1] - (c[1]-a[1])*r[0]) / den
    if 0 <= t <= 1 and 0 <= u <= 1:
        return (a[0] + t*r[0], a[1] + t*r[1])
    return None


def along(poly, i, hit, dist, step):
    """The point `dist` along a polyline from `hit` (on segment i, i+1), heading `step` (+1 or -1)."""
    p, j, left = hit, i + (1 if step > 0 else 0), dist
    while 0 <= j < len(poly):
        q = poly[j]; seg = math.hypot(q[0]-p[0], q[1]-p[1])
        if seg >= left:
            f = left/seg
            return (p[0] + (q[0]-p[0])*f, p[1] + (q[1]-p[1])*f)
        left -= seg; p, j = q, j + step
    return p


def circle_poly(cx, cy, r, n=24):
    return [(cx + r*math.cos(2*math.pi*i/n), cy + r*math.sin(2*math.pi*i/n)) for i in range(n)]


def fillets(edges, bodies, anchor, d):
    """Round each concave corner where two different bodies meet on the outline. The patch is the curve tangent to
    both edges, closed through `anchor` (deep inside the ink) so it overlaps solid ink and leaves no hairline seam."""
    union = lambda pt: any(inside(pt, b) for b in bodies.values())
    out, seen = [], []
    names = list(edges)
    for ia, na in enumerate(names):
        for nb in names[ia+1:]:
            (ba, pa), (bb, pb) = edges[na], edges[nb]
            if ba == bb:
                continue
            for i in range(len(pa)-1):
                for j in range(len(pb)-1):
                    P = seg_hit(pa[i], pa[i+1], pb[j], pb[j+1])
                    if not P or any(math.hypot(P[0]-q[0], P[1]-q[1]) < 1.5 for q in seen):
                        continue
                    ring = [(P[0] + 0.5*math.cos(k*math.pi/6), P[1] + 0.5*math.sin(k*math.pi/6)) for k in range(12)]
                    if all(union(q) for q in ring):
                        continue                          # buried inside the ink, not on the outline
                    # the exposed end of each edge is the one whose next stretch stays on the outline
                    def exposed(poly, k):
                        for step in (1, -1):
                            q = along(poly, k, P, d, step)
                            m = along(poly, k, P, d*0.5, step)
                            # on the outline: nudge off the edge, one side must be outside the union
                            ok = False
                            for sgn in (1, -1):
                                e = (q[0]-P[0], q[1]-P[1]); L = math.hypot(*e) or 1
                                n2 = (-e[1]/L*sgn*0.3, e[0]/L*sgn*0.3)
                                if not union((m[0]+n2[0], m[1]+n2[1])):
                                    ok = True
                            if ok:
                                return q
                        return None
                    A, B = exposed(pa, i), exposed(pb, j)
                    if A is None or B is None:
                        continue
                    seen.append(P)
                    out.append((P, A, B, anchor))
    return out


class Limb:
    """A tapered limb along a quadratic arc. The last 18% opens by `flare` so it runs straight into its crown."""
    def __init__(self, p0, c, p1, w0, w1, flare, n=48):
        pts = [quad(p0, c, p1, i/n) for i in range(n+1)]
        self.cen, self.nor, self.l, self.r = pts, [], [], []
        for i, (x, y) in enumerate(pts):
            a, b = pts[max(i-1, 0)], pts[min(i+1, n)]
            dx, dy = b[0]-a[0], b[1]-a[1]; d = math.hypot(dx, dy)
            t = i/n
            h = (w0 + (w1-w0)*t + flare*smooth((t-0.70)/0.30)) / 2
            nx, ny = -dy/d, dx/d
            self.nor.append((nx, ny))
            self.l.append((x + nx*h, y + ny*h)); self.r.append((x - nx*h, y - ny*h))
        self.poly = self.l + self.r[::-1]

    def at(self, t):
        n = len(self.cen) - 1
        i = min(int(t*n), n-1); f = t*n - i
        p = tuple(self.cen[i][k] + (self.cen[i+1][k]-self.cen[i][k])*f for k in (0, 1))
        return p, self.nor[i]


def groove(at, lo, hi, wmax, off=0.0, sgn=1):
    """A hairline cut: a tapered lens following at(t) -> (point, unit normal) from lo to hi, widest wmax mid-way."""
    n = 20; a, b = [], []
    for i in range(n+1):
        u = i/n
        (x, y), (nx, ny) = at(lo + (hi-lo)*u)
        nx, ny = nx*sgn, ny*sgn
        w = wmax/2 * math.sin(math.pi*u)**0.8
        x += nx*off; y += ny*off
        a.append((x + nx*w, y + ny*w)); b.append((x - nx*w, y - ny*w))
    return pts_d(a + b[::-1][1:-1])


def tuft(x, y, R, tilt, trim):
    """Nine blades on one arc, alternating long and short. trim(angle) shortens a blade that would crowd a neighbouring crown."""
    o = []
    for i in range(9):
        k = i - 4
        ang = tilt - 90 + k*24
        a = math.radians(ang)
        L = R * (1.0 if abs(k) < 3.5 else 0.9) * (1.0 if i % 2 == 0 else 0.9) * trim(ang)
        hw = 2.6 if i % 2 == 0 else 2.3
        px, py = -math.sin(a)*hw, math.cos(a)*hw
        o.append(f'M{x+px:.2f} {y+py:.2f}L{x+math.cos(a)*L:.2f} {y+math.sin(a)*L:.2f}L{x-px:.2f} {y-py:.2f}Z')
    return "".join(o)


# start, control, tip, base width, tip width, flare, crown tilt
ARMS = [((50, 66), (30, 66), (22, 50), 7.4, 4.6, 0.8, -35),
        ((49, 62), (40, 46), (34, 31), 7.4, 4.6, 0.8, -20),
        ((51, 62), (60, 46), (66, 31), 7.4, 4.6, 0.8, 20),
        ((50, 66), (70, 66), (78, 50), 7.4, 4.6, 0.8, 35)]
CROWN_R = 3.0
FILLET = 2.5
TL = ((35.0, 97.8), (43.5, 94.0), (45.0, 78.0), (45.5, 62.0))
TR = ((65.0, 97.8), (56.5, 94.0), (55.0, 78.0), (54.5, 62.0))


def build():
    limbs = [Limb(*a[:6]) for a in ARMS]
    o = ['<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">']
    # the ground line: a short tapered stroke the trunk stands on
    o.append(f'<path d="M20 97.8Q50 95.4 80 97.8Q50 100.2 20 97.8Z" fill="{ink}"/>')

    # trunk, with two bark grooves cut through it (evenodd: a groove is a hole)
    def trunk_at(frac):
        def f(t):
            a, b = cubic(*TL, 1-t), cubic(*TR, 1-t)
            a2, b2 = cubic(*TL, 1-t-0.01), cubic(*TR, 1-t-0.01)
            x, y = a[0] + (b[0]-a[0])*frac, a[1] + (b[1]-a[1])*frac
            x2, y2 = a2[0] + (b2[0]-a2[0])*frac, a2[1] + (b2[1]-a2[1])*frac
            dx, dy = x2-x, y2-y; d = math.hypot(dx, dy)
            return (x, y), (-dy/d, dx/d)
        return f
    cuts = groove(trunk_at(0.33), 0.36, 0.90, 0.75) + groove(trunk_at(0.67), 0.46, 0.86, 0.5)
    o.append(f'<path fill-rule="evenodd" d="M{TL[0][0]} {TL[0][1]}C{TL[1][0]} {TL[1][1]} {TL[2][0]} {TL[2][1]} {TL[3][0]} {TL[3][1]}'
             f'L{TR[3][0]} {TR[3][1]}C{TR[2][0]} {TR[2][1]} {TR[1][0]} {TR[1][1]} {TR[0][0]} {TR[0][1]}Z{cuts}" fill="{ink}"/>')

    tips = [a[2] for a in ARMS]
    for k, (a, limb) in enumerate(zip(ARMS, limbs)):
        p1, tilt = a[2], a[6]
        mid = limb.nor[len(limb.nor)//2]
        sgn = -1 if mid[0] + mid[1] > 0 else 1                 # shift the cut toward the upper-left light
        cut = groove(limb.at, 0.22, 0.64, 0.42, off=0.2, sgn=sgn)
        o.append(f'<path fill-rule="evenodd" d="{pts_d(limb.poly)}{cut}" fill="{ink}"/>')
        others = [t for j, t in enumerate(tips) if j != k]
        def trim(ang, p1=p1, others=others):
            ca, sa = math.cos(math.radians(ang)), math.sin(math.radians(ang))
            f = 1.0
            for q in others:
                dx, dy = q[0]-p1[0], q[1]-p1[1]; dist = math.hypot(dx, dy)
                cosang = (ca*dx + sa*dy)/dist
                if cosang > 0.80 and dist < 40:
                    f = min(f, 0.88)
            return f
        o.append(f'<path d="{tuft(p1[0], p1[1], 14.6, tilt, trim)}" fill="{ink}"/><circle cx="{p1[0]}" cy="{p1[1]}" r="{CROWN_R}" fill="{ink}"/>')
    o.append(f'<circle cx="50" cy="65" r="5.6" fill="{ink}"/>')
    # fillet the concave corners of the outline: trunk to limbs, limb to limb
    tl = [cubic(*TL, i/40) for i in range(41)]; tr = [cubic(*TR, i/40) for i in range(41)]
    bodies = {"trunk": tl + tr[::-1], "hub": circle_poly(50, 65, 5.6)}
    edges = {"trunk.l": ("trunk", tl), "trunk.r": ("trunk", tr)}
    for k, limb in enumerate(limbs):
        bodies[f"arm{k}"] = limb.poly
        edges[f"arm{k}.l"] = (f"arm{k}", limb.l); edges[f"arm{k}.r"] = (f"arm{k}", limb.r)
    patch = "".join(f"M{c[0]} {c[1]}L{A[0]:.2f} {A[1]:.2f}Q{P[0]:.2f} {P[1]:.2f} {B[0]:.2f} {B[1]:.2f}Z"
                    for P, A, B, c in fillets(edges, bodies, (50, 66), FILLET))
    o.append(f'<path d="{patch}" fill="{ink}"/>')
    o.append('</svg>')
    return "".join(o)


def write(path, text):
    with open(os.path.join(ROOT, path), "w") as f:
        f.write(text)


def main():
    svg = build()
    write("landing/logo.svg", svg)
    write("landing/mark-tree.svg", svg)
    body = svg[svg.index(">") + 1:svg.rindex("</svg>")]
    tile = ('<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 200 200" width="200" height="200">\n'
            '  <!-- The 2.12 mark (logo.svg) on paper, one ink on one paper. -->\n'
            f'  <rect width="200" height="200" rx="44" fill="{paper}"/>\n'
            f'  <g transform="translate(30 30) scale(1.4)">{body}</g>\n'
            '</svg>\n')
    write("landing/icon.svg", tile)
    write("icon.svg", tile)


if __name__ == "__main__":
    main()
