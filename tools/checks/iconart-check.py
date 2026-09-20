#!/usr/bin/env python3
"""Headless proof that the authored icon artwork is actually what the dock
is drawing, the same shape as iconhalo-check.py / iconedge-check.py: boot
kernel.elf with -display none, wait for the desktop's first frame, pmemsave
the real 1920x1080 framebuffer, and assert on actual dock pixels.

What it asserts, and why that is a real oracle rather than "prints ok":
every authored tile (art/icons/*.svg) carries a 1.4px white rim light along
its top edge at 0.60 alpha, fading to nothing by 42% down the tile. That is
an authoring decision the old runtime primitive path structurally cannot
produce: gui_rounded_rect_gradient starts at gui_blend(bg, white) and
darkens MONOTONICALLY downward, so a primitive tile's top-centre column
falls by only a few luminance steps per pixel all the way down. An authored
tile spikes at its very first row and then drops 50-75 luminance in a
single pixel before settling into its own gradient.

Real measured separation on this build's own capture, top-centre column,
lum(y=0) - lum(y=1):
    authored   Files 51  Mail 58  Calendar 53  Notes 63  Terminal 75  Weather 69
    primitive  Apps 4    Reminders 3   Chat 3   Trash 4
The threshold below sits in the middle of that gap, not tuned to the edge
of it. Confirmed discriminating: zeroing the ICON_ART table entries in
kernel/icon_art.h and rebuilding drops every authored slot to the primitive
3-4 range and this check FAILs on all six; restored, it PASSes.

The primitive slots are printed, not asserted, on purpose: they are the
contrast baseline for a conversion that is deliberately incremental, and
they are expected to cross the threshold as each one gets its own artwork.

Usage: tools/checks/iconart-check.py   (from the repo root, after make kernel.elf)
"""
import json, os, re, shutil, socket, subprocess, sys, time
from PIL import Image

LOG = "/tmp/jt-iconart-serial.log"
DUMP = "/tmp/jt-iconart.raw"
FB = 0xfd000000; W, H = 1920, 1080
PORT = 4453

# Same geometry dockhover-check.py / iconhalo-check.py already derived and
# verified for this exact 960x540@2x boot config (dock_scale_pct 7,
# GUI_ICON_COUNT 10). Update these together if dock geometry changes.
DOCK_ICON, DOCK_GAP, SLOT0_X, ICON_TOP_Y, SCALE = 37, 6, 268, 469, 2
PITCH = DOCK_ICON + DOCK_GAP
# GUI_DOCK_DEFAULT order.
SLOTS = ["Apps", "Files", "Mail", "Calendar", "Notes", "Reminders", "Terminal", "Chat", "Weather", "Trash"]
# The slots whose icon index has authored artwork in tools/gen/gen_icon_art.py.
AUTHORED = {"Files", "Mail", "Calendar", "Notes", "Terminal", "Weather"}
RIM_MIN = 25

os.chdir(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
for f in (LOG, DUMP):
    try: os.remove(f)
    except FileNotFoundError: pass

# The generated header must match the SVGs it claims to come from, or the
# pixels below are proving something about a stale build artifact.
gen = subprocess.run([sys.executable, "tools/gen/gen_icon_art.py", "--check"],
                     capture_output=True, text=True)
print(gen.stdout.strip() or gen.stderr.strip())
if gen.returncode != 0:
    sys.exit(1)

# v0.78.0 put a real back buffer under every draw: a frame only reaches the
# visible framebuffer when window_present() copies it across. Sampling on a
# fixed sleep alone races that present and can read the previous frame, so
# resolve window_present_count out of the ELF and wait for it to actually
# move, the same technique editor_qa.py's presented() already uses.
# This kernel is higher-half (linked at 0xC0000000+, loaded physically at
# 1MB) and QMP's `xp` reads PHYSICAL memory, so the nm address has to have
# the offset taken off it, exactly as editor_qa.py already does. Without
# that subtraction the read lands on unrelated physical memory that can
# change on its own, which would let this wait pass for the wrong reason.
# `nm` (binutils), not `llvm-nm`: the latter comes from an apt package CI's
# install line does not pull in, the trap walldefault-check.sh already hit.
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
        """Read window_present_count out of guest memory, or None."""
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
    time.sleep(5.0)  # desktop up, same margin dockhover-check.py uses
    # Then wait for a real present, so the bytes sampled below are a frame
    # that actually reached the screen and not the one before it.
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
    # QEMU can tear the QMP socket down the instant it processes quit,
    # before this side reads a reply; a reset on cleanup is expected, not a
    # masked failure of the assertions above.
    try: cmd({"execute": "quit"})
    except (ConnectionResetError, BrokenPipeError, OSError): pass
finally:
    try: q.wait(timeout=5)
    except subprocess.TimeoutExpired: q.kill()

img = Image.frombytes("RGBA", (W, H), open(DUMP, "rb").read(), "raw", "BGRA").convert("RGB")
pw = DOCK_ICON * SCALE


def lum(p):
    return (p[0] * 299 + p[1] * 587 + p[2] * 114) // 1000


fail = 0
for slot, name in enumerate(SLOTS):
    x0 = (SLOT0_X + slot * PITCH) * SCALE
    y0 = ICON_TOP_Y * SCALE
    cx = x0 + pw // 2
    rim = lum(img.getpixel((cx, y0))) - lum(img.getpixel((cx, y0 + 1)))
    if name in AUTHORED:
        ok = rim >= RIM_MIN
        print("%-10s rim falloff %3d  %s" % (name, rim, "ok" if ok else "TOO FLAT"))
        if not ok:
            fail = 1
    else:
        print("%-10s rim falloff %3d  (primitive, not asserted)" % (name, rim))

if fail:
    print("FAIL: an authored dock icon has no rim light, the artwork path is not what drew it")
    sys.exit(1)
print("PASS: all %d authored dock icons carry their rim light, artwork path is live" % len(AUTHORED))
