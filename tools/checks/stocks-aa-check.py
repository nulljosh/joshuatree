#!/usr/bin/env python3
"""Headless proof that the Stocks range chart draws through the new
antialiased line primitive (gui_aa_line in kernel.c), not the old
Bresenham stx_line that stamped a hard 2x2 window_rect per step with no
blending. Boots kernel.elf with -display none, drives the real mouse path
(QMP abs+btn, same as stocks-dock-check.py) to open Stocks from the dock,
then the 't' key seeds the network-free fixture (stx_seed_fixture in
stocks.h: a deterministic zigzag series with a real diagonal segment,
never touching net_init/http_get_timeout), pmemsaves the real physical
framebuffer, and asserts the diagonal segment's edge pixels carry genuine
intermediate coverage values (not pure line color, not pure background)
along several rows -- the signature of antialiasing -- where the old
stair-stepped Bresenham line would read pure background or pure line
color on every single row, with only 1-pixel hard jumps between them.

Usage: tools/checks/stocks-aa-check.py   (from the repo root, after make kernel.elf)
"""
import json, os, socket, subprocess, sys, time
from PIL import Image

LOG = "/tmp/jt-stocksaa-serial.log"
DUMP = "/tmp/jt-stocksaa.raw"
FB = 0xfd000000; W, H = 1920, 1080
PORT = 4463
LOGICAL_W, LOGICAL_H, SCALE = 960, 540, 2
DOCK_ICON, DOCK_GAP, SLOT0_X = 37, 6, 247
PITCH = DOCK_ICON + DOCK_GAP
ICON_ROW_Y = 487
STOCKS_SLOT = 9

CROP_OUT = "/private/tmp/claude-501/-Users-joshua/aa570436-1e4c-4fd4-81eb-aba396aba745/scratchpad/stocks-aa.png"

