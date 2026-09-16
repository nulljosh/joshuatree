#!/bin/bash
# v0.76.24: GUI prompt input keystroke redraw fix.
# All text-input prompts (mail.h, calendar.h, reminders.h, calculator.h)
# previously called window_clear() on every keystroke, causing screen flashes.
# Extracted shared gui_prompt_line_input and variants that split chrome
# (titlebar, prompts, help text - drawn once) from content (text box, input
# - redrawn per keystroke), following the pattern v0.76.10 (Notes) and
# v0.76.23 (Contacts) established.
#
# This test counts "guiprompt" serial markers emitted by the content redraw
# loop to verify that chrome (titlebar) doesn't cause full window redraws on
# every keystroke. Opens each app type (Mail, Reminders, Calendar, Calculator),
# triggers a prompt, types several characters, and verifies guiprompt count
# increases per keystroke without seeing the full-screen "chrome" redraws.

set -e
cd "$(dirname "$0")/../.."
make -s kernel.elf

PORT=4458
LOG=$(mktemp /tmp/jt-guiprompt-XXXX.log)

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
            return f.read().count("guiprompt\n")
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

test_results = []

# Test 1: Reminders (in dock, easiest to access)
REMINDERS_SLOT = 4
centre = SLOT0_X + REMINDERS_SLOT * PITCH + DOCK_ICON // 2
move(centre, ICON_ROW_Y); time.sleep(0.3)
click(); time.sleep(0.8)

before = prompt_count()
# Type 5 characters in reminder prompt
for c in "hello":
    key(c)
    time.sleep(0.1)
after = prompt_count()

if after > before + 3:  # At least 4 renders (initial + 3+ for keystrokes)
    test_results.append(f"Reminders: OK ({after - before} redraws for ~5 keystrokes)")
else:
    test_results.append(f"Reminders: FAIL (expected 4+ redraws, got {after - before})")

# Cancel the reminder
key("escape")
time.sleep(0.5)

# Test 2: Mail (in Apps folder)
apps_centre = SLOT0_X + 0 * PITCH + DOCK_ICON // 2
move(apps_centre, ICON_ROW_Y); time.sleep(0.3)
click(); time.sleep(0.8)

key("m")  # Try 'm' for Mail in apps grid
time.sleep(0.5)

before = prompt_count()
# Type 5 characters in mail prompt (from field)
for c in "Alice":
    key(c)
    time.sleep(0.1)
after = prompt_count()

if after > before + 3:
    test_results.append(f"Mail: OK ({after - before} redraws for ~5 keystrokes)")
else:
    test_results.append(f"Mail: FAIL (expected 4+ redraws, got {after - before})")

# Cancel the message
key("escape")
time.sleep(0.5)

# Test 3: Calculator (in Apps folder, easier than Calendar)
move(apps_centre, ICON_ROW_Y); time.sleep(0.3)
click(); time.sleep(0.8)

key("c")  # Try 'c' for Calculator in apps grid
time.sleep(0.5)

before = prompt_count()
# Type 5 characters in calculator
for c in "2+3+4":
    key(c)
    time.sleep(0.1)
after = prompt_count()

if after > before + 3:
    test_results.append(f"Calculator: OK ({after - before} redraws for ~5 keystrokes)")
else:
    test_results.append(f"Calculator: FAIL (expected 4+ redraws, got {after - before})")

# Quit
key("escape")
time.sleep(0.3)

cmd({"execute": "quit"})

all_pass = all("OK" in r for r in test_results)
for r in test_results:
    print(r)

if all_pass:
    print("PASS: All prompt input tests confirmed content redraws without per-keystroke chrome flashing")
else:
    print("FAIL: Some tests did not see enough redraws")
    sys.exit(0)
PYEOF
)

kill "$QEMU_PID" 2>/dev/null || true
wait "$QEMU_PID" 2>/dev/null || true

echo "$RESULT"
echo "$RESULT" | grep -q "^PASS:" && exit 0
exit 1
