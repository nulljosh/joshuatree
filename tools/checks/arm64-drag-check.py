#!/usr/bin/env python3
"""ARM64 desktop window drag: the Console, the Terminal and the Browser follow the pointer by their title band.

QEMU's virt machine with ramfb, a USB keyboard and a USB boot mouse behind a hub (the arm64 mouse check's setup). Through
QMP input-send-event it:
  - presses in the Console's white well and moves: nothing drags, no "window at" line, the frame stays;
  - presses on the title band (between the dots), moves 100 right and 15 down and releases: "window at 250,115", the red,
    yellow and grey dots and the title are at the new place, the rectangle it left is flat wallpaper, the window's pixels
    (log text, white well) are the ones it had before, and the dock band and the line over it are untouched;
  - drags far right and down, then far left and up: the window clamps whole on screen, below the menu bar and above the dock;
  - clicks the red dot at the new place: "console closed", and the wallpaper is flat where the window was;
  - opens the Browser (Spotlight) where the Console had been put, drags it, checks the same, presses in its page well
    (nothing drags) and closes it with Esc: the Console returns at the new place.
Then the dragtest build on QEMU's Raspberry Pi 4B model at 1080p (window_scale 2): the boot-time drag lands on the clamped
spot, the old area is flat and the dots moved.
Skips (exit 0) when clang's aarch64 target, ld.lld or qemu-system-aarch64 is missing.
Usage: tools/checks/arm64-drag-check.py   (from the repo root)
"""
import json, os, re, shutil, socket, subprocess, sys, tempfile, time

root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
arch = os.path.join(root, "arch/arm64")
if not all(shutil.which(t) for t in ("clang", "ld.lld", "qemu-system-aarch64")):
    print("SKIP: clang, ld.lld or qemu-system-aarch64 not installed"); sys.exit(0)
subprocess.run(["make", "-C", arch, "clean"], capture_output=True)
if subprocess.run(["make", "-C", arch], capture_output=True).returncode:
    print("FAIL: arch/arm64 `make` does not build"); sys.exit(1)
fails = []
def check(name, ok, detail=""):
    print(("  ok: " if ok else "  FAIL: ") + name + ("" if ok else "  " + repr(detail)[:500]))
    if not ok: fails.append(name)

WALL, RED, YELLOW, GREY, WHITE = (0xB5, 0x50, 0x2C), (0xFF, 0x5F, 0x57), (0xFF, 0xD6, 0x4A), (0xD8, 0xD4, 0xCE), (255, 255, 255)

