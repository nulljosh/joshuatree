#!/usr/bin/env python3
"""Bake a real, once-captured satellite image into kernel/wall_sat.h, the
same "host-side generator produces a committed C header" pattern
gen_wallpaper.sh (the tree photo) and gen_icon_art.py (the dock artwork)
already use.

Why this exists: v86 (the landing page's in-browser emulator) has no real
network at all (confirmed by grep, zero hits for any NIC driver in
libv86.js -- see drivers/rtl8139.c's own ARCHITECTURE.md row). wall_fetch()
in kernel.c can therefore never complete there, so v0.79.2 made
font_is_fallback() (the real v86 signal) fall back to the baked tree photo
instead of a permanently black desktop. Direct owner request, Sep 2026:
the demo should show a satellite look, not the tree, and the only honest
way to do that with no live network is to bake a REAL satellite capture in
at build time, the same way the tree photo is baked in, and use that as
the v86 fallback instead.

This script fetches the SAME real tiles wall_fetch() fetches for
WALL_THEME_SAT (mt0.google.com/vt/lyrs=s&x=X&y=Y&z=Z, plain HTTP, real
baseline JPEG, no key) at the SAME zoom/tile-grid/crop math
(kernel/kernel.c's WALL_ZOOM/WALL_TILE/WALL_COLS/WALL_ROWS and the
xf/yf slippy-map formula in wall_fetch) for a real default location:
Langley, BC (49.0847, -122.5765), this repo's own real-world reference
point (CLAUDE.md's Vancouver-area default). If wall_fetch's constants
below ever change, update them here too so a future capture matches what
the kernel would really fetch.

Kernel-image budget (checked the same way gen_icon_art.py's own comment
documents for the icon-art PNG-storage pass, since it's the same
0xC0500000 ring-3 program window ceiling, see boot/linker.ld and
docs/SYSCALL-ABI.md): a raw 960x540x3 RGB blob is 1,555,200 bytes, more
than double the ~638KB of headroom this kernel image had before this
capture (measured via `size`/section-header dump against
boot/linker.ld's `. + .bss <= 0xC0500000` ASSERT) -- a raw bake would not
link. Quantized to a 32-color indexed PNG (drivers/png.c has decoded
8-bit indexed/PLTE images since v75, exactly this shape) the same real
capture is ~270KB, a >5.6x saving, decodes byte-for-byte through the same
decoder path icon_art.h already proves correct (tools/png-host-check.sh),
and leaves real headroom for whatever ships next. The decoded RGB buffer
(960x540x3, same layout as wallpaper_rgb) lives in kmalloc'd heap once,
not in the linked image, the same lazy-decode-and-keep pattern
wall_dark_fallback_init() already uses for its own permanent buffer.

Usage:
    python3 tools/gen/gen_wall_sat.py             # fetch + regenerate kernel/wall_sat.h
    python3 tools/gen/gen_wall_sat.py --budget     # report sizes only, no network needed if tiles are cached
"""
import math
import os
import struct
import sys
import urllib.request

# Must match kernel/kernel.c's own WALL_ZOOM/WALL_TILE/WALL_COLS/WALL_ROWS
# and WALLPAPER_W/WALLPAPER_H (drivers/wallpaper.h). If the kernel's own
# constants change, this capture is stale until re-run with matching values.
WALL_ZOOM = 14
WALL_TILE = 256
WALL_COLS = 4
WALL_ROWS = 3
WALLPAPER_W = 960
WALLPAPER_H = 540

# Langley, BC -- this repo's own real-world reference point (see
# CLAUDE.md), used because wall_fetch() itself has no compiled-in default
# location on purpose (geo_fetch's real IP lookup is the only source at
# runtime, "no real location, no fetch, nothing fabricated"). This is a
# build-time asset, captured once like the tree photo, not tied to that
# runtime lookup.
LAT, LON = 49.0847, -122.5765

QUANT_COLORS = 32  # see the module docstring's budget note for the tradeoff

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
OUT = os.path.join(ROOT, "kernel", "wall_sat.h")
CACHE_DIR = "/tmp/jt-wall-sat-tiles"


def slippy_tile_origin(lat, lon, zoom):
    """Same formula as wall_fetch()'s xf/yf/tx/ty math in kernel.c."""
    n = 2 ** zoom
    latr = math.radians(lat)
    t = math.tan(latr)
    xf = (lon + 180.0) / 360.0 * n
    yf = (1.0 - math.log(t + math.sqrt(1.0 + t * t)) / math.pi) / 2.0 * n
    tx = int(xf - 1.5)
    ty = int(yf - 1.0)
    px = int((xf - tx) * WALL_TILE)
    py = int((yf - ty) * WALL_TILE)
    cx = px - WALLPAPER_W // 2
    cy = py - WALLPAPER_H // 2
    cx = max(0, min(cx, WALL_COLS * WALL_TILE - WALLPAPER_W))
    cy = max(0, min(cy, WALL_ROWS * WALL_TILE - WALLPAPER_H))
    return tx, ty, cx, cy


