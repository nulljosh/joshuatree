# SPDX-License-Identifier: CC-BY-NC-SA-4.0
# Copyright (c) 2026 Joshua Trommel. Hardware design, see docs/hardware/LICENSE-NOTICE.md
# Strata Kit enclosure, print-ready parts (v4: 200 x 200 x 55 mm, staggered quarter seams, feet hide the bottom hardware, cap = frame + whole-tree panel). build123d -> STL per printable part, one STEP, exploded SVGs.
# mm throughout. Numbers match docs/HARDWARE.md. Target: Bambu A1 mini (180 x 180 x 180), FDM.
#   uv run --with build123d python docs/hardware/strata_cad.py docs/hardware
import sys, os, json, re, math
from build123d import *

OUT = sys.argv[1] if len(sys.argv) > 1 else "."
STL_DIR, ASM_DIR = os.path.join(OUT, "stl"), os.path.join(OUT, "assembly")

# ---- the board: ASRock J4125B-ITX, numbers from docs/HARDWARE.md ----
BOARD = 170.0                    # mini-ITX is 170 x 170
HOLE_DX, HOLE_DY = 154.94, 157.48  # mini-ITX hole spacing (ATX spec)
SHIELD_W, SHIELD_H = 158.75, 44.45  # stock I/O shield
TALLEST = 30.0                   # heatsink + tallest rear part above the PCB; ESTIMATE, measure on the real board
PCB_T, STANDOFF = 1.6, 5.0

# ---- FDM rules ----
BED = 180.0                      # A1 mini bed, each axis
FIT = 0.2                        # clearance on every mating face
MIN_WALL = 1.6                   # four 0.4 mm lines
MAX_BRIDGE = 10.0                # longest flat roof printed without supports

CAV = BOARD + 4                  # 2 mm of air each side of the board
WALL = 2.0
CORE = CAV + 2 * WALL            # 178, still under the 180 bed
THICK = [6.5, 7.1, 7.1, 7.1, 7.1, 7.1]   # one rhythm; ring 0 stops where the I/O window starts, so the base ring is never notched; sums to 42
GAP = 2.0                        # spacer height = the vent line
STEP_IN = 0.0                    # flush column, every stratum 200 wide
BASE_W = 200.0
CAP_T = 3.0                      # flush on ring 5, hides the top nuts
ROD = 3.4                        # M3 clearance hole (3.0 + 0.4)
CH, CC = 4.2, 2.5                # vertical corner chamfers: tray outside and tray cavity (the board corner stays 1 mm clear)
CHR = CH + 0.2 * (2 - 2 ** 0.5) + 0.0   # ring hole chamfer leg, a true 0.2 mm offset of the tray chamfer
ROD_AT = 91.1                    # rods tuck into the chamfered corners
NUT_AF, NUT_T = 5.5 + 2 * FIT, 2.4 + FIT   # M3 nut plus clearance
FOOT_T, FOOT_OD = 1.6, 7.6        # printed or TPU foot, recessed into ring 0's underside, covers the nut pocket and the rod end
CAP_SPLIT = 0.0                  # cap frame cuts sit on the tree's axes, symmetric, never on a ring cut
SPLIT = 30.0                     # quarter cuts sit at +-30 mm, alternating, so no seam lines up with the ring above or below
# one maker's line, Bambu Lab PLA Matte (the A1 mini's own maker): hex as published on its filament listing, check the swatch at purchase
FILAMENT = [("Terracotta", "#B15533"), ("Caramel", "#AE835B"), ("Latte Brown", "#D3B7A7"), ("Bone White", "#CBC6B8"), ("Desert Tan", "#E8DBB7"), ("Ivory White", "#FFFFFF")]
CAP_FILAMENT = ("Desert Tan", "#E8DBB7")
INK = (0x1E, 0x1C, 0x1A)
TOPC = tuple(int(CAP_FILAMENT[1][i:i + 2], 16) for i in (1, 3, 5))
def mix(a, b, t): return tuple(round(a[i] + (b[i] - a[i]) * t) for i in range(3))
def hexc(c): return "#%02X%02X%02X" % tuple(c)
TOP_POCKET = 4.0                 # top nut pocket in ring 5: the nut sits on a 3 mm floor, the cap closes it
PAD_T, PAD_L = 2.4, 16.0         # vent spacer pad: 2.4 mm wide, 24 mm long, centred on the front and both side edges, printed into the ring above it, hugging the core so it sits 8.5 mm behind the face
BOSS_OD, PILOT = 8.0, 2.6        # board standoff printed into the tray floor, 2.6 mm pilot for a self-tapping M3x8
HOLES = [(sx * HOLE_DX / 2, sy * HOLE_DY / 2) for sx in (-1, 1) for sy in (-1, 1)]
IO_W, IO_H = 160.0, 45.0         # shield opening 158.75 x 44.45 plus clearance both sides
PW = IO_W + 2 * MIN_WALL         # rear plate width, window plus a 1.6 mm frame each side
FAC_Z0 = WALL + STANDOFF - 0.5 - MIN_WALL   # facade bottom: ring 0 is notched to here, the window bottom strip is 1.6 mm
Z_PLATE_TOP = None
CAP_R = 2.0                      # panel and frame hole corner radius
IO_Z = WALL + STANDOFF - 0.5 + IO_H / 2   # window starts 0.5 mm under the PCB, which is exactly where ring 0 ends

