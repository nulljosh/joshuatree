#!/bin/bash
# v0.76.18: direct report, still reproducing after the earlier Notes/
# Terminal/Chat chrome-redraw fixes: "keystroke re-rendering glitch still
# present." Root cause, same bug shape those fixes already covered, just
# never extended to the multi-window Mail/Calendar/Reminders windows:
# gui_run's own v0.75.0 "cheap tier" for a keystroke into one of those
# windows (mw_key_repaint) was already scoped to just that one window
# instead of the whole desktop, but it called the OLD gui_multiwin_draw_one,
# which unconditionally redrew that window's ENTIRE chrome -- the rounded-
# rect wallpaper blend across the whole ~820x385 rect, all three traffic
# lights, and the title -- before ever touching content, on every single
# keystroke. None of that chrome depends on what's being typed.
#
# Fix: gui_multiwin_draw_one split into gui_multiwin_draw_chrome (the
# expensive, unchanging part) and gui_multiwin_draw_content_only (the part
# that actually needs to redraw on a keystroke). The keystroke fast path
# now calls only the latter; window-open/full-repaint paths still call
# both via the unchanged gui_multiwin_draw_one.
#
# Proven here via a new serial marker, "mwchrome\n" (only emitted by
# gui_multiwin_draw_chrome): opens Reminders (multi-window), presses 'a'
# to start adding one, types "milk" (4 plain keystrokes), and demands the
# chrome-draw count stay flat at 1 (the one real draw from opening the
# window) through all five following keystrokes.
#
# Proven discriminating: reverted the keystroke fast path locally to call
# gui_multiwin_draw_one instead of gui_multiwin_draw_content_only, reran,
# got a real FAIL (mwchrome grew 1 -> 6, one per keystroke); restored,
# clean PASS.
set -e
cd "$(dirname "$0")/../.."
make -s kernel.elf

cleanup() { pkill -9 -f "qemu-system-i386.*jt-mwkeyflash" >/dev/null 2>&1 || true; }
trap cleanup EXIT

PORT=4485
LOG=/tmp/jt-mwkeyflash-check.log
rm -f "$LOG"
qemu-system-i386 -kernel kernel.elf -display none -vga std \
    -qmp "tcp:127.0.0.1:$PORT,server,nowait" -serial "file:$LOG" -name jt-mwkeyflash &

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
DOCK_ICON, DOCK_GAP, SLOT0_X = 37, 6, 247  # same dock constants as appclose-check.py
PITCH = DOCK_ICON + DOCK_GAP
ICON_ROW_Y = 487
REMINDERS_SLOT = 5  # Apps,Files,Mail,Calendar,Notes,Reminders,...

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

centre = SLOT0_X + REMINDERS_SLOT * PITCH + DOCK_ICON // 2
move(centre, ICON_ROW_Y); time.sleep(0.3); click(); time.sleep(1.0)  # open Reminders (multi-window)
after_open = count("mwchrome\n")

key("a"); time.sleep(0.3)  # start adding
for c in "milk":
    key(c)
time.sleep(0.3)
after_typing = count("mwchrome\n")

cmd({"execute": "quit"})
print("after_open=%d after_typing=%d" % (after_open, after_typing))
if after_open != 1:
    print("FAIL: expected exactly 1 chrome draw right after opening Reminders, got %d" % after_open); sys.exit(1)
if after_typing != 1:
    print("FAIL: expected chrome draw count to stay at 1 through 'a' + 'milk', got %d" % after_typing); sys.exit(1)
print("PASS: Reminders' multi-window chrome drew once on open and stayed flat through real typing")
PYEOF
STATUS=$?
cleanup
exit $STATUS
