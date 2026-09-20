#!/bin/bash
# v0.76.19: direct report ("our window toolbar shows duplicated, two x
# buttons two minimize buttons"). Real, long-standing bug, present since
# multi-window Files/Weather shipped (v0.73.0) and Mail/Calendar/Reminders
# (v0.75.0) -- not something the v0.76.18 chrome/content split introduced.
#
# Root cause: every multi-window app's *_content function (gui_draw_
# files_content, gui_draw_weather_content, gui_draw_mail_content,
# gui_draw_calendar_content, gui_draw_reminders_content) calls the shared
# gui_draw_app_titlebar(), which only skips drawing its OWN traffic-light
# circles + "x"/"-" when the global gui_app_windowed flag is set. That flag
# was only ever set by the OLD single-window gui_launch_from_dock path
# (bracketing its blocking gui_launch() call) -- gui_multiwin_draw_
# content_only never set it, so every multi-window content redraw painted
# a SECOND, real, viewport-relative (26,20)/(46,20)/(66,20) set of traffic
# lights on top of gui_multiwin_draw_chrome's own real ones at (24,16)/
# (46,16)/(68,16) -- two visibly offset close/minimize buttons, exactly as
# reported.
#
# Fix: gui_multiwin_draw_content_only now sets gui_app_windowed = 1 before
# calling into any *_content function, and resets it to 0 after -- the
# same bracket gui_launch_from_dock already uses for the single-window
# path, just extended to the multi-window one.
#
# Proven here with a real framebuffer pixel sample: opens Files (multi-
# window), samples the REAL close circle at logical (94,56) -- must be red
# -- and the BOGUS second close circle's real screen position at (104,92)
# (window origin (70,40) + content viewport origin (8,32) + the titlebar's
# own hardcoded (26,20)) -- must NOT be red once fixed.
#
# Proven discriminating: reverted the gui_app_windowed=1/0 bracket locally,
# reran -- (104,92) came back red (253,94,86), a real second close button;
# restored, clean background (250,248,246).
set -e
cd "$(dirname "$0")/../.."
make -s kernel.elf

cleanup() { pkill -9 -f "qemu-system-i386.*jt-dupetoolbar" >/dev/null 2>&1 || true; }
trap cleanup EXIT

PORT=4492
LOG=/tmp/jt-dupetoolbar-check.log
RAW=/tmp/jt-dupetoolbar-check.raw
rm -f "$LOG" "$RAW"
qemu-system-i386 -kernel kernel.elf -display none -vga std \
    -qmp "tcp:127.0.0.1:$PORT,server,nowait" -serial "file:$LOG" -name jt-dupetoolbar &

python3 - "$PORT" "$RAW" <<'PYEOF'
import json, socket, sys, time
from PIL import Image

port, raw_path = int(sys.argv[1]), sys.argv[2]
FB = 0xfd000000; W, H = 1920, 1080
CLOSE_RED = (0xFF, 0x5F, 0x57)

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

def move(x, y):
    cmd({"execute": "input-send-event", "arguments": {"events": [
        {"type": "abs", "data": {"axis": "x", "value": int(x * 32768 / 960)}},
        {"type": "abs", "data": {"axis": "y", "value": int(y * 32768 / 540)}}]}})
def click():
    cmd({"execute": "input-send-event", "arguments": {"events": [{"type": "btn", "data": {"down": True, "button": "left"}}]}})
    time.sleep(0.1)
    cmd({"execute": "input-send-event", "arguments": {"events": [{"type": "btn", "data": {"down": False, "button": "left"}}]}})

DOCK_ICON, DOCK_GAP, SLOT0_X = 37, 6, 247  # same dock constants as appclose-check.py
FILES_SLOT = 1
centre = SLOT0_X + FILES_SLOT * (DOCK_ICON + DOCK_GAP) + DOCK_ICON // 2
move(centre, 487); time.sleep(0.3); click(); time.sleep(1.2)  # open Files (multi-window)

cmd({"execute": "pmemsave", "arguments": {"val": FB, "size": W * H * 4, "filename": raw_path}})
try: cmd({"execute": "quit"})
except (ConnectionResetError, BrokenPipeError, OSError): pass

img = Image.frombytes("RGBA", (W, H), open(raw_path, "rb").read(), "raw", "BGRA").convert("RGB")
def pixel(x, y): return img.getpixel((x * 2 + 1, y * 2 + 1))
def is_red(p): return max(abs(p[i] - CLOSE_RED[i]) for i in range(3)) <= 12

real_close = pixel(94, 56)
bogus_close = pixel(104, 92)
print("real close circle (94,56): %s  bogus second close circle (104,92): %s" % (real_close, bogus_close))
if not is_red(real_close):
    print("FAIL: the real close button isn't even there -- sampling point is wrong"); sys.exit(1)
if is_red(bogus_close):
    print("FAIL: a second red close button is still drawn inside the content viewport -- duplicated toolbar is back"); sys.exit(1)
print("PASS: multi-window Files draws exactly one toolbar, no duplicate traffic lights inside the content viewport")
PYEOF
STATUS=$?
cleanup
exit $STATUS
