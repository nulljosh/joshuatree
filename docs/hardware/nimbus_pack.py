# SPDX-License-Identifier: CC-BY-NC-SA-4.0
# Copyright (c) 2026 Joshua Trommel. Hardware design, see docs/hardware/LICENSE-NOTICE.md
# The Nimbus print pack, built from the CAD: checks every STL is watertight, writes the 1:1 blueprint (A3 PDF, plus A4 and Letter tiles),
# the bill of materials, the per-part print sheet, six assembly diagrams and the one-page print-shop sheet into docs/hardware/nimbus/pack/.
#   NIMBUS_PACK=docs/hardware/nimbus/pack uv run --with build123d --with shapely python docs/hardware/nimbus_cad.py docs/hardware   (STLs and quarters)
#   uv run --with build123d --with shapely --with trimesh --with numpy python docs/hardware/nimbus_pack.py docs/hardware
# Every size comes from the CAD (nimbus/info.json, the STEP files and the STLs). Time, filament and cost are estimates and are marked so.
import sys, os, json, math, csv, glob, shutil, subprocess
import numpy as np
import trimesh
from build123d import *
from shapely.geometry import Polygon, box
from shapely.ops import unary_union

OUT = sys.argv[1] if len(sys.argv) > 1 else "."
DIR = os.path.join(OUT, "nimbus"); PACK = os.path.join(DIR, "pack")
info = json.load(open(os.path.join(DIR, "info.json")))
os.makedirs(os.path.join(PACK, "assembly"), exist_ok=True); os.makedirs(os.path.join(PACK, "sheets"), exist_ok=True)
INK, SOFT, TERRA, PAPER, CREAM = "#1C1C1E", "#8A857C", "#B5502C", "#FAF8F4", "#EFEBE4"

# ---- 0. quarters for a 220 mm bed: 45 degree scarf joints (print-orientation meshes sliced and capped), so each piece prints with no supports ----
#   dropped: the 220 mm quarters. Both shells measure under 256 x 256 whole (236 x 228), so the pack ships them whole for a 256 mm bed.
shutil.rmtree(os.path.join(PACK, "split-220"), ignore_errors=True)

stl = {}
for f in sorted(glob.glob(os.path.join(PACK, "*.stl")) + glob.glob(os.path.join(PACK, "split-220", "*.stl"))):
    m = trimesh.load(f); name = os.path.relpath(f, PACK)
    ok = bool(m.is_watertight and m.is_winding_consistent)
    stl[name] = {"watertight": ok, "volume_cm3": round(abs(m.volume) / 1000, 1) if ok else None, "size_mm": [round(v, 1) for v in m.extents], "mb": round(os.path.getsize(f) / 1e6, 2)}
    print("%-34s watertight=%s size=%s %.1f MB" % (name, ok, stl[name]["size_mm"], stl[name]["mb"]))
    assert stl[name]["mb"] < 5.0, name + " is over 5 MB"
json.dump(stl, open(os.path.join(PACK, "stl-check.json"), "w"), indent=1)
print("not watertight:", [k for k, v in stl.items() if not v["watertight"]])

# ---- 2. the 1:1 blueprint ----
meshes = {}
for n in ("front_shell", "back_cover"):
    v, t = import_step(os.path.join(DIR, n + ".step")).tessellate(0.06, 0.12)
    meshes[n] = (np.array([(p.X, p.Y, p.Z) for p in v]), np.array(t))
OW, OD, OH = info["overall"]
def tri_polys(fn, parts):
    out = []
    for n in parts:
        v, t = meshes[n]
        for a, b, c in t:
            p = Polygon([fn(*v[a]), fn(*v[b]), fn(*v[c])])
            if p.area > 1e-6: out.append(p)
    return unary_union(out).buffer(0.05).buffer(-0.05)
def section_segments(parts, x0=0.0):
    """the cut at x = x0: every triangle edge pair that crosses it, as (y, z) segments"""
    segs = []
    for n in parts:
        v, t = meshes[n]
        for a, b, c in t:
            pts = []
            for i, j in ((a, b), (b, c), (c, a)):
                d0, d1 = v[i][0] - x0, v[j][0] - x0
                if d0 * d1 < 0:
                    u = d0 / (d0 - d1); q = v[i] + (v[j] - v[i]) * u; pts.append((q[1], q[2]))
            if len(pts) == 2: segs.append((n, pts))
    return segs
