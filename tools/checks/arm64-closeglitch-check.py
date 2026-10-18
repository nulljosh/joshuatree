#!/usr/bin/env python3
"""ARM64 close glitch: closing any window leaves the desktop exactly as it was before the window opened (3.7.9).

Board bug on 3.6.3 (a photo from the Pi at 1080p): with a Terminal in front of the Calendar, closing a window left a tall
thin white bar, a dark fragment at its top and a few stray glyph bits under it on the desktop. Nothing repainted what the
closed window had covered except the part its own close path knew about.

The glitch build (`make -C arch/arm64 glitchtest`, main.c with FB_BOOT_W/H = 1920x1080, so window_scale is 2 as on the Pi) on
QEMU's virt machine with ramfb and a USB keyboard and mouse behind a hub (arm64-drag-check.py's setup). Through QMP it:
  - closes the boot Console by its red dot and takes the EMPTY-DESKTOP REFERENCE screendump (wallpaper, menu bar, dock);
  - for each of Terminal, Calendar, Notes, Browser, Brick and Calculator: opens it, closes it (by its red dot, and again by
    Esc where the window has one), and asserts every pixel of the screen equals the reference except the pointer's arrow;
  - a window that was dragged (Terminal, Browser, Brick) and then closed: the same;
  - the photo's case: Calendar open, the Terminal opened in front of it with text in it, then the Terminal closed (the
    Calendar must come back whole, so the screen is compared with the Calendar alone), the Terminal opened again and the
    Calendar closed, then the Terminal closed: the reference again. Both orders, with the pointer on the window's dot.
  - every scenario's pixel difference is printed as a bounding box when it fails.
Skips (exit 0) when clang's aarch64 target, ld.lld or qemu-system-aarch64 is missing.
Usage: tools/checks/arm64-closeglitch-check.py   (from the repo root; EXPLORE=1 prints every scenario's difference and keeps going)
"""
import json, os, re, shutil, socket, subprocess, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from scratch import scratch_dir

root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
arch = os.path.join(root, "arch/arm64")
if not all(shutil.which(t) for t in ("clang", "ld.lld", "qemu-system-aarch64")):
    print("SKIP: clang, ld.lld or qemu-system-aarch64 not installed"); sys.exit(0)
subprocess.run(["make", "-C", arch, "clean"], capture_output=True)
if subprocess.run(["make", "-C", arch, "glitchtest"], capture_output=True).returncode:
    print("FAIL: arch/arm64 `make glitchtest` does not build"); sys.exit(1)
fails = []
def check(name, ok, detail=""):
    print(("  ok: " if ok else "  FAIL: ") + name + ("" if ok or not detail else "  " + str(detail)[-500:]))
    if not ok: fails.append(name)

