# SPDX-License-Identifier: CC-BY-NC-SA-4.0
# Copyright (c) 2026 Joshua Trommel. Hardware design, see docs/hardware/LICENSE-NOTICE.md
# Nimbus blueprint sheet for 3D printing, drawn from the real CAD: nimbus/nimbus-blueprint.svg and .pdf.
#   uv run --with build123d --with shapely python docs/hardware/nimbus_blueprint.py docs/hardware
# Reads nimbus/*.step and nimbus/info.json (both written by nimbus_cad.py, run that first). Each view is the exact outline of the
# tessellated parts projected along one axis (shapely unions the triangles), laid out with the overall dimensions, the 2.4 mm
# wall, the M3 screw positions, the vent, the print orientation and 256 x 256 mm bed fit of every part, and a parts list with
# print settings. Writes the PDF with rsvg-convert when it is installed. Text lives only on this sheet; the computer carries none.
import sys, os, json, math, shutil, subprocess
from build123d import *
from shapely.geometry import Polygon
from shapely.ops import unary_union

OUT = sys.argv[1] if len(sys.argv) > 1 else "."
DIR = os.path.join(OUT, "nimbus")
info = json.load(open(os.path.join(DIR, "info.json")))
names = ("front_shell", "back_cover")
meshes = {}
for n in names:
    v, t = import_step(os.path.join(DIR, n + ".step")).tessellate(0.06, 0.12)
    meshes[n] = ([(p.X, p.Y, p.Z) for p in v], t)
W, H = 1700.0, 1202.0
INK, SOFT, TERRA, PAPER, CREAM = "#1C1C1E", "#8A857C", "#B5502C", "#FAF8F4", "#EFEBE4"
K = 1.25                                    # drawing scale, px per mm
OW, OD, OH = info["overall"]
els = []

def outline(fn, parts=names):
    polys = []
    for n in parts:
        v, tris = meshes[n]
        for a, b, c in tris:
            p = Polygon([fn(*v[i]) for i in (a, b, c)])
            if p.area > 1e-6: polys.append(p)
    return unary_union(polys).buffer(0.05).buffer(-0.05)
def pathd(geom, ox, oy, k, cx, cy):
    d = []
    for p in (geom.geoms if hasattr(geom, "geoms") else [geom]):
        for ring in [p.exterior] + list(p.interiors):
            pts = list(ring.simplify(0.05).coords)
            d.append("M" + " L".join("%.1f %.1f" % (ox + (x - cx) * k, oy - (y - cy) * k) for x, y in pts) + "Z")
    return " ".join(d)
def text(x, y, s, size=13, anchor="start", fill=INK, weight=400):
    return '<text x="%.1f" y="%.1f" font-size="%d" text-anchor="%s" fill="%s" font-weight="%d" font-family="Helvetica, Arial, sans-serif">%s</text>' % (x, y, size, anchor, fill, weight, s)
def line(x1, y1, x2, y2, c=SOFT, w=0.7, dash=""):
    return '<line x1="%.1f" y1="%.1f" x2="%.1f" y2="%.1f" stroke="%s" stroke-width="%s" %s/>' % (x1, y1, x2, y2, c, w, ('stroke-dasharray="%s"' % dash) if dash else "")
def hdim(x1, x2, y, label, off=24):
    yo = y + off
    return "".join([line(x1, y, x1, yo + 4), line(x2, y, x2, yo + 4), line(x1, yo, x2, yo, INK, 0.9), line(x1 - 4, yo + 4, x1 + 4, yo - 4, INK, 0.9), line(x2 - 4, yo + 4, x2 + 4, yo - 4, INK, 0.9), text((x1 + x2) / 2, yo - 6, label, 13, "middle")])
