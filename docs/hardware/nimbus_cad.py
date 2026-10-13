# SPDX-License-Identifier: CC-BY-NC-SA-4.0
# Copyright (c) 2026 Joshua Trommel. Hardware design, see docs/hardware/LICENSE-NOTICE.md
# Nimbus: a small square computer. A Pi 4 and the official 7 inch touch display in one printed body whose front is the Joshua Tree
# icon tile stood up (superellipse, SQUIRCLE_N 4.8 from docs/DESIGN.md), leaning back 6 degrees. Two printed shells, a front
# and a back cover, one 0.5 mm reveal between them, screws from underneath, and two inlays (the mark's terracotta ground, the ink slot).
#   uv run --with build123d --with shapely python docs/hardware/nimbus_cad.py docs/hardware
# Writes nimbus/<part>.stl (print orientation, coarse) and nimbus/<part>.step (assembled position; one STEP per part keeps each under 5 MB). With NIMBUS_MESH=dir it also writes fine meshes of
# every part in assembled position (Z up, table at z 0) for render_nimbus.py.
# mm throughout. Face frame: X right, Y up, Z out of the face (face at z = 0, back at z = -D). Assembled frame: Z up, front looks to -Y.
# Stand-in numbers (verify with calipers before printing): the official Raspberry Pi 7 inch touch display outline 194 x 110, active
# area 154.08 x 86.0, about 6.4 thick with a driver board behind; its holes are not used, pegs from the cover hold it instead.
import sys, os, math, re, json
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from build123d import *
import pi4b

OUT = sys.argv[1] if len(sys.argv) > 1 else "."
STL_DIR = os.path.join(OUT, "nimbus")
MESH = os.environ.get("NIMBUS_MESH")

# ---- the squircle (docs/DESIGN.md) and the body ----
SQUIRCLE_N = 4.8
A, B = 120.0, 105.0              # half width and half height of the front outline: 240 x 210
R = 9.0                          # the front radius: the roll-over and every opening corner
R2 = 16.0                        # the back rolls over softer, so the body reads light from behind
D = 98.0                         # depth, front face to back
T = 2.4                          # wall
LEAN = 6.0                       # degrees the face leans back
CUT = 3.5                        # the flat rear foot is cut this far above the lowest point; the chin hovers over the desk
GAP = 0.5                        # the one reveal
FIT = 0.2
BED = 256.0
D_G = R2 + 8.0                   # parting line, measured from the back
ZG = -D + D_G                    # z of the front shell's rear edge (face frame)
# ---- the display (stand-in numbers) ----
GLASS_W, GLASS_H, GLASS_T = 194.0, 110.0, 1.6     # the front glass sheet; the module (LCD, board) hangs behind it
ACT_W, ACT_H = 154.08, 86.0
OPEN_W, OPEN_H = GLASS_W + 2 * GAP, GLASS_H + 2 * GAP   # the glass sits flush in the face with the 0.5 mm reveal all round
LIP = 0.2                        # the glass front sits this far behind the face
SHELF_Z0, SHELF_Z1, SHELF_IN = -3.0, -1.8, 2.0   # a 2 mm ledge behind the glass edge that the sheet is glued to
MODULE_T = 4.8
YC = 27.0                        # glass centre height on the face
# ---- the Pi (numbers from pi4b.py) ----
PI_X = 26.0                      # the board sits right of centre so the port strip is centred on the back
ZB = 24.0                        # underside of the board above the desk
USB_OVER = 2.0                   # the USB block stands this far past the board edge
# ---- front details ----
MARK_W, MARK_D, MARK_X, MARK_Y = 22.0, 0.5, 0.0, -66.0
MIN_FEATURE = 0.5
STRIP_D = 1.0                    # depth of the back port strip

def sq_pts(a, b, k=56):
    out = []
    for i in range(k):
        t = 2 * math.pi * i / k; c, s = math.cos(t), math.sin(t)
        r = (abs(c) ** SQUIRCLE_N / a ** SQUIRCLE_N + abs(s) ** SQUIRCLE_N / b ** SQUIRCLE_N) ** (-1 / SQUIRCLE_N)
        out.append((r * c, r * s))
    return out