views = {"front": lambda x, y, z: (x, z), "side": lambda x, y, z: (y, z), "rear": lambda x, y, z: (-x, z), "under": lambda x, y, z: (x, -y)}
sil = {k: tri_polys(f, ("front_shell", "back_cover")) for k, f in views.items()}
PW, PH = 420.0, 297.0
def T(x, y, s, size=3.2, anchor="start", fill=INK, weight=400): return '<text x="%.2f" y="%.2f" font-size="%.2f" text-anchor="%s" fill="%s" font-weight="%d" font-family="Helvetica, Arial, sans-serif">%s</text>' % (x, y, size, anchor, fill, weight, s)
def L(x1, y1, x2, y2, c=INK, w=0.25, dash=""): return '<line x1="%.2f" y1="%.2f" x2="%.2f" y2="%.2f" stroke="%s" stroke-width="%s" %s/>' % (x1, y1, x2, y2, c, w, ('stroke-dasharray="%s"' % dash) if dash else "")
def pathd(g, ox, oy, cx, cy):
    d = []
    for p in (g.geoms if hasattr(g, "geoms") else [g]):
        for r in [p.exterior] + list(p.interiors):
            d.append("M" + " L".join("%.2f %.2f" % (ox + x - cx, oy - (y - cy)) for x, y in r.simplify(0.04).coords) + "Z")
    return " ".join(d)
def hdim(x1, x2, y, label, off=9): yo = y + off; return L(x1, y, x1, yo + 1.5, SOFT) + L(x2, y, x2, yo + 1.5, SOFT) + L(x1, yo, x2, yo) + L(x1 - 1, yo + 1, x1 + 1, yo - 1) + L(x2 - 1, yo + 1, x2 + 1, yo - 1) + T((x1 + x2) / 2, yo - 1.5, label, 3.4, "middle")
def vdim(x, y1, y2, label, off=9):
    xo = x + off
    return L(x, y1, xo + 1.5, y1, SOFT) + L(x, y2, xo + 1.5, y2, SOFT) + L(xo, y1, xo, y2) + L(xo - 1, y1 + 1, xo + 1, y1 - 1) + L(xo - 1, y2 + 1, xo + 1, y2 - 1) + '<text x="%.2f" y="%.2f" font-size="3.4" text-anchor="middle" transform="rotate(-90 %.2f %.2f)" font-family="Helvetica, Arial, sans-serif" fill="%s">%s</text>' % (xo - 1.5, (y1 + y2) / 2, xo - 1.5, (y1 + y2) / 2, INK, label)
def frame(title, sub, body, n, scale_note="Scale 1:1 (print at 100 percent, do not fit to page)"):
    bar = L(20, 285, 120, 285, INK, 0.5) + "".join(L(20 + 10 * i, 283, 20 + 10 * i, 287, INK, 0.3) for i in range(11)) + T(20, 281.5, "0", 2.8) + T(120, 281.5, "100 mm", 2.8, "end")
    return ('<svg xmlns="http://www.w3.org/2000/svg" width="%gmm" height="%gmm" viewBox="0 0 %g %g"><rect width="%g" height="%g" fill="%s"/>' % (PW, PH, PW, PH, PW, PH, PAPER)
            + '<rect x="8" y="8" width="%g" height="%g" fill="none" stroke="%s" stroke-width="0.4"/>' % (PW - 16, PH - 16, INK)
            + T(14, 18, "Nimbus", 7, weight=600) + T(14, 25, title + ": " + sub, 3.6, fill=SOFT) + bar
            + T(PW - 14, 281.5, "Units: millimetres. General tolerance +/- 0.3 mm. " + scale_note, 2.8, "end", SOFT)
            + T(PW - 14, 286, "Sheet %d of 6. Joshua Tree OS hardware, CC BY-NC-SA 4.0. Drawn from the CAD (nimbus_cad.py)." % n, 2.8, "end", SOFT) + body + "</svg>")
