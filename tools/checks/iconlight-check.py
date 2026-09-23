#!/usr/bin/env python3
"""Headless proof that every dock icon is lit the macOS Big Sur way: one
light from the top, a few percent of falloff, a soft top highlight, and no
dark outline. Same boot + pmemsave shape as iconart-check.py: boot
kernel.elf with -display none, wait for a real present, dump the 1920x1080
framebuffer, measure each of the eleven dock tiles on its own background.

Why: the owner's feedback on the glossy pass was "icons still look too
Windows or Linux". Measured on that build's real capture, every tile ran
90-99 luminance from its top band to its bottom band (a vignetted dark
chip) and then darkened by another 17-28 across its last few rows (the rim
stroke's black bottom term, i.e. a dark outline). tools/gen/restyle_icons.py
now writes the opposite, and this check pins it.

Per tile, over physical columns 20..53 from the slot origin the other dock
checks use (that origin sits one logical pixel right of where the tile
really starts, so these are tile columns 22..55; either way well clear of
the corners), in rows no glyph reaches (the restyle table keeps glyphs out
of the top 16 and bottom 20 SVG units):

  top    = mean luminance of rows 3..7        (the lit band)
  bottom = mean luminance of rows 64..69      (the shaded band)
  hl     = mean luminance of row 0            (the inner top highlight)
  edge   = mean luminance of row 73           (the very bottom edge)

  1. lit from the top:  DEPTH_MIN <= top - bottom <= DEPTH_MAX
     (lighter at the top, but only by a few percent; the glossy tiles
     measured 90-99, these measure 16-28)
  2. a highlight line:  hl - top >= HL_MIN and >= HL_FRAC * (255 - top)
     (relative, because a white tile has only a few steps of headroom
     left above its own lit band, where a coloured one has a hundred)
  3. no dark outline:   bottom - edge <= EDGE_MAX
     (glossy tiles 17-28, these about 2)
  4. the bands are tile background, not glyph: every band row's
     max - min luminance across those columns <= BAND_RANGE_MAX, so the
     numbers above are measuring the tile and not a sun ray or a folder.

Confirmed discriminating: on the glossy build (art/icons from before this
pass) every one of the eleven fails 1 and 3 (and Weather 4); on this build
all eleven pass.

Usage: tools/checks/iconlight-check.py   (from the repo root, after make kernel.elf)
"""
import json, os, re, shutil, socket, subprocess, sys, time
from PIL import Image

LOG = "/tmp/jt-iconlight-serial.log"
DUMP = "/tmp/jt-iconlight.raw"
FB = 0xfd000000; W, H = 1920, 1080
PORT = 4487

# Same geometry iconart-check.py / dockhover-check.py use for this boot
# config (960x540 @2x, dock_scale_pct 7, GUI_ICON_COUNT 11).
DOCK_ICON, DOCK_GAP, SLOT0_X, ICON_TOP_Y, SCALE = 37, 6, 247, 469, 2
PITCH = DOCK_ICON + DOCK_GAP
SLOTS = ["Apps", "Files", "Mail", "Calendar", "Notes", "Reminders", "Terminal", "Chat", "Weather", "Stocks", "Trash"]
COLS = range(20, 54)
TOP_ROWS, BOT_ROWS, HL_ROW, EDGE_ROW = range(3, 8), range(64, 70), 0, 73
DEPTH_MIN, DEPTH_MAX = 6, 45   # glossy 90-99, Big Sur 16-28
HL_MIN, HL_FRAC = 3, 0.30  # row 0 at least 3 brighter AND 30% of the way from the band to white
EDGE_MAX = 6                   # glossy 17-28, Big Sur about 2
BAND_RANGE_MAX = 12

os.chdir(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
for f in (LOG, DUMP):
    try: os.remove(f)
    except FileNotFoundError: pass

# Wait on window_present_count, not a bare sleep, for the reason
# iconart-check.py documents (the back buffer can lag the sampled frame).
syms = {}
nm = shutil.which("nm") or "nm"
for line in subprocess.run([nm, "kernel.elf"], capture_output=True, text=True).stdout.splitlines():
    parts = line.split()
    if len(parts) == 3:
        syms[parts[2]] = int(parts[0], 16) - 0xC0000000
PRESENT = syms.get("window_present_count")

q = subprocess.Popen(["qemu-system-i386", "-name", "jt-iconlight", "-kernel", "kernel.elf",
                      "-display", "none", "-vga", "std",
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
    if before is not None:
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
    return (p[0] * 299 + p[1] * 587 + p[2] * 114) / 1000.0


fail = 0
for slot, name in enumerate(SLOTS):
    x0 = (SLOT0_X + slot * PITCH) * SCALE
    y0 = ICON_TOP_Y * SCALE
    rows = {}
    for y in list(TOP_ROWS) + list(BOT_ROWS) + [HL_ROW, EDGE_ROW]:
        v = [lum(img.getpixel((x0 + x, y0 + y))) for x in COLS]
        rows[y] = (sum(v) / len(v), max(v) - min(v))
    top = sum(rows[y][0] for y in TOP_ROWS) / len(TOP_ROWS)
    bot = sum(rows[y][0] for y in BOT_ROWS) / len(BOT_ROWS)
    band_range = max(rows[y][1] for y in list(TOP_ROWS) + list(BOT_ROWS))
    depth, hl, edge = top - bot, rows[HL_ROW][0] - top, bot - rows[EDGE_ROW][0]
    why = []
    if depth < DEPTH_MIN: why.append("not lit from the top")
    if depth > DEPTH_MAX: why.append("falloff too heavy (glossy/vignetted)")
    if hl < HL_MIN or hl < HL_FRAC * (255 - top): why.append("no top highlight")
    if edge > EDGE_MAX: why.append("dark bottom outline")
    if band_range > BAND_RANGE_MAX: why.append("glyph crosses the measured bands")
    print("%-10s top %5.1f bottom %5.1f depth %5.1f  highlight +%5.1f  bottom-edge drop %5.1f  band range %5.1f  %s"
          % (name, top, bot, depth, hl, edge, band_range, "ok" if not why else "FAIL: " + ", ".join(why)))
    if why:
        fail = 1

if fail:
    print("FAIL: a dock icon is not lit the Big Sur way (one top light, a few percent of falloff, highlight, no dark outline)")
    sys.exit(1)
print("PASS: all %d dock icons share one soft top light, a top highlight and no dark outline" % len(SLOTS))