C = (Align.CENTER, Align.CENTER, Align.MIN)
def plate(w, t, r): return extrude(RectangleRounded(w, w, r), t)
def chamf_sq(w, c, t):
    h = w / 2; p = [(-h + c, -h), (h - c, -h), (h, -h + c), (h, h - c), (h - c, h), (-h + c, h), (-h, h - c), (-h, -h + c)]
    return extrude(Face(Wire.make_polygon([Vector(x, y, 0) for x, y in p], close=True)), t)
def rod_xy(): return [(x * ROD_AT, y * ROD_AT) for x in (-1, 1) for y in (-1, 1)]
def nut_pocket(x, y, z, t=NUT_T): return Pos(x, y, z) * Rot(0, 0, 15) * extrude(RegularPolygon(NUT_AF / 3 ** 0.5, 6), t)   # flat of the hex faces the corner chamfer

# ---- parts, in assembled coordinates ----
z = 0.0                         # ring 0 sits on the table, nuts sink into its underside
ring_z, rings = [], []
for i, t in enumerate(THICK):
    w = BASE_W - i * STEP_IN
    s = plate(w, t, 6) - Pos(0, 0, -1) * chamf_sq(CORE + 2 * FIT, CHR, t + 2)
    for x, y in rod_xy(): s -= Pos(x, y, -1) * Cylinder(ROD / 2, t + 2, align=C)
    if i == 0:
        for x, y in rod_xy():
            s -= Pos(x, y, -1) * Cylinder(FOOT_OD / 2 + 0.2, FOOT_T + 1, align=C)   # counterbore for the foot
            s -= nut_pocket(x, y, FOOT_T)                        # nut sits above the foot, the foot closes it
    if i == len(THICK) - 1:
        for x, y in rod_xy(): s -= nut_pocket(x, y, t - TOP_POCKET, TOP_POCKET + 1)   # top nut, closed by the cap
    s = Pos(0, 0, z) * s
    nz = max(z, FAC_Z0 - FIT)                              # ring 0's rear wall is notched 1.6 mm so the window gets a bottom frame strip
    if z < IO_Z + IO_H / 2 and z + t > FAC_Z0:             # any ring the window touches is notched, so no ring has a roof over the notch
        s -= Pos(0, CORE / 2 + 20, nz) * Box(PW + 2 * FIT, 40, z + t - nz + 1, align=C)
    rings.append(s); ring_z.append(z)
    z += t + (GAP if i < len(THICK) - 1 else 0)   # the cap sits straight on ring 5, no gap
STACK_TOP = z                    # underside of the cap
top_w = BASE_W - (len(THICK) - 1) * STEP_IN
Z_PLATE_TOP = IO_Z + IO_H / 2 + MIN_WALL    # rear plate top, 1.6 mm of frame above the window; the cap frame is rebated over it
def tree_polys(svg):
    from shapely.geometry import Polygon, Point
    out = []
    for d in re.findall(r' d="([^"]+)"', svg):
        toks = re.findall(r"[MLCZ]|-?\d+\.?\d*", d); i = 0; pts = []
        while i < len(toks):
            t = toks[i]
            if t in "ML": pts.append((float(toks[i + 1]), float(toks[i + 2]))); i += 3
            elif t == "C":                          # sample the cubic, never treat control points as vertices
                p0 = pts[-1]; c = [(float(toks[i + 1 + 2 * j]), float(toks[i + 2 + 2 * j])) for j in range(3)]
                for k in range(1, 17):
                    u = k / 16; w = [(1 - u) ** 3, 3 * u * (1 - u) ** 2, 3 * u ** 2 * (1 - u), u ** 3]
                    pts.append(tuple(sum(w[j] * q[ax] for j, q in enumerate([p0] + c)) for ax in (0, 1)))
                i += 7
            elif t == "Z":
                if len(pts) >= 3: out.append(Polygon(pts).buffer(0))
                pts = []; i += 1
            else: i += 1
    for cx, cy, r in re.findall(r'<circle cx="([\d.]+)" cy="([\d.]+)" r="([\d.]+)"', svg):
        out.append(Point(float(cx), float(cy)).buffer(float(r), 24))
    return out
