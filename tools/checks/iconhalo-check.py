#!/usr/bin/env python3
"""Headless proof of the v66 icon-corner-bleed fix, the same shape as
dockhover-check.py: boots kernel.elf with -display none, waits for the
desktop's first frame (no click needed, the dock renders on boot), then
pmemsaves the real framebuffer and asserts on actual pixels.

Real, confirmed bug (not a guess): several dock icon glyphs (the Weather
icon's 8 sun rays foremost, by design reaching toward every corner; Mail's
checkmark and Calendar's binder rings to a lesser extent) draw primitives
sized to reach past gui_render_icon_cached's own rounded-corner curve.
Every glyph primitive (gui_draw_capsule, gui_fill_circle, ...) only knows
to blend its own edge toward `bg`, the icon's flat base color, so wherever
a primitive's reach or AA fringe lands past the curve, it overwrites pixels
gui_rounded_rect_gradient had already correctly painted pure background,
and the cache's later blit (which only skips a pixel when it's an EXACT
match to that background color) does not catch a fringe pixel one shade
off. Root-caused with a real headless capture, sampled directly (not
guessed at): the Weather icon's top-left corner, a 10x10 physical-pixel
block that a real rounded corner leaves untouched (dx,dy < ~16 of the 74px
physical tile), came back solid (255,255,255) white before the fix, the
ray's own un-clipped AA fringe, not the tray's real (239,235,228) cream.

This checks exactly that corner, on the one icon whose glyph reaches
furthest into it. Confirmed discriminating: fails on the pre-fix code
(gui_render_icon_cached without its final corner-clip pass), passes with
it restored.

Usage: tools/checks/iconhalo-check.py   (from the repo root, after make kernel.elf)
"""
import json, os, socket, subprocess, sys, time
from PIL import Image

LOG = "/tmp/jt-iconhalo-serial.log"
DUMP = "/tmp/jt-iconhalo.raw"
FB = 0xfd000000; W, H = 1920, 1080
PORT = 4451
LOGICAL_W, LOGICAL_H, SCALE = 960, 540, 2

# Same geometry dockhover-check.py already derived and verified for this
# exact 960x540@2x boot config (dock_scale_pct 7, GUI_ICON_COUNT 10):
# DOCK_ICON 37, slot 0 tray x 268, slot pitch 43. Weather is dock slot 8
# (GUI_DOCK_DEFAULT = {Apps, Files, Mail, Calendar, Notes, Reminders,
# Terminal, Chat, Weather, Trash}). Update these if dock geometry changes.
DOCK_ICON, DOCK_GAP, SLOT0_X, WEATHER_SLOT = 37, 6, 268, 8
PITCH = DOCK_ICON + DOCK_GAP
ICON_TOP_Y = 469          # logical y of the tray's icon row (matches dockhover-check.py)
TRAY = (239, 235, 228)

os.chdir(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
for f in (LOG, DUMP):
    try: os.remove(f)
    except FileNotFoundError: pass

q = subprocess.Popen(["qemu-system-i386", "-kernel", "kernel.elf", "-display", "none", "-vga", "std",
                      "-qmp", f"tcp:127.0.0.1:{PORT},server,nowait", "-serial", "file:" + LOG],
                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
try:
    time.sleep(1.0)
    s = socket.create_connection(("127.0.0.1", PORT)); f = s.makefile("rw")
    def cmd(o):
        f.write(json.dumps(o) + "\n"); f.flush()
        while True:
            r = json.loads(f.readline())
            if "return" in r or "error" in r: return r
    f.readline()
    cmd({"execute": "qmp_capabilities"})
    time.sleep(5.0)  # desktop up, same margin dockhover-check.py uses
    cmd({"execute": "pmemsave", "arguments": {"val": FB, "size": W * H * 4, "filename": DUMP}})
    cmd({"execute": "quit"})
finally:
    try: q.wait(timeout=5)
    except subprocess.TimeoutExpired: q.kill()

img = Image.frombytes("RGBA", (W, H), open(DUMP, "rb").read(), "raw", "BGRA").convert("RGB")

tile_x0 = (SLOT0_X + WEATHER_SLOT * PITCH) * SCALE
tile_y0 = ICON_TOP_Y * SCALE

fail = 0
# A 4x4 physical-pixel block right at the tile's own outer corner. The true
# rounded corner's arc centre sits (radius, radius) in from each edge,
# ~16 physical px at this icon size (37 logical * 22% * 2x scale); a point
# this close to the raw corner is Euclidean distance ~18-23px from that arc
# centre, comfortably past even the outer edge of the AA band (radius +0,
# band starts at inner=radius-AA_BAND), so a correctly clipped icon leaves
# it pure tray color unconditionally. Confirmed on real captures before
# this fix shipped: this exact block came back solid (255,255,255), the
# sun ray's own un-clipped fill; a 6-8px-deeper block was checked too and
# rejected for this test, it's inside the real AA falloff and legitimately
# not pure tray even after the fix (recorded here so the range isn't
# "tuned" back and forth without the reason on record).
bad = []
for dy in range(0, 4):
    for dx in range(0, 4):
        p = img.getpixel((tile_x0 + dx, tile_y0 + dy))
        if max(abs(p[i] - TRAY[i]) for i in range(3)) > 4:
            bad.append((dx, dy, p))

print(f"corner block off-tray pixels: {len(bad)}/16")
if bad[:5]:
    print("  sample:", bad[:5])
if bad:
    print("FAIL: Weather icon's top-left corner is not pure tray color, glyph bleed past the rounded curve"); fail = 1
else:
    print("PASS: Weather icon's corner clips cleanly to the tray, no glyph bleed past the curve")
sys.exit(fail)
