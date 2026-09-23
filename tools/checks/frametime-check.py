#!/usr/bin/env python3
"""Issue #14 ("everything super laggy, hardly usable"): the permanent
frame-time gate. Same headless QMP shape as dockhover-check.py (mouse
moved across the dock) and editor_qa.py (symbols read by `nm` + the QMP
`xp` monitor command, not guessed offsets), but instead of asserting on
pixels it reads the real instrument drivers/window.c now keeps:
`window_present_ticks_last` (PIT ticks, 100Hz, kernel/irq.h's ticks(),
see window.c's own comment) and `window_present_ticks_max`, both stamped
by every real window_present().

Three scenarios, each polling window_present_ticks_last as fast as QMP
allows for a fixed real-time window and keeping the max seen, since any
ambient redraw (the minute clock, the wind sway) can overwrite the symbol
between one poll and the next:

  1. idle desktop  -- cursor parked on open wallpaper, not moving: only
     ambient redraws (wind sway, the minute clock) present at all.
  2. dock hover     -- the pointer hops across five dock icons, the same
     move sequence dockhover-check.py already drives.
  3. window open    -- Files (dock slot 1) clicked open from the desktop.

Prints ticks-per-present for all three plus the session's running max
(`window_present_ticks_max`, whatever single present cost the most, dock
hover, an app open or the boot's own first full-desktop frame), and FAILS
if any scenario or the running max is over its budget.

Usage: tools/checks/frametime-check.py   (from the repo root, after make kernel.elf)
"""
import json, os, re, shutil, socket, subprocess, sys, time

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
os.chdir(ROOT)
LOG = "/tmp/jt-frametime-serial.log"
PORT = 4670
LOGICAL_W, LOGICAL_H, SCALE = 960, 540, 2
DOCK_ICON, DOCK_GAP, SLOT0_X = 37, 6, 247
PITCH = DOCK_ICON + DOCK_GAP
ICON_ROW_Y = 487
PARK = (480, 200)  # open wallpaper, away from the dock and every icon
FILES_SLOT = 1

# v0.78.x back-buffer note (editor_qa.py's Machine.presented, dockhover-
# check.py's own move()) applies here too: the instrument only moves once
# a real window_present() ran, so every scenario below waits on real time,
# never a single sample.

# Budget: 2026-09-23, issue #14. Ticks are 100Hz PIT counts (1 tick =
# 10ms), real wall-clock time divided by a TCG guest's actual instruction
# rate, which host load moves around run to run -- confirmed directly,
# back to back runs on an otherwise idle host measured window open at
# 28-34 ticks, and under real host load minutes later the same fixed
# kernel measured 37-42. Before the fix (window_clear's viewport branch
# and gui_rounded_rect_on_wallpaper's solid interior both walking every
# pixel through the generic per-pixel damage-tracked path, kernel.c/
# window.c): idle 4-7, dock hover 5-8, window open 44-68, session max
# 52-68, eight runs across both load conditions. After
# (window_fill_rect_phys, a bulk solid-rect fill that pays the viewport/
# back/screen_band routing once per rect instead of once per pixel):
# idle 2-7, dock hover 2-6, window open 28-42, session max 42-57, well
# over a dozen runs across both. Idle/hover stay budgeted loose -- both
# are single-digit ticks, dominated by host scheduling jitter rather than
# real kernel cost, and this check runs `once` (no retry, see
# ci-suite.sh), so a tight budget on a noisy low number would be a flake
# generator, not a regression gate. Window open and the session max carry
# the real signal: a ceiling with real headroom over the after range on a
# loaded host, still clearly under where a regression back to the
# unfixed per-pixel path would land even on an idle one.
BUDGET_IDLE = 12      # ambient wind-sway/clock redraw while nothing moves
BUDGET_HOVER = 12     # dock hover band repaint
BUDGET_OPEN = 60      # Files opening (full desktop + window content repaint)

nm = shutil.which('nm') or 'nm'
symbols = {}
for line in subprocess.check_output([nm, os.path.join(ROOT, 'kernel.elf')], text=True).splitlines():
    fields = line.split()
    if len(fields) == 3:
        symbols[fields[2]] = int(fields[0], 16) - 0xC0000000

for f in (LOG,):
    try: os.remove(f)
    except FileNotFoundError: pass

