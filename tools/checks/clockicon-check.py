#!/usr/bin/env python3
"""Headless proof that the Clock icon is a live analog face: its hands sit at
the current time (kernel gui_clock_draw_hands) instead of the old reused
grid-of-tiles glyph.

Two boots, QEMU -rtc base= at 03:00 and 09:00 (the same real CMOS path the
menu bar clock reads). Each opens the Apps folder, arrows down to Clock's row,
and dumps the real framebuffer. The Clock tile sits at a fixed grid cell. On the tile's face the hour hand points right at 3 and left
at 9 and the minute hand points up in both, so:

  1. the dark ink on the face's right half beats the left half at 03:00, and
     the left beats the right at 09:00 (the hands moved with the time);
  2. both boots have dark ink above the center (the minute hand at 12);
  3. the face holds a real white disc (the old glyph had a 3x3 grid, no disc);
  4. a third boot seeded 25s before a minute boundary, folder left open and
     untouched, shows the hands move when the minute rolls over.

Ports 4513-4515, inside the reserved 4511-4519 range.

Usage: tools/checks/clockicon-check.py   (from the repo root, after make kernel.elf)
"""
import json, os, socket, subprocess, sys, time
from PIL import Image

W, H = 1920, 1080
FB = 0xfd000000
LOGICAL_W, LOGICAL_H = 960, 540
DOCK_ICON, DOCK_GAP, SLOT0_X, ICON_ROW_Y = 37, 6, 247, 487
PITCH = DOCK_ICON + DOCK_GAP

os.chdir(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))

TRAY = (239, 235, 228)
FACE_WHITE_MIN = 3000  # Clock's face disc measures ~4400 white px in the tile box, Calculator ~1800
CLOSE_X, CLOSE_Y, CLOSE_RED = 80, 46, (0xFF, 0x5F, 0x57)  # the Apps folder window's close dot (ring-3 app windows sit lower, at 94,56)

def near(p, c, tol=12): return max(abs(p[i] - c[i]) for i in range(3)) <= tol

