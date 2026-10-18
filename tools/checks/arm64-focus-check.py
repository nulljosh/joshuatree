#!/usr/bin/env python3
"""ARM64 desktop slice 5, typing into apps: two windows on screen, a click focuses one, and the keys land in it.

QEMU's virt machine with ramfb and a USB keyboard and a USB boot mouse behind a hub (the arm64 mouse check's setup,
so every event comes through xhci.c's HID path). Through QMP it:
  - clicks the Terminal tile, then opens the Calculator from Spotlight (F1, "calc", Enter; the Apps tile is the Launchpad since 3.8.2): it opens over the right of the Terminal, both on screen;
  - types 9-2 and Enter: the UART says "calc: 9-2 = 7" and the Terminal's prompt row did not change a pixel;
  - clicks the Terminal's uncovered body: "focus terminal", and the Terminal is drawn over the Calculator's corner;
  - types 123: the prompt row gains ink, and nothing reaches the Calculator;
  - clicks the Calculator's title band: "focus calc", it is on top again, and Enter prints "calc: 9-2 = 7" once more,
    with no 123 in it (with the old routing, the keys went to any open Calculator);
  - clicks the Calculator's red dot: "calc closed", and its rectangle beside the Terminal is the boot wallpaper again.
Skips (exit 0) when clang's aarch64 target, ld.lld or qemu-system-aarch64 is missing.
Usage: tools/checks/arm64-focus-check.py   (from the repo root)
"""
import json, os, re, shutil, socket, subprocess, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from scratch import scratch_dir

root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
arch = os.path.join(root, "arch/arm64")
if not all(shutil.which(t) for t in ("clang", "ld.lld", "qemu-system-aarch64")):
    print("SKIP: clang, ld.lld or qemu-system-aarch64 not installed"); sys.exit(0)
subprocess.run(["make", "-C", arch, "clean"], capture_output=True)
if subprocess.run(["make", "-C", arch], capture_output=True).returncode:
    print("FAIL: arch/arm64 `make` does not build"); sys.exit(1)
fails = []

