#!/usr/bin/env python3
"""The traffic lights as on a Mac, on the Pi (6.6.0, arch/arm64/windows.c traffic_tick), in headless QEMU virt with a USB
mouse behind a hub. The boot Console is the only window, so it is in front:

  1. its red dot is red with no X on it while the pointer is elsewhere;
  2. the pointer over its dots puts the X on (`traffic front 1 hover`);
  3. Calendar opened from the dock goes in front: the Console's red dot turns grey, and Calendar's is red with no X;
  4. a click on the Console's body brings it back in front, red again.

Discriminating: pass GUI_TL_GLYPHS for every window in tl_state and steps 1 and 3 fail.

Usage: tools/checks/arm64-traffic-check.py   (from the repo root)
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

tmp = scratch_dir("arm64-traffic")
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
    def px(img, x, y): i = (y * W + x) * 3; return img[i:i + 3]
    def dark(img, cx, cy): return sum(1 for y in range(cy - 5, cy + 6) for x in range(cx - 5, cx + 6) if max(px(img, x, y)) < 0x90)   # the X's strokes
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
    RED, GREY = b"\xff\x5f\x57", b"\xd9\xd3\xcb"
    con = ((W - 500) // 2 + 24, 100 + 16)   # the boot Console's red dot (logical = physical at 800 x 600)
    move_to(*PARK); time.sleep(0.6)
    a = shot()
    check("the Console in front: its red dot is red with no X", px(a, *con) == RED and dark(a, *con) == 0, (px(a, *con), dark(a, *con)))
    n = len(uart()); move_to(con[0] + 22, con[1]); 
    hov = wait_for("traffic front 1 hover", 40, n); time.sleep(0.3); b = shot()
    check("the pointer over its dots puts the X on", hov and dark(b, *con) > 5, (hov, dark(b, *con)))
    n = len(uart()); move_to(*slot_centre(3)); time.sleep(0.3); click()
    check("Calendar opens from the dock", wait_for("M3c window calendar first frame", 80, n), uart()[n:][-300:])
    move_to(*PARK); time.sleep(0.8)
    c = shot()
    m = re.findall(r"M3c window calendar \d+x\d+ at (\d+),(\d+)", uart())   # its content's corner; the frame is 8 left and 32 up
    cal = (int(m[-1][0]) - 8 + 24, int(m[-1][1]) - 32 + 16) if m else (0, 0)
    check("Calendar in front: the Console's red dot is grey", px(c, *con) == GREY, px(c, *con))
    check("and Calendar's red dot is red with no X", bool(m) and px(c, *cal) == RED and dark(c, *cal) == 0, (cal, px(c, *cal), dark(c, *cal)))
    n = len(uart()); move_to(200, 380); time.sleep(0.2); click(); move_to(*PARK); time.sleep(0.8)
    d = shot()
    check("a click on the Console brings it back in front, red again", px(d, *con) == RED, px(d, *con))
    check("no fault, no crash", "KERNEL CRASH" not in uart(), uart()[-300:])
except SystemExit:
    pass
finally:
    q.terminate()
    try: q.wait(5)
    except subprocess.TimeoutExpired: q.kill()

if fails: print("FAIL: " + "; ".join(fails)); sys.exit(1)
print("PASS: the front window's dots are plain until the pointer is over them, a window behind another goes grey, and focus moves the colour with it")
