#!/usr/bin/env python3
"""Mines (Minesweeper) runs as a ring-3 program and a scripted game is won.

Boots headless with `open=mine`, which launches Mines from the dock path
the moment the desktop is up. user/mines.c deals its first board from a
fixed xorshift seed, so this check grows the same 9x9 board with the same
generator, walks the cursor over every cell in a snake and presses space
on each safe one. The app must then log "mines: won" and never
"mines: boom". Esc must close it with exit 0, and the desktop must still
open Mail from the dock afterwards.

Discriminating: change the seed, the mine count or the reveal rule in
user/mines.c and the scripted game hits a mine or never reaches 71 open
cells.

Usage: tools/checks/ring3mines-check.py   (from the repo root, after make kernel.elf)
"""
import json, os, socket, subprocess, sys, time
from PIL import Image
from freeport import free_port
from scratch import scratch_dir

TMP = scratch_dir("ring3mines")
LOG = os.path.join(TMP, "serial.log")
DUMP = os.path.join(TMP, "fb.raw")
FB = 0xfd000000; W, H = 1920, 1080
PORT = free_port()
LOGICAL_W, LOGICAL_H, SCALE = 960, 540, 2
DOCK_ICON, DOCK_GAP, SLOT0_X = 37, 6, 247
PITCH = DOCK_ICON + DOCK_GAP
ICON_ROW_Y = 487
CLOSE_X, CLOSE_Y = 94, 56
CLOSE_RED = (0xFF, 0x5F, 0x57)
PARK = (480, 200)
MW, MH, NMINES, SEED = 9, 9, 10, 0x9E3779B9

def board():
    """The same deal() as user/mines.c: xorshift32 from the fixed seed."""
    s = SEED; m = 0xFFFFFFFF
    mines = set()
    while len(mines) < NMINES:
        s ^= (s << 13) & m; s ^= s >> 17; s ^= (s << 5) & m
        r = s % (MW * MH)
        mines.add((r // MW, r % MW))
    return mines

os.chdir(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
for f in (LOG, DUMP):
    try: os.remove(f)
    except FileNotFoundError: pass

q = subprocess.Popen(["qemu-system-i386", "-kernel", "kernel.elf", "-append", "open=mine",
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
    def keys(*qcodes):
        r = cmd({"execute": "send-key", "arguments": {"keys": [{"type": "qcode", "data": k} for k in qcodes]}})
        if "error" in r: raise SystemExit(f"FAIL: QMP rejected send-key {qcodes}: {r['error']}")
    def move(x, y):
        cmd({"execute": "input-send-event", "arguments": {"events": [
            {"type": "abs", "data": {"axis": "x", "value": int(x * 32768 / LOGICAL_W)}},
            {"type": "abs", "data": {"axis": "y", "value": int(y * 32768 / LOGICAL_H)}}]}})
    def click():
        cmd({"execute": "input-send-event", "arguments": {"events": [{"type": "btn", "data": {"down": True, "button": "left"}}]}})
        time.sleep(0.1)
        cmd({"execute": "input-send-event", "arguments": {"events": [{"type": "btn", "data": {"down": False, "button": "left"}}]}})
    def frame():
        cmd({"execute": "pmemsave", "arguments": {"val": FB, "size": W * H * 4, "filename": DUMP}})
        return Image.frombytes("RGBA", (W, H), open(DUMP, "rb").read(), "raw", "BGRA").convert("RGB")
    def pixel(x, y, img=None):
        return (img or frame()).getpixel((x * SCALE + 1, y * SCALE + 1))
    def near(p, c, tol=12): return max(abs(p[i] - c[i]) for i in range(3)) <= tol

    # 1. the program is up and has its window
    if not wait_serial("ring3app: launching MINES.BIN at ring 3", 40):
        fails.append("Mines was never launched as a ring-3 program (open=mine flag or ring3app.c broken)")
    if not wait_serial("mines: ring-3 window 804x345", 10):
        fails.append("the program did not report the app viewport's size (expected 804x345) through write()")
    time.sleep(0.5)
    if os.environ.get("JT_SHOT"): frame().save(os.environ["JT_SHOT"])

    # A help-panel click belongs to the app; only Escape or the red dot closes.
    move(700, 200); click(); time.sleep(0.3)
    assert "mines: closed" not in serial(), "help-panel click closed Mines"

    # 2. play the scripted game: snake over the board, space on every safe cell
    mines = board()
    for y in range(MH):
        xs = range(MW) if y % 2 == 0 else range(MW - 1, -1, -1)
        for i, x in enumerate(xs):
            if (y, x) not in mines:
                keys("spc"); time.sleep(0.08)
            if i < MW - 1:
                keys("right" if y % 2 == 0 else "left"); time.sleep(0.08)
        if y < MH - 1:
            keys("down"); time.sleep(0.08)
    if not wait_serial("mines: won", 5):
        fails.append("the scripted game did not win: the board, the reveal rule or the key path differs from the check")
    if "mines: boom" in serial():
        fails.append("the scripted game hit a mine: user/mines.c deals a different board than the check")

    # 3. Esc closes it cleanly and the desktop comes back
    exits = serial().count("MINES.BIN exited 0")
    keys("esc")
    if not wait_serial("mines: closed", 5):
        fails.append("Esc did not reach the program (no closed line)")
    released = wait_serial("syscall: window released, task gone", 5)
    for _ in range(50):
        if serial().count("MINES.BIN exited 0") > exits: break
        time.sleep(0.1)
    if not released or serial().count("MINES.BIN exited 0") <= exits:
        fails.append("Mines did not exit 0 and release its window on Esc")
    if "exception: ring-0" in serial() or "panic in" in serial() or "ring3app: BUG" in serial():
        fails.append("the kernel faulted or ring3app logged a BUG line")
    move(*PARK); time.sleep(0.5)
    for _ in range(100):
        if not near(pixel(CLOSE_X, CLOSE_Y), CLOSE_RED): break
        time.sleep(0.1)
    if near(pixel(CLOSE_X, CLOSE_Y), CLOSE_RED):
        fails.append("an app window is still open after Esc")
    move(SLOT0_X + 2 * PITCH + DOCK_ICON // 2, ICON_ROW_Y); time.sleep(0.3); click()
    opened = False
    for _ in range(40):
        time.sleep(0.1)
        if near(pixel(CLOSE_X, CLOSE_Y), CLOSE_RED): opened = True; break
    print(f"Mail opens from the dock after the close: {'yes' if opened else 'NO'}")
    if not opened: fails.append("Mail did not open from a dock click after Mines closed: desktop not responsive")
    keys("esc"); time.sleep(0.5)
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
print("PASS: Mines ran at ring 3 with its own window, a scripted game was won by keyboard, closed on Esc, and the desktop stayed alive")
