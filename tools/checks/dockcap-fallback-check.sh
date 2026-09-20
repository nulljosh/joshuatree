#!/bin/bash
# v0.76.56: real bug, found while investigating the roadmap's "clicking
# Calendar opens Reminders" / "Mail renders blank" live-QA reports.
# Re-verified via the QEMU monitor (gui_window_count/gui_windows dumped
# through the "xp" human-monitor-command, the app-interact-check.py
# pattern) that gui_order/gui_launch's icon dispatch is NOT the bug --
# every dock click computed the correct slot and the correct icon, both
# at press and at release.
#
# The real bug: GUI_MULTIWIN_MAX caps concurrent multi-window apps
# (Files/Weather/Mail/Calendar/Reminders) at 2. Once that cap is hit,
# gui_multiwin_open() correctly returns -1, but the click handler in
# gui_run() just dropped the click on the floor: no new window opened, no
# error, nothing closed, and the previously-topmost window (whatever it
# was) stayed exactly as it was. From the outside that is indistinguishable
# from "I clicked App X and got App Y" -- the exact live-QA symptom -- even
# though the true cause was a swallowed click at a window-count cap, not a
# wrong icon index.
#
# Fix: when gui_multiwin_open() returns -1, fall through to the existing
# blocking single-window path (gui_launch_from_dock, the same one
# unsupported apps already use) instead of dropping the click.
#
# Proven here via a new serial marker, "mwcapfallback\n" (only emitted on
# the fallback path): opens two multi-window apps (Files, Reminders,
# hitting the real cap of 2), then clicks a third dock icon (Mail) and
# demands the fallback marker actually fired and gui_window_count did NOT
# grow past 2 (the fallback path is the blocking single-window one, not a
# third multi-window slot).
#
# Proven discriminating: reverted the "< 0" fallback locally back to an
# unconditional gui_multiwin_open() call with no fallback, reran, got a
# real FAIL (marker count stayed 0, third click did nothing); restored,
# clean PASS.
set -e
cd "$(dirname "$0")/../.."
make -s kernel.elf

cleanup() { pkill -9 -f "qemu-system-i386.*jt-dockcap" >/dev/null 2>&1 || true; }
trap cleanup EXIT

PORT=4486
LOG=/tmp/jt-dockcap-check.log
rm -f "$LOG"
qemu-system-i386 -kernel kernel.elf -display none -vga std \
    -qmp "tcp:127.0.0.1:$PORT,server,nowait" -serial "file:$LOG" -name jt-dockcap &

python3 - "$PORT" "$LOG" <<'PYEOF'
import json, re, socket, subprocess, sys, time
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
FILES_SLOT, REMINDERS_SLOT, MAIL_SLOT = 1, 5, 2  # Apps,Files,Mail,Calendar,Notes,Reminders,...

def move(x, y):
    cmd({"execute": "input-send-event", "arguments": {"events": [
        {"type": "abs", "data": {"axis": "x", "value": int(x * 32768 / LOGICAL_W)}},
        {"type": "abs", "data": {"axis": "y", "value": int(y * 32768 / LOGICAL_H)}}]}})
def click():
    cmd({"execute": "input-send-event", "arguments": {"events": [{"type": "btn", "data": {"down": True, "button": "left"}}]}})
    time.sleep(0.1)
    cmd({"execute": "input-send-event", "arguments": {"events": [{"type": "btn", "data": {"down": False, "button": "left"}}]}})
def open_slot(slot):
    centre = SLOT0_X + slot * PITCH + DOCK_ICON // 2
    move(480, 200); time.sleep(0.2)
    move(centre, ICON_ROW_Y); time.sleep(0.3)
    click(); time.sleep(1.0)

symbols = {fields[2]: int(fields[0], 16) - 0xC0000000
           for line in subprocess.check_output(['nm', 'kernel.elf'], text=True).splitlines()
           if len(fields := line.split()) == 3}
def window_count():
    result = cmd({'execute': 'human-monitor-command', 'arguments': {
        'command-line': f"xp /4xb 0x{symbols['gui_window_count']:x}"}})['return']
    b = bytes(int(v, 16) for line in result.splitlines() if ':' in line
              for v in re.findall(r'0x([0-9a-f]{2})\b', line.split(':', 1)[1]))
    return int.from_bytes(b, 'little')

move(480, 200); time.sleep(0.5)
open_slot(FILES_SLOT)
open_slot(REMINDERS_SLOT)
after_two = window_count()
before_marker = count("mwcapfallback\n")

open_slot(MAIL_SLOT)  # cap already hit: must fall back, not drop the click
after_three = window_count()
after_marker = count("mwcapfallback\n")

cmd({"execute": "quit"})
print("after_two=%d after_three=%d marker_before=%d marker_after=%d" % (after_two, after_three, before_marker, after_marker))
if after_two != 2:
    print("FAIL: expected exactly 2 multi-window windows open (Files, Reminders), got %d" % after_two); sys.exit(1)
if after_marker != before_marker + 1:
    print("FAIL: expected the cap-fallback marker to fire exactly once for the third dock click, got %d -> %d" % (before_marker, after_marker)); sys.exit(1)
if after_three != 2:
    print("FAIL: gui_window_count grew past the real cap (%d), fallback should use the blocking single-window path, not a third multiwin slot" % after_three); sys.exit(1)
print("PASS: third dock click past the multi-window cap fell back to the blocking single-window path instead of being silently dropped")
PYEOF
STATUS=$?
cleanup
exit $STATUS