def geom(w, h):   # main.c's numbers: scale, the window's logical rectangle, the lowest and highest places it may sit
    s = 2 if h >= 1080 else 1
    sc = lambda v: v * h // 600
    lw, lh = sc(500) // s, sc(350) // s
    lx, ly = (w - sc(500)) // 2 // s, sc(100) // s
    lgh = h // s
    icon = max(16, min(lgh * 7 // 100, (min(740, w // s - 40) - 2 * 10 - 11 * 6) // 12))
    band_top = lgh - icon - 2 * 10 - 24 - 24
    top = (26 * h // 540) // s + 8
    return s, lx, ly, lw, lh, top, band_top - 22 - lh, w // s - lw, band_top

def screendump(cmd, tmp):
    shot = tmp + "/shot%d.ppm" % time.monotonic_ns()
    if "error" in cmd("screendump", filename=shot): return None
    time.sleep(0.3)
    parts = open(shot, "rb").read().split(b"\n", 3); w, h = map(int, parts[1].split())
    return w, h, parts[3]
def px(img, x, y):
    w, h, b = img; i = (y * w + x) * 3
    return tuple(b[i:i + 3])
def rows(img, x, y, w, h):   # the pixels of a rectangle, one bytes per row
    iw, _, b = img
    return [b[((y + j) * iw + x) * 3:((y + j) * iw + x + w) * 3] for j in range(h)]
def flat(img, x, y, w, h, colour, skip=()):   # every pixel of the rectangle is `colour`, except inside the skipped boxes
    bad = 0
    for j, r in enumerate(rows(img, x, y, w, h)):
        for i in range(w):
            if tuple(r[i * 3:i * 3 + 3]) != colour and not any(bx <= x + i < bx + bw and by <= y + j < by + bh for bx, by, bw, bh in skip): bad += 1
    return bad
def left_over(img, old, new, skip):   # pixels of the old rectangle outside the new one that are not wallpaper
    bad = 0
    ox, oy, w, h = old; nx, ny = new[0], new[1]
    for j, r in enumerate(rows(img, ox, oy, w, h)):
        for i in range(w):
            x, y = ox + i, oy + j
            if nx <= x < nx + w and ny <= y < ny + h: continue
            if tuple(r[i * 3:i * 3 + 3]) != WALL and not any(bx <= x < bx + bw and by <= y < by + bh for bx, by, bw, bh in skip): bad += 1
    return bad
def dots(img, wx, wy, s):
    return [px(img, (wx + d - 5) * s, (wy + 16) * s) for d in (24, 46, 68)]   # left of each glyph, which sits in the dot's middle

tmp = tempfile.mkdtemp()
log, sock = tmp + "/uart", tmp + "/qmp"
q = subprocess.Popen(["qemu-system-aarch64", "-machine", "virt", "-cpu", "cortex-a72", "-m", "256", "-nic", "none", "-device", "ramfb",
                      "-device", "pcie-root-port,id=rp,chassis=1", "-device", "qemu-xhci,id=x,bus=rp",
                      "-device", "usb-hub,id=h,bus=x.0,port=1",
                      "-device", "usb-kbd,bus=x.0,port=1.1", "-device", "usb-mouse,id=m1,bus=x.0,port=1.2",
                      "-display", "none", "-serial", "file:" + log, "-qmp", "unix:%s,server,nowait" % sock,
                      "-kernel", os.path.join(arch, "kernel8.elf")], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
def uart(): return open(log, errors="replace").read() if os.path.exists(log) else ""
def wait_for(text, tries=100, count=1):
    for _ in range(tries):
        if uart().count(text) >= count: return True
        time.sleep(0.1)
    return False
try:
    if not wait_for("usb ready: 1 kbd, 1 mouse", 300):
        fails.append(f"the keyboard and mouse behind the hub did not come up, got {uart()[-800:]!r}")
    else:
        s_ = socket.socket(socket.AF_UNIX); s_.connect(sock); f = s_.makefile("rw")
        f.readline()
        def cmd(c, **a):
            f.write(json.dumps({"execute": c, "arguments": a}) + "\n"); f.flush()
            while True:
                r = json.loads(f.readline())
                if "return" in r or "error" in r: return r
        cmd("qmp_capabilities")
        time.sleep(0.5)
        base = screendump(cmd, tmp)
        W, H = base[0], base[1]
        s, wx, wy, lw, lh, ytop, ybot, xright, band_top = geom(W, H)
        pos = [W // 2, H // 2]
        def move_to(x, y):
            while (pos[0], pos[1]) != (x, y):
                dx = max(-100, min(100, x - pos[0])); dy = max(-100, min(100, y - pos[1]))
                cmd("input-send-event", events=[{"type": "rel", "data": {"axis": "x", "value": dx}}, {"type": "rel", "data": {"axis": "y", "value": dy}}])
                pos[0] += dx; pos[1] += dy
                if not wait_for(f"mouse {pos[0]},{pos[1]}\n", 40): return False
            return True
        def button(down):
            n = uart().count("key 272 " + ("down" if down else "up"))
            cmd("input-send-event", events=[{"type": "btn", "data": {"down": down, "button": "left"}}])
            return wait_for("key 272 " + ("down" if down else "up"), 30, n + 1)
        def click(): return button(True) and button(False)
        def key(name):
            cmd("send-key", keys=[{"type": "qcode", "data": name}], **{"hold-time": 60}); time.sleep(0.25)
        def drag(grab, target):   # press at physical `grab`, move to `target`, release; the UART line says where the window ended
            n = uart().count("window at ")
            if not (move_to(*grab) and button(True) and move_to(*target) and button(False) and wait_for("window at ", 30, n + 1)):
                return None
            m = re.findall(r"window at (\d+),(\d+)\n", uart())[-1]
            time.sleep(0.2)
            return int(m[0]), int(m[1])
        arrow = lambda gx, gy: (gx, gy, 13 * s, 19 * s)   # the arrow's box, its tip at the pointer

        # 1. a press in the white well is not a drag
        move_to((wx + 250) * s, (wy + 200) * s)
        n = uart().count("window at ")
        ok = button(True) and move_to((wx + 310) * s, (wy + 230) * s) and button(False)
        time.sleep(0.2)
        img = screendump(cmd, tmp)
        check("a press in the Console's well and a move drag nothing", ok and uart().count("window at ") == n and dots(img, wx, wy, s) == [RED, YELLOW, GREY], uart()[-200:])
        # 2. a drag by the title band: 100 right, 15 down
        grab = ((wx + 150) * s, (wy + 12) * s)
        move_to(*grab)
        before = screendump(cmd, tmp)
        old = (wx * s, wy * s, lw * s, lh * s)
        ink = sum(1 for y in range(old[1] + 40 * s, old[1] + old[3] - 10 * s, 3) for x in range(old[0] + 12 * s, old[0] + old[2] - 12 * s, 3) if max(px(before, x, y)) < 0x90)
        check("the Console shows its log before the drag", ink > 100, ink)
        nx, ny = wx + 100, min(wy + 15, ybot)
        got = drag(grab, (grab[0] + 100 * s, grab[1] + (ny - wy) * s))
        check("releasing prints where the window is", got == (nx, ny), (got, uart()[-200:]))
        after = screendump(cmd, tmp)
        skip = [arrow(*grab), arrow(grab[0] + 100 * s, grab[1] + (ny - wy) * s)]
        check("the three dots are at the new place", got is not None and dots(after, nx, ny, s) == [RED, YELLOW, GREY], dots(after, nx, ny, s))
        check("the rectangle it left is flat wallpaper", left_over(after, old, (nx * s, ny * s), skip) == 0, left_over(after, old, (nx * s, ny * s), skip))
        moved = rows(after, nx * s, ny * s, lw * s, lh * s); was = rows(before, wx * s, wy * s, lw * s, lh * s)
        diff = sum(1 for j in range(lh * s) for i in range(lw * s)
                   if moved[j][i * 3:i * 3 + 3] != was[j][i * 3:i * 3 + 3] and not (grab[0] - wx * s <= i < grab[0] - wx * s + 13 * s and grab[1] - wy * s <= j < grab[1] - wy * s + 19 * s))
        check("the window's pixels (title, log, well) came along unchanged", diff == 0, diff)
        band = (band_top - 2) * s
        check("the dock and the line over it are untouched", rows(after, 0, band, W, H - band) == rows(before, 0, band, W, H - band))
        # 3. the clamps: far right and down, then far left and up
        grab = ((nx + 150) * s, (ny + 12) * s)
        got = drag(grab, (grab[0] + 200 * s, grab[1] + 200 * s))
        check("dragged past the right edge and the dock the window stops at the corner", got == (xright, ybot), (got, xright, ybot))
        img = screendump(cmd, tmp)
        check("the clamped window is whole on screen above the dock band", got is not None and dots(img, got[0], got[1], s) == [RED, YELLOW, GREY] and (ybot + lh) * s <= band_top * s)
        nx, ny = got if got else (nx, ny)
        grab = ((nx + 150) * s, (ny + 12) * s)
        got = drag(grab, (grab[0] - 420 * s, grab[1] - 120 * s))
        check("dragged past the left edge and up the window stops under the menu bar", got == (0, ytop), (got, ytop))
        img = screendump(cmd, tmp)
        nx, ny = got if got else (nx, ny)
        # 4. the red dot at the new place closes it
        move_to((nx + 24) * s, (ny + 16) * s)
        click()
        closed = wait_for("console closed", 30)
        img = screendump(cmd, tmp)
        check("the red dot at the new place closes the Console", closed and flat(img, nx * s, ny * s, lw * s, lh * s, WALL, [arrow(pos[0], pos[1])]) == 0, uart()[-200:])
        # 5. the Browser opens where the window was put, and drags the same way
        key("f1"); [key(c) for c in "browser"]; key("ret")
        check("Spotlight opens the Browser", wait_for("browser open", 30), uart()[-300:])
        time.sleep(0.3)
        img = screendump(cmd, tmp)
        check("the Browser opens at the dragged place", dots(img, nx, ny, s) == [RED, YELLOW, GREY], dots(img, nx, ny, s))
        grab = ((nx + 300) * s, (ny + 12) * s)
        move_to(*grab)
        before = screendump(cmd, tmp)
        old = (nx * s, ny * s, lw * s, lh * s)
        tx, ty = nx + 100, min(ny + 40, ybot)
        got = drag(grab, (grab[0] + 100 * s, grab[1] + (ty - ny) * s))
        after = screendump(cmd, tmp)
        check("the Browser follows its title band", got == (tx, ty) and dots(after, tx, ty, s) == [RED, YELLOW, GREY], (got, dots(after, tx, ty, s)))
        skip = [arrow(*grab), arrow(grab[0] + 100 * s, grab[1] + (ty - ny) * s)]
        check("the rectangle it left is flat wallpaper", left_over(after, old, (tx * s, ty * s), skip) == 0, left_over(after, old, (tx * s, ty * s), skip))
        moved = rows(after, tx * s, ty * s, lw * s, lh * s); was = rows(before, nx * s, ny * s, lw * s, lh * s)
        ox, oy = grab[0] - nx * s, grab[1] - ny * s
        diff = sum(1 for j in range(lh * s) for i in range(lw * s)
                   if moved[j][i * 3:i * 3 + 3] != was[j][i * 3:i * 3 + 3] and not (ox <= i < ox + 13 * s and oy <= j < oy + 19 * s))
        check("the Browser's address bar and page well came along unchanged", diff == 0, diff)
        nx, ny = got if got else (nx, ny)
        n = uart().count("window at ")
        move_to((nx + 250) * s, (ny + 200) * s)
        ok = button(True) and move_to((nx + 200) * s, (ny + 180) * s) and button(False)
        time.sleep(0.2)
        img = screendump(cmd, tmp)
        check("a press in the Browser's page well drags nothing", ok and uart().count("window at ") == n and dots(img, nx, ny, s) == [RED, YELLOW, GREY], uart()[-200:])
        move_to(W - 40, H // 2)
        key("esc"); key("esc")   # the first leaves the address bar, the second closes the window
        check("Esc closes the Browser", wait_for("browser closed", 30), uart()[-200:])
        time.sleep(0.3)
        img = screendump(cmd, tmp)
        check("the Console comes back at the dragged place", dots(img, nx, ny, s) == [RED, YELLOW, GREY], dots(img, nx, ny, s))
        ink = sum(1 for y in range((ny + 34) * s, (ny + lh - 8) * s, 2) for x in range((nx + 8) * s, (nx + lw - 8) * s, 2) if max(px(img, x, y)) < 0x90)
        check("its log is drawn inside the new well (the text origin followed)", ink > 100, ink)
        if "KERNEL CRASH" in uart(): fails.append("the kernel crashed")
finally:
    q.kill(); q.wait()

# The Pi 4B model has no USB in QEMU: the dragtest build drags the Console at boot, 100 right and 60 down, at 1080p (scale 2).
if "raspi4b" in subprocess.run(["qemu-system-aarch64", "-machine", "help"], capture_output=True, text=True).stdout:
    if subprocess.run(["make", "-C", arch, "dragtest"], capture_output=True).returncode: fails.append("`make dragtest` does not build")
    else:
        log2, sock2 = tmp + "/uart2", tmp + "/qmp2"
        q = subprocess.Popen(["qemu-system-aarch64", "-machine", "raspi4b", "-serial", "null", "-global", "bcm2835-fb.xres=1920", "-global", "bcm2835-fb.yres=1080",
                              "-display", "none", "-serial", "file:" + log2, "-qmp", "unix:%s,server,nowait" % sock2,
                              "-kernel", os.path.join(arch, "drag-kernel8.img")], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        try:
            out = ""
            for _ in range(200):
                time.sleep(0.1)
                out = open(log2, errors="replace").read() if os.path.exists(log2) else ""
                if "window at " in out: break
            s, wx, wy, lw, lh, ytop, ybot, xright, band_top = geom(1920, 1080)
            nx, ny = wx + 100, min(wy + 60, ybot)
            if f"window at {nx},{ny}\n" not in out: fails.append(f"pi 1080p dragtest: no 'window at {nx},{ny}', got {out[-300:]!r}")
            else:
                s_ = socket.socket(socket.AF_UNIX); s_.connect(sock2); f = s_.makefile("rw"); f.readline()
                def cmd(c, **a):
                    f.write(json.dumps({"execute": c, "arguments": a}) + "\n"); f.flush()
                    while True:
                        r = json.loads(f.readline())
                        if "return" in r or "error" in r: return r
                cmd("qmp_capabilities")
                img = screendump(cmd, tmp)
                old = (wx * s, wy * s, lw * s, lh * s)
                check("pi 1080p: the dots are at the clamped place", dots(img, nx, ny, s) == [RED, YELLOW, GREY], dots(img, nx, ny, s))
                check("pi 1080p: the old area is flat wallpaper", left_over(img, old, (nx * s, ny * s), []) == 0, left_over(img, old, (nx * s, ny * s), []))
                well = [px(img, x, y) for y in range((ny + 40) * s, (ny + lh - 12) * s, 7) for x in range((nx + 12) * s, (nx + lw - 12) * s, 7)]
                check("pi 1080p: the white well came along", sum(1 for c in well if c == WHITE) * 2 > len(well), len(well))
        finally:
            q.kill(); q.wait()
else:
    print("  QEMU here has no raspi4b model (needs QEMU 9 or newer), the 1080p drag skipped")
shutil.rmtree(tmp, ignore_errors=True)
subprocess.run(["make", "-C", arch, "clean"], capture_output=True)
for m in fails: print("FAIL: " + m)
if fails: sys.exit(1)
print("PASS: the Console and the Browser follow their title band, clamp on screen, leave flat wallpaper and the dock behind, keep their contents, and the red dot and the page well behave")
