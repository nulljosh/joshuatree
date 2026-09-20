"""Drives a headless Joshua Tree boot like a real user, over QMP, capturing
a frame every step so tools/qa-demo.sh can turn the session into a video.

Deliberately not a pass/fail test: the checks in tools/checks/ are the
assertions. This is the dogfood run, it opens everything, types real demo
content, and records what actually happened, including the ugly parts.
The serial log it writes alongside is where real errors surface."""
import json, os, socket, sys, time

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

def shot(tag=""):
    n[0] += 1
    cmd({"execute": "screendump", "arguments": {"filename": "%s/f%04d.ppm" % (frames, n[0])}})
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
DOCK_ICON, DOCK_GAP, SLOT0_X = 37, 6, 268
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

def key(qcode, settle=0.25):
    cmd({"execute": "send-key", "arguments": {"keys": [{"type": "qcode", "data": qcode}]}})
    time.sleep(settle)
    shot()

def type_text(text):
    for ch in text:
        key("spc" if ch == " " else ch, settle=0.15)

def dock(slot):
    move(SLOT0_X + slot * PITCH + DOCK_ICON // 2, ICON_ROW_Y)
    time.sleep(0.2); shot()
    click(); time.sleep(1.2)

print("booting")
time.sleep(6)
hold(1.5, "desktop")

# Dock order per roadmap: Apps, Files, Mail, Calendar, Notes, Reminders,
# Terminal, Chat, Weather, Trash. Each one opens, gets looked at, and closes
# on esc, the same contract tools/checks/appclose-check.py already relies on.
DOCK = ["Apps", "Files", "Mail", "Calendar", "Notes", "Reminders",
        "Terminal", "Chat", "Weather", "Trash"]
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
    key("esc", settle=1.0)
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
key("esc", settle=1.0)
hold(1.0, "back to desktop")

cmd({"execute": "quit"})
print("captured %d frames" % n[0])
