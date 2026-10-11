# Samantha box: a desk unit with a 7 inch HDMI screen, a Raspberry Pi 4B, a round speaker and a mic hole.
# build123d -> STEP (whole thing) + one STL per part. mm throughout.
# Run:  python3 samantha_cad.py OUTDIR    (needs build123d)
# Every part number below is a stand-in until the parts are in hand: measure the real screen, board and speaker
# with calipers, edit the block marked PARTS and rebuild. The asserts stop the build when a change stops fitting.
# Axes: X is width, Y is up the screen, Z comes out of the screen toward you. The Pi numbers are neo_pi_cad.py's.
import sys, os
from build123d import *

OUT = sys.argv[1] if len(sys.argv) > 1 else "."

# ---- PARTS: replace with the real measurements ----
SCREEN_W, SCREEN_H, SCREEN_T = 164.9, 100.0, 5.0     # 7 inch HDMI panel outline (Waveshare 7inch HDMI LCD (C) class)
VIEW_W, VIEW_H = 154.2, 85.9                          # its lit area
VIEW_DY = 0.0                                         # lit area centre above the panel centre
BOARD_L, BOARD_S = 85.0, 56.0                         # Pi 4B
HOLES = [(-39.0, -24.5), (19.0, -24.5), (-39.0, 24.5), (19.0, 24.5)]   # 58 x 49, from the board centre
SPK_D, SPK_H = 40.0, 8.0                              # round speaker, outside diameter and height
MIC_D = 6.0                                           # acoustic hole for a small USB mic, in the top wall
# ---------------------------------------------------

CLR, WALL, FLOOR, STANDOFF, PCB_T = 0.5, 2.4, 2.4, 4.0, 1.4
PCB_TOP = FLOOR + STANDOFF + PCB_T                    # 7.8 above the outside of the back
LIP = 1.6                                             # the bezel's front lip around the lit area
BEZEL_T = SCREEN_T + LIP
DEPTH = 34.0                                          # whole box, front face to back face
SHELL_H = DEPTH - BEZEL_T
INNER_W, INNER_H = SCREEN_W + 2 * CLR, SCREEN_H + 2 * CLR
OUT_W, OUT_H = INNER_W + 2 * WALL, INNER_H + 2 * WALL
TOP_PORT = 16.5                                       # the USB and Ethernet stack, the tallest thing on the board

# Pi window ranges, tray style: (centre along the wall, width, z from, z to), copied from neo_pi_cad.py
FRONT_WIN = (-11.0, 52.0, PCB_TOP - 1.0, PCB_TOP + 8.0)    # USB-C power, two micro-HDMI, the 3.5 mm jack
RIGHT_WIN = (0.25, 53.5, PCB_TOP - 1.0, PCB_TOP + TOP_PORT)  # Ethernet and the four USB ports
PI_X = INNER_W / 2 - BOARD_L / 2 - CLR - 0.5                # the Pi sits against the right wall
PI_Y = -INNER_H / 2 + BOARD_S / 2 + CLR + 0.5               # and the bottom wall, so its ports meet the walls
SPK_X, SPK_Y = -INNER_W / 2 + SPK_D / 2 + 6, 0.0            # the speaker on the left, firing out of the back
SD_X, SD_Y = PI_X - BOARD_L / 2 - 8.0, PI_Y                 # the card slot's side: a gap left for fingers

def box(w, d, h):
    return Box(w, d, h, align=(Align.CENTER, Align.CENTER, Align.MIN))

def post(x, y, z, r, h):
    return Pos(x, y, z) * Cylinder(r, h, align=(Align.CENTER, Align.CENTER, Align.MIN))

CORNERS = [(sx * (OUT_W / 2 - 5.0), sy * (OUT_H / 2 - 5.0)) for sx in (-1, 1) for sy in (-1, 1)]

parts = {}

# back shell: floor and four walls, four screw bosses, Pi standoffs, speaker ring, windows, speaker grille
shell = box(OUT_W, OUT_H, SHELL_H) - Pos(0, 0, FLOOR) * box(INNER_W, INNER_H, SHELL_H)
for x, y in CORNERS:
    shell += post(x, y, FLOOR, 4.0, SHELL_H - FLOOR)
    shell -= post(x, y, FLOOR, 1.2, SHELL_H - FLOOR + 1)                 # pilot hole for an M3 screw