def place(name, cx, cy):
    g = sil[name]; b = g.bounds; return g, (cx, cy, (b[0] + b[2]) / 2, (b[1] + b[3]) / 2)
def outline(g, box_, fill=CREAM): return '<path d="%s" fill="%s" stroke="%s" stroke-width="0.5" fill-rule="evenodd"/>' % (pathd(g, *box_), fill, INK)
def at(box_, u, v): return box_[0] + u - box_[2], box_[1] - (v - box_[3])
pages = []
# page 1 front
g, bx = place("front", PW / 2, PH / 2 - 4); b = g.bounds
body = outline(g, bx)
gx = info["glass_center"]; ox, oy = at(bx, gx[0], gx[2])
body += '<rect x="%.2f" y="%.2f" width="%.2f" height="%.2f" rx="%.1f" fill="%s" fill-opacity="0.9"/>' % (ox - info["opening"][0] / 2, oy - info["opening"][1] / 2, info["opening"][0], info["opening"][1], info["radius"], INK)
body += '<rect x="%.2f" y="%.2f" width="%.2f" height="%.2f" fill="none" stroke="%s" stroke-width="0.3"/>' % (ox - info["active"][0] / 2, oy - info["active"][1] / 2, info["active"][0], info["active"][1], PAPER) + T(ox, oy, "active area %.0f x %.0f" % (info["active"][0], info["active"][1]), 3.2, "middle", PAPER)
sd = info["sd_slot"]; sx_, sy_ = at(bx, sd[0], sd[1]); body += '<rect x="%.2f" y="%.2f" width="%.2f" height="%.2f" rx="%.2f" fill="%s"/>' % (sx_ - sd[2] / 2, sy_ - sd[3] / 2, sd[2], sd[3], sd[3] / 2, INK) + T(sx_, sy_ + 6, "SD slot %.0f x %.1f" % (sd[2], sd[3]), 2.8, "middle")
body += hdim(bx[0] - (b[2] - b[0]) / 2, bx[0] + (b[2] - b[0]) / 2, bx[1] + (b[3] - b[1]) / 2 + 1, "%.1f" % OW) + vdim(bx[0] + (b[2] - b[0]) / 2 + 1, bx[1] - (b[3] - b[1]) / 2, bx[1] + (b[3] - b[1]) / 2, "%.1f" % OH)
body += T(14, 276,"Front face is plain: the screen and the SD slot only. Outline: superellipse n = %.1f, %.0f x %.0f, one radius %.0f. Wall %.1f." % (info["squircle_n"], info["front"][0], info["front"][1], info["radius"], info["wall"]), 3.2)
pages.append(frame("Front", "elevation", body, 1))
# page 2 side
g, bx = place("side", PW / 2, PH / 2 - 4); b = g.bounds; body = outline(g, bx)
body += hdim(bx[0] - (b[2] - b[0]) / 2, bx[0] + (b[2] - b[0]) / 2, bx[1] + (b[3] - b[1]) / 2 + 1, "%.1f" % OD) + vdim(bx[0] + (b[2] - b[0]) / 2 + 1, bx[1] - (b[3] - b[1]) / 2, bx[1] + (b[3] - b[1]) / 2, "%.1f" % OH)
body += T(14, 276,"The face leans back %.0f degrees. One reveal of %.1f mm splits the front shell from the back cover. The flat rear foot is the only part on the desk." % (info["lean"], info["reveal"]), 3.2)
pages.append(frame("Side", "elevation", body, 2))
# page 3 rear
g, bx = place("rear", PW / 2, PH / 2 - 4); b = g.bounds; body = outline(g, bx)
a_, b_ = at(bx, -info["strip"][1], info["strip"][3]); c_, d_ = at(bx, -info["strip"][0], info["strip"][2])
body += '<rect x="%.2f" y="%.2f" width="%.2f" height="%.2f" rx="3.5" fill="none" stroke="%s" stroke-width="0.4"/>' % (a_, b_, c_ - a_, d_ - b_, INK) + T((a_ + c_) / 2, d_ + 6, "port strip: 2 x USB-C, 1 x 8 mm round jack", 2.8, "middle")
mk = info["mark_rear"]; mx_, my_ = at(bx, -mk[0], mk[1]); body += '<rect x="%.2f" y="%.2f" width="%.2f" height="%.2f" fill="none" stroke="%s" stroke-width="0.3" stroke-dasharray="1.5 1"/>' % (mx_ - 13, my_ - 15, 26, 30, TERRA) + T(mx_, my_ - 17, "tree mark, 0.6 deep, ground line inlay", 2.8, "middle")
fx_, fy_ = at(bx, 0, info["scoop_center"][2]); body += '<rect x="%.2f" y="%.2f" width="%.2f" height="%.2f" rx="5" fill="none" stroke="%s" stroke-width="0.4"/>' % (fx_ - info["pull"][0] / 2, fy_ - info["pull"][1] / 2, info["pull"][0], info["pull"][1], INK) + T(fx_, fy_ - 8, "finger scoop %.0f x %.0f x %.1f deep" % (info["pull"][0], info["pull"][1], info["pull"][2]), 2.8, "middle")
body += hdim(bx[0] - (b[2] - b[0]) / 2, bx[0] + (b[2] - b[0]) / 2, bx[1] + (b[3] - b[1]) / 2 + 1, "%.1f" % OW)
pages.append(frame("Rear", "elevation", body, 3))
# page 4 section at x = 0
segs = section_segments(("front_shell", "back_cover"))
ys = [p[0] for _, q in segs for p in q]; zs = [p[1] for _, q in segs for p in q]
cx_, cy_ = (min(ys) + max(ys)) / 2, (min(zs) + max(zs)) / 2; ox, oy = PW / 2, PH / 2 + 4
body = "".join(L(ox + q[0][0] - cx_, oy - (q[0][1] - cy_), ox + q[1][0] - cx_, oy - (q[1][1] - cy_), INK if n == "front_shell" else TERRA, 0.35) for n, q in segs)
body += hdim(ox + min(ys) - cx_, ox + max(ys) - cx_, oy + (max(zs) - min(zs)) / 2 + 1, "%.1f" % (max(ys) - min(ys)))
body += T(14, 262, "Section on the centre line (x = 0). Front shell in ink, back cover in terracotta. Walls %.1f. Half-lap at the reveal (%.1f); the cover's lip (1.1 clearance 0.2) sits inside the shell." % (info["wall"], info["reveal"]), 3.2)
body += T(14, 268, "Two M3 ribs and the Pi standoffs are off the centre line. The vent slot (%.0f x %.1f) is in the floor of the cover." % (info["vent"]["length"], info["vent"]["width"]), 3.2)
pages.append(frame("Section", "on the centre line", body, 4))
# page 5 underside
g, bx = place("under", PW / 2, PH / 2 - 4); b = g.bounds; body = outline(g, bx)
for sxx, syy in info["screws_m3"]:
    px, py = at(bx, sxx, -syy); body += '<circle cx="%.2f" cy="%.2f" r="1.7" fill="none" stroke="%s" stroke-width="0.4"/>' % (px, py, INK) + L(px - 5, py, px + 5, py, SOFT, 0.2) + L(px, py - 5, px, py + 5, SOFT, 0.2) + T(px, py + 9, "M3 clearance 3.4, (%.0f, %.0f)" % (sxx, syy), 2.8, "middle")
