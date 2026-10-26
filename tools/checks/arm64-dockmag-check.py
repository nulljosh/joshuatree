#!/usr/bin/env python3
"""The Pi's magnifying dock (6.3.0, arch/arm64/crash.c dock_magnify), in headless QEMU virt with a USB mouse behind a hub.

  0. A plain hover (the pointer in the label zone above the tray) and back leaves no pixel behind: the band copy starts
     DOCK_BAND_LIFT above band_top, under the label capsule's soft edge (it left 387 pixels on the line above the dock).
  1. The pointer on a dock tile grows it: `dock magnify 5 size S of B` with S over B * 1.4, and the hover label still names
     it (`M1d hover 5 Reminders`).
  2. The grown tile reaches up into the room above the tray: pixels there change, and only near the pointer (the room at
     the screen's far left is untouched).
  3. Moving along the dock follows the pointer (`dock magnify 6`).
  4. Leaving the dock puts the screen back exactly: every pixel from the room's top to the bottom of the screen equals the
     screen before the pointer arrived (`dock magnify off`).
  5. A click on the magnified Terminal tile opens the Terminal (the tile under the pointer is still the one it hits).
  6.4.0: the tile grows in over MAG_IN_MS on the shared ease_out: the first size logged on arrival is below the last.

Discriminating: drop the dock_magnify call in pointer_moved (windows.c) and steps 1 to 3 fail.

Usage: tools/checks/arm64-dockmag-check.py   (from the repo root)
"""
import json, os, shutil, socket, subprocess, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from scratch import scratch_dir

root = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
arch = os.path.join(root, "arch", "arm64")
if not all(shutil.which(t) for t in ("clang", "ld.lld", "qemu-system-aarch64")):
    print("SKIP: no aarch64 toolchain"); sys.exit(0)
subprocess.run(["make", "-s", "-C", arch, "kernel8.elf"], check=True)
fails = []
def check(name, ok, detail=""):
    print(("  ok: " if ok else "  FAIL: ") + name + (f" ({detail})" if detail and not ok else ""))
    if not ok: fails.append(name)

