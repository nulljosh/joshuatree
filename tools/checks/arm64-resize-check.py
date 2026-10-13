#!/usr/bin/env python3
"""ARM64 desktop window drag and resize, every window: the Console, the Browser, the Clock, Brick, the Calculator and the
ring-3 apps Calendar, Notes, Music and Books.

QEMU's virt machine with ramfb, a USB keyboard and a USB boot mouse behind a hub (the drag check's setup), 800x600. Each window
is opened (Spotlight) and put through the same script, read back from real screendumps and the kernel's UART lines:
  - drag by the title band to a new place: "window at X,Y" is it, the three dots are there, the rectangle it left is flat
    wallpaper, and a drag past the menu bar stops under it;
  - drag the bottom-right corner (12 px zone) out: "window size WxH" grew by what the pointer moved, the frame's right and
    bottom edges are at the new place, and the content was laid out again for the new size (the Console shows more lines, the
    Browser's mascot, the Clock and Brick re-centre, the Calculator re-spaces its keys; a ring-3 app is sent JT_EV_RESIZE,
    re-opens its window, "ring3: window now" and draws a frame into the new buffer, "redrawn");
  - drag the corner far inside: the size stops at the window's minimum, and the same press on the right edge alone and the
    bottom edge alone changes one side only;
  - drag it far out: it stops at the right of the screen and at the dock's band, which is untouched;
  - back to its first size, then the red dot closes it and leaves flat wallpaper.
Last, the Calendar over the Console: dragging it away draws the Console behind whole again where it was covered.
Skips (exit 0) when clang's aarch64 target, ld.lld or qemu-system-aarch64 is missing.
Usage: tools/checks/arm64-resize-check.py   (from the repo root)
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
    print(("  ok: " if ok else "  FAIL: ") + name + ("" if ok else "  " + repr(detail)[:600]))
    if not ok: fails.append(name)

WALL, RED, YELLOW, GREY = (0x6E, 0x6E, 0x6E), (0xFF, 0x5F, 0x57), (0xFF, 0xD6, 0x4A), (0xD8, 0xD4, 0xCE)
ZONE = 12

def screendump(cmd, tmp):
    shot = tmp + "/shot%d.ppm" % time.monotonic_ns()
    if "error" in cmd("screendump", filename=shot): return None
    time.sleep(0.3)
    parts = open(shot, "rb").read().split(b"\n", 3); w, h = map(int, parts[1].split())
    os.unlink(shot)
    return w, h, parts[3]
def px(img, x, y):
    w, h, b = img; i = (y * w + x) * 3
    return tuple(b[i:i + 3])
def arrow(x, y): return (x, y, 13, 19)
def inside(box, x, y): return box[0] <= x < box[0] + box[2] and box[1] <= y < box[1] + box[3]
def dots(img, x, y): return [px(img, x + d - 5, y + 16) for d in (24, 46, 68)]
def not_flat_outside(img, old, new, skip):   # pixels of the old rectangle outside the new one that are not wallpaper
    bad = 0
    for j in range(old[3]):
        for i in range(old[2]):
            x, y = old[0] + i, old[1] + j
            if inside(new, x, y) or any(inside(b, x, y) for b in skip): continue
            if px(img, x, y) != WALL: bad += 1
    return bad
def centroid(img, box, pred, skip):   # the mean place of the pixels in `box` that satisfy `pred`, and how many
    sx = sy = n = 0
    for y in range(box[1], box[1] + box[3], 2):
        for x in range(box[0], box[0] + box[2], 2):
            if any(inside(b, x, y) for b in skip) or not pred(px(img, x, y)): continue
            sx += x; sy += y; n += 1
    return (sx / n, sy / n, n) if n else (0, 0, 0)
dark = lambda p: max(p) < 0x70
terra = lambda p: p[0] > 140 and p[1] < 120 and p[2] < 100 and p[0] - p[1] > 60

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
        W, H = screendump(cmd, tmp)[:2]
        S = 2 if H >= 1080 else 1
        lw_, lh_ = W // S, H // S
        icon = max(16, min(lh_ * 7 // 100, (min(740, lw_ - 40) - 2 * 10 - 10 * 6) // 11))
        BAND = lh_ - icon - 2 * 10 - 24 - 24
        TOP, BOTTOM, SW = (26 * H // 540) // S + 8, BAND - 22, lw_
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
        def drag(grab, target, via=None):
            """press at logical `grab`, move (through `via`) to `target`, release: the UART lines say where the window ended and its size."""
            n, ns = uart().count("window at "), uart().count("window size ")
            if not (move_to(grab[0] * S, grab[1] * S) and button(True) and (via is None or move_to(via[0] * S, via[1] * S)) and move_to(target[0] * S, target[1] * S) and button(False) and wait_for("window at ", 30, n + 1)):
                return None, None
            time.sleep(0.25)
            out = uart()
            at = tuple(map(int, re.findall(r"window at (\d+),(\d+)\n", out)[-1]))
            sizes = re.findall(r"window size (\d+)x(\d+)\n", out)
            return at, (tuple(map(int, sizes[-1])) if len(sizes) > ns else None)

        class Win:
            def __init__(self, name, rect, floor, opener, closer, relayout, wait, app=False):
                self.name, self.rect, self.floor, self.opener, self.closer, self.relayout, self.wait, self.app = name, list(rect), floor, opener, closer, relayout, wait, app
                self.first = tuple(rect[2:])
        def wait_re(pattern, count, tries=60):
            for _ in range(tries):
                if len(re.findall(pattern, uart())) >= count: return True
                time.sleep(0.1)
            return False
        def spot(word):
            key("f1")
            for c in word: key(c)
            key("ret")
        def grab_title(r): return (r[0] + r[2] // 2, r[1] + 12)
        def corner(r): return (r[0] + r[2] - 6, r[1] + r[3] - 6)

        def exercise(w, open_marker):
            n = w.name
            w.opener()
            if not wait_for(open_marker, 150): check(f"{n}: opens", False, uart()[-400:]); return
            time.sleep(0.4)
            if w.app:
                m = re.findall(r"M3c window %s (\d+)x(\d+) at (\d+),(\d+)\n" % n.lower(), uart())[-1]
                w.rect[:] = [int(m[2]) - 8, int(m[3]) - 32, int(m[0]) + 16, int(m[1]) + 40]; w.first = tuple(w.rect[2:])
                wait_for(f"M3c window {n.lower()} first frame", 100)
            r = w.rect
            mnw, mnh = w.floor
            mnw, mnh = min(mnw, w.first[0]), min(mnh, w.first[1])
            if n == "Brick": mnw, mnh = w.first
            img0 = screendump(cmd, tmp)
            check(f"{n}: opens with its three dots", dots(img0, r[0] * S, r[1] * S) == [RED, YELLOW, GREY], dots(img0, r[0] * S, r[1] * S))
            # 1. a drag by the title band to the top left (or a step off it when it is there): moved, dots, wallpaper left behind
            tx, ty = (24, TOP - 20) if (r[0], r[1]) != (24, TOP) else (84, TOP + 24)
            old = (r[0] * S, r[1] * S, r[2] * S, r[3] * S)
            g = grab_title(r)
            at, size = drag(g, (g[0] + tx - r[0], g[1] + ty - r[1]))
            want = (tx, max(ty, TOP))
            check(f"{n}: a title-band drag ends at {want}", at == want and size is None, (at, size, uart()[-200:]))
            if at is None: return
            r[0], r[1] = at
            img = screendump(cmd, tmp)
            check(f"{n}: its dots are at the new place", dots(img, r[0] * S, r[1] * S) == [RED, YELLOW, GREY], dots(img, r[0] * S, r[1] * S))
            skip = [arrow(pos[0], pos[1]), arrow(g[0] * S, g[1] * S)]
            check(f"{n}: the rectangle it left is flat wallpaper", not_flat_outside(img, old, (r[0] * S, r[1] * S, r[2] * S, r[3] * S), skip) == 0,
                  not_flat_outside(img, old, (r[0] * S, r[1] * S, r[2] * S, r[3] * S), skip))
            # 2. the corner, out
            before = screendump(cmd, tmp)
            dw, dh = min(70, SW - (r[0] + r[2])), min(40, BOTTOM - (r[1] + r[3]))
            c = corner(r)
            if w.app: pre = uart().count("ring3: window now ")
            at, size = drag(c, (c[0] + dw, c[1] + dh))
            check(f"{n}: the corner drag grows it by what the pointer moved", at == (r[0], r[1]) and size == (r[2] + dw, r[3] + dh), (at, size, (r[2] + dw, r[3] + dh)))
            if size is None: return
            r0 = tuple(r)
            r[2], r[3] = size
            time.sleep(0.4)
            after = screendump(cmd, tmp)
            ar = arrow(pos[0] * S, pos[1] * S)
            check(f"{n}: the dots stay and the right and bottom edges are at the new place",
                  dots(after, r[0] * S, r[1] * S) == [RED, YELLOW, GREY] and px(after, (r[0] + r[2] - 4) * S, (r[1] + r[3] // 2) * S) != WALL and px(after, (r[0] + r[2] + 3) * S, (r[1] + r[3] // 2) * S) == WALL
                  and px(after, (r[0] + r[2] // 2) * S, (r[1] + r[3] - 4) * S) != WALL and px(after, (r[0] + r[2] // 2) * S, (r[1] + r[3] + 3) * S) == WALL,
                  (dots(after, r[0] * S, r[1] * S), px(after, (r[0] + r[2] - 4) * S, (r[1] + r[3] // 2) * S), px(after, (r[0] + r[2] + 3) * S, (r[1] + r[3] // 2) * S)))
            ok, detail = w.relayout(before, after, r0, tuple(r), dw, dh, ar)
            if os.environ.get("RESIZE_DEBUG"):
                for tag, im in (("b", before), ("a", after)): open(os.path.join(os.environ["RESIZE_DEBUG"], f"{n}-{tag}.ppm"), "wb").write(b"P6\n%d %d\n255\n" % (im[0], im[1]) + im[2])
            check(f"{n}: the content was laid out again for the new size", ok, detail)
            if w.app:
                cw, ch = r[2] - 16, r[3] - 40
                check(f"{n}: the app got JT_EV_RESIZE, re-opened its window at {cw}x{ch} and drew a frame in it",
                      wait_for(f"M3c event resize ", 20) and wait_for(f"M3c window {n.lower()} {cw}x{ch} resized", 40) and wait_for(f"ring3: window now {cw}x{ch}", 40)
                      and wait_for(f"M3c window {n.lower()} redrawn {cw}x{ch}", 40) and uart().count("ring3: window now ") > pre, uart()[-500:])
            # 3. the minimum, by the corner; then the right edge alone, and the bottom edge alone
            c = corner(r)
            at, size = drag(c, (r[0] + 4, r[1] + 4))
            check(f"{n}: dragged far inside it stops at its minimum {mnw}x{mnh}", at == (r[0], r[1]) and size == (mnw, mnh), (at, size))
            if size is None: return
            r[2], r[3] = size
            at, size = drag((r[0] + r[2] - 5, r[1] + r[3] // 2), (r[0] + r[2] - 5 + 30, r[1] + r[3] // 2 + 20))
            check(f"{n}: the right edge alone changes the width only", size == (r[2] + 30, r[3]), (at, size))
            if size is None: return
            r[2], r[3] = size
            at, size = drag((r[0] + r[2] // 2, r[1] + r[3] - 5), (r[0] + r[2] // 2 + 20, r[1] + r[3] - 5 + 20))
            check(f"{n}: the bottom edge alone changes the height only", size == (r[2], r[3] + 20), (at, size))
            if size is None: return
            r[2], r[3] = size
            right0 = r[0] + r[2]
            at, size = drag((r[0] + 5, r[1] + r[3] // 2), (r[0] + 5 + 30, r[1] + r[3] // 2 + 15))
            check(f"{n}: the left edge moves in, the right edge stays", at == (r[0] + 30, r[1]) and size == (r[2] - 30, r[3]) and at[0] + size[0] == right0, (at, size, right0))
            if size is None: return
            r[0], r[2] = at[0], size[0]
            at, size = drag((r[0] + 5, r[1] + r[3] // 2), (r[0] + 5 - 30, r[1] + r[3] // 2))
            check(f"{n}: the left edge back out puts the window where it was", at == (r[0] - 30, r[1]) and size == (r[2] + 30, r[3]) and at[0] + size[0] == right0, (at, size, right0))
            if size is None: return
            r[0], r[2] = at[0], size[0]
            # 4. far out: the screen's right edge and the dock's band
            band0 = screendump(cmd, tmp)
            c = corner(r)
            at, size = drag(c, (W // S - 2, H // S - 2), via=(W // S - 2, c[1]))   # down the screen's right edge: a trip over a dock tile leaves the hover label's top two rows behind (not this change's)
            check(f"{n}: dragged far out it stops at the screen's right edge and above the dock", at == (r[0], r[1]) and size == (SW - r[0], BOTTOM - r[1]), (at, size, (SW - r[0], BOTTOM - r[1])))
            if size is None: return
            r[2], r[3] = size
            time.sleep(0.4)
            img = screendump(cmd, tmp)
            ar = arrow(pos[0], pos[1])
            rows_same = all(px(img, x, y) == px(band0, x, y) for y in list(range((BAND - 18) * S, (BAND - 3) * S, 2)) + list(range((BAND + 24) * S, H, 3)) for x in range(0, W, 5) if not inside(ar, x, y))
            if not rows_same and os.environ.get("RESIZE_DEBUG"):
                for tag, im in (("db", band0), ("da", img)): open(os.path.join(os.environ["RESIZE_DEBUG"], f"{n}-{tag}.ppm"), "wb").write(b"P6\n%d %d\n255\n" % (im[0], im[1]) + im[2])
            check(f"{n}: the dock and the line over it are untouched", rows_same)
            # 5. back to its first size, then the red dot
            c = corner(r)
            at, size = drag(c, (c[0] + w.first[0] - r[2], c[1] + w.first[1] - r[3]))
            check(f"{n}: back to its first size {w.first[0]}x{w.first[1]}", size == w.first, (at, size))
            if size is not None: r[2], r[3] = size
            move_to(*[v * S for v in (r[0] + 24, r[1] + 16)])
            click()
            check(f"{n}: the red dot closes it", w.closer(), uart()[-300:])
            time.sleep(0.4)

        # --- relayout checks: (before, after, rect before, rect after, dw, dh, the arrow's box after) -> (ok, detail)
        def r_console(b, a, r0, r1, dw, dh, ar):   # the white well grew to the new size and the title bar's "lines N of M" moved to the new right edge
            white = lambda p: p == (255, 255, 255)
            strip_b = centroid(a, ((r0[0] + 12) * S, (r0[1] + r0[3] - 6) * S, (r1[2] - 24) * S, (dh - 4) * S), white, [ar])
            strip_r = centroid(a, ((r0[0] + r0[2] - 6) * S, (r1[1] + 36) * S, (dw - 4) * S, (r1[3] - 50) * S), white, [ar])
            title = centroid(a, ((r1[0] + r1[2] - 130) * S, (r1[1] + 8) * S, 120 * S, 18 * S), lambda p: max(p) < 0xB0, [ar])
            full = lambda c, w, h: c[2] * 4 >= w * h * S * S * 0.85 / 1   # centroid counts every other pixel in each direction: a quarter of the area
            return (full(strip_b, r1[2] - 24, dh - 4) and full(strip_r, dw - 4, r1[3] - 50) and title[2] > 15), (strip_b[2], strip_r[2], title[2])
        def r_terminal(b, a, r0, r1, dw, dh, ar):   # the ask> row follows the bottom of the taller well: ink there now, none where the row was
            now = centroid(a, ((r1[0] + 12) * S, (r1[1] + r1[3] - 28) * S, 120 * S, 18 * S), dark, [ar])
            was = centroid(a, ((r0[0] + 12) * S, (r0[1] + r0[3] - 28) * S, 120 * S, 18 * S), dark, [ar])
            return now[2] > 10 and was[2] == 0, (now[2], was[2])
        def r_browser(b, a, r0, r1, dw, dh, ar):   # the empty page's mascot stays in the middle of a bigger well
            sk = [ar, arrow(pos[0], pos[1])]
            f0 = centroid(b, ((r0[0] + 10) * S, (r0[1] + 66) * S, (r0[2] - 20) * S, (r0[3] - 96) * S), terra, sk)
            f1 = centroid(a, ((r1[0] + 10) * S, (r1[1] + 66) * S, (r1[2] - 20) * S, (r1[3] - 96) * S), terra, sk)
            return f0[2] > 20 and f1[2] > 20 and abs((f1[0] - f0[0]) - dw * S / 2) <= 4 * S and abs((f1[1] - f0[1]) - dh * S / 2) <= 4 * S, (f0, f1)
        def r_centred(pred):   # Clock (its ink) and Brick (its bricks): the content moves by half of what the window grew
            def check_(b, a, r0, r1, dw, dh, ar):
                sk = [ar]
                f0 = centroid(b, ((r0[0] + 10) * S, (r0[1] + 40) * S, (r0[2] - 20) * S, (r0[3] - 60) * S), pred, sk)
                f1 = centroid(a, ((r1[0] + 10) * S, (r1[1] + 40) * S, (r1[2] - 20) * S, (r1[3] - 60) * S), pred, sk)
                return f0[2] > 20 and f1[2] > 20 and abs((f1[0] - f0[0]) - dw * S / 2) <= 8 * S and abs((f1[1] - f0[1]) - dh * S / 2) <= 6 * S, (f0, f1)
            return check_
        def r_calc(b, a, r0, r1, dw, dh, ar):   # the keys spread over the bigger window: their labels' mean place moves out
            sk = [ar]
            f0 = centroid(b, ((r0[0] + 10) * S, (r0[1] + 90) * S, (r0[2] - 20) * S, (r0[3] - 100) * S), dark, sk)
            f1 = centroid(a, ((r1[0] + 10) * S, (r1[1] + 90) * S, (r1[2] - 20) * S, (r1[3] - 100) * S), dark, sk)
            return f0[2] > 20 and f1[2] > 20 and (f1[0] - f0[0]) >= 0.3 * dw * S and (f1[1] - f0[1]) >= 0.3 * dh * S, (f0, f1)
        def r_app(b, a, r0, r1, dw, dh, ar):   # drawn by the app: the window's UART lines are the proof, here only that the frame holds a frame of the new size
            return px(a, (r1[0] + r1[2] - 12) * S, (r1[1] + r1[3] - 12) * S) != WALL, px(a, (r1[0] + r1[2] - 12) * S, (r1[1] + r1[3] - 12) * S)

        def closer(marker, count=1):
            return lambda: wait_for(marker, 40, count)
        # The Console is up at boot: the geometry the kernel starts from
        sc6 = lambda v: v * H // 600
        con = (( W - sc6(500)) // 2 // S, sc6(100) // S, sc6(500) // S, sc6(350) // S)
        calc_w = 280; calc_x = con[0] + con[2] - 150
        if calc_x + calc_w > SW - 8: calc_x = SW - 8 - calc_w
        calc_y = con[1] + 40; calc_h = min(300, BOTTOM - calc_y)
        cons = Win("Console", con, (320, 220), lambda: None, closer("console closed"), r_console, 0)
        exercise(cons, "M1c fb ok")
        for nm, op, mk, rl, cl in (("Browser", "browser", "browser open", r_browser, "browser closed"), ("Clock", "clock", "clock open", r_centred(dark), "clock closed"), ("Brick", "brick", "brick open", r_centred(terra), "brick closed")):
            n0 = uart().count(cl)
            wn = Win(nm, cons.rect, (320, 220), (lambda o=op: spot(o)), (lambda c=cl, n=n0: wait_for(c, 40, n + 1)), rl, 0)
            wn.first = tuple(cons.first)
            wn.rect[:] = cons.rect
            exercise(wn, mk)
            cons.rect[:] = wn.rect
        # the Console is back after the last close: close it so the apps and the Calculator have bare wallpaper behind them
        nc = uart().count("console closed")
        move_to(*[v * S for v in (cons.rect[0] + 24, cons.rect[1] + 16)]); click()
        wait_for("console closed", 40, nc + 1)
        n0 = uart().count("calc closed")
        calc = Win("Calculator", (calc_x, calc_y, calc_w, calc_h), (220, 260), lambda: spot("calc"), lambda: wait_for("calc closed", 40, n0 + 1), r_calc, 0)
        exercise(calc, "calc open")
        for nm, mk in (("Calendar", "calendar"), ("Notes", "notes"), ("Music", "music"), ("Books", "books")):
            pat = r"M3c app %s [^\n]*ran " % mk
            n0 = len(re.findall(pat, uart()))
            wa = Win(nm, (0, 0, 0, 0), (400, 300), (lambda o=mk: spot(o)), (lambda p=pat, n=n0: wait_re(p, n + 1)), r_app, 0, app=True)
            exercise(wa, f"M3c window {mk} ")
        # the Terminal: the same pane rectangle as the Console, F2 opens it
        nt = uart().count("terminal closed")
        term = Win("Terminal", cons.rect, (320, 220), lambda: key("f2"), lambda: wait_for("terminal closed", 40, nt + 1), r_terminal, 0)
        term.first = tuple(cons.first)
        exercise(term, "terminal open")
        cons.rect[:] = term.rect
        # the Calendar over the Terminal: moving it draws the Terminal whole again where it was covered
        nt = uart().count("terminal open")
        key("f2")
        check("F2 opens the Terminal", wait_for("terminal open", 30, nt + 1))
        time.sleep(0.3)
        nf = uart().count("M3c window calendar first frame") + 1
        spot("calendar")
        if wait_for("M3c window calendar first frame", 150, nf):
            m = re.findall(r"M3c window calendar (\d+)x(\d+) at (\d+),(\d+)\n", uart())[-1]
            r = [int(m[2]) - 8, int(m[3]) - 32, int(m[0]) + 16, int(m[1]) + 40]
            img0 = screendump(cmd, tmp)
            t = tuple(cons.rect)
            # a probe inside the Terminal and the Calendar's frame both, so before it is the Calendar, after it must be the Terminal
            px0, py0 = max(r[0], t[0]) + 6, t[1] + t[3] // 2
            check("the probe point is inside both windows", t[0] <= px0 < t[0] + t[2] and r[0] <= px0 < r[0] + r[2] and r[1] <= py0 < r[1] + r[3], (r, t))
            g = grab_title(r)
            at, size = drag(g, (g[0] + 150, g[1] + 20))
            time.sleep(0.4)
            img = screendump(cmd, tmp)
            check("the Terminal behind is drawn whole where the Calendar left it", at is not None and px(img, px0 * S, py0 * S) != WALL, (at, px(img, px0 * S, py0 * S)))
        else:
            check("the Calendar opens over the Terminal", False, uart()[-300:])
        for name, bad in (("a kernel crash", "KERNEL CRASH"), ("an app fault", "M3c app [a-z]+ [^\n]*fault"), ("an out-of-memory line", "oom ")):
            check(f"the UART shows no {name}", not re.search(bad, uart()), [l for l in uart().split("\n") if re.search(bad, l)][:3])
finally:
    q.kill(); q.wait()

shutil.rmtree(tmp, ignore_errors=True)
subprocess.run(["make", "-C", arch, "clean"], capture_output=True)
for m in fails: print("FAIL: " + m)
if fails: sys.exit(1)
print("PASS: every window drags by its title band and resizes by its edges and corner, keeps to its minimum and the dock, lays its content out again, and the apps get JT_EV_RESIZE")
