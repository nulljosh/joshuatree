#!/usr/bin/env python3
"""Paddle runs as a ring-3 program: the ball moves, a key moves your paddle, points get scored, p pauses.

Boots headless with `open=padd`. Reads the framebuffer to find the ball (the
only terracotta inside the window) and your paddle (the ink at the window's
left edge). The ball must move between two frames; three presses of Up must
lift the paddle (after space serves); with your paddle parked at the top, someone must score within 40 seconds
("paddle: score"); p must freeze the ball; Esc must close it with exit 0.

Discriminating: stop stepping the game on the clock, drop the key handler
or the scoring in user/paddle.c and one of these fails.

Usage: tools/checks/ring3-paddle-check.py   (from the repo root, after make kernel.elf)
"""
import json, os, socket, subprocess, sys, time
from PIL import Image
from freeport import free_port
from scratch import scratch_dir

TMP = scratch_dir("ring3paddle")
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

q = subprocess.Popen(["qemu-system-i386", "-kernel", "kernel.elf", "-append", "open=padd",
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
        hits = [(x, y) for y in range(y0, y1, 4) for x in range(x0, x1, 4) if img.getpixel((x, y)) == BALL]
        return (sum(h[0] for h in hits) // len(hits), sum(h[1] for h in hits) // len(hits)) if hits else None
    def paddle_top(img):
        x0, y0, x1, y1 = WIN
        ys = [y for y in range(y0, y1, 2) for x in range(x0, x0 + 120, 2) if img.getpixel((x, y)) == INK]
        return min(ys) if ys else None

    if not wait_serial("ring3app: launching PADDLE.BIN at ring 3", 40):
        fails.append("Paddle was never launched as a ring-3 program (open=padd flag or ring3app.c broken)")
    if not wait_serial("paddle: ring-3 window ", 10):
        fails.append("the program did not report its window through write()")
    time.sleep(0.8)   # the window takes keys a moment after it reports itself
    key("spc")   # serve
    time.sleep(1.5)   # past the serve pause
    a = frame()
    if os.environ.get("JT_SHOT"): a.save(os.environ["JT_SHOT"])
    time.sleep(0.4)
    b = frame()
    pa, pb = ball(a), ball(b)
    print(f"ball {pa} then {pb}")
    if not pa or not pb: fails.append("no ball in the window")
    elif pa == pb: fails.append("the ball did not move: the game is not stepping on the clock")

    top0 = paddle_top(frame())
    for _ in range(3): key("up")
    time.sleep(0.3)
    top1 = paddle_top(frame())
    print(f"your paddle's top {top0} then {top1} after three Ups")
    if top0 is None or top1 is None or top1 >= top0 - 40:
        fails.append("Up did not lift your paddle")

    for _ in range(12): key("up")   # park your paddle at the top so the computer's shots get past it
    if not wait_serial("paddle: score ", 40):
        fails.append("nobody scored in 40 seconds with you idle")

    key("p"); time.sleep(0.3)
    c = ball(frame()); time.sleep(0.5); d = ball(frame())
    print(f"paused: ball {c} then {d}")
    if c != d: fails.append("p did not pause the ball")
    key("p")

    exits = serial().count("PADDLE.BIN exited 0")
    key("esc")
    if not wait_serial("paddle: closed", 5):
        fails.append("Esc did not reach the program (no closed line)")
    for _ in range(50):
        if serial().count("PADDLE.BIN exited 0") > exits: break
        time.sleep(0.1)
    if serial().count("PADDLE.BIN exited 0") <= exits:
        fails.append("Paddle did not exit 0 on Esc")
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
print("PASS: Paddle ran at ring 3, the ball moved on the clock, Up moved your paddle, a point was scored, p paused, and it closed on Esc")