def vdim(x, y1, y2, label, off=24):
    xo = x + off
    return "".join([line(x, y1, xo + 4, y1), line(x, y2, xo + 4, y2), line(xo, y1, xo, y2, INK, 0.9), line(xo - 4, y1 + 4, xo + 4, y1 - 4, INK, 0.9), line(xo - 4, y2 + 4, xo + 4, y2 - 4, INK, 0.9),
                    '<text x="%.1f" y="%.1f" font-size="13" fill="%s" text-anchor="middle" transform="rotate(-90 %.1f %.1f)" font-family="Helvetica, Arial, sans-serif">%s</text>' % (xo - 6, (y1 + y2) / 2, INK, xo - 6, (y1 + y2) / 2, label)])
def shape(g, box, k=K, fill=CREAM, w=1.1):
    x0, y0, cx, cy = box
    return '<path d="%s" fill="%s" stroke="%s" stroke-width="%s" stroke-linejoin="round" fill-rule="evenodd"/>' % (pathd(g, x0, y0, k, cx, cy), fill, INK, w)

els += [text(56, 78, "Nimbus", 40, weight=600), text(56, 104, "A small square computer. Blueprint for 3D printing, drawn from the CAD (nimbus_cad.py). Dimensions in mm.", 15, fill=SOFT), line(56, 122, W - 56, 122, INK, 1.2)]
views = {"front": lambda x, y, z: (x, z), "side": lambda x, y, z: (y, z), "rear": lambda x, y, z: (-x, z), "top": lambda x, y, z: (x, y), "under": lambda x, y, z: (x, -y)}
sil = {k: outline(f) for k, f in views.items()}
def place(name, x0, y0):
    g = sil[name]; bx0, by0, bx1, by1 = g.bounds; w, h = (bx1 - bx0) * K, (by1 - by0) * K
    return g, (x0 + w / 2, y0 + h / 2, (bx0 + bx1) / 2, (by0 + by1) / 2), w, h
def at(box, u, v, k=K): return box[0] + (u - box[2]) * k, box[1] - (v - box[3]) * k

# ---- front, side, rear ----
gx = info["glass_center"]; sx0, sx1, sz0, sz1 = info["strip"]
g, box, w, h = place("front", 56, 150); els.append(text(56, 140, "FRONT", 14, weight=600)); els.append(shape(g, box))
ox, oy = at(box, gx[0], gx[2]); ow, oh = info["opening"][0] * K, info["opening"][1] * K
els.append('<rect x="%.1f" y="%.1f" width="%.1f" height="%.1f" rx="%.1f" fill="%s" fill-opacity="0.92"/>' % (ox - ow / 2, oy - oh / 2, ow, oh, info["radius"] * K, INK))
els.append('<rect x="%.1f" y="%.1f" width="%.1f" height="%.1f" fill="none" stroke="%s" stroke-width="0.8"/>' % (ox - info["active"][0] * K / 2, oy - info["active"][1] * K / 2, info["active"][0] * K, info["active"][1] * K, PAPER))
els.append(text(ox, oy + 4, "%.0f x %.0f active" % (info["active"][0], info["active"][1]), 11, "middle", PAPER))
els.append(hdim(box[0] - w / 2, box[0] + w / 2, 150 + h, "%.1f" % OW)); els.append(vdim(box[0] + w / 2, 150, 150 + h, "%.1f" % OH))
els.append(text(56, 150 + h + 58, "front %.0f x %.0f, squircle n = %.1f, wall %.1f" % (info["front"][0], info["front"][1], info["squircle_n"], info["wall"]), 12, fill=SOFT))
sidex = 56 + w + 110
g, box, w2, h2 = place("side", sidex, 150); els.append(text(sidex, 140, "SIDE", 14, weight=600)); els.append(shape(g, box))
els.append(hdim(box[0] - w2 / 2, box[0] + w2 / 2, 150 + h2, "%.1f" % OD)); els.append(text(sidex, 150 + h2 + 58, "leans back %.0f degrees" % info["lean"], 12, fill=SOFT))
rx = sidex + w2 + 110
g, box, w3, h3 = place("rear", rx, 150); els.append(text(rx, 140, "REAR", 14, weight=600)); els.append(shape(g, box))
a, b = at(box, -sx1, sz1); c, d = at(box, -sx0, sz0)
els.append('<rect x="%.1f" y="%.1f" width="%.1f" height="%.1f" rx="6" fill="none" stroke="%s" stroke-width="1"/>' % (a, b, c - a, d - b, INK))
els.append(text(rx, 150 + h3 + 58, "one slim strip: two USB-C and a round jack", 12, fill=SOFT))
# ---- top and underside ----
row2 = 150 + h + 130
g, box, w4, h4 = place("top", 56, row2); els.append(text(56, row2 - 10, "TOP", 14, weight=600)); els.append(shape(g, box))
cx_, cy_ = at(box, info["scoop_center"][0], info["scoop_center"][1]); hx, hz = info["scoop_half"]
els.append('<ellipse cx="%.1f" cy="%.1f" rx="%.1f" ry="%.1f" fill="none" stroke="%s" stroke-width="1" stroke-dasharray="5 3"/>' % (cx_, cy_, hx * K, hz * K, INK))
els.append(vdim(box[0] + w4 / 2, row2, row2 + h4, "%.1f" % OD)); els.append(text(56, row2 + h4 + 24, "finger pull: a small crescent in the back face, wall kept at %.1f" % info["wall"], 12, fill=SOFT))
ux = 56 + w4 + 110
g, box, w5, h5 = place("under", ux, row2); els.append(text(ux, row2 - 10, "UNDERSIDE", 14, weight=600)); els.append(shape(g, box))
for sx, sy in info["screws_m3"]:
    px, py = at(box, sx, -sy)
    els.append('<circle cx="%.1f" cy="%.1f" r="%.1f" fill="none" stroke="%s" stroke-width="1.3"/>' % (px, py, 1.7 * K + 2, INK)); els.append(line(px - 10, py, px + 10, py, INK, 0.6)); els.append(line(px, py - 10, px, py + 10, INK, 0.6))
    els.append(text(px, py + 24, "M3 (%.0f, %.0f)" % (sx, sy), 11, "middle"))