W, H = 800, 600
icon = max(16, min(H * 7 // 100, (min(740, W - 40) - 2 * 10 - 10 * 6) // 11))
dock_x0, dock_y0 = (W - (11 * icon + 10 * 6 + 2 * 10)) // 2, H - icon - 2 * 10 - 24
ROOM = 46
def slot_centre(slot): return dock_x0 + 10 + slot * (icon + 6) + icon // 2, dock_y0 + 10 + icon // 2

tmp = scratch_dir("arm64-dockmag")
log, sock = tmp + "/uart", tmp + "/qmp"
q = subprocess.Popen(["qemu-system-aarch64", "-machine", "virt", "-cpu", "cortex-a72", "-m", "256", "-nic", "none", "-device", "ramfb",
                      "-device", "pcie-root-port,id=rp,chassis=1", "-device", "qemu-xhci,id=x,bus=rp",
                      "-device", "usb-hub,id=h,bus=x.0,port=1",
                      "-device", "usb-kbd,bus=x.0,port=1.1", "-device", "usb-mouse,id=m1,bus=x.0,port=1.2",
                      "-display", "none", "-serial", "file:" + log, "-qmp", "unix:%s,server,nowait" % sock,
                      "-kernel", os.path.join(arch, "kernel8.elf")], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
def uart(): return open(log, errors="replace").read() if os.path.exists(log) else ""
def wait_for(text, tries=60, after=0):
    for _ in range(tries):
        if text in uart()[after:]: return True
        time.sleep(0.1)
    return False
try:
    if not wait_for("usb ready: 1 kbd, 1 mouse", 300): check("the mouse behind the hub came up", False, uart()[-400:]); raise SystemExit
    s_ = socket.socket(socket.AF_UNIX); s_.connect(sock); f = s_.makefile("rw"); f.readline()
    def cmd(c, **a):
        f.write(json.dumps({"execute": c, "arguments": a}) + "\n"); f.flush()
        while True:
            r = json.loads(f.readline())
            if "return" in r or "error" in r: return r
    cmd("qmp_capabilities"); time.sleep(0.5)
    def shot():
        path = tmp + "/shot%d.ppm" % time.monotonic_ns()
        cmd("screendump", filename=path)
        for _ in range(30):
            if os.path.exists(path) and os.path.getsize(path) >= W * H * 3: break
            time.sleep(0.1)
        parts = open(path, "rb").read().split(b"\n", 3); os.remove(path); return parts[3]
    def rows(img, y0, y1): return img[y0 * W * 3:y1 * W * 3]
    def changed(a, b, x0, y0, x1, y1):
        return sum(1 for y in range(y0, y1) for x in range(x0, x1) if a[(y * W + x) * 3:(y * W + x) * 3 + 3] != b[(y * W + x) * 3:(y * W + x) * 3 + 3])
    pos = [W // 2, H // 2]
    def move_to(x, y):
        while (pos[0], pos[1]) != (x, y):
            dx = max(-100, min(100, x - pos[0])); dy = max(-100, min(100, y - pos[1]))
            cmd("input-send-event", events=[{"type": "rel", "data": {"axis": "x", "value": dx}}, {"type": "rel", "data": {"axis": "y", "value": dy}}])
            pos[0] += dx; pos[1] += dy
            if not wait_for(f"mouse {pos[0]},{pos[1]}\n", 40): return False
        return True
    move_to(W // 2, 200); time.sleep(0.3)
    base = shot()
    n = len(uart()); move_to(slot_centre(5)[0], dock_y0 - 14); time.sleep(0.4)   # the label zone above the tray: the plain hover, no magnify
    labelled = "M1d hover 5" in uart()[n:]
    move_to(W // 2, 200); time.sleep(0.4)
    d0 = changed(base, shot(), 0, dock_y0 - ROOM, W, H)
    check("a plain hover label leaves nothing behind, its soft edge over the line above the dock included", labelled and d0 == 0, (labelled, d0))
    n = len(uart())
    move_to(*slot_centre(5)); time.sleep(0.4)
    import re
    m = re.findall(r"dock magnify 5 size (\d+) of (\d+)", uart()[n:])
    check("the pointer on a tile grows it to at least 1.4 times", bool(m) and int(m[-1][0]) * 10 >= int(m[-1][1]) * 14, (m, uart()[n:][-200:]))
    check("it grows in (6.4.0, lib/ease.h): the first size on arrival is below the last", len(m) >= 2 and int(m[0][0]) < int(m[-1][0]), m)
    check("the hover label still names it", "M1d hover 5 Reminders" in uart()[n:], uart()[n:][-200:])
    mag = shot()
    cx = slot_centre(5)[0]
    near = changed(base, mag, max(0, cx - 60), dock_y0 - ROOM, min(W, cx + 60), dock_y0)
    far = changed(base, mag, 0, dock_y0 - ROOM, 40, dock_y0)
    check("the grown tile reaches into the room above the tray, near the pointer only", near > 300 and far == 0, (near, far))
    n = len(uart()); move_to(*slot_centre(6)); time.sleep(0.4)
    check("moving along follows the pointer (dock magnify 6)", "dock magnify 6" in uart()[n:], uart()[n:][-200:])
    n = len(uart()); move_to(W // 2, 200); time.sleep(0.5)
    check("leaving the dock says so (dock magnify off)", "dock magnify off" in uart()[n:], uart()[n:][-200:])
    back = shot()
    y_top = dock_y0 - ROOM
    diff = changed(base, back, 0, y_top, W, H)
    if os.environ.get("JT_DOCKMAG_DUMP"):
        for nm, im in (("base", base), ("mag", mag), ("back", back)): open(f"{os.environ['JT_DOCKMAG_DUMP']}/{nm}.ppm", "wb").write(b"P6\n%d %d\n255\n" % (W, H) + im)
    box = [(x, y) for y in range(y_top, H) for x in range(W) if base[(y * W + x) * 3:(y * W + x) * 3 + 3] != back[(y * W + x) * 3:(y * W + x) * 3 + 3]]
    bb = (min(p[0] for p in box), min(p[1] for p in box), max(p[0] for p in box), max(p[1] for p in box)) if box else None
    check("and puts every pixel from the room's top to the screen's bottom back", diff == 0, (diff, "box", bb, "dock_y0", dock_y0))
    move_to(*slot_centre(6)); time.sleep(0.4)
    n = len(uart())
    cmd("input-send-event", events=[{"type": "btn", "data": {"down": True, "button": "left"}}])
    cmd("input-send-event", events=[{"type": "btn", "data": {"down": False, "button": "left"}}])
    check("a click on the magnified Terminal tile opens the Terminal", wait_for("terminal open", 40, n), uart()[n:][-300:])
    check("no fault, no crash", "KERNEL CRASH" not in uart() and "oom dock magnify" not in uart(), uart()[-300:])
except SystemExit:
    pass
finally:
    q.terminate()
    try: q.wait(5)
    except subprocess.TimeoutExpired: q.kill()

if fails: print("FAIL: " + "; ".join(fails)); sys.exit(1)
print("PASS: the Pi's dock magnifies under the pointer, grows into the room above the tray only, follows the pointer, still opens the tile clicked, and leaves the screen exactly as it was")
