#!/bin/bash
# v0.77.x: the real back buffer, and the systemic end of the "almost every
# click or interaction makes the entire page redraw, and typing each key
# does about the same" report.
#
# Root cause, which nine versions of per-app scoped-redraw helpers narrowed
# but could never remove: every draw in this kernel landed straight on the
# framebuffer being scanned out, so an app that clears and repaints its own
# window is a visible erase-then-redraw. The redraw was never the thing the
# owner saw. Watching it happen was.
#
# Fix: drivers/window.c now draws into an offscreen buffer of the same
# physical geometry, and window_present() copies only the damaged bounding
# box to the framebuffer, at real frame boundaries (the point each input
# loop has finished drawing and is about to wait for input).
#
# This checks the two properties that actually matter, and the two opposite
# ways of breaking the fix each trip a different one:
#
#   1. "backbuffer=ok" -- window_backbuffer_selftest() writes through the
#      normal drawing path, reads the VISIBLE framebuffer back directly and
#      requires it NOT to have moved, then presents and requires it to have
#      moved. Remove the back buffer and this reports "backbuffer=none".
#
#   2. the VISIBLE framebuffer, pmemsave'd after a real dock hover, a real
#      window open and real typing, actually holds the desktop that was
#      drawn: a rich grid of colours, and the dock tray's own colour on the
#      dock row. This is the half that catches the opposite failure, a back
#      buffer nothing ever presents. Strip the window_present() calls from
#      the input-wait boundaries and every draw stays stranded offscreen:
#      the framebuffer keeps whatever it had at boot, a frozen, near-uniform
#      screen.
#
# Proven discriminating by actually reverting, not assumed. Forcing
# back = 0 in window_open_scaled: real FAIL, "backbuffer=none", present=0.
# Stripping every window_present() call from the input-wait boundaries:
# real FAIL on check 2, screen_colours 135 -> 1 and tray_hits 30 -> 0,
# while check 1 still passes (the back buffer is fine, nothing shows it).
# Restored: clean PASS.
set -e
cd "$(dirname "$0")/../.."
make -s kernel.elf

cleanup() { pkill -9 -f "qemu-system-i386.*jt-backbuffer" >/dev/null 2>&1 || true; }
trap cleanup EXIT

PORT=4491
LOG=/tmp/jt-backbuffer-check.log
rm -f "$LOG"
qemu-system-i386 -kernel kernel.elf -display none -vga std \
    -qmp "tcp:127.0.0.1:$PORT,server,nowait" -serial "file:$LOG" -name jt-backbuffer &

set +e
python3 - "$PORT" "$LOG" <<'PYEOF'
import json, os, socket, sys, time
port, log_path = int(sys.argv[1]), sys.argv[2]

def read():
    try:
        with open(log_path) as f: return f.read()
    except FileNotFoundError: return ""

s = None
for _ in range(50):
    time.sleep(0.2)
    try: s = socket.create_connection(("127.0.0.1", port)); break
    except OSError: pass
if s is None:
    print("FAIL: QEMU's QMP socket never came up"); sys.exit(1)
f = s.makefile("rw")
def cmd(o):
    f.write(json.dumps(o) + "\n"); f.flush()
    while True:
        r = json.loads(f.readline())
        if "return" in r or "error" in r: return r
f.readline()
cmd({"execute": "qmp_capabilities"})
time.sleep(3.0)

LOGICAL_W, LOGICAL_H = 960, 540
DOCK_ICON, DOCK_GAP, SLOT0_X = 37, 6, 268   # same dock constants as mwkeyflash-check.sh
PITCH = DOCK_ICON + DOCK_GAP
ICON_ROW_Y = 487
REMINDERS_SLOT = 5

FB, FBW, FBH = 0xfd000000, 1920, 1080   # same framebuffer geometry as dockhover-check.py
RAW = "/tmp/jt-backbuffer-fb.raw"
TRAY = (239, 235, 228)                  # the dock tray's own colour, the constant dockhover-check.py asserts on

def move(x, y):
    cmd({"execute": "input-send-event", "arguments": {"events": [
        {"type": "abs", "data": {"axis": "x", "value": int(x * 32768 / LOGICAL_W)}},
        {"type": "abs", "data": {"axis": "y", "value": int(y * 32768 / LOGICAL_H)}}]}})
def click():
    cmd({"execute": "input-send-event", "arguments": {"events": [{"type": "btn", "data": {"down": True, "button": "left"}}]}})
    time.sleep(0.1)
    cmd({"execute": "input-send-event", "arguments": {"events": [{"type": "btn", "data": {"down": False, "button": "left"}}]}})
def key(qcode):
    cmd({"execute": "send-key", "arguments": {"keys": [{"type": "qcode", "data": qcode}]}})
    time.sleep(0.1)

boot = read()

# real interaction: hover across the dock, open a window, type into it
for slot in range(2, 7):
    move(SLOT0_X + slot * PITCH + DOCK_ICON // 2, ICON_ROW_Y)
    time.sleep(0.25)
move(SLOT0_X + REMINDERS_SLOT * PITCH + DOCK_ICON // 2, ICON_ROW_Y)
time.sleep(0.3); click(); time.sleep(1.0)
key("a"); time.sleep(0.3)
for c in "milk":
    key(c)
time.sleep(0.8)

try: os.remove(RAW)
except FileNotFoundError: pass
cmd({"execute": "pmemsave", "arguments": {"val": FB, "size": FBW * FBH * 4, "filename": RAW}})
time.sleep(1.0)
fb = open(RAW, "rb").read()

def px(x, y):
    o = (y * FBW + x) * 4
    return (fb[o + 2], fb[o + 1], fb[o])

# a grid sample of the whole visible screen: a real desktop is rich, a
# framebuffer nothing ever presented to is one flat colour
colours = set()
for y in range(0, FBH, 17):
    for x in range(0, FBW, 17):
        colours.add(px(x, y))
# the dock tray row, in physical pixels (logical y 487 at scale 2)
tray_hits = sum(1 for x in range(520, 1400, 4) if px(x, ICON_ROW_Y * 2) == TRAY)

log = read()
cmd({"execute": "quit"})

selftest = "backbuffer=ok" in boot or "backbuffer=ok" in log
presents = log.count("present\n")
print("backbuffer_selftest=%s present=%d screen_colours=%d tray_hits=%d"
      % (selftest, presents, len(colours), tray_hits))

if "backbuffer=none" in log:
    print("FAIL: no back buffer was allocated, every draw still lands straight on the visible framebuffer"); sys.exit(1)
if "backbuffer=broken" in log:
    print("FAIL: a back buffer exists but drawing is not actually interposed (window_backbuffer_selftest said so)"); sys.exit(1)
if not selftest:
    print("FAIL: gui_run never reported a back-buffer self-test result at all"); sys.exit(1)
if presents == 0:
    print("FAIL: nothing was ever presented, so nothing the kernel drew could reach the screen"); sys.exit(1)
if len(colours) < 40:   # real numbers measured both ways: 135 with the fix, 1 with the presents stripped
    print("FAIL: the visible framebuffer holds only %d distinct colours, the desktop that was drawn never reached the screen" % len(colours)); sys.exit(1)
if tray_hits < 20:
    print("FAIL: the dock tray's own colour is not on the dock row of the visible framebuffer (%d hits), so what is presented is not the real desktop" % tray_hits); sys.exit(1)
print("PASS: drawing is genuinely offscreen (%d presents) and the real desktop reached the visible framebuffer (%d colours, %d tray pixels)"
      % (presents, len(colours), tray_hits))
PYEOF
STATUS=$?
set -e
cleanup
exit $STATUS
