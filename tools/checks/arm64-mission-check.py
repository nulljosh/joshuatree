#!/usr/bin/env python3
"""Mission Control on the Pi (11.2.0, arch/arm64/keys.c mc_open): Ctrl+Up shows the four desktops as chips and this
desktop's app windows as thumbnails, on a card of Spotlight's machinery. Headless QEMU virt, USB keyboard and mouse.

  1. Calendar, then Notes, open on desktop 1. Ctrl+Up: `mission control open, desktop 1, 2 windows`, the chip for desktop 1
     is terracotta, and both thumbnails hold a window's pixels (not a blank card).
  2. Right, Enter: the second window (Calendar) comes to the front and the card closes.
  3. Ctrl+Up, a click on the Desktop 2 chip: `space 2`. Ctrl+Up there says 0 windows; Esc puts the screen back pixel for
     pixel. A click outside the card closes it too.

Discriminating: drop the mc_mode line in windows.c pointer_click and step 3's chip click does nothing.

Usage: tools/checks/arm64-mission-check.py   (from the repo root)
"""
import json, os, shutil, socket, subprocess, sys, time, re
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
icon = max(16, min(H * 7 // 100, (min(740, W - 40) - 2 * 10 - 11 * 6) // 12))
dock_x0, dock_y0 = (W - (12 * icon + 11 * 6 + 2 * 10)) // 2, H - icon - 2 * 10 - 24
def slot_centre(slot): return dock_x0 + 10 + slot * (icon + 6) + icon // 2, dock_y0 + 10 + icon // 2
Y_TOP, Y_BOT = 30, dock_y0 - 50   # below the menu bar (its clock ticks), above the dock and the magnify room
PARK = (W - 40, 60)

tmp = scratch_dir("arm64-mission")
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
    def band(img): return img[Y_TOP * W * 3:Y_BOT * W * 3]
    def ndiff(a, b): return sum(1 for i in range(0, len(a), 3) if a[i:i + 3] != b[i:i + 3])
    def where(a, b):   # the differing pixels' box, on the screen
        d = [(i // 3 % W, Y_TOP + i // 3 // W) for i in range(0, len(a), 3) if a[i:i + 3] != b[i:i + 3]]
        return (min(x for x, _ in d), min(y for _, y in d), max(x for x, _ in d), max(y for _, y in d)) if d else None
    pos = [W // 2, H // 2]
    def move_to(x, y):
        while (pos[0], pos[1]) != (x, y):
            dx = max(-100, min(100, x - pos[0])); dy = max(-100, min(100, y - pos[1]))
            cmd("input-send-event", events=[{"type": "rel", "data": {"axis": "x", "value": dx}}, {"type": "rel", "data": {"axis": "y", "value": dy}}])
            pos[0] += dx; pos[1] += dy
            if not wait_for(f"mouse {pos[0]},{pos[1]}\n", 40): return False
        return True
    def click():
        cmd("input-send-event", events=[{"type": "btn", "data": {"down": True, "button": "left"}}])
        cmd("input-send-event", events=[{"type": "btn", "data": {"down": False, "button": "left"}}])
    def keys(*k): cmd("send-key", keys=[{"type": "qcode", "data": x} for x in k]); time.sleep(0.3)
    def keys(*k): cmd("send-key", keys=[{"type": "qcode", "data": x} for x in k]); time.sleep(0.3)
    def px(img, x, y): i = (y * W + x) * 3; return img[i:i + 3]
    TERRA, CREAM = b"\xb5\x50\x2c", b"\xfa\xf8\xf4"
    n = len(uart()); move_to((W - 500) // 2 + 24, 100 + 16); time.sleep(0.2); click()
    check("the boot Console closes first", wait_for("console closed", 40, n), uart()[n:][-200:])
    for slot, name in ((3, "calendar"), (4, "notes")):
        n = len(uart()); move_to(*slot_centre(slot)); time.sleep(0.3); click()
        check(name + " opens", wait_for("M3c window %s first frame" % name, 80, n), uart()[n:][-300:])
    move_to(*PARK); time.sleep(0.6)
    n = len(uart()); keys("ctrl", "up")
    check("Ctrl+Up opens Mission Control with both windows", wait_for("mission control open, desktop 1, 2 windows", 40, n), uart()[n:][-300:])
    time.sleep(0.5); mc = shot()
    if os.environ.get("JT_MC_SHOT"): open(os.environ["JT_MC_SHOT"], "wb").write(b"P6\n%d %d\n255\n" % (W, H) + mc)
    terra = [(i // 3 % W, i // 3 // W) for i in range(0, len(mc), 3) if mc[i:i + 3] == TERRA and i // 3 // W < H // 2]
    if not terra: check("the Desktop 1 chip is terracotta", False); raise SystemExit
    top = min(y for _, y in terra); chip = (min(x for x, y in terra if y == top), top)   # the chips sit above the selection ring
    check("the Desktop 1 chip is terracotta", px(mc, chip[0] + 4, chip[1] + 3) == TERRA, chip)
    cy = chip[1] + 13
    def ink(x0, y0, x1, y1): return sum(1 for y in range(y0, y1, 2) for x in range(x0, x1, 2) if px(mc, x, y) != CREAM)
    left, right = ink(chip[0], chip[1] + 50, W // 2 - 10, chip[1] + 250), ink(W // 2 + 10, chip[1] + 50, W - chip[0], chip[1] + 250)
    check("both thumbnails hold a window's pixels", left > 2000 and right > 2000, (left, right))
    n = len(uart()); keys("right"); keys("ret")
    check("Right, Enter brings the second window (Calendar) up and closes the card",
          wait_for("mission control: calendar", 40, n) and "mission control close" in uart()[n:], uart()[n:][-300:])
    move_to(*PARK); time.sleep(0.4)
    n = len(uart()); keys("ctrl", "up"); wait_for("mission control open", 40, n)
    n = len(uart()); move_to(chip[0] + 120 + 50, cy); time.sleep(0.2); click()
    check("a click on the Desktop 2 chip goes there", wait_for("space 2", 40, n) and "mission control close" in uart()[n:], uart()[n:][-300:])
    move_to(*PARK); time.sleep(0.6); d2 = shot()
    n = len(uart()); keys("ctrl", "up")
    check("Mission Control on desktop 2 shows no windows", wait_for("mission control open, desktop 2, 0 windows", 40, n), uart()[n:][-300:])
    n = len(uart()); keys("esc")
    check("Esc closes it", wait_for("mission control close", 40, n), uart()[n:][-200:])
    time.sleep(0.5); back = shot()
    check("and the screen is back pixel for pixel", ndiff(band(d2), band(back)) == 0, (ndiff(band(d2), band(back)), where(band(d2), band(back))))
    n = len(uart()); keys("ctrl", "up"); wait_for("mission control open", 40, n)
    n = len(uart()); move_to(10, H // 2); time.sleep(0.2); click()
    check("a click outside the card closes it", wait_for("mission control close", 40, n), uart()[n:][-200:])
    check("no fault, no crash", "KERNEL CRASH" not in uart() and "oom" not in uart(), uart()[-300:])
except SystemExit:
    pass
finally:
    q.terminate()
    try: q.wait(5)
    except subprocess.TimeoutExpired: q.kill()

if fails: print("FAIL: " + "; ".join(fails)); sys.exit(1)
print("PASS: Ctrl+Up shows the desktops and this desktop's windows as thumbnails, Enter raises one, a chip goes to its desktop, Esc or a click outside closes it and puts the screen back")
