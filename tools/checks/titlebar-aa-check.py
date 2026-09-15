#!/usr/bin/env python3
"""Headless proof of the v82 titlebar-circle AA fix, same shape as
traycorner-check.py: boots kernel.elf with -display none, drives the real
mouse path (QMP abs move + btn events, the same v62 vmmouse mechanism
dockhover-check.py/app-interact-check.py already rely on) to click the
Weather dock icon on the real desktop, waits for its window to open, then
pmemsaves the real physical framebuffer and asserts on actual pixels.

Real, confirmed bug (not a guess, real pmemsave capture, see roadmap.md's
v82 entry): gui_draw_app_titlebar's traffic-light dots (drawn by
gui_fill_circle, on every single windowed app: Weather, Mail, Calendar,
Contacts, Settings, ...) computed their AA ramp in LOGICAL pixel units and
wrote it through plain window_pixel. At window_scale() 2 (every real
dock-launched app), window_pixel replicates each logical pixel it's given
into a flat, uninterpolated 2x2 PHYSICAL block. So the AA ramp was real in
logical space but landed on screen as a small number of hard-edged
physical terraces, not a smooth gradient: a real macro-visible staircase,
the same root shape v79 found and fixed in the dock tray's own corner,
just in a different function this repo's other rounded-shape drawer
(gui_rounded_rect_on_wallpaper) doesn't touch. Confirmed with an exact
pixel: physical (187, 98), on the red close-dot's own AA fringe, read pure
(255, 95, 87) pre-fix (the un-blended fill color, zero coverage) and a
real intermediate blend post-fix.

Fix (kernel.c, gui_fill_circle): when there's no offscreen render target
(window_has_target() is false, i.e. this is a direct/physical draw, not
one of the icon-glyph calls inside the 6x-supersampled ICON_SS_SCALE
buffer) and the window is actually scaled, compute coverage in PHYSICAL
pixels instead: 4x4 = 16 subsamples per physical pixel, same box-filter
idea as v79's tray-corner fix, via window_pixel_phys. Every icon-glyph
caller is unaffected (window_has_target() is true there, unchanged path).

Usage: tools/checks/titlebar-aa-check.py   (from the repo root, after make kernel.elf)
"""
import json, os, socket, subprocess, sys, time
from PIL import Image

LOG = "/tmp/jt-titlebaraa-serial.log"
DUMP = "/tmp/jt-titlebaraa.raw"
FB = 0xfd000000; W, H = 1920, 1080
PORT = 4462
LOGICAL_W, LOGICAL_H, SCALE = 960, 540, 2
DOCK_ICON, DOCK_GAP, SLOT0_X = 37, 6, 268
PITCH = DOCK_ICON + DOCK_GAP
ICON_ROW_Y = 487
DOCK_SLOTS = ["Apps", "Files", "Mail", "Calendar", "Notes", "Reminders", "Terminal", "Chat", "Weather", "Trash"]

# Physical pixel on the red close dot's own AA fringe, root-caused above:
# pure fill color pre-fix, a real blended intermediate value post-fix.
PX, PY = 187, 98
RED = (0xFF, 0x5F, 0x57)          # the traffic-light dot's own fill color
BG = (0xFA, 0xF8, 0xF6)           # gui_launch_weather's window background

os.chdir(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
for f in (LOG, DUMP):
    try: os.remove(f)
    except FileNotFoundError: pass

q = subprocess.Popen(["qemu-system-i386", "-kernel", "kernel.elf", "-display", "none", "-vga", "std",
                      "-qmp", f"tcp:127.0.0.1:{PORT},server,nowait", "-serial", "file:" + LOG],
                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
try:
    s = None
    for _ in range(50):
        time.sleep(0.2)
        try: s = socket.create_connection(("127.0.0.1", PORT)); break
        except OSError: pass
    if s is None: raise SystemExit("FAIL: QEMU's QMP socket never came up")
    f = s.makefile("rw")
    def cmd(o):
        f.write(json.dumps(o) + "\n"); f.flush()
        while True:
            r = json.loads(f.readline())
            if "return" in r or "error" in r: return r
    f.readline()
    cmd({"execute": "qmp_capabilities"})
    time.sleep(5.0)

    def move(x, y):
        cmd({"execute": "input-send-event", "arguments": {"events": [
            {"type": "abs", "data": {"axis": "x", "value": int(x * 32768 / LOGICAL_W)}},
            {"type": "abs", "data": {"axis": "y", "value": int(y * 32768 / LOGICAL_H)}}]}})
    def click():
        cmd({"execute": "input-send-event", "arguments": {"events": [{"type": "btn", "data": {"down": True, "button": "left"}}]}})
        cmd({"execute": "input-send-event", "arguments": {"events": [{"type": "btn", "data": {"down": False, "button": "left"}}]}})
    centre = lambda slot: SLOT0_X + slot * PITCH + DOCK_ICON // 2

    move(480, 200); time.sleep(0.3)
    move(centre(DOCK_SLOTS.index("Weather")), ICON_ROW_Y); time.sleep(0.5)
    click(); time.sleep(1.0)
    cmd({"execute": "pmemsave", "arguments": {"val": FB, "size": W * H * 4, "filename": DUMP}})
    # QEMU can tear down the QMP socket the instant it processes quit,
    # before this side ever reads a reply -- a real race, not a bug in
    # the assertions above (which already ran); a reset here must not
    # mask a genuine PASS as a crash.
    try: cmd({"execute": "quit"})
    except (ConnectionResetError, BrokenPipeError, OSError): pass
finally:
    try: q.wait(timeout=5)
    except subprocess.TimeoutExpired: q.kill()

img = Image.frombytes("RGBA", (W, H), open(DUMP, "rb").read(), "raw", "BGRA").convert("RGB")
p = img.getpixel((PX, PY))

d_red = max(abs(p[i] - RED[i]) for i in range(3))
d_bg = max(abs(p[i] - BG[i]) for i in range(3))

print(f"pixel ({PX},{PY}) = {p}  (dist to pure red: {d_red}, dist to pure bg: {d_bg})")

fail = 0
if d_red <= 10:
    print("FAIL: titlebar dot pixel is pure un-blended fill color, the block-replicated staircase is back")
    fail = 1
elif d_bg <= 10:
    print("FAIL: titlebar dot pixel is pure background, unexpected (should be a genuine intermediate blend here)")
    fail = 1
else:
    print("PASS: titlebar dot pixel is a real intermediate blend, physical-resolution coverage AA is active")
sys.exit(fail)
