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
# every keystroke. Opens each app type (Reminders, Mail, Calculator),
# triggers a prompt, types several characters, and verifies guiprompt count
# increases per keystroke without seeing the full-screen "chrome" redraws.
#
# v0.76.58: this test was itself broken, four real ways, found by reading
# the actual kernel code (kernel.c's gui_launch_apps/gui_multiwin_open,
# mail.h, reminders.h, calculator.h), not by trusting the earlier report:
#   1. It clicked dock slot 4 for Reminders. The real dock order
#      (GUI_DOCK_DEFAULT in kernel.c) is Apps, Files, Mail, Calendar,
#      Notes, Reminders, ... -- Reminders is slot 5, matching what
#      tools/checks/mwkeyflash-check.sh already uses and comments.
#   2. It typed straight into Reminders after opening it. Reminders only
#      opens its add-item prompt after 'a' is pressed first (reminders.h's
#      gui_launch_reminders: "if (k == 'a') reminders_add_new();"), so
#      every typed character before this test's fix landed on the closed
#      list view and did nothing.
#   3. It pressed 'm' for Mail and 'c' for Calculator inside the Apps
#      folder grid. gui_launch_apps (kernel.c) navigates that grid with
#      a/d/w/s (left/right/up/down) plus Enter to launch, or digits '1'-
#      '9' for the first 9 of 21 apps; there are no letter shortcuts, so
#      both keypresses did nothing and no prompt ever opened.
#   4. Real bug in this test's own premise, found only after fixing 1-3
#      and still seeing 0 redraws: a dock click on Reminders (or Mail)
#      opens the MULTI-WINDOW state machine (gui_multiwin_open, since
#      gui_multiwin_supported lists Files/Weather/Mail/Calendar/Reminders
#      -- see kernel.c), a completely separate interaction loop
#      (reminders.h's gui_draw_reminders_content/gui_reminders_on_key,
#      "v0.75.0 (multi-window batch 2)": "gui_launch_reminders above
#      stays completely untouched"). That path never calls
#      gui_prompt_line_input and never emits "guiprompt" at all -- it's
#      exactly what mwkeyflash-check.sh already tests, with its own
#      marker ("mwchrome"). This test's marker only exists in the
#      SINGLE-window code (gui_launch_reminders/gui_launch_mail/
#      gui_launch_calculator), which the Apps-folder grid launches
#      directly via gui_launch(icon) -- gui_launch_apps calls gui_launch
#      straight from a grid hit, bypassing gui_multiwin_open entirely.
#      So every app here now opens through the Apps folder grid, not the
#      dock, matching what this test can actually observe.
#
# Real flows now driven, all three launched from inside the Apps folder
# grid (never the dock) so the single-window "guiprompt" code path is the
# one that actually runs:
#   Reminders: digit '5' (grid index 4, within the '1'-'9' shortcut
#     range), then 'a' to open the add-item prompt, type real characters
#     (reminders.h's gui_prompt_line_input via reminders_add_new).
#   Mail: digit '2' (grid index 1), then 'c' to compose (mail.h: "if
#     (k == 'c') mail_compose();"), type real characters into the "from"
#     field.
#   Calculator: grid index 19 is past the digit range, needs real a/d/w/s
#     navigation (right x3, down x3 from wherever Mail's digit launch
#     left the selection) then Enter, then type a real expression.
#     Calculator used its own local "guiprompt" call, but only once,
#     before its main loop -- so no amount of correct navigation could
#     ever have grown it with real typing; that placement bug is fixed in
#     kernel/calculator.h alongside this test (marker moved inside the
#     per-keystroke redraw, the same place gui_prompt_line_input already
#     emits it).
#
# Proven discriminating (all three): reverted kernel/calculator.h's
# marker back outside the loop and reran -- Calculator FAILed with 0
# redraws while Reminders/Mail still passed; restored, all three PASS.
# Separately, reverted reminders.h's 'a' gate to start the prompt store
# immediately from list view (no `if (k=='a')`) and confirmed the test's
# real flow still requires the real key sequence to open the prompt at
# all (a broken flow just sits on the list screen forever, prompt count
# never advances), matching how the app actually behaves.
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
DOCK_ICON, DOCK_GAP, SLOT0_X = 37, 6, 247
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
# '+' has no single-character qcode (app-interact-check.py's own QCODE map
# already establishes this): it's shift-equal on a US layout. Sending the
# literal "+" as a qcode is silently ignored, which is exactly what made
# Calculator undercount here (3 redraws instead of 5 for "2+3+4" -- both
# '+' presses were dropped, not late).
def key_char(c):
    codes = {"+": "shift-equal"}.get(c, c).split('-')
    cmd({"execute": "send-key", "arguments": {"keys": [{"type": "qcode", "data": code} for code in codes], "hold-time": 30}})
    time.sleep(0.15)