def sq_wire(inset, z): return Wire([Edge.make_spline([Vector(x, y, z) for x, y in sq_pts(A - inset, B - inset)], periodic=True)])
def in_sq(x, y, inset): return (abs(x) / (A - inset)) ** SQUIRCLE_N + (abs(y) / (B - inset)) ** SQUIRCLE_N <= 1.0
def prism(inset, z0, z1): return Solid.extrude(Face(sq_wire(inset, z0)), Vector(0, 0, z1 - z0))
THS = [math.radians(i * 22.5) for i in range(5)]
def loft(pf, pr):
    """pf(theta) and pr(theta) -> (inset, z) along the front and back roll-over arcs; the back one is mirrored to the back face."""
    front = [pf(t) for t in THS]; rear = [(i, -D - z) for i, z in reversed([pr(t) for t in THS])]
    z0, z1 = front[-1][1], rear[0][1]
    mid = [(front[-1][0], z0 + (z1 - z0) * k / 3) for k in (1, 2)]
    return Solid.make_loft([sq_wire(i, z) for i, z in front + mid + rear], False)
RI, RI2 = R - T, R2 - T
outer = loft(lambda t: (R - R * math.sin(t), -R + R * math.cos(t)), lambda t: (R2 - R2 * math.sin(t), -R2 + R2 * math.cos(t)))
cavity = loft(lambda t: (R - RI * math.sin(t), -R + RI * math.cos(t)), lambda t: (R2 - RI2 * math.sin(t), -R2 + RI2 * math.cos(t)))

# ---- assembled frame: lean, then Z up, then the foot on the desk ----
RAW = Rot(90, 0, 0) * Rot(-LEAN, 0, 0)
ZOFF = -outer.moved(RAW).bounding_box().min.Z
ASM = Pos(0, 0, ZOFF) * RAW
INV = ASM.inverse()
def asm(shape): return ASM * shape
def face_of(shape): return INV * shape
def below(z, size=900.0): return face_of(Pos(0, 0, z - size / 2) * Box(size, size, size))   # everything under height z, as a face-frame tool
def above(z, size=900.0): return face_of(Pos(0, 0, z + size / 2) * Box(size, size, size))
def ptf(x, y, z):
    p = (INV * Pos(x, y, z)).position; return p.X, p.Y, p.Z
def pta(x, y, z):
    p = (ASM * Pos(x, y, z)).position; return p.X, p.Y, p.Z

# ---- the handle scoop: a shallow ellipsoid dish across the top, rim angle about 23 degrees, wall kept at 2.4 ----
SC = (80.0, 40.0, 26.0)
SC_DEPTH = 17.0
SC_C = (0, B - SC_DEPTH + 40.0, -45.0)
def ellipsoid(grow): return Pos(*SC_C) * scale(Sphere(1), by=(SC[0] + grow, SC[1] + grow, SC[2] + grow))
outer = outer - ellipsoid(0)
try:
    rim = [e for e in outer.edges() if e.bounding_box().center().Y > B - 14 and abs(e.bounding_box().center().Z - SC_C[2]) < 26 and e.bounding_box().size.X > 40]
    outer = fillet(rim, 6.0) if rim and not os.environ.get('NIMBUS_NOFILLET') else outer
    print('outer valid', outer.is_valid)
    print("scoop rim edges filleted:", len(rim))
except Exception as ex:
    print("scoop fillet skipped:", ex)
cavity = cavity - ellipsoid(T)
# the desk foot: flat pad, the cavity floor stays 2.4 above it
outer_c = outer - face_of(Pos(0, 0, CUT - 450) * Box(900, 900, 900))
cavity_c = cavity - face_of(Pos(0, 0, CUT + T - 450) * Box(900, 900, 900))
shell = outer_c - cavity_c

