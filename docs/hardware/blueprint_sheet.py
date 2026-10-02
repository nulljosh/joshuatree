# Dimensioned blueprint sheet for Strata: section (front) + plan, drawn from strata_cad.py's own numbers.
import sys, os
src = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "strata_cad.py")).read()
exec(src[src.index("BOARD ="):src.index("def rounded_plate")])  # ponytail: one source of truth for dimensions
OUT = sys.argv[1] if len(sys.argv) > 1 else "strata-blueprint.svg"

K = 2.4                  # px per mm
INK, SOFT, ACC = "#1a1814", "#8a8378", "#b9542c"
el = []
def line(x1, y1, x2, y2, c=INK, w=1.2, dash=""):
    el.append(f'<line x1="{x1:.1f}" y1="{y1:.1f}" x2="{x2:.1f}" y2="{y2:.1f}" stroke="{c}" stroke-width="{w}" {dash}/>')
def rect(x, y, w, h, c=INK, fill="none", sw=1.2, dash="", r=0):
    el.append(f'<rect x="{x:.1f}" y="{y:.1f}" width="{w:.1f}" height="{h:.1f}" rx="{r}" fill="{fill}" stroke="{c}" stroke-width="{sw}" {dash}/>')
def text(x, y, s, size=12, c=INK, anchor="middle", weight=400):
    el.append(f'<text x="{x:.1f}" y="{y:.1f}" font-size="{size}" fill="{c}" text-anchor="{anchor}" font-weight="{weight}">{s}</text>')
def dim_h(x1, x2, y, label):
    line(x1, y, x2, y, SOFT, 0.8); line(x1, y - 5, x1, y + 5, SOFT, 0.8); line(x2, y - 5, x2, y + 5, SOFT, 0.8)
    text((x1 + x2) / 2, y - 6, label, 11, SOFT)
def dim_v(x, y1, y2, label):
    line(x, y1, x, y2, SOFT, 0.8); line(x - 5, y1, x + 5, y1, SOFT, 0.8); line(x - 5, y2, x + 5, y2, SOFT, 0.8)
    text(x + 8, (y1 + y2) / 2 + 4, label, 11, SOFT, "start")
DASH = 'stroke-dasharray="5 4"'

W, H = 1500, 1080
tones = ["#B9542C", "#C9744A", "#D8A27A", "#E4C9A8", "#ECDDC6", "#F0E7D8"]

# section A: front elevation, section cut through the middle
cx, base = 400, 380
text(cx, 70, "SECTION A  front, cut at centre", 14, INK, weight=600)
z = 0.0
for i, t in enumerate(THICK):
    w = BASE_W - i * STEP_IN
    y = base - (z + t) * K
    rect(cx - w / 2 * K, y, (w - CORE) / 2 * K, t * K, INK, tones[i], 1)
    rect(cx + CORE / 2 * K, y, (w - CORE) / 2 * K, t * K, INK, tones[i], 1)
    text(cx + w / 2 * K + 10, y + t * K / 2 + 4, f"S{i}  {t:g}", 10, SOFT, "start")
    z += t + GAP
top_w = BASE_W - len(THICK) * STEP_IN
rect(cx - top_w / 2 * K, base - (z + CAP_T) * K, top_w * K, CAP_T * K, INK, "#F4EEE3", 1.2)
text(cx, base - (z + CAP_T) * K - 8, "cap, tree mark laser engraved 0.5 deep", 10, SOFT)
# core tray walls + floor, board, I/O window
rect(cx - CORE / 2 * K, base - CORE_H * K, WALL * K, CORE_H * K, INK, "#2b2622", 0.6)
rect(cx + (CORE / 2 - WALL) * K, base - CORE_H * K, WALL * K, CORE_H * K, INK, "#2b2622", 0.6)
rect(cx - CORE / 2 * K, base - WALL * K, CORE * K, WALL * K, INK, "#2b2622", 0.6)
rect(cx - BOARD / 2 * K, base - (WALL + 6 + 1.6) * K, BOARD * K, 1.6 * K, ACC, "none", 1.2)
text(cx, base - (WALL + 6 + 1.6) * K - 6, "mini-ITX board on 6 mm standoffs", 10, ACC)
rect(cx - IO_W / 2 * K, base - (IO_Z + IO_H / 2) * K, IO_W * K, IO_H * K, ACC, "none", 1, DASH)
text(cx, base - IO_Z * K + 4, "rear I/O window beyond", 10, ACC)
dim_h(cx - BASE_W / 2 * K, cx + BASE_W / 2 * K, base + 30, f"{BASE_W:g}")
dim_h(cx - top_w / 2 * K, cx + top_w / 2 * K, base - (z + CAP_T) * K - 34, f"{top_w:g}")
dim_v(cx - BASE_W / 2 * K - 56, base, base - (z + CAP_T) * K, f"{z + CAP_T:g}")
dim_v(cx - BASE_W / 2 * K - 22, base, base - CORE_H * K, "")
text(cx - BASE_W / 2 * K - 22, base + 16, f"core {CORE_H:g}", 10, SOFT)
text(cx, base + 62, f"gaps {GAP:g} mm = vent lines, set by 2 mm bosses printed into each ring", 11, INK)