q = subprocess.Popen(["qemu-system-i386", "-kernel", "kernel.elf", "-display", "none", "-vga", "std",
                      "-qmp", f"tcp:127.0.0.1:{PORT},server,nowait", "-serial", "file:" + LOG],
                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

results = {}
fail = 0
try:
    time.sleep(1.0)
    s = socket.create_connection(("127.0.0.1", PORT), timeout=10)
    f = s.makefile("rw")
    def cmd(o):
        f.write(json.dumps(o) + "\n"); f.flush()
        while True:
            r = json.loads(f.readline())
            if "return" in r or "error" in r: return r
    f.readline()
    cmd({"execute": "qmp_capabilities"})

    def monitor(command):
        return cmd({"execute": "human-monitor-command", "arguments": {"command-line": command}})["return"]

    def memory(symbol, count):
        result = monitor(f"xp /{count}xb 0x{symbols[symbol]:x}")
        return bytes(int(v, 16) for line in result.splitlines() if ':' in line
                     for v in re.findall(r'0x([0-9a-f]{2})\b', line.split(':', 1)[1]))

    def integer(symbol):
        return int.from_bytes(memory(symbol, 4), 'little')

    def move(x, y):
        cmd({"execute": "input-send-event", "arguments": {"events": [
            {"type": "abs", "data": {"axis": "x", "value": int(x * 32768 / LOGICAL_W)}},
            {"type": "abs", "data": {"axis": "y", "value": int(y * 32768 / LOGICAL_H)}}]}})

    def click():
        for down in (True, False):
            cmd({"execute": "input-send-event", "arguments": {"events": [{"type": "btn", "data": {"down": down, "button": "left"}}]}})
            time.sleep(0.15)

    def poll_max(duration):
        """Max window_present_ticks_last seen over a real-time window: the
        symbol is overwritten by whatever presents next, so a single read
        can land after a cheap ambient frame has already erased the
        expensive one this scenario cares about."""
        deadline = time.time() + duration
        seen = 0
        while time.time() < deadline:
            seen = max(seen, integer('window_present_ticks_last'))
            time.sleep(0.02)
        return seen

    # Boot marker (kernel.c: *(volatile unsigned int*)0x9000 = 0xB007C0DE,
    # written right before gui_run()), same wait editor_qa.py's Machine
    # uses so this doesn't guess a fixed boot delay.
    for attempt in range(100):
        if monitor("xp /1xw 0x9000").find("b007c0de") >= 0:
            break
        time.sleep(0.1)
    else:
        print("FAIL: boot marker never reached"); sys.exit(1)
    time.sleep(2.0)  # first desktop frame settles

    centre = lambda slot: SLOT0_X + slot * PITCH + DOCK_ICON // 2

    # 1. idle desktop: park away from the dock, let the parking move's own
    # present pass, then measure only ambient redraws.
    move(*PARK); time.sleep(0.6)
    idle_ticks = poll_max(2.0)
    results['idle desktop'] = (idle_ticks, BUDGET_IDLE)

    # 2. dock hover: the same hop dockhover-check.py drives (park already
    # done above), five icons, sampled the whole way across.
    move(centre(1), ICON_ROW_Y); time.sleep(0.3)
    hover_ticks = poll_max(0.3)
    for slot in (2, 3, 4, 5):
        move(centre(slot), ICON_ROW_Y)
        hover_ticks = max(hover_ticks, poll_max(0.12))
    hover_ticks = max(hover_ticks, poll_max(0.3))
    results['dock hover'] = (hover_ticks, BUDGET_HOVER)
    move(*PARK); time.sleep(0.5)

    # 3. window open: click Files (dock slot 1), measure the open's own
    # frame (full desktop repaint + the new window's content).
    move(centre(FILES_SLOT), ICON_ROW_Y); time.sleep(0.2)
    open_ticks = poll_max(0.15)
    click()
    open_ticks = max(open_ticks, poll_max(0.8))
    results['window open'] = (open_ticks, BUDGET_OPEN)
    # close it: red close button, gui_launch_from_dock's window 0 rect
    # (x=70,y=40) -> close at (94,56), same geometry appclose-check.py uses.
    move(94, 56); time.sleep(0.2)
    click()
    time.sleep(0.3)

    # ponytail: reported, not gated. The worst present is usually the boot's
    # first full-desktop frame (one-time cost), and on a shared CI runner it
    # swung past 75 with no code change (#137: 81). Window open is the real
    # regression gate; gate this again if a repeated spike shows up here.
    session_max = integer('window_present_ticks_max')

    try: cmd({"execute": "quit"})
    except (ConnectionResetError, BrokenPipeError, OSError): pass
finally:
    try: q.wait(timeout=5)
    except subprocess.TimeoutExpired: q.kill()

for name, (ticks, budget) in results.items():
    ms = ticks * 10
    status = "ok" if ticks <= budget else "OVER BUDGET"
    print(f"{name}: {ticks} ticks/present ({ms}ms), budget {budget} -- {status}")
    if ticks > budget:
        fail = 1

print(f"session max: {session_max} ticks/present ({session_max * 10}ms), reported only")
if not fail:
    print("PASS: idle, dock hover and window open all stay within budget")
else:
    print("FAIL: at least one scenario exceeded its frame-time budget")
sys.exit(fail)