# ---- the mark, from landing/mark-bold.svg (the shape logo.py writes for small sizes) ----
def mark_geoms():
    from shapely.geometry import LineString
    from shapely.ops import unary_union
    from shapely import affinity
    svg = open(os.path.join(HERE, "..", "..", "landing", "mark-bold.svg")).read()
    tree, ground = [], []
    for m in re.finditer(r'<path d="([^"]+)"([^>]*)/>', svg):
        d, attrs = m.groups(); sw = float(re.search(r'stroke-width="([\d.]+)"', attrs).group(1))
        is_ground = 'stroke="#b5502c"' in attrs
        for sub in re.findall(r"M[^M]+", d):
            toks = re.findall(r"[MC]|-?\d+\.?\d*", sub); pts = []; i = 0
            while i < len(toks):
                if toks[i] == "M": pts.append((float(toks[i + 1]), float(toks[i + 2]))); i += 3
                else:
                    p0 = pts[-1]; c = [(float(toks[i + 1 + 2 * j]), float(toks[i + 2 + 2 * j])) for j in range(3)]
                    for k in range(1, 13):
                        u = k / 12; w = [(1 - u) ** 3, 3 * u * (1 - u) ** 2, 3 * u * u * (1 - u), u ** 3]
                        pts.append(tuple(sum(w[j] * q[ax] for j, q in enumerate([p0] + c)) for ax in (0, 1)))
                    i += 7
            (ground if is_ground else tree).append(LineString(pts).buffer(sw * (0.36 if is_ground else 0.5), 12))
    t, g = unary_union(tree), unary_union(ground); u = unary_union([t, g]); x0, y0, x1, y1 = u.bounds; k = MARK_W / (x1 - x0)
    place = lambda s: affinity.translate(affinity.scale(affinity.translate(s, -(x0 + x1) / 2, -(y0 + y1) / 2), k, -k, origin=(0, 0)), MARK_X, MARK_Y)
    r = MIN_FEATURE / 2
    t, g = place(t), place(g)
    from shapely.geometry import Polygon, MultiPolygon
    solid = lambda geom: unary_union([Polygon(p.exterior) for p in (geom.geoms if hasattr(geom, "geoms") else [geom])])   # fill the loops: the solid mark
    t = solid(t).buffer(-r, join_style=1).buffer(r, join_style=1).simplify(0.01)
    return t, g.simplify(0.01)
def poly_solid(geom, z0, depth):
    out = None
    for p in (geom.geoms if hasattr(geom, "geoms") else [geom]):
        w = lambda ring: Wire.make_polygon([Vector(x, y, z0) for x, y in list(ring.coords)[:-1]], close=True)
        s = Solid.extrude(Face(w(p.exterior), [w(h) for h in p.interiors]), Vector(0, 0, depth))
        out = s if out is None else out + s
    return out
tree_g, ground_g = mark_geoms()
tree_cut = poly_solid(tree_g, -MARK_D, MARK_D + 1)
ground_cut = poly_solid(ground_g, -MARK_D, MARK_D + 1)
ground_inlay = poly_solid(ground_g, -MARK_D, MARK_D)

# ---- the face: opening, glass pocket, one slot, the mark ----
def rrect(w, h, r, z0, z1): return Pos(0, 0, z0) * extrude(RectangleRounded(w, h, r), z1 - z0)
shell = shell - Pos(0, YC, 0) * rrect(OPEN_W, OPEN_H, R, -T - 0.5, 2.0)
shelf = Pos(0, YC, 0) * (rrect(OPEN_W + 4.0, OPEN_H + 4.0, R + 2, SHELF_Z0, SHELF_Z1) - rrect(GLASS_W - 2 * SHELF_IN, GLASS_H - 2 * SHELF_IN, R - 2, SHELF_Z0 - 1, SHELF_Z1 + 1))
shell = shell + shelf
shell = shell - tree_cut - ground_cut

# ---- split: front shell and back cover, half-lap with a 0.5 reveal ----
front = shell - Pos(0, 0, ZG - 200) * Box(900, 900, 400)
front = front - (prism(1.1, ZG, ZG + 8.0) - prism(T, ZG, ZG + 8.0))
cover = shell - Pos(0, 0, ZG - GAP + 200) * Box(900, 900, 400)
spigot = (prism(1.3, ZG - GAP, ZG + 7.8) - prism(T, ZG - GAP, ZG + 7.8)) - face_of(Pos(0, 0, CUT - 450) * Box(900, 900, 900))
cover = cover + spigot

