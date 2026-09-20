#!/bin/bash
# The Apps folder repainted the wallpaper and its glass panel on every pass
# of its poll loop, not just when something changed, which is what made
# scrolling and moving the selection flash the whole screen. The loop now
# paints the full frame once (marker "appsfullrepaint") and repaints the
# panel rect alone on every later change (marker "appsgridrepaint").
#
# This asserts exactly that: open the Apps folder from the dock, then move
# the selection with real keystrokes and demand the full-repaint count stay
# flat at 1 while the panel-repaint count actually grows (a flat panel count
# would mean the selection is not being drawn at all, so both halves matter).
#
# Discriminating: hoist the full paint back inside the loop and the first
# assertion fails immediately, one full repaint per poll tick.
set -e
cd "$(dirname "$0")/../.."
make -s kernel.elf

cleanup() { pkill -9 -f "qemu-system-i386.*jt-appsredraw" >/dev/null 2>&1 || true; }
trap cleanup EXIT

PORT=4489
LOG=/tmp/jt-appsredraw-check.log
rm -f "$LOG"
qemu-system-i386 -kernel kernel.elf -display none -vga std \
    -qmp "tcp:127.0.0.1:$PORT,server,nowait" -serial "file:$LOG" -name jt-appsredraw &

python3 - "$PORT" "$LOG" <<'PYEOF'
import json, socket, sys, time
port, log_path = int(sys.argv[1]), sys.argv[2]

def count(marker):
    try:
        with open(log_path) as f: return f.read().count(marker)
    except FileNotFoundError: return 0

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
DOCK_ICON, DOCK_GAP, SLOT0_X = 37, 6, 268  # same dock constants as appclose-check.py
PITCH = DOCK_ICON + DOCK_GAP
ICON_ROW_Y = 487
APPS_SLOT = 0

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
    time.sleep(0.2)

centre = SLOT0_X + APPS_SLOT * PITCH + DOCK_ICON // 2
move(centre, ICON_ROW_Y); time.sleep(0.3); click(); time.sleep(1.5)  # open the Apps folder
full_open, panel_open = count("appsfullrepaint\n"), count("appsgridrepaint\n")

for c in ("d", "d", "s", "a"):  # right, right, down, left: real selection moves
    key(c)
time.sleep(0.5)
full_after, panel_after = count("appsfullrepaint\n"), count("appsgridrepaint\n")

cmd({"execute": "quit"})
print("full: %d -> %d   panel: %d -> %d" % (full_open, full_after, panel_open, panel_after))
if full_open != 1:
    print("FAIL: expected exactly one full repaint on open, got %d" % full_open); sys.exit(1)
if full_after != 1:
    print("FAIL: full repaint count grew to %d while only the selection moved" % full_after); sys.exit(1)
if panel_after <= panel_open:
    print("FAIL: panel never repainted, so the selection moves are not being drawn at all"); sys.exit(1)
print("PASS: Apps folder painted the full frame once and repainted the panel alone for every selection move")
PYEOF
STATUS=$?
cleanup
exit $STATUS
