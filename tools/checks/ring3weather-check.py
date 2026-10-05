#!/usr/bin/env python3
"""Weather runs as a real ring-3 process, the twentieth app out of the
kernel (roadmap 2.0, 1.9.22).

Boots headless with no NIC and `open=weat`, which launches Weather from the
dock path the moment the desktop is up. Weather is user/weather.c, a flat
binary run at CPL 3 through the table-driven launcher (kernel/ring3app.c,
RING3_APPS). The kernel's weather_fetch still owns the network and leaves
WEATHER.TXT for the app; with no NIC the file says "offline", so the window
must show the honest offline face over labelled sample data. The check:

  1. asserts the launch, SYS_WINDOW_OPEN, the app's "wxwin=offline sample"
     and the seven sample forecast days, and that the window is the cream
     Weather surface;
     1c. proves from framebuffer pixels (not serial) that every section of the
     2.10.0 window drew, at the geometry the sample data implies: the hero's sun
     disc, one terracotta high/low bar per day in the 7-day card at the x range its
     low and high give, the hourly strip's sun and cloud glyphs and its scroll
     thumb, and six detail tiles; then Tab/arrow keys: the terracotta focus ring
     appears on the details tiles, moves to the week card, Down picks the next
     day (selection pill moves, the tiles and their title change), and Right in
     the hourly strip scrolls it (the thumb moves, "Now" leaves);
  2. presses R: the app draws "Fetching..." (wxwin=fetching), calls SYS_REFRESH,
     the desktop loop fetches exactly once more and the same window reloads
     and shows the offline face again;
     Also crops the big temperature and the location label and asserts the
     type is antialiased: real intermediate colours along glyph edges, many
     distinct levels between the ink and the cream, not just two colours
     (ring-3 text comes from user/libjt/text.c);
  3. closes on Esc with a clean exit 0 and a released window, and asserts
     the desktop is back (Mail opens).

Every wait has a deadline. Discriminating: drop the RING3_APPS row and step 1
never sees the launch; skip draw_week, draw_hourly or draw_details in user/weather.c
and step 1c names the missing section; stop the kernel writing WEATHER.TXT and the face
reads "Not fetched yet" with no wxwin=offline; drop the SYS_REFRESH pickup and
step 2 sees no second fetch.
(tools/checks/weather-app-check.sh covers the live, stale, bad and timeout faces.)

Usage: tools/checks/ring3weather-check.py   (from the repo root, after make kernel.elf)
"""
import json, os, socket, subprocess, sys, time
from PIL import Image, ImageChops
from freeport import free_port

LOG = "/tmp/jt-ring3weather-serial.log"
DUMP = "/tmp/jt-ring3weather.raw"
FB = 0xfd000000; W, H = 1920, 1080
PORT = free_port()
LOGICAL_W, LOGICAL_H, SCALE = 960, 540, 2
DOCK_ICON, DOCK_GAP, SLOT0_X = 37, 6, 247
PITCH = DOCK_ICON + DOCK_GAP
ICON_ROW_Y = 487
CLOSE_X, CLOSE_Y = 94, 56
CLOSE_RED = (0xFF, 0x5F, 0x57)
VIEW_X, VIEW_Y = 78, 72   # gui_launch_from_dock: viewport at (x+8, y+32) for x=70, y=40
PARK = (480, 200)