def boot(port, rtc, tag, later=0):
    log, dump = "/tmp/jt-clockicon-%s.log" % tag, "/tmp/jt-clockicon-%s.raw" % tag
    for f in (log, dump):
        try: os.remove(f)
        except FileNotFoundError: pass
    q = subprocess.Popen(["qemu-system-i386", "-kernel", "kernel.elf", "-display", "none", "-vga", "std",
                          "-rtc", "base=" + rtc, "-qmp", "tcp:127.0.0.1:%d,server,nowait" % port,
                          "-serial", "file:" + log], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        s = None
        for _ in range(50):
            time.sleep(0.2)
            try: s = socket.create_connection(("127.0.0.1", port)); break
            except OSError: pass
        if s is None: raise SystemExit("FAIL: QEMU's QMP socket never came up")
        f = s.makefile("rw")
        def cmd(o):
            f.write(json.dumps(o) + "\n"); f.flush()
            while True:
                r = json.loads(f.readline())
                if "return" in r or "error" in r: return r
        f.readline(); cmd({"execute": "qmp_capabilities"})
        def grab():
            cmd({"execute": "pmemsave", "arguments": {"val": FB, "size": W * H * 4, "filename": dump}})
            return Image.frombytes("RGBA", (W, H), open(dump, "rb").read(), "raw", "BGRA").convert("RGB")
        def wait_for(pred, secs, what):
            end = time.time() + secs
            while time.time() < end:
                if pred(grab()): return True
                time.sleep(0.3)
            print("note: timed out waiting for " + what); return False
        # Every wait below is for a state, not a delay: a loaded CI runner boots
        # and animates several times slower, and fixed sleeps captured the grid
        # mid-open (white disc 1756 px instead of ~4487) and mid-scroll.
        wait_for(lambda im: any(im.getpixel((960, y)) == TRAY for y in range(900, H, 4)), 90, "the desktop dock")
        def move(x, y):
            cmd({"execute": "input-send-event", "arguments": {"events": [
                {"type": "abs", "data": {"axis": "x", "value": int(x * 32768 / LOGICAL_W)}},
                {"type": "abs", "data": {"axis": "y", "value": int(y * 32768 / LOGICAL_H)}}]}})
        def key(qc):
            cmd({"execute": "send-key", "arguments": {"keys": [{"type": "qcode", "data": qc}]}}); time.sleep(0.35)
        move(SLOT0_X + DOCK_ICON // 2, ICON_ROW_Y); time.sleep(0.3)
        cmd({"execute": "input-send-event", "arguments": {"events": [{"type": "btn", "data": {"down": True, "button": "left"}}]}})
        time.sleep(0.1)
        cmd({"execute": "input-send-event", "arguments": {"events": [{"type": "btn", "data": {"down": False, "button": "left"}}]}})
        wait_for(lambda im: near(im.getpixel((CLOSE_X * 2 + 1, CLOSE_Y * 2 + 1)), CLOSE_RED), 60, "the Apps folder window")
        time.sleep(1.0)
        for _ in range(2): key("d")  # the folder reads w/a/s/d: Clock is APPS[23], grid position 22 (Portfolio hidden), row 4 col 2
        for _ in range(4): key("s")
        # Settled: the grid has scrolled to Clock (its full white face disc, ~4400 px;
        # the neighbouring Calculator tile has ~1800, and that is where a starved
        # runner's selection sits while the arrow keys are still queued) and the tile
        # is byte-identical across two frames half a second apart (hands drawn).
        img, prev, end = None, None, time.time() + 120
        while time.time() < end:
            img = grab(); box = tile_box(img)
            crop = img.crop(box).tobytes()
            if crop == prev and measure(img, box)[3] >= FACE_WHITE_MIN: break
            prev = crop; time.sleep(0.5)
        if later:
            # The folder stays open; poll until the minute rolls over and the hands move.
            before, after, end = img, img, time.time() + later
            while time.time() < end:
                time.sleep(1.0)
                after = grab()
                if face_diff(before, after) >= 20: break
            img = (before, after)
        try: cmd({"execute": "quit"})
        except (ConnectionResetError, BrokenPipeError, OSError): pass
        return img
    finally:
        try: q.wait(timeout=5)
        except subprocess.TimeoutExpired: q.kill()

def tile_box(img):
    # Clock is APPS[23]: row 4, col 3. Selecting it scrolls the grid to offset 2, so it sits in the
    # last visible row (gui_launch_apps: x0=(960-5*150)/2=105, y0=88, cell 150x116, tile 74, 2x scale).
    cx, cy = (105 + 2 * 150 + 75) * 2, (62 + 88 + 2 * 116 + 37) * 2  # +62: the Apps window's viewport top
    return cx - 60, cy - 60, cx + 60, cy + 60

def lum(p): return (p[0] * 299 + p[1] * 587 + p[2] * 114) // 1000

def measure(img, box):
    x0, y0, x1, y1 = box
    cx, cy = (x0 + x1) // 2, (y0 + y1) // 2
    r = (x1 - x0) // 3
    px = img.load()
    left = right = up = down = white = 0
    for y in range(cy - r, cy + r):
        for x in range(cx - r, cx + r):
            p = px[x, y]
            if (x - cx) ** 2 + (y - cy) ** 2 > r * r: continue
            if lum(p) > 235: white += 1
            if lum(p) < 90:
                if x < cx - 3: left += 1
                elif x > cx + 3: right += 1
                if y < cy - 3 and abs(x - cx) <= 3: up += 1
    return left, right, up, white

def face_diff(before, after):
    x0, y0, x1, y1 = tile_box(before)
    return sum(1 for y in range(y0 + 10, y1 - 10) for x in range(x0 + 10, x1 - 10)
               if (lum(before.getpixel((x, y))) < 90) != (lum(after.getpixel((x, y))) < 90))

fails = []
res = {}
for port, rtc, tag in ((4513, "2026-03-10T03:00:00", "a"), (4514, "2026-03-10T09:00:00", "b")):
    img = boot(port, rtc, tag)
    box = tile_box(img)
    if box is None:
        fails.append("%s: Clock tile (0x565A7A body) missing" % rtc); continue
    # Sample the tile's face only: the tile is square, the hands are inside its middle.
    res[tag] = measure(img, box)
    print("%s tile=%s left/right/up/white=%s" % (rtc, box, res[tag]))
if "a" in res and "b" in res:
    la, ra, ua, wa = res["a"]; lb, rb, ub, wb = res["b"]
    if not ra > la + 12: fails.append("03:00 hour hand should point right (left %d, right %d)" % (la, ra))
    if not lb > rb + 12: fails.append("09:00 hour hand should point left (left %d, right %d)" % (lb, rb))
    if ua < 15 or ub < 15: fails.append("minute hand at 12 missing (up ink %d, %d)" % (ua, ub))
    if wa < 400 or wb < 400: fails.append("no white face disc (white px %d, %d)" % (wa, wb))
# Redraw on the minute: seeded 25s before :30, the folder open and untouched.
before, after = boot(4515, "2026-03-10T03:29:35", "c", later=130)
diff = face_diff(before, after)
print("minute rollover: %d face pixels changed" % diff)
if diff < 20: fails.append("hands did not redraw when the minute changed (%d pixels differ)" % diff)
if fails:
    for x in fails: print("FAIL:", x)
    sys.exit(1)
print("PASS: Clock icon is a live analog face, hands follow the RTC")