MARK_W, MARK_D, MIN_FEATURE = 64.0, 0.8, 0.5     # tree width on the cap, engraving depth, narrowest engraved feature a 0.4 mm nozzle can follow
def build_mark():
    from shapely.ops import unary_union
    from shapely import affinity
    svg = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "landing", "logo.svg")).read()
    u = unary_union(tree_polys(svg)); x0, y0, x1, y1 = u.bounds; k = MARK_W / (x1 - x0)
    u = affinity.scale(affinity.translate(u, -(x0 + x1) / 2, -(y0 + y1) / 2), k, -k, origin=(0, 0))
    r = MIN_FEATURE / 2
    o = u.buffer(-r, join_style=1).buffer(r, join_style=1).simplify(0.01)   # opening: nothing narrower than 0.5 mm survives, spike tips round to a 0.25 mm radius
    assert o.symmetric_difference(o.buffer(-(r - 0.01), join_style=1).buffer(r - 0.01, join_style=1)).area < 0.003 * o.area, "engraved feature under 0.5 mm"
    assert o.area > 0.9 * u.area, "opening ate the tree"
    z0 = CAP_T - MARK_D; mk = None
    for g in (o.geoms if hasattr(o, "geoms") else [o]):
        wire = lambda ring: Wire.make_polygon([Vector(x, y, z0) for x, y in list(ring.coords)[:-1]], close=True)
        e = extrude(Face(wire(g.exterior), [wire(h) for h in g.interiors]), MARK_D + 1, dir=(0, 0, 1))
        mk = e if mk is None else mk + e
    return mk
mark = build_mark()
# the cap is a panel that carries the whole tree plus a frame: the panel rests on the tray walls, the frame sits on ring 5 and hides the top nuts
cap_panel = Pos(0, 0, STACK_TOP) * (extrude(RectangleRounded(CORE, CORE, CAP_R), CAP_T) - mark)
cap_frame = plate(BASE_W, CAP_T, 6) - Pos(0, 0, -1) * extrude(RectangleRounded(CORE + 2 * FIT, CORE + 2 * FIT, CAP_R + FIT), CAP_T + 2)
cap_frame -= Pos(0, CORE / 2 + 20, -1) * Box(PW + 2 * FIT, 40, Z_PLATE_TOP - STACK_TOP + FIT + 1, align=C)   # rebate over the rear plate top
cap_frame = Pos(0, 0, STACK_TOP) * cap_frame
TOP = STACK_TOP + CAP_T
ROD_LEN = 50.0                   # stock M3 x 50, ends inside the top nut

CORE_H = STACK_TOP               # tray walls run flush to the cap underside and carry the cap panel
# tray = floor + left, right, front walls. Rear wall is its own plate so nothing prints as a 160 mm bridge.
tray = chamf_sq(CORE, CH, CORE_H) - Pos(0, 0, WALL) * chamf_sq(CAV, CC, CORE_H)
tray -= Pos(0, CORE / 2 - WALL / 2, WALL) * Box(CORE + 2, WALL + 0.2, CORE_H, align=C)
zz = 0.0
for t in THICK:
    zz += t
    for side in ((1, 0), (-1, 0), (0, -1)):  # left, right, front; rear is the I/O plate
        for k in range(-5, 6):
            x = side[0] * CORE / 2 if side[0] else k * 13
            y = side[1] * CORE / 2 if side[1] else k * 13
            dims = (WALL * 3, 8, GAP) if side[0] else (8, WALL * 3, GAP)
            if zz + GAP < CORE_H - 2: tray -= Pos(x, y, zz + GAP / 2) * Box(*dims)
    zz += GAP
for hx, hy in HOLES:                                      # board standoffs are printed into the floor, no metal standoffs
    tray += Pos(hx, hy, WALL - 0.01) * Cylinder(BOSS_OD / 2, STANDOFF + 0.01, align=C)
    tray -= Pos(hx, hy, WALL + STANDOFF - 5) * Cylinder(PILOT / 2, 6, align=C)
