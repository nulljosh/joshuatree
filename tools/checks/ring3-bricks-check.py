#!/usr/bin/env python3
"""Bricks runs as a ring-3 program: the serve breaks a brick, a key moves the paddle, p pauses.

Boots headless with `open=bric`. Space launches the ball, which goes up into the wall, so
"bricks: brick 1" must show within 15 seconds. Reads the framebuffer to find
the paddle (the ink in the bottom strip of the window): three presses of
Left must move it left. p must freeze the ball (the terracotta square below
the wall), and Esc must close it with exit 0.

Discriminating: stop stepping the game on the clock, drop the brick test or
the key handler in user/bricks.c and one of these fails.

Usage: tools/checks/ring3-bricks-check.py   (from the repo root, after make kernel.elf)
"""
import json, os, socket, subprocess, sys, time
from PIL import Image
from freeport import free_port
from scratch import scratch_dir

TMP = scratch_dir("ring3bricks")
LOG = os.path.join(TMP, "serial.log")
DUMP = os.path.join(TMP, "fb.raw")
FB = 0xfd000000; W, H = 1920, 1080
PORT = free_port()
BALL = (0xB5, 0x50, 0x2C)
INK = (0x1C, 0x1C, 0x1E)
WIN = (100, 150, 1780, 850)   # device pixels inside the app window, below its title bar and above the dock

os.chdir(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
for f in (LOG, DUMP):
    try: os.remove(f)
    except FileNotFoundError: pass

q = subprocess.Popen(["qemu-system-i386", "-kernel", "kernel.elf", "-append", "open=bric",
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
        time.sleep(0.1)
    def frame():
        cmd({"execute": "pmemsave", "arguments": {"val": FB, "size": W * H * 4, "filename": DUMP}})
        return Image.frombytes("RGBA", (W, H), open(DUMP, "rb").read(), "raw", "BGRA").convert("RGB")
    def ball(img):
        x0, y0, x1, y1 = WIN
        hits = [(x, y) for y in range(y0 + 260, y1, 4) for x in range(x0, x1, 4) if img.getpixel((x, y)) == BALL]   # below the wall
        return (sum(h[0] for h in hits) // len(hits), sum(h[1] for h in hits) // len(hits)) if hits else None
    def paddle_x(img):
        x0, y0, x1, y1 = WIN
        xs = [x for y in range(y1 - 120, y1, 2) for x in range(x0, x1, 2) if img.getpixel((x, y)) == INK]
        return sum(xs) // len(xs) if xs else None

    if not wait_serial("ring3app: launching BRICKS.BIN at ring 3", 40):
        fails.append("Bricks was never launched as a ring-3 program (open=bric flag or ring3app.c broken)")
    if not wait_serial("bricks: ring-3 window ", 10):
        fails.append("the program did not report its window through write()")
    time.sleep(0.5)
    img = frame()
    if os.environ.get("JT_SHOT"): img.save(os.environ["JT_SHOT"])
    x0 = paddle_x(img)
    for _ in range(3): key("left")
    time.sleep(0.3)
    x1 = paddle_x(frame())
    print(f"the paddle's middle {x0} then {x1} after three Lefts")
    if x0 is None or x1 is None or x1 >= x0 - 60:
        fails.append("Left did not move the paddle")
    key("spc")   # launch: the ball rides the paddle until space
    if not wait_serial("bricks: brick 1", 15):
        fails.append("the first serve did not break a brick")
    time.sleep(1)

    key("p"); time.sleep(0.3)
    c = ball(frame()); time.sleep(0.5); d = ball(frame())
    print(f"paused: ball {c} then {d}")
    if c != d: fails.append("p did not pause the ball")
    key("p")

    exits = serial().count("BRICKS.BIN exited 0")
    key("esc")
    if not wait_serial("bricks: closed", 5):
        fails.append("Esc did not reach the program (no closed line)")
    for _ in range(50):
        if serial().count("BRICKS.BIN exited 0") > exits: break
        time.sleep(0.1)
    if serial().count("BRICKS.BIN exited 0") <= exits:
        fails.append("Bricks did not exit 0 on Esc")
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
print("PASS: Bricks ran at ring 3, the serve broke a brick, Left moved the paddle, p paused, and it closed on Esc")
