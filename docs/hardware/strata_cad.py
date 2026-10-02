# Strata Kit enclosure, print-ready parts (v2: no feet, no loose spacers, no metal standoffs). build123d -> STL per printable part, one STEP, exploded SVGs.
# mm throughout. Numbers match docs/HARDWARE.md. Target: Bambu A1 mini (180 x 180 x 180), FDM.
#   uv run --with build123d python docs/hardware/strata_cad.py docs/hardware
import sys, os, json
from build123d import *

OUT = sys.argv[1] if len(sys.argv) > 1 else "."
STL_DIR, ASM_DIR = os.path.join(OUT, "stl"), os.path.join(OUT, "assembly")

# ---- the board: ASRock J4125B-ITX, numbers from docs/HARDWARE.md ----
BOARD = 170.0                    # mini-ITX is 170 x 170
HOLE_DX, HOLE_DY = 154.94, 157.48  # mini-ITX hole spacing (ATX spec)
SHIELD_W, SHIELD_H = 158.75, 44.45  # stock I/O shield
TALLEST = 30.0                   # heatsink + tallest rear part above the PCB; ESTIMATE, measure on the real board
PCB_T, STANDOFF = 1.6, 6.0

# ---- FDM rules ----
BED = 180.0                      # A1 mini bed, each axis
FIT = 0.2                        # clearance on every mating face
MIN_WALL = 1.6                   # four 0.4 mm lines
MAX_BRIDGE = 10.0                # longest flat roof printed without supports

CAV = BOARD + 4                  # 2 mm of air each side of the board
WALL = 2.0
CORE = CAV + 2 * WALL            # 178, still under the 180 bed
THICK = [9.0, 7.0, 10.0, 6.5, 8.5, 7.0]
GAP = 2.0                        # spacer height = the vent line
STEP_IN = 3.0                    # each stratum 3 mm narrower than the one below
BASE_W = 224.0
CAP_T = 4.5
ROD = 3.4                        # M3 clearance hole (3.0 + 0.4)
ROD_AT = CORE / 2 + 8            # rods sit 8 mm outside the core wall
NUT_AF, NUT_T = 5.5 + 2 * FIT, 2.4 + FIT   # M3 nut plus clearance
SP_OD = 7.0                      # spacer boss outer diameter (printed into the ring above it)
BOSS_OD, PILOT = 8.0, 2.6        # board standoff printed into the tray floor, 2.6 mm pilot for a self-tapping M3x8
HOLES = [(sx * HOLE_DX / 2, sy * HOLE_DY / 2) for sx in (-1, 1) for sy in (-1, 1)]
IO_W, IO_H, IO_Z = 160.0, 46.0, 30.5   # shield opening 158.75 x 44.45 plus clearance both sides

C = (Align.CENTER, Align.CENTER, Align.MIN)
def plate(w, t, r): return extrude(RectangleRounded(w, w, r), t)
def rod_xy(): return [(x * ROD_AT, y * ROD_AT) for x in (-1, 1) for y in (-1, 1)]
def nut_pocket(x, y, z): return Pos(x, y, z) * extrude(RegularPolygon(NUT_AF / 3 ** 0.5, 6), NUT_T)

# ---- parts, in assembled coordinates ----
z = 0.0                         # ring 0 sits on the table, nuts sink into its underside
ring_z, rings = [], []
for i, t in enumerate(THICK):
    w = BASE_W - i * STEP_IN
    s = plate(w, t, 6) - Pos(0, 0, -1) * extrude(Rectangle(CORE + 2 * FIT, CORE + 2 * FIT), t + 2)
    for x, y in rod_xy(): s -= Pos(x, y, -1) * Cylinder(ROD / 2, t + 2, align=C)
    if i == 0:
        for x, y in rod_xy(): s -= nut_pocket(x, y, 0)   # nut drops in from underneath, so no feet
    s = Pos(0, 0, z) * s
    if i > 0:                                            # the 2 mm vent spacer is a boss on the ring's underside
        for x, y in rod_xy(): s += Pos(x, y, z - GAP) * (extrude(Circle(SP_OD / 2), GAP) - Pos(0, 0, -1) * Cylinder(ROD / 2, GAP + 2, align=C))
    if z < IO_Z + IO_H / 2 and z + t > IO_Z - IO_H / 2:   # any ring the window touches is notched its full height, so no ring has a roof over the notch
        s -= Pos(0, CORE / 2 + 20, z - 1) * Box(IO_W, 40, t + 2, align=C)
    rings.append(s); ring_z.append(z)
    z += t + GAP
