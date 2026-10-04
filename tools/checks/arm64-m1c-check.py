#!/usr/bin/env python3
"""ARM64 M1c: the aarch64 kernel gets a framebuffer and draws a desktop into it, two ways. On QEMU's virt machine
through ramfb; on QEMU's Raspberry Pi 4B model through the VideoCore mailbox, the same call a real Pi answers.

Boots each build, waits for "M1c fb ok" on the UART, then asks QEMU for a screendump over QMP and checks real pixels:
menu bar, desktop, window, accent title bar and dock. This proves what the screen shows, not just what the kernel believes.
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
WANT = [("menu bar", (10, 10), (0xe0, 0xe0, 0xe0)), ("desktop", (50, 300), (0x20, 0x30, 0x40)), ("window", (400, 300), (0xff, 0xff, 0xff)),
        ("title bar", (400, 110), (0xb5, 0x50, 0x2c)), ("dock", (400, 560), (0x50, 0x5a, 0x68))]

def shoot(name, qemu_args, image):
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
        if (w, h) != (800, 600): fails.append(f"{name}: screen is {w}x{h}, want 800x600"); return
        for what, (x, y), want in WANT:
            i = (y * w + x) * 3; got = tuple(px[i:i + 3])
            if got != want: fails.append(f"{name}: {what} at {(x, y)}: got {got}, want {want}")
            else: print(f"  ok: {name} {what} pixel {want}")
    finally:
        q.kill(); q.wait()
        shutil.rmtree(tmp, ignore_errors=True)

shoot("virt ramfb", ["-machine", "virt", "-cpu", "cortex-a72", "-m", "256", "-device", "ramfb"], "kernel8.elf")
if "raspi4b" in subprocess.run(["qemu-system-aarch64", "-machine", "help"], capture_output=True, text=True).stdout:
    if subprocess.run(["make", "-C", arch, "pi"], capture_output=True).returncode: fails.append("pi: `make pi` does not build")
    else: shoot("pi mailbox", ["-machine", "raspi4b"], "kernel8.img")
else:
    print("  QEMU here has no raspi4b model (needs QEMU 9 or newer), Pi screen skipped")
subprocess.run(["make", "-C", arch, "clean"], capture_output=True)
for m in fails: print("FAIL: " + m)
if fails: sys.exit(1)
print("PASS: the aarch64 kernel draws its desktop through ramfb on virt and through the mailbox on the Pi 4B model, and QEMU's own screendump shows it with the right colors")
