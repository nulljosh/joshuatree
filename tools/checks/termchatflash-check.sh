#!/bin/bash
# v0.76.11: direct report, "every keystroke causes page to re-render" was
# still reproducing on the landing demo after v0.76.10's Notes-only fix.
# Root cause: term_render() (kernel.c) and chat.h's gui_launch_chat_app/
# chat_prompt_line had the identical shape v0.76.10 fixed for Notes --
# a full window_clear() plus gui_draw_app_titlebar() redraw on every
# single keystroke, not just a real dirty-flag flip -- and were never
# touched. Both apps' titlebar text never changes at all (no toggling
# title like Notes' "Notes *"), so the fix here is simpler: chrome draws
# exactly once per open (serial_puts("termchrome\n") / "chatchrome\n"),
# never again per keystroke; only the content region redraws on typing.
#
# This proves the fix holds for real: opens Terminal, types 4 plain
# characters ("help"), demands the chrome marker settle at exactly 1
# (drawn on open, never again); opens Chat, presses 'n' (enters compose)
# then types 3 plain characters, demands its own chrome marker also
# settle at exactly 1.
#
# Proven discriminating: reverted term_draw_chrome()'s call site back
# inside the loop (matching the old bug shape) locally while building
# this fix, reran, got termchrome=5 after "help" (1 open + 4 keystrokes)
# instead of 1 -- a clean FAIL; restored, clean PASS.
set -e
cd "$(dirname "$0")/../.."
make -s kernel.elf

PORT=4472
LOG=$(mktemp /tmp/jt-termchatflash-XXXX.log)

qemu-system-i386 -kernel kernel.elf -display none -vga std \
    -qmp "tcp:127.0.0.1:$PORT,server,nowait" -serial "file:$LOG" &
QEMU_PID=$!
trap 'kill "$QEMU_PID" 2>/dev/null || true; rm -f "$LOG"' EXIT

RESULT=$(python3 - "$PORT" "$LOG" <<'PYEOF'
import json, socket, sys, time
port = int(sys.argv[1])
log_path = sys.argv[2]

def count(marker):
    try:
        with open(log_path) as f:
            return f.read().count(marker)
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
ICON_ROW_Y = 488
TERMINAL_SLOT, CHAT_SLOT = 6, 7

def move(x, y):
    cmd({"execute": "input-send-event", "arguments": {"events": [
        {"type": "abs", "data": {"axis": "x", "value": int(x * 32768 / LOGICAL_W)}},
        {"type": "abs", "data": {"axis": "y", "value": int(y * 32768 / LOGICAL_H)}}]}})
def click():
    cmd({"execute": "input-send-event", "arguments": {"events": [{"type": "btn", "data": {"down": True, "button": "left"}}]}})
    time.sleep(0.1)
    cmd({"execute": "input-send-event", "arguments": {"events": [{"type": "btn", "data": {"down": False, "button": "left"}}]}})
def click_at(x, y):
    move(x, y); time.sleep(0.3)
    click(); time.sleep(0.5)
def key(qcode):
    cmd({"execute": "send-key", "arguments": {"keys": [{"type": "qcode", "data": qcode}]}})
    time.sleep(0.1)

centre = lambda slot: SLOT0_X + slot * PITCH + DOCK_ICON // 2

# Terminal: open, type "help" (4 plain keystrokes), check chrome count.
click_at(centre(TERMINAL_SLOT), ICON_ROW_Y); time.sleep(1.0)
after_term_open = count("termchrome\n")
for c in "help":
    key(c)
time.sleep(0.3)
after_term_typing = count("termchrome\n")
click_at(94, 56)  # close via its own X
time.sleep(0.5)

# Chat: open, 'n' (enters compose), type 3 plain characters, check chrome count.
click_at(centre(CHAT_SLOT), ICON_ROW_Y); time.sleep(1.0)
after_chat_open = count("chatchrome\n")
key("n"); time.sleep(0.3)
for c in "yes":
    key(c)
time.sleep(0.3)
after_chat_typing = count("chatchrome\n")
key("esc"); time.sleep(0.3)
click_at(94, 56)  # close via its own X

cmd({"execute": "quit"})

print("after_term_open=%d after_term_typing=%d after_chat_open=%d after_chat_typing=%d" %
      (after_term_open, after_term_typing, after_chat_open, after_chat_typing))
if after_term_open != 1:
    print("FAIL: expected exactly 1 Terminal chrome draw right after opening, got %d" % after_term_open); sys.exit(0)
if after_term_typing != 1:
    print("FAIL: expected Terminal chrome draw count to stay at 1 after typing 'help', got %d" % after_term_typing); sys.exit(0)
if after_chat_open != 1:
    print("FAIL: expected exactly 1 Chat chrome draw right after opening, got %d" % after_chat_open); sys.exit(0)
if after_chat_typing != 1:
    print("FAIL: expected Chat chrome draw count to stay at 1 after entering compose and typing, got %d" % after_chat_typing); sys.exit(0)
print("PASS: Terminal and Chat chrome each drew exactly once on open and stayed flat through real typing")
PYEOF
)

kill "$QEMU_PID" 2>/dev/null || true
wait "$QEMU_PID" 2>/dev/null || true

echo "$RESULT"
echo "$RESULT" | grep -q "^PASS:" && exit 0
exit 1
