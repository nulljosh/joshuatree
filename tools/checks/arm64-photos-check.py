#!/usr/bin/env python3
"""Photos on the Pi (12.0, user/photos.c): the PHOTOS folder as a grid, one full size, and edits kept beside the photos.

The card (sdcard.py) gets JT/PHOTOS/A.PNG (64x48: the top-left quarter pure red, the rest pure green) and B.PNG (all
blue). Two boots of the virt build with a keyboard, at 800x600:
  1. Spotlight opens Photos: `photos=list n=2`, and the grid shows A's red. Enter: `photos=view A.PNG 64x48`, the red
     in the top-left of the picture. r: `rot=1`, "photos=saved", and the red is now top-right (a quarter clockwise).
     Right shows B, Left brings A back. b and +: `rot=1 bw=1 light=20`, no pure red left, and the old red is grey 96
     (red's 76 plus 20). After QEMU stops, JT/PHOTOEDT.TXT holds "A.PNG 1 1 20", and A.PNG on the card is unchanged.
  2. Second boot: A opens turned and grey, from the file. 0 reverts: `rot=0 bw=0 light=0`, red top-left again.
Discriminating: make sample() in photos.c ignore rot and the red stays top-left after r.
Skips (exit 0) when clang's aarch64 target, ld.lld, qemu-system-aarch64 or mtools is missing.
Usage: tools/checks/arm64-photos-check.py   (from the repo root)
"""
import json, os, shutil, socket, struct, subprocess, sys, time, zlib
from pathlib import Path

root = Path(__file__).resolve().parents[2]
arch = root / "arch/arm64"
sys.path.insert(0, str(Path(__file__).resolve().parent))
from scratch import scratch_dir
from sdcard import PART, build_card
if not all(shutil.which(t) for t in ("clang", "ld.lld", "qemu-system-aarch64", "mtype", "mcopy", "mmd")):
    print("SKIP: clang, ld.lld, qemu-system-aarch64 or mtools not installed"); sys.exit(0)
tmp = Path(scratch_dir("arm64-photos"))

def png(path, w, h, colour):   # colour(x, y) -> (r, g, b); 8-bit RGB, filter 0 on every row
    rows = b"".join(b"\0" + b"".join(bytes(colour(x, y)) for x in range(w)) for y in range(h))
    chunk = lambda t, d: struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    path.write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) +
                     chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b""))
RED, GREEN, BLUE = (255, 0, 0), (0, 255, 0), (0, 0, 255)
png(tmp / "A.PNG", 64, 48, lambda x, y: RED if x < 32 and y < 24 else GREEN)
png(tmp / "B.PNG", 64, 48, lambda x, y: BLUE)

fails = []
def check(name, ok, detail=""):
    print(("  ok: " if ok else "  FAIL: ") + name + ("" if ok else "  " + repr(detail)[-500:]))
    if not ok: fails.append(name)

card = tmp / "card.img"
build_card(card, False)
MT = dict(os.environ, MTOOLS_SKIP_CHECK="1")
def mt(*a): return subprocess.run([a[0], "-i", f"{card}@@{PART * 512}", *a[1:]], capture_output=True, env=MT)
for d in ("::JT", "::JT/PHOTOS"): mt("mmd", d)
for f in ("A.PNG", "B.PNG"): mt("mcopy", str(tmp / f), "::JT/PHOTOS/" + f)
original = (tmp / "A.PNG").read_bytes()

subprocess.run(["make", "-C", str(arch), "clean"], capture_output=True, timeout=60)
r = subprocess.run(["make", "-C", str(arch), "kernel8.elf"], capture_output=True, text=True, timeout=600)
if r.returncode: print(r.stderr[-2000:]); print("FAIL: arch/arm64 does not build"); sys.exit(1)

def where(img, rgb):   # count and centre of the pixels exactly rgb
    w, h, d = img; want = bytes(rgb); xs = ys = n = 0
    for i in range(0, len(d), 3):
        if d[i:i + 3] == want: p = i // 3; xs += p % w; ys += p // w; n += 1
    return (n, xs / n, ys / n) if n else (0, 0, 0)
def picture(img):   # the box around the photo's own colours, whichever it shows
    w, h, d = img; box = [w, h, -1, -1]
    keep = {bytes(c) for c in (RED, GREEN, (96, 96, 96), (169, 169, 169))}
    for i in range(0, len(d), 3):
        if d[i:i + 3] in keep:
            p = i // 3; x, y = p % w, p // w
            box = [min(box[0], x), min(box[1], y), max(box[2], x), max(box[3], y)]
    return ((box[0] + box[2]) / 2, (box[1] + box[3]) / 2) if box[2] >= 0 else (0, 0)

