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
#
# v0.76.58: this test was itself broken, real bug found by reading the
# actual kernel code (kernel.c's gui_launch_apps, contacts.h), not by
# trusting the earlier "Expected multiple prompt redraws, got 0" report:
#   It opened the Apps folder, then immediately pressed 'a'. gui_launch_apps
#   (kernel.c) navigates the grid with a/d/w/s (left/right/up/down) plus
#   Enter to launch, or digits '1'-'9' for the first 9 of 21 apps; 'a' there
#   means "move left" (a no-op at the leftmost cell), not "add a contact".
#   Contacts (icon 18) was never launched at all, so the typed characters
#   after it landed on the still-open grid, where they do nothing (no letter
#   shortcuts exist), and contacts_add/contacts_prompt_line (the only place
#   that emits "contactsprompt") never ran.
#
#   Contacts has no dock icon (GUI_DOCK_DEFAULT only covers icons 0-7) and
#   isn't in gui_multiwin_supported's list either, so the Apps-folder grid is
#   the only way to reach it -- no multi-window complication to route around
#   here, unlike Reminders/Mail in gui-prompt-keystroke-check.sh.
#
# Real flow now driven: open the Apps folder (dock slot 0), navigate the
# grid to icon 18 (row 3, col 3: right x3, down x3 from the top-left cell,
# the same cell math gui_launch_apps itself uses), press Enter to launch
# Contacts, press 'a' to open the add-contact name prompt (contacts.h:
# "if (k == 'a') { contacts_add(); continue; }"), then type real characters
# and verify "contactsprompt" grows once per keystroke.
#
# Proven discriminating: reverted contacts.h's 'a' gate so contacts_add()
# never ran (renamed the key check to an unreachable key) and reran --
# real FAIL, 0 redraws, since the prompt never opened; restored, real PASS.
set -e
cd "$(dirname "$0")/../.."
make -s kernel.elf

PORT=4457
LOG=$(mktemp /tmp/jt-contactskeypress-XXXX.log)

cleanup() { kill "$QEMU_PID" 2>/dev/null || true; rm -f "$LOG"; }
trap cleanup EXIT

qemu-system-i386 -kernel kernel.elf -display none -vga std \
    -qmp "tcp:127.0.0.1:$PORT,server,nowait" -serial "file:$LOG" -name jt-contactskeypress &
QEMU_PID=$!

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

# Open the Apps folder from the dock (first slot).
apps_centre = SLOT0_X + 0 * PITCH + DOCK_ICON // 2
move(apps_centre, ICON_ROW_Y); time.sleep(0.3)
click(); time.sleep(0.8)

# Navigate the grid to Contacts (icon 18, row 3 col 3): right x3, down x3
# from the top-left cell (index 0), the same layout math gui_launch_apps
# itself uses (row = i / APPS_COLS, col = i % APPS_COLS, APPS_COLS = 5).
for c in ("d", "d", "d", "s", "s", "s"):
    key(c)
key("ret"); time.sleep(0.6)  # launch Contacts

key("a")  # open the add-contact name prompt (contacts.h's real shortcut)
time.sleep(0.5)

before_prompt = prompt_count()

# Type 8 characters (should trigger 8+ prompt redraws)
for c in "testname":
    key(c)
    time.sleep(0.15)

after_typing = prompt_count()

# Press ESC to cancel the prompt, then ESC again to close Contacts.
key("esc"); time.sleep(0.3)
key("esc"); time.sleep(0.5)

cmd({"execute": "quit"})

if after_typing > before_prompt + 3:  # At least 4 renders (initial + 3+ keystrokes)
    print(f"PASS: Contacts prompt redraws on content changes ({after_typing - before_prompt} redraws for ~8 keystrokes)")
else:
    print(f"FAIL: Expected multiple prompt redraws, got {after_typing - before_prompt}")
    sys.exit(0)
PYEOF
)

echo "$RESULT"
echo "$RESULT" | grep -q "^PASS:" && exit 0
exit 1