rear = Pos(0, CORE / 2 - WALL / 2, WALL) * Box(CAV - 2 * FIT, WALL, CORE_H - WALL, align=C) & chamf_sq(CORE, CH, CORE_H)   # clipped to the chamfered corners so it never touches the rings
# facade: flush with the ring rear faces (y 100), fills the ring notch, sits on ring 0, window frame strips all >= MIN_WALL
FAC_Y0 = CORE / 2 - WALL                        # starts at the inner plate's front face so nothing overhangs while printing
FAC_SLOPE = 2.2                                 # the facade top is level with the cap panel underside until the panel edge, then rises to the rebated frame
_prof = [(FAC_Y0, FAC_Z0), (BASE_W / 2, FAC_Z0), (BASE_W / 2, Z_PLATE_TOP), (CORE / 2 + FIT + FAC_SLOPE, Z_PLATE_TOP), (CORE / 2 + FIT, STACK_TOP), (FAC_Y0, STACK_TOP)]
rear += Pos(-PW / 2, 0, 0) * extrude(Face(Wire.make_polygon([Vector(0, y, zz_) for y, zz_ in _prof], close=True)), PW, dir=(1, 0, 0))
rear -= Pos(0, CORE / 2 + 10, IO_Z) * Box(IO_W, 40, IO_H)
foot_pos = rod_xy()
feet = [Pos(x, y, 0) * Cylinder(FOOT_OD / 2, FOOT_T, align=C) for x, y in foot_pos]
rods = [Pos(x, y, FOOT_T) * Cylinder(3.0 / 2, ROD_LEN, align=C) for x, y in rod_xy()]

# ---- printable pieces, in assembled position, cut with a 0.2 mm FIT gap on every seam; (stem, shape, qty, filament hex) ----
def box_y(y0, y1): return Pos(0, (y0 + y1) / 2, 150) * Box(400, y1 - y0, 300)
def box_x(x0, x1, y0=-300, y1=300): return Pos((x0 + x1) / 2, (y0 + y1) / 2, 150) * Box(x1 - x0, y1 - y0, 300)
RET = lambda i: 50.0 if i % 2 == 0 else 40.0      # side return of the front piece, alternating so ring seams stagger on the side faces
CAP_RET = 45.0
ARM_END = 2.0                                      # pad stays this far off any cut
from shapely.geometry import box as sbox, Polygon as SPoly
from shapely import affinity
def prect(cx, cy, L, T, ang=0.0): return affinity.rotate(sbox(cx - L / 2, cy - T / 2, cx + L / 2, cy + T / 2), ang, origin=(cx, cy))
CP_T, CP_L = 2.3, 8.0                              # rod-corner pad along the inner chamfer
def pad_polys(side, ret):
    """footprints of the pads under one ring piece; side is 'front', 'rl' or 'rr'"""
    inner = CORE / 2 + FIT; yc = -BASE_W / 2 + ret
    def chamfer(sx, sy):
        m = inner - CHR / 2; d = CP_T / 2 / 2 ** 0.5
        return prect(sx * (m + d), sy * (m + d), CP_L, CP_T, -sx * sy * 45.0)
    if side == "front":
        return [chamfer(-1, -1), chamfer(1, -1), prect(0, -(inner + PAD_T / 2), PAD_L, PAD_T)] + \
               [prect(sx * (inner + PAD_T / 2), yc - FIT / 2 - ARM_END - PAD_L / 2, PAD_T, PAD_L) for sx in (-1, 1)]
    sx = -1 if side == "rl" else 1
    return [chamfer(sx, 1), prect(sx * (inner + PAD_T / 2), yc + FIT / 2 + ARM_END + PAD_L / 2, PAD_T, PAD_L), prect(sx * (inner + PAD_T / 2), 35.0, PAD_T, 24.0)]
def pad_solid(poly, z0):
    w = Wire.make_polygon([Vector(x, y, z0 - GAP) for x, y in list(poly.exterior.coords)[:-1]], close=True)
    return extrude(Face(w), GAP + 0.5, dir=(0, 0, 1))
def three(s, ret, pads_at=None):
    """front U (whole 200 mm front plus two returns), then two rear L pieces: side bar plus the rear stub"""
    yc = -BASE_W / 2 + ret; out = {}
    out["front"] = s & box_y(-300, yc - FIT / 2)
    rest = s & box_y(yc + FIT / 2, 300)
    out["rl"] = rest & box_x(-300, -FIT / 2); out["rr"] = rest & box_x(FIT / 2, 300)
    return out, yc
def to_bed(s):
    bb = s.bounding_box()
    return Pos(-bb.min.X, -bb.min.Y, -bb.min.Z) * s
