# SPDX-License-Identifier: CC-BY-NC-SA-4.0
# Copyright (c) 2026 Joshua Trommel. Hardware design, see docs/hardware/LICENSE-NOTICE.md
# Samantha box: a desk unit with a 7 inch HDMI screen, a Raspberry Pi 4B, a round speaker and a mic hole.
# build123d -> one STL per part, plus a STEP of the box itself (shell and bezel). mm throughout.
# Run:  uv run --with build123d python3 docs/hardware/samantha_cad.py OUTDIR
# Every part number below is a stand-in until the parts are in hand: measure the real screen and speaker with
# calipers, edit the block marked PARTS and rebuild. The Pi numbers are pi4b.py's, shared with nimbus_cad.py.
# The asserts run before anything is written, so a change that stops fitting leaves no parts on disk.
# Axes: X is width, Y is up the screen, Z comes out of the screen toward you. The box's back face is z = 0.
import sys, os, math
from build123d import *
from pi4b import BOARD_L, BOARD_S, PCB_T, STANDOFF, TOP_PORT, HOLES, FRONT_WIN, RIGHT_WIN

OUT = sys.argv[1] if len(sys.argv) > 1 else "."

# ---- PARTS: replace with the real measurements ----
SCREEN_W, SCREEN_H, SCREEN_T = 164.9, 100.0, 5.0     # 7 inch HDMI panel outline (Waveshare 7inch HDMI LCD (C) class)
VIEW_W, VIEW_H = 154.2, 85.9                          # its lit area
VIEW_DY = 0.0                                         # lit area centre above the panel centre
SPK_D, SPK_H = 40.0, 8.0                              # round speaker, outside diameter and height
MIC_D = 6.0                                           # acoustic hole for a small USB mic, in the top wall
PLUG_H = 12.0                                         # a right-angle USB-C power plug: how far it stands off the bottom wall
# ---------------------------------------------------

CLR, WALL, FLOOR = 0.5, 2.4, 2.4
PCB_TOP = FLOOR + STANDOFF + PCB_T                    # 7.8 above the outside of the back
LIP = 1.6                                             # the bezel's front lip around the lit area
BEZEL_T = SCREEN_T + LIP
DEPTH = 34.0                                          # whole box, front face to back face
SHELL_H = DEPTH - BEZEL_T
POCKET_W, POCKET_H = SCREEN_W + 2 * CLR, SCREEN_H + 2 * CLR     # the screen's pocket, in the bezel and in the fit test
BOSS_R, PILOT_R, M3_R = 4.0, 1.2, 1.7                 # screw bosses: outside radius, M3 pilot hole, M3 clearance in the bezel
HEAD_R, HEAD_T = 3.1, 3.2                             # a recess for each M3 socket head, so it sits below the bezel's face
RIM = 2 * BOSS_R + 1.0                              # a band above and below the screen for the bosses, so no screw lands on the panel
INNER_W, INNER_H = POCKET_W, POCKET_H + 2 * RIM       # the shell's cavity
OUT_W, OUT_H = INNER_W + 2 * WALL, INNER_H + 2 * WALL

PI_X = INNER_W / 2 - BOARD_L / 2 - CLR - 0.5          # the Pi sits against the right wall
PI_Y = -INNER_H / 2 + BOARD_S / 2 + CLR + 0.5         # and the bottom wall, so its ports meet the walls
SPK_X, SPK_Y = -INNER_W / 2 + SPK_D / 2 + 6, 0.0      # the speaker on the left, firing out of the back
SD_X, SD_Y = PI_X - BOARD_L / 2 - 8.0, PI_Y           # the card slot's side: a gap left for fingers

# Screw bosses in the rims, each touching its walls. The Pi fills the bottom right corner, so the fourth boss
# sits on the bottom rim to the Pi's left. The asserts below catch a Pi, port or screen change that reaches one.
BX, BY = INNER_W / 2 - BOSS_R, INNER_H / 2 - BOSS_R
BOSS4_X = -10.0
BOSSES = [(-BX, BY), (BX, BY), (-BX, -BY), (BOSS4_X, -BY)]

def box(w, d, h):
    return Box(w, d, h, align=(Align.CENTER, Align.CENTER, Align.MIN))

def post(x, y, z, r, h):
    return Pos(x, y, z) * Cylinder(r, h, align=(Align.CENTER, Align.CENTER, Align.MIN))