vx, vy = at(bx, 0, -info["vent"]["y"]); body += '<rect x="%.2f" y="%.2f" width="%.2f" height="%.2f" rx="1.6" fill="%s"/>' % (vx - info["vent"]["length"] / 2, vy - info["vent"]["width"] / 2, info["vent"]["length"], info["vent"]["width"], INK) + T(vx, vy - 4, "vent slot %.0f x %.1f" % (info["vent"]["length"], info["vent"]["width"]), 2.8, "middle")
body += T(14, 276,"Underside. The only screws are the two in the foot, from below. Counterbore 6.2 wide, 1.2 deep.", 3.2)
pages.append(frame("Underside", "plan view", body, 5))
# page 6 exploded at 1:2
def obl(dy): return lambda x, y, z: (x + 0.5 * (y + dy), z + 0.28 * (y + dy))
gf, gb = tri_polys(obl(-70), ("front_shell",)), tri_polys(obl(60), ("back_cover",)); ub = unary_union([gf, gb]).bounds
ek = 0.5; bx = (PW / 2, PH / 2 + 2, (ub[0] + ub[2]) / 2, (ub[1] + ub[3]) / 2)
body = '<path d="%s" fill="%s" stroke="%s" stroke-width="0.5" fill-rule="evenodd"/>' % (pathd(gb, bx[0], bx[1], bx[2], bx[3]).replace("", ""), CREAM, INK)
def scaled(g): return pathd(g, bx[0], bx[1], bx[2], bx[3])
from shapely import affinity
def sg(g): return affinity.scale(g, ek, ek, origin=((ub[0] + ub[2]) / 2, (ub[1] + ub[3]) / 2))
body = '<path d="%s" fill="%s" stroke="%s" stroke-width="0.5" fill-rule="evenodd"/><path d="%s" fill="%s" stroke="%s" stroke-width="0.5" fill-rule="evenodd"/>' % (scaled(sg(gb)), CREAM, INK, scaled(sg(gf)), CREAM, INK)
body += T(14, 262, "Scale 1:2. Parts: 1 front shell, 2 back cover, 3 tree ground inlay (in the back, not shown). Bought parts are on the bill of materials.", 3.2)
pages.append(frame("Exploded", "front shell and back cover apart", body, 6, "Scale 1:2"))
sheets = []
for i, pg in enumerate(pages, 1):
    f = os.path.join(PACK, "sheets", "sheet%d.svg" % i); open(f, "w").write(pg); sheets.append(f)