vx, vy = at(box, 0, -info["vent"]["y"]); vl, vw = info["vent"]["length"] * K, info["vent"]["width"] * K
els.append('<rect x="%.1f" y="%.1f" width="%.1f" height="%.1f" rx="%.1f" fill="%s" stroke="%s" stroke-width="1"/>' % (vx - vl / 2, vy - vw / 2, vl, vw, vw / 2, INK, INK)); els.append(text(vx, vy - 10, "vent slot %.0f x %.1f" % (info["vent"]["length"], info["vent"]["width"]), 11, "middle"))
els.append(text(ux, row2 + h5 + 24, "screws go up from underneath; the flat rear pad is the foot", 12, fill=SOFT))
# ---- exploded view: oblique, shells pulled apart ----
def obl(dy): return lambda x, y, z: (x + 0.5 * (y + dy), z + 0.28 * (y + dy))
gf, gb = outline(obl(-70), ("front_shell",)), outline(obl(60), ("back_cover",))
u0 = unary_union([gf, gb]).bounds; ek = 0.8
ex0 = W - 56 - (u0[2] - u0[0]) * ek
bx = (ex0 + (u0[2] - u0[0]) * ek / 2, row2 + (u0[3] - u0[1]) * ek / 2, (u0[0] + u0[2]) / 2, (u0[1] + u0[3]) / 2)
els.append(text(ex0, row2 - 10, "EXPLODED", 14, weight=600)); els.append(shape(gb, bx, ek)); els.append(shape(gf, bx, ek))
els.append(text(ex0, row2 + (u0[3] - u0[1]) * ek + 24, "front shell and back cover, apart", 12, fill=SOFT))
# ---- parts and print settings ----
row3 = row2 + 340
els += [line(56, row3 - 30, W - 56, row3 - 30, INK, 1.2), text(56, row3, "PARTS AND PRINT SETTINGS", 14, weight=600)]
BEDK = 0.42
cards = [("front_shell", "Front shell", "face down on the bed", "no supports", "teardrop standoffs and the screw holes bridge"),
         ("back_cover", "Back cover", "back panel down on the bed", "no supports", "the port windows sit in the first layers"),
         ("ground_inlay", "Mark ground inlay", "flat, one colour change", "no supports", "a 0.5 mm filament-swap line")]