RING_HEX = [h for _n, h in FILAMENT]
printables = []; PADS = {}; SEAMS = {}
for i, r in enumerate(rings):
    pcs, yc = three(r, RET(i)); SEAMS[f"ring{i}"] = yc
    for nm, p in pcs.items():
        if i > 0:
            polys = pad_polys(nm, RET(i)); PADS[f"ring{i}_{nm}"] = polys
            for pl in polys: p = p + pad_solid(pl, ring_z[i])
        assert len(p.solids()) == 1, f"ring{i}_{nm} fell into loose pieces"
        printables.append((f"ring{i}_{nm}", p, 1, RING_HEX[i]))
cpcs, cyc = three(cap_frame, CAP_RET); SEAMS["cap"] = cyc
for nm, p in cpcs.items():
    assert len(p.solids()) == 1, f"cap_frame_{nm} fell into loose pieces"
    printables.append((f"cap_frame_{nm}", p, 1, hexc(TOPC)))
printables += [("cap_panel", cap_panel, 1, hexc(TOPC)), ("tray", tray, 1, hexc(INK)), ("rear_plate", rear, 1, hexc(INK)), ("foot", feet[0], 4, hexc(INK))]
FLIP = lambda n: (n.startswith("ring") and not n.startswith("ring0")) or n.startswith("cap_frame")  # spacer pad goes up, flat face on the bed; the cap prints right side up so the engraving is the last layer

# ---- checks, in code ----
def downward_roofs(s):
    # flat faces facing down that are not on the bed: list their longest side
    out = []
    for f in s.faces():
        if f.center().Z > 0.05 and f.normal_at().Z < -0.707 and f.geom_type == GeomType.PLANE:
            bb = f.bounding_box(); out.append(max(bb.size.X, bb.size.Y, bb.size.Z))
    return out

report = []
os.makedirs(STL_DIR, exist_ok=True); os.makedirs(ASM_DIR, exist_ok=True)
worst_roof = 0.0
for name, s, qty, hx in printables:
    p = Rot(180, 0, 0) * s if FLIP(name) else (Rot(90, 0, 0) * s if name == "rear_plate" else s)
    if name.endswith("_front"): p = Rot(0, 0, 45) * p          # the 200 mm front lies diagonally on the bed
    p = to_bed(p)
    bb = p.bounding_box()
    assert max(bb.size.X, bb.size.Y, bb.size.Z) <= BED, f"{name} {bb.size} does not fit the {BED} mm bed"
    roofs = downward_roofs(p)
    worst_roof = max([worst_roof] + roofs)
    assert all(r <= MAX_BRIDGE for r in roofs), f"{name} has a {max(roofs):.1f} mm unsupported roof"
    export_stl(p, os.path.join(STL_DIR, f"{name}.stl"))
    report.append({"part": name, "qty": qty, "hex": hx, "x": round(bb.size.X, 1), "y": round(bb.size.Y, 1), "z": round(bb.size.Z, 1)})

