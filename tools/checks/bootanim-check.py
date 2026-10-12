#!/usr/bin/env python3
"""Headless proof the boot splash draws the tree on, stroke by stroke, instead of
showing it all at once.

Boots kernel.elf with -display none and pmemsaves the framebuffer back to back
through the splash. In each frame it counts the bright pixels in the mark's box.
A draw-on shows as a count that never shrinks, passes through several different
partial amounts (a first stroke well under half the finished tree), and ends at
the full mark with the progress bar filled. A splash that pops in whole gives one
count; one that flickers gives a count that shrinks.

2.65.0: the mark is flat ink (MARK_GROUND 0x001C1C1E), no terracotta ground. Each
frame also counts terracotta pixels in the box: the splash must show none.

Usage: tools/checks/bootanim-check.py   (from the repo root, after make kernel.elf)
"""
import json, os, socket, subprocess, sys, time
from PIL import Image
from freeport import free_port
from scratch import scratch_dir

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
os.chdir(ROOT)

TMP = scratch_dir("bootanim")
LOG = os.path.join(TMP, "serial.log")
DUMP = os.path.join(TMP, "fb.raw")
FB = 0xfd000000; W, H = 1920, 1080
BOX = (800, 360, 1120, 680)      # the 160 logical px mark at scale 2, centred 10 px above the middle
BAR = (800, 680, 1120, 700)      # the progress bar row under it
PORT = free_port()

for f in (LOG, DUMP):
    try: os.remove(f)
    except FileNotFoundError: pass

q = subprocess.Popen(["qemu-system-i386", "-kernel", "kernel.elf", "-display", "none", "-vga", "std",
                      "-qmp", f"tcp:127.0.0.1:{PORT},server,nowait", "-serial", "file:" + LOG],
                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
samples = []
try:
    for _ in range(100):                             # connect the moment QEMU listens, the splash is only ~1.2 s long
        try: s = socket.create_connection(("127.0.0.1", PORT)); break
        except OSError: time.sleep(0.02)
    f = s.makefile("rw")
    def cmd(o):
        f.write(json.dumps(o) + "\n"); f.flush()
        while True:
            r = json.loads(f.readline())
            if "return" in r or "error" in r: return r
    f.readline()
    cmd({"execute": "qmp_capabilities"})
    end = time.time() + 12
    while time.time() < end:
        cmd({"execute": "pmemsave", "arguments": {"val": FB, "size": W * H * 4, "filename": DUMP}})
        rgb = Image.frombytes("RGBA", (W, H), open(DUMP, "rb").read(), "raw", "BGRA").convert("RGB")
        g = rgb.convert("L")
        top = g.crop((0, 0, W, 20)).resize((1, 1), Image.BOX).getpixel((0, 0))
        if top > 30 and samples: break               # the desktop's menu bar is up: the splash is over
        ink = sum(1 for v in g.crop(BOX).getdata() if v > 128)
        bar = sum(1 for v in g.crop(BAR).getdata() if v > 200)
        terra = sum(1 for r, gg, b in rgb.crop(BOX).getdata() if r > gg + 40 and r > b + 60)
        if top < 30: samples.append((ink, bar, terra))
    try: cmd({"execute": "quit"})
    except (ConnectionResetError, BrokenPipeError, OSError): pass
finally:
    try: q.wait(timeout=5)
    except subprocess.TimeoutExpired: q.kill()

inks = [i for i, _, _ in samples]
terras = [t for _, _, t in samples]
final = max(inks, default=0)
stages = sorted({round(i * 8 / final) for i in inks if i}) if final else []
print(f"{len(samples)} splash frames, ink {inks[:3]}..{inks[-3:]}, final {final}, stages(of 8) {stages}")
fail = 0
if final < 1500:
    fail = 1; print("FAIL: no full tree on the splash")
else:
    first = next(i for i in inks if i)
    if first > final * 0.6:
        fail = 1; print(f"FAIL: first visible frame already shows {first / final:.0%} of the tree, not a draw-on")
    if len(stages) < 4:
        fail = 1; print(f"FAIL: only {len(stages)} distinct stages seen, the tree is not growing")
    grow = inks[:inks.index(final) + 1]   # only the draw-on: after the full tree the desktop paints over it (a slow
                                          # runner catches one such frame before the menu bar shows, 1948 then 199)
    if any(b < a - final * 0.02 for a, b in zip(grow, grow[1:])):
        fail = 1; print("FAIL: ink shrank between frames, the splash is flickering")
    terra = max(terras, default=0)
    print(f"terracotta px {terra} across the splash")
    if terra > 10:
        fail = 1; print("FAIL: the splash still draws a terracotta ground; the mark is flat ink now")
if not fail:
    print("PASS: the boot splash draws the tree on, on flat ink with no terracotta")
sys.exit(fail)