# ---- the Pi, standing on its floor with the USB end at the back ----
V1 = pta(0, 0, -D); V2 = pta(0, 1, -D)
def y_back(zc): return V1[1] + (V2[1] - V1[1]) / (V2[2] - V1[2]) * (zc - V1[2])    # the outer back plane at height zc
ZM = ZB + 1.4 + 8.0
Y_EDGE = y_back(ZM) - 1.0 - USB_OVER                   # the board's rear edge; the USB block tip sits 1 mm inside the back face
Y_PC = Y_EDGE - pi4b.BOARD_L / 2
HOLES = [(PI_X + hy, Y_PC + hx) for hx, hy in pi4b.HOLES]    # board x runs to the back, board y runs across
def d_of(y, z=ZB): return (y_back(z) - y)             # depth from the back face, level with the board
def standoff(x, y, z0, ztop, apex):
    r = 3.2; sd = 2 ** 0.5
    tri = Face(Wire.make_polygon([Vector(x - r / sd, y + apex * r / sd, 0), Vector(x, y + apex * r * sd, 0), Vector(x + r / sd, y + apex * r / sd, 0)], close=True))
    body = Solid.extrude(tri, Vector(0, 0, ztop - z0)) + Pos(x, y, 0) * Cylinder(r, ztop - z0, align=(Align.CENTER, Align.CENTER, Align.MIN))
    body = Pos(0, 0, z0) * body
    return body
pilot = lambda x, y, ztop: Pos(x, y, ztop - 9) * Cylinder(1.05, 9.5, align=(Align.CENTER, Align.CENTER, Align.MIN))
for (hx, hy) in HOLES:
    d = d_of(hy)
    if d < D_G - 1.5:   # in the cover, printed back down: the teardrop point faces the front, -Y
        so = standoff(hx, hy, CUT + 1.0, ZB, -1)
        cover = cover + face_of(so) - face_of(pilot(hx, hy, ZB))
    else:               # in the front shell, printed face down: the point faces the back, +Y
        so = standoff(hx, hy, ZB - 7.5, ZB, +1)
        front = front + face_of(so) - face_of(pilot(hx, hy, ZB))
    assert ZB - 6.0 >= 2.0

# ---- two ribs and two screws from underneath join the shells ----
RIB_X, RIB_W, RIB_H = 62.0, 9.0, 12.0
SCREWS = []
Y_FLOOR = lambda dd: y_back(CUT + T + 1) - dd
def rib_block(d0, d1, zb):
    y0, y1 = Y_FLOOR(d1), Y_FLOOR(d0)
    return Pos(0, (y0 + y1) / 2, zb + RIB_H / 2) * Box(RIB_W, y1 - y0, RIB_H)
for sx in (-1, 1):
    rib = Pos(sx * RIB_X, 0, 0) * (rib_block(D_G + 1.5, 36.0, CUT + T - 0.2) + rib_block(8.0, D_G + 3.0, CUT + T + FIT))
    front = front + face_of(rib)
    sy = Y_FLOOR(13.0); SCREWS.append((sx * RIB_X, sy))
    hole = Pos(sx * RIB_X, sy, CUT - 1) * Cylinder(1.7, 12, align=(Align.CENTER, Align.CENTER, Align.MIN))
    cbore = Pos(sx * RIB_X, sy, CUT - 1) * Cylinder(3.1, 2.2, align=(Align.CENTER, Align.CENTER, Align.MIN))
    pil = Pos(sx * RIB_X, sy, CUT + T + FIT) * Cylinder(1.3, 9.5, align=(Align.CENTER, Align.CENTER, Align.MIN))
    cover = cover - face_of(hole) - face_of(cbore)
    front = front - face_of(pil)
# ---- hidden vent: one slot through the back cover's foot, along the rear bottom edge ----
vent = Pos(0, Y_FLOOR(11.5), CUT - 1) * extrude(SlotOverall(96, 3.2), 12)
cover = cover - face_of(vent)

# ---- pegs: four posts from the cover press the display against the lip ----
PEG_Z1 = SHELF_Z1 - MODULE_T - FIT
pegs = None
for sx in (-1, 1):
    for sy in (-1, 1):
        p = Pos(sx * 92.0, YC + sy * 48.0, -D + T - 0.5) * Cylinder(3.0, PEG_Z1 - (-D + T - 0.5), align=(Align.CENTER, Align.CENTER, Align.MIN))
        pegs = p if pegs is None else pegs + p
