#!/bin/bash
# Real discriminating test for mouse wheel scrolling in the Apps folder.
#
# Replaces the old appsfolder-wheelscroll-test.mjs, which drove a
# Playwright browser against the LIVE deployed site
# (https://joshuatree.heyitsmejosh.com) through v86's in-browser PS/2
# emulation, not this repo's local build, and asserted on a screenshot
# pixel-diff fingerprint. That test reported "0.0% pixel difference" --
# looked like a broken test, but root-causing it against the real kernel
# (headless QEMU + QMP, the same pattern appsfolder-redraw-check.sh
# already uses) found two real, separate kernel bugs, confirmed live by
# instrumenting the real code, not by reasoning about it:
#
# 1. drivers/vmmouse.c's vmmouse_pump() read the EDX/z word off every
#    real VMware-backdoor absolute-pointer packet (the wheel step; QEMU
#    and v86 both fill it whenever a real wheel event fires, confirmed
#    live: a QMP "wheel-up" button event landed here as z=-1 on all 5
#    presses sent) into a local variable and threw it away. Nothing ever
#    forwarded it to the PS/2 driver's own accum_dz, so mouse_get_wheel()
#    had nothing to return whenever vmmouse is active -- the default
#    under QEMU, since mouse_handle_byte already drops the raw PS/2
#    packet's own contents once the backdoor takes over. Fixed by adding
#    vmmouse_take_wheel() and folding it into accum_dz in mouse.c's
#    vmmouse_fold().
# 2. Even with that wired up, drivers/mouse.c's mouse_get_delta() reset
#    accum_dz to 0 as an unrelated side effect, and gui_app_mouse_tick()
#    (cursor tracking) calls mouse_get_delta() on every single
#    get_key_or_click poll tick, wiping any real wheel step before
#    mouse_get_wheel() -- called later the same loop -- ever got a turn
#    to read it. Confirmed live: with bug 1 fixed alone, a real QMP wheel
#    press still produced zero Apps-folder scroll; only removing the
#    `accum_dz = 0` from mouse_get_delta made scrolling real.
#
# A third real gap, found but not directly reachable under QEMU's
# vmmouse backdoor (which owns the pointer here and always wins): real
# PS/2 hardware needs the IntelliMouse magic sample-rate sequence
# (0xF3 200/100/80) before a wheel mouse will ever send a 4th packet
# byte at all. mouse_init() never sent it. Added per the documented
# protocol (OSDev's Mouse Input page, matched by Linux's psmouse
# driver), harmless on a plain 3-byte mouse.
#
# This test proves the real, whole path: open the Apps folder from the
# dock, send real QMP wheel-down button events (the same vmmouse
# backdoor path a real host uses, not a synthetic byte feed), and demand
# the real panel-repaint marker (appsgridrepaint, the same marker
# appsfolder-redraw-check.sh already trusts) actually grows. A scroll
# that silently did nothing would leave this flat, exactly what it
# caught before the fix.
#
# Discriminating: reverted just the mouse_get_delta accum_dz fix,
# reran, panel count stayed flat at 1 (real FAIL) even though the
# vmmouse wheel-step marker still fired; restored, panel count grew.
set -e
cd "$(dirname "$0")/../.."
make -s kernel.elf

cleanup() { pkill -9 -f "qemu-system-i386.*jt-wheelscroll" >/dev/null 2>&1 || true; }
trap cleanup EXIT

PORT=4507
LOG=/tmp/jt-wheelscroll-check.log
rm -f "$LOG"
qemu-system-i386 -kernel kernel.elf -display none -vga std \
    -qmp "tcp:127.0.0.1:$PORT,server,nowait" -serial "file:$LOG" -name jt-wheelscroll &

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
time.sleep(3.0)

LOGICAL_W, LOGICAL_H = 960, 540
DOCK_ICON, DOCK_GAP, SLOT0_X = 37, 6, 268  # same dock constants as appclose-check.py
PITCH = DOCK_ICON + DOCK_GAP
ICON_ROW_Y = 487
APPS_SLOT = 0

def move(x, y):
    cmd({"execute": "input-send-event", "arguments": {"events": [
        {"type": "abs", "data": {"axis": "x", "value": int(x * 32768 / LOGICAL_W)}},
        {"type": "abs", "data": {"axis": "y", "value": int(y * 32768 / LOGICAL_H)}}]}})
def click():
    cmd({"execute": "input-send-event", "arguments": {"events": [{"type": "btn", "data": {"down": True, "button": "left"}}]}})
    time.sleep(0.1)
    cmd({"execute": "input-send-event", "arguments": {"events": [{"type": "btn", "data": {"down": False, "button": "left"}}]}})
def wheel_down():
    cmd({"execute": "input-send-event", "arguments": {"events": [{"type": "btn", "data": {"down": True, "button": "wheel-down"}}]}})
    time.sleep(0.05)
    cmd({"execute": "input-send-event", "arguments": {"events": [{"type": "btn", "data": {"down": False, "button": "wheel-down"}}]}})

centre = SLOT0_X + APPS_SLOT * PITCH + DOCK_ICON // 2
move(centre, ICON_ROW_Y); time.sleep(0.3); click(); time.sleep(1.5)  # open the Apps folder

panel_open = count("appsgridrepaint\n")

for _ in range(5):
    wheel_down()
    time.sleep(0.3)
time.sleep(0.5)

panel_after = count("appsgridrepaint\n")

cmd({"execute": "quit"})
print("panel repaint: %d -> %d after 5 real QMP wheel-down events" % (panel_open, panel_after))
if panel_after <= panel_open:
    print("FAIL: Apps folder never repainted its panel after real wheel events, scrolling is dead")
    sys.exit(1)
print("PASS: real QMP wheel events actually scrolled the Apps folder (panel repainted)")
PYEOF
STATUS=$?
cleanup
exit $STATUS