def pdf(svg, out): subprocess.run(["rsvg-convert", "-f", "pdf", "-o", out, svg], check=True)
if shutil.which("rsvg-convert") and shutil.which("pdfunite"):
    tmp = os.path.join(PACK, "sheets"); parts_pdf = []
    for f in sheets: o = f.replace(".svg", ".pdf"); pdf(f, o); parts_pdf.append(o)
    subprocess.run(["pdfunite", *parts_pdf, os.path.join(PACK, "nimbus-blueprint-1to1-A3.pdf")], check=True)
    # tiled: the 1:1 pages cut into windows with 10 mm overlap and crop marks, for A4 and US Letter
    for label, tw, th in (("A4", 210.0, 297.0), ("Letter", 215.9, 279.4)):
        iw, ih = tw - 16, th - 16; tiles = []
        for pi, pg in enumerate(pages[:5], 1):
            cols, rows = math.ceil((PW - 10) / (iw - 10)), math.ceil((PH - 10) / (ih - 10))
            for r in range(rows):
                for c in range(cols):
                    x0, y0 = c * (iw - 10), r * (ih - 10)
                    inner = pg.split(">", 1)[1].rsplit("</svg>", 1)[0]
                    svg = ('<svg xmlns="http://www.w3.org/2000/svg" width="%gmm" height="%gmm" viewBox="%g %g %g %g"><rect x="%g" y="%g" width="%g" height="%g" fill="white"/>' % (tw, th, x0 - 8, y0 - 8, tw, th, x0 - 8, y0 - 8, tw, th)
                           + inner + '<rect x="%g" y="%g" width="%g" height="%g" fill="none" stroke="%s" stroke-width="0.2" stroke-dasharray="2 2"/>' % (x0, y0, iw, ih, SOFT)
                           + T(x0 + 2, y0 + ih - 2, "sheet %d, tile %d-%d of %dx%d: print at 100 percent, join on the dashed lines" % (pi, r + 1, c + 1, rows, cols), 2.6, fill=TERRA) + "</svg>")
                    f = os.path.join(PACK, "sheets", "tile_%s_%d_%d_%d.svg" % (label, pi, r, c)); open(f, "w").write(svg); o = f.replace(".svg", ".pdf"); pdf(f, o); tiles.append(o)
        subprocess.run(["pdfunite", *tiles, os.path.join(PACK, "nimbus-blueprint-1to1-%s-tiles.pdf" % label)], check=True)
        for o in tiles: os.remove(o); os.remove(o.replace(".pdf", ".svg"))
    for f in parts_pdf: os.remove(f)