cover = cover + pegs

# ---- ports out the back: the Pi's USB and Ethernet block, plus panel-mount USB-C and micro HDMI ----
usb_w = pi4b.RIGHT_WIN[1] + 1.0
usb_cx = PI_X + pi4b.RIGHT_WIN[0]
z_lo, z_hi = ZB + 1.4 + pi4b.RIGHT_WIN[2] - 0.5, ZB + 1.4 + pi4b.RIGHT_WIN[3] + 0.5
WINDOWS = [("usb", usb_cx, usb_w, z_lo, z_hi, 1.5), ("usbc", -22.0, 10.2, ZB + 1.4 + 1.0, ZB + 1.4 + 5.6, 2.2), ("hdmi", -38.0, 8.0, ZB + 1.4 + 1.0, ZB + 1.4 + 5.6, 2.2)]
win_corners = []
for name, cx, w, za, zb, rr in WINDOWS:
    zc = (za + zb) / 2
    fx, fy, fz = ptf(cx, y_back(zc), zc)
    hf = (zb - za) / math.cos(math.radians(LEAN))
    tool = Pos(fx, fy, -D - 1.0) * extrude(RectangleRounded(w, hf, rr), T + 2.0)
    cover = cover - tool
    for sx in (-1, 1):
        for sy in (-1, 1): win_corners.append((name, fx + sx * w / 2, fy + sy * hf / 2))

STRIP = (-52.0, PI_X + 28.0, z_lo - 4.0, z_hi + 4.0)   # one recessed strip holds every port: x from, x to, z from, z to
sx0, sx1, sz0, sz1 = STRIP; sc_, sz_ = (sx0 + sx1) / 2, (sz0 + sz1) / 2
fx, fy, fz = ptf(sc_, y_back(sz_), sz_)
cover = cover - Pos(fx, fy, -D - 1.0) * extrude(RectangleRounded(sx1 - sx0, (sz1 - sz0) / math.cos(math.radians(LEAN)), R - 3), 1.0 + STRIP_D)
for _sx in (-1, 1):
    for _sy in (-1, 1): win_corners.append(("strip", fx + _sx * (sx1 - sx0) / 2, fy + _sy * (sz1 - sz0) / 2 / math.cos(math.radians(LEAN))))
# ---- stand-ins that are not printed: the display, its image, the Pi's ports ----
glass = Pos(0, YC, -LIP - GLASS_T) * extrude(RectangleRounded(GLASS_W, GLASS_H, R - GAP), GLASS_T)
module = Pos(0, YC, SHELF_Z1 - MODULE_T) * extrude(RectangleRounded(GLASS_W - 2 * SHELF_IN - 1, GLASS_H - 2 * SHELF_IN - 1, R - 2), MODULE_T)
driver = Pos(0, YC, SHELF_Z1 - MODULE_T - 12.0) * extrude(Rectangle(100, 58), 12.0)
def port_block(cx, w, za, zb, tip_in):
    zc = (za + zb) / 2; yt = y_back(zc) - tip_in
    return Pos(cx, yt - 14.0 / 2, zc) * Box(w, 14.0, zb - za)
ports = port_block(usb_cx, pi4b.RIGHT_WIN[1], z_lo + 0.5, z_hi - 0.5, 1.0) + port_block(-22.0, 9.0, ZB + 2.9, ZB + 5.0, 1.0) + port_block(-38.0, 6.4, ZB + 2.9, ZB + 5.0, 1.0)
board = Pos(PI_X, Y_PC, ZB + 0.7) * Box(pi4b.BOARD_S, pi4b.BOARD_L, pi4b.PCB_T)

# ---- every fit is asserted before a file is written ----
cx_, cy_ = GLASS_W / 2 - (R + 1) * (1 - 2 ** -0.5), YC + GLASS_H / 2 - (R + 1) * (1 - 2 ** -0.5)
assert in_sq(cx_, cy_, T + 1.5), "display corner touches the inner wall"
assert in_sq(OPEN_W / 2 - R * (1 - 2 ** -0.5), YC + OPEN_H / 2 - R * (1 - 2 ** -0.5), R + 1.0), "opening runs into the roll-over"
assert in_sq(92 + 3, YC + 48 + 3, T + 1.0), "peg outside the inner wall"
for name, x, y in win_corners:
    assert in_sq(x, y, R2 + 1.0), "window %s runs into the back roll-over" % name
