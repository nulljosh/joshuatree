#!/usr/bin/env python3
"""ARM64 back buffer and dirty rectangles (4.3.0): a dragged window repaints only its own rectangle, in one copy.

The dirtyrect build (`make -C arch/arm64 dirtyrecttest`: the glitch build's 1920x1080 ramfb, so window_scale is 2 as on a Pi
at 1080p, plus FB_TRACE, which prints `fb: flush x,y,w,h copy N us` once per window move) on QEMU's virt machine with a USB
keyboard and mouse behind a hub. Through QMP the boot Console is closed and the 4.0 gate's three apps, Calendar, Toroid and
Notes, are opened through Spotlight, each in its own slot, cascaded. Then:
  - Calendar is raised (a click on the part of its title band the others leave showing) and the pointer is parked on bare
    wallpaper; two screendumps a second apart are equal (nothing animates in front);
  - Calendar is dragged by its title band with ONE pointer report, 50 left and 10 up (logical), and the pointer is parked
    again in the same place;
  - every pixel that changed is inside the union of Calendar's old and new rectangles;
  - Calendar's frame and content at the new place are the pixels it had at the old place (corners aside, which blend over
    whatever is under them);
  - the strip it uncovered, where no window was behind, is flat wallpaper, and where Toroid was behind it is Toroid again;
  - the kernel printed exactly one `fb: flush` line for the drag, and its rectangle is that union;
  - then the 4.0 gate on the same desktop: Toroid is raised and sent the backquote it crashes on, the kernel survives, Toroid
    ends, and Calendar and Notes still answer a click each (their `M3c event click` lines).
Skips (exit 0) when clang's aarch64 target, ld.lld or qemu-system-aarch64 is missing.
Usage: tools/checks/arm64-dirtyrect-check.py   (from the repo root; EXPLORE=1 keeps the screendumps)
"""
import json, os, re, shutil, socket, subprocess, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from scratch import scratch_dir

root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
arch = os.path.join(root, "arch/arm64")
if not all(shutil.which(t) for t in ("clang", "ld.lld", "qemu-system-aarch64")):
    print("SKIP: clang, ld.lld or qemu-system-aarch64 not installed"); sys.exit(0)
if subprocess.run(["make", "-C", arch, "dirtyrecttest"], capture_output=True).returncode:
    print("FAIL: arch/arm64 `make dirtyrecttest` does not build"); sys.exit(1)
fails = []
def check(name, ok, detail=""):
    print(("  ok: " if ok else "  FAIL: ") + name + ("" if ok or not detail else "  " + str(detail)[-500:]))
    if not ok: fails.append(name)

