#!/usr/bin/env python3
"""Movies on the Pi (6.1.0): the unchanged user/movies.c, built for EL0, plays a motion-JPEG AVI from the card.

QEMU virt, headless, a check card (sdcard.py) whose JT/ holds CLIP.AVI (tools/media/avigen.py --movie: 2 s at 20 fps, each
frame's number drawn as six black or white squares) and NOTAVI.AVI (text, not a clip):
  1. Spotlight finds Movies and Enter opens it; the picker lists both files;
  2. Enter on CLIP.AVI plays it: `movie=play CLIP.AVI frames=40`, frame lines that never go backwards, and
     `movie=done frames=40`; virt has no sound card, so the app's 100 Hz tick is the clock;
  3. a screendump mid-clip holds the frame code's pure black squares, which the desktop never draws;
  4. NOTAVI.AVI shows an error line, not a crash, and the desktop is still up (Esc closes the app, Spotlight opens).
  5. LONG.AVI is CLIP.AVI with 6.5 MB of padding spliced into its movi list after frame 19, so frames 20 to 39 sit past
     the 6 MB that SYS_READFILE holds: it plays all 40 frames, streamed from the card with SYS_READAT (fat_read_at).

Discriminating: drop `movies` from UAPPS in arch/arm64/Makefile and step 1 fails.

Usage: tools/checks/arm64-movies-check.py   (from the repo root)
"""
import json, os, re, shutil, socket, subprocess, sys, time
from pathlib import Path
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from scratch import scratch_dir

ROOT = Path(__file__).resolve().parents[2]
os.chdir(ROOT)
TMP = Path(scratch_dir("movies"))
fails = []
def check(name, ok, detail=""):
    print(("  ok: " if ok else "  FAIL: ") + name + (f" ({detail})" if detail and not ok else ""))
    if not ok: fails.append(name)

if not all(shutil.which(t) for t in ("clang", "ld.lld", "qemu-system-aarch64", "mmd", "mcopy")):
    print("SKIP: no aarch64 toolchain or mtools"); sys.exit(0)
from sdcard import PART, build_card
subprocess.run(["make", "-s", "-C", "arch/arm64", "kernel8.elf"], check=True)
fx = TMP / "fx"; subprocess.run([sys.executable, "tools/media/avigen.py", str(fx), "--movie"], check=True, capture_output=True)
card = str(TMP / "card.img"); build_card(card, False)
MT = dict(os.environ, MTOOLS_SKIP_CHECK="1")
def mt(*a): return subprocess.run([a[0], "-i", f"{card}@@{PART * 512}", *a[1:]], capture_output=True, env=MT)
mt("mmd", "::JT")
mt("mcopy", str(fx / "CLIP.AVI"), "::JT/CLIP.AVI")
import struct
clip = (fx / "CLIP.AVI").read_bytes(); m = clip.index(b"movi") + 4; pos, frames = m, 0
while frames < 20:                                   # after the 20th video chunk (and the sound after it)
    cid, size = clip[pos:pos + 4], struct.unpack("<I", clip[pos + 4:pos + 8])[0]
    pos += 8 + size + (size & 1)
    if cid == b"01wb": frames += 1
pad = b"JUNK" + struct.pack("<I", 6_500_000) + b"\0" * 6_500_000
long = bytearray(clip[:pos] + pad + clip[pos:])
struct.pack_into("<I", long, 4, len(long) - 8)                                   # RIFF size
struct.pack_into("<I", long, m - 8, struct.unpack("<I", long[m - 8:m - 4])[0] + len(pad))   # movi LIST size
(TMP / "long").write_bytes(bytes(long)); mt("mcopy", str(TMP / "long"), "::JT/LONG.AVI")
(TMP / "notavi").write_text("this is not a movie\n" * 40); mt("mcopy", str(TMP / "notavi"), "::JT/NOTAVI.AVI")

