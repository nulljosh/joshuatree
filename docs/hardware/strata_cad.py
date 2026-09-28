# Strata enclosure, manufacturable parts. build123d -> STEP (whole thing) + STL per part.
# mm throughout. Numbers match docs/HARDWARE.md "Strata build blueprint".
import sys, os
from build123d import *

OUT = sys.argv[1] if len(sys.argv) > 1 else "."
BOARD = 170.0                    # mini-ITX
CAV = BOARD + 4                  # 2 mm clearance each side
WALL = 2.0
CORE = CAV + 2 * WALL            # 178
CORE_H = 62.0
THICK = [9.0, 7.0, 10.0, 6.5, 8.5, 7.0]
GAP = 2.0                        # spacer height = the vent line
STEP_IN = 3.0                    # each stratum 3 mm narrower than the one below
BASE_W = 224.0
CAP_T = 4.5
ROD = 3.4                        # M3 clearance
ROD_AT = (CORE / 2 + 8)          # rods sit 8 mm outside the core wall, inside every stratum
# ponytail: mini-ITX hole spacing 154.94 x 157.48 from the ATX spec; check the board before cutting
HOLES = [(-77.47, -78.74), (77.47, -78.74), (-77.47, 78.74), (77.47, 78.74)]
IO_W, IO_H, IO_Z = 160.0, 46.0, 30.5            # standard shield opening 158.75 x 44.45, 0.6 mm clearance

def rounded_plate(w, t, r):
    return extrude(RectangleRounded(w, w, r), t)

def rods(t):
    return [Pos(x * ROD_AT, y * ROD_AT, 0) * Cylinder(ROD / 2, t, align=(Align.CENTER, Align.CENTER, Align.MIN))
            for x in (-1, 1) for y in (-1, 1)]

parts, z = {}, 0.0
io_cut = Pos(0, CORE / 2 + 20, IO_Z) * Box(IO_W, 40, IO_H)
for i, t in enumerate(THICK):
    w = BASE_W - i * STEP_IN
    s = rounded_plate(w, t, 6) - Pos(0, 0, -1) * extrude(Rectangle(CORE + 0.4, CORE + 0.4), t + 2)
    for r in rods(t): s -= r
    s = Pos(0, 0, z) * s
    s -= io_cut
    parts[f"stratum{i}"] = s
    z += t + GAP
top_w = BASE_W - len(THICK) * STEP_IN
cap = rounded_plate(top_w, CAP_T, 4)
for r in rods(CAP_T): cap -= r
parts["cap"] = Pos(0, 0, z) * cap

# core tray: floor + four walls, rear I/O window, vent slots aligned with every gap
tray = Box(CORE, CORE, CORE_H, align=(Align.CENTER, Align.CENTER, Align.MIN)) \
     - Pos(0, 0, WALL) * Box(CAV, CAV, CORE_H, align=(Align.CENTER, Align.CENTER, Align.MIN))
tray -= Pos(0, CORE / 2, IO_Z) * Box(IO_W - 2, WALL * 3, IO_H - 2)
zz = 0.0
for t in THICK:
    zz += t
    for side in ((1, 0), (-1, 0), (0, -1)):  # left, right, front; rear is the I/O
        for k in range(-5, 6):
            x = side[0] * CORE / 2 if side[0] else k * 13
            y = side[1] * CORE / 2 if side[1] else k * 13
            dims = (WALL * 3, 8, GAP) if side[0] else (8, WALL * 3, GAP)
            if zz + GAP < CORE_H - 2: tray -= Pos(x, y, zz + GAP / 2) * Box(*dims)
    zz += GAP
for hx, hy in HOLES:
    tray -= Pos(hx, hy, 0) * Cylinder(1.7, WALL * 3)
parts["core_tray"] = tray

os.makedirs(OUT, exist_ok=True)
for name, p in parts.items():
    export_stl(p, os.path.join(OUT, f"{name}.stl"))
export_step(Compound(list(parts.values())), os.path.join(OUT, "strata.step"))

# plan + front elevation as one SVG drawing
asm = Compound(list(parts.values()))
drw = ExportSVG(unit=Unit.MM, line_weight=0.25)
drw.add_layer("v", line_weight=0.3)
vis_front, _ = asm.project_to_viewport((0, -1000, 0), (0, 0, 1))
drw.add_shape(vis_front, layer="v")
drw.write(os.path.join(OUT, "strata-front.svg"))
drw2 = ExportSVG(unit=Unit.MM, line_weight=0.25); drw2.add_layer("v", line_weight=0.3)
vis_top, _ = asm.project_to_viewport((0, 0, 1000), (0, 1, 0))
drw2.add_shape(vis_top, layer="v"); drw2.write(os.path.join(OUT, "strata-plan.svg"))

bb = asm.bounding_box()
print("PARTS", len(parts), "SIZE %.1f x %.1f x %.1f mm" % (bb.size.X, bb.size.Y, bb.size.Z))
assert CORE < top_w - 2 * 3, "cap must overhang the core"
assert ROD_AT + ROD / 2 < top_w / 2 - 2, "rods must stay inside the smallest stratum"