else:
    print("PDF skipped: rsvg-convert and pdfunite are needed")

# ---- 3. bill of materials ----
bom = [("Raspberry Pi 4 Model B (any RAM)", 1, "85 x 56 x 17 mm, holes 58 x 49 mm (pi4b.py)", "CAD"),
       ("16:9 HDMI panel, 8 inch class, with touch if wanted", 1, "outline 194 x 110 mm, active %.0f x %.0f mm, about 5 mm thick plus its driver board 100 x 58 x 12 mm" % (info["active"][0], info["active"][1]), "stand-in: measure the real panel before printing"),
       ("Cover glass or acrylic sheet", 1, "194 x 110 x 1.6 mm, corners R 8.5, glued to the 2 mm ledge", "CAD (optional; the panel's own glass can sit in the opening)"),
       ("Self-tapping screw M2.5 x 6", 4, "Pi to the printed standoffs, pilot hole 2.1 mm", "CAD pilot, screw length estimate"),
       ("Self-tapping screw M3 x 12", 2, "from underneath through the foot into the two ribs, clearance hole 3.4, pilot 2.6", "CAD pilot, screw length estimate"),
       ("microSD to SD card extension cable (short)", 1, "card end sits in the front slot 27 x 2.6 mm", "estimate"),
       ("USB-C panel-mount extension cable", 2, "slot 9.8 x 3.6 mm in the back strip", "CAD slot, part estimate"),
       ("3.5 mm panel-mount jack (or 8 mm round connector)", 1, "round hole 8 mm in the back strip", "CAD hole, part estimate"),
       ("Micro-HDMI to HDMI cable, right angle, 20 to 30 cm", 1, "Pi to the panel", "estimate"),
       ("USB-C power supply for the Pi, 5 V 3 A", 1, "outside the case, into one back USB-C extender", "estimate"),
       ("Double-sided foam tape, 2 mm thick, 6 mm wide", "about 1 m", "holds the panel module and the glass", "estimate"),
       ("Super glue or plastic cement", 1, "for the terracotta ground line in its groove, and any loose join", "estimate"),
       ("Heat-set inserts: not needed", 0, "the screws cut their own thread in the printed ribs", "note"),
       ("Fan", 0, "none: one vent slot %.0f x %.1f in the foot and the shell is open behind the Pi" % (info["vent"]["length"], info["vent"]["width"]), "note")]
with open(os.path.join(PACK, "bom.csv"), "w", newline="") as f:
    w = csv.writer(f); w.writerow(["item", "qty", "size or note", "source of the number"]); w.writerows(bom)

# ---- 4. per-part print sheet (volume from the mesh; time and filament are estimates) ----
DENS = {"PLA": 1.24, "PETG": 1.27}; FLOW = 9.0                      # g/cm3; assumed average volumetric speed, mm3/s (estimate)
rows = []
for key, qty, orient, sup, note in (("front_shell", 1, "face down on the bed", "no", "teardrop standoffs and the screw holes are the only overhangs"),
                                    ("back_cover", 1, "back panel down on the bed", "no", "port windows and the scoop sit in the first layers"),
                                    ("ground_inlay", 1, "flat", "no", "print in terracotta; it sits flush in the groove on the back")):
    s = stl.get(key + ".stl"); vol = (s["volume_cm3"] if s and s["volume_cm3"] else None) or info["volumes_cm3"][key]      # the CAD volume where the mesh is not closed
    rows.append([key, qty, "PLA (PETG works)", "0.20 mm", "6 walls = 2.4 mm", "15 percent gyroid", sup, orient, "%.1f x %.1f x %.1f" % tuple(s["size_mm"]), vol,
                 round(vol * DENS["PLA"]) if vol else "", ("%.1f" % (vol * 1000 / FLOW / 3600)) if vol else "", note])
