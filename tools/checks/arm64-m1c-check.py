#!/usr/bin/env python3
"""ARM64 M1c: the aarch64 kernel gets a framebuffer, draws a desktop into it and mirrors its boot log in the window in smooth DejaVu text, two ways. On QEMU's virt machine
through ramfb; on QEMU's Raspberry Pi 4B model through the VideoCore mailbox, the same call a real Pi answers.

Boots each build, waits for "M1c fb ok" on the UART, then asks QEMU for a screendump over QMP and checks real pixels:
menu bar, desktop, window, accent title bar, dock and the tribute line above it. On the Pi model it also fakes a 1080p
and a 4K monitor and checks the desktop fills a 1920x1080 screen. This proves what the screen shows, not just what the
kernel believes. The cache cleaning a real board needs is invisible to QEMU, so that part is checked in the source.
Skips (exit 0) when clang's aarch64 target, ld.lld or qemu-system-aarch64 is missing.
Usage: tools/checks/arm64-m1c-check.py   (from the repo root)
"""
import json, os, shutil, socket, subprocess, sys, tempfile, time

root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
arch = os.path.join(root, "arch/arm64")
if not all(shutil.which(t) for t in ("clang", "ld.lld", "qemu-system-aarch64")):
    print("SKIP: clang, ld.lld or qemu-system-aarch64 not installed"); sys.exit(0)
subprocess.run(["make", "-C", arch, "clean"], capture_output=True)
if subprocess.run(["make", "-C", arch], capture_output=True).returncode:
    print("FAIL: arch/arm64 `make` does not build"); sys.exit(1)

fails = []

# The board-only half: QEMU has no cache, so a missing cache clean can never show up in a screendump. On the real Pi 4
# it did (2.12.5): the console's page wipe ran after the only clean, and the screen kept just the two pixel columns of
# old text left of a cache line boundary. So the source itself is checked: every console draw and wipe cleans what it
# touched, and the boot code finds its stack and .bss PC-relative, wherever the firmware loaded it.
main_c = open(os.path.join(arch, "main.c")).read()
def body(name):
    i = main_c.find("static void %s(" % name); j = main_c.find("\n}\n", i)
    return main_c[i:j] if i >= 0 else ""
for fn in ("con_glyph", "con_wipe"):
    if "fb_flush(" not in body(fn): fails.append(f"main.c {fn}() no longer cleans the cache over what it drew: a real Pi will not show it")
    else: print(f"  ok: {fn}() cleans the cache over what it drew")
start_s = open(os.path.join(arch, "start.S")).read()
if "ldr x0, =" in start_s or "ldr x1, =" in start_s: fails.append("start.S loads an absolute address: wrong if the firmware loads us anywhere but the link address")
elif "msr s3_3_c4_c4_0, xzr" not in start_s: fails.append("start.S no longer zeroes FPCR")
else: print("  ok: start.S is PC-relative and zeroes FPCR")
if "kernel_address=0x80000" not in open(os.path.join(root, "tools/pi-config.txt")).read():
    fails.append("tools/pi-config.txt lost kernel_address=0x80000")