log, sock = TMP / "uart", TMP / "qmp"
q = subprocess.Popen(["qemu-system-aarch64", "-machine", "virt", "-cpu", "cortex-a72", "-m", "256", "-global", "virtio-mmio.force-legacy=false",
                      "-nic", "none", "-device", "ramfb", "-device", "virtio-keyboard-device",
                      "-drive", "if=none,id=card,file=%s,format=raw" % card, "-device", "virtio-blk-device,drive=card",
                      "-display", "none", "-serial", "file:%s" % log, "-qmp", "unix:%s,server,nowait" % sock,
                      "-kernel", "arch/arm64/kernel8.elf"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
uart = lambda: log.read_text(errors="replace") if log.exists() else ""
def wait(text, secs, after=0):
    for _ in range(int(secs * 20)):
        if text in uart()[after:]: return True
        time.sleep(0.05)
    return False
try:
    if not wait("M2 input ready", 90): check("the desktop came up", False, uart()[-300:]); raise SystemExit
    s = None
    for _ in range(100):
        try: s = socket.socket(socket.AF_UNIX); s.settimeout(30); s.connect(str(sock)); break
        except OSError: s = None; time.sleep(0.05)
    f = s.makefile("rw"); f.readline()
    def cmd(c, **a):
        f.write(json.dumps({"execute": c, **({"arguments": a} if a else {})}) + "\n"); f.flush()
        while True:
            r = json.loads(f.readline())
            if "return" in r or "error" in r: return r
    cmd("qmp_capabilities")
    def key(*k): cmd("send-key", keys=[{"type": "qcode", "data": x} for x in k], **{"hold-time": 60}); time.sleep(0.15)
    def shot(name):
        p = TMP / (name + ".ppm"); cmd("screendump", filename=str(p))
        for _ in range(40):
            if p.exists() and p.stat().st_size > 1000: time.sleep(0.2); break
            time.sleep(0.05)
        d = p.read_bytes(); parts = d.split(b"\n", 3); return parts[3]
    def bw(px):   # pixels that are exactly black and exactly white
        b = w = 0
        for i in range(0, len(px) - 2, 3):
            t = px[i:i + 3]
            if t == b"\x00\x00\x00": b += 1
            elif t == b"\xff\xff\xff": w += 1
        return b, w   # ponytail: a byte scan of one screendump, fast enough at 800 x 600
    time.sleep(1.0)
    b0, w0 = bw(shot("desk"))
    n = len(uart())
    key("meta_l", "spc"); time.sleep(0.4)
    for c in "movies": key(c)
    key("ret")
    check("Spotlight opens Movies and the picker lists the three files", wait("movie=list", 20, n) and re.search(r"movie=list \S* n=3", uart()[n:]) is not None, uart()[n:][-300:])
    sel = lambda: (re.findall(r"movie=select (\S+)", uart()) or [None])[-1]
    for _ in range(4):
        if sel() == "CLIP.AVI": break
        key("down" if sel() == "BAD" else "up"); time.sleep(0.3)
    n = len(uart()); key("ret")
    check("Enter plays CLIP.AVI (40 frames)", wait("movie=play CLIP.AVI frames=40", 15, n), uart()[n:][-300:])
    time.sleep(0.8)
    b1, w1 = bw(shot("playing"))
    check("the frame code's black squares are on the screen (the desktop draws no pure black)", b0 == 0 and b1 > 1500, (b0, b1))
    check("the clip ends: movie=done frames=40", wait("movie=done frames=40", 20, n), uart()[n:][-300:])
    frames = [int(x) for x in re.findall(r"movie=frame (\d+) ", uart()[n:])]
    check("frames never go backwards and reach the end", frames and frames == sorted(frames) and frames[-1] >= 30, frames[-8:])
    key("esc"); time.sleep(0.5)                               # the player goes back to the list
    n = len(uart())
    for _ in range(4):
        if sel() == "NOTAVI.AVI": break
        key("down"); time.sleep(0.3)
    key("ret")
    check("a file that is not a clip shows an error, not a crash", wait("movie=error", 10, n) and "KERNEL CRASH" not in uart() and "fault" not in uart()[n:].lower(), uart()[n:][-300:])
    key("ret"); time.sleep(0.5)                          # the error page goes back to the list
    for _ in range(4):
        if sel() == "LONG.AVI": break
        key("up"); time.sleep(0.3)
    n = len(uart()); key("ret")
    check("LONG.AVI (6.6 MB) plays: all 40 frames, the second half read from past 6 MB", wait("movie=play LONG.AVI frames=40", 20, n) and wait("movie=done frames=40", 30, n) and "movie=error" not in uart()[n:], uart()[n:][-300:])
    late = [int(x) for x in re.findall(r"movie=frame (\d+) ", uart()[n:])]
    check("frames past the padding are shown", any(f >= 25 for f in late), late[-8:])
    key("esc"); time.sleep(0.5)
    n = len(uart()); key("esc"); time.sleep(0.5); key("esc"); time.sleep(0.5)
    key("meta_l", "spc")
    check("the desktop is still up (Spotlight opens again)", wait("spotlight open", 10, n), uart()[n:][-300:])
except SystemExit:
    pass
finally:
    q.terminate()
    try: q.wait(5)
    except subprocess.TimeoutExpired: q.kill()

if fails: print("FAIL: " + "; ".join(fails)); sys.exit(1)
print("PASS: Movies runs on the Pi: Spotlight opens it, a motion-JPEG AVI plays all 40 frames in order and is drawn, a clip over 6 MB streams from the card past the old limit, a file that is not a clip is refused, and the desktop survives")
