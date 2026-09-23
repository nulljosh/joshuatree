"""Drives a headless Joshua Tree boot like a real user, over QMP, capturing
a frame every step so tools/qa-demo.sh can turn the session into a video.

Deliberately not a pass/fail test: the checks in tools/checks/ are the
assertions. This is the dogfood run, it opens everything, types real demo
content, and records what actually happened, including the ugly parts.
The serial log it writes alongside is where real errors surface."""
import re, json, os, socket, sys, time
from PIL import Image

port, out = int(sys.argv[1]), sys.argv[2]
frames = os.path.join(out, "frames")
n = [0]

s = None
for _ in range(60):
    time.sleep(0.2)
    try:
        s = socket.create_connection(("127.0.0.1", port)); break
    except OSError:
        pass
if s is None:
    print("FAIL: QEMU's QMP socket never came up"); sys.exit(1)
f = s.makefile("rw")

def cmd(o):
    f.write(json.dumps(o) + "\n"); f.flush()
    while True:
        r = json.loads(f.readline())
        if "return" in r or "error" in r:
            return r

f.readline()
cmd({"execute": "qmp_capabilities"})

# The framebuffer read straight out of guest memory (pmemsave), the same way
# every tools/checks pixel test does. QEMU's screendump is unreliable
# headless (CLAUDE.md); here it produced a stripe pattern, not the desktop.
FB, FB_W, FB_H = 0xfd000000, 1920, 1080
def shot(tag=""):
    n[0] += 1
    raw = "%s/f%04d.raw" % (frames, n[0])
    cmd({"execute": "pmemsave", "arguments": {"val": FB, "size": FB_W * FB_H * 4, "filename": raw}})
    Image.frombytes("RGBA", (FB_W, FB_H), open(raw, "rb").read(), "raw", "BGRA").convert("RGB").save(raw[:-4] + ".png")
    os.remove(raw)
    if tag:
        print("  frame %04d  %s" % (n[0], tag))

def hold(seconds, tag=""):
    """Record while time passes: a still period is exactly when a flash or a
    stray repaint shows up, so idle time gets frames too."""
    end = time.time() + seconds
    while time.time() < end:
        shot(tag); tag = ""
        time.sleep(0.25)

LOGICAL_W, LOGICAL_H = 960, 540
# Dock order and geometry come from kernel/kernel.c itself (GUI_DOCK_DEFAULT,
# GUI_LABELS, gui_dock_w/gui_dock_x0), the way tools/checks/dockslots-check.py
# reads them. A hardcoded copy went stale when Stocks joined the dock: every
# click landed a tile off and the "Weather" and "Trash" steps both opened
# Stocks. Icon size is the 960x540, dock_scale_pct 7 default (37 px).
_k = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "kernel", "kernel.c")).read()
_labels = re.findall(r'"([^"]+)"', re.search(r"GUI_LABELS\[GUI_APP_COUNT\] = \{(.*?)\};", _k, re.S).group(1))
_sym = {"GUI_APPS_FOLDER": _labels.index("Apps"), "GUI_TRASH": _labels.index("Trash")}
DOCK = [_labels[_sym[t] if t in _sym else int(t)]
        for t in re.search(r"GUI_DOCK_DEFAULT\[GUI_ICON_COUNT\] = \{(.*?)\};", _k).group(1).replace(" ", "").split(",")]
DOCK_ICON, DOCK_GAP, DOCK_PAD = 37, 6, 10
PITCH = DOCK_ICON + DOCK_GAP
SLOT0_X = (LOGICAL_W - (len(DOCK) * DOCK_ICON + (len(DOCK) - 1) * DOCK_GAP + 2 * DOCK_PAD)) // 2 + DOCK_PAD
ICON_ROW_Y = 487

def move(x, y):
    cmd({"execute": "input-send-event", "arguments": {"events": [
        {"type": "abs", "data": {"axis": "x", "value": int(x * 32768 / LOGICAL_W)}},
        {"type": "abs", "data": {"axis": "y", "value": int(y * 32768 / LOGICAL_H)}}]}})

def click():
    cmd({"execute": "input-send-event", "arguments": {"events": [{"type": "btn", "data": {"down": True, "button": "left"}}]}})
    time.sleep(0.1)
    cmd({"execute": "input-send-event", "arguments": {"events": [{"type": "btn", "data": {"down": False, "button": "left"}}]}})

def key(qcode, settle=0.25):
    cmd({"execute": "send-key", "arguments": {"keys": [{"type": "qcode", "data": qcode}]}})
    time.sleep(settle)
    shot()

# send-key takes QEMU qcodes, not characters: a bare "." is rejected, which is
# why "ship 1.0.0" used to arrive as "ship 100". Not the kernel's keymap.
QCODE = {" ": "spc", ".": "dot", "-": "minus", "/": "slash", ",": "comma"}
def type_text(text):
    for ch in text:
        key(QCODE.get(ch, ch), settle=0.15)

def dock(slot):
    move(SLOT0_X + slot * PITCH + DOCK_ICON // 2, ICON_ROW_Y)
    time.sleep(0.2); shot()
    click(); time.sleep(1.2)

print("booting")
time.sleep(6)
hold(1.5, "desktop")

# Every dock tile, in kernel order: each one opens, gets looked at, and closes
# from its own red button, the contract tools/checks/appclose-check.py relies
# on. Not esc: on main, esc with Files open drops the whole desktop to text
# mode (PR #83 fixes it), and the headless framebuffer keeps showing the last
# frame, so every step after Files silently recorded the same still.
CLOSE = {"Apps": (80, 46)}           # the folder's larger window (56, 30)
DEFAULT_CLOSE = (94, 56)             # gui_launch_from_dock / gui_multiwin_geom slot 0: (x+24, y+16), x=70, y=40
def close_window(name):
    move(*CLOSE.get(name, DEFAULT_CLOSE)); time.sleep(0.3)
    click(); time.sleep(1.0); shot()
    move(480, 200); time.sleep(0.2)
for slot, name in enumerate(DOCK):
    print("open %s" % name)
    dock(slot)
    hold(1.0, name)
    if name == "Notes":
        type_text("demo note")          # real content, not an empty window
    if name == "Reminders":
        key("a"); type_text("ship 1.0.0")
    if name == "Terminal":
        type_text("help"); key("ret", settle=1.0)
    if name == "Chat":
        type_text("hello")
    hold(0.75)
    close_window(name)
    hold(0.5, "%s closed" % name)

print("apps folder, wheel scroll and arrow keys")
dock(0)
hold(1.0, "apps folder")
for _ in range(3):
    cmd({"execute": "input-send-event", "arguments": {"events": [
        {"type": "btn", "data": {"down": True, "button": "wheel-down"}}]}})
    time.sleep(0.3); shot("wheel down")
for c in ("d", "d", "s", "a"):
    key(c, settle=0.4)
close_window("Apps")
hold(1.0, "back to desktop")

cmd({"execute": "quit"})
print("captured %d frames" % n[0])
