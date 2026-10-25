#!/usr/bin/env python3
"""Snake runs as a ring-3 program: it moves on the clock, eats, refuses to reverse, and dies at a wall.

Boots headless with `open=snak`. The first apple comes from a fixed seed and
sits ahead of the head in the default window, so with no steering the snake
must eat it ("snake: apple 1"). Left is pressed first: a reverse onto its own
neck must be ignored, or the run ends at once. After the apple, Up must turn
it into the top wall ("snake: over 1"). Esc must close it with exit 0.

Discriminating: stop the clock, drop the reverse guard, the growth or the
wall test in user/snake.c and one of these fails.

Usage: tools/checks/ring3-snake-check.py   (from the repo root, after make kernel.elf)
"""
import json, os, socket, subprocess, sys, time
from PIL import Image
from freeport import free_port
from scratch import scratch_dir

TMP = scratch_dir("ring3snake")
LOG = os.path.join(TMP, "serial.log")
DUMP = os.path.join(TMP, "fb.raw")
FB = 0xfd000000; W, H = 1920, 1080
PORT = free_port()
WIN = (100, 150, 1780, 800)   # device pixels inside the app window, below its title bar, above its HUD

os.chdir(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
for f in (LOG, DUMP):
    try: os.remove(f)
    except FileNotFoundError: pass

q = subprocess.Popen(["qemu-system-i386", "-kernel", "kernel.elf", "-append", "open=snak",
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

    if not wait_serial("ring3app: launching SNAKE.BIN at ring 3", 40):
        fails.append("Snake was never launched as a ring-3 program (open=snak flag or ring3app.c broken)")
    if not wait_serial("snake: grid 49018", 10):
        fails.append("the grid is not 49 by 18: the window changed size, so the seeded first apple is no longer ahead")
    if not wait_serial("snake: first apple 605", 5):
        fails.append("the first apple is not at row 9, column 29 (605): the seed or the apple order changed")
    time.sleep(0.3)
    if os.environ.get("JT_SHOT"): frame().save(os.environ["JT_SHOT"])
    key("left")   # straight back onto the neck: must be ignored
    if not wait_serial("snake: apple 1", 15):
        fails.append("the snake did not eat the apple ahead (a reverse turn was not ignored, or it does not move)")
    if "snake: over 0" in serial():
        fails.append("Left reversed the snake into itself")
    key("up")
    if not wait_serial("snake: over 1", 15):
        fails.append("Up did not turn the snake into the top wall")

    exits = serial().count("SNAKE.BIN exited 0")
    key("esc")
    if not wait_serial("snake: closed", 5):
        fails.append("Esc did not reach the program (no closed line)")
    for _ in range(50):
        if serial().count("SNAKE.BIN exited 0") > exits: break
        time.sleep(0.1)
    if serial().count("SNAKE.BIN exited 0") <= exits:
        fails.append("Snake did not exit 0 on Esc")
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
print("PASS: Snake ran at ring 3, ate the apple ahead, ignored a reverse, died at the wall, and closed on Esc")
