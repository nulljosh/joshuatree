#!/usr/bin/env python3
"""Headless proof that the authored icon artwork is actually what the dock
is drawing, the same shape as iconhalo-check.py / iconedge-check.py: boot
kernel.elf with -display none, wait for the desktop's first frame, pmemsave
the real 1920x1080 framebuffer, and assert on actual dock pixels.

The oracle: each dock tile's own background colour, sampled on the real
capture, matches the colour its SVG in art/icons says it is. Every dock SVG
is written by tools/gen/restyle_icons.py and states its tile gradient in a
header comment ("Tile #RRGGBB -> #RRGGBB"); the tile fill is that
gradient over the full 128-unit canvas, so the expected colour at any row
is a straight lerp. Measured at two bands no glyph reaches (rows 3..7 and
64..69 of the 74-pixel tile, columns 20..53), every channel within TOL.

Why this and not the rim/body-depth oracles this check used to carry. Those
pinned the glossy technique itself (a >= 25 rim spike and >= 85 luminance
of top-to-bottom depth), and the Big Sur pass deliberately replaced that
technique: subtle 16-28 depth, a soft highlight, no dark rim. The lighting
property now lives in iconlight-check.py; this check keeps its original
job, "the authored art is what the dock is drawing", with a sharper oracle.
The runtime primitive fallback paints GUI_COLORS' own hues (dusty rose for
Mail where the art is blue, and so on), and any stale header or stale SVG
paints some other colour, so both miss by far more than TOL. Confirmed
discriminating: on the pre-Big-Sur build every slot misses by 118-186 on
some channel; on this build every slot is within 0.3.

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
# GUI_ICON_COUNT 11). Update these together if dock geometry changes.
DOCK_ICON, DOCK_GAP, SLOT0_X, ICON_TOP_Y, SCALE = 37, 6, 247, 469, 2
PITCH = DOCK_ICON + DOCK_GAP
# GUI_DOCK_DEFAULT order.
SLOTS = ["Apps", "Files", "Mail", "Calendar", "Notes", "Reminders", "Terminal", "Chat", "Weather", "Stocks", "Trash"]
SVGS = ["apps", "files", "mail", "calendar", "notes", "reminders", "terminal", "chat", "weather", "stocks", "trash"]
COLS = range(20, 54)
BANDS = (range(3, 8), range(64, 70))
TOL = 8

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


def hexrgb(h):
    return tuple(int(h[i:i + 2], 16) for i in (1, 3, 5))


fail = 0
for slot, (name, svg) in enumerate(zip(SLOTS, SVGS)):
    m = re.search(r"Tile (#[0-9A-Fa-f]{6}) -> (#[0-9A-Fa-f]{6})", open("art/icons/%s.svg" % svg).read())
    if not m:
        print("%-10s art/icons/%s.svg states no tile gradient" % (name, svg)); fail = 1; continue
    top, bot = hexrgb(m.group(1)), hexrgb(m.group(2))
    x0 = (SLOT0_X + slot * PITCH) * SCALE
    y0 = ICON_TOP_Y * SCALE
    worst = 0
    for band in BANDS:
        got = [0, 0, 0]; want = [0.0, 0.0, 0.0]; n = 0
        for y in band:
            t = (y + 0.5) / pw
            for x in COLS:
                p = img.getpixel((x0 + x, y0 + y))
                for c in range(3):
                    got[c] += p[c]; want[c] += top[c] + (bot[c] - top[c]) * t
                n += 1
        worst = max(worst, max(abs(got[c] - want[c]) / n for c in range(3)))
    ok = worst <= TOL
    print("%-10s tile %s -> %s  worst channel miss %5.1f  %s" % (name, m.group(1), m.group(2), worst, "ok" if ok else "WRONG ART"))
    if not ok:
        fail = 1

if fail:
    print("FAIL: a dock tile is not the colour its authored SVG says, the dock is not drawing that artwork")
    sys.exit(1)
print("PASS: all %d dock tiles match the colours their authored SVGs state" % len(SLOTS))
