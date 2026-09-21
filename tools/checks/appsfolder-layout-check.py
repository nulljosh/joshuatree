#!/usr/bin/env python3
"""Regression test for the four visible Apps-folder QA defects found in a
real headless screenshot (dock slot 0, right after opening, before any
scroll):

1. A dead black band under the title bar, above the glass panel.
   Root cause: gui_draw_wallpaper_rows_sway_ex's GUI_MENUBAR_H clamp
   assumed every caller is painting the real desktop (which reserves that
   strip for the system menu bar); a windowed app has no menu bar inside
   its own clipped viewport, so the clamp silently pulled the paint's top
   edge back down, leaving whatever window_clear had set (near-black)
   unpainted. Same root cause broke the wallpaper's own vertical scale
   (gui_wallpaper_row) for that band once the clamp was lifted -- it kept
   assuming a GUI_MENUBAR_H-tall reserved strip and rendered a stretched
   sliver of the wallpaper photo's own row 0 there instead. Both gated on
   the existing gui_app_windowed flag now.
2. A 4th row of icons drawn OUTSIDE the 375px glass panel, below it, and
   cut in half by the window's own bottom edge. Root cause: the
   row-visibility check in gui_apps_draw_grid compared row*cell_h against
   the panel's pixel height (375) instead of the row COUNT the panel
   actually fits (APPS_VIS_ROWS=3, already computed and named two lines
   above it) -- 3*108=324 is still less than 375, so that row slipped
   through the check despite its bottom edge (432) being nowhere near
   contained.
3. Two extra icons (stale "Contacts"/"Calculator" pixels) appearing after
   Stocks/Search/Epiphany in that same 4th row. Root cause: stale pixels,
   not a second index range and not a wraparound -- confirmed live with a
   temporary per-row serial trace: the very first full repaint (scroll
   offset 0) draws that out-of-panel row (bug 2) with indices 15-19
   (Sparkjar..Calculator); the Apps folder loop's own click-to-open can
   itself trigger one wheel step before the first real scroll input
   (a separate, pre-existing vmmouse quirk, not touched by this fix), and
   the panel-only repaint that follows never clears anything outside the
   375px panel rect it redraws, so indices 15-19's row-3 pixels for
   columns 3 and 4 were never overwritten by the new scroll offset's
   shorter row (20-22, only 3 items). Fixing bug 2 removes the
   out-of-panel row entirely, so there is nothing left to go stale.
4. The heading "Apps" drawn twice: once in the window's own title bar
   (gui_launch_from_dock) and again inside the panel
   (gui_apps_redraw_panel). The inner one is now gone; the key-hint line
   moved up into its place.

Reproduces headlessly with the same QMP absolute-pointer + pmemsave
pattern as launchpad-click-check.py: open the Apps folder from the dock
(dock slot 0), dump the framebuffer immediately (no scroll), then send
real QMP wheel-down events until scrolled to the end and dump again.

Assertions on the FIRST dump (fresh open):
  - No black/near-black band in the strip between the title bar and the
    panel's top edge.
  - No icon-toned pixels in the strip between the panel's bottom edge and
    the window's own bottom edge (bug 2/3's exact location).
  - Exactly one "Apps" heading glyph run in the window (title bar only).

Assertions on the SECOND dump (scrolled to the end):
  - The last row shows exactly the tail apps (Stocks/Search/Epiphany, 3
    tiles) with no extra icon-toned tile in the row's remaining columns
    (bug 3's exact regression shape, at the opposite scroll extreme).

Discriminating: reverting kernel/kernel.c's gui_apps_draw_grid row bound
back to `row * cell_h >= 375` (bug 2/3's real fix) or reverting the
gui_app_windowed-gated wallpaper clamp/scale (bug 1's real fix) each make
this fail on its own; both together reproduce the original screenshot.

Usage: tools/checks/appsfolder-layout-check.py   (from repo root, after make kernel.elf)
"""
import json, os, socket, subprocess, sys, time
from PIL import Image