def prism(rect, z0, z1):                              # a plan rectangle (x0, x1, y0, y1), from z0 up to z1
    x0, x1, y0, y1 = rect
    return Pos((x0 + x1) / 2, (y0 + y1) / 2, z0) * box(x1 - x0, y1 - y0, z1 - z0)

def window(win):                                      # pi4b.py measures a window's z from the top of the board
    c, w, z0, z1 = win
    return c, w, PCB_TOP + z0, PCB_TOP + z1

# The two port windows in plan: through the wall and 1 mm into the box, no further, so they cut nothing inside.
c, w, FZ0, FZ1 = window(FRONT_WIN)                    # USB-C power, two micro-HDMI and the jack, in the bottom wall
FRONT_RECT = (PI_X + c - w / 2, PI_X + c + w / 2, -OUT_H / 2 - 1, -INNER_H / 2 + 1)
c, w, RZ0, RZ1 = window(RIGHT_WIN)                    # Ethernet and the four USB ports, in the right wall
RIGHT_RECT = (INNER_W / 2 - 1, OUT_W / 2 + 1, PI_Y + c - w / 2, PI_Y + c + w / 2)
PI_RECT = (PI_X - BOARD_L / 2, PI_X + BOARD_L / 2, PI_Y - BOARD_S / 2, PI_Y + BOARD_S / 2)
POCKET_RECT = (-POCKET_W / 2, POCKET_W / 2, -POCKET_H / 2, POCKET_H / 2)
VIEW_RECT = (-VIEW_W / 2, VIEW_W / 2, VIEW_DY - VIEW_H / 2, VIEW_DY + VIEW_H / 2)
SD_RECT = (SD_X - 10.0, SD_X + 10.0, SD_Y - 9.0, SD_Y + 9.0)

# Speaker grille: rings of radial slots through the floor. Each ring is (radius, slots). Every bar of floor left
# between two slots, and between two rings, must be at least WEB_MIN wide, or the centre falls out.
SLOT_L, SLOT_W, WEB_MIN = 3.5, 2.0, 1.2
GRILLE = [(6.0, 6), (11.0, 12), (16.0, 24)]

def slot_web(r, n):                                   # floor between neighbouring slots, at their inner ends (the narrowest)
    a = math.pi / n
    return 2 * (r - SLOT_L / 2) * math.sin(a) - SLOT_W * math.cos(a)

def ring_web(r0, r1):                                 # floor between one ring's outer corners and the next ring's inner ends
    return (r1 - SLOT_L / 2) - math.hypot(r0 + SLOT_L / 2, SLOT_W / 2)

parts = {}

# back shell: floor and four walls, four screw bosses, Pi standoffs, speaker ring, windows, speaker grille
shell = box(OUT_W, OUT_H, SHELL_H) - Pos(0, 0, FLOOR) * box(INNER_W, INNER_H, SHELL_H)
for x, y in BOSSES:
    shell += post(x, y, FLOOR, BOSS_R, SHELL_H - FLOOR)
    shell -= post(x, y, FLOOR, PILOT_R, SHELL_H - FLOOR + 1)          # pilot hole for an M3 screw from the front
for hx, hy in HOLES:
    shell += post(PI_X + hx, PI_Y + hy, FLOOR, 3.0, STANDOFF)          # the board centre is PI_X, PI_Y
    shell -= post(PI_X + hx, PI_Y + hy, FLOOR, 1.1, STANDOFF + 1)
ring = post(SPK_X, SPK_Y, FLOOR, SPK_D / 2 + 2.0, SPK_H) - post(SPK_X, SPK_Y, FLOOR - 1, SPK_D / 2 + 0.2, SPK_H + 2)
shell += ring
for i, (r, n) in enumerate(GRILLE):
    for k in range(n):
        a = (k + 0.5 * (i % 2)) * 360 / n                               # every other ring turned half a slot
        shell -= Pos(SPK_X, SPK_Y, -1) * Rot(0, 0, a) * Pos(r, 0, 0) * box(SLOT_L, SLOT_W, FLOOR + 2)