# Geometry at 800x600 (scale 1), the same math as kernel/dock_geom.c and fb_init.
W, H = 800, 600
icon = max(16, min(H * 7 // 100, (min(740, W - 40) - 2 * 10 - 10 * 6) // 11))
dock_x0, dock_y0 = (W - (11 * icon + 10 * 6 + 2 * 10)) // 2, H - icon - 2 * 10 - 24
def slot_centre(slot): return dock_x0 + 10 + slot * (icon + 6) + icon // 2, dock_y0 + 10 + icon // 2
wx, wy, ww, wh = (W - 500) // 2, 100, 500, 350                    # the Console's and the Terminal's frame
cx, cy, cw = wx + ww - 150, wy + 40, 280                            # the Calculator's
ch = min(300, dock_y0 - 24 - 22 - cy)
prompt = (wx + 8, wy + 32 + ((wh - 46) // 16 - 1) * 16, cx - 4 - (wx + 8), 16)  # the Terminal's prompt row, the part left of the Calculator
CREAM, WHITE = (0xf5, 0xf0, 0xeb), (255, 255, 255)

tmp = scratch_dir("arm64-focus")
log, sock = tmp + "/uart", tmp + "/qmp"
q = subprocess.Popen(["qemu-system-aarch64", "-machine", "virt", "-cpu", "cortex-a72", "-m", "256", "-nic", "none", "-device", "ramfb",
                      "-device", "pcie-root-port,id=rp,chassis=1", "-device", "qemu-xhci,id=x,bus=rp",
                      "-device", "usb-hub,id=h,bus=x.0,port=1",
                      "-device", "usb-kbd,bus=x.0,port=1.1", "-device", "usb-mouse,id=m1,bus=x.0,port=1.2",
                      "-display", "none", "-serial", "file:" + log, "-qmp", "unix:%s,server,nowait" % sock,
                      "-kernel", os.path.join(arch, "kernel8.elf")], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
def uart(): return open(log, errors="replace").read() if os.path.exists(log) else ""
def wait_for(pred, tries=60):
    for _ in range(tries):
        if pred(uart()): return True
        time.sleep(0.1)
    return False
def has(text, after=0): return lambda u: text in u[after:]
try:
    if not wait_for(has("usb ready: 1 kbd, 1 mouse"), 300):
        fails.append(f"the keyboard and mouse behind the hub did not come up, got {uart()[-600:]!r}"); raise SystemExit
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
    def ink(img, box): return sum(1 for c in region(img, *box) if max(c) < 0x90)
    base = shot()
    pos = [W // 2, H // 2]
    def move_to(x, y):
        while (pos[0], pos[1]) != (x, y):
            dx = max(-100, min(100, x - pos[0])); dy = max(-100, min(100, y - pos[1]))
            cmd("input-send-event", events=[{"type": "rel", "data": {"axis": "x", "value": dx}}, {"type": "rel", "data": {"axis": "y", "value": dy}}])
            pos[0] += dx; pos[1] += dy
            if not wait_for(has(f"mouse {pos[0]},{pos[1]}\n"), 40): return False
        return True
    def click(x, y, want):
        n = len(uart())
        if not move_to(x, y): fails.append(f"could not move the pointer to {x},{y}"); return False
        cmd("input-send-event", events=[{"type": "btn", "data": {"down": True, "button": "left"}}])
        cmd("input-send-event", events=[{"type": "btn", "data": {"down": False, "button": "left"}}])
        if not wait_for(has(want, n), 40): fails.append(f"a click at {x},{y} printed no {want!r}, got {uart()[n:][-300:]!r}"); return False
        time.sleep(0.3); return True
    def keys(*qcodes):
        for k in qcodes: cmd("send-key", keys=[{"type": "qcode", "data": k}]); time.sleep(0.15)
    def calc_lines(): return re.findall(r"calc: (.*) = (.*)\n", uart())

    def spotlight_calc():
        n = len(uart()); keys("f1")
        if not wait_for(has("spotlight open", n), 30): return False
        keys("c", "a", "l", "c", "ret"); return wait_for(has("calc open", n), 30)
    if click(*slot_centre(6), "terminal open") and spotlight_calc():
        a = shot()
        if px(a, wx + 19, wy + 16) != (0xff, 0x5f, 0x57) or px(a, cx + 19, cy + 16) != (0xff, 0x5f, 0x57):
            fails.append("the Terminal and the Calculator are not both on screen with their red dots")
        else: print("  ok: the Terminal and the Calculator are both on screen, the Calculator over the Terminal's right")
        keys("9", "minus", "2", "ret")
        if not wait_for(lambda u: ("9-2", "7") in calc_lines(), 30): fails.append(f"typing 9-2 Enter into the Calculator printed no 'calc: 9-2 = 7', got {uart()[-300:]!r}")
        else:
            b = shot()
            if region(a, *prompt) != region(b, *prompt): fails.append("keys for the Calculator changed the Terminal's prompt row")
            else: print("  ok: with the Calculator in front, 9-2 Enter gives 'calc: 9-2 = 7' and the Terminal's prompt row is untouched")
            if click(wx + 150, wy + 150, "focus terminal"):
                c = shot()
                if px(c, cx + 90, cy + 15) != WHITE: fails.append(f"after the click the Terminal is not over the Calculator's corner ({px(c, cx + 90, cy + 15)})")
                else: print("  ok: a click on the Terminal's body focuses it and draws it over the Calculator's corner")
                before = ink(c, prompt)
                n = len(uart())
                keys("1", "2", "3")
                if not wait_for(has("key 4 up", n), 30): fails.append("the 3 key never reached the kernel")
                d = shot()
                for _ in range(20):
                    if ink(d, prompt) >= before + 40: break
                    time.sleep(0.1); d = shot()
                if ink(d, prompt) < before + 40: fails.append(f"typing 123 with the Terminal in front put no text on its prompt row ({before} then {ink(d, prompt)} ink pixels)")
                else: print(f"  ok: 123 lands on the Terminal's prompt row ({before} then {ink(d, prompt)} ink pixels)")
                if click(cx + 200, cy + 15, "focus calc"):
                    e = shot()
                    if px(e, cx + 90, cy + 15) != CREAM: fails.append("after the click the Calculator's title band is not back on top")
                    n = len(calc_lines())
                    keys("ret")
                    if not wait_for(lambda u: len(calc_lines()) > n, 30): fails.append("Enter with the Calculator focused again printed no calc line")
                    else:
                        line = calc_lines()[-1]
                        if "123" in line[0] or line != ("9-2", "7"): fails.append(f"the Terminal's keys reached the Calculator: 'calc: {line[0]} = {line[1]}'")
                        else: print("  ok: a click on the Calculator's title focuses it again, and its sum is still 9-2 = 7: none of 123 reached it")
                    if click(cx + 24, cy + 16, "calc closed"):
                        g = shot()
                        side = (wx + ww + 6, cy + 6, cx + cw - (wx + ww) - 12, ch - 12)   # the Calculator's part right of the Terminal
                        if region(g, *side) != region(base, *side): fails.append("closing the Calculator did not put the boot wallpaper back beside the Terminal")
                        elif px(g, cx + 90, cy + 60) != WHITE: fails.append("closing the Calculator did not leave the Terminal whole")
                        else: print("  ok: the red dot closes the Calculator: the wallpaper is back pixel for pixel and the Terminal is whole")
    if "KERNEL CRASH" in uart(): fails.append("the kernel crashed")
except SystemExit:
    pass
finally:
    q.kill(); q.wait()
subprocess.run(["make", "-C", arch, "clean"], capture_output=True)
for m in fails: print("FAIL: " + m)
if fails: sys.exit(1)
print("PASS: a click focuses the Terminal or the Calculator, and typed keys land only in the window in front")