for i, (key, title, orient, sup, note) in enumerate(cards):
    x0 = 56 + i * 335; y0 = row3 + 24; sz = info["parts"][key]; bw = info["bed"] * BEDK; pw, ph = sz[0] * BEDK, sz[1] * BEDK
    els.append('<rect x="%.1f" y="%.1f" width="%.1f" height="%.1f" fill="none" stroke="%s" stroke-width="0.8" stroke-dasharray="4 3"/>' % (x0, y0, bw, bw, SOFT))
    ink = TERRA if key == "ground_inlay" else INK
    els.append('<rect x="%.1f" y="%.1f" width="%.1f" height="%.1f" rx="%.1f" fill="%s" fill-opacity="%s" stroke="%s" stroke-width="1.2"/>' % (x0 + (bw - pw) / 2, y0 + (bw - ph) / 2, pw, ph, 8 if key != "ground_inlay" else 1, CREAM if key != "ground_inlay" else TERRA, 1 if key != "ground_inlay" else 0.3, ink))
    els.append(text(x0 + 6, y0 + bw - 8, "bed %d x %d" % (info["bed"], info["bed"]), 11, fill=SOFT)); els.append(text(x0 + bw + 14, y0 + 16, title, 15, weight=600))
    for j, ln in enumerate(["%.1f x %.1f x %.1f mm" % tuple(sz), orient, sup, "0.2 mm layers, 6 walls (2.4 mm)", "15 percent gyroid infill", "PLA matte #EFEBE4" if key != "ground_inlay" else "PLA matte terracotta #B5502C", note]):
        els.append(text(x0 + bw + 14, y0 + 40 + j * 20, ln, 12, fill=INK if j < 3 else SOFT))
els.append(text(1090, row3 + 40, "Fasteners and fit", 15, weight=600))
facts = ["2 x M3 x 12 self-tapping from underneath, at %s" % ", ".join("(%.0f, %.0f)" % tuple(p) for p in info["screws_m3"]),
         "4 x M2.5 x 6 for the Pi 4 on teardrop standoffs, board 26 mm off the floor",
         "wall %.1f mm everywhere; one %.1f mm reveal, half-lap joint" % (info["wall"], info["reveal"]),
         "display (stand-in 16:9 panel): %.0f x %.0f glass, %.0f x %.0f active, glued to a 2 mm ledge" % (info["glass"][0], info["glass"][1], info["active"][0], info["active"][1]),
         "the display numbers are stand-ins: check them with calipers before printing",
         "the front leans %d degrees back; the flat rear pad is the foot, so the chin hovers" % info["lean"]]
for j, ln in enumerate(facts): els.append(text(1090, row3 + 66 + j * 22, ln, 12, fill=INK if j < 3 else SOFT))
els.append(text(W - 56, H - 30, "Joshua Tree OS, hardware, CC BY-NC-SA 4.0", 11, "end", SOFT))
svg = '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 %d %d" width="%d" height="%d"><rect width="100%%" height="100%%" fill="%s"/>%s</svg>' % (W, H, W, H, PAPER, "".join(els))
svg_path = os.path.join(DIR, "nimbus-blueprint.svg"); open(svg_path, "w").write(svg)
if shutil.which("rsvg-convert"):
    subprocess.run(["rsvg-convert", "-f", "pdf", "-o", os.path.join(DIR, "nimbus-blueprint.pdf"), svg_path], check=True)
    if os.environ.get("NIMBUS_BLUEPRINT_PNG"): subprocess.run(["rsvg-convert", "-w", "1700", "-o", os.environ["NIMBUS_BLUEPRINT_PNG"], svg_path], check=True)
else:
    print("pdf skipped: rsvg-convert not installed (brew install librsvg)")
print("ok")
