#!/bin/bash
# Issue #13: Weather app broken. Root cause: when the fetch failed (NIC up,
# no route/DNS), gui_draw_weather_content re-ran the blocking fetch on EVERY
# repaint, freezing the window. Headless proof: boot with a NIC whose SLIRP
# has no outside access (restrict=on), open Weather from the dock three times, and count "wxfetch" serial lines. Fixed kernel:
# 1 (boot cycle only). Old kernel: 4 (one per open).

set -e
cd "$(dirname "$0")/../.."
make -s kernel.elf

PORT=4455
LOG=$(mktemp /tmp/jt-weatherapp-XXXX.log)

qemu-system-i386 -kernel kernel.elf -display none -vga std \
    -net nic,model=rtl8139 -net user,restrict=on -qmp "tcp:127.0.0.1:$PORT,server,nowait" -serial "file:$LOG" &
QEMU_PID=$!
trap 'kill "$QEMU_PID" 2>/dev/null || true; rm -f "$LOG"' EXIT

RESULT=$(python3 - "$PORT" "$LOG" <<'PYEOF'
import json, socket, sys, time
port = int(sys.argv[1])
log_path = sys.argv[2]

def fetch_count():
    try:
        return open(log_path).read().count("wxfetch")
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
time.sleep(15.0)

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

# Open and close Weather three times; each open repaints the content
centre = SLOT0_X + WEATHER_SLOT * PITCH + DOCK_ICON // 2
for _ in range(3):
    move(centre, ICON_ROW_Y); time.sleep(0.3)
    click(); time.sleep(2.0)
    cmd({"execute": "send-key", "arguments": {"keys": [{"type": "qcode", "data": "q"}]}})
    time.sleep(1.0)

cmd({"execute": "quit"})

n = fetch_count()
if n == 1:
    print("PASS: Weather fetch attempted %d time(s) across 3 opens" % n)
else:
    print("FAIL: weather_fetch ran %d times (want 1): failed fetch retried per repaint" % n)
PYEOF
)

kill "$QEMU_PID" 2>/dev/null || true
wait "$QEMU_PID" 2>/dev/null || true

echo "$RESULT"
echo "$RESULT" | grep -q "^PASS:" && exit 0
exit 1
