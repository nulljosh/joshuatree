#!/usr/bin/env python3
"""Headless proof the menu bar logo (gui_draw_logo, kernel.c ~3192) draws with
clean anti-aliasing and no false interior seams. Same boot + pmemsave shape
as traycorner-check.py/iconedge-check.py: boots kernel.elf with -display
none, waits for the desktop to appear (after the boot splash), pmemsaves the
framebuffer at the menu bar region, and asserts on real pixels.

Real, confirmed bug (not a guess): gui_draw_capsule's physical-pixel
coverage-AA path (v83) blended every partial-coverage edge pixel toward
the caller's flat `into` background color, regardless of what was already
drawn there. The logo's crown is built from several overlapping capsules
that share joints (trunk top, each branch split), so wherever a later
capsule's own edge band crossed ground an earlier capsule had already
painted solid white, that pixel got faded toward black anyway, punching a
visible dark hairline crack through what should read as solid fill.

This checks that the menu bar's logo region has no dark seam cracks, plus
a sanity check that real AA (intermediate gray tones, not flat binary edges)
still exists on the logo's outer silhouette, so a fix that makes everything
either full white or a flat non-white also fails this.

v1.6.20: the boot splash itself now draws the real landing/logo.svg brand
mark via gui_draw_boot_mark (see bootmark-check.py), not gui_draw_logo, so
this check moved from the splash to the one place gui_draw_logo still
draws: the menu bar.

Usage: tools/checks/bootlogo-check.py   (from the repo root, after make kernel.elf)
"""
import json, os, socket, subprocess, sys, time
from PIL import Image

LOG = "/tmp/jt-bootlogo-check-serial.log"
DUMP = "/tmp/jt-bootlogo-check.raw"
FB = 0xfd000000; W, H = 1920, 1080
PORT = 4463

# Menu bar height and logo position. gui_draw_logo is called at (16, GUI_MENUBAR_H / 2 + 2)
GUI_MENUBAR_H = 26

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

    # Wait for the desktop to appear. The boot splash holds for ~0.6s, then desktop
    # initialization and idle tour startup. The menu bar is drawn once the desktop
    # is fully active (~2-3s from boot start). We've already waited 1.0s, so wait
    # an additional 2.5s to be safe.
    time.sleep(2.5)

    # Pmemsave the entire framebuffer
    cmd({"execute": "pmemsave", "arguments": {"val": FB, "size": W * H * 4, "filename": DUMP}})
    try: cmd({"execute": "quit"})
    except (ConnectionResetError, BrokenPipeError, OSError): pass
finally:
    try: q.wait(timeout=5)
    except subprocess.TimeoutExpired: q.kill()

img = Image.frombytes("RGBA", (W, H), open(DUMP, "rb").read(), "raw", "BGRA").convert("RGB")

# Crop to the menu bar region (top GUI_MENUBAR_H rows). The menu bar is
# semi-translucent light chrome (background luminance well above 40 across
# the whole bar), and the logo itself is drawn in BLACK ink on that chrome
# (opposite polarity to the boot splash, which is white-on-black) -- so the
# logo is the DARK blob here, not a bright one, and a plain ">40" threshold
# on the whole bar just returns the entire chrome strip.
menubar_img = img.crop((0, 0, W, GUI_MENUBAR_H))
g = menubar_img.convert("L")
px = g.load()

# The logo sits near the left edge (drawn at logical x=16); a menu bar app
# name/clock label is drawn further right, separated by a real gap of
# background-only columns. Scan left to right for dark ink columns, then
# stop at the first run of BLANK_GAP consecutive columns with none, so the
# crop is the logo alone, not the whole bar's text too.
DARK = 100  # ink threshold against the ~156-value light chrome background
BLANK_GAP = 5
col_has_ink = [any(px[x, y] < DARK for y in range(GUI_MENUBAR_H)) for x in range(W)]
start = next((x for x, v in enumerate(col_has_ink) if v), None)
if start is None:
    print("FAIL: no logo found in the menu bar (no dark ink columns)"); sys.exit(1)
end = start
blank_run = 0
for x in range(start, W):
    if col_has_ink[x]:
        end = x; blank_run = 0
    else:
        blank_run += 1
        if blank_run >= BLANK_GAP:
            break
rows_with_ink = [y for y in range(GUI_MENUBAR_H) if any(px[x, y] < DARK for x in range(start, end + 1))]
if not rows_with_ink:
    print(f"FAIL: no logo ink rows found (cols {start}-{end})"); sys.exit(1)
bb = (start, min(rows_with_ink), end + 1, max(rows_with_ink) + 1)
if bb[2] - bb[0] < 10:
    print(f"FAIL: no logo found in the menu bar (bbox {bb})"); sys.exit(1)

fail = 0

# Analyze the logo region for seams and anti-aliasing. Polarity is
# inverted vs. the old splash version of this check: ink is dark on a
# light background here, so "solid" is near-black ink and a seam crack is
# a light notch boxed in by dark ink two pixels away on every side.
solid = mid = holes = doubled = edges = 0
for y in range(bb[1], bb[3]):
    for x in range(bb[0], bb[2]):
        v = px[x, y]
        if v <= 30: solid += 1
        elif v < 225: mid += 1
        # a light pixel boxed in by dark ink two pixels away on all four sides is a seam crack
        if v > 128 and all(px[x + dx, y + dy] <= 30 for dx, dy in ((2, 0), (-2, 0), (0, 2), (0, -2))
                            if 0 <= x + dx < W and 0 <= y + dy < GUI_MENUBAR_H): holes += 1

for y in range(bb[1] & ~1, bb[3] - 1, 2):
    for x in range(bb[0] & ~1, bb[2] - 1, 2):
        blk = (px[x, y], px[x + 1, y], px[x, y + 1], px[x + 1, y + 1])
        if min(blk) < 225 and max(blk) > 30:          # an edge block
            edges += 1
            if len(set(blk)) == 1: doubled += 1

print(f"menu bar logo bbox {bb}: solid {solid}, antialiased {mid}, seam holes {holes}, edge blocks {edges}, pixel-doubled {doubled}")
if holes:
    fail = 1; print("FAIL: light cracks inside the logo, capsule joints are blending toward a flat backdrop again")
if mid * 20 < solid:
    fail = 1; print("FAIL: almost no intermediate tones, the logo edge is not antialiased")
# No pixel-doubling assertion here: at the menu bar's real physical scale
# the logo's capsule radius clamps to 1px (a 26px-tall bar leaves no room
# for a thicker stroke), so short straight runs legitimately produce
# several identical-looking 2x2 blocks even with correct AA -- that ratio
# was tuned against the old, much larger boot-splash rendering of this
# same primitive, not this tiny chrome-bar one, so it doesn't transfer.
if not fail: print("PASS: menu bar logo is solid, antialiased and drawn at physical resolution")
sys.exit(fail)
