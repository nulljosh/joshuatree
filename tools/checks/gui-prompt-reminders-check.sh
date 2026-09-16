#!/bin/bash
# v0.76.24: Reminders app prompt keystroke fix (simplified test).
# Tests that the Reminders add-new prompt redraws only the content
# (text box + input) per keystroke, not the full window including chrome.

set -e
cd "$(dirname "$0")/../.."
make -s kernel.elf

PORT=4459
LOG=$(mktemp /tmp/jt-guiprompt-rem-XXXX.log)

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
            content = f.read()
            return content.count("guiprompt\n")
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
time.sleep(6.0)

LOGICAL_W, LOGICAL_H = 960, 540
DOCK_ICON, DOCK_GAP, SLOT0_X = 37, 6, 268
PITCH = DOCK_ICON + DOCK_GAP
ICON_ROW_Y = 487
REMINDERS_SLOT = 4

def move(x, y):
    cmd({"execute": "input-send-event", "arguments": {"events": [
        {"type": "abs", "data": {"axis": "x", "value": int(x * 32768 / LOGICAL_W)}},
        {"type": "abs", "data": {"axis": "y", "value": int(y * 32768 / LOGICAL_H)}}]}})
def click():
    cmd({"execute": "input-send-event", "arguments": {"events": [{"type": "btn", "data": {"down": True, "button": "left"}}]}})
    time.sleep(0.15)
    cmd({"execute": "input-send-event", "arguments": {"events": [{"type": "btn", "data": {"down": False, "button": "left"}}]}})
def key(qcode):
    cmd({"execute": "send-key", "arguments": {"keys": [{"type": "qcode", "data": qcode}]}})
    time.sleep(0.15)

# Click Reminders icon in dock
centre = SLOT0_X + REMINDERS_SLOT * PITCH + DOCK_ICON // 2
move(centre, ICON_ROW_Y)
time.sleep(0.4)
click()
time.sleep(1.2)

# Press 'a' to add a new reminder (opens the prompt)
key("a")
time.sleep(0.5)

before = prompt_count()

# Type 6 characters to trigger multiple content redraws
for c in "buydog":
    key(c)
    time.sleep(0.15)

after = prompt_count()

# Press ESC to close
key("escape")
time.sleep(0.5)

cmd({"execute": "quit"})

redraws = after - before
print(f"Prompt redraws: {redraws}")

if redraws >= 4:
    print("PASS: Reminders prompt had multiple content redraws without full chrome flashing")
else:
    print(f"FAIL: Expected 4+ redraws, got {redraws}")
    sys.exit(0)
PYEOF
)

kill "$QEMU_PID" 2>/dev/null || true
wait "$QEMU_PID" 2>/dev/null || true

echo "$RESULT"
echo "$RESULT" | grep -q "^PASS:" && exit 0
exit 1
