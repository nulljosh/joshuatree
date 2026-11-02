#!/usr/bin/env python3
"""The Pi's boot splash is a neutral grey screen with the flat tree glyph in one ink in the middle and a calm grey bar under it.

Joshua's photo of the real boot once showed a terracotta screen and a dark blurry blob where the mark should be. The splash paints
light grey 0xE5E5E7 and, since 7.10, the glyph (arch/arm64/splash_mark.h, from docs/brand/glyph.png) in ink 0x1C1C1E, Apple style.
Boots the -DQUIET_BOOT -DSPLASH_STEP_MS=1500 build headless on QEMU's virt machine with ramfb, takes a screendump while the splash
holds, and asserts on real pixels: the background is the grey, nothing outside the mark's box is terracotta, and the box matches
the glyph inked on the grey at that size (mean colour difference) with no colour in it, so a torn, missing, blurred or painted mark fails.
Set SPLASH_OUT=/path.png to keep the screendump. Skips (exit 0) when clang, ld.lld or qemu-system-aarch64 is missing.
Usage: tools/checks/arm64-splash-check.py   (from the repo root)
"""
import json, os, shutil, socket, subprocess, sys, tempfile, time
from PIL import Image

root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
arch = os.path.join(root, "arch/arm64")
if not all(shutil.which(t) for t in ("clang", "ld.lld", "qemu-system-aarch64")):
    print("SKIP: clang, ld.lld or qemu-system-aarch64 not installed"); sys.exit(0)
GREY, INK = (0xE5, 0xE5, 0xE7), (0x1C, 0x1C, 0x1E)
MARK, LIFT = 100, 24   # desk.c splash_begin: the mark is sg(100) square, centred sg(24) above the middle
fails = []
tmp = tempfile.mkdtemp(prefix="jt-splash-")
subprocess.run(["make", "-C", arch, "clean"], capture_output=True)
if subprocess.run(["make", "-C", arch, "MAINFLAGS=-DQUIET_BOOT -DSPLASH_STEP_MS=1500 -DWIFI_ICON_TEST=0"], capture_output=True).returncode:
    print("FAIL: arch/arm64 splash build does not build"); sys.exit(1)
