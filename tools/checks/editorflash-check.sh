#!/bin/bash
# v0.76.10: direct report + a real screen recording, "[Notes] redraws the
# entire screen on every keystroke." Notes is a ring-3 window now (user/notes.c),
# and its editor splits chrome (title, footer hint, on-screen keyboard) from the
# text area: ed_draw() clears and repaints only the text area on a plain key, and
# ed_chrome() does the full clear plus chrome, writing the "editorchrome" marker
# (one ring-3 write, so the serial line ends in "editorchrome\n") once per real
# chrome paint. The chrome is painted on open, after a resize, and when what it
# says changes.
#
# This proves it holds for real: boots with `open=notes`, presses n for a fresh
# note, and demands the chrome-paint count is exactly 1 after the editor opens;
# types 20 plain characters (no toolbar-like key) and demands it settles at
# exactly 2 -- the one real, legitimate change on the very first keystroke (the
# dirty flag flips the title to "Notes *"), then nothing across keystrokes 2-20.
# Then Ctrl+S (a real chrome-relevant change: the footer says "Saved." and the
# title drops its star) and demands the count becomes exactly 3, proving the
# mechanism still repaints chrome when chrome-relevant state genuinely changes,
# not just "never repaints again."
source "$(dirname "$0")/freeport.sh"
set -e
cd "$(dirname "$0")/../.."
make -s kernel.elf

PORT=$(free_port)
LOG=$(mktemp /tmp/jt-editorflash-XXXX.log)

qemu-system-i386 -kernel kernel.elf -display none -vga std -append "open=notes" \
    -qmp "tcp:127.0.0.1:$PORT,server,nowait" -serial "file:$LOG" &
QEMU_PID=$!
trap 'kill "$QEMU_PID" 2>/dev/null || true; rm -f "$LOG"' EXIT

RESULT=$(python3 - "$PORT" "$LOG" <<'PYEOF'
import json, socket, sys, time
port = int(sys.argv[1])
log_path = sys.argv[2]

def chrome_count():
    try:
        with open(log_path) as f:
            return f.read().count("editorchrome\n")
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

def key(*qcodes):
    cmd({"execute": "send-key", "arguments": {"keys": [{"type": "qcode", "data": q} for q in qcodes], "hold-time": 30}})
    time.sleep(0.1)

def serial():
    try:
        with open(log_path) as f:
            return f.read()
    except FileNotFoundError:
        return ""

def wait_for(needle, secs):
    for _ in range(int(secs * 10)):
        if needle in serial(): return True
        time.sleep(0.1)
    return False

# `open=notes` opens the ring-3 Notes window the moment the desktop is up.
if not wait_for("notes: ring-3 window", 60):
    print("FAIL: Notes never opened (no 'notes: ring-3 window' on serial)"); cmd({"execute": "quit"}); sys.exit(0)
time.sleep(1.5)
key("n")   # a fresh empty note, opened in the editor
if not wait_for("notes: edit=", 10):
    print("FAIL: n did not open a note in the editor"); cmd({"execute": "quit"}); sys.exit(0)
time.sleep(1.0)

after_open = chrome_count()

for c in "abcdefghijklmnopqrst":
    key(c)
time.sleep(0.5)
after_typing = chrome_count()

key("ctrl", "s")
# Wait for the save's redraw to land instead of reading after a fixed 0.3 s:
# on a slow CI runner the read beat the redraw and saw 2.
for _ in range(50):
    time.sleep(0.1)
    after_save = chrome_count()
    if after_save != after_typing: break

cmd({"execute": "quit"})

print("after_open=%d after_typing=%d after_save=%d" % (after_open, after_typing, after_save))
if after_open != 1:
    print("FAIL: expected exactly 1 chrome redraw right after opening Notes, got %d" % after_open); sys.exit(0)
if after_typing != 2:
    print("FAIL: expected chrome redraw count to settle at 2 (open + the one real dirty-flag flip) after 20 plain keystrokes, got %d" % after_typing); sys.exit(0)
if after_save != 3:
    print("FAIL: expected chrome redraw count to become 3 after Ctrl+S (a real footer and title change), got %d" % after_save); sys.exit(0)
print("PASS: chrome redrew on open and on the one real dirty-flag flip, stayed flat through the remaining plain keystrokes, and redrew again on a real chrome change (Ctrl+S)")
PYEOF
)

kill "$QEMU_PID" 2>/dev/null || true
wait "$QEMU_PID" 2>/dev/null || true

echo "$RESULT"
echo "$RESULT" | grep -q "^PASS:" && exit 0
exit 1
