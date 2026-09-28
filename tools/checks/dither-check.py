#!/usr/bin/env python3
# 1-bit kernel: every UI grey and alpha blend is a 4x4 Bayer ordered dither,
# so the framebuffer only ever holds the two source colours of a blend, never
# a mid tone. gui_blend/gui_lerp (kernel/gui_prims.c) hand back a pair handle
# and drivers/window.c picks a or b per pixel.
#
# Two regions the change owns, read off the VISIBLE framebuffer via pmemsave:
#
#   1. The menu bar. It is a 50/50 blend of white toward the wallpaper colour
#      of each row (gui_draw_menubar). Before: one flat tint per row, never
#      pure white. After: every row holds at most two colours, one of them
#      pure white, and neither colour fills the row alone (a real pattern,
#      not a flattened fill).
#   2. The dock tray shadow. A blend of the wallpaper toward black, fading
#      over ten logical rows. Before: darkened wallpaper, never pure black.
#      After: pure black pixels appear, mixed with untouched wallpaper, with
#      the black share falling row by row.
#
# Discriminating by construction: revert gui_dither to a flat lerp and check 1
# sees zero white on every row, check 2 sees zero pure black.
import json, os, socket, subprocess, sys, time

PORT = int(os.environ.get("JT_QMP_PORT", "4497"))   # not 4444/4491: another VM may be up
LOG = "/tmp/jt-dither-check.log"
RAW = "/tmp/jt-dither-fb.raw"
FB, FBW, FBH = 0xfd000000, 1920, 1080
SC = 2
MENUBAR_H = 26
DOCK_ROW_Y = 487

os.chdir(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
subprocess.run(["make", "-s", "kernel.elf"], check=True)
for f in (LOG, RAW):
    try: os.remove(f)
    except FileNotFoundError: pass
qemu = subprocess.Popen(["qemu-system-i386", "-kernel", "kernel.elf", "-display", "none", "-vga", "std",
                         "-qmp", "tcp:127.0.0.1:%d,server,nowait" % PORT, "-serial", "file:" + LOG, "-name", "jt-dither"])

def fail(msg):
    print("FAIL: " + msg)
    qemu.kill(); sys.exit(1)

s = None
for _ in range(50):
    time.sleep(0.2)
    try: s = socket.create_connection(("127.0.0.1", PORT)); break
    except OSError: pass
if s is None: fail("QEMU's QMP socket never came up")
f = s.makefile("rw")
def cmd(o):
    f.write(json.dumps(o) + "\n"); f.flush()
    while True:
        r = json.loads(f.readline())
        if "return" in r or "error" in r: return r
f.readline()
cmd({"execute": "qmp_capabilities"})
time.sleep(4.0)
# a real mouse move so the desktop and dock band have been composed at least once
cmd({"execute": "input-send-event", "arguments": {"events": [
    {"type": "abs", "data": {"axis": "x", "value": 16384}}, {"type": "abs", "data": {"axis": "y", "value": 8000}}]}})
time.sleep(1.0)
cmd({"execute": "pmemsave", "arguments": {"val": FB, "size": FBW * FBH * 4, "filename": RAW}})
time.sleep(1.0)
fb = open(RAW, "rb").read()
cmd({"execute": "quit"})
try: qemu.wait(timeout=5)
except subprocess.TimeoutExpired: qemu.kill()

def px(x, y):
    o = (y * FBW + x) * 4
    return (fb[o + 2], fb[o + 1], fb[o])

WHITE, BLACK = (255, 255, 255), (0, 0, 0)

# 1. menu bar, a text-free band between the menu titles and the clock
X0, X1 = 800, 1300
bad_rows, white_rows = [], 0
for y in range(0, MENUBAR_H * SC - 2):   # the last two physical rows are the bar's solid hairline
    row = [px(x, y) for x in range(X0, X1)]
    cols = set(row)
    whites = row.count(WHITE)
    if len(cols) > 2 or whites == 0 or whites == len(row):
        bad_rows.append((y, len(cols), whites))
    else:
        white_rows += 1
print("menubar rows=%d two_tone_with_white=%d bad=%s" % (MENUBAR_H * SC - 2, white_rows, bad_rows[:4]))

# 2. dock shadow: the rows just under the tray, between the tray's own edges
shadow_rows = []
for y in range(DOCK_ROW_Y * SC + 30, FBH):
    row = [px(x, y) for x in range(700, 1200)]
    blacks = row.count(BLACK)
    if 0 < blacks < len(row): shadow_rows.append((y, blacks))
print("shadow rows with a black/wallpaper mix: %d %s" % (len(shadow_rows), shadow_rows[:3]))

if bad_rows: fail("menu bar rows are not a two-tone white dither: %s" % bad_rows[:6])
if len(shadow_rows) < 8: fail("dock shadow is not a black/wallpaper dither (only %d mixed rows)" % len(shadow_rows))
shares = [b for _, b in shadow_rows]
if not (shares[0] > shares[-1]): fail("dock shadow black share does not fade with distance: %s" % shares)
print("PASS: 1-bit chrome, menu bar is white-or-wallpaper on all %d rows, dock shadow is black-or-wallpaper over %d rows" % (white_rows, len(shadow_rows)))
