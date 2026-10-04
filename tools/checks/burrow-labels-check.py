#!/usr/bin/env python3
"""Burrow never cuts a file name short (2.6.30). Joshua: "File names are being cut off."

Icon view used to chop a label at 76 px with no sign it had been chopped, so a
12 character FAT name lost its end. Now a long name wraps onto a second line.

Boots headless with a FAT16 image holding a short file (A.TXT) and a long one
(LONGNAME.TXT), opens Burrow, switches to icon view (key 2) and looks at the
two tiles' label lines:

  1. the short name has dark text on line one and nothing on line two;
  2. the long name has dark text on BOTH lines (it wrapped, it was not cut);
  3. the serial log shows no fault.

Discriminating: put label() back to one cut line and step 2 fails.

Usage: tools/checks/burrow-labels-check.py   (from the repo root, after make kernel.elf)
       BURROW_LABELS_SHOT=/tmp/x.png saves the icon view for a human to look at.
"""
import json, os, socket, subprocess, sys, tempfile, time
from PIL import Image
from freeport import free_port

FB = 0xfd000000; W, H = 1920, 1080
LOGICAL_W, LOGICAL_H, SCALE = 960, 540, 2
DOCK_ICON, DOCK_GAP, SLOT0_X = 37, 6, 247
PITCH = DOCK_ICON + DOCK_GAP
ICON_ROW_Y = 487
BURROW_SLOT = 1
VIEW_X, VIEW_Y = 78, 72
TILE, ICON, ICON_TOP, LIST_H = 84, 40, 76, 18       # user/burrow.c
LINE1, LINE2 = ICON + 6, ICON + 6 + LIST_H - 2      # label line tops inside a tile
os.chdir(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))

DISK = "/tmp/jt-burrowlabels-fat.img"
LOG, DUMP = "/tmp/jt-burrowlabels-serial.log", "/tmp/jt-burrowlabels.raw"
for f in (LOG, DUMP):
    try: os.remove(f)
    except FileNotFoundError: pass
try:
    subprocess.run(["bash", "tools/mkdisk.sh", DISK], check=True, stdout=subprocess.DEVNULL)
    with tempfile.NamedTemporaryFile("w", suffix=".txt", delete=False) as t:
        t.write("x\n"); src = t.name
    for n in ("A.TXT", "LONGNAME.TXT"):
        subprocess.run(["mcopy", "-i", DISK, src, "::" + n], check=True)
    os.remove(src)
except (subprocess.CalledProcessError, FileNotFoundError) as e:
    raise SystemExit(f"FAIL: could not build the FAT16 image: {e}")

port = free_port()
q = subprocess.Popen(["qemu-system-i386", "-kernel", "kernel.elf", "-nic", "none", "-display", "none", "-vga", "std",
                      "-qmp", f"tcp:127.0.0.1:{port},server,nowait", "-serial", "file:" + LOG,
                      "-drive", f"file={DISK},format=raw,if=ide,index=0"],
                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
s = None
for _ in range(50):
    time.sleep(0.2)
    try: s = socket.create_connection(("127.0.0.1", port)); break
    except OSError: pass
if s is None: raise SystemExit("FAIL: QEMU's QMP socket never came up")
qf = s.makefile("rw")
def cmd(o):
    qf.write(json.dumps(o) + "\n"); qf.flush()
    while True:
        r = json.loads(qf.readline())
        if "return" in r or "error" in r: return r
qf.readline(); cmd({"execute": "qmp_capabilities"})
def serial():
    try: return open(LOG, errors="replace").read()
    except OSError: return ""
def wait(needle, secs):
    for _ in range(int(secs * 10)):
        if needle in serial(): return True
        time.sleep(0.1)
    return False
def move(x, y):
    cmd({"execute": "input-send-event", "arguments": {"events": [
        {"type": "abs", "data": {"axis": "x", "value": int(x * 32768 / LOGICAL_W)}},
        {"type": "abs", "data": {"axis": "y", "value": int(y * 32768 / LOGICAL_H)}}]}})
def click():
    cmd({"execute": "input-send-event", "arguments": {"events": [{"type": "btn", "data": {"down": True, "button": "left"}}]}})
    time.sleep(0.12)
    cmd({"execute": "input-send-event", "arguments": {"events": [{"type": "btn", "data": {"down": False, "button": "left"}}]}})
def key(k):
    cmd({"execute": "send-key", "arguments": {"keys": [{"type": "qcode", "data": k}]}}); time.sleep(0.5)
def frame():
    cmd({"execute": "pmemsave", "arguments": {"val": FB, "size": W * H * 4, "filename": DUMP}})
    return Image.frombytes("RGBA", (W, H), open(DUMP, "rb").read(), "raw", "BGRA").convert("RGB")
def dark_in(img, x0, y0, w, h):
    n = 0
    for x in range(x0 * SCALE, (x0 + w) * SCALE):
        for y in range(y0 * SCALE, (y0 + h) * SCALE):
            r, g, b = img.getpixel((x, y))
            if r + g + b < 3 * 0x80: n += 1
    return n

fails = []
try:
    time.sleep(5.0)
    move(SLOT0_X + BURROW_SLOT * PITCH + DOCK_ICON // 2, ICON_ROW_Y); time.sleep(0.3); click(); time.sleep(1.5)
    if not wait("burrow: ring-3 window", 20): fails.append("Burrow never opened its window")
    key("2"); time.sleep(0.8)
    img = frame()
    if os.environ.get("BURROW_LABELS_SHOT"):
        img.crop((VIEW_X * SCALE, VIEW_Y * SCALE, (VIEW_X + 480) * SCALE, (VIEW_Y + 200) * SCALE)).save(os.environ["BURROW_LABELS_SHOT"])
    rows = []
    for i in range(2):
        x = VIEW_X + 20 + i * TILE; y = VIEW_Y + ICON_TOP
        rows.append((dark_in(img, x, y + LINE1, TILE - 8, 14), dark_in(img, x, y + LINE2, TILE - 8, 14)))
    one = [r for r in rows if r[0] > 30 and r[1] < 5]
    two = [r for r in rows if r[0] > 30 and r[1] > 30]
    if len(one) != 1: fails.append(f"expected exactly one single-line label (A.TXT), got tiles {rows}")
    if len(two) != 1: fails.append(f"expected exactly one wrapped label (LONGNAME.TXT), got tiles {rows}")
    if "exception: ring-0" in serial() or "panic in" in serial(): fails.append("the kernel faulted")
    key("esc")
finally:
    try: cmd({"execute": "quit"})
    except (ConnectionResetError, BrokenPipeError, OSError, ValueError, TypeError): pass
    q.terminate()
    try: q.wait(5)
    except subprocess.TimeoutExpired: q.kill()
if fails:
    print("FAIL:"); [print("  - " + x) for x in fails]; sys.exit(1)
print("PASS: Burrow icon view keeps a short name on one line and wraps a long one onto a second line instead of cutting it")