os.chdir(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
for f in (LOG, DUMP):
    try: os.remove(f)
    except FileNotFoundError: pass

q = subprocess.Popen(["qemu-system-i386", "-kernel", "kernel.elf", "-append", "open=weat",
                      "-nic", "none", "-display", "none", "-vga", "std",
                      "-qmp", f"tcp:127.0.0.1:{PORT},server,nowait", "-serial", "file:" + LOG],
                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
fails = []
def serial():
    try: return open(LOG, errors="replace").read()
    except OSError: return ""
def wait_serial(needle, secs, count=1):
    for _ in range(int(secs * 10)):
        if serial().count(needle) >= count: return True
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
        time.sleep(0.35)  # faster than this drops scancodes on a loaded host
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
    # 1. the program is up and shows the offline face over labelled sample data
    if not wait_serial("ring3app: launching WEATHER.BIN at ring 3", 40):
        fails.append("Weather was never launched as a ring-3 program (open=weat flag or ring3app.c broken)")
    if not wait_serial("syscall: window opened for ring-3 task", 10):
        fails.append("SYS_WINDOW_OPEN never succeeded from ring 3")
    if not wait_serial("wxwin=offline sample", 15):
        fails.append("the app did not read the kernel's WEATHER.TXT and show the offline face (wxwin=offline sample)")
    if not wait_serial("wxrow=7 Mon,Tue,Wed,Thu,Fri,Sat,Sun facts=yes", 5):
        fails.append("the sample face did not draw its seven labelled forecast days")
    if not wait_serial("wxhours=24", 5):
        fails.append("the sample face did not carry 24 hourly entries")
    time.sleep(0.5)
    bg = pixel(VIEW_X + 5, VIEW_Y + 5)
    print(f"surface pixel: {bg}")
    if not near(bg, (0xF5, 0xF0, 0xEB), 6): fails.append(f"the window is not Weather's cream surface (got {bg})")

    # 1b. antialiased type: temperature (display face) and a label (body face)
    img = frame()
    if os.environ.get("JT_AA_DUMP"): img.save(os.environ["JT_AA_DUMP"])
    def aa_stats(x0, y0, x1, y1):
        lum = lambda p: (p[0] * 77 + p[1] * 150 + p[2] * 29) >> 8
        ink = mid = 0; levels = set()
        for yy in range(y0 * SCALE, y1 * SCALE):
            for xx in range(x0 * SCALE, x1 * SCALE):
                p = img.getpixel((xx, yy))
                if near(p, (0xF5, 0xF0, 0xEB), 4): continue
                if near(p, (0x40, 0x34, 0x39), 4) or near(p, (0x64, 0x50, 0x57), 4): ink += 1
                else: mid += 1; levels.add(lum(p) // 6)
        return ink, mid, len(levels)
    for name, box in (("temperature", (VIEW_X, VIEW_Y + 70, VIEW_X + 260, VIEW_Y + 140)),
                      ("label", (VIEW_X, VIEW_Y + 16, VIEW_X + 300, VIEW_Y + 38))):
        ink, mid, lv = aa_stats(*box)
        print(f"{name}: {ink} solid ink px, {mid} intermediate px, {lv} distinct levels")
        if ink < 20: fails.append(f"no {name} text found to measure (ink={ink})")
        elif mid < ink * 0.15 or lv < 5:
            fails.append(f"{name} is not antialiased: {mid} intermediate pixels vs {ink} solid, {lv} distinct levels")

    # 1c. every section is on screen, measured in framebuffer pixels. The window is 804x345 logical (wxdim),
    # wide, so the layout is fixed: overview x20 y12 w352, hourly x20 y176 w352 h157, week x386 y12 w398
    # h202 (rows 24 tall from y+30), details title y222 then two rows of three tiles 41 tall from y244.
    # Colours are user/weather.c's: ACCENT sun and bars, CLOUD, CARD tiles, SEL the picked day's pill,
    # TRACK and MID the hourly scroll bar. Sample data (offline) is fixed, so so is every number below.
    ACCENT, CLOUD, CARD, SEL, TRACK, MID = (0xB5, 0x50, 0x2C), (0xB9, 0xAE, 0xA6), (0xEC, 0xE5, 0xDC), (0xE1, 0xD6, 0xC9), (0xD9, 0xCD, 0xBF), (0x64, 0x50, 0x57)
    img = frame()
    def vp(vx, vy, im=None): return pixel(VIEW_X + vx, VIEW_Y + vy, im or img)
    def run(vy, lo_x, hi_x, col, im=None, tol=10):
        xs = [x for x in range(lo_x, hi_x) if near(vp(x, vy, im), col, tol)]
        return (min(xs), max(xs)) if xs else None
    sections = {}
    sections["overview (sun glyph)"] = near(vp(326, 78), ACCENT)                       # hero glyph at x+w-46, y+36+30
    sections["overview (fact cards)"] = all(near(vp(x, 148), CARD) for x in (28, 146, 266))   # three cards at y124..166
    # hourly: cell 54 wide from x32; hour 0 is a day-time clear sky (sun), hour 5 is cloud, the thumb is MID at its left end
    sections["hourly (sun at Now)"] = near(vp(59, 252), ACCENT)
    sections["hourly (cloud at hour 5)"] = near(vp(32 + 5 * 54 + 27, 252), CLOUD)
    sections["hourly (scroll thumb)"] = near(vp(36, 321), MID) and near(vp(300, 321), TRACK)
    # week: bars on one shared 9..21 scale from x580 to x722 (iw-50): Mon (row 0) lo 12 hi 21, Thu (row 3) lo 9 hi 15
    bars = {}
    for i, (lo, hi_) in enumerate(((12, 21), (11, 19), (10, 17), (9, 15), (10, 18), (11, 20), (11, 19))):
        a, z = 580 + (lo - 9) * 142 // 12, 580 + (hi_ - 9) * 142 // 12
        r = run(12 + 30 + i * 24 + 12, 570, 735, ACCENT)
        bars[i] = r is not None and abs(r[0] - (a - 3)) <= 3 and abs(r[1] - (z + 3)) <= 3
        if not bars[i]: print(f"week bar {i}: found {r}, wanted about {(a - 3, z + 3)}")
    sections["week (seven high/low bars at their scale)"] = all(bars.values())
    sections["week (picked-day pill on row 0)"] = near(vp(394, 12 + 30 + 4), SEL) and near(vp(394, 12 + 30 + 24 + 4), CARD)
    tile_ok = []
    for i in range(6):
        tx, ty = 386 + (i % 3) * 136, 222 + 22 + (i // 3) * 49
        tile_ok.append(near(vp(tx + 20, ty + 38), CARD) and near(vp(tx + 3, ty + 20), CARD) and not near(vp(tx + 20, ty + 43), CARD))
    sections["details (six tiles)"] = all(tile_ok)
    for name, ok in sections.items():
        print(f"section {name}: {'drawn' if ok else 'MISSING'}")
        if not ok: fails.append(f"the {name} did not draw in the framebuffer")
    def region(im, x0, y0, x1, y1): return im.crop(((VIEW_X + x0) * SCALE, (VIEW_Y + y0) * SCALE, (VIEW_X + x1) * SCALE, (VIEW_Y + y1) * SCALE))
    def changed(a, b, box): return ImageChops.difference(region(a, *box), region(b, *box)).convert('L').point(lambda v: 255 if v else 0).histogram()[255]
    ring = lambda im, x, y: near(vp(x, y, im), ACCENT, 8)
    # keys: no ring until one is pressed; Tab week -> details puts the ring round the six tiles
    if ring(img, 385, 280) or ring(img, 385, 100): fails.append("a focus ring is drawn before any key was pressed")
    keys("tab")
    if not wait_serial("wxsec=details day=0 hoff=0", 5): fails.append("Tab did not move focus to the details section")
    time.sleep(0.4); img_d = frame()
    if not ring(img_d, 385, 270): fails.append("no terracotta focus ring round the details tiles after Tab")
    keys("up")
    if not wait_serial("wxsec=week day=0 hoff=0", 5): fails.append("Up from details did not return to the week")
    keys("down"); keys("down")
    if not wait_serial("wxsec=week day=2 hoff=0", 5): fails.append("Down in the week did not pick the third day")
    time.sleep(0.4); img_w = frame()
    if not ring(img_w, 385, 100): fails.append("no terracotta focus ring round the 7-day card")
    if not (near(vp(394, 12 + 30 + 2 * 24 + 4, img_w), SEL) and near(vp(394, 12 + 30 + 4, img_w), CARD)):
        fails.append("the picked-day pill did not move to the third row")
    if changed(img, img_w, (386, 244, 784, 334)) < 60: fails.append("the details tiles did not change when another day was picked")
    if changed(img, img_w, (386, 222, 520, 240)) < 15: fails.append("the details title did not change to the picked day")
    keys("tab"); keys("tab")   # week -> details -> hourly
    if not wait_serial("wxsec=hourly day=2 hoff=0", 5): fails.append("Tab twice did not reach the hourly strip")
    keys("right"); keys("right")
    if not wait_serial("wxsec=hourly day=2 hoff=2", 5): fails.append("Right in the hourly strip did not scroll it")
    time.sleep(0.4); img_h = frame()
    if not ring(img_h, 19, 250): fails.append("no terracotta focus ring round the hourly card")
    # the thumb is 82 px of the 328 px track: it started at x32 and now starts at 32 + 246 * 2 / 18 = 59
    if not (near(vp(36, 321, img_h), TRACK) and near(vp(75, 321, img_h), MID)): fails.append("the hourly scroll thumb did not move")
    if changed(img, img_h, (20, 205, 100, 225)) < 20: fails.append("the hourly strip did not scroll (Now still first)")
    keys("left"); keys("left")
    if os.environ.get("JT_AA_DUMP"): img_h.save(os.environ["JT_AA_DUMP"] + ".keys.png")

    # 2. R: Fetching..., one more kernel fetch, a fresh run of the app
    fetches, launches = serial().count("wxfetch"), serial().count("launching WEATHER.BIN")
    keys("r")
    if not wait_serial("wxwin=fetching", 10): fails.append("R did not show the fetching state")
    wait_serial("wxwin=offline sample", 30, 2)  # SYS_REFRESH: the app stays up, reloads when the stamp moves
    if serial().count("launching WEATHER.BIN") != launches: fails.append("R relaunched the app; it must stay one window")
    if "WEATHER.BIN exited 7" in serial(): fails.append("the app still exits 7 on R")
    n = serial().count("wxfetch")
    if n != fetches + 1: fails.append(f"R did not cause exactly one more fetch (had {fetches}, now {n})")
    if serial().count("wxwin=offline sample") < 2: fails.append("the relaunched app did not draw the offline face again")

    # 3. Esc closes it cleanly
    exits = serial().count("WEATHER.BIN exited 0")
    keys("esc")
    if not wait_serial("WEATHER.BIN exited 0", 8, exits + 1):
        fails.append("Weather did not exit 0 on Esc")
    if not wait_serial("syscall: window released, task gone", 5):
        fails.append("Weather did not release its window on Esc")
    move(*PARK); time.sleep(0.5)
    for _ in range(100):
        if not near(pixel(CLOSE_X, CLOSE_Y), CLOSE_RED): break
        time.sleep(0.1)
    if near(pixel(CLOSE_X, CLOSE_Y), CLOSE_RED):
        fails.append("an app window is still open after Esc")

    # 5. the desktop must answer
    if "exception: ring-0" in serial() or "panic in" in serial() or "ring3app: BUG" in serial():
        fails.append("the kernel faulted or ring3app logged a BUG line")
    dock = pixel(480, 511)
    if dock != (0xEF, 0xEB, 0xE4):
        fails.append(f"desktop dock not on screen after the close (got {dock})")
    move(SLOT0_X + 2 * PITCH + DOCK_ICON // 2, ICON_ROW_Y); time.sleep(0.3); click()
    opened = False
    for _ in range(40):
        time.sleep(0.1)
        if near(pixel(CLOSE_X, CLOSE_Y), CLOSE_RED): opened = True; break
    print(f"Mail opens from the dock after the close: {'yes' if opened else 'NO'}")
    if not opened: fails.append("Mail did not open from a dock click after Weather closed: desktop not responsive")
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
print("PASS: Weather ran at ring 3 with its own window, showed the offline face from the kernel's WEATHER.TXT, R refetched once in the same window, Esc closed it, and the desktop stayed alive")
