#!/usr/bin/env python3
"""The mascot on the empty Music and Books screens of the ARM64 desktop (user/libjt/mascot.h, drawn by user/music.c and user/books.c).

Boots the virt build with a keyboard and the check card, which has no JT/MUSIC songs and no JT/BOOKS books. Spotlight (F1, the
letters, Enter) opens each app in turn; for each one:
  1. the app logs an empty library (`music: library n=0`, `books: books=0`);
  2. a screendump of its window has the mascot's ink and terracotta (warm and dark pixels where the mascot's lines and ground fall) above the caption, where
     the old screen was bare off-white and grey words, and the caption itself has ink under the mascot.
A revert of the draw calls (or of the art) leaves the warm block empty and this check fails. Esc closes each app.
Skips (exit 0) when clang's aarch64 target, ld.lld, qemu-system-aarch64 or mtools is missing.
Usage: tools/checks/arm64-mascot-empty-check.py   (from the repo root)
"""
import json, os, re, shutil, socket, subprocess, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from scratch import scratch_dir
from sdcard import build_card

root = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
arch = os.path.join(root, "arch/arm64")
if not all(shutil.which(t) for t in ("clang", "ld.lld", "qemu-system-aarch64")):
    print("SKIP: clang, ld.lld or qemu-system-aarch64 not installed"); sys.exit(0)
fails = []
def check(name, ok, detail=""):
    print(("  ok: " if ok else "  FAIL: ") + name + ("" if ok or not detail else "  (" + str(detail)[-600:] + ")"))
    if not ok: fails.append(name)

tmp = scratch_dir("arm64-mascot-empty")
subprocess.run(["make", "-C", arch, "clean"], capture_output=True)
r = subprocess.run(["make", "-C", arch, "kernel8.elf"], capture_output=True, text=True, timeout=900)
if r.returncode: print("FAIL: arch/arm64 `make kernel8.elf` does not build:\n" + (r.stdout + r.stderr)[-1500:]); sys.exit(1)

card = os.path.join(tmp, "card.img")
build_card(card, False)
log, sock = os.path.join(tmp, "uart"), os.path.join(tmp, "qmp")
q = subprocess.Popen(["qemu-system-aarch64", "-machine", "virt", "-cpu", "cortex-a72", "-m", "256", "-global", "virtio-mmio.force-legacy=false",
                      "-nic", "none", "-device", "ramfb", "-device", "virtio-keyboard-device",
                      "-drive", "if=none,id=card,file=%s,format=raw" % card, "-device", "virtio-blk-device,drive=card",
                      "-display", "none", "-serial", "file:" + log, "-qmp", "unix:%s,server,nowait" % sock,
                      "-kernel", os.path.join(arch, "kernel8.elf")], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
def uart(): return open(log, errors="replace").read() if os.path.exists(log) else ""
def wait_re(pat, secs, after=0):
    end = time.time() + secs
    while time.time() < end:
        m = re.search(pat, uart()[after:])
        if m: return m
        time.sleep(0.05)
    return None
def wait_for(text, secs, after=0): return wait_re(re.escape(text), secs, after)
try:
    if not wait_for("M2 input ready", 60) or not wait_for("sd fat32 partition at block 2048", 20):
        fails.append("the desktop or the card did not come up: %r" % uart()[-600:]); raise SystemExit
    time.sleep(0.5)
    s_ = socket.socket(socket.AF_UNIX); s_.settimeout(20); s_.connect(sock); f = s_.makefile("rw"); f.readline()
    def cmd(c, **a):
        f.write(json.dumps({"execute": c, "arguments": a}) + "\n"); f.flush()
        while True:
            r = json.loads(f.readline())
            if "return" in r or "error" in r: return r
    cmd("qmp_capabilities")
    def key(*qcodes): cmd("send-key", keys=[{"type": "qcode", "data": k} for k in qcodes]); time.sleep(0.15)
    def shot():
        path = os.path.join(tmp, "shot%d.ppm" % time.monotonic_ns())
        cmd("screendump", filename=path)
        for _ in range(30):
            if os.path.exists(path) and os.path.getsize(path) >= 800 * 600 * 3: break
            time.sleep(0.1)
        parts = open(path, "rb").read().split(b"\n", 3)
        return int(parts[1].split()[0]), parts[3]
    def count(img, x, y, w, h, test):
        n = 0
        for j in range(y, y + h):
            for i in range(x, x + w):
                o = (j * img[0] + i) * 3
                if test(*img[1][o:o + 3]): n += 1
        return n
    warm = lambda r, g, b: r > 140 and r - g > 40 and r - b > 50   # the mascot's terracotta and its warm tints, never the grey or ink words
    dark = lambda r, g, b: max(r, g, b) < 0x90                     # caption ink

    for app, lib, area in (("music", "music: library n=0\n", "left pane"), ("books", "books: books=0\n", "page")):
        n0 = len(uart())
        key("f1")
        if not wait_for("spotlight open", 5, n0): fails.append("F1 did not open Spotlight for %s" % app); break
        for k in app: key(k)
        key("ret")
        m = wait_re(r"M3c window %s (\d+)x(\d+) at (\d+),(\d+)\n" % app, 10, n0)
        check("%s: Enter starts it at EL0 in its own window" % app, m, uart()[n0:][-400:])
        if not m: break
        cw, ch, cx, cy = map(int, m.groups())
        check("%s: it reports an empty library" % app, wait_for(lib, 10, n0), uart()[n0:][-400:])
        wait_for("M3c window %s first frame" % app, 5, n0); time.sleep(0.6)
        img = shot()
        if app == "music":   # the library pane: mascot 80 px at LIST_Y 46, caption at LIST_Y + 92
            lw = min(270, max(150, cw // 3))
            mx, my, mw, mh = cx + 10, cy + 46, lw - 4, 84
            cap = (cx + 20, cy + 46 + 92, 130, 22)
        else:                # the page: mascot 96 px centred at LIST_Y 64, caption at LIST_Y + 108
            mx, my, mw, mh = cx + cw // 2 - 50, cy + 64, 100, 100
            cap = (cx + cw // 2 - 60, cy + 64 + 108, 120, 22)
        w_, k_ = count(img, mx, my, mw, mh, warm), count(img, mx, my, mw, mh, dark)
        check("%s: the mascot (its terracotta ground line and its ink, %d warm and %d dark pixels) is in the %s above the caption" % (app, w_, k_, area), w_ > 60 and k_ > 150, (w_, k_))
        i_ = count(img, *cap, dark)
        check("%s: the caption sits under it (%d ink pixels)" % (app, i_), i_ > 20, i_)
        n1 = len(uart())
        key("esc")
        check("%s: Esc closes it" % app, wait_for("M3c app %s exit 0" % app, 6, n1), uart()[n1:][-300:])
        time.sleep(0.5)
    u = uart().partition("M3c app music starts")[2]   # the boot's own EL0 probes fault on purpose
    for bad in ("M3 EL0 fault", "KERNEL CRASH", "oom", "M3c load FAIL"):
        check("%r never printed once Music started" % bad, bad not in u, u[-300:])
except SystemExit:
    pass
finally:
    q.kill(); q.wait()
    subprocess.run(["make", "-C", arch, "clean"], capture_output=True)
if fails:
    for msg in fails: print("FAIL: " + str(msg)[:400])
    sys.exit(1)
print("PASS: on the ARM kernel an empty Music and an empty Books each draw the mascot's terracotta above a plain caption")