for hx, hy in HOLES:
    shell += post(PI_X + hx, PI_Y + hy, FLOOR, 3.0, STANDOFF)  # the board centre is PI_X, PI_Y
    shell -= post(PI_X + hx, PI_Y + hy, FLOOR, 1.1, STANDOFF + 1)
ring = post(SPK_X, SPK_Y, FLOOR, SPK_D / 2 + 2.0, SPK_H) - post(SPK_X, SPK_Y, FLOOR - 1, SPK_D / 2 + 0.2, SPK_H + 2)
shell += ring
for r in (6.0, 12.0, 18.0):                                              # a ring-and-spokes grille through the floor
    for k in range(12):
        a = k * 30 + (15 if r == 12.0 else 0)
        shell -= Pos(SPK_X, SPK_Y, -1) * Rot(0, 0, a) * Pos(r, 0, 0) * box(5.0, 2.2, FLOOR + 2)
c, w, z0, z1 = FRONT_WIN
shell -= Pos(PI_X + c, -OUT_H / 2 - 10, z0 + (z1 - z0) / 2) * Box(w, 30, z1 - z0)           # bottom wall
c, w, z0, z1 = RIGHT_WIN
shell -= Pos(OUT_W / 2 + 10, PI_Y + c, z0 + (z1 - z0) / 2) * Box(30, w, z1 - z0)            # right wall
shell -= Pos(SD_X, SD_Y, -1) * box(20.0, 18.0, FLOOR + 2)                                   # the card, reached from the back
shell -= Pos(0, OUT_H / 2 + 10, PCB_TOP + 4) * Rot(90, 0, 0) * Cylinder(MIC_D / 2, 30)      # the mic hole, top wall
parts["back_shell"] = shell

# front bezel: a plate with a pocket for the screen on its back side and a window for the lit area
bez = box(OUT_W, OUT_H, BEZEL_T)
bez -= Pos(0, 0, -1) * box(SCREEN_W + 2 * CLR, SCREEN_H + 2 * CLR, SCREEN_T + 1)
bez -= Pos(0, VIEW_DY, -1) * box(VIEW_W, VIEW_H, BEZEL_T + 2)
for x, y in CORNERS:
    bez -= post(x, y, -1, 1.7, BEZEL_T + 2)                              # M3 clearance
parts["front_bezel"] = Pos(0, 0, DEPTH - BEZEL_T) * bez

# stand: a block with a slot cut at the tilt, so the box leans back. The box's bottom edge drops in.
TILT = 15.0
SL_W, SL_D = OUT_W - 40.0, DEPTH + 0.8
stand = box(OUT_W - 20.0, 50.0, 30.0)
cut = Pos(0, 0, 30.0 - 18.0) * Rot(-TILT, 0, 0) * box(SL_W, SL_D, 40.0)
stand -= cut
parts["stand"] = stand

# fit test: a flat frame the size of the screen. Print it first, lay the screen in it.
fit = box(OUT_W, OUT_H, 1.6) + Pos(0, 0, 1.6) * (box(SCREEN_W + 0.6, SCREEN_H + 0.6, 1.6) - Pos(0, 0, -1) * box(SCREEN_W - 3.4, SCREEN_H - 3.4, 4))
parts["fit_test"] = fit

os.makedirs(OUT, exist_ok=True)
for name, p in parts.items():
    export_stl(p, os.path.join(OUT, f"{name}.stl"))
asm = Compound([parts["back_shell"], parts["front_bezel"]])
export_step(asm, os.path.join(OUT, "samantha-box.step"))

bb = asm.bounding_box()
print("PARTS", len(parts), "BOX %.1f x %.1f x %.1f mm" % (bb.size.X, bb.size.Y, bb.size.Z))
assert abs(bb.size.Z - DEPTH) < 0.01, "shell and bezel must stack to the box depth"
assert SHELL_H - FLOOR >= STANDOFF + PCB_T + TOP_PORT + 1.0, "the tallest port must clear the screen's back"
assert PI_X + BOARD_L / 2 + CLR + 0.5 <= INNER_W / 2 + 0.001, "the Pi must sit inside the right wall"
assert SPK_X + SPK_D / 2 + 2.0 < SD_X - 10.0, "the speaker ring must leave the card a 10 mm channel"
assert SPK_Y + SPK_D / 2 + 2.0 <= INNER_H / 2, "the speaker must fit the height"
assert cut.bounding_box().min.Z >= 4.0, "the stand slot must leave 4 mm of floor under it"
