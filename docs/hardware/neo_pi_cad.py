# Neo Pi enclosure for a Raspberry Pi 4 Model B, printable parts. build123d -> STEP (whole thing) + STL per part.
# Same stacked-ring look as neo_cad.py (the mini-ITX case), sized for an 85 x 56 mm board. mm throughout.
# Run:  python3 neo_pi_cad.py OUTDIR    (needs build123d)
# Print docs/hardware/PI-CASE.md's fit_test.stl first. Every number from the Raspberry Pi 4B mechanical drawing
# is below as BOARD / HOLES / the window ranges. The windows are deliberately bigger than the connectors, so a
# half-millimetre of error in a port position still fits. Check the real board with calipers before the full print.
import sys, os
from build123d import *

OUT = sys.argv[1] if len(sys.argv) > 1 else "."
BOARD_L, BOARD_S = 85.0, 56.0          # Pi 4B board
CLR = 2.0                              # clearance each side
WALL = 2.0
CAV_L, CAV_S = BOARD_L + CLR, BOARD_S + CLR
CORE_L, CORE_S = CAV_L + 2 * WALL, CAV_S + 2 * WALL        # 93 x 64
FLOOR, STANDOFF, PCB_T = 2.0, 4.0, 1.4
PCB_TOP = FLOOR + STANDOFF + PCB_T                          # 7.4 above the tray floor
THICK = [5.0, 4.0, 5.5, 4.0]
GAP = 2.0                                                   # the vent line between strata
STEP_IN = 3.0                                               # each stratum 3 mm smaller than the one below
CORE_H = sum(THICK) + GAP * len(THICK)                      # 26.5, the cap sits on the tray
CAP_T = 3.0
ROD = 3.4                                                   # M3 clearance
ROD_X, ROD_Y = CORE_L / 2 + 8, CORE_S / 2 + 8
TOP_L, TOP_S = 2 * (ROD_X + 6), 2 * (ROD_Y + 6)             # smallest stratum keeps 6 mm round every rod
BASE_L, BASE_S = TOP_L + len(THICK) * STEP_IN, TOP_S + len(THICK) * STEP_IN
# Pi 4B mounting holes: 58 x 49 mm, 3.5 mm in from each corner, relative to the board centre
HOLES = [(-39.0, -24.5), (19.0, -24.5), (-39.0, 24.5), (19.0, 24.5)]

def plate(w, d, t, r):
    return extrude(RectangleRounded(w, d, r), t)

def rods(t):
    return [Pos(x * ROD_X, y * ROD_Y, 0) * Cylinder(ROD / 2, t, align=(Align.CENTER, Align.CENTER, Align.MIN))
            for x in (-1, 1) for y in (-1, 1)]

# Windows, all in tray coordinates. (centre along the wall, width, z from, z to)
FRONT = (-11.0, 52.0, PCB_TOP - 1.0, PCB_TOP + 8.0)     # USB-C power, two micro-HDMI, 3.5 mm jack: board x 5.5 to 57.5
RIGHT = (0.25, 53.5, PCB_TOP - 1.0, PCB_TOP + 16.5)     # Ethernet and the four USB ports: board y 1.5 to 55
BACK = (-9.5, 51.5, PCB_TOP, PCB_TOP + 9.5)             # the 40-pin GPIO header, for a serial cable or a HAT
LEFT = (0.0, 14.0, 3.0, PCB_TOP)                        # the microSD card under the board

def cut(side, win, depth=40.0):
    c, w, z0, z1 = win
    h = z1 - z0
    if side == "front": return Pos(c, -(CORE_S / 2 + depth / 2), z0 + h / 2) * Box(w, depth, h)
    if side == "back":  return Pos(c, CORE_S / 2 + depth / 2, z0 + h / 2) * Box(w, depth, h)
    if side == "right": return Pos(CORE_L / 2 + depth / 2, c, z0 + h / 2) * Box(depth, w, h)
    return Pos(-(CORE_L / 2 + depth / 2), c, z0 + h / 2) * Box(depth, w, h)

WINDOWS = [("front", FRONT), ("right", RIGHT), ("back", BACK), ("left", LEFT)]