# plan view
px, py = 400, 740
text(px, 500, "PLAN  from above, cap removed", 14, INK, weight=600)
rect(px - BASE_W / 2 * K, py - BASE_W / 2 * K, BASE_W * K, BASE_W * K, INK, tones[0], 1.2, r=6 * K)
rect(px - top_w / 2 * K, py - top_w / 2 * K, top_w * K, top_w * K, INK, tones[5], 1, r=4 * K)
rect(px - CORE / 2 * K, py - CORE / 2 * K, CORE * K, CORE * K, INK, "#2b2622", 1)
rect(px - BOARD / 2 * K, py - BOARD / 2 * K, BOARD * K, BOARD * K, ACC, "#3a332d", 1.2, DASH)
for hx, hy in HOLES:
    el.append(f'<circle cx="{px + hx * K:.1f}" cy="{py - hy * K:.1f}" r="{1.7 * K:.1f}" fill="none" stroke="{ACC}" stroke-width="1.2"/>')
for sx in (-1, 1):
    for sy in (-1, 1):
        el.append(f'<circle cx="{px + sx * ROD_AT * K:.1f}" cy="{py + sy * ROD_AT * K:.1f}" r="{ROD / 2 * K:.1f}" fill="#fff" stroke="{INK}" stroke-width="1.2"/>')
rect(px - IO_W / 2 * K, py - (CORE / 2 + 2) * K, IO_W * K, 4 * K, ACC, ACC, 1)
text(px, py - (CORE / 2 + 6) * K, "rear I/O", 10, ACC)
text(px, py + 4, f"board {BOARD:g}  cavity {CAV:g}  core {CORE:g}", 11, "#efe8dc")
dim_h(px - BASE_W / 2 * K, px + BASE_W / 2 * K, py + BASE_W / 2 * K + 26, f"{BASE_W:g}")
text(px + BASE_W / 2 * K + 12, py + ROD_AT * K + 4, f"M3 rod x4, {ROD_AT:g} from centre", 10, SOFT, "start")

# notes column
nx = 900
notes = [
    ("STRATA  Joshua Tree dev kit enclosure", 16, 600),
    ("all dimensions mm, first article", 11, 400), ("", 8, 400),
    ("PARTS", 13, 600),
    (f"S0 to S5  six rings, {BASE_W:g} down to {BASE_W - 5 * STEP_IN:g}, each {STEP_IN:g} smaller", 11, 400),
    (f"thickness {' / '.join(f'{t:g}' for t in THICK)}", 11, 400),
    (f"cap  {top_w:g} square, {CAP_T:g} thick, mark engraved", 11, 400),
    (f"core tray  {CORE:g} square, {CORE_H:g} tall, {WALL:g} walls", 11, 400),
    ("vent slots in the tray line up with every gap", 11, 400),
    ("4 x M3 threaded rod, 8 nuts, bosses printed in", 11, 400),
    ("4 printed board standoffs, 4 x M3x8 screws", 11, 400), ("", 8, 400),
    ("MAKE IT", 13, 600),
    ("1  print the rings: SLS nylon or FDM PETG, 0.2 mm", 11, 400),
    ("2  colour: dye or paint per ring, base to cap", 11, 400),
    ("    terracotta B9542C to cream F0E7D8", 11, 400),
    ("3  tray: bent 1.5 mm aluminium, or printed", 11, 400),
    ("4  laser engrave the tree on the cap", 11, 400),
    ("5  stack: tray, rods, then ring over ring", 11, 400),
    ("6  board in, stock I/O shield, cap, nuts", 11, 400), ("", 8, 400),
    ("CHECK BEFORE CUTTING", 13, 600),
    ("board hole spacing 154.94 x 157.48 (mini-ITX spec)", 11, 400),
    ("tallest part on the board vs the 62 mm tray", 11, 400),
    ("power input type from the board manual", 11, 400),
    ("I/O window fits the stock 158.75 x 44.45 shield", 11, 400), ("", 8, 400),
    ("SOURCE", 13, 600),
    ("docs/hardware/strata_cad.py makes the STEP and STLs", 11, 400),
    ("this sheet reads the same numbers", 11, 400),
]
y = 90
for s, size, wt in notes:
    if s: text(nx, y, s, size, INK, "start", wt)
    y += size + 11

svg = (f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" viewBox="0 0 {W} {H}" '
       f'font-family="-apple-system, Helvetica, Arial, sans-serif">'
       f'<rect width="{W}" height="{H}" fill="#f7f3ec"/>'
       f'<rect x="20" y="20" width="{W - 40}" height="{H - 40}" fill="none" stroke="{INK}" stroke-width="1.5"/>'
       + "".join(el) + "</svg>")
open(OUT, "w").write(svg)
print("SHEET", OUT)
