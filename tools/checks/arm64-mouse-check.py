#!/usr/bin/env python3
"""ARM64 desktop slice 4, the mouse: a USB mouse moves the i386 arrow over the ARM desktop, the dock's hover label
follows it, and clicks do something on screen.

QEMU's virt machine with ramfb, an xHCI controller behind a PCIe root port and a usb-hub, with a USB keyboard and a
USB boot mouse both behind the hub (no virtio input, so every event came through xhci.c's HID path). Through QMP
input-send-event it:
  - moves the mouse onto dock slot 3 (Calendar): the UART says "mouse X,Y" and "M1d hover 3 Calendar", and a
    screendump shows the arrow (black body, white outline) at the pointer and the pale hover capsule above the tile;
  - moves it off onto the wallpaper: the capsule is gone, the dock's band matches the untouched boot screen pixel for
    pixel (no ghost of the arrow left in the band's saved copy), and the arrow shows at the new spot;
  - clicks the Console's red close button: "console closed", and the wallpaper is back where the window was;
  - clicks the Terminal tile: "console open" and "dock Terminal: not on ARM yet", the window is back with its log;
  - plugs a second mouse into the hub after boot and moves it ("usb mouse ... port N.3", then a new "mouse X,Y").
Then the cursortest build on QEMU's Raspberry Pi 4B model at 1080p (window_scale 2): the arrow and the label over
slot 3. The cache cleaning a real board needs is invisible to QEMU, so the source is checked for it.
Skips (exit 0) when clang's aarch64 target, ld.lld or qemu-system-aarch64 is missing.
Usage: tools/checks/arm64-mouse-check.py   (from the repo root)
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

# The board-only half: every arrow move and the close button clean what they drew out of the data cache.
main_c = open(os.path.join(arch, "main.c")).read()
def body(name):   # the definition, not a forward declaration
    m = re.search(r"\n(static )?void %s\([^)]*\) \{" % name, main_c)
    return main_c[m.start():main_c.find("\n}\n", m.start())] if m else ""
for fn, call in (("cur_flush", "fb_flush("), ("cur_show", "cur_flush("), ("cur_hide", "cur_flush("), ("console_close", "fb_flush("), ("console_open", "fb_flush(")):
    if call not in body(fn): fails.append(f"main.c {fn}() no longer cleans the cache over what it drew: a real Pi will not show it")
if "cur_hide();" not in body("dock_hover"):
    fails.append("main.c dock_hover() no longer takes the arrow off before copying the dock's band")
if not fails: print("  ok: the arrow, the close button and the reopen clean the cache; dock_hover takes the arrow off first")

# Geometry: the same math as kernel/dock_geom.c and fb_init, at 800x600 (scale 1).
def geom(w, h):
    s = 2 if h >= 1080 else 1; lw, lh = w // s, h // s
    icon = max(16, min(lh * 7 // 100, (min(740, lw - 40) - 2 * 10 - 10 * 6) // 11))
    dock_w = 11 * icon + 10 * 6 + 2 * 10; x0 = (lw - dock_w) // 2; y0 = lh - icon - 2 * 10 - 24
    return s, icon, x0, y0
def slot_centre(w, h, slot):
    s, icon, x0, y0 = geom(w, h)
    return (x0 + 10 + slot * (icon + 6) + icon // 2) * s, (y0 + 10 + icon // 2) * s

def screendump(cmd, tmp):
    shot = tmp + "/shot%d.ppm" % time.monotonic_ns()
    r = cmd("screendump", filename=shot)
    if "error" in r: return None
    time.sleep(0.3)
    parts = open(shot, "rb").read().split(b"\n", 3); w, h = map(int, parts[1].split())
    return w, h, parts[3]
def px(img, x, y):
    w, h, b = img; i = (y * w + x) * 3
    return tuple(b[i:i + 3])
def arrow(img, x, y, s):   # black body and white outline pixels in the arrow's box, its tip at physical (x, y)
    box = [px(img, x + i, y + j) for j in range(19 * s) for i in range(13 * s) if x + i < img[0] and y + j < img[1]]
    return sum(1 for c in box if max(c) < 24), sum(1 for c in box if min(c) > 240)
def capsule(img, slot):
    w, h, _ = img; s, icon, x0, y0 = geom(w, h)
    cx, ly = x0 + 10 + slot * (icon + 6) + icon // 2, y0 - 21
    return [px(img, (cx + d) * s, (ly + 1) * s) for d in range(-10, 11, 2)]
LABEL = (0xf4, 0xf1, 0xec)

tmp = tempfile.mkdtemp()
log, sock = tmp + "/uart", tmp + "/qmp"
q = subprocess.Popen(["qemu-system-aarch64", "-machine", "virt", "-cpu", "cortex-a72", "-m", "256", "-nic", "none", "-device", "ramfb",
                      "-device", "pcie-root-port,id=rp,chassis=1", "-device", "qemu-xhci,id=x,bus=rp",
                      "-device", "usb-hub,id=h,bus=x.0,port=1",
                      "-device", "usb-kbd,bus=x.0,port=1.1", "-device", "usb-mouse,id=m1,bus=x.0,port=1.2",
                      "-display", "none", "-serial", "file:" + log, "-qmp", "unix:%s,server,nowait" % sock,
                      "-kernel", os.path.join(arch, "kernel8.elf")], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
def uart(): return open(log, errors="replace").read() if os.path.exists(log) else ""
def wait_for(text, tries=100):
    for _ in range(tries):
        if text in uart(): return True
        time.sleep(0.1)
    return False
try:
    if not wait_for("usb ready: 1 kbd, 1 mouse", 300):
        fails.append(f"the keyboard and mouse behind the hub did not come up, got {uart()[-1200:]!r}")
    else:
        if re.search(r"usb mouse addr \d+ port \d+\.2 ", uart()) and re.search(r"usb kbd addr \d+ port \d+\.1 ", uart()):
            print("  ok: a USB keyboard and a USB boot mouse, both behind the hub")
        else: fails.append("the keyboard and mouse were not both found behind the hub")
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
        pos = [W // 2, H // 2]
        def move_to(x, y, device=None):
            while (pos[0], pos[1]) != (x, y):
                dx = max(-100, min(100, x - pos[0])); dy = max(-100, min(100, y - pos[1]))
                ev = [{"type": "rel", "data": {"axis": "x", "value": dx}}, {"type": "rel", "data": {"axis": "y", "value": dy}}]
                cmd("input-send-event", events=ev, **({"device": device} if device else {}))
                pos[0] += dx; pos[1] += dy
                if not wait_for(f"mouse {pos[0]},{pos[1]}\n", 40): return False
            return True
        def click():
            cmd("input-send-event", events=[{"type": "btn", "data": {"down": True, "button": "left"}}])
            cmd("input-send-event", events=[{"type": "btn", "data": {"down": False, "button": "left"}}])
        b0, w0 = arrow(base, *slot_centre(W, H, 3), 1)
        cx, cy = slot_centre(W, H, 3)
        if not move_to(cx, cy): fails.append(f"moving onto slot 3: no 'mouse {cx},{cy}' on the UART, got {uart()[-300:]!r}")
        elif not wait_for("M1d hover 3 Calendar", 30): fails.append("the pointer over slot 3 printed no 'M1d hover 3 Calendar'")
        else:
            img = screendump(cmd, tmp)
            b, wh = arrow(img, cx, cy, 1)
            if b < 25 + b0 or wh < 15: fails.append(f"no arrow at the pointer over slot 3: {b} black (boot screen {b0}), {wh} white pixels")
            else: print(f"  ok: the arrow is drawn at {cx},{cy}: {b} black body, {wh} white outline pixels")
            cap = capsule(img, 3)
            if any(c != LABEL for c in cap): fails.append(f"no hover label capsule over slot 3 when the pointer is on it: {cap}")
            else: print("  ok: the hover label follows the pointer onto slot 3 (Calendar)")
        # off the dock, onto bare wallpaper right of the window: label gone, the band exactly as at boot, arrow at the new spot
        ox, oy = W - 60, H // 3
        if not move_to(ox, oy): fails.append(f"moving off the dock: no 'mouse {ox},{oy}', got {uart()[-300:]!r}")
        else:
            time.sleep(0.3)
            img = screendump(cmd, tmp)
            s, icon, x0, y0 = geom(W, H); top = (y0 - 24) * s
            diff = sum(1 for y in range(top, H) for x in range(0, W, 1) if px(img, x, y) != px(base, x, y))
            if any(c == LABEL for c in capsule(img, 3)): fails.append("the hover label stayed after the pointer left the dock")
            elif diff: fails.append(f"the dock's band differs from the boot screen in {diff} pixels after the pointer left: a ghost of the arrow or the label")
            else: print("  ok: off the dock the label is gone and the band is the boot screen, pixel for pixel")
            b, wh = arrow(img, ox, oy, 1); bb, _ = arrow(base, ox, oy, 1)
            if b < 25 + bb: fails.append(f"no arrow at the new spot {ox},{oy} ({b} black pixels)")
            else: print(f"  ok: the arrow moved to {ox},{oy}")
        # the Console's red close button: gui_draw_window_frame's dot at (x + 24, y + 16) on the logical grid
        win_w, win_h = 500 * H // 600, 350 * H // 600; wx, wy = (W - win_w) // 2, 100 * H // 600
        if not move_to(wx + 24, wy + 16): fails.append("could not move onto the close button")
        else:
            click()
            if not wait_for("console closed", 30): fails.append(f"clicking the close button printed no 'console closed', got {uart()[-300:]!r}")
            else:
                img = screendump(cmd, tmp)
                well = [px(img, x, y) for y in range(wy + 60, wy + win_h - 20, 7) for x in range(wx + 30, wx + win_w - 30, 7)]
                white = sum(1 for c in well if c == (255, 255, 255))
                if white * 10 > len(well) or len(set(well)) < 100: fails.append(f"the window is still there after closing ({white} of {len(well)} white, {len(set(well))} colours)")
                else: print(f"  ok: closing the Console puts the wallpaper back ({len(set(well))} colours where the window was)")
        tx, ty = slot_centre(W, H, 6)
        if not move_to(tx, ty): fails.append("could not move onto the Terminal tile")
        else:
            click()
            if not (wait_for("console open", 30) and wait_for("dock Terminal: not on ARM yet", 30)):
                fails.append(f"clicking the Terminal tile did not reopen the Console, got {uart()[-300:]!r}")
            else:
                time.sleep(0.3)
                img = screendump(cmd, tmp)
                if px(img, W // 2, (wy // 1 + win_h - 8) - 2) != (255, 255, 255) or px(img, wx + 19, wy + 16) != (0xff, 0x5f, 0x57):
                    fails.append("the reopened Console has no white well or no red close button")
                else:
                    well = [px(img, x, y) for y in range(wy + 34, wy + win_h - 14) for x in range(wx + 8, wx + win_w - 4)]
                    ink = sum(1 for c in well if max(c) < 0x90)
                    if ink < 300: fails.append(f"the reopened Console shows no log ({ink} ink pixels)")
                    else: print(f"  ok: a dock click reopens the Console with its log ({ink} ink pixels) and prints 'dock Terminal: not on ARM yet'")
        # hot-plug: a second mouse on a free hub port, found by the once-a-second rescan, and it moves the same pointer
        r = cmd("device_add", driver="usb-mouse", bus="x.0", port="1.3", id="m2")
        if "error" in r: fails.append(f"device_add of a second mouse failed: {r}")
        elif not any(re.search(r"usb mouse addr \d+ port \d+\.3 ", uart()) or time.sleep(0.1) for _ in range(80)):
            fails.append(f"a mouse plugged into the hub after boot was never found, got {uart()[-300:]!r}")
        else:
            time.sleep(0.5)
            if move_to(pos[0] - 40, pos[1] - 40): print("  ok: a mouse plugged in after boot is found and moves the pointer")   # the newest mouse takes the events print("  ok: a mouse plugged in after boot is found and moves the pointer")
            else: fails.append(f"the hot-plugged mouse did not move the pointer, got {uart()[-300:]!r}")
            cmd("device_del", id="m2")
            if not wait_for(".3 disconnected", 60): fails.append("unplugging the second mouse was not noticed")
        if "KERNEL CRASH" in uart(): fails.append("the kernel crashed")
finally:
    q.kill(); q.wait()

# The Pi 4B model has no USB in QEMU, so the cursortest build puts the pointer over slot 3 at boot: the scale-2 arrow at 1080p.
if "raspi4b" in subprocess.run(["qemu-system-aarch64", "-machine", "help"], capture_output=True, text=True).stdout:
    if subprocess.run(["make", "-C", arch, "cursortest"], capture_output=True).returncode: fails.append("`make cursortest` does not build")
    else:
        log2, sock2 = tmp + "/uart2", tmp + "/qmp2"
        q = subprocess.Popen(["qemu-system-aarch64", "-machine", "raspi4b", "-global", "bcm2835-fb.xres=1920", "-global", "bcm2835-fb.yres=1080",
                              "-display", "none", "-serial", "file:" + log2, "-qmp", "unix:%s,server,nowait" % sock2,
                              "-kernel", os.path.join(arch, "cursor-kernel8.img")], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        try:
            out = ""
            for _ in range(200):
                time.sleep(0.1)
                out = open(log2, errors="replace").read() if os.path.exists(log2) else ""
                if "M1c" in out: break
            if "M1c fb ok" not in out or "M1d hover 3 Calendar" not in out: fails.append(f"pi 1080p cursortest: no 'M1c fb ok' and 'M1d hover 3 Calendar', got {out[-300:]!r}")
            else:
                s_ = socket.socket(socket.AF_UNIX); s_.connect(sock2); f = s_.makefile("rw"); f.readline()
                def cmd(c, **a):
                    f.write(json.dumps({"execute": c, "arguments": a}) + "\n"); f.flush()
                    while True:
                        r = json.loads(f.readline())
                        if "return" in r or "error" in r: return r
                cmd("qmp_capabilities")
                img = screendump(cmd, tmp)
                if (img[0], img[1]) != (1920, 1080): fails.append(f"pi cursortest: screen is {img[0]}x{img[1]}")
                else:
                    cx, cy = slot_centre(1920, 1080, 3)
                    b, wh = arrow(img, cx, cy, 2)
                    if b < 100 or wh < 60: fails.append(f"pi 1080p: no scale-2 arrow over slot 3 ({b} black, {wh} white pixels)")
                    elif any(c != LABEL for c in capsule(img, 3)): fails.append("pi 1080p: no hover label under the arrow over slot 3")
                    else: print(f"  ok: pi 1080p, the arrow at scale 2 ({b} black, {wh} white pixels) with the label over slot 3")
        finally:
            q.kill(); q.wait()
else:
    print("  QEMU here has no raspi4b model (needs QEMU 9 or newer), the 1080p arrow skipped")
shutil.rmtree(tmp, ignore_errors=True)
subprocess.run(["make", "-C", arch, "clean"], capture_output=True)
for m in fails: print("FAIL: " + m)
if fails: sys.exit(1)
print("PASS: a USB mouse behind the hub moves the arrow over the ARM desktop, the dock label follows it, the close button and a dock click work, and a mouse plugged in later moves it too")
