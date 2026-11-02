#!/usr/bin/env python3
"""Spaces on the Pi (11.1.0, arch/arm64/appwin.c space_switch): four desktops, Ctrl+Left and Ctrl+Right step between them,
and an app's window stays on the desktop it was on. Headless QEMU virt, a USB keyboard and mouse behind a hub.

  1. Calendar opens from its dock tile on desktop 1.
  2. Ctrl+Right: `space 2`, and every pixel between the menu bar and the dock is the desktop from before Calendar opened;
     a click where Calendar was reaches nothing (no event for it), yet it is still running.
  3. Notes opens on desktop 2. Ctrl+Left: `space 1`, and the screen is Calendar's as it was, pixel for pixel: Notes is gone.
  4. Cmd+Tab on desktop 1 does not bring Notes over.
  5. Notes' dock tile goes to desktop 2 (`space 2`) and the screen is Notes' as it was.

Discriminating: open the windows on every desktop (skip the park loop in space_switch) and step 2 fails; drop the min == 2
line in app_raise and step 5 brings Notes onto desktop 1.

Usage: tools/checks/arm64-spaces-check.py   (from the repo root)
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

tmp = scratch_dir("arm64-spaces")
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
    n = len(uart()); move_to((W - 500) // 2 + 24, 100 + 16); time.sleep(0.2); click()   # the boot Console's red dot: its log would scroll under the test
    check("the boot Console closes first", wait_for("console closed", 40, n), uart()[n:][-200:])
    move_to(*PARK); time.sleep(0.4)
    base = shot()
    n = len(uart()); move_to(*slot_centre(3)); time.sleep(0.3); click()
    check("Calendar opens from its dock tile", wait_for("M3c window calendar first frame", 80, n), uart()[n:][-300:])
    move_to(*PARK); time.sleep(0.6)
    cal = shot()
    box = where(band(base), band(cal))
    if not box: check("Calendar's window is on the screen", False); raise SystemExit
    n0 = n = len(uart()); keys("ctrl", "right")
    check("Ctrl+Right goes to desktop 2", wait_for("space 2", 40, n), uart()[n:][-300:])
    time.sleep(0.6); d2 = shot()
    check("desktop 2 is the bare desktop from before Calendar opened", ndiff(band(base), band(d2)) == 0, (ndiff(band(base), band(d2)), where(band(base), band(d2))))
    n = len(uart()); move_to((box[0] + box[2]) // 2, (box[1] + box[3]) // 2 + 30); time.sleep(0.2); click(); time.sleep(1.0)
    check("a click where Calendar was reaches nothing", "to calendar" not in uart()[n:], uart()[n:][-300:])
    check("Calendar is still running", "M3c app calendar" not in uart()[n0:], uart()[n0:][-200:])
    n = len(uart()); move_to(*slot_centre(4)); time.sleep(0.3); click()
    check("Notes opens on desktop 2", wait_for("M3c window notes first frame", 80, n), uart()[n:][-300:])
    move_to(*PARK); time.sleep(0.6)
    notes = shot()
    n = len(uart()); keys("ctrl", "left")
    check("Ctrl+Left goes back to desktop 1", wait_for("space 1", 40, n), uart()[n:][-300:])
    time.sleep(0.6); d1 = shot()
    check("desktop 1 is Calendar's screen as it was, Notes gone", ndiff(band(cal), band(d1)) == 0, (ndiff(band(cal), band(d1)), where(band(cal), band(d1))))
    n = len(uart()); keys("meta_l", "tab"); time.sleep(0.8)
    check("Cmd+Tab on desktop 1 does not bring Notes over", "switch notes" not in uart()[n:] and "space 2" not in uart()[n:], uart()[n:][-300:])
    n = len(uart()); move_to(*slot_centre(4)); time.sleep(0.3); click()
    check("Notes' dock tile goes to desktop 2", wait_for("space 2", 40, n), uart()[n:][-300:])
    move_to(*PARK); time.sleep(0.6); back = shot()
    if os.environ.get("JT_SPACES_SHOTS"):
        for nm, im in (("notes", notes), ("back", back)): open("%s/%s.ppm" % (os.environ["JT_SPACES_SHOTS"], nm), "wb").write(b"P6\n%d %d\n255\n" % (W, H) + im)
    check("and the screen is Notes' as it was", ndiff(band(notes), band(back)) == 0, (ndiff(band(notes), band(back)), where(band(notes), band(back))))
    check("no fault, no crash", "KERNEL CRASH" not in uart() and "oom" not in uart(), uart()[-300:])
except SystemExit:
    pass
finally:
    q.terminate()
    try: q.wait(5)
    except subprocess.TimeoutExpired: q.kill()

if fails: print("FAIL: " + "; ".join(fails)); sys.exit(1)
print("PASS: Ctrl+Right and Ctrl+Left step between desktops, an app's window stays on its own (not drawn, not clickable, still running elsewhere), Cmd+Tab keeps to the desktop, and a dock tile goes to its window's desktop")
