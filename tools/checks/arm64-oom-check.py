#!/usr/bin/env python3
"""ARM64 out of memory: kmalloc returns 0 when the bump heap is full, and every caller in arch/arm64 copes.

`make -C arch/arm64 oomtest OOM_HEAP=<bytes>` shrinks HEAP_SIZE so an allocation fails on purpose. Three builds:
  - virt, heap too small for the 800x600 ramfb buffer: "oom fb" prints, the screen is skipped, and the kernel carries on
    over the UART to "M3 userland ok" (it used to write through a null framebuffer pointer);
  - virt, heap with room for the framebuffer but not for the three DejaVu faces: "oom text" prints, the boot still reaches
    "M1c fb ok", and the console is drawn in the 8x16 VGA font (a screendump shows text with no anti-aliased edges);
  - raspi4b (its framebuffer is the GPU's, not the heap), heap too small for the faces: the same "oom text" fallback.
In every case no "KERNEL CRASH" appears, and "M1d text FAIL" (the font is bad, not the heap) must not.
Skips (exit 0) when clang's aarch64 target, ld.lld or qemu-system-aarch64 is missing.
Usage: tools/checks/arm64-oom-check.py   (from the repo root)
"""
import json, os, shutil, socket, subprocess, sys, tempfile, time

root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
arch = os.path.join(root, "arch/arm64")
if not all(shutil.which(t) for t in ("clang", "ld.lld", "qemu-system-aarch64")):
    print("SKIP: clang, ld.lld or qemu-system-aarch64 not installed"); sys.exit(0)
fails = []
FB_HEAP = 4096 + 112 + 16 + 800 * 600 * 4   # the self-test's three blocks, then the 800x600 framebuffer
VIRT = ["-machine", "virt", "-cpu", "cortex-a72", "-m", "256", "-device", "ramfb"]

def build(heap):
    subprocess.run(["make", "-C", arch, "clean"], capture_output=True)
    return subprocess.run(["make", "-C", arch, "oomtest", "OOM_HEAP=%d" % heap], capture_output=True).returncode == 0

def run(name, heap, qemu_args, image, expect, screen):
    if not build(heap): fails.append(f"{name}: `make oomtest OOM_HEAP={heap}` does not build"); return
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
            if "M3 userland ok" in out or "CRASH" in out: break
        lines = out.splitlines()
        if "KERNEL CRASH" in out or "M3 userland ok" not in out: fails.append(f"{name}: the kernel did not survive the failed allocation: {out[-400:]!r}"); return
        if expect not in lines: fails.append(f"{name}: no {expect!r} line, got {out[-400:]!r}"); return
        if "M1d text FAIL" in out: fails.append(f"{name}: printed 'M1d text FAIL' where the heap was the cause (want {expect!r})"); return
        if screen and "M1c fb ok" not in out: fails.append(f"{name}: the desktop was not reached ('M1c fb ok' missing)"); return
        print(f"  ok: {name} printed {expect!r} and the boot reached 'M3 userland ok'")
        if not screen: return
        s = socket.socket(socket.AF_UNIX); s.connect(sock); f = s.makefile("rw")
        f.readline()
        def cmd(c, **a):
            f.write(json.dumps({"execute": c, "arguments": a}) + "\n"); f.flush()
            while True:
                r = json.loads(f.readline())
                if "return" in r or "error" in r: return r
        cmd("qmp_capabilities")
        if "error" in cmd("screendump", filename=shot): fails.append(f"{name}: screendump failed"); return
        time.sleep(0.3)
        parts = open(shot, "rb").read().split(b"\n", 3); w, h = map(int, parts[1].split()); px = parts[3]
        win_w, win_h = 500 * h // 600, 350 * h // 600; win_x, win_y = (w - win_w) // 2, 100 * h // 600
        win = [tuple(px[(y * w + x) * 3:(y * w + x) * 3 + 3]) for y in range(win_y + 32 * h // 600, win_y + win_h - 14 * h // 600) for x in range(win_x + 8 * h // 600, win_x + win_w - 4 * h // 600)]
        ink = sum(1 for c in win if max(c) < 0x90)
        soft = sum(1 for c in win if 0x30 < c[0] < 0xd0 and c[0] == c[1] == c[2])
        if ink < 300: fails.append(f"{name}: the boot log is not on screen ({ink} text pixels)")
        elif soft: fails.append(f"{name}: {soft} anti-aliased pixels: the smooth font drew although its allocation failed")
        else: print(f"  ok: {name} desktop up, console in the VGA fallback font ({ink} text pixels, no anti-aliased edges)")
    finally:
        q.kill(); q.wait()
        shutil.rmtree(tmp, ignore_errors=True)

run("virt, no room for the framebuffer", 4524, VIRT, "oom-kernel8.elf", "oom fb", False)
run("virt, no room for the fonts", FB_HEAP + 200, VIRT, "oom-kernel8.elf", "oom text", True)
if "raspi4b" in subprocess.run(["qemu-system-aarch64", "-machine", "help"], capture_output=True, text=True).stdout:
    run("pi, no room for the fonts", 4224 + 300, ["-machine", "raspi4b"], "oom-kernel8.img", "oom text", True)
else:
    print("  QEMU here has no raspi4b model (needs QEMU 9 or newer), Pi step skipped")
subprocess.run(["make", "-C", arch, "clean"], capture_output=True)
for m in fails: print("FAIL: " + m)
if fails: sys.exit(1)
print("PASS: a full heap makes the aarch64 kernel print 'oom fb' or 'oom text' and carry on (no screen, or the VGA fallback font), never crash")
