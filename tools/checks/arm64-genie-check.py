#!/usr/bin/env python3
"""Minimize into the dock with the genie on the Pi (6.5.0, arch/arm64/appwin.c app_minimize), in headless QEMU virt with a
USB mouse behind a hub.

  1. A click on Calendar's dock tile opens its window (an EL0 app).
  2. A click on its yellow dot minimizes it: `app minimize calendar, genie N frames` with N of at least 5, and every pixel
     between the menu bar and the dock is the desktop from before Calendar opened. The app is still running (no exit line).
  3. A click on its dock tile brings it back the same way: `app restore calendar, genie N frames`, and every pixel between
     the menu bar and the dock is the screen from before it was minimized.

Discriminating: drop the on_min_dot line in windows.c and step 2 fails.

Usage: tools/checks/arm64-genie-check.py   (from the repo root)
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
icon = max(16, min(H * 7 // 100, (min(740, W - 40) - 2 * 10 - 10 * 6) // 11))
dock_x0, dock_y0 = (W - (11 * icon + 10 * 6 + 2 * 10)) // 2, H - icon - 2 * 10 - 24
def slot_centre(slot): return dock_x0 + 10 + slot * (icon + 6) + icon // 2, dock_y0 + 10 + icon // 2
Y_TOP, Y_BOT = 30, dock_y0 - 50   # below the menu bar (its clock ticks), above the dock and the magnify room
PARK = (W - 40, 60)

tmp = scratch_dir("arm64-genie")
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
    n = len(uart()); move_to((W - 500) // 2 + 24, 100 + 16); time.sleep(0.2); click()   # the boot Console's red dot: its log would scroll under the test
    check("the boot Console closes first", wait_for("console closed", 40, n), uart()[n:][-200:])
    move_to(*PARK); time.sleep(0.4)
    base = shot()
    n = len(uart()); move_to(*slot_centre(3)); time.sleep(0.3); click()
    check("a click on Calendar's dock tile opens its window", wait_for("M3c window calendar first frame", 80, n), uart()[n:][-300:])
    move_to(*PARK); time.sleep(0.6)
    up = shot()
    yel = [(i // 3 % W, i // 3 // W) for i in range(0, len(up), 3) if up[i:i + 3] == b"\xff\xd6\x4a"]
    if not yel: check("the window's yellow dot is on the screen", False); raise SystemExit
    mx = max(x for x, _ in yel); pts = [(x, y) for x, y in yel if x > mx - 16]
    dot = (sum(x for x, _ in pts) // len(pts), sum(y for _, y in pts) // len(pts))
    n = len(uart()); move_to(*dot); time.sleep(0.2); click()
    m = re.findall(r"app minimize calendar, genie (\d+) frames", uart()[n:]) if wait_for("app minimize calendar", 40, n) else []
    check("the yellow dot minimizes it with the genie (5 frames or more)", bool(m) and int(m[-1]) >= 5, (m, uart()[n:][-300:]))
    move_to(*PARK); time.sleep(0.5)
    gone = shot()
    check("every pixel between the menu bar and the dock is the desktop from before it opened", ndiff(band(base), band(gone)) == 0, (ndiff(band(base), band(gone)), where(band(base), band(gone))))
    check("the app is still running", "M3c app calendar" not in uart()[n:], uart()[n:][-200:])
    n = len(uart()); move_to(*slot_centre(3)); time.sleep(0.3); click()
    m = re.findall(r"app restore calendar, genie (\d+) frames", uart()[n:]) if wait_for("app restore calendar", 40, n) else []
    check("its dock tile brings it back with the genie (5 frames or more)", bool(m) and int(m[-1]) >= 5, (m, uart()[n:][-300:]))
    move_to(*PARK); time.sleep(0.6)
    back = shot()
    check("and every pixel between the menu bar and the dock is the window as it was", ndiff(band(up), band(back)) == 0, (ndiff(band(up), band(back)), where(band(up), band(back))))
    check("no fault, no crash", "KERNEL CRASH" not in uart() and "oom" not in uart(), uart()[-300:])
except SystemExit:
    pass
finally:
    q.terminate()
    try: q.wait(5)
    except subprocess.TimeoutExpired: q.kill()

if fails: print("FAIL: " + "; ".join(fails)); sys.exit(1)
print("PASS: the yellow dot minimizes an app's window into its dock tile with the genie, the desktop comes back exactly, the app keeps running, and its tile brings the window back the same way")
