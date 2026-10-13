#!/usr/bin/env python3
"""ARM64 Finder (3.14.0): user/finder.c runs on the ARM kernel, lists the card's JT/ folder, goes into a folder and back.

The plain kernel on QEMU's virt machine with a virtio keyboard and the check card (tools/checks/sdcard.py) as a virtio disk
holding JT/HELLO.TXT, JT/DOCUMENTS/{NOTE.TXT, SUB/INNER.TXT}, JT/MUSIC/ALPHA.WAV and JT/TRASH/. Through QMP:
  - F1 and "finder", Enter: the kernel starts Finder at EL0 in its own window ("finder: dir=. n=4": three folders and a file,
    folders first); on screen the sidebar and the first rows carry ink and the selected row is shaded;
  - Enter on Documents (DOCUME~1 on an 8.3 card, as mtools makes it) lists it (SUB first, then NOTE.TXT), Enter on SUB lists INNER.TXT, Backspace goes back up to Documents;
  - Enter on NOTE.TXT previews it read-only ("finder: preview=NOTE.TXT bytes=N"), the text is on screen, Backspace closes it;
  - Left and Down in the sidebar go to Music ("finder: dir=MUSIC n=1");
  - Esc closes it ("finder: closed", exit 0).
A second boot with no card: Spotlight's Burrow (the dock's tile, slot 1) opens Finder, which says "finder: builtin" and lists the built-in apps with a plain note.
No EL0 fault, no KERNEL CRASH. FINDER_SHOT=path.png keeps a screendump of the window of the list (needs sips or magick).
Skips (exit 0) when clang, ld.lld, qemu-system-aarch64 or mtools is missing.
Usage: tools/checks/arm64-finder-check.py   (from the repo root)
"""
import json, os, re, shutil, socket, subprocess, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from scratch import scratch_dir
from sdcard import PART, build_card

root = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
arch = os.path.join(root, "arch/arm64")
if not all(shutil.which(t) for t in ("clang", "ld.lld", "qemu-system-aarch64", "mmd", "mcopy")):
    print("SKIP: clang, ld.lld, qemu-system-aarch64 or mtools not installed"); sys.exit(0)
tmp = scratch_dir("finder")
fails = []
def check(name, ok, detail=""):
    print(("  ok: " if ok else "  FAIL: ") + name + ("" if ok or not detail else " -- " + str(detail)[-400:]))
    if not ok: fails.append(name)

subprocess.run(["make", "-C", arch, "clean"], capture_output=True)
r = subprocess.run(["make", "-C", arch, "kernel8.elf"], capture_output=True, text=True, timeout=900)
if r.returncode: print("FAIL: arch/arm64 `make kernel8.elf` does not build:\n" + (r.stdout + r.stderr)[-1500:]); sys.exit(1)

NOTE = b"A short note for the Finder preview.\nSecond line of the note.\n"
card = os.path.join(tmp, "card.img")
build_card(card, False)
MT = dict(os.environ, MTOOLS_SKIP_CHECK="1")
def mt(*a): return subprocess.run([a[0], "-i", f"{card}@@{PART * 512}", *a[1:]], capture_output=True, env=MT)
for name, blob in (("NOTE.TXT", NOTE), ("INNER.TXT", b"inner\n"), ("HELLO.TXT", b"hello\n"), ("ALPHA.WAV", b"RIFF" + bytes(600))):
    open(os.path.join(tmp, name), "wb").write(blob)
for args in (("mmd", "::JT"), ("mmd", "::JT/DOCUMENTS"), ("mmd", "::JT/DOCUMENTS/SUB"), ("mmd", "::JT/MUSIC"), ("mmd", "::JT/TRASH"),
             ("mcopy", os.path.join(tmp, "NOTE.TXT"), "::JT/DOCUMENTS/NOTE.TXT"), ("mcopy", os.path.join(tmp, "INNER.TXT"), "::JT/DOCUMENTS/SUB/INNER.TXT"),
             ("mcopy", os.path.join(tmp, "HELLO.TXT"), "::JT/HELLO.TXT"), ("mcopy", os.path.join(tmp, "ALPHA.WAV"), "::JT/MUSIC/ALPHA.WAV")):
    if mt(*args).returncode: print("FAIL: mtools could not write %r onto the check card" % (args,)); sys.exit(1)


