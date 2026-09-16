#!/bin/bash
# v0.76.17: two direct reports, fixed together, same pass.
#
# 1. "It reflashes the screen when we hover a new item" (Apple-menu
#    dropdown, top-left corner button list: About/Files/Notes/Settings/
#    Restart/Shut Down). Root cause: gui_run()'s three repaint tiers (v40,
#    the exact "icons flash when I hover" fix for the DOCK) never got
#    extended to the Apple menu's own hover highlight -- cursor_only
#    explicitly excludes any menu_hover change, dock_only requires the
#    menu closed, so switching which row is highlighted fell through to
#    the full-repaint branch (whole photo blit + dock + every open window)
#    on every row hovered. Fix: a 4th tier, menu_only, a direct copy of
#    dock_only's shape using gui_draw_apple_menu (already self-contained,
#    same as gui_redraw_dock_band). Proven here via two serial markers:
#    "menuonly\n" (the new cheap tier) and "fullrepaint\n" (the expensive
#    one) -- hovering three different rows must grow the first and never
#    the second.
#
# 2. "Time in top right ... doesn't load when the minute or hour changes."
#    Root cause: gui_draw_menubar() already self-gated correctly on a real
#    minute change (gui_menubar_last_min), but was only ever CALLED from
#    mouse-in-menubar paths or the ten-minute weather cycle -- an idle
#    desktop with the cursor elsewhere could sit with a stale clock for up
#    to ten minutes. Fix: call it unconditionally every gui_run loop
#    iteration; its own gate makes that a no-op except the one frame the
#    minute actually ticks over. Polling the raw CMOS RTC registers at
#    ~100Hz instead of rarely also makes it far more likely to eventually
#    land inside the real MC146818 chip's once-a-second "Update In
#    Progress" window and read a torn value, a real, documented hardware
#    hazard (see cmos_read_time_stable's own comment) applied proactively
#    here, not one this session caught this exact QEMU reproducing.
#    Proven two ways via "menubarredraw\n" (only emitted past the
#    self-gate, i.e. a real accepted minute change): first, a plain boot
#    with the mouse held still for 4 real seconds must not grow the count
#    by more than the one real rollover that window could plausibly
#    contain (guards against a torn-read regression, even though this
#    kernel's own repeated test runs never tripped it); second, a QEMU
#    booted with its RTC seeded 10s before a minute boundary must grow
#    the count again within the following 12s, with the mouse still never
#    touched -- proving the redraw isn't just absent, it's actually
#    reachable on a real minute change.
#
# Proven discriminating for the menu-hover half: reverting the menu_only
# tier back to falling through (commenting the branch out) locally made
# fullrepaint grow by 3 across three hovers instead of 0 -- a clean FAIL;
# restored, clean PASS. The clock half's positive check (before=0,
# after=2) is real and reproduces every run; its torn-read guard is a
# real, standard hardware-correctness fix that this session could not
# force a failing repro for (QEMU's own CMOS model didn't exhibit the
# race in repeated local runs), so treat that specific line of defense as
# verified by inspection against the documented erratum, not by a local
# red/green cycle.
set -e
cd "$(dirname "$0")/../.."
make -s kernel.elf

cleanup() { pkill -9 -f "qemu-system-i386.*jt-menuclock" >/dev/null 2>&1 || true; }
trap cleanup EXIT

PORT=4483
LOG=/tmp/jt-menuclock-check.log
rm -f "$LOG"
qemu-system-i386 -kernel kernel.elf -display none -vga std \
    -qmp "tcp:127.0.0.1:$PORT,server,nowait" -serial "file:$LOG" -name jt-menuclock &

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
time.sleep(2.0)

def move(x, y):
    cmd({"execute": "input-send-event", "arguments": {"events": [
        {"type": "abs", "data": {"axis": "x", "value": int(x * 32768 / 960)}},
        {"type": "abs", "data": {"axis": "y", "value": int(y * 32768 / 540)}}]}})
def click():
    cmd({"execute": "input-send-event", "arguments": {"events": [{"type": "btn", "data": {"down": True, "button": "left"}}]}})
    time.sleep(0.1)
    cmd({"execute": "input-send-event", "arguments": {"events": [{"type": "btn", "data": {"down": False, "button": "left"}}]}})

# --- clock: baseline redraw count, then 4 real seconds untouched ---
redraws_t0 = count("menubarredraw\n")
time.sleep(4.0)
redraws_t1 = count("menubarredraw\n")

# --- Apple menu hover flash ---
move(16, 13); time.sleep(0.2); click(); time.sleep(0.5)  # open menu (logo hit-box)
after_open_full = count("fullrepaint\n")
after_open_menu = count("menuonly\n")
for y in (45, 67, 89):  # About / Files / Notes rows
    move(94, y); time.sleep(0.4)
after_hover_full = count("fullrepaint\n")
after_hover_menu = count("menuonly\n")

cmd({"execute": "quit"})
print("redraws_t0=%d redraws_t1=%d after_open_full=%d after_open_menu=%d after_hover_full=%d after_hover_menu=%d" %
      (redraws_t0, redraws_t1, after_open_full, after_open_menu, after_hover_full, after_hover_menu))

if redraws_t1 - redraws_t0 > 1:
    print("FAIL: menubarredraw fired %d times in 4 idle seconds with the mouse untouched -- torn CMOS reads are back" %
          (redraws_t1 - redraws_t0)); sys.exit(1)
if after_hover_full != after_open_full:
    print("FAIL: hovering three menu rows grew the full-repaint count (%d -> %d), the flash is back" %
          (after_open_full, after_hover_full)); sys.exit(1)
if after_hover_menu <= after_open_menu:
    print("FAIL: hovering three menu rows never grew the cheap menu_only tier's count (%d -> %d)" %
          (after_open_menu, after_hover_menu)); sys.exit(1)
print("PASS: menu hover stays on the cheap tier, and the clock's redraw count stayed sane with no torn reads")
PYEOF
STATUS=$?
cleanup
if [ "$STATUS" -ne 0 ]; then exit "$STATUS"; fi

# --- Part 2: positive proof a real minute rollover still redraws the
# clock with the mouse untouched (RTC seeded 2s before a minute boundary,
# same "no mouse movement at all" shape the negative check above used). ---
PORT2=4484
LOG2=/tmp/jt-menuclock-check2.log
rm -f "$LOG2"
qemu-system-i386 -kernel kernel.elf -display none -vga std \
    -rtc "base=2024-01-01T00:00:50" \
    -qmp "tcp:127.0.0.1:$PORT2,server,nowait" -serial "file:$LOG2" -name jt-menuclock &

python3 - "$PORT2" "$LOG2" <<'PYEOF'
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
time.sleep(0.5)  # RTC seeded 10s before the minute boundary, capture baseline early
before = count("menubarredraw\n")
time.sleep(12.0)  # no mouse movement at all in this window; the boundary must land inside it
after = count("menubarredraw\n")
cmd({"execute": "quit"})
print("before=%d after=%d" % (before, after))
if after <= before:
    print("FAIL: the minute rolled over (RTC seeded at :50, waited 12s) but menubarredraw never fired again with the cursor untouched"); sys.exit(1)
print("PASS: clock live update")
PYEOF
STATUS2=$?
cleanup
exit $STATUS2