# board and port cutouts clear the J4125B-ITX
assert CAV >= BOARD + 2 * 1.0, "cavity leaves under 1 mm each side of the board"
assert IO_W >= SHIELD_W + 2 * FIT and IO_H >= SHIELD_H + 2 * FIT, "I/O window smaller than the shield plus 0.2 mm"
assert IO_Z - IO_H / 2 >= WALL and IO_Z + IO_H / 2 <= CORE_H, "I/O window leaves the plate"
assert IO_Z - IO_H / 2 <= WALL + STANDOFF + PCB_T, "window bottom sits above the board's rear ports"
assert IO_W < CAV - 2 * MIN_WALL, "I/O window leaves the plate too little frame"
for hx, hy in HOLES: assert abs(hx) < BOARD / 2 and abs(hy) < BOARD / 2 and abs(hx) <= CAV / 2 - 3 and abs(hy) <= CAV / 2 - 3, "mount hole off the board"
assert abs(2 * HOLES[3][0] - HOLE_DX) < 1e-6 and abs(2 * HOLES[3][1] - HOLE_DY) < 1e-6, "hole spacing is not 154.94 x 157.48"
assert CORE_H >= WALL + STANDOFF + PCB_T + TALLEST, "tray too short for the tallest part on the board"
# tie rods clear the core, sit inside every ring, and have wall left
hd = lambda r: (2 * ROD_AT - (CORE + 2 * FIT - CHR)) / 2 ** 0.5   # distance from the rod axis to the chamfered ring hole line
assert hd(0) - ROD / 2 >= MIN_WALL - 1e-9, "rod hole too close to the core"
assert hd(0) - (FOOT_OD / 2 + 0.2) >= MIN_WALL - 1e-9, "foot recess too close to the core"
assert hd(0) - NUT_AF / 2 >= MIN_WALL - 1e-9, "nut pocket too close to the core"
assert (CORE - CH - (CAV - CC)) / 2 ** 0.5 >= MIN_WALL - 1e-9, "tray corner wall under MIN_WALL"
assert (2 * (CAV / 2) - CC - 2 * BOARD / 2) / 2 ** 0.5 >= 1.0, "cavity chamfer leaves the board corner under 1 mm"
BOSS_FACE = BASE_W / 2 - (CORE / 2 + FIT + PAD_T)
assert BOSS_FACE >= 8.0, "spacer pad closer than 8 mm to the outer face"
assert PAD_T >= MIN_WALL, "spacer pad thin"
from shapely.geometry import Point
from shapely.ops import unary_union
_h = (CORE + 2 * FIT) / 2
core_poly = SPoly([(-_h + CHR, -_h), (_h - CHR, -_h), (_h, -_h + CHR), (_h, _h - CHR), (_h - CHR, _h), (-_h + CHR, _h), (-_h, _h - CHR), (-_h, -_h + CHR)])
rod_pts = [Point(x, y) for x, y in rod_xy()]
worst_rod = 0.0; worst_com = 0.0
for nm, polys in PADS.items():
    assert len(polys) >= 2, f"{nm} has under 2 pads"
    u = unary_union(polys)
    assert abs(u.area - sum(p.area for p in polys)) < 1e-6, f"{nm} pads overlap each other"
    assert u.intersection(core_poly).area < 1e-6, f"{nm} pad hangs into the core"
    sh = next(sh for n, sh, q, hx in printables if n == nm); cm = sh.center(CenterOf.MASS)
    hull = u.convex_hull; pt = Point(cm.X, cm.Y)
    # the pads sit 8.4 mm behind the faces, the side bars are 10.8 mm wide, so the COM of an L piece can lie up to half a wall outboard of the pad line: allow that, nothing more
    assert hull.buffer(CORE / 2 * 0 + (BASE_W / 2 - CORE / 2) / 2).contains(pt), f"{nm} centre of mass outside its pads"
    worst_com = max(worst_com, hull.distance(pt))
    for rp in rod_pts:
        if nm.endswith("front") and rp.y > 0: continue
        if nm.endswith("rl") and not (rp.x < 0 and rp.y > 0): continue
        if nm.endswith("rr") and not (rp.x > 0 and rp.y > 0): continue
        d = u.distance(rp); worst_rod = max(worst_rod, d); assert d <= 5.0, f"{nm} rod axis {d:.1f} mm from nearest pad"
    for p in polys:
        assert (BASE_W / 2 - max(abs(c) for c in p.bounds)) >= 8.4 - 1e-6 or False, f"{nm} pad closer than 8.4 mm to a face"
        assert all(p.distance(rp) >= ROD / 2 + MIN_WALL - 1e-6 for rp in rod_pts), f"{nm} pad leaves under 1.6 mm round a rod hole"