LOG = "/tmp/jt-appslayout-serial.log"
DUMP1 = "/tmp/jt-appslayout-open.raw"
DUMP2 = "/tmp/jt-appslayout-scrolled.raw"
FB = 0xfd000000; W, H = 1920, 1080
PORT = 4513
LOGICAL_W, LOGICAL_H, SCALE = 960, 540, 2
DOCK_ICON, DOCK_GAP, SLOT0_X = 37, 6, 247
PITCH = DOCK_ICON + DOCK_GAP
ICON_ROW_Y = 487
APPS_FOLDER_SLOT = 0

# Window geometry (gui_launch_from_dock, apps=1 branch): x=56,y=30,w=848,h=490
# -> viewport origin (64,62), size (832,450). Panel (gui_apps_redraw_panel):
# panel_x=x0-28, panel_y=25, panel_w=grid_w+56, panel_h=375, x0=41, grid_w=750
# -> panel spans local (13,25)-(819,400), i.e. logical screen (77,87)-(883,462).
WIN_X, WIN_Y = 56, 30
VX, VY = WIN_X + 8, WIN_Y + 32
PANEL_TOP_LOCAL, PANEL_BOTTOM_LOCAL = 25, 400
TITLEBAR_BOTTOM_LOCAL = 0  # viewport's own top edge; the real title bar is drawn outside it

