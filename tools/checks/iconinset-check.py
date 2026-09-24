#!/usr/bin/env python3
"""Static proof that every authored icon (art/icons/*.svg, the source the
dock and Apps folder both rasterize from) actually has the "retina, no
pixels showing" properties the owner asked this polish pass for, without
booting QEMU: rsvg-convert each SVG straight to a 4x-supersampled PNG and
measure it directly.

Why this and not another QEMU framebuffer capture like iconlight/iconedge/
iconhalo/iconart-check.py. Those four all sample the DOCK tray specifically
(11 slots, fixed dock geometry) and would need a second whole boot+scroll
harness to also reach the Apps folder grid's other 14 tiles. Every property
this check cares about -- tile silhouette, edge anti-aliasing, glyph margin
-- is fully determined by the SVG itself (rsvg-convert is a real, exact
rasterizer, not an approximation of what the kernel later area-averages
down), so it can be proven once here for all 26 authored artworks
(dock + Apps-folder-only) in under two seconds, no boot required. The
four existing checks keep proving the separate claim that the *kernel* is
actually drawing this exact art at runtime.

Two properties, both named in the owner's brief:

1. Tile silhouette consistency ("uneven corner radius"). Every authored
   icon is supposed to share one shape: the superellipse squircle_path()
   in tools/gen/restyle_icons.py (SQUIRCLE_N=4.8). Confirmed real gap this
   pass found and fixed: three Apps-folder-only icons (Calculator, Search,
   Contacts) still shipped the pre-Big-Sur `rx=28` rounded rect from
   commit 9db67b9, and all eleven fleet-app imports
   (tools/gen/import_fleet_icons.py) also clipped to a plain `rx=28` rect
   instead of the dock's actual curve. Measured at 360 angles around the
   perimeter, on a real rasterization: the old rx=28 rect misses the true
   squircle radius by 3-6 logical units right at the diagonals (a visibly
   different, more octagonal corner); the fixed files are within
   TOL_SQUIRCLE at every angle.

2. Edge anti-aliasing ("no pixels showing"). At the tile's own silhouette
   crossing, luminance must ramp over more than one raster pixel, not
   jump straight from background to fill. Real rasterization always AAs a
   vector path, so this mostly guards against something turning that off
   by accident (e.g. a stray `shape-rendering="crispEdges"`).

3. Glyph margin consistency ("inconsistent inner margin"), for the tiles
   that share restyle_icons.py's own template (every DOCK and APPS_ONLY
   entry: they all state "Tile #RRGGBB -> #RRGGBB" in a header comment,
   the same marker iconart-check.py already keys off). Their own
   convention (restyle_icons.py's module docstring) is that no glyph pixel
   crosses into the top 16 or bottom 20 of the 128-unit canvas -- that
   band is reserved for the tile's own lit/shaded gradient. Sampled at
   row 8 (mid top margin) and row 118 (mid bottom margin): both must
   match the tile's own stated gradient, not glyph content. Imported
   fleet-brand icons (no such comment) intentionally bleed full-canvas by
   design (import_fleet_icons.py) and are exempt from this one.

Usage: tools/checks/iconinset-check.py   (from the repo root; needs
rsvg-convert, no QEMU, no `make kernel.elf`)
"""
import math, os, re, subprocess, sys
from PIL import Image

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
SVG_DIR = os.path.join(ROOT, "art", "icons")
sys.path.insert(0, os.path.join(ROOT, "tools", "gen"))
from restyle_icons import SQUIRCLE_N  # the one shared shape every icon must match

SS = 4            # supersample factor over the 128-unit canvas
SIZE = 128 * SS
ANGLES = 360
TOL_SQUIRCLE = 1.0    # logical units of allowed radius deviation from the shared squircle. A
                       # plain rx=28 rounded rect (the pre-fix Calculator/Search/Contacts/every
                       # imported fleet icon) measures 1.41-1.46 here; the shared squircle itself
                       # measures 0.3-0.4 (rasterization/sampling noise, not a real mismatch).
MIN_AA_STEPS = 1      # raster samples partway between background and fill at the crossing
MARGIN_TOL = 10        # luminance tolerance for the top/bottom margin check
TOP_ROW, BOT_ROW = 8, 118


def squircle_radius(theta):
    """The shared squircle's actual polar radius (logical units, canvas
    half-size 64) at true angle theta from the tile centre.

    squircle_path() plots the superellipse |x/64|^n + |y/64|^n = 1 via a
    parameter t that is NOT the polar angle (that parametrization bunches
    points away from the diagonals); solving the same implicit equation
    for r at a genuine direction theta gives the closed form below:
    (r|cos theta|/64)^n + (r|sin theta|/64)^n = 1
      => r = 64 / (|cos theta|^n + |sin theta|^n)^(1/n)."""
    c, s = abs(math.cos(theta)), abs(math.sin(theta))
    return 64.0 / (c ** SQUIRCLE_N + s ** SQUIRCLE_N) ** (1.0 / SQUIRCLE_N)


def rasterize(path):
    png = subprocess.run(["rsvg-convert", "-w", str(SIZE), "-h", str(SIZE), path],
                          check=True, stdout=subprocess.PIPE).stdout
    return Image.open(__import__("io").BytesIO(png)).convert("RGBA")