log, sock = tmp + "/uart", tmp + "/qmp"
q = subprocess.Popen(["qemu-system-aarch64", "-machine", "virt", "-cpu", "cortex-a72", "-m", "256", "-nic", "none", "-device", "ramfb",
                      "-display", "none", "-serial", "file:" + log, "-qmp", "unix:%s,server,nowait" % sock,
                      "-kernel", os.path.join(arch, "kernel8.elf")], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
try:
    def uart(): return open(log, errors="replace").read() if os.path.exists(log) else ""
    for _ in range(300):
        if "splash step 3 " in uart(): break
        time.sleep(0.1)
    else:
        print("FAIL: no splash, got %r" % uart()[-400:]); sys.exit(1)
    s = socket.socket(socket.AF_UNIX)
    for _ in range(50):
        try: s.connect(sock); break
        except OSError: time.sleep(0.2)
    f = s.makefile("rw"); f.readline()
    def cmd(c, **a):
        f.write(json.dumps({"execute": c, "arguments": a}) + "\n"); f.flush()
        while True:
            r = json.loads(f.readline())
            if "return" in r or "error" in r: return r
    cmd("qmp_capabilities")
    shot = tmp + "/s.ppm"; cmd("screendump", filename=shot); time.sleep(0.5)
    parts = open(shot, "rb").read().split(b"\n", 3); W, H = map(int, parts[1].split()); d = parts[3]
    def px(x, y): i = (y * W + x) * 3; return tuple(d[i:i + 3])
    img = Image.frombytes("RGB", (W, H), d)
    out = os.environ.get("SPLASH_OUT")
    if out: os.makedirs(os.path.dirname(out), exist_ok=True); img.save(out)
    sc = H / 540; sz = int(MARK * sc); cx, cy = W // 2, H // 2 - int(LIFT * sc)
    bx0, by0 = cx - sz // 2, cy - sz // 2
    inbox = lambda x, y: bx0 <= x < bx0 + sz and by0 <= y < by0 + sz
    pts = [px(x, y) for y in range(0, H, 3) for x in range(0, W, 3) if not inbox(x, y)]
    grey = sum(1 for c in pts if c == GREY) / len(pts)
    terra = sum(1 for c in pts if c[0] > c[2] + 24)
    def painted(n):   # the glyph's coverage inked on the grey at n px, what the splash should show
        a = Image.open(os.path.join(root, "docs", "brand", "glyph.png")).convert("RGBA").getchannel("A").resize((n, n), Image.BOX)
        return Image.composite(Image.new("RGB", (n, n), INK), Image.new("RGB", (n, n), GREY), a)
    want = painted(sz).load()
    diff = sum(abs(a - b) for y in range(sz) for x in range(sz) for a, b in zip(px(bx0 + x, by0 + y), want[x, y])) / (sz * sz * 3)
    colour = sum(1 for y in range(sz) for x in range(sz) if max(px(bx0 + x, by0 + y)) - min(px(bx0 + x, by0 + y)) > 8)
    inked = sum(1 for y in range(sz) for x in range(sz) if px(bx0 + x, by0 + y)[0] < 0x60)
    if grey < 0.9: fails.append(f"background is not the grey ({grey:.0%} of samples are #E5E5E7)")
    if terra: fails.append(f"{terra} terracotta-ish samples outside the mark")
    if diff > 4: fails.append(f"the mark is not the glyph: mean difference {diff:.1f} per channel from it inked on the grey (4 allowed)")
    if colour: fails.append(f"the mark is not flat: {colour} pixels in it carry colour, the glyph is one ink")
    if inked < sz * sz // 10: fails.append(f"the mark is missing: only {inked} ink pixels in its box")
    bw = int(140 * sc); bx = (W - bw) // 2; by = H // 2 + int(62 * sc)
    bar = [px(x, by) for x in range(bx, bx + bw)]
    if any(c[0] > c[2] + 24 for c in bar) or len(set(bar)) < 2: fails.append("the progress bar is missing or not grey")
    if not fails: print(f"  ok: grey {grey:.0%}, no terracotta outside the mark, the flat glyph within {diff:.1f} per channel, {inked} ink pixels, grey bar, at {W}x{H}")
finally:
    q.kill(); q.wait(); subprocess.run(["make", "-C", arch, "clean"], capture_output=True)
# 7.5.4: sharp at 1080p, where the mark is 200 px; the 400 px copy is only ever box-filtered down.
if not fails:
    subprocess.run(["make", "-C", arch, "clean"], capture_output=True)
    if subprocess.run(["make", "-C", arch, "MAINFLAGS=-DQUIET_BOOT -DSPLASH_STEP_MS=1500 -DWIFI_ICON_TEST=0 -DFB_BOOT_W=1920 -DFB_BOOT_H=1080"], capture_output=True).returncode:
        fails.append("the 1080p splash build does not build")
    else:
        log2, sock2 = tmp + "/uart2", tmp + "/qmp2"
        q = subprocess.Popen(["qemu-system-aarch64", "-machine", "virt", "-cpu", "cortex-a72", "-m", "256", "-nic", "none", "-device", "ramfb",
                              "-display", "none", "-serial", "file:" + log2, "-qmp", "unix:%s,server,nowait" % sock2,
                              "-kernel", os.path.join(arch, "kernel8.elf")], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        try:
            for _ in range(300):
                if os.path.exists(log2) and "splash step 3 " in open(log2, errors="replace").read(): break
                time.sleep(0.1)
            s2 = socket.socket(socket.AF_UNIX)
            for _ in range(50):
                try: s2.connect(sock2); break
                except OSError: time.sleep(0.2)
            f2 = s2.makefile("rw"); f2.readline()
            def cmd2(c, **a):
                f2.write(json.dumps({"execute": c, "arguments": a}) + "\n"); f2.flush()
                while True:
                    r = json.loads(f2.readline())
                    if "return" in r or "error" in r: return r
            cmd2("qmp_capabilities"); shot2 = tmp + "/s2.ppm"; cmd2("screendump", filename=shot2); time.sleep(0.5)
            img2 = Image.open(shot2).convert("RGB"); W2, H2 = img2.size
            sz2 = int(MARK * H2 / 540); cx2, cy2 = W2 // 2, H2 // 2 - int(LIFT * H2 / 540)
            box2 = list(img2.crop((cx2 - sz2 // 2, cy2 - sz2 // 2, cx2 - sz2 // 2 + sz2, cy2 - sz2 // 2 + sz2)).tobytes())
            ref = list(painted(sz2).tobytes())
            diff = sum(abs(a - b) for a, b in zip(box2, ref)) / len(ref)
            if (W2, H2) != (1920, 1080): fails.append(f"the 1080p splash build came up at {W2}x{H2}")
            elif diff > 4.5: fails.append(f"the 1080p splash mark is soft: mean difference {diff:.1f} per channel from the glyph at {sz2} px (4.5 allowed)")
            else: print(f"  ok: at 1920x1080 the {sz2} px glyph is within {diff:.1f} per channel of the source")
        finally:
            q.kill(); q.wait(); subprocess.run(["make", "-C", arch, "clean"], capture_output=True)
if fails:
    for m in fails: print("FAIL:", m)
    sys.exit(1)
print("PASS: the Pi boot splash is grey with the flat ink glyph, sharp at 800x600 and 1080p, and a grey bar")