os.chdir(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
for f in (LOG, DUMP1, DUMP2):
    try: os.remove(f)
    except FileNotFoundError: pass

subprocess.run(["make", "-s", "kernel.elf"], check=True)

q = subprocess.Popen(["qemu-system-i386", "-kernel", "kernel.elf", "-display", "none", "-vga", "std",
                      "-qmp", f"tcp:127.0.0.1:{PORT},server,nowait", "-serial", "file:" + LOG, "-name", "jt-appslayout"],
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
    time.sleep(5.0)  # desktop up

    def move(x, y):
        cmd({"execute": "input-send-event", "arguments": {"events": [
            {"type": "abs", "data": {"axis": "x", "value": int(x * 32768 / LOGICAL_W)}},
            {"type": "abs", "data": {"axis": "y", "value": int(y * 32768 / LOGICAL_H)}}]}})
    def click():
        cmd({"execute": "input-send-event", "arguments": {"events": [
            {"type": "btn", "data": {"down": True, "button": "left"}}]}})
        time.sleep(0.12)
        cmd({"execute": "input-send-event", "arguments": {"events": [
            {"type": "btn", "data": {"down": False, "button": "left"}}]}})
    def wheel_down():
        cmd({"execute": "input-send-event", "arguments": {"events": [
            {"type": "btn", "data": {"down": True, "button": "wheel-down"}}]}})
        time.sleep(0.1)
        cmd({"execute": "input-send-event", "arguments": {"events": [
            {"type": "btn", "data": {"down": False, "button": "wheel-down"}}]}})
    def dump(path):
        cmd({"execute": "pmemsave", "arguments": {"val": FB, "size": W * H * 4, "filename": path}})
    centre = lambda slot: SLOT0_X + slot * PITCH + DOCK_ICON // 2

    move(centre(APPS_FOLDER_SLOT), ICON_ROW_Y); time.sleep(0.4)
    click(); time.sleep(2.0)
    dump(DUMP1)

    for _ in range(6):
        wheel_down(); time.sleep(0.3)
    dump(DUMP2)

    try: cmd({"execute": "quit"})
    except (ConnectionResetError, BrokenPipeError, OSError): pass
finally:
    try: q.wait(timeout=5)
    except subprocess.TimeoutExpired: q.kill()

def load(path):
    return Image.frombytes("RGBA", (W, H), open(path, "rb").read(), "raw", "BGRA").convert("RGB")

def logical_to_px(lx, ly):
    return lx * SCALE, ly * SCALE

fails = []

img1 = load(DUMP1)

# --- Defect 1: dead black band between the title bar and the panel top.
# Local y in [0, PANEL_TOP_LOCAL) at a representative x, well clear of the
# rounded panel corner, should be wallpaper (varied, non-near-black), not
# the window_clear(0x00201922) fill.
band_x_logical = VX + 400
dark = 0
sampled = 0
for local_y in range(2, PANEL_TOP_LOCAL - 2):
    ly = VY + local_y
    px, py = logical_to_px(band_x_logical, ly)
    r, g, b = img1.getpixel((px, py))
    sampled += 1
    if r < 40 and g < 35 and b < 40:
        dark += 1
if sampled and dark / sampled > 0.5:
    fails.append("defect 1 (black band): sampled %d/%d near-black pixels under the title bar, above the panel" % (dark, sampled))

# --- Defect 2/3: nothing icon-like between the panel's bottom edge and
# the window's own bottom edge. Icons sit on a light cell background
# (0x00E9DEE0-ish) or draw saturated glyph colours; the raw wallpaper rect
# just below the panel is a muted aerial photo. Look for any strongly
# saturated (icon-tile) pixel in that strip.
def saturation(r, g, b):
    mx, mn = max(r, g, b), min(r, g, b)
    return (mx - mn)
spill_hits = 0
spill_checked = 0
for local_y in range(PANEL_BOTTOM_LOCAL + 4, min(PANEL_BOTTOM_LOCAL + 90, (WIN_Y + 490 - 38) - VY)):
    ly = VY + local_y
    for local_x in range(20, 800, 8):
        lx = VX + local_x
        px, py = logical_to_px(lx, ly)
        if px >= W or py >= H: continue
        r, g, b = img1.getpixel((px, py))
        spill_checked += 1
        if saturation(r, g, b) > 90:
            spill_hits += 1
if spill_checked and spill_hits > 6:
    fails.append("defect 2/3 (row spill / ghost icons): %d saturated icon-toned pixels found below the panel's bottom edge (checked %d)" % (spill_hits, spill_checked))

# --- Defect 4: "Apps" heading not duplicated inside the panel. The old
# inner heading's dark-text glyph run sat at local (x0=41, y=40); that
# exact row should now be the (lighter, grey) hint-line text instead of a
# second dark "Apps" run. Cheap discriminator: count near-black glyph
# pixels on the row the old heading occupied, within the first ~60px
# (where "Apps" would start) -- the hint line uses a lighter grey
# (0x006A6064) so it should score much lower than the old dark heading
# (0x002A2226) did.
heading_row_dark = 0
for local_x in range(41, 41 + 90):
    lx = VX + local_x
    for local_y in range(33, 48):
        ly = VY + local_y
        px, py = logical_to_px(lx, ly)
        r, g, b = img1.getpixel((px, py))
        if r < 55 and g < 55 and b < 60:
            heading_row_dark += 1
if heading_row_dark > 40:
    fails.append("defect 4 (duplicate heading): %d dark glyph pixels found where the old inner \"Apps\" heading used to sit" % heading_row_dark)

# --- Scrolled-to-the-end: last row shows only the 3 real tail apps, no
# ghosts in the row's remaining columns. Row math (gui_launch_apps):
# cell_w=150, cell_h=108, tile=60, x0=41, y0=95; at max scroll the tail
# row (Stocks/Search/Epiphany, cols 0-2) lands at local row APPS_VIS_ROWS-1.
img2 = load(DUMP2)
CELL_W, CELL_H, X0, Y0, APPS_VIS_ROWS = 150, 108, 41, 95, 3
tail_row_local_y = Y0 + (APPS_VIS_ROWS - 1) * CELL_H
ghost_hits = 0
ghost_checked = 0
for col in (3, 4):  # columns with no real app at max scroll
    cx = X0 + col * CELL_W + CELL_W // 2
    for dy in range(-30, 40, 4):
        ly = VY + tail_row_local_y + dy
        lx = VX + cx
        px, py = logical_to_px(lx, ly)
        if px >= W or py >= H: continue
        r, g, b = img2.getpixel((px, py))
        ghost_checked += 1
        if saturation(r, g, b) > 90:
            ghost_hits += 1
if ghost_checked and ghost_hits > 4:
    fails.append("scrolled-to-end regression: %d saturated pixels found in the tail row's empty columns (checked %d) -- ghost icons after the real apps" % (ghost_hits, ghost_checked))

if fails:
    print("FAIL:")
    for msg in fails: print("  - " + msg)
    sys.exit(1)

print("PASS: no black band, no row spilled past the panel, no ghost icons at open or at max scroll, heading drawn once")
sys.exit(0)