class Boot:
    def __init__(self, disk):
        Boot.n = getattr(Boot, "n", 0) + 1
        self.log, sock = os.path.join(tmp, "uart%d" % Boot.n), os.path.join(tmp, "q%d" % Boot.n)
        args = ["qemu-system-aarch64", "-machine", "virt", "-cpu", "cortex-a72", "-m", "256", "-global", "virtio-mmio.force-legacy=false",
                "-nic", "none", "-device", "ramfb", "-device", "virtio-keyboard-device"]
        if disk: args += ["-drive", "if=none,id=card,file=%s,format=raw" % card, "-device", "virtio-blk-device,drive=card"]
        args += ["-display", "none", "-serial", "file:" + self.log, "-qmp", "unix:%s,server,nowait" % sock, "-kernel", os.path.join(arch, "kernel8.elf")]
        self.q = subprocess.Popen(args, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        if not self.wait_for("M2 input ready", 60) or (disk and not self.wait_for("sd fat32 partition at block 2048", 20)):
            raise SystemExit(fails.append("the desktop did not come up: %r" % self.uart()[-500:]))
        time.sleep(0.5)
        s_ = socket.socket(socket.AF_UNIX); s_.settimeout(20); s_.connect(sock); self.f = s_.makefile("rw"); self.f.readline()
        self.cmd("qmp_capabilities")
    def uart(self): return open(self.log, errors="replace").read() if os.path.exists(self.log) else ""
    def wait_for(self, text, secs, after=0):
        end = time.time() + secs
        while time.time() < end:
            if text in self.uart()[after:]: return True
            time.sleep(0.05)
        return False
    def cmd(self, c, **a):
        self.f.write(json.dumps({"execute": c, "arguments": a}) + "\n"); self.f.flush()
        while True:
            r = json.loads(self.f.readline())
            if "return" in r or "error" in r: return r
    def key(self, *qcodes): self.cmd("send-key", keys=[{"type": "qcode", "data": k} for k in qcodes]); time.sleep(0.15)
    def type(self, text):
        for k in text: self.key(k)
    def shot(self, keep=None):
        path = os.path.join(tmp, "shot%d.ppm" % time.monotonic_ns())
        self.cmd("screendump", filename=path)
        for _ in range(30):
            if os.path.exists(path) and os.path.getsize(path) >= 800 * 600 * 3: break
            time.sleep(0.1)
        if keep:
            os.makedirs(os.path.dirname(keep), exist_ok=True)
            if shutil.which("sips"): subprocess.run(["sips", "-s", "format", "png", path, "--out", keep], capture_output=True)
            elif shutil.which("magick"): subprocess.run(["magick", path, keep], capture_output=True)
        parts = open(path, "rb").read().split(b"\n", 3)
        return int(parts[1].split()[0]), parts[3]
    def close(self): self.q.kill(); self.q.wait()

def ink(img, x, y, w, h, thr=0x60): return sum(1 for j in range(y, y + h) for i in range(x, x + w) if max(img[1][(j * img[0] + i) * 3:(j * img[0] + i) * 3 + 3]) < thr)
def px(img, x, y): i = (y * img[0] + x) * 3; return tuple(img[1][i:i + 3])
def accent(img, x, y, w, h): return sum(1 for j in range(y, y + h) for i in range(x, x + w) if abs(px(img, i, j)[0] - 0xB5) < 12 and abs(px(img, i, j)[1] - 0x50) < 12 and abs(px(img, i, j)[2] - 0x2C) < 12)

def open_finder(b, word="finder"):
    n0 = len(b.uart())
    b.key("f1")
    if not b.wait_for("spotlight open", 5, n0): fails.append("F1 did not open Spotlight"); raise SystemExit
    b.type(word)
    check("Spotlight lists %s" % word, b.wait_for("spotlight: %s 1\n" % word, 5, n0), b.uart()[n0:][-300:])
    b.key("ret")
    return n0

b = None
try:
    b = Boot(True)
    n0 = open_finder(b)
    ok = b.wait_for("finder: dir=. n=4\n", 10, n0)
    out = b.uart()[n0:]
    check("Enter starts user/finder.c at EL0 in its own window and it lists JT/: three folders and a file", ok and "M3c app finder starts" in out, out[-500:])
    m = re.search(r"M3c window finder (\d+)x(\d+) at (\d+),(\d+)\n", out)
    if not m: fails.append("no 'M3c window finder' line"); raise SystemExit
    cw, ch, cx, cy = map(int, m.groups())
    b.wait_for("M3c window finder first frame", 5, n0); time.sleep(0.5)
    img = b.shot(os.environ.get("FINDER_SHOT"))
    rows = [ink(img, cx + 170, cy + 32 + i * 24, 120, 24) for i in range(5)]
    check("on screen, the four rows have ink in the name column and the fifth does not (%s)" % rows, all(v > 20 for v in rows[:4]) and rows[4] == 0, rows)
    check("the folder icons are the house accent (#b5502c)", accent(img, cx + 146, cy + 32, 20, 72) > 60, accent(img, cx + 146, cy + 32, 20, 72))
    check("the sidebar carries ink", ink(img, cx, cy, 130, 160) > 40, ink(img, cx, cy, 130, 160))
    check("the selected row is shaded (#ede6dc) and the one under it is not", px(img, cx + 300, cy + 32 + 3) == (0xed, 0xe6, 0xdc) and px(img, cx + 300, cy + 32 + 24 + 3) == (0xfa, 0xf8, 0xf6),
          (px(img, cx + 300, cy + 35), px(img, cx + 300, cy + 59)))
    n1 = len(b.uart()); b.key("ret")
    check("Enter on Documents (DOCUME~1 on an 8.3 card, as mtools makes it) lists it: a folder and a file", b.wait_for("finder: dir=DOCUME~1 n=2\n", 5, n1), b.uart()[n1:][-300:])
    n1 = len(b.uart()); b.key("ret")
    check("Enter on SUB (folders sort first) goes a level down", b.wait_for("finder: dir=DOCUME~1/SUB n=1\n", 5, n1), b.uart()[n1:][-300:])
    n1 = len(b.uart()); b.key("backspace")
    check("Backspace goes back up to Documents", b.wait_for("finder: dir=DOCUME~1 n=2\n", 5, n1), b.uart()[n1:][-300:])
    n1 = len(b.uart()); b.key("down"); b.key("ret")
    check("Enter on NOTE.TXT previews it read-only", b.wait_for("finder: preview=NOTE.TXT bytes=%d\n" % len(NOTE), 5, n1), b.uart()[n1:][-300:])
    time.sleep(0.4)
    img = b.shot()
    check("the note's text is on screen", ink(img, cx + 140, cy + 80, 340, 60) > 150, ink(img, cx + 140, cy + 80, 340, 60))
    n1 = len(b.uart()); b.key("backspace"); time.sleep(0.4)
    check("Backspace closes the preview and stays in Documents", "finder: up" not in b.uart()[n1:], b.uart()[n1:][-200:])
    n1 = len(b.uart()); b.key("left"); b.key("down")
    check("Left then Down in the sidebar goes to the next place after Home, Documents", b.wait_for("finder: dir=DOCUME~1 n=2\n", 5, n1), b.uart()[n1:][-200:])
    n1 = len(b.uart()); b.key("down")
    check("Down again lands on MUSIC with its one song", b.wait_for("finder: dir=MUSIC n=1\n", 5, n1), b.uart()[n1:][-300:])
    n1 = len(b.uart()); b.key("esc")
    check("Esc closes Finder with exit 0", b.wait_for("finder: closed\n", 5, n1) and b.wait_for("M3c app finder exit 0", 5, n1), b.uart()[n1:][-300:])
    after = b.uart().partition("M3c app finder starts")[2]
    for bad in ("M3 EL0 fault", "KERNEL CRASH", "M3c load FAIL"): check("%r never printed once Finder started" % bad, bad not in after, after[-300:])
    b.close(); b = None

    b = Boot(False)
    n0 = open_finder(b, "burrow")   # slot 1 is the dock's Burrow tile, the file window: it opens Finder too
    check("with no card Finder says it is showing what is built in", b.wait_for("finder: builtin\n", 10, n0), b.uart()[n0:][-300:])
    m = re.search(r"M3c window finder (\d+)x(\d+) at (\d+),(\d+)\n", b.uart()[n0:])
    if m:
        cw, ch, cx, cy = map(int, m.groups()); b.wait_for("M3c window finder first frame", 5, n0); time.sleep(0.5)
        img = b.shot()
        check("the built-in list and the note under it are drawn", ink(img, cx + 170, cy + 32, 120, 96) > 80 and ink(img, cx + 140, cy + ch - 20, 400, 18, 0xa0) > 40, "")
except SystemExit:
    pass
finally:
    if b: b.close()
    subprocess.run(["make", "-C", arch, "clean"], capture_output=True)
if fails:
    for msg in fails: print("FAIL: " + str(msg)[:400])
    sys.exit(1)
print("PASS: on the ARM kernel Spotlight opens user/finder.c, which lists the card's JT/ folder with flat icons, goes into a folder and back, "
      "previews a .TXT read-only, moves between sidebar places, and with no card says it shows the built-in apps")
