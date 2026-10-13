#!/usr/bin/env python3
"""The Pi's boot splash is a neutral grey screen with the tree mark crisp in the middle and a calm grey bar under it.

Joshua's photo of the real boot showed a terracotta screen and a dark blurry blob where the mark should be (the 80 px menu bar
copy stretched 2.75x, ink on orange). The splash now paints light grey 0xE5E5E7 and the 160 px stored mark in ink 0x1C1C1E over it.
Boots the -DQUIET_BOOT -DSPLASH_STEP_MS=1500 build headless on QEMU's virt machine with ramfb, takes a screendump while the splash
holds, and asserts on real pixels: the background is the grey, no pixel anywhere is terracotta (red well above blue), the mark's
box has many dark ink pixels, and its edges are crisp (many neighbouring pixels a strong brightness step apart).
Set SPLASH_OUT=/path.png to keep the screendump. Skips (exit 0) when clang, ld.lld or qemu-system-aarch64 is missing.
Usage: tools/checks/arm64-splash-check.py   (from the repo root)
"""
import json, os, shutil, socket, subprocess, sys, tempfile, time

root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
arch = os.path.join(root, "arch/arm64")
if not all(shutil.which(t) for t in ("clang", "ld.lld", "qemu-system-aarch64")):
    print("SKIP: clang, ld.lld or qemu-system-aarch64 not installed"); sys.exit(0)
GREY = (0xE5, 0xE5, 0xE7)
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
    from PIL import Image
    img = Image.frombytes("RGB", (W, H), d)
    out = os.environ.get("SPLASH_OUT")
    if out: os.makedirs(os.path.dirname(out), exist_ok=True); img.save(out)
    pts = [px(x, y) for y in range(0, H, 3) for x in range(0, W, 3)]
    grey = sum(1 for c in pts if c == GREY) / len(pts)
    terra = sum(1 for c in pts if c[0] > c[2] + 24)
    sc = H / 540; sz = int(150 * sc); cx, cy = W // 2, H // 2 - int(30 * sc)
    box = [px(x, y) for y in range(cy - sz // 2, cy + sz // 2) for x in range(cx - sz // 2, cx + sz // 2)]
    ink = sum(1 for c in box if max(c) < 0x50)
    hard = 0   # crisp edges: neighbouring pixels (right or below) that differ by a strong step in brightness
    for y in range(cy - sz // 2, cy + sz // 2 - 1):
        for x in range(cx - sz // 2, cx + sz // 2 - 1):
            v = max(px(x, y))
            if abs(v - max(px(x + 1, y))) >= 0x60 or abs(v - max(px(x, y + 1))) >= 0x60: hard += 1
    # Row by row against the stored art: every row that should carry ink must, so torn or missing rows (the Pi showed slivers) fail.
    import re
    src = open(os.path.join(root, "kernel", "boot_mark.h")).read()
    body = re.search(r"boot_mark_cov\[\d+\][^{]*\{(.*?)\};", src, re.S).group(1)
    cov = bytes(int(v, 0) for v in re.findall(r"0x[0-9a-fA-F]+|\d+", body))[:160 * 160]
    exp = Image.frombytes("L", (160, 160), cov).resize((sz, sz), Image.BILINEAR)
    ex = exp.load(); torn = 0; rows = 0; mism = 0; tot = 0
    for r in range(sz):
        want = [x for x in range(sz) if ex[x, r] >= 200]
        if len(want) < 2: continue
        rows += 1
        got = sum(1 for x in want if max(px(cx - sz // 2 + x, cy - sz // 2 + r)) < 0x60)
        tot += len(want); mism += len(want) - got
        if got < len(want) * 0.6: torn += 1
    if torn > rows // 25 or mism > tot * 0.25: fails.append(f"the mark does not match its art row by row: {torn} of {rows} ink rows torn, {mism} of {tot} ink pixels missing")
    if grey < 0.9: fails.append(f"background is not the grey ({grey:.0%} of samples are #E5E5E7)")
    if terra: fails.append(f"{terra} terracotta-ish samples on the splash")
    if ink < sz * sz // 14: fails.append(f"the mark is faint: only {ink} ink pixels in its {sz}px box")
    if hard < sz // 2: fails.append(f"the mark's edges are soft: {hard} dark/light steps")
    bw = int(140 * sc); bx = (W - bw) // 2; by = H // 2 + int(62 * sc)
    bar = [px(x, by) for x in range(bx, bx + bw)]
    if any(c[0] > c[2] + 24 for c in bar) or len(set(bar)) < 2: fails.append("the progress bar is missing or not grey")
    if not fails: print(f"  ok: grey {grey:.0%}, no terracotta, {ink} ink pixels, {hard} crisp edge steps, grey bar, every ink row of the art present ({rows} rows) at {W}x{H}")
finally:
    q.kill(); q.wait(); subprocess.run(["make", "-C", arch, "clean"], capture_output=True)
if fails:
    for m in fails: print("FAIL:", m)
    sys.exit(1)
print("PASS: the Pi boot splash is grey with a crisp ink tree mark and a grey bar, no terracotta")