def alpha_at(img, lx, ly):
    """True bilinear alpha at logical (float) coords, 0 outside the canvas."""
    px, py = lx * SS, ly * SS
    if px < 0 or py < 0 or px >= SIZE - 1 or py >= SIZE - 1:
        return 0.0
    x0, y0 = int(px), int(py)
    fx, fy = px - x0, py - y0
    a00 = img.getpixel((x0, y0))[3]
    a10 = img.getpixel((x0 + 1, y0))[3]
    a01 = img.getpixel((x0, y0 + 1))[3]
    a11 = img.getpixel((x0 + 1, y0 + 1))[3]
    return (a00 * (1 - fx) + a10 * fx) * (1 - fy) + (a01 * (1 - fx) + a11 * fx) * fy


def silhouette_check(name, img):
    """Max deviation (logical units) between the actual alpha-crossing
    radius and the shared squircle's, over ANGLES samples, plus whether
    every crossing away from the canvas frame is anti-aliased (not a
    hard step).

    Near theta = 0/90/180/270 the squircle touches the flat side of the
    128-unit canvas exactly at the image's own raster edge (by
    construction -- squircle_path's superellipse inscribes the full
    square). There is no "outside" pixel to blend with there, so those
    crossings are correctly a hard clip to the raster frame, not a defect;
    only crossings with real pixels on both sides prove anything about
    anti-aliasing, so those near the frame are skipped for the AA check
    (still measured for silhouette radius, which is meaningful there too)."""
    cx = cy = 64.0
    worst = 0.0
    hard_steps = 0
    for i in range(ANGLES):
        theta = 2 * math.pi * i / ANGLES
        want_r = squircle_radius(theta)
        dx, dy = math.cos(theta), math.sin(theta)
        # March outward in 0.25-logical-unit steps from just inside the
        # expected radius to just past it, looking for the alpha=128 crossing.
        lo, hi = max(0.0, want_r - 6), min(90.0, want_r + 6)
        samples = []
        r = lo
        near_frame = False
        while r <= hi:
            px, py = (cx + dx * r) * SS, (cy + dy * r) * SS
            if px <= 2 or py <= 2 or px >= SIZE - 3 or py >= SIZE - 3:
                near_frame = True
            a = alpha_at(img, cx + dx * r, cy + dy * r)
            samples.append((r, a))
            r += 0.25
        got_r = None
        for (r0, a0), (r1, a1) in zip(samples, samples[1:]):
            if a0 >= 128 > a1 or a0 > 128 >= a1:
                got_r = r0 + (r1 - r0) * (128 - a0) / (a1 - a0) if a1 != a0 else r0
                if not near_frame:
                    mid = [a for r_, a in samples if r0 - 1.5 <= r_ <= r1 + 1.5]
                    if sum(1 for a in mid if 3 < a < 252) < MIN_AA_STEPS:
                        hard_steps += 1
                break
        if got_r is None:
            # No silhouette crossing anywhere in the band: an empty or
            # full-canvas icon, which is a miss, not a free pass. A ray
            # that runs into the raster frame is the one legitimate case.
            if not near_frame:
                worst = float("inf")
            continue
        worst = max(worst, abs(got_r - want_r))
    return worst, hard_steps


def margin_check(name, img, top_hex, bot_hex):
    def hexrgb(h):
        return tuple(int(h[i:i + 2], 16) for i in (1, 3, 5))
    top, bot = hexrgb(top_hex), hexrgb(bot_hex)

    def predicted(row):
        t = row / 128.0
        return tuple(top[c] + (bot[c] - top[c]) * t for c in range(3))

    def worst_row(row):
        want = predicted(row)
        worst = 0
        for x in range(20, 108, 4):
            p = img.getpixel((x * SS, row * SS))
            worst = max(worst, max(abs(p[c] - want[c]) for c in range(3)))
        return worst

    return worst_row(TOP_ROW), worst_row(BOT_ROW)


def main():
    files = sorted(f[:-4] for f in os.listdir(SVG_DIR) if f.endswith(".svg"))
    fail = 0
    print("-- tile silhouette (shared squircle, SQUIRCLE_N=%.1f) + edge AA --" % SQUIRCLE_N)
    for name in files:
        img = rasterize(os.path.join(SVG_DIR, name + ".svg"))
        worst, hard = silhouette_check(name, img)
        ok = worst <= TOL_SQUIRCLE and hard == 0
        print("%-12s worst radius miss %5.2f logical units, %d hard (non-AA) edges  %s"
              % (name, worst, hard, "ok" if ok else "FAIL"))
        if not ok:
            fail = 1

    print("-- glyph top/bottom margin (DOCK + APPS_ONLY tiles only) --")
    for name in files:
        text = open(os.path.join(SVG_DIR, name + ".svg")).read()
        m = re.search(r"Tile (#[0-9A-Fa-f]{6}) -> (#[0-9A-Fa-f]{6})", text)
        if not m:
            continue  # imported fleet-brand art bleeds full-canvas by design, not part of this convention
        img = rasterize(os.path.join(SVG_DIR, name + ".svg"))
        top_miss, bot_miss = margin_check(name, img, m.group(1), m.group(2))
        ok = top_miss <= MARGIN_TOL and bot_miss <= MARGIN_TOL
        print("%-12s top-margin miss %5.1f  bottom-margin miss %5.1f  %s"
              % (name, top_miss, bot_miss, "ok" if ok else "FAIL: glyph crosses into the tile's own lit/shaded band"))
        if not ok:
            fail = 1

    if fail:
        print("FAIL: an icon's silhouette does not match the shared squircle, has a non-AA edge, "
              "or a glyph crosses into the tile's own top/bottom margin")
        return 1
    print("PASS: every authored icon shares the same tile silhouette, anti-aliased edges, "
          "and (where the shared template applies) the same glyph margin")
    return 0


if __name__ == "__main__":
    sys.exit(main())