def fetch_tiles(tx, ty):
    from PIL import Image
    os.makedirs(CACHE_DIR, exist_ok=True)
    mosaic = Image.new("RGB", (WALL_COLS * WALL_TILE, WALL_ROWS * WALL_TILE))
    for row in range(WALL_ROWS):
        for col in range(WALL_COLS):
            x, y = tx + col, ty + row
            fn = os.path.join(CACHE_DIR, "tile_%d_%d_%d.jpg" % (x, y, WALL_ZOOM))
            if not os.path.exists(fn):
                url = "http://mt0.google.com/vt/lyrs=s&x=%d&y=%d&z=%d" % (x, y, WALL_ZOOM)
                req = urllib.request.Request(url, headers={"User-Agent": "curl/8.7.1"})
                with urllib.request.urlopen(req, timeout=20) as resp:
                    data = resp.read()
                with open(fn, "wb") as f:
                    f.write(data)
            tile = Image.open(fn).convert("RGB")
            if tile.size != (WALL_TILE, WALL_TILE):
                raise SystemExit("tile %d,%d is %r, expected %dx%d" % (x, y, tile.size, WALL_TILE, WALL_TILE))
            mosaic.paste(tile, (col * WALL_TILE, row * WALL_TILE))
    return mosaic


def capture_png():
    from PIL import Image
    import io
    tx, ty, cx, cy = slippy_tile_origin(LAT, LON, WALL_ZOOM)
    mosaic = fetch_tiles(tx, ty)
    crop = mosaic.crop((cx, cy, cx + WALLPAPER_W, cy + WALLPAPER_H))
    if crop.size != (WALLPAPER_W, WALLPAPER_H):
        raise SystemExit("crop is %r, expected %dx%d" % (crop.size, WALLPAPER_W, WALLPAPER_H))
    quantized = crop.quantize(colors=QUANT_COLORS, method=Image.MEDIANCUT, dither=Image.FLOYDSTEINBERG)
    buf = io.BytesIO()
    quantized.save(buf, format="PNG", optimize=True, compress_level=9)
    png = buf.getvalue()

    if png[:8] != b"\x89PNG\r\n\x1a\n":
        raise SystemExit("did not produce a PNG")
    w, h, depth, ctype, comp, filt, inter = struct.unpack(">IIBBBBB", png[16:29])
    if (w, h) != (WALLPAPER_W, WALLPAPER_H):
        raise SystemExit("rasterized to %dx%d, expected %dx%d" % (w, h, WALLPAPER_W, WALLPAPER_H))
    if (depth, ctype, comp, filt, inter) != (8, 3, 0, 0, 0):
        raise SystemExit(
            "IHDR is depth %d colour-type %d interlace %d; drivers/png.c only decodes "
            "8-bit indexed (colour-type 3) or truecolor(+alpha), non-interlaced, for this "
            "path -- this would fail at runtime" % (depth, ctype, inter))
    return png, (tx, ty, cx, cy)


def c_array(data):
    out = ["static const unsigned char wall_sat_png[%d] = {" % len(data)]
    for i in range(0, len(data), 16):
        out.append("    " + ",".join("%d" % b for b in data[i:i + 16]) + ",")
    out.append("};")
    return "\n".join(out)


def build(png, coords):
    tx, ty, cx, cy = coords
    return """/* GENERATED by tools/gen/gen_wall_sat.py -- do not edit.
   A real, once-captured satellite photograph, baked in the same "host-side
   generator produces a committed C header" pattern kernel/icon_art.h and
   drivers/wallpaper.h (the tree photo) already use. Captured from the same
   real source wall_fetch() uses at runtime for WALL_SAT (Google's
   mt0.google.com/vt/lyrs=s slippy-map tiles, plain HTTP, real baseline
   JPEG), same zoom/tile-grid/crop math, for Langley, BC (49.0847,
   -122.5765) -- this repo's own real-world reference point, since
   wall_fetch() itself has no compiled-in default location.
   Tile origin z=%d x=%d y=%d, crop offset (%d,%d) inside the %dx%d mosaic.
   Stored as an 8-bit indexed (%d-color, PLTE) PNG -- drivers/png.c has
   decoded that shape since v75, and quantizing to %d colors takes a
   1,555,200-byte raw 960x540x3 RGB capture down to %d bytes, small enough
   to fit the ~638KB of headroom the kernel image had left under
   boot/linker.ld's 0xC0500000 ring-3 program window ceiling (see this
   script's own module docstring for the real measured numbers).
   The kernel png_decode()s this once, lazily, on first need (v86 desktop
   paint with no map ever going to arrive) and keeps the decoded 960x540x3
   buffer for the session, the same permanent-buffer pattern
   wall_dark_fallback_init() already uses; see wall_sat_init() in
   kernel/kernel.c.
   Regenerate with: python3 tools/gen/gen_wall_sat.py */
#ifndef WALL_SAT_H
#define WALL_SAT_H

#define WALL_SAT_PNG_LEN %d

%s

#endif
""" % (WALL_ZOOM, tx, ty, cx, cy, WALL_COLS * WALL_TILE, WALL_ROWS * WALL_TILE,
       QUANT_COLORS, QUANT_COLORS, len(png), len(png), c_array(png))


def main():
    if "--budget" in sys.argv:
        png, _ = capture_png()
        raw = WALLPAPER_W * WALLPAPER_H * 3
        print("captured satellite PNG: %d bytes (%.0f KB); raw RGB would be %d bytes (%.0f KB), a %.1fx saving"
              % (len(png), len(png) / 1024.0, raw, raw / 1024.0, raw / len(png)))
        return 0
    png, coords = capture_png()
    text = build(png, coords)
    with open(OUT, "w") as f:
        f.write(text)
    print("wrote %s (%d bytes of indexed PNG, tile origin z=%d x=%d y=%d)"
          % (OUT, len(png), WALL_ZOOM, coords[0], coords[1]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