shell -= prism(FRONT_RECT, FZ0, FZ1)
shell -= prism(RIGHT_RECT, RZ0, RZ1)
shell -= prism(SD_RECT, -1, FLOOR + 1)                                  # the card, reached from the back
shell -= Pos(0, (INNER_H + OUT_H) / 4, PCB_TOP + 4) * Rot(90, 0, 0) * Cylinder(MIC_D / 2, WALL + 2)   # the mic hole, top wall
parts["back_shell"] = shell

# front bezel: a plate with a pocket for the screen on its back side, a window for the lit area, and M3 clearance
# holes over the bosses, all in the rims outside the screen. The screw heads sit in recesses, flush or below the
# face, so the stand's front wall slides past them.
bez = box(OUT_W, OUT_H, BEZEL_T)
bez -= Pos(0, 0, -1) * box(POCKET_W, POCKET_H, SCREEN_T + 1)
bez -= Pos(0, VIEW_DY, -1) * box(VIEW_W, VIEW_H, BEZEL_T + 2)
for x, y in BOSSES:
    bez -= post(x, y, -1, M3_R, BEZEL_T + 2)
    bez -= post(x, y, BEZEL_T - HEAD_T, HEAD_R, HEAD_T + 1)
parts["front_bezel"] = Pos(0, 0, DEPTH - BEZEL_T) * bez

# stand: two cradles, one under each end of the box, with a slot cut at the tilt so the box leans back. The slot
# runs cheek to cheek with play, and the bottom wall stays open between the cradles, so the power lead reaches
# the Pi with the box in the stand.
TILT = 15.0
SLOT_CLR = 0.5                                        # play each side of the box in the slot
SL_W, SL_D = OUT_W + 2 * SLOT_CLR, DEPTH + 0.8        # the slot: cheek to cheek, front to back
SLOT_Z = 18.0                                         # the slot floor's centre above the desk
CRADLE_W, CHEEK, STAND_D, STAND_H = 24.0, 4.0, 50.0, 34.0   # each cradle's reach under the box, end wall, depth, height
slot = Pos(0, 0, SLOT_Z) * Rot(-TILT, 0, 0) * box(SL_W, SL_D, 60.0)
stand = None
for s in (-1, 1):
    x_out, x_in = s * (SL_W / 2 + CHEEK), s * (OUT_W / 2 - CRADLE_W)
    cradle = Pos((x_out + x_in) / 2, 0, 0) * box(abs(x_out - x_in), STAND_D, STAND_H)
    stand = cradle if stand is None else stand + cradle
stand -= slot
parts["stand"] = stand

# fit test: a thin tray with the bezel's screen pocket, SCREEN_T deep, on a ledge. The middle is open, because the
# panel's own connectors stick out of its back. Print it first and drop the screen in: it should sit flush with a
# hair of play, and the finger notch lifts it out.
FIT_RIM, FIT_LEDGE, FIT_FLOOR = 2.0, 3.0, 1.2
fit = box(POCKET_W + 2 * FIT_RIM, POCKET_H + 2 * FIT_RIM, FIT_FLOOR + SCREEN_T)
fit -= Pos(0, 0, FIT_FLOOR) * box(POCKET_W, POCKET_H, SCREEN_T + 1)
fit -= Pos(0, 0, -1) * box(POCKET_W - 2 * FIT_LEDGE, POCKET_H - 2 * FIT_LEDGE, FIT_FLOOR + 2)
fit -= post(0, POCKET_H / 2, -1, 9.0, FIT_FLOOR + SCREEN_T + 2)        # finger notch on the top edge
parts["fit_test"] = fit

asm = Compound([parts["back_shell"], parts["front_bezel"]])

# ---- checks: all of them run before anything is written ----
def clear_of(circle, rect, gap=0.5):                  # a plan circle (x, y, r) stays gap clear of a rectangle (x0, x1, y0, y1)
    x, y, r = circle
    x0, x1, y0, y1 = rect
    return math.hypot(max(x0 - x, 0, x - x1), max(y0 - y, 0, y - y1)) >= r + gap

def overlap(a, b):                                    # volume two solids share; 0 when they only touch
    return sum(s.volume for s in (a & b).solids())