assert len(PADS) == 15, "rings 1 to 5 need three pieces each"
assert all(SEAMS[k] > -BASE_W / 2 + (BASE_W - CORE) / 2 + 20 for k in SEAMS), "a seam reaches the front face"
assert all(next(sh for n, sh, q, hx in printables if n == f"ring{i}_front").bounding_box().size.X > BASE_W - 0.01 for i in range(6)), "a ring front is not one piece"
assert next(sh for n, sh, q, hx in printables if n == "cap_frame_front").bounding_box().size.X > BASE_W - 0.01, "the cap front is not one piece"
assert abs(SEAMS["cap"] - SEAMS["ring5"]) >= 5.0 - 1e-9 and abs(SEAMS["cap"] - SEAMS["ring4"]) >= 5.0 - 1e-9, "cap and ring side seams closer than 5 mm"
assert all(abs(SEAMS[f"ring{i}"] - SEAMS[f"ring{i + 1}"]) >= 9.9 for i in range(5)), "neighbouring ring seams not staggered"
STRIP = lambda: ((PW - IO_W) / 2, IO_Z - IO_H / 2 - FAC_Z0, Z_PLATE_TOP - (IO_Z + IO_H / 2))
assert ROD_AT + ROD / 2 + MIN_WALL <= top_w / 2, "rod hole leaves under 1.6 mm in the top ring and cap"
assert STACK_TOP - TOP_POCKET + NUT_T - 1.0 <= FOOT_T + ROD_LEN <= STACK_TOP - 0.3, "rod must reach into the top nut and stay under the cap"
assert TOP <= 55.0 + 1e-6 and BASE_W <= 200.0, "over the 200 x 200 x 55 target"
assert THICK[-1] - TOP_POCKET >= MIN_WALL and TOP_POCKET >= NUT_T, "top nut pocket leaves a thin floor or is too shallow"
assert CAP_T - MARK_D >= MIN_WALL, "engraving leaves a thin cap"
# walls
assert WALL >= MIN_WALL and (top_w - CORE - 2 * FIT) / 2 >= MIN_WALL, "ring frame too thin"
assert THICK[0] - FOOT_T - NUT_T >= MIN_WALL, "nut pocket leaves a thin floor"
assert all(abs(v - MIN_WALL) < 1e-9 for v in STRIP()), "rear plate frame strips are not all 1.6 mm"
assert CAP_T - (Z_PLATE_TOP - STACK_TOP) >= MIN_WALL, "cap lip over the rear plate under MIN_WALL"
assert max(THICK) - min(THICK[1:]) < 1e-9, "strata rhythm broken"
assert (BOSS_OD - PILOT) / 2 >= MIN_WALL and abs(HOLES[0][0]) + BOSS_OD / 2 <= CAV / 2, "board standoff wall thin or off the tray"
assert 13 - 8 >= MIN_WALL and min(THICK) >= MIN_WALL, "vent web too thin"
assert (CORE + 2 * FIT - CORE) / 2 >= FIT - 1e-9, "ring hole must clear the tray by 0.2 mm"
# no two printed parts may overlap in the assembled position
placed = [(n, sh) for n, sh, q, hx in printables if n != "foot"] + [(f"foot{j}", f) for j, f in enumerate(feet)]
bbs = [sh.bounding_box() for _, sh in placed]
worst_ov = 0.0
for a in range(len(placed)):
    for b in range(a + 1, len(placed)):
        A, B = bbs[a], bbs[b]
        if A.max.X < B.min.X or B.max.X < A.min.X or A.max.Y < B.min.Y or B.max.Y < A.min.Y or A.max.Z < B.min.Z or B.max.Z < A.min.Z: continue
        try: v = (placed[a][1] & placed[b][1]).volume
        except Exception: v = 0.0
        worst_ov = max(worst_ov, v)
        assert v < 0.01, f"{placed[a][0]} and {placed[b][0]} overlap by {v:.3f} mm3"
asm_parts = rings + [cap_panel, cap_frame, tray, rear, *feet, *rods]
asm = Compound(asm_parts)
if os.environ.get("STRATA_MESH"):                  # assembled-position meshes for render_strata.py, one per printed piece
    import shutil; shutil.rmtree(os.environ["STRATA_MESH"], ignore_errors=True); os.makedirs(os.environ["STRATA_MESH"])
    meshes = []
    for n, sh, q, hx in printables:
        for j in range(q if n == "foot" else 1):
            nm = f"{n}{j}" if n == "foot" else n
            export_stl(feet[j] if n == "foot" else sh, os.path.join(os.environ["STRATA_MESH"], nm + ".stl"), tolerance=0.02, angular_tolerance=0.1)
            meshes.append({"name": nm, "hex": hx})
    for i, r in enumerate(rods):
        export_stl(r, os.path.join(os.environ["STRATA_MESH"], f"rod{i}.stl")); meshes.append({"name": f"rod{i}", "hex": "#787878"})
    json.dump(meshes, open(os.path.join(os.environ["STRATA_MESH"], "meshes.json"), "w"))
bb = asm.bounding_box()
export_step(asm, os.path.join(OUT, "strata.step"))
json.dump({"license": "CC-BY-NC-SA-4.0", "copyright": "2026 Joshua Trommel", "parts": report}, open(os.path.join(STL_DIR, "manifest.json"), "w"), indent=1)

# ---- exploded step diagrams (iso view, visible edges) ----
def explode(items):
    out = []
    for shape, dz in items: out.append(Pos(0, 0, dz) * shape)
    return Compound(out)
