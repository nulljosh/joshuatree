#!/usr/bin/env python3
"""Headless proof that the dock icon contact shadow has a genuinely smooth
falloff, not a stepped one. Same shape as iconart-check.py / iconhalo-check.py:
boot kernel.elf with -display none, wait for a real window_present, pmemsave
the 1920x1080 framebuffer and measure actual pixels.

The oracle, in one line: a real gradient has no large single-step jumps down
its profile, a blocky one is made of them.

This measures the shadow's horizontal luminance profile on the rows just
under each icon's bottom edge, and reports two numbers per row: the largest
luminance change between adjacent pixels, and how many distinct luminances
the profile contains. A smooth penumbra moves a little at a time and visits
many values; a banded one sits on a few plateaus and jumps between them.

The real defect this was written against, measured on real captures rather
than argued from the code. gui_draw_icon_shadow used to squash dx into dy's
scale with `int sdx = dx * ry / rx` before taking the radius, integer
division, and at dock size rx is 37 while ry is 8, so sdx took 17 distinct
values across 75 real columns. The shadow was 9 flat plateaus about 4.5px
wide with a hard step between each. v58 had already moved the loop to
physical resolution and doubled ry, which halved the plateau width but left
the division, so the banding survived it.

Measured separation, all 10 dock slots, rows +2/+3/+4 under the icon:
    before   max single-step jump 20-21, distinct luminances 8-9
    after    max single-step jump 5,     distinct luminances 30-34
The thresholds below sit in the middle of both gaps, not at the edge of
either. Confirmed discriminating: restoring the old `sdx` line and
rebuilding fails this check on every slot; with the fix in place all pass.

Usage: tools/checks/dockshadow-check.py   (from the repo root, after make kernel.elf)
"""
import json, os, re, shutil, socket, subprocess, sys, time
from PIL import Image

LOG = "/tmp/jt-dockshadow-serial.log"
DUMP = "/tmp/jt-dockshadow.raw"
FB = 0xfd000000; W, H = 1920, 1080
PORT = 4455

# Same geometry every other dock check here already derived and verified for
# this 960x540@2x boot config (dock_scale_pct 7, GUI_ICON_COUNT 11).
DOCK_ICON, DOCK_GAP, SLOT0_X, ICON_TOP_Y, SCALE, SLOTS = 37, 6, 247, 469, 2, 11
PITCH = DOCK_ICON + DOCK_GAP
ROWS = (2, 3, 4)          # physical rows below the icon's own bottom edge
HALF = 38                 # sample half-width, just past the ellipse's rx of 37
MAX_STEP = 10             # before 20-21, after 5
MIN_DISTINCT = 20         # before 8-9, after 30-34

os.chdir(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
for f in (LOG, DUMP):
    try: os.remove(f)
    except FileNotFoundError: pass

# Higher-half kernel, and QMP's `xp` reads physical memory, so the nm address
# needs the 0xC0000000 offset taken off it. Same as editor_qa.py.
syms = {}
nm = shutil.which("nm") or "nm"
for line in subprocess.run([nm, "kernel.elf"], capture_output=True, text=True).stdout.splitlines():
    parts = line.split()
    if len(parts) == 3:
        syms[parts[2]] = int(parts[0], 16) - 0xC0000000
PRESENT = syms.get("window_present_count")

q = subprocess.Popen(["qemu-system-i386", "-kernel", "kernel.elf", "-display", "none", "-vga", "std",
                      "-qmp", "tcp:127.0.0.1:%d,server,nowait" % PORT, "-serial", "file:" + LOG],
                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
try:
    time.sleep(1.0)
    s = socket.create_connection(("127.0.0.1", PORT)); f = s.makefile("rw")
    def cmd(o):
        f.write(json.dumps(o) + "\n"); f.flush()
        while True:
            r = json.loads(f.readline())
            if "return" in r or "error" in r: return r

    def presents():
        if PRESENT is None:
            return None
        r = cmd({"execute": "human-monitor-command",
                 "arguments": {"command-line": "xp /4xb 0x%x" % PRESENT}})
        out = r.get("return", "")
        vals = [int(v, 16) for line in out.splitlines() if ":" in line
                for v in re.findall(r"0x([0-9a-f]{2})\b", line.split(":", 1)[1])]
        return int.from_bytes(bytes(vals[:4]), "little") if len(vals) >= 4 else None

    f.readline()
    cmd({"execute": "qmp_capabilities"})
    time.sleep(5.0)
    before = presents()
    if before is None:
        print("note: window_present_count not in this build, sampling on the sleep alone")
    else:
        for _ in range(100):
            if presents() != before:
                time.sleep(0.05)
                break
            time.sleep(0.05)
        else:
            print("FAIL: no frame was presented, the screen never updated")
            sys.exit(1)
    cmd({"execute": "pmemsave", "arguments": {"val": FB, "size": W * H * 4, "filename": DUMP}})
    try: cmd({"execute": "quit"})
    except (ConnectionResetError, BrokenPipeError, OSError): pass
finally:
    try: q.wait(timeout=5)
    except subprocess.TimeoutExpired: q.kill()

img = Image.frombytes("RGBA", (W, H), open(DUMP, "rb").read(), "raw", "BGRA").convert("RGB")


def lum(p):
    return (p[0] * 299 + p[1] * 587 + p[2] * 114) // 1000


fail = 0
worst_step, fewest = 0, 10 ** 9
for slot in range(SLOTS):
    cx = (SLOT0_X + slot * PITCH + DOCK_ICON // 2) * SCALE
    cy = (ICON_TOP_Y + DOCK_ICON) * SCALE
    for row in ROWS:
        y = cy - SCALE + row
        prof = [lum(img.getpixel((cx + dx, y))) for dx in range(-HALF, HALF + 1)]
        step = max(abs(prof[i + 1] - prof[i]) for i in range(len(prof) - 1))
        distinct = len(set(prof))
        worst_step = max(worst_step, step)
        fewest = min(fewest, distinct)
        if step > MAX_STEP or distinct < MIN_DISTINCT:
            print("slot %d row +%d: max step %2d (limit %d), distinct %2d (min %d)  BANDED"
                  % (slot, row, step, MAX_STEP, distinct, MIN_DISTINCT))
            fail = 1

print("worst max-step %d (limit %d), fewest distinct %d (min %d) across %d slots x %d rows"
      % (worst_step, MAX_STEP, fewest, MIN_DISTINCT, SLOTS, len(ROWS)))
if fail:
    print("FAIL: the dock icon shadow is stepped, not a smooth falloff")
    sys.exit(1)
print("PASS: every dock icon shadow falls off smoothly, no banding")