with open(os.path.join(PACK, "print-sheet.csv"), "w", newline="") as f:
    w = csv.writer(f); w.writerow(["part", "qty", "material", "layer", "walls", "infill", "supports", "orientation", "size mm", "volume cm3 (mesh)", "filament g (estimate)", "print hours (estimate)", "note"]); w.writerows(rows)
json.dump({"rows": rows, "assumptions": "density PLA 1.24 g/cm3; average volumetric speed 9 mm3/s; both are estimates and the shell volume is almost all walls, so infill barely changes the weight"}, open(os.path.join(PACK, "print-sheet.json"), "w"), indent=1)

# ---- 5. assembly diagrams ----
def step_svg(n, title, lines, draw):
    w, h = 900.0, 520.0
    s = '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 %g %g" width="%g" height="%g"><rect width="%g" height="%g" fill="%s"/>' % (w, h, w, h, w, h, PAPER)
    s += T(40, 60, "Step %d" % n, 14, weight=600) and '<text x="40" y="58" font-size="30" font-weight="600" font-family="Helvetica, Arial, sans-serif" fill="%s">Step %d: %s</text>' % (INK, n, title)
    for i, ln in enumerate(lines): s += '<text x="40" y="%d" font-size="17" font-family="Helvetica, Arial, sans-serif" fill="%s">%s</text>' % (95 + 26 * i, SOFT, ln)
    return s + draw + "</svg>"
def arrow(x1, y1, x2, y2): return '<line x1="%g" y1="%g" x2="%g" y2="%g" stroke="%s" stroke-width="3"/><polygon points="%g,%g %g,%g %g,%g" fill="%s"/>' % (x1, y1, x2, y2, TERRA, x2, y2, x2 - 10 * math.copysign(1, x2 - x1 or 1) * (abs(x2 - x1) > abs(y2 - y1)), y2 - 6, x2 - 10 * math.copysign(1, x2 - x1 or 1) * (abs(x2 - x1) > abs(y2 - y1)), y2 + 6, TERRA)
def shp(name, x, y, k, fill=CREAM):
    g = sil[name]; b = g.bounds; return '<path d="%s" fill="%s" stroke="%s" stroke-width="1.5" fill-rule="evenodd"/>' % (pathd(g, x, y, (b[0] + b[2]) / 2, (b[1] + b[3]) / 2).replace("", ""), fill, INK)
def sh(name, x, y, k):
    from shapely import affinity as af
    g = af.scale(sil[name], k, k, origin=(0, 0)); b = g.bounds
    return '<path d="%s" fill="%s" stroke="%s" stroke-width="1.5" fill-rule="evenodd"/>' % (pathd(g, x, y, (b[0] + b[2]) / 2, (b[1] + b[3]) / 2), CREAM, INK)
K1 = 1.4
steps = [
 ("Glue the glass to the ledge", ["Front shell face down on a soft cloth.", "Run a bead of foam tape round the 2 mm ledge behind the opening.", "Lay the 194 x 110 glass in the opening: 0.5 mm gap all round."], lambda: sh("front", 450, 330, 1.35) + arrow(450, 140, 450, 215)),
 ("Seat the display", ["Tape the display module to the back of the glass, driver board in the middle.", "Keep the ribbon and power leads free for step 4."], lambda: sh("side", 450, 330, 1.7) + arrow(180, 330, 330, 330)),
 ("Screw the Pi down", ["Four M2.5 x 6 screws into the teardrop standoffs on the floor.", "USB end toward the back; the board stands 26 mm off the desk."], lambda: sh("under", 450, 330, 1.45) + arrow(450, 140, 450, 235)),
 ("Fit the back ports", ["Push the two USB-C extenders and the jack into the strip on the back cover.", "Plug their cables into the Pi and the panel; run the SD extender out of the front slot."], lambda: sh("rear", 450, 330, 1.35) + arrow(450, 500, 450, 440)),
 ("Close the case", ["Fold the cables in and bring the back cover onto the lip: the 0.5 mm reveal shows all round.", "Turn the case over and drive the two M3 x 12 screws up through the foot."], lambda: sh("side", 450, 330, 1.7) + arrow(760, 330, 640, 330)),
 ("Fill the mark", ["Press the terracotta ground line into its groove under the tree on the back.", "A drop of glue holds it flush."], lambda: sh("rear", 450, 330, 1.35) + arrow(450, 170, 450, 250)),
]
for i, (t, ls, d) in enumerate(steps, 1):
    open(os.path.join(PACK, "assembly", "step%d-%s.svg" % (i, t.lower().replace(" ", "-"))), "w").write(step_svg(i, t, ls, d()))