step_sets = {
 "step1-feet-and-rods": [(rings[0], 0)] + [(r, 28) for r in rods] + [(f, -10) for f in feet],
 "step2-tray":          [(rings[0], 0)] + [(tray, 40)],
 "step3-rings":         [(rings[0], 0), (tray, 0)] + [(r, 14 * i) for i, r in enumerate(rings) if i],
 "step4-rear-plate":    [(tray, 0)] + list(zip(rings, [0] * 6)) + [(rear, 30)],
 "step5-cap-and-nuts":  [(tray, 0)] + list(zip(rings, [0] * 6)) + [(rear, 0), (cap_panel, 45), (cap_frame, 45)] + [(r, 0) for r in rods],
}
for name, items in step_sets.items():
    vis, _ = explode(items).project_to_viewport((120, -160, 110), (0, 0, 1))
    d = ExportSVG(unit=Unit.MM, line_weight=0.3); d.add_layer("v", line_weight=0.3); d.add_shape(vis, layer="v")
    d.write(os.path.join(ASM_DIR, f"{name}.svg"))

print("ASSEMBLED %.1f x %.1f x %.1f mm, rod M3x%d, spacers printed into the rings" % (bb.size.X, bb.size.Y, bb.size.Z, ROD_LEN))
print("PASS fits %d mm bed; widest roof %.1f mm <= %.0f; shield %.2fx%.2f in %.0fx%.0f window; wall >= %.1f" % (BED, worst_roof, MAX_BRIDGE, SHIELD_W, SHIELD_H, IO_W, IO_H, MIN_WALL))
vol = sum(s.volume * q for _, s, q, _h in printables) / 1000
print("SOLID VOLUME %.0f cm3 (about %.0f g PLA at 1.24 g/cm3 and 100%% fill)" % (vol, vol * 1.24))
for r in report: print("  %-22s x%d  %5.1f x %5.1f x %5.1f  %s" % (r["part"], r["qty"], r["x"], r["y"], r["z"], r["hex"]))
print("PIECES", sum(r["qty"] for r in report), "FILES", len(report))
print("pad to face %.1f mm, worst part overlap %.4f mm3, strips %s" % (BOSS_FACE, worst_ov, ["%.2f" % v for v in STRIP()]))
# the parts table in docs/HARDWARE.md is generated here from the constants and manifest.json, so it cannot drift
npc = lambda pre: sum(r["qty"] for r in report if r["part"].startswith(pre))
tbl = "\n".join([
 "| Part | Size | Print |", "|---|---|---|",
 "| Rings S0 to S5 | %.0f mm square, %s mm thick (base ring first, then equal strata), %.1f mm vent gap between each, %d pieces per ring: one 200 mm front with side returns of 50 and 40 mm alternating, and two rear L pieces, so every seam lands on a side face; rings 1 to 5 carry 5 pads under the front and 3 under each rear L (rod corner, arm end, mid), %.1f x %.1f mm and 8.4 mm or more behind the face, nut pocket under ring 0 and on top of ring 5 | FDM PLA or PETG at 0.2 mm, one tone each |" % (BASE_W, " / ".join("%g" % t for t in THICK), GAP, npc("ring") // 6, PAD_L, PAD_T),
 "| Cap | %.0f mm square frame in %d pieces (one front U plus two rear L, side cut at 45 mm, 5 mm from the ring 5 seam) and a %.0f mm panel in one piece carrying the whole tree, %.0f mm thick, panel and frame hole corners R%.0f, tree from `landing/logo.svg` engraved %.1f mm | Same print, printed right side up |" % (BASE_W, npc("cap_frame"), CORE, CAP_T, CAP_R, MARK_D),
 "| Core tray | %.0f mm square, %.1f mm tall, %.0f mm walls, corners chamfered %.1f mm, vent slots on every gap line, 4 board standoffs printed on the floor, no rear wall | Printed, floor down, no supports |" % (CORE, CORE_H, WALL, CH),
 "| Rear plate | %.1f x %.1f mm and %.0f mm deep including the facade, one piece, with the %.0f x %.0f I/O window and a %.1f mm frame on all four sides, flush with the ring faces | Printed lying flat |" % (next(r for r in report if r["part"] == "rear_plate")["x"], next(r for r in report if r["part"] == "rear_plate")["y"], next(r for r in report if r["part"] == "rear_plate")["z"], IO_W, IO_H, MIN_WALL),
 "| Feet | %.1f mm round, %.1f mm thick, recessed under ring 0, cover the nut and the rod end | TPU or PLA |" % (FOOT_OD, FOOT_T)])
hw = os.path.join(OUT, "..", "HARDWARE.md")
if os.path.exists(hw):
    t = open(hw).read(); a, b = "<!-- parts:start -->", "<!-- parts:end -->"
    if a in t: open(hw, "w").write(t[:t.index(a) + len(a)] + "\n" + tbl + "\n" + t[t.index(b):])