bb = asm.bounding_box()
assert abs(bb.size.Z - DEPTH) < 0.01, "shell and bezel must stack to the box depth"
assert SHELL_H - FLOOR >= STANDOFF + PCB_T + TOP_PORT + 1.0, "the tallest port must clear the screen's back"
assert abs(HOLES[1][0] - HOLES[0][0]) == 58.0 and abs(HOLES[2][1] - HOLES[0][1]) == 49.0, "Pi 4B holes are 58 x 49 mm apart"
for x, y in BOSSES:
    for r in (M3_R, HEAD_R):
        assert clear_of((x, y, r), VIEW_RECT) and clear_of((x, y, r), POCKET_RECT), "a bezel screw hole lands on the screen"
    assert clear_of((x, y, BOSS_R), POCKET_RECT), "a screw boss sits under the screen"
    assert clear_of((x, y, BOSS_R), PI_RECT), "a screw boss runs into the Pi"
    assert clear_of((x, y, BOSS_R), FRONT_RECT) and clear_of((x, y, BOSS_R), RIGHT_RECT), "a port window cuts a screw boss open"
    assert clear_of((x, y, BOSS_R), SD_RECT), "a screw boss blocks the card hole"
    assert math.hypot(x - SPK_X, y - SPK_Y) >= SPK_D / 2 + 2.0 + BOSS_R + 0.5, "a screw boss runs into the speaker ring"
assert clear_of((SPK_X, SPK_Y, SPK_D / 2 + 2.0), PI_RECT), "the speaker ring runs into the Pi"
assert SPK_X + SPK_D / 2 + 2.0 < SD_X - 10.0, "the speaker ring must leave the card a 10 mm channel"
assert overlap(Pos(PI_X, PI_Y, FLOOR + STANDOFF) * box(BOARD_L, BOARD_S, PCB_T + TOP_PORT), shell) < 0.01, \
    "the Pi and its tallest port must fit the shell"
assert all(slot_web(r, n) >= WEB_MIN for r, n in GRILLE), "grille slots too close: the floor between them is too thin"
assert all(ring_web(GRILLE[i][0], GRILLE[i + 1][0]) >= WEB_MIN for i in range(len(GRILLE) - 1)), "grille rings too close"
assert math.hypot(GRILLE[-1][0] + SLOT_L / 2, SLOT_W / 2) <= SPK_D / 2 - 1.0, "the grille must stay under the speaker"
assert len(shell.solids()) == 1, "the back shell must print as one piece, nothing cut loose"
panel = box(SCREEN_W, SCREEN_H, SCREEN_T)             # the screen as the PARTS block has it
assert overlap(panel, bez) < 0.01 and overlap(Pos(0, 0, FIT_FLOOR) * panel, fit) < 0.01, \
    "the screen must drop into the bezel and the fit test"
assert abs(fit.bounding_box().max.Z - (FIT_FLOOR + SCREEN_T)) < 0.01, "the fit test pocket must be SCREEN_T deep"
# the box standing in the stand: its bottom wall on the slot floor, the screen facing forward (-Y), leaning back
PLACE = Pos(0, 0, SLOT_Z) * Rot(-TILT, 0, 0) * Pos(0, DEPTH / 2, OUT_H / 2) * Rot(90, 0, 0)
lit = Pos(0, VIEW_DY, DEPTH) * box(VIEW_W, VIEW_H, 300.0)              # everything straight in front of the lit area
plug = prism((FRONT_RECT[0], FRONT_RECT[1], -OUT_H / 2 - PLUG_H, -OUT_H / 2), FZ0, FZ1)   # the power plug under the box
assert slot.bounding_box().size.X > OUT_W + 2 * 0.4, "the slot must take the box's full width with play"
assert stand.bounding_box().size.X >= OUT_W, "the stand must be at least as wide as the box"
assert slot.bounding_box().min.Z >= 4.0, "the stand slot must leave 4 mm of cradle under it"
assert overlap(PLACE * asm, stand) < 0.01, "the box must drop into the stand"
assert overlap(PLACE * lit, stand) < 0.01, "the stand must not hide the screen"
assert overlap(PLACE * plug, stand) < 0.01 and (PLACE * plug).bounding_box().min.Z >= 0.0, \
    "the power plug must fit under the box, between the cradles and above the desk"

os.makedirs(OUT, exist_ok=True)
for name, p in parts.items():
    export_stl(p, os.path.join(OUT, f"{name}.stl"))
export_step(asm, os.path.join(OUT, "samantha-box.step"))
print("PARTS", len(parts), "BOX %.1f x %.1f x %.1f mm" % (bb.size.X, bb.size.Y, bb.size.Z))