def boot(steps):
    log, sock = tmp / "uart", tmp / "qmp"
    log.unlink(missing_ok=True)
    q = subprocess.Popen(["qemu-system-aarch64", "-machine", "virt", "-cpu", "cortex-a72", "-m", "256",
                          "-global", "virtio-mmio.force-legacy=false", "-device", "ramfb", "-nic", "none",
                          "-drive", "if=none,id=card,file=%s,format=raw" % card, "-device", "virtio-blk-device,drive=card",
                          "-device", "virtio-keyboard-device", "-device", "virtio-tablet-device", "-display", "none",
                          "-serial", "file:" + str(log), "-qmp", "unix:%s,server,nowait" % sock,
                          "-kernel", str(arch / "kernel8.elf")], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        def uart(): return log.read_text(errors="replace") if log.exists() else ""
        def wait_for(text, secs, after=0):
            end = time.time() + secs
            while time.time() < end:
                if text in uart()[after:]: return True
                time.sleep(0.1)
            return False
        if not wait_for("M2 input ready", 40): check("the virt build boots", False, uart()[-600:]); return
        time.sleep(0.5)
        s = socket.socket(socket.AF_UNIX); s.settimeout(30); s.connect(str(sock)); f = s.makefile("rw"); f.readline()
        def command(name, args=None):
            f.write(json.dumps({"execute": name, **({"arguments": args} if args else {})}) + "\n"); f.flush()
            while True:
                reply = json.loads(f.readline())
                if "error" in reply: raise SystemExit("FAIL: QMP " + repr(reply))
                if "return" in reply: return reply["return"]
        def key(*names):
            command("send-key", {"keys": [{"type": "qcode", "data": k} for k in names], "hold-time": 50}); time.sleep(0.2)
        def press(k, want, secs=10):
            n = len(uart()); key(*k) if isinstance(k, tuple) else key(k); ok = wait_for(want, secs, n); time.sleep(0.4)
            return ok, uart()[n:][-400:]
        shots = [0]
        def shot():
            shots[0] += 1; path = tmp / ("shot%d.ppm" % shots[0])
            command("screendump", {"filename": str(path)})
            for _ in range(40):
                if path.exists() and path.stat().st_size >= 800 * 600 * 3: break
                time.sleep(0.1)
            parts = path.read_bytes().split(b"\n", 3); w, h = map(int, parts[1].split()); return (w, h, parts[3])
        command("qmp_capabilities")
        n = len(uart()); key("f1")
        if not wait_for("spotlight open", 10, n): check("F1 opens Spotlight", False, uart()[n:][-300:]); return
        for c in "photos": key(c)
        steps(press, shot, uart, wait_for, check, len(uart()))
    finally:
        q.terminate()
        try: q.wait(5)
        except subprocess.TimeoutExpired: q.kill()

def first(press, shot, uart, wait_for, check, n0):
    ok, out = press("ret", "photos=list n=2", 20)
    check("Spotlight opens Photos and it lists the two photos", ok, out)
    time.sleep(0.6)
    check("the grid shows A's red", where(shot(), RED)[0] > 50)
    ok, out = press("ret", "photos=view A.PNG 64x48")
    check("Enter opens A full size", ok, out)
    img = shot(); n, rx, ry = where(img, RED); cx, cy = picture(img)
    check("the red is the picture's top-left quarter", n > 1000 and rx < cx and ry < cy, (n, rx, ry, cx, cy))
    ok, out = press("r", "photos=edit A.PNG rot=1 bw=0 light=0")
    check("r turns it a quarter clockwise and saves", ok and wait_for("photos=saved", 5), out)
    img = shot(); n, rx, ry = where(img, RED); cx, cy = picture(img)
    check("the red is now top-right", n > 1000 and rx > cx and ry < cy, (n, rx, ry, cx, cy))
    check("Right shows B", *press("right", "photos=view B.PNG"))
    check("Left brings A back", *press("left", "photos=view A.PNG"))
    press("b", "photos=edit A.PNG rot=1 bw=1 light=0")
    ok, out = press("equal", "photos=edit A.PNG rot=1 bw=1 light=20")
    check("b and + make it black and white and 20 lighter", ok, out)
    img = shot(); grey = where(img, (96, 96, 96))
    check("no pure red is left, and the old red is grey 96", where(img, RED)[0] == 0 and grey[0] > 1000, (where(img, RED)[0], grey[0]))
    press("esc", "")
    check("Esc on the grid closes Photos", *press("esc", "photos: closed"))

def second(press, shot, uart, wait_for, check, n0):
    press("ret", "photos=list n=2", 20)
    ok, out = press("ret", "photos=view A.PNG 64x48")
    time.sleep(0.4)
    img = shot(); grey = where(img, (96, 96, 96)); cx, cy = picture(img)
    check("after a restart A opens turned and grey, from the file", ok and grey[0] > 1000 and grey[1] > cx and grey[2] < cy, (out, grey, cx, cy))
    ok, out = press("0", "photos=edit A.PNG rot=0 bw=0 light=0")
    img = shot(); n, rx, ry = where(img, RED); cx, cy = picture(img)
    check("0 reverts: red in the top-left again", ok and n > 1000 and rx < cx and ry < cy, (out, n, rx, ry, cx, cy))

boot(first)
edits = mt("mtype", "::JT/PHOTOEDT.TXT").stdout.decode(errors="replace")
check("JT/PHOTOEDT.TXT holds A's edits", edits == "A.PNG 1 1 20\n", edits)
mt("mcopy", "-o", "::JT/PHOTOS/A.PNG", str(tmp / "A.back"))
check("A.PNG on the card is unchanged", (tmp / "A.back").exists() and (tmp / "A.back").read_bytes() == original)
boot(second)

if fails: print("FAIL: " + "; ".join(fails)); sys.exit(1)
print("PASS: Photos lists the PHOTOS folder, opens one full size, turns it, makes it black and white and lighter, keeps the edits in PHOTOEDT.TXT across a restart and never rewrites the photo")