W, H, S = 1920, 1080, 2
tmp = scratch_dir("arm64-dirtyrect")
log, sock = tmp + "/uart", tmp + "/qmp"
if os.path.exists(log): os.remove(log)
q = subprocess.Popen(["qemu-system-aarch64", "-machine", "virt", "-cpu", "cortex-a72", "-m", "512", "-nic", "none", "-device", "ramfb",
                      "-device", "pcie-root-port,id=rp,chassis=1", "-device", "qemu-xhci,id=x,bus=rp",
                      "-device", "usb-hub,id=h,bus=x.0,port=1",
                      "-device", "usb-kbd,bus=x.0,port=1.1", "-device", "usb-mouse,id=m1,bus=x.0,port=1.2",
                      "-display", "none", "-serial", "file:" + log, "-qmp", "unix:%s,server,nowait" % sock,
                      "-kernel", os.path.join(arch, "dirtyrect-kernel8.elf")], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
def uart(): return open(log, errors="replace").read() if os.path.exists(log) else ""
def wait_for(text, secs=6.0, after=0):
    end = time.time() + secs
    while time.time() < end:
        if text in uart()[after:]: return True
        time.sleep(0.05)
    return False

def diff_box(a, b):   # (count, bounding box x0, y0, x1, y1 inclusive) of the pixels that differ
    if a == b: return 0, None
    n = 0; x0 = y0 = 1 << 30; x1 = y1 = -1
    for y in range(H):
        ra, rb = a[y * W * 3:(y + 1) * W * 3], b[y * W * 3:(y + 1) * W * 3]
        if ra == rb: continue
        for x in range(W):
            if ra[x * 3:x * 3 + 3] != rb[x * 3:x * 3 + 3]:
                n += 1; x0, y0, x1, y1 = min(x0, x), min(y0, y), max(x1, x), max(y1, y)
    return n, (x0, y0, x1, y1)
def region(img, x, y, w, h): return b"".join(img[((y + j) * W + x) * 3:((y + j) * W + x + w) * 3] for j in range(h))
def px(img, x, y): i = (y * W + x) * 3; return tuple(img[i:i + 3])

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
    def shot(name):
        path = tmp + "/%s.ppm" % name
        if os.path.exists(path): os.remove(path)
        cmd("screendump", filename=path)
        for _ in range(60):
            if os.path.exists(path) and os.path.getsize(path) >= W * H * 3: break
            time.sleep(0.1)
        time.sleep(0.2)
        img = open(path, "rb").read().split(b"\n", 3)[3]
        if not os.environ.get("EXPLORE"): os.remove(path)
        return img
    pos = [W // 2, H // 2]
    def rel(dx, dy):
        cmd("input-send-event", events=[{"type": "rel", "data": {"axis": "x", "value": dx}}, {"type": "rel", "data": {"axis": "y", "value": dy}}])
        pos[0] += dx; pos[1] += dy
        return wait_for(f"mouse {pos[0]},{pos[1]}\n", 4)
    def move_to(x, y):
        while (pos[0], pos[1]) != (x, y):
            if not rel(max(-100, min(100, x - pos[0])), max(-100, min(100, y - pos[1]))): return False
        return True
    def button(down):
        word = "key 272 " + ("down" if down else "up"); n = uart().count(word)
        cmd("input-send-event", events=[{"type": "btn", "data": {"down": down, "button": "left"}}])
        end = time.time() + 4
        while time.time() < end and uart().count(word) <= n: time.sleep(0.05)
    def click(x, y): move_to(x, y); button(True); time.sleep(0.05); button(False); time.sleep(0.5)
    PARK = (40, 600)   # bare wallpaper, left of every window and above the dock
    def key(name): cmd("send-key", keys=[{"type": "qcode", "data": name}], **{"hold-time": 60}); time.sleep(0.2)
    sc = lambda v: v * H // 600
    con = ((W - sc(500)) // 2 // S, sc(100) // S)   # the boot Console's corner, logical: closed by its red dot so nothing behind changes
    click((con[0] + 24) * S, (con[1] + 16) * S)
    check("the boot Console closes by its red dot", wait_for("console closed", 6))
    for name in ("calendar", "toroid", "notes"):   # the 4.0 gate's three apps, opened through Spotlight, each in its own slot
        n = len(uart()); key("f1"); wait_for("spotlight open", 4, n)
        for ch in name: key(ch)
        key("ret")
        check(name + " opens", wait_for("M3c window %s first frame" % name, 15, n), uart()[n:][-300:]); time.sleep(0.5)
    win, order = {}, []
    for m in re.finditer(r"M3c window (\w+) (\d+)x(\d+) at (\d+),(\d+)\n", uart()):
        win[m[1]] = (int(m[4]) - 8, int(m[5]) - 32, int(m[2]) + 16, int(m[3]) + 40); order.append(m[1])   # the frame, logical
    if len(win) != 3: raise SystemExit

    cal = win["calendar"]
    if order[-1] != "calendar":   # the right end of its title band: the other two sit 36 and 72 to its left
        n = len(uart()); click((cal[0] + cal[2] - 20) * S, (cal[1] + 12) * S); wait_for("focus app", 4, n)
    move_to(*PARK); time.sleep(1.0)
    a = shot("before"); time.sleep(1.0); a2 = shot("before2")
    n0, box0 = diff_box(a, a2)
    check("the desktop is still before the drag (Calendar in front, nothing animates)", n0 == 0, f"{n0} pixels differ, box {box0}")

    # ---- the drag: one pointer report, 50 left and 10 up ----
    gx, gy = (cal[0] + 200) * S, (cal[1] + 12) * S
    move_to(gx, gy); n = len(uart())
    button(True); rel(-100, -20); button(False)
    check("the drag ends where it should", wait_for("window at %d,%d\n" % (cal[0] - 50, cal[1] - 10), 4, n), uart()[n:][-400:])
    flushes = re.findall(r"fb: flush (\d+),(\d+),(\d+),(\d+) copy (\d+) us", uart()[n:])
    new = (cal[0] - 50, cal[1] - 10, cal[2], cal[3])
    ux, uy = min(cal[0], new[0]) * S, min(cal[1], new[1]) * S
    ex, ey = max(cal[0] + cal[2], new[0] + new[2]) * S, max(cal[1] + cal[3], new[1] + new[3]) * S
    union = (ux, uy, ex - ux, ey - uy)
    check("the kernel printed one flush for the drag", len(flushes) == 1, flushes)
    if flushes:
        got = tuple(int(v) for v in flushes[0][:4])
        check("its rectangle is the union of the old and new window rectangles %s" % (union,), got == union, got)
        print("  measured: one copy of %dx%d px (%.1f MB) in %s us under QEMU" % (got[2], got[3], got[2] * got[3] * 4 / 1e6, flushes[0][4]))
    move_to(*PARK); time.sleep(0.8)
    b = shot("after")
    nd, boxd = diff_box(a, b)
    inside = boxd is None or (boxd[0] >= ux and boxd[1] >= uy and boxd[2] < ex and boxd[3] < ey)
    check("every pixel that changed is inside that union", nd > 0 and inside, f"{nd} pixels differ, box {boxd}, union {union}")
    same = True
    for bx, by, bw, bh in ((2, 20, cal[2] - 4, cal[3] - 40), (20, 2, cal[2] - 40, cal[3] - 4)):   # the frame and content, the 18 px corners aside
        if region(a, (cal[0] + bx) * S, (cal[1] + by) * S, bw * S, bh * S) != region(b, (new[0] + bx) * S, (new[1] + by) * S, bw * S, bh * S): same = False
    check("Calendar's frame and content moved whole: the same pixels, shifted", same)
    wall = px(a, PARK[0] + 40, PARK[1])
    rx0 = (new[0] + new[2]) * S + 2   # the strip it left on the right
    behind = [w for k, w in win.items() if k != "calendar"]
    strip_wall = all(px(b, x, y) == wall for x in range(rx0, (cal[0] + cal[2]) * S, 4) for y in range((cal[1] + 40) * S, (cal[1] + cal[3] - 30) * S, 16)
                     if not any(w[0] * S <= x < (w[0] + w[2]) * S and w[1] * S <= y < (w[1] + w[3]) * S for w in behind))
    check("the strip it uncovered where nothing was behind is flat wallpaper", strip_wall)
    tor = win["toroid"]
    tx = (tor[0] + tor[2]) * S - 6   # Toroid's right edge, which Calendar covered at the old place and not at the new one
    check("where Toroid was behind, Toroid's frame is back (not wallpaper)", rx0 <= tx < (cal[0] + cal[2]) * S and px(b, tx, (tor[1] + tor[3] // 2) * S) != wall,
          (tx, px(b, tx, (tor[1] + tor[3] // 2) * S)))

    # ---- the 4.0 gate on the same desktop: Toroid crashes, the kernel and the other two carry on ----
    n = len(uart()); click(tx, (tor[1] + tor[3] // 2) * S); wait_for("focus app", 4, n)
    cmd("send-key", keys=[{"type": "qcode", "data": "grave_accent"}], **{"hold-time": 60})
    check("Toroid crashed on purpose and ended", wait_for("toroid: crashing on purpose", 8, n) and wait_for("M3c app toroid", 8, n), uart()[n:][-400:])
    time.sleep(1.0)
    nts = win["notes"]
    for name, x in (("notes", nts[0] + 12), ("calendar", new[0] + new[2] - 14)):   # a strip of each that the other leaves showing
        n = len(uart()); click(x * S, (new[1] + 200) * S); click(x * S, (new[1] + 200) * S)
        check(name + " still answers a click after the crash", wait_for("to %s\n" % name, 6, n), uart()[n:][-300:])
    check("the kernel did not crash", "KERNEL CRASH" not in uart())
except SystemExit:
    pass
finally:
    q.kill(); q.wait()
for m in fails: print("FAIL: " + m)
if fails: print(uart()[-1500:]); sys.exit(1)
print("PASS: a drag repaints only the union of the old and new rectangles, in one copy, and the 4.0 gate holds")