STACK_TOP = z                    # underside of the cap
top_w = BASE_W - len(THICK) * STEP_IN
cap = plate(top_w, CAP_T, 4)
for x, y in rod_xy():
    cap -= Pos(x, y, -1) * Cylinder(ROD / 2, CAP_T + 2, align=C)
    cap -= nut_pocket(x, y, CAP_T - NUT_T)              # nut sinks into the top face
cap = Pos(0, 0, STACK_TOP) * cap
for x, y in rod_xy(): cap += Pos(x, y, STACK_TOP - GAP) * (extrude(Circle(SP_OD / 2), GAP) - Pos(0, 0, -1) * Cylinder(ROD / 2, GAP + 2, align=C))
TOP = STACK_TOP + CAP_T
ROD_LEN = 5 * -(-(TOP - 1) // 5)  # next 5 mm up, M3 rods come in 5 mm steps

CORE_H = STACK_TOP - FIT         # tray top stops 0.2 under the cap
# tray = floor + left, right, front walls. Rear wall is its own plate so nothing prints as a 160 mm bridge.
tray = Box(CORE, CORE, CORE_H, align=C) - Pos(0, 0, WALL) * Box(CAV, CAV, CORE_H, align=C)
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
rear = Pos(0, CORE / 2 - WALL / 2, WALL) * Box(CAV - 2 * FIT, WALL, CORE_H - WALL, align=C)
rear -= Pos(0, CORE / 2, IO_Z) * Box(IO_W, WALL * 3, IO_H)

rods = [Pos(x, y, 0) * Cylinder(3.0 / 2, ROD_LEN, align=C) for x, y in rod_xy()]

# ---- printable pieces: (file stem, shape, qty). Each is cut to quadrants if it can't fit the bed. ----
def quadrant(s, sx, sy):
    return s & Pos(sx * 150, sy * 150, 150) * Box(300, 300, 300)

def to_bed(s):
    bb = s.bounding_box()
    return Pos(-bb.min.X, -bb.min.Y, -bb.min.Z) * s

def same(a, b): return abs(a.volume - b.volume) < 1.0
printables = []
for i, r in enumerate(rings):
    fr, rl, rr = quadrant(r, 1, -1), quadrant(r, -1, 1), quadrant(r, 1, 1)
    if same(fr, rr) and same(fr, rl): printables.append((f"ring{i}_quarter", fr, 4))
    else:
        printables += [(f"ring{i}_front_quarter", fr, 2), (f"ring{i}_rear_left", rl, 1), (f"ring{i}_rear_right", rr, 1)]
printables += [("cap_quarter", quadrant(cap, 1, -1), 4), ("tray", tray, 1), ("rear_plate", Rot(90, 0, 0) * rear, 1)]  # rear plate printed lying flat, window is just a hole
FLIP = lambda n: n.startswith("cap") or (n.startswith("ring") and not n.startswith("ring0"))  # spacer boss goes up, flat face on the bed

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
for name, s, qty in printables:
    p = to_bed(Rot(180, 0, 0) * s if FLIP(name) else s)
    bb = p.bounding_box()
    assert max(bb.size.X, bb.size.Y, bb.size.Z) <= BED, f"{name} {bb.size} does not fit the {BED} mm bed"
    roofs = downward_roofs(p)
    worst_roof = max([worst_roof] + roofs)
    assert all(r <= MAX_BRIDGE for r in roofs), f"{name} has a {max(roofs):.1f} mm unsupported roof"
    export_stl(p, os.path.join(STL_DIR, f"{name}.stl"))
    report.append({"part": name, "qty": qty, "x": round(bb.size.X, 1), "y": round(bb.size.Y, 1), "z": round(bb.size.Z, 1)})

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
assert ROD_AT - ROD / 2 > CORE / 2 + FIT + MIN_WALL, "rod hole too close to the core"
assert ROD_AT + ROD / 2 + MIN_WALL <= top_w / 2, "rod hole leaves under 1.6 mm in the top ring and cap"
assert ROD_LEN >= TOP - NUT_T and ROD_LEN <= BED, "rod length"
# walls
assert WALL >= MIN_WALL and (top_w - CORE - 2 * FIT) / 2 >= MIN_WALL, "ring frame too thin"
assert CAP_T - NUT_T >= MIN_WALL and THICK[0] - NUT_T >= MIN_WALL, "nut pocket leaves a thin floor"
assert (SP_OD - ROD) / 2 >= MIN_WALL, "spacer boss wall too thin"
assert (BOSS_OD - PILOT) / 2 >= MIN_WALL and abs(HOLES[0][0]) + BOSS_OD / 2 <= CAV / 2, "board standoff wall thin or off the tray"
assert 13 - 8 >= MIN_WALL and min(THICK) >= MIN_WALL, "vent web too thin"
assert (CORE + 2 * FIT - CORE) / 2 >= FIT - 1e-9, "ring hole must clear the tray by 0.2 mm"
asm_parts = rings + [cap, tray, rear, *rods]
assert len(asm_parts) == len(rings) + 1 + 1 + 1 + 4
asm = Compound(asm_parts)
bb = asm.bounding_box()
export_step(asm, os.path.join(OUT, "strata.step"))
json.dump(report, open(os.path.join(STL_DIR, "manifest.json"), "w"), indent=1)

# ---- exploded step diagrams (iso view, visible edges) ----
def explode(items):
    out = []
    for shape, dz in items: out.append(Pos(0, 0, dz) * shape)
    return Compound(out)
step_sets = {
 "step1-feet-and-rods": [(rings[0], 0)] + [(r, 28) for r in rods],
 "step2-tray":          [(rings[0], 0)] + [(tray, 40)],
 "step3-rings":         [(rings[0], 0), (tray, 0)] + [(r, 14 * i) for i, r in enumerate(rings) if i],
 "step4-rear-plate":    [(tray, 0)] + list(zip(rings, [0] * 6)) + [(rear, 0)],
 "step5-cap-and-nuts":  [(tray, 0)] + list(zip(rings, [0] * 6)) + [(rear, 0), (cap, 45)] + [(r, 0) for r in rods],
}
for name, items in step_sets.items():
    vis, _ = explode(items).project_to_viewport((120, -160, 110), (0, 0, 1))
    d = ExportSVG(unit=Unit.MM, line_weight=0.3); d.add_layer("v", line_weight=0.3); d.add_shape(vis, layer="v")
    d.write(os.path.join(ASM_DIR, f"{name}.svg"))

print("ASSEMBLED %.1f x %.1f x %.1f mm, rod M3x%d, spacers printed into the rings" % (bb.size.X, bb.size.Y, bb.size.Z, ROD_LEN))
print("PASS fits %d mm bed; widest roof %.1f mm <= %.0f; shield %.2fx%.2f in %.0fx%.0f window; wall >= %.1f" % (BED, worst_roof, MAX_BRIDGE, SHIELD_W, SHIELD_H, IO_W, IO_H, MIN_WALL))
vol = sum(s.volume * q for _, s, q in printables) / 1000
print("SOLID VOLUME %.0f cm3 (about %.0f g PLA at 1.24 g/cm3 and 100%% fill)" % (vol, vol * 1.24))
for r in report: print("  %-22s x%d  %5.1f x %5.1f x %5.1f" % (r["part"], r["qty"], r["x"], r["y"], r["z"]))