parts, z = {}, 0.0
for i, t in enumerate(THICK):
    s = plate(BASE_L - i * STEP_IN, BASE_S - i * STEP_IN, t, 6) \
        - Pos(0, 0, -1) * extrude(Rectangle(CORE_L + 0.4, CORE_S + 0.4), t + 2)
    for r in rods(t): s -= r
    s = Pos(0, 0, z) * s
    for side, win in WINDOWS: s -= cut(side, win)
    parts[f"stratum{i}"] = s
    z += t + GAP
cap = plate(TOP_L, TOP_S, CAP_T, 4)
for r in rods(CAP_T): cap -= r
parts["cap"] = Pos(0, 0, z - GAP) * cap     # sits straight on the tray top

# core tray: floor, four walls, four standoffs for M2.5 screws, the port windows, vent slots on every gap line
tray = Box(CORE_L, CORE_S, CORE_H, align=(Align.CENTER, Align.CENTER, Align.MIN)) \
     - Pos(0, 0, FLOOR) * Box(CAV_L, CAV_S, CORE_H, align=(Align.CENTER, Align.CENTER, Align.MIN))
for hx, hy in HOLES:
    post = Pos(hx, hy, FLOOR) * Cylinder(3.0, STANDOFF, align=(Align.CENTER, Align.CENTER, Align.MIN))
    tray += post
    tray -= Pos(hx, hy, FLOOR) * Cylinder(1.1, STANDOFF + 1, align=(Align.CENTER, Align.CENTER, Align.MIN))   # pilot hole for an M2.5 screw
for side, win in WINDOWS: tray -= cut(side, win)
zz = 0.0
for t in THICK:
    zz += t
    for k in (-20, -7, 7, 20):                       # left wall
        tray -= Pos(-CORE_L / 2, k, zz + GAP / 2) * Box(WALL * 3, 8, GAP)
    for x in (24, 38):                               # front wall, right of the port window
        tray -= Pos(x, -CORE_S / 2, zz + GAP / 2) * Box(8, WALL * 3, GAP)
    zz += GAP
parts["core_tray"] = tray

# fit test: a flat plate with a lip the size of the board and the four mounting holes. Print it first, drop the board on it.
fit = Box(CORE_L, CORE_S, 2.0, align=(Align.CENTER, Align.CENTER, Align.MIN))
fit += Pos(0, 0, 2.0) * (extrude(Rectangle(BOARD_L + 0.6, BOARD_S + 0.6), 1.0) - Pos(0, 0, -1) * extrude(Rectangle(BOARD_L - 1.4, BOARD_S - 1.4), 3.0))
for hx, hy in HOLES: fit -= Pos(hx, hy, 0) * Cylinder(1.4, 6, align=(Align.CENTER, Align.CENTER, Align.MIN))
parts["fit_test"] = fit

os.makedirs(OUT, exist_ok=True)
for name, p in parts.items():
    export_stl(p, os.path.join(OUT, f"{name}.stl"))
stack = [p for n, p in parts.items() if n != "fit_test"]
export_step(Compound(stack), os.path.join(OUT, "neo-pi.step"))

asm = Compound(stack)
for name, eye, up in (("front", (0, -1000, 0), (0, 0, 1)), ("plan", (0, 0, 1000), (0, 1, 0))):
    drw = ExportSVG(unit=Unit.MM, line_weight=0.25); drw.add_layer("v", line_weight=0.3)
    vis, _ = asm.project_to_viewport(eye, up); drw.add_shape(vis, layer="v")
    drw.write(os.path.join(OUT, f"neo-pi-{name}.svg"))

bb = asm.bounding_box()
print("PARTS", len(parts), "SIZE %.1f x %.1f x %.1f mm" % (bb.size.X, bb.size.Y, bb.size.Z))
assert CORE_L < TOP_L - 2 * 3 and CORE_S < TOP_S - 2 * 3, "cap must overhang the core"
assert ROD_X + ROD / 2 < TOP_L / 2 - 2 and ROD_Y + ROD / 2 < TOP_S / 2 - 2, "rods must stay inside the smallest stratum"
assert PCB_TOP + 16.5 + 1.0 <= CORE_H, "the tallest port (USB stack, 16.5 mm) must clear the cap"
assert abs(HOLES[1][0] - HOLES[0][0]) == 58.0 and abs(HOLES[2][1] - HOLES[0][1]) == 49.0, "Pi 4B holes are 58 x 49 mm apart"
