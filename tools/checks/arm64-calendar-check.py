#!/usr/bin/env python3
"""ARM64 M3c part 2: user/calendar.c, unchanged, opens its own window on the ARM desktop and draws the month, at EL0.

The caltest kernel (the clock fixed at CAL_UTC, 2026-03-31, a Tuesday) on QEMU's virt machine with ramfb and a USB keyboard
and mouse behind a hub (arm64-focus-check.py's setup). Through QMP it:
  - clicks the Calendar tile: the scheduler starts calendar ("M3c app calendar starts"), JT_SYS_WINDOW_OPEN puts up a window
    ("M3c window calendar 560x360 at X,Y") with the Calculator's frame, its red dot, and a title whose ink is the word
    Calendar exactly as the dock's own hover label draws it, centred; calendar.c says "today 2026-03-31 dow 2";
  - samples the framebuffer against calendar.c's own layout: the rule under the weekday names, ink in all 31 day cells and
    none past the 31st, and today's cell (row 4, column 2) filled with its accent colour, the only accent in the grid;
  - presses Right: "M3c event key 262 to calendar", and the month steps to April 2026: the accent circle is gone and the
    selection sits on the 30th (row 4, column 4), where it was not before;
  - presses Esc: calendar closes itself ("calendar: closed", "M3c app calendar exit 0"), the wallpaper is back pixel for
    pixel where the window stood beside the Console, and the Console is whole; then F2 and /help: the Terminal answers;
  - runs `userrun loop`, a test program that opens a window and yields forever, ignoring every event (Esc does nothing),
    and with it running: F1 opens Spotlight within a bounded time, five times over, and the Terminal answers /help. The
    desktop's loop resumed it all along ("ran N times", N > 50), and its red dot ends it ("closed by its red dot": it never
    reads the close click, so the kernel ends it), with the wallpaper back again and the Terminal still answering;
  - opens Calendar once more from Spotlight (F1, "cale", Enter) and clicks its red dot: the close click (y < 0, as on
    i386) reaches calendar, which says "calendar: closed" and exits 0.
That is the design call's proof: an app that loops forever on yield cannot take the desktop. The boot self-test still runs
calendar outside the scheduler, where its window call is -ENODEV ("calendar: no window"). No KERNEL CRASH, no load failure.
Skips (exit 0) when clang's aarch64 target, ld.lld or qemu-system-aarch64 is missing.
Usage: tools/checks/arm64-calendar-check.py   (from the repo root)
"""
import json, os, re, shutil, socket, subprocess, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from scratch import scratch_dir

root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
arch = os.path.join(root, "arch/arm64")
if not all(shutil.which(t) for t in ("clang", "ld.lld", "qemu-system-aarch64")):
    print("SKIP: clang, ld.lld or qemu-system-aarch64 not installed"); sys.exit(0)
CAL_UTC = 1775000000   # 2026-03-31 23:33:20 UTC; calendar.c reads the date in UTC
subprocess.run(["make", "-C", arch, "clean"], capture_output=True)
if subprocess.run(["make", "-C", arch, "CAL_UTC=%d" % CAL_UTC, "caltest"], capture_output=True).returncode:
    print("FAIL: arch/arm64 `make caltest` does not build"); sys.exit(1)
fails = []
def check(name, ok, detail=""):
    print(("  ok: " if ok else "  FAIL: ") + name + ("" if ok or not detail else " -- " + str(detail)[-400:]))
    if not ok: fails.append(name)

