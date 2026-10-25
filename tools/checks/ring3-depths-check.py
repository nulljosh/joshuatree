#!/usr/bin/env python3
"""Depths, the first-person maze shooter, runs as a ring-3 program and plays.

Boots headless with `open=dept`. You start facing east down the top corridor
with a creature four tiles ahead, so space must log "depths: hit 1". The
picture must show the ceiling and the floor colours and a shaded wall, a turn
(Left) and a step (Up) must each change the picture, and Esc must close it
with exit 0.

Discriminating: break the raycaster, the projection that puts the creature
in the crosshair, or the key handler in user/depths.c and one of these fails.

Usage: tools/checks/ring3-depths-check.py   (from the repo root, after make kernel.elf)
"""
import json, os, socket, subprocess, sys, time
from PIL import Image, ImageChops
from freeport import free_port
from scratch import scratch_dir

TMP = scratch_dir("ring3depths")
LOG = os.path.join(TMP, "serial.log")
DUMP = os.path.join(TMP, "fb.raw")
FB = 0xfd000000; W, H = 1920, 1080
PORT = free_port()
CEIL = (0x2A, 0x27, 0x24)
FLOOR = (0x5A, 0x53, 0x4B)
WIN = (100, 150, 1780, 800)   # device pixels inside the app window, below its title bar, above its HUD

os.chdir(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
for f in (LOG, DUMP):
    try: os.remove(f)
    except FileNotFoundError: pass

q = subprocess.Popen(["qemu-system-i386", "-kernel", "kernel.elf", "-append", "open=dept",
                      "-display", "none", "-vga", "std",
                      "-qmp", f"tcp:127.0.0.1:{PORT},server,nowait", "-serial", "file:" + LOG],
                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
fails = []
def serial():
    try: return open(LOG, errors="replace").read()
    except OSError: return ""
def wait_serial(needle, secs):
    for _ in range(int(secs * 10)):
        if needle in serial(): return True
        time.sleep(0.1)
    return False
try:
    s = None
    for _ in range(50):
        time.sleep(0.2)
        try: s = socket.create_connection(("127.0.0.1", PORT)); break
        except OSError: pass
    if s is None: raise SystemExit("FAIL: QEMU's QMP socket never came up")
    f = s.makefile("rw")
    def cmd(o):
        f.write(json.dumps(o) + "\n"); f.flush()
        while True:
            r = json.loads(f.readline())
            if "return" in r or "error" in r: return r
    f.readline()
    cmd({"execute": "qmp_capabilities"})
    def key(k):
        r = cmd({"execute": "send-key", "arguments": {"keys": [{"type": "qcode", "data": k}]}})
        if "error" in r: raise SystemExit(f"FAIL: QMP rejected send-key {k}: {r['error']}")
        time.sleep(0.15)
    def frame():
        cmd({"execute": "pmemsave", "arguments": {"val": FB, "size": W * H * 4, "filename": DUMP}})
        return Image.frombytes("RGBA", (W, H), open(DUMP, "rb").read(), "raw", "BGRA").convert("RGB").crop(WIN)
    def changed(a, b): return ImageChops.difference(a, b).getbbox() is not None

    if not wait_serial("ring3app: launching DEPTHS.BIN at ring 3", 40):
        fails.append("Depths was never launched as a ring-3 program (open=dept flag or ring3app.c broken)")
    if not wait_serial("depths: ready", 10):
        fails.append("the program never drew its first frame")
    time.sleep(0.5)
    a = frame()
    if os.environ.get("JT_SHOT"): a.save(os.environ["JT_SHOT"])
    px = [a.getpixel((x, y)) for y in range(0, a.height, 6) for x in range(0, a.width, 6)]
    ceil, floor = sum(p == CEIL for p in px), sum(p == FLOOR for p in px)
    wall = sum(p[0] > p[2] + 40 and p not in (CEIL, FLOOR) for p in px)
    print(f"samples: ceiling {ceil}, floor {floor}, wall {wall} of {len(px)}")
    if ceil < len(px) // 10 or floor < len(px) // 10 or wall < len(px) // 20:
        fails.append("the first frame is not a ceiling, a floor and shaded walls")

    key("spc")
    if not wait_serial("depths: hit 1", 3):
        fails.append("space did not hit the creature straight ahead")
    time.sleep(0.3)
    b = frame(); key("left"); time.sleep(0.4); c = frame()
    if not changed(b, c): fails.append("Left did not turn the view")
    for _ in range(3): key("right")
    d = frame(); key("up"); key("up"); time.sleep(0.4); e = frame()
    if not changed(d, e): fails.append("Up did not move you")

    exits = serial().count("DEPTHS.BIN exited 0")
    key("esc")
    if not wait_serial("depths: closed", 5):
        fails.append("Esc did not reach the program (no closed line)")
    for _ in range(50):
        if serial().count("DEPTHS.BIN exited 0") > exits: break
        time.sleep(0.1)
    if serial().count("DEPTHS.BIN exited 0") <= exits:
        fails.append("Depths did not exit 0 on Esc")
    if "exception: ring-0" in serial() or "panic in" in serial() or "ring3app: BUG" in serial():
        fails.append("the kernel faulted or ring3app logged a BUG line")
finally:
    try: cmd({"execute": "quit"})
    except (ConnectionResetError, BrokenPipeError, OSError, ValueError, NameError): pass
    q.terminate()
    try: q.wait(5)
    except subprocess.TimeoutExpired: q.kill()

if fails:
    print("FAIL:")
    for x in fails: print("  - " + x)
    print("--- serial tail ---")
    print(serial()[-1500:])
    sys.exit(1)
print("PASS: Depths ran at ring 3, drew walls, floor and ceiling, hit the creature ahead, turned and walked, and closed on Esc")