assert YC + 29.0 < B - SC_DEPTH - T - 4, "scoop reaches the display driver board"
assert ZB - 6.0 >= 3.0, "standoff too short"
# the display clears the Pi stack and its ports (display is high on the face, Pi at the bottom)
assert ptf(0, 0, ZB + 1.4 + 16.5 + 3)[1] < YC - GLASS_H / 2 - 5, "Pi stack reaches the display"
parts = {"front_shell": front, "back_cover": cover, "ground_inlay": ground_inlay}
PRINT = {"front_shell": Rot(180, 0, 0), "back_cover": Pos(0, 0, D), "ground_inlay": Pos(0, 0, MARK_D)}
os.makedirs(STL_DIR, exist_ok=True)
report = {}
for name, shp in parts.items():
    p = PRINT[name] * shp
    bb = p.bounding_box(); sz = (bb.size.X, bb.size.Y, bb.size.Z)
    assert max(sz) <= BED, "%s does not fit the bed: %s" % (name, sz)
    assert bb.min.Z > -0.01, "%s dips under the bed" % name
    export_stl(p, os.path.join(STL_DIR, name + ".stl"), tolerance=0.08, angular_tolerance=0.3)
    report[name] = [round(v, 1) for v in sz]
    print("%-13s %s  %.0f cm3" % (name, report[name], shp.volume / 1000))
asm_parts = {k: asm(v) for k, v in parts.items()}
bb = Compound([asm_parts["front_shell"], asm_parts["back_cover"]]).bounding_box()
info = {"parts": report, "overall": [round(bb.size.X, 1), round(bb.size.Y, 1), round(bb.size.Z, 1)], "wall": T, "lean": LEAN, "reveal": GAP, "bed": BED,
        "screws_m3": [[round(x, 1), round(y, 1)] for x, y in SCREWS], "board_standoffs_m25": [[round(x, 1), round(y, 1)] for x, y in HOLES],
        "vent": {"y": round(Y_FLOOR(11.5), 1), "length": 96, "width": 3.2}, "glass": [GLASS_W, GLASS_H], "active": [ACT_W, ACT_H], "glass_center": [round(v, 1) for v in pta(0, YC, 0)], "opening": [OPEN_W, OPEN_H], "strip": [round(v, 1) for v in STRIP], "scoop_center": [round(v, 1) for v in pta(0, B, SC_C[2])], "scoop_half": [round(SC[0] * (1 - ((SC_C[1] - B) / SC[1]) ** 2) ** 0.5, 1), round(SC[2] * (1 - ((SC_C[1] - B) / SC[1]) ** 2) ** 0.5, 1)], "mark": [MARK_W, round(MARK_Y, 1)], "squircle_n": SQUIRCLE_N, "radius": R, "front": [2 * A, 2 * B]}
json.dump(info, open(os.path.join(STL_DIR, "info.json"), "w"), indent=1)
asm_parts = {k: asm(v) for k, v in parts.items()}
for _k, _v in asm_parts.items():
    export_step(_v, os.path.join(STL_DIR, _k + ".step"))
if MESH:
    os.makedirs(MESH, exist_ok=True)
    refs = {"glass": glass, "module": module, "driver": driver, "ports": ports, "board": board}
    meshes = []
    for name, shp in list(parts.items()) + list(refs.items()):
        s = asm_parts[name] if name in asm_parts else (asm(shp) if name not in ("ports", "board") else shp)
        export_stl(s, os.path.join(MESH, name + ".stl"), tolerance=0.02, angular_tolerance=0.1); meshes.append(name)
    json.dump({"meshes": meshes, "zoff": ZOFF, "lean": LEAN, "screen": [list(pta(sx * ACT_W / 2, YC + sy * ACT_H / 2, -LIP + 0.05)) for sx, sy in ((-1, -1), (1, -1), (1, 1), (-1, 1))]}, open(os.path.join(MESH, "meshes.json"), "w"))
print("valid", front.is_valid, cover.is_valid, "ok")