# ---- 6. print-shop sheet ----
tot_v = sum(r[9] for r in rows if r[9]); tot_g = sum(r[10] for r in rows if r[10]); tot_h = sum(float(r[11]) for r in rows if r[11])
shop = ('<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 800 1000" width="800" height="1000"><rect width="800" height="1000" fill="%s"/>' % PAPER)
def sl(y, s, size=18, w=400, c=INK): return '<text x="50" y="%d" font-size="%d" font-weight="%d" font-family="Helvetica, Arial, sans-serif" fill="%s">%s</text>' % (y, size, w, c, s)
lines = [(70, "Print shop sheet: Nimbus case", 34, 600, INK), (105, "Bring this page and the three STL files in the pack folder.", 17, 400, SOFT),
         (160, "What to ask for", 22, 600, INK),
         (190, "Three parts, printed once each: front_shell.stl, back_cover.stl, ground_inlay.stl.", 17, 400, INK),
         (216, "Material: PLA, matte if they have it, cream or off-white (#EFEBE4). The small inlay in terracotta (#B5502C).", 17, 400, INK),
         (242, "Layer height 0.2 mm, 6 wall lines (2.4 mm), 15 percent gyroid infill, no supports, no raft.", 17, 400, INK),
         (268, "Orientation is already set in the STL files: front shell face down, back cover back down.", 17, 400, INK),
         (294, "Biggest part %.0f x %.0f x %.0f mm: needs a bed of 256 x 256 mm or more (Bambu class)." % tuple(stl["front_shell.stl"]["size_mm"]), 17, 400, INK),
         (320, "Both shells measure about 236 x 228 mm, so they print whole on a 256 x 256 mm bed. A smaller bed does not fit them.", 17, 400, INK),
         (375, "Check before you pay", 22, 600, INK),
         (405, "The two shells should sit together with a thin even gap. Do a dry fit with the panel and the Pi before gluing.", 17, 400, INK),
         (431, "Do not scale the files. Units are millimetres. Tolerance about 0.3 mm.", 17, 400, INK),
         (486, "Rough size of the job (estimates, not quotes)", 22, 600, INK),
         (516, "Model volume %.0f cm3, filament about %.0f g, machine time about %.0f hours." % (tot_v, tot_g, tot_h), 17, 400, INK),
         (542, "Material: about 15 to 25 dollars of filament. A shop will often charge 60 to 200 dollars for this much machine time.", 17, 400, INK),
         (568, "Those money and time numbers are estimates from the mesh volume and typical rates, not from a quote.", 17, 400, SOFT),
         (623, "You also need to buy", 22, 600, INK), (653, "See bom.csv: a Pi 4, a 16:9 panel, two USB-C extenders, a jack, an SD extender, a few screws and tape.", 17, 400, INK),
         (708, "Files", 22, 600, INK), (738, "front_shell.stl, back_cover.stl, ground_inlay.stl, nimbus-blueprint-1to1-A3.pdf, bom.csv, print-sheet.csv.", 17, 400, INK),
         (764, "Exact CAD in the .step files. Drawn from the CAD by docs/hardware/nimbus_pack.py.", 17, 400, SOFT)]
for y, t, sz, wt, c in lines: shop += sl(y, t, sz, wt, c)
shop += "</svg>"; open(os.path.join(PACK, "print-shop.svg"), "w").write(shop)
if shutil.which("rsvg-convert"): subprocess.run(["rsvg-convert", "-f", "pdf", "-o", os.path.join(PACK, "print-shop.pdf"), os.path.join(PACK, "print-shop.svg")], check=True)
print("pack ok: volume %.0f cm3, filament %.0f g, %.1f h (estimates)" % (tot_v, tot_g, tot_h))