# The desktop at 800x600 (scale 1), the same math as kernel/dock_geom.c and fb_init.
W, H = 800, 600
icon = max(16, min(H * 7 // 100, (min(740, W - 40) - 2 * 10 - 10 * 6) // 11))
dock_x0, dock_y0 = (W - (11 * icon + 10 * 6 + 2 * 10)) // 2, H - icon - 2 * 10 - 24
def slot_centre(slot): return dock_x0 + 10 + slot * (icon + 6) + icon // 2, dock_y0 + 10 + icon // 2
con = ((W - 500) // 2, 100, 500, 350)   # the Console's frame
BG, ACCENT, RULE, SEL = (0xfa, 0xf8, 0xf6), (0xa0, 0x55, 0x3f), (0xdd, 0xd9, 0xd3), (0xed, 0xe6, 0xdc)   # calendar.c's colours
RED, WHITE = (0xff, 0x5f, 0x57), (255, 255, 255)

def dow(y, m, d):   # calendar.c's cal_dow: Zeller, 0 = Sunday
    if m < 3: m += 12; y -= 1
    K, J = y % 100, y // 100
    return ((d + 13 * (m + 1) // 5 + K + K // 4 + J // 4 + 5 * J) % 7 + 6) % 7
def month_layout(y, m, cw, ch):   # calendar.c's draw_month: the grid origin, cell size and each day's cell
    n = [31, 29 if y % 4 == 0 and (y % 100 or y % 400 == 0) else 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31][m - 1]
    first = dow(y, m, 1); rows = (first + n + 6) // 7
    cell_w = min(72, (cw - 16) // 7); x0 = (cw - 7 * cell_w) // 2; y0 = -32 + 150
    cell_h = max(32, min(44, (ch - y0 - 6) // rows))
    cells = {d: (x0 + (first + d - 1) % 7 * cell_w, y0 + (first + d - 1) // 7 * cell_h) for d in range(1, n + 1)}
    return x0, y0, cell_w, cell_h, rows, cells

tmp = scratch_dir("arm64-calendar")
log, sock = tmp + "/uart", tmp + "/qmp"
q = subprocess.Popen(["qemu-system-aarch64", "-machine", "virt", "-cpu", "cortex-a72", "-m", "256", "-nic", "none", "-device", "ramfb",
                      "-device", "pcie-root-port,id=rp,chassis=1", "-device", "qemu-xhci,id=x,bus=rp",
                      "-device", "usb-hub,id=h,bus=x.0,port=1",
                      "-device", "usb-kbd,bus=x.0,port=1.1", "-device", "usb-mouse,id=m1,bus=x.0,port=1.2",
                      "-display", "none", "-serial", "file:" + log, "-qmp", "unix:%s,server,nowait" % sock,
                      "-kernel", os.path.join(arch, "cal-kernel8.elf")], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
def uart(): return open(log, errors="replace").read() if os.path.exists(log) else ""
def wait_for(text, secs=6.0, after=0):
    end = time.time() + secs
    while time.time() < end:
        if text in uart()[after:]: return True
        time.sleep(0.05)
    return False
try:
    if not wait_for("usb ready: 1 kbd, 1 mouse", 40):
        fails.append(f"the keyboard and mouse behind the hub did not come up, got {uart()[-600:]!r}"); raise SystemExit
    boot = uart()
    check("the boot self-test still runs calendar outside the scheduler: its window call is -ENODEV and it says no window",
          re.search(r"M3c svc 384 a0=0x[0-9a-f]{16} -> -19\ncalendar: no window\nM3 EL0 exit 1\nM3c userland calendar ok", boot), boot[-600:])
    s_ = socket.socket(socket.AF_UNIX); s_.connect(sock); f = s_.makefile("rw"); f.readline()
    def cmd(c, **a):
        f.write(json.dumps({"execute": c, "arguments": a}) + "\n"); f.flush()
        while True:
            r = json.loads(f.readline())
            if "return" in r or "error" in r: return r
    cmd("qmp_capabilities")
    time.sleep(0.5)
    def shot():
        path = tmp + "/shot%d.ppm" % time.monotonic_ns()
        cmd("screendump", filename=path)
        for _ in range(30):
            if os.path.exists(path) and os.path.getsize(path) >= W * H * 3: break
            time.sleep(0.1)
        parts = open(path, "rb").read().split(b"\n", 3)
        return int(parts[1].split()[0]), parts[3]
    def px(img, x, y): i = (y * img[0] + x) * 3; return tuple(img[1][i:i + 3])
    def region(img, x, y, w, h): return [px(img, i, j) for j in range(y, y + h) for i in range(x, x + w)]
    def ink_mask(img, x, y, w, h):   # dark pixels in a box, as a set of offsets from the ink's own top-left corner
        pts = [(i, j) for j in range(y, y + h) for i in range(x, x + w) if max(px(img, i, j)) < 0x90]
        if not pts: return set(), 0, 0
        mx, my = min(p[0] for p in pts), min(p[1] for p in pts)
        return {(i - mx, j - my) for i, j in pts}, mx, max(p[0] for p in pts) - mx + 1
    pos = [W // 2, H // 2]
    def move_to(x, y):
        while (pos[0], pos[1]) != (x, y):
            dx = max(-100, min(100, x - pos[0])); dy = max(-100, min(100, y - pos[1]))
            cmd("input-send-event", events=[{"type": "rel", "data": {"axis": "x", "value": dx}}, {"type": "rel", "data": {"axis": "y", "value": dy}}])
            pos[0] += dx; pos[1] += dy
            if not wait_for(f"mouse {pos[0]},{pos[1]}\n", 4): return False
        return True
    def click(x, y, want):
        n = len(uart())
        if not move_to(x, y): fails.append(f"could not move the pointer to {x},{y}"); return False
        cmd("input-send-event", events=[{"type": "btn", "data": {"down": True, "button": "left"}}])
        cmd("input-send-event", events=[{"type": "btn", "data": {"down": False, "button": "left"}}])
        if not wait_for(want, 6, n): fails.append(f"a click at {x},{y} printed no {want!r}, got {uart()[n:][-300:]!r}"); return False
        time.sleep(0.3); return True
    def key(*qcodes): cmd("send-key", keys=[{"type": "qcode", "data": k} for k in qcodes]); time.sleep(0.12)
    def say(text, expect, secs=20):
        n = len(uart())
        for ch in text: key({" ": "spc", "/": "slash"}.get(ch, ch))
        key("ret")
        return wait_for(expect, secs, n)
    def until(pred, secs=4.0):   # a screendump that passes pred, or the last one tried
        end = time.time() + secs
        while True:
            img = shot()
            if pred(img) or time.time() > end: return img
            time.sleep(0.2)

    base = shot()
    # ---- Calendar from the dock ----
    n0 = len(uart())
    if not click(*slot_centre(3), "M3c window calendar first frame"): raise SystemExit
    out = uart()[n0:]
    check("the Calendar tile starts user/calendar.c under the scheduler and its window opens at EL0",
          "M3c app calendar starts" in out and re.search(r"M3c svc 384 a0=0x[0-9a-f]{16} -> 0\ncalendar: ring-3 window 560x360\n", out), out[-500:])
    check("calendar computed today from JT_SYS_TIME: 2026-03-31, a Tuesday", "calendar: today 2026-03-31 dow 2\n" in out, out[-300:])
    check("its file calls still get -ENOSYS (M3d) and load() fails quietly: no events, no fault",
          re.search(r"M3c svc 5 a0=0x[0-9a-f]{16} -> -38\ncalendar: loaded 0\n", out) and "M3 EL0 fault" not in out, out[-300:])
    m = re.search(r"M3c window calendar (\d+)x(\d+) at (\d+),(\d+)\n", out)
    if not m: fails.append("no 'M3c window calendar WxH at X,Y' line"); raise SystemExit
    cw, ch, cx0, cy0 = map(int, m.groups())
    fx, fy, fw, fh = cx0 - 8, cy0 - 32, cw + 16, ch + 40
    a = shot()
    check("the window wears the Calculator's frame: its red dot, and the content inside the screen", px(a, fx + 19, fy + 16) == RED and fx + fw <= W and fy + fh <= dock_y0, (fx, fy, fw, fh))
    title, tx, tw = ink_mask(a, fx + 80, fy + 4, fw - 100, 22)
    lx = slot_centre(3)[0]
    label, _, lw = ink_mask(a, lx - 38, dock_y0 - 22, 76, 19)   # the dock's hover label, "Calendar", the pointer is on its tile
    same = len(title ^ label) <= max(4, len(label) // 20)
    check("the title is Calendar: its ink is the dock label's word pixel for pixel (%d of %d differ), centred in the frame" % (len(title ^ label), len(label)),
          label and same and abs(tx + tw / 2 - (fx + fw / 2)) <= 2, (tw, lw, tx, fx))
    x0, y0, cwid, chgt, rows, cells = month_layout(2026, 3, cw, ch)
    rule = [px(a, cx0 + x0 + k, cy0 + y0 - 8) for k in range(0, 7 * cwid, 24)]
    check("the rule under the weekday names spans the grid", all(c == RULE for c in rule), rule)
    def cell_ink(img, d, cellmap):
        x, y = cellmap[d]
        return sum(1 for c in region(img, cx0 + x + 6, cy0 + y + 6, cwid - 12, chgt - 12) if c not in (BG, SEL))
    inked = [d for d in cells if cell_ink(a, d, cells) > 8]
    after = [(cx0 + x0 + c * cwid, cy0 + y0 + 4 * chgt) for c in range(3, 7)]   # row 4 past the 31st: blank
    blank = all(all(p == BG for p in region(a, x + 6, y + 6, cwid - 12, chgt - 12)) for x, y in after)
    check("the month grid: all 31 day cells of March 2026 have their digits, and the cells past the 31st are blank", len(inked) == 31 and blank, (len(inked), blank))
    tcx, tcy = cx0 + cells[31][0] + cwid // 2, cy0 + cells[31][1] + chgt // 2
    ring = [px(a, tcx - 12, tcy), px(a, tcx + 12, tcy), px(a, tcx, tcy - 12), px(a, tcx, tcy + 11)]
    stray = [(i, j) for j in range(cy0 + y0, cy0 + ch) for i in range(cx0, cx0 + cw) if px(a, i, j) == ACCENT and (i - tcx) ** 2 + (j - tcy) ** 2 > 16 * 16]
    check("today, the 31st (row 4, column 2), sits in the accent circle, the only accent in the grid", all(c == ACCENT for c in ring) and not stray, (ring, stray[:5]))
    # ---- Right steps the month: April 2026, the selection on the 30th ----
    n = len(uart()); key("right")
    check("Right reaches calendar as JT_KEY_RIGHT", wait_for("M3c event key 262 to calendar", 4, n), uart()[n:][-200:])
    apr = month_layout(2026, 4, cw, ch)[5]
    sx, sy = cx0 + apr[30][0] + 4, cy0 + apr[30][1] + 4
    b = until(lambda img: px(img, sx, sy) == SEL)
    check("the second sample differs where expected: no accent left in the grid, the selection moved to April 30 (row 4, column 4)",
          px(b, sx, sy) == SEL and px(a, sx, sy) == BG and px(b, tcx - 12, tcy) != ACCENT
          and not any(c == ACCENT for c in region(b, cx0, cy0 + y0, cw, ch - y0)), (px(a, sx, sy), px(b, sx, sy), px(b, tcx - 12, tcy)))
    check("the 28th is where the 31st was, in plain ink", cell_ink(b, 28, apr) > 8 and cells[31] == apr[28])
    # ---- Esc: calendar closes itself, the desktop is back ----
    n = len(uart()); key("esc")
    check("Esc closes Calendar: it says so, exits 0, and the window is freed", wait_for("M3c app calendar exit 0", 4, n) and "calendar: closed\n" in uart()[n:], uart()[n:][-300:])
    side = (con[0] + con[2] + 2, fy, fx + fw - (con[0] + con[2]) - 2, fh)   # the window's part right of the Console
    top = (fx, fy, fw, con[1] - fy - 2)                                         # and above it
    def desktop_back(img): return region(img, *side) == region(base, *side) and region(img, *top) == region(base, *top)
    c = until(desktop_back, 3)
    check("the wallpaper is back pixel for pixel where the window stood, and the Console is whole", desktop_back(c) and px(c, con[0] + 300, con[1] + 300) == WHITE)
    key("f2")
    check("F2 opens the Terminal", wait_for("terminal open", 4))
    time.sleep(0.3)
    check("the Terminal still answers after Calendar: /help", say("/help", "/clear  new conversation"), uart()[-300:])
    # ---- the looping program: yields forever, ignores Esc, the desktop stays usable ----
    n = len(uart())
    check("`userrun loop` starts the test program in its own window", say("userrun loop", "userrun loop: running in its window", 6) and "M3c window loop " in uart()[n:], uart()[n:][-300:])
    n = len(uart()); key("esc"); time.sleep(1.0)
    check("Esc does nothing to it: it never polls, so it is still running a second later", "M3c app loop" not in uart()[n:], uart()[n:][-200:])
    lat = []
    for _ in range(5):
        n = len(uart()); t = time.time(); key("f1")
        lat.append(time.time() - t if wait_for("spotlight open", 3, n) else 9.0)
        n = len(uart()); key("esc"); wait_for("spotlight close", 3, n)
    check("with it looping, F1 opens Spotlight every time within a bounded time (%s s)" % ", ".join("%.2f" % x for x in lat), max(lat) < 1.0, lat)
    n = len(uart()); key("f2")
    check("F2 brings the Terminal up over it", wait_for("focus terminal", 4, n), uart()[n:][-200:])
    time.sleep(0.3)
    t = time.time(); ok = say("/help", "/clear  new conversation")
    check("and the Terminal answers /help while it loops (%.1f s)" % (time.time() - t), ok, uart()[-300:])
    if click(fx + 24, fy + 16, "focus app") and click(fx + 24, fy + 16, "M3c app loop closed by its red dot"):
        m = re.search(r"M3c app loop closed by its red dot, ran (\d+) times\n", uart())
        check("its red dot ends it, and the desktop's loop had been resuming it all along (%s runs)" % (m.group(1) if m else "?"), m and int(m.group(1)) > 50)
        move_to(con[0] + 250, con[1] + 200)   # the arrow off the compared strips, over the Terminal
        d = until(desktop_back, 3)
        check("the wallpaper is back again where its window stood", desktop_back(d))
        check("and the Terminal still answers /help", say("/help", "/clear  new conversation"), uart()[-300:])
    # ---- Spotlight opens Calendar too ----
    n = len(uart()); key("f1"); wait_for("spotlight open", 3, n)
    for k in "cale": key(k)
    key("ret")
    check("Spotlight finds Calendar and Enter opens it again in its window", wait_for("M3c window calendar first frame", 6, n), uart()[n:][-300:])
    n = len(uart())
    if click(fx + 24, fy + 16, "M3c app calendar exit 0"):
        check("its red dot is i386's close: a click above the content (y < 0) that calendar answers by exiting 0",
              re.search(r"M3c event click \d+,-\d+ to calendar\ncalendar: closed\nM3 EL0 exit 0\n", uart()[n:]), uart()[n:][-300:])
    u = uart()
    for bad in ("KERNEL CRASH", "M3c load FAIL", "M3c userland calendar FAIL", "M3 userland FAIL", "oom"):
        check("%r never printed" % bad, bad not in u)
except SystemExit:
    pass
finally:
    q.kill(); q.wait()
subprocess.run(["make", "-C", arch, "clean"], capture_output=True)
if fails:
    for msg in fails: print("FAIL: " + msg)
    sys.exit(1)
print("PASS: user/calendar.c opens its own window on the ARM desktop from the dock and draws March 2026 with today in its accent; "
      "Right steps the month, Esc closes it and the wallpaper and the Terminal come back; a program that yields forever leaves "
      "Spotlight and the Terminal answering, and its red dot ends it")