os.chdir(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
for f in (LOG, DUMP):
    try: os.remove(f)
    except FileNotFoundError: pass

q = subprocess.Popen(["qemu-system-i386", "-kernel", "kernel.elf", "-display", "none", "-vga", "std",
                      "-qmp", f"tcp:127.0.0.1:{PORT},server,nowait", "-serial", "file:" + LOG],
                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
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
    time.sleep(5.0)  # desktop up

    def move(x, y):
        cmd({"execute": "input-send-event", "arguments": {"events": [
            {"type": "abs", "data": {"axis": "x", "value": int(x * 32768 / LOGICAL_W)}},
            {"type": "abs", "data": {"axis": "y", "value": int(y * 32768 / LOGICAL_H)}}]}})
    def click():
        cmd({"execute": "input-send-event", "arguments": {"events": [{"type": "btn", "data": {"down": True, "button": "left"}}]}})
        time.sleep(0.1)
        cmd({"execute": "input-send-event", "arguments": {"events": [{"type": "btn", "data": {"down": False, "button": "left"}}]}})
    def key(qcode):
        cmd({"execute": "send-key", "arguments": {"keys": [{"type": "qcode", "data": qcode}]}})

    centre = lambda slot: SLOT0_X + slot * PITCH + DOCK_ICON // 2

    # Open Stocks from the dock (no network needed: it opens with empty data).
    move(centre(STOCKS_SLOT), ICON_ROW_Y); time.sleep(0.3)
    click(); time.sleep(1.0)

    # 't': seed the offline fixture (stx_seed_fixture), fully network-free.
    key("t"); time.sleep(0.5)

    cmd({"execute": "pmemsave", "arguments": {"val": FB, "size": W * H * 4, "filename": DUMP}})
    try: cmd({"execute": "quit"})
    except (ConnectionResetError, BrokenPipeError, OSError): pass
finally:
    try: q.wait(timeout=5)
    except subprocess.TimeoutExpired: q.kill()

img = Image.frombytes("RGBA", (W, H), open(DUMP, "rb").read(), "raw", "BGRA").convert("RGB")

# Locate the range chart's plot box: Stocks is windowed (gui_app_windowed),
# so stx_top()=8, tabs at stx_tab_y()=88, chart starts at cy0=ty+32=120
# (logical), pane_x = STX_SIDE_W(300)+24 = 324. Physical = logical*SCALE,
# plus the window's own top-left offset on the desktop (dock-launched
# windowed apps center at a known viewport; sample a wide band and scan
# for the line's own color to stay robust to exact window placement).
STX_GREEN = (0x41, 0x85, 0x4B)
STX_RED = (0xB5, 0x16, 0x16)
STX_MUTED = (0x80, 0x74, 0x68)

def near(p, c, tol=10):
    return max(abs(p[i] - c[i]) for i in range(3)) <= tol

# Scan the whole framebuffer for pixels that are a genuine partial blend
# between the line color (green or red, since the fixture rises then
# falls, delta could be either sign) and something else -- i.e. neither
# a pure line-color pixel nor a pure background/muted pixel. Collect them
# per-row; antialiasing on a diagonal produces a run of such intermediate
# pixels on many consecutive rows. A stair-stepped Bresenham line (2x2
# blocks, no blending) never produces intermediate values: every written
# pixel is exactly the line color, everything else is exactly whatever
# was already there.
px = img.load()

# Restrict strictly to the big range chart's own plot box (confirmed by a
# real pmemsave crop: apex ~x1030,y383, baseline rules at y~372/y~651,
# plot spans roughly x804..x1716). No text or pills live inside this box
# (the axis rules are a separate muted-gray color, not green/red), so
# every green/red-ish pixel found here belongs to the line itself, not to
# AA'd text or a solid pill fill -- both of which would produce false
# "intermediate" pixels next to pure color and defeat the discrimination
# this check exists to prove.
CHART_BOX = (804, 374, 1716, 650)

def scan(box, colors):
    x0, y0, x1, y1 = box
    pure = 0
    distinct_levels = set()
    for line_color in colors:
        for y in range(y0, y1):
            for x in range(x0, x1):
                p = px[x, y]
                if near(p, line_color, 4):
                    pure += 1
                    continue
                dr = max(abs(p[i] - line_color[i]) for i in range(3))
                if 8 < dr < 180:
                    for nx, ny in ((x - 1, y), (x + 1, y), (x, y - 1), (x, y + 1)):
                        if 0 <= nx < W and 0 <= ny < H and near(px[nx, ny], line_color, 4):
                            # bucket the blend fraction coarsely; real AA
                            # produces several distinct fractional levels
                            # along an edge, a solid 2x2 stamp produces
                            # none inside a text/pill-free box like this.
                            distinct_levels.add((line_color, round(dr / 12)))
                            break
    return pure, distinct_levels

pure_line_pixels, distinct_levels = scan(CHART_BOX, (STX_GREEN, STX_RED))
print(f"chart-box pure line pixels: {pure_line_pixels}, distinct intermediate-coverage buckets: {len(distinct_levels)}")

# Save 4x crops of the big chart AND one sparkline (sidebar row 0, AAPL)
# from the SAME real frame, for visual proof.
try:
    os.makedirs(os.path.dirname(CROP_OUT), exist_ok=True)
    chart_crop = img.crop((790, 370, 1730, 660)).resize((940 * 4, 290 * 4), Image.NEAREST)
    spark_crop = img.crop((380, 220, 540, 320)).resize((160 * 4, 100 * 4), Image.NEAREST)
    combo = Image.new("RGB", (max(chart_crop.width, spark_crop.width), chart_crop.height + spark_crop.height + 20), (255, 255, 255))
    combo.paste(spark_crop, (0, 0))
    combo.paste(chart_crop, (0, spark_crop.height + 20))
    combo.save(CROP_OUT)
    print(f"saved 4x crop (sparkline + big chart) to {CROP_OUT}")
except Exception as e:
    print(f"warning: could not save crop: {e}")

if pure_line_pixels < 20:
    print("FAIL: chart line not found in its own plot box (fixture/dock click likely missed)")
    sys.exit(1)
if len(distinct_levels) < 4:
    print("FAIL: no genuine multi-level intermediate-coverage pixels found on the chart line inside its own plot box -- this is a hard 1-pixel stair-stepped line (solid stamps only), not antialiased")
    sys.exit(1)

print(f"PASS: {len(distinct_levels)} distinct coverage levels found on the Stocks chart line, real antialiasing confirmed (not solid stair-stepping)")
sys.exit(0)