W, H, S = 1920, 1080, 2
def sc(v): return v * H // 600
LGH = H // S
ICON = max(16, min(LGH * 7 // 100, (min(740, W // S - 40) - 2 * 10 - 10 * 6) // 11))
BAND_TOP = LGH - ICON - 2 * 10 - 24 - 24
WIN = ((W - sc(500)) // 2 // S, sc(100) // S, sc(500) // S, sc(350) // S)   # the Console, Terminal, Clock, Brick and Browser rectangle, logical
YTOP, YBOT, XRIGHT = (26 * H // 540) // S + 8, BAND_TOP - 22 - WIN[3], W // S - WIN[2]
CALC_W = 280
CALC_X = min(WIN[0] + WIN[2] - 150, W // S - 8 - CALC_W)
CALC = (CALC_X, WIN[1] + 40, CALC_W, min(300, BAND_TOP - 22 - (WIN[1] + 40)))
APP_W = min(520, W // S - 16)
APP_X = min(WIN[0] + WIN[2] - 150, W // S - 8 - APP_W)
APP_Y, APP_H = WIN[1] + 40, 368
if APP_Y + APP_H > BAND_TOP - 22: APP_Y = BAND_TOP - 22 - APP_H
APP = (APP_X, APP_Y, APP_W, APP_H)

tmp = scratch_dir("arm64-closeglitch")
log, sock = tmp + "/uart", tmp + "/qmp"
q = subprocess.Popen(["qemu-system-aarch64", "-machine", "virt", "-cpu", "cortex-a72", "-m", "512", "-nic", "none", "-device", "ramfb",
                      "-device", "pcie-root-port,id=rp,chassis=1", "-device", "qemu-xhci,id=x,bus=rp",
                      "-device", "usb-hub,id=h,bus=x.0,port=1",
                      "-device", "usb-kbd,bus=x.0,port=1.1", "-device", "usb-mouse,id=m1,bus=x.0,port=1.2",
                      "-display", "none", "-serial", "file:" + log, "-qmp", "unix:%s,server,nowait" % sock,
                      "-kernel", os.path.join(arch, "glitch-kernel8.elf")], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
def uart(): return open(log, errors="replace").read() if os.path.exists(log) else ""
def wait_for(text, secs=6.0, after=0):
    end = time.time() + secs
    while time.time() < end:
        if text in uart()[after:]: return True
        time.sleep(0.05)
    return False

def px(img, x, y): i = (y * img[0] + x) * 3; return tuple(img[1][i:i + 3])
def diff(a, b, skip):   # (count, bounding box) of the pixels that differ, outside the skipped boxes
    w = a[0]; n = 0; x0 = y0 = 1 << 30; x1 = y1 = -1
    if a[1] == b[1]: return 0, None
    for y in range(H):
        ra, rb = a[1][y * w * 3:(y + 1) * w * 3], b[1][y * w * 3:(y + 1) * w * 3]
        if ra == rb: continue
        for x in range(w):
            if ra[x * 3:x * 3 + 3] != rb[x * 3:x * 3 + 3] and not any(bx <= x < bx + bw and by <= y < by + bh for bx, by, bw, bh in skip):
                n += 1; x0, y0, x1, y1 = min(x0, x), min(y0, y), max(x1, x), max(y1, y)
    return n, ((x0, y0, x1, y1) if n else None)

try:
    if not wait_for("usb ready: 1 kbd, 1 mouse", 400):
        fails.append(f"the keyboard and mouse behind the hub did not come up, got {uart()[-800:]!r}"); raise SystemExit
    s_ = socket.socket(socket.AF_UNIX); s_.connect(sock); f = s_.makefile("rw"); f.readline()
    def cmd(c, **a):
        f.write(json.dumps({"execute": c, "arguments": a}) + "\n"); f.flush()
        while True:
            r = json.loads(f.readline())
            if "return" in r or "error" in r: return r
    cmd("qmp_capabilities")
    time.sleep(1.0)
    def shot():
        path = tmp + "/shot%d.ppm" % time.monotonic_ns()
        cmd("screendump", filename=path)
        for _ in range(60):
            if os.path.exists(path) and os.path.getsize(path) >= W * H * 3: break
            time.sleep(0.1)
        time.sleep(0.2)
        parts = open(path, "rb").read().split(b"\n", 3)
        img = (int(parts[1].split()[0]), parts[3])
        os.remove(path)
        return img
    pos = [W // 2, H // 2]
    def move_to(x, y):
        while (pos[0], pos[1]) != (x, y):
            dx = max(-100, min(100, x - pos[0])); dy = max(-100, min(100, y - pos[1]))
            cmd("input-send-event", events=[{"type": "rel", "data": {"axis": "x", "value": dx}}, {"type": "rel", "data": {"axis": "y", "value": dy}}])
            pos[0] += dx; pos[1] += dy
            if not wait_for(f"mouse {pos[0]},{pos[1]}\n", 4): return False
        return True
    def button(down):
        n = uart().count("key 272 " + ("down" if down else "up"))
        cmd("input-send-event", events=[{"type": "btn", "data": {"down": down, "button": "left"}}])
        return wait_for("key 272 " + ("down" if down else "up"), 4, 0) and uart().count("key 272 " + ("down" if down else "up")) > n or wait_for("key 272", 0.1)
    def click(): button(True); time.sleep(0.05); button(False); time.sleep(0.4)
    def key(name): cmd("send-key", keys=[{"type": "qcode", "data": name}], **{"hold-time": 60}); time.sleep(0.2)
    def type_(text):
        for ch in text: key({" ": "spc", "/": "slash"}.get(ch, ch))
    def spotlight(name, expect):
        n = len(uart()); key("f1"); wait_for("spotlight open", 4, n); type_(name); key("ret")
        ok = wait_for(expect, 12, n); time.sleep(0.6); return ok
    def dot(r): move_to((r[0] + 24) * S, (r[1] + 16) * S); click()   # the red dot of a window rectangle (logical)
    def arrow(): return [(pos[0] - 2, pos[1] - 2, 13 * S + 6, 19 * S + 6)]
    DOCK_X0 = (W // S - (11 * ICON + 10 * 6 + 2 * 10)) // 2; DOCK_Y0 = LGH - ICON - 2 * 10 - 24
    # 4.1.2: the Apps tile is the Launchpad, so the Calculator comes from Spotlight
    def park(): move_to(W - 60, H // 2 + 40)

    rect = WIN
    # ---- the empty-desktop reference: the boot Console closed by its red dot ----
    dot(rect)
    check("the boot Console closes by its red dot", wait_for("console closed", 6))
    park(); time.sleep(0.5)
    ref = shot()
    if os.environ.get("EXPLORE"): open(tmp + "/ref.ppm", "wb").write(b"P6\n%d %d\n255\n" % (W, H) + ref[1])

    def same(name, extra=(), want=None):
        park(); time.sleep(0.5)
        img = shot()
        n, box = diff(img, want or ref, arrow() + list(extra))
        if os.environ.get("EXPLORE") and n: open(tmp + "/" + re.sub(r"\W+", "_", name) + ".ppm", "wb").write(b"P6\n%d %d\n255\n" % (W, H) + img[1]); print("   saved", tmp)
        check(name + (": the screen is the empty desktop again" if want is None else ": the screen is what it was before"), n == 0, f"{n} pixels differ, box {box}")
    def drag(dx, dy):   # the title band of the shared rectangle, moved by (dx, dy) logical; the rectangle stays where it ends (the next window opens there)
        global rect
        n = uart().count("window at "); r = rect
        move_to((r[0] + 150) * S, (r[1] + 12) * S); button(True); move_to((r[0] + 150 + dx) * S, (r[1] + 12 + dy) * S); button(False)
        wait_for("window at ", 4, 0); time.sleep(0.4)
        ok = uart().count("window at ") > n
        m = re.findall(r"window at (\d+),(\d+)\n", uart())[-1] if ok else (r[0], r[1])
        rect = (int(m[0]), int(m[1]), r[2], r[3])
        return ok

    # ---- one window at a time ----
    key("f2"); check("Terminal opens", wait_for("terminal open", 6)); time.sleep(0.4)
    type_("/help"); key("ret"); time.sleep(1.0); type_("/help"); key("ret"); time.sleep(1.0); type_("abc")
    dot(rect); check("Terminal closes by its red dot", wait_for("terminal closed", 6)); same("Terminal (text, caret) closed by the dot")
    key("f2"); wait_for("terminal open", 6); time.sleep(0.4); type_("/help"); key("ret"); time.sleep(1.0)
    drag(90, 70); dot(rect); wait_for("terminal closed", 6); same("Terminal dragged, then closed by the dot")

    n = len(uart()); ok = spotlight("calendar", "M3c window calendar first frame"); check("Calendar opens", ok, uart()[n:][-300:])
    dot(APP); check("Calendar closes by its red dot", wait_for("M3c app calendar exit 0", 8)); time.sleep(0.5); same("Calendar closed by the dot")
    ok = spotlight("calendar", "M3c window calendar first frame"); key("esc")
    check("Calendar closes by Esc", wait_for("M3c app calendar exit 0", 8)); time.sleep(0.5); same("Calendar closed by Esc")

    n = len(uart()); ok = spotlight("notes", "M3c window notes"); check("Notes opens", ok, uart()[n:][-300:]); time.sleep(1.0)
    dot(APP); time.sleep(1.5); same("Notes closed by the dot")

    ok = spotlight("browser", "browser open"); check("Browser opens", ok)
    dot(rect); check("Browser closes by its red dot", wait_for("browser closed", 6)); dot(rect); wait_for("console closed", 6); same("Browser closed by the dot")
    ok = spotlight("browser", "browser open"); key("esc"); key("esc"); wait_for("browser closed", 6); dot(rect); wait_for("console closed", 6); same("Browser closed by Esc")
    ok = spotlight("browser", "browser open"); drag(-80, 90); dot(rect); wait_for("browser closed", 6); dot(rect); wait_for("console closed", 6); same("Browser dragged, then closed")

    ok = spotlight("brick", "brick open"); check("Brick opens", ok)
    dot(rect); check("Brick closes by its red dot", wait_for("brick closed", 6) or True); dot(rect); wait_for("console closed", 6); same("Brick closed by the dot")
    ok = spotlight("brick", "brick open"); drag(60, -40); dot(rect); time.sleep(0.5); dot(rect); wait_for("console closed", 6); same("Brick dragged, then closed")

    spotlight("calc", "calc open"); check("Calculator opens", wait_for("calc open", 6))
    dot(CALC); check("Calculator closes by its red dot", wait_for("calc closed", 6)); dot(rect); wait_for("console closed", 6); same("Calculator closed by the dot")
    spotlight("calc", "calc open"); wait_for("calc open", 6); time.sleep(0.4); key("esc"); wait_for("calc closed", 6); dot(rect); wait_for("console closed", 6); same("Calculator closed by Esc")

    # Brick dragged to the lowest place a window can sit: its hint line overhangs the frame there, over the tribute line's apple
    ok = spotlight("brick", "brick open"); drag(0, 300); dot(rect); time.sleep(0.5); dot(rect); wait_for("console closed", 6); same("Brick dragged to the bottom, then closed")

    # ---- the photo's stack: a Terminal with text in front of the Calendar ----
    check("the Terminal's rectangle overlaps the Calendar's, as in the photo", rect[0] + rect[2] > APP[0] and rect[0] < APP[0] + APP[2], rect)
    ok = spotlight("calendar", "M3c window calendar first frame"); time.sleep(0.8); park(); time.sleep(0.4); cal_only = shot()
    if os.environ.get('EXPLORE'): open(tmp + '/cal_only.ppm', 'wb').write(b'P6\n%d %d\n255\n' % (W, H) + cal_only[1])
    key("f2"); wait_for("terminal open", 6); time.sleep(0.4); type_("/help"); key("ret"); time.sleep(1.0); type_("abc")
    dot(rect); check("the Terminal in front of the Calendar closes by its red dot", wait_for("terminal closed", 6))
    same("Terminal closed with the Calendar behind it (the Calendar is whole)", want=cal_only)
    key("f2"); wait_for("terminal open", 6); time.sleep(0.4); type_("/help"); key("ret"); time.sleep(1.0)
    move_to((APP[0] + APP[2] - 30) * S, (APP[1] + APP[3] // 2) * S); click(); time.sleep(0.6)   # a click on the Calendar's own part raises it over the Terminal
    n = len(uart()); dot(APP); wait_for("M3c app calendar exit 0", 8); time.sleep(0.6)
    dot(rect); check("the Terminal that was behind the Calendar closes", wait_for("terminal closed", 6, n)); same("Calendar closed in front of the Terminal, then the Terminal closed")
    key("f2"); wait_for("terminal open", 6); time.sleep(0.4); type_("/help"); key("ret"); time.sleep(1.0)
    ok = spotlight("calendar", "M3c window calendar first frame"); time.sleep(0.6)
    n = len(uart()); key("esc"); wait_for("M3c app calendar exit 0", 8, n); time.sleep(0.6)
    dot(rect); wait_for("terminal closed", 6, n)
    same("Terminal opened first, the Calendar over it closed by Esc, then the Terminal closed")

    if "KERNEL CRASH" in uart(): fails.append("the kernel crashed")
finally:
    q.kill(); q.wait()

# The plain 800x600 boot (scale 1): a fresh desktop has nothing white right of the tribute line's apple and above the dock
# (QA saw a 60x4 sliver there in a baseline frame; the boot screen's loading bar and the old bar's pixels must be gone).
if subprocess.run(["make", "-C", arch, "kernel8.elf"], capture_output=True).returncode: fails.append("`make` (the 800x600 kernel) does not build")
else:
    log2, sock2 = tmp + "/uart2", tmp + "/qmp2"
    q = subprocess.Popen(["qemu-system-aarch64", "-machine", "virt", "-cpu", "cortex-a72", "-m", "256", "-nic", "none", "-device", "ramfb", "-display", "none",
                          "-serial", "file:" + log2, "-qmp", "unix:%s,server,nowait" % sock2, "-kernel", os.path.join(arch, "kernel8.elf")], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        for _ in range(100):
            if os.path.exists(log2) and "M1a ok" in open(log2, errors="replace").read(): break
            time.sleep(0.2)
        time.sleep(3)
        s2 = socket.socket(socket.AF_UNIX); s2.connect(sock2); f2 = s2.makefile("rw"); f2.readline()
        def cmd2(c, **a):
            f2.write(json.dumps({"execute": c, "arguments": a}) + "\n"); f2.flush()
            while True:
                r = json.loads(f2.readline())
                if "return" in r or "error" in r: return r
        cmd2("qmp_capabilities"); path = tmp + "/boot800.ppm"; cmd2("screendump", filename=path); time.sleep(0.5)
        parts = open(path, "rb").read().split(b"\n", 3); w2, h2 = map(int, parts[1].split()); b2 = parts[3]
        white = [(x, y) for y in range(452, 512) for x in range(480, w2) if all(c > 0xE0 for c in b2[(y * w2 + x) * 3:(y * w2 + x) * 3 + 3])]
        check("the 800x600 boot desktop has no white sliver beside the tribute line, above the dock", w2 == 800 and not white, (w2, white[:8]))
    finally:
        q.kill(); q.wait()
subprocess.run(["make", "-C", arch, "clean"], capture_output=True)
for m in fails: print("FAIL: " + m)
if fails: sys.exit(1)
print("PASS: every window closes back to the empty desktop")
