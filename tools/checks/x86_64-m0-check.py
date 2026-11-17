#!/usr/bin/env python3
"""x86-64, slice 1 (13.18.0): the 64-bit kernel boots in qemu-system-x86_64 and says so on the serial port and the screen.

`make -C arch/x86_64` links kernel64.bin, a flat multiboot image. The serial log must say, in order: multiboot ok, long
mode ok with EFER 0x500 (LME and LMA), 64-bit math ok, paging ok, screen ok 1024x768, done. A QMP screendump must be
1024x768, the calm blue #3B6EA5 away from the text, with white text where the lines are drawn (more than 2000 white
pixels in the text block, none in the bottom half).

Discriminating: the same image on a CPU without long mode (-cpu pentium3 under qemu-system-x86_64) prints
"JT64 no long mode" and none of the ok lines, so the checks read the CPU's real state and not a constant.
Skips (exit 0) without clang, ld.lld or qemu-system-x86_64.
"""
import json, os, shutil, socket, subprocess, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from scratch import scratch_dir
root = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
arch = os.path.join(root, "arch/x86_64")
if not all(shutil.which(t) for t in ("clang", "ld.lld", "qemu-system-x86_64")):
    print("SKIP: clang, ld.lld or qemu-system-x86_64 not installed"); sys.exit(0)
tmp = scratch_dir("x86_64-m0")
fails = []
def check(name, ok, detail=""):
    print(("  ok: " if ok else "  FAIL: ") + name + ("" if ok else "  " + repr(detail)[-500:]), flush=True)
    if not ok: fails.append(name)
r = subprocess.run(["make", "-C", arch, "-B", "kernel64.bin"], capture_output=True, text=True, timeout=300)
if r.returncode: print(r.stdout[-1500:], r.stderr[-1500:]); print("FAIL: arch/x86_64 does not build"); sys.exit(1)
img = os.path.join(arch, "kernel64.bin")

def boot(tag, extra, secs, want):
    log, sock = os.path.join(tmp, tag + ".serial"), os.path.join(tmp, tag + ".qmp")
    for f in (log, sock):
        if os.path.exists(f): os.unlink(f)
    q = subprocess.Popen(["qemu-system-x86_64", "-kernel", img, "-vga", "std", "-display", "none", "-no-reboot", "-m", "256",
                          "-serial", "file:" + log, "-qmp", "unix:%s,server,nowait" % sock, *extra], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    out, end = "", time.time() + secs
    while time.time() < end:
        out = open(log, errors="replace").read() if os.path.exists(log) else ""
        if want in out or q.poll() is not None: break
        time.sleep(0.1)
    return q, out, sock

q, out, sock = boot("a", [], 30, "JT64 M0 done")
try:
    lines = [l for l in out.splitlines() if l.startswith("JT64")]
    want = ["JT64 M0 Joshua Tree, x86-64", "JT64 multiboot ok", "JT64 long mode ok, EFER 0x500", "JT64 64-bit math ok 0x300000007",
            "JT64 paging ok, 4 GiB mapped in 2048 pages of 2 MiB", None, "JT64 M0 done"]
    check("the serial log has the seven lines in order", len(lines) == 7 and all(w is None or w == l for w, l in zip(want, lines)), lines)
    check("the screen line names 1024x768 and a framebuffer address", len(lines) > 5 and lines[5].startswith("JT64 screen ok 1024x768 at 0x"), lines[5:6])
    check("nothing failed", "FAIL" not in out, out[-300:])
    s = socket.socket(socket.AF_UNIX); s.settimeout(20); s.connect(sock); f = s.makefile("rw"); f.readline()
    def cmd(c, **a):
        f.write(json.dumps({"execute": c, **({"arguments": a} if a else {})}) + "\n"); f.flush()
        while True:
            rr = json.loads(f.readline())
            if "return" in rr or "error" in rr: return rr
    cmd("qmp_capabilities")
    ppm = os.path.join(tmp, "shot.ppm"); cmd("screendump", filename=ppm); time.sleep(0.5)
    parts = open(ppm, "rb").read().split(b"\n", 3)
    w, h = (int(x) for x in parts[1].split()); px = parts[3]
    at = lambda x, y: tuple(px[(y * w + x) * 3:(y * w + x) * 3 + 3])
    check("the screen is 1024x768", (w, h) == (1024, 768), (w, h))
    check("it is the calm blue away from the text (#3B6EA5)", at(1000, 700) == (0x3B, 0x6E, 0xA5) and at(5, 5) == (0x3B, 0x6E, 0xA5), (at(1000, 700), at(5, 5)))
    white = lambda y0, y1: sum(1 for y in range(y0, y1) for x in range(0, w) if at(x, y) == (255, 255, 255))
    top, bottom = white(24, 24 + 7 * 34), white(400, 768)
    check("the seven lines are drawn in white on it (over 2000 white pixels), and nothing below", top > 2000 and bottom == 0, (top, bottom))
    row = lambda k: white(24 + k * 34, 24 + k * 34 + 32)
    check("each of the seven rows has ink, the eighth has none", all(row(k) > 100 for k in range(7)) and row(8) == 0, [row(k) for k in range(9)])
finally:
    q.kill(); q.wait()
q, out, _ = boot("b", ["-cpu", "pentium3"], 10, "JT64 no long mode")
q.kill(); q.wait()
check("a CPU without long mode is told so, and no ok line is printed", "JT64 no long mode" in out and "ok" not in out, out[-200:])
subprocess.run(["make", "-s", "-C", arch, "clean"], capture_output=True)
if fails: print("FAIL: " + ", ".join(fails)); sys.exit(1)
print("PASS: x86-64 slice 1: the 64-bit kernel boots in qemu-system-x86_64 into long mode, maps 4 GiB, and prints on the serial port and a 1024x768 screen; a 32-bit CPU is refused in words")