def shoot(name, qemu_args, image, size=(800, 600)):
    tmp = tempfile.mkdtemp()
    log, sock, shot = tmp + "/uart", tmp + "/qmp", tmp + "/shot.ppm"
    q = subprocess.Popen(["qemu-system-aarch64", *qemu_args, "-display", "none", "-serial", "file:" + log,
                          "-qmp", "unix:%s,server,nowait" % sock, "-kernel", os.path.join(arch, image)],
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        out = ""
        for _ in range(150):
            time.sleep(0.1)
            out = open(log, errors="replace").read() if os.path.exists(log) else ""
            if "M1c" in out: break
        if "M1c fb ok" not in out:
            fails.append(f"{name}: kernel did not report M1c fb ok, got {out!r}"); return
        s = socket.socket(socket.AF_UNIX); s.connect(sock); f = s.makefile("rw")
        f.readline()
        def cmd(c, **a):
            f.write(json.dumps({"execute": c, "arguments": a}) + "\n"); f.flush()
            while True:
                r = json.loads(f.readline())
                if "return" in r or "error" in r: return r
        cmd("qmp_capabilities")
        r = cmd("screendump", filename=shot)
        if "error" in r: fails.append(f"{name}: screendump failed: {r['error']}"); return
        time.sleep(0.3)
        parts = open(shot, "rb").read().split(b"\n", 3); w, h = map(int, parts[1].split()); px = parts[3]
        if (w, h) != size: fails.append(f"{name}: screen is {w}x{h}, want {size[0]}x{size[1]}"); return
        sc = lambda v: v * h // 600                                  # the kernel's own layout rule: an 800x600 design scaled by height
        win_w, win_h = sc(500), sc(350); win_x, win_y = (w - win_w) // 2, sc(100); dock_y = h - sc(60)
        win = [tuple(px[(y * w + x) * 3:(y * w + x) * 3 + 3]) for y in range(win_y + sc(32), win_y + win_h - sc(14)) for x in range(win_x + sc(8), win_x + win_w - sc(4))]
        ink = sum(1 for c in win if max(c) < 0x90)                  # dark text pixels
        soft = sum(1 for c in win if 0x30 < c[0] < 0xd0 and c[0] == c[1] == c[2])   # grey edge pixels: only smooth, anti-aliased text has them
        if ink < 300: fails.append(f"{name}: the boot log is not on screen (only {ink} text pixels in the window)")
        elif soft < 150: fails.append(f"{name}: the text is not anti-aliased ({soft} soft edge pixels), the smooth font is not drawing")
        else: print(f"  ok: {name} boot log drawn in the window ({ink} text pixels, {soft} soft edge pixels: smooth text)")
        band = [tuple(px[(y * w + x) * 3:(y * w + x) * 3 + 3]) for y in range(dock_y - sc(30), dock_y) for x in range(w // 2 - sc(150), w // 2 + sc(150))]
        light = sum(1 for c in band if c[0] > 0x70)
        if light < 40: fails.append(f"{name}: no tribute line above the dock ({light} light pixels)")
        else: print(f"  ok: {name} tribute line above the dock ({light} light pixels)")
        want = [("menu bar", (w // 2, sc(10)), (0xe0, 0xe0, 0xe0)), ("desktop", (50, sc(300)), (0x20, 0x30, 0x40)),
                ("window", (w // 2, win_y + win_h - 4), (0xff, 0xff, 0xff)), ("title bar", (w // 2, win_y + sc(10)), (0xb5, 0x50, 0x2c)),
                ("dock", (w // 2, h - sc(40)), (0x50, 0x5a, 0x68))]
        for what, (x, y), c in want:
            i = (y * w + x) * 3; got = tuple(px[i:i + 3])
            if got != c: fails.append(f"{name}: {what} at {(x, y)}: got {got}, want {c}")
            else: print(f"  ok: {name} {what} pixel {c}")
    finally:
        q.kill(); q.wait()
        shutil.rmtree(tmp, ignore_errors=True)

shoot("virt ramfb", ["-machine", "virt", "-cpu", "cortex-a72", "-m", "256", "-device", "ramfb"], "kernel8.elf")
if "raspi4b" in subprocess.run(["qemu-system-aarch64", "-machine", "help"], capture_output=True, text=True).stdout:
    if subprocess.run(["make", "-C", arch, "pi"], capture_output=True).returncode: fails.append("pi: `make pi` does not build")
    else:
        shoot("pi mailbox", ["-machine", "raspi4b"], "kernel8.img")   # QEMU's model reports a 640x480 monitor: too small, stays 800x600
        # A 1080p monitor gets a 1080p desktop; a 4K one gets exactly half, so the firmware's 2x scale stays sharp.
        shoot("pi 1080p", ["-machine", "raspi4b", "-global", "bcm2835-fb.xres=1920", "-global", "bcm2835-fb.yres=1080"], "kernel8.img", (1920, 1080))
        shoot("pi 4K", ["-machine", "raspi4b", "-global", "bcm2835-fb.xres=3840", "-global", "bcm2835-fb.yres=2160"], "kernel8.img", (1920, 1080))
else:
    print("  QEMU here has no raspi4b model (needs QEMU 9 or newer), Pi screen skipped")
subprocess.run(["make", "-C", arch, "clean"], capture_output=True)
for m in fails: print("FAIL: " + m)
if fails: sys.exit(1)
print("PASS: the aarch64 kernel draws its desktop through ramfb on virt and through the mailbox on the Pi 4B model, at 800x600 and at the monitor's own size, and QEMU's own screendump shows it with the right colors")
