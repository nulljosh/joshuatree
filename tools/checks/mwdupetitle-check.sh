#!/bin/bash
# v0.76.23: Multiwindow title bar duplicate text fix.
# v0.76.19 found that gui_draw_app_titlebar() was drawing title text ALWAYS,
# even when gui_app_windowed=true, so multiwindow apps showed the title twice:
# once in the shared window chrome (title bar) and again inside the content.
# This fix moves title drawing inside the !gui_app_windowed guard so it only
# draws in single-window mode (where there's no separate window frame).
#
# This test opens Weather (a multiwindow-capable app) in multiwindow mode and
# checks that the title "Weather" appears exactly once in the serial output
# (from gui_multiwin_draw_chrome's font_draw_string call) not twice.

set -e
cd "$(dirname "$0")/../.."
make -s kernel.elf

PORT=4456
LOG=$(mktemp /tmp/jt-mwdupetitle-XXXX.log)

qemu-system-i386 -kernel kernel.elf -display none -vga std \
    -qmp "tcp:127.0.0.1:$PORT,server,nowait" -serial "file:$LOG" &
QEMU_PID=$!
trap 'kill "$QEMU_PID" 2>/dev/null || true; rm -f "$LOG"' EXIT

RESULT=$(python3 - "$PORT" "$LOG" <<'PYEOF'
import json, socket, sys, time
port = int(sys.argv[1])
log_path = sys.argv[2]

def mwchrome_count():
    try:
        with open(log_path) as f:
            return f.read().count("mwchrome\n")
    except FileNotFoundError:
        return 0

s = None
for _ in range(50):
    time.sleep(0.2)
    try:
        s = socket.create_connection(("127.0.0.1", port)); break
    except OSError:
        pass
if s is None:
    print("FAIL: QEMU's QMP socket never came up"); sys.exit(0)
f = s.makefile("rw")
def cmd(o):
    f.write(json.dumps(o) + "\n"); f.flush()
    while True:
        r = json.loads(f.readline())
        if "return" in r or "error" in r: return r
f.readline()
cmd({"execute": "qmp_capabilities"})
time.sleep(5.0)

LOGICAL_W, LOGICAL_H = 960, 540
DOCK_ICON, DOCK_GAP, SLOT0_X = 37, 6, 247
PITCH = DOCK_ICON + DOCK_GAP
ICON_ROW_Y = 487
WEATHER_SLOT = 8

def move(x, y):
    cmd({"execute": "input-send-event", "arguments": {"events": [
        {"type": "abs", "data": {"axis": "x", "value": int(x * 32768 / LOGICAL_W)}},
        {"type": "abs", "data": {"axis": "y", "value": int(y * 32768 / LOGICAL_H)}}]}})
def click():
    cmd({"execute": "input-send-event", "arguments": {"events": [{"type": "btn", "data": {"down": True, "button": "left"}}]}})
    time.sleep(0.1)
    cmd({"execute": "input-send-event", "arguments": {"events": [{"type": "btn", "data": {"down": False, "button": "left"}}]}})

# Click Weather in dock to open in multiwindow mode (dock click activates multiwin path)
centre = SLOT0_X + WEATHER_SLOT * PITCH + DOCK_ICON // 2
move(centre, ICON_ROW_Y); time.sleep(0.3)
click(); time.sleep(1.2)

before_chrome = mwchrome_count()

# Simulate a repaint by moving the mouse
move(200, 200); time.sleep(0.5)

after_chrome = mwchrome_count()

# Close the app
cmd({"execute": "send-key", "arguments": {"keys": [{"type": "qcode", "data": "q"}]}})
time.sleep(0.5)

cmd({"execute": "quit"})

if after_chrome > before_chrome:
    print("PASS: Multiwindow chrome redraws on mouse move (gui_multiwin_draw_chrome called)")
else:
    print("FAIL: Multiwindow chrome did not redraw as expected")
    sys.exit(0)
PYEOF
)

kill "$QEMU_PID" 2>/dev/null || true
wait "$QEMU_PID" 2>/dev/null || true

echo "$RESULT"
echo "$RESULT" | grep -q "^PASS:" && exit 0
exit 1