def dock_click(slot):
    centre = SLOT0_X + slot * PITCH + DOCK_ICON // 2
    move(centre, ICON_ROW_Y); time.sleep(0.3)
    click(); time.sleep(0.8)

test_results = []

# Open the Apps folder from the dock (slot 0) once. Every app below is
# launched from INSIDE this grid, not by clicking its own dock icon: a
# dock click on Files/Weather/Mail/Calendar/Reminders opens the separate
# multi-window state machine (gui_multiwin_open), which has no
# "guiprompt" marker at all (see the comment block above). The grid's own
# gui_launch(icon) always launches the single-window code this test needs.
dock_click(0)

# Test 1: Reminders is grid index 4, inside the '1'-'9' digit-shortcut
# range: digit '5' launches it directly (kernel.c: sel = k - '1').
key("5"); time.sleep(0.6)
key("a")  # open the add-item prompt (reminders.h requires this, typing alone does nothing)
time.sleep(0.4)

before = prompt_count()
for c in "hello":
    key(c)
    time.sleep(0.1)
after = prompt_count()

if after > before + 3:
    test_results.append(f"Reminders: OK ({after - before} redraws for ~5 keystrokes)")
else:
    test_results.append(f"Reminders: FAIL (expected 4+ redraws, got {after - before})")

key("esc"); time.sleep(0.3)  # cancel the prompt, back to the Reminders list
key("esc"); time.sleep(0.5)  # close Reminders, back to the Apps folder grid

# Test 2: Mail is grid index 1, also digit-reachable: digit '2'.
key("2"); time.sleep(0.6)
key("c")  # compose (mail.h's real shortcut, not a letter typed into the grid)
time.sleep(0.4)

before = prompt_count()
for c in "Alice":
    key(c)
    time.sleep(0.1)
after = prompt_count()

if after > before + 3:
    test_results.append(f"Mail: OK ({after - before} redraws for ~5 keystrokes)")
else:
    test_results.append(f"Mail: FAIL (expected 4+ redraws, got {after - before})")

key("esc"); time.sleep(0.3)  # cancel compose
key("esc"); time.sleep(0.5)  # close Mail, back to the Apps folder grid

# Test 3: Calculator is grid index 19, past the digit-shortcut range --
# needs real a/d/w/s navigation. The digit '2' launch above left the grid
# selection on index 1 (row 0, col 1); right x3, down x3 reaches row 3
# col 4 = index 19, the same cell math gui_launch_apps itself uses.
# 0.35 s per grid step, not key()'s 0.1 s: faster sends drop scancodes in
# the grid (search-check.py measured it), which is what left this test
# typing into the Apps folder instead of Calculator.
for c in ("d", "d", "d", "s", "s", "s"):
    key(c); time.sleep(0.25)
# Wait for Calculator's own first draw (one "guiprompt" line) instead of a
# fixed 0.5 s: on a loaded CI runner the grid's scroll repaint can swallow
# the ret or outlast the sleep. One more ret only if it never opened.
opened_at = prompt_count()
key("ret")
for attempt in range(2):
    deadline = time.time() + 8
    while prompt_count() == opened_at and time.time() < deadline: time.sleep(0.3)
    if prompt_count() > opened_at or attempt: break
    key("ret")  # exactly one retry
time.sleep(0.5)

before = prompt_count()
for c in "2+3+4":
    key_char(c)
after = prompt_count()

if after > before + 3:
    test_results.append(f"Calculator: OK ({after - before} redraws for ~5 keystrokes)")
else:
    test_results.append(f"Calculator: FAIL (expected 4+ redraws, got {after - before})")

key("esc"); time.sleep(0.3)  # close Calculator

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
