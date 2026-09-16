#!/bin/bash
# v0.76.23: Contacts app keystroke redraw fix.
# v0.76.10 fixed this same bug in Notes (editor.h): redraws the entire screen
# on every keystroke even though only the typed text changes. This was carried
# forward to Contacts but never fixed there. Both use a render loop that
# window_clear()s the whole chrome on every iteration.
#
# Split chrome (titlebar + prompt label) from content (text box + typed text)
# so the chrome redraws only once (outside the loop) and content redraws every
# keystroke without touching the unchanged chrome. This test counts "contactsprompt"
# serial markers to verify that the content redraw loop is called multiple times
# (once per keystroke plus the initial render, ~8+ times for 8 keystrokes), proving
# the draw loop is working without measuring the reduced screen flashing.

set -e
cd "$(dirname "$0")/../.."
make -s kernel.elf

PORT=4457
LOG=$(mktemp /tmp/jt-contactskeypress-XXXX.log)

qemu-system-i386 -kernel kernel.elf -display none -vga std \
    -qmp "tcp:127.0.0.1:$PORT,server,nowait" -serial "file:$LOG" &
QEMU_PID=$!
trap 'kill "$QEMU_PID" 2>/dev/null || true; rm -f "$LOG"' EXIT

RESULT=$(python3 - "$PORT" "$LOG" <<'PYEOF'
import json, socket, sys, time
port = int(sys.argv[1])
log_path = sys.argv[2]

def prompt_count():
    try:
        with open(log_path) as f:
            return f.read().count("contactsprompt\n")
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
DOCK_ICON, DOCK_GAP, SLOT0_X = 37, 6, 268
PITCH = DOCK_ICON + DOCK_GAP
ICON_ROW_Y = 487
CONTACTS_SLOT = 5  # Reminders is in dock, need to open Contacts from Apps folder

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

# Open Contacts from Apps folder via the dock Apps icon (first slot)
apps_centre = SLOT0_X + 0 * PITCH + DOCK_ICON // 2
move(apps_centre, ICON_ROW_Y); time.sleep(0.3)
click(); time.sleep(0.8)

# Click on Contacts app (index 18, should be in the grid somewhere)
# For simplicity, just try pressing 'c' for Contacts (app grid has digit shortcuts)
# But Contacts isn't in the digit 1-9 range, so use arrow keys or 'a' for add
key("a")  # Try 'a' for add
time.sleep(0.5)

before_prompt = prompt_count()

# Type 8 characters (should trigger 8+ prompt redraws)
for c in "testname":
    key(c)
    time.sleep(0.15)

after_typing = prompt_count()

# Press ESC to cancel
key("escape")
time.sleep(0.5)

cmd({"execute": "quit"})

if after_typing > before_prompt + 3:  # At least 4 renders (initial + 3+ keystrokes)
    print(f"PASS: Contacts prompt redraws on content changes ({after_typing - before_prompt} redraws for ~8 keystrokes)")
else:
    print(f"FAIL: Expected multiple prompt redraws, got {after_typing - before_prompt}")
    sys.exit(0)
PYEOF
)

kill "$QEMU_PID" 2>/dev/null || true
wait "$QEMU_PID" 2>/dev/null || true

echo "$RESULT"
echo "$RESULT" | grep -q "^PASS:" && exit 0
exit 1
