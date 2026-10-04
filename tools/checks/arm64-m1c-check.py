#!/usr/bin/env python3
"""ARM64 M1c: the aarch64 kernel configures a ramfb framebuffer on QEMU's virt machine and draws a desktop into it.

Boots arch/arm64, waits for "M1c fb ok" on the UART, then asks QEMU for a screendump over QMP and checks real pixels:
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

tmp = tempfile.mkdtemp()
log, sock, shot = tmp + "/uart", tmp + "/qmp", tmp + "/shot.ppm"
q = subprocess.Popen(["qemu-system-aarch64", "-machine", "virt", "-cpu", "cortex-a72", "-m", "256", "-device", "ramfb",
                      "-display", "none", "-serial", "file:" + log, "-qmp", "unix:%s,server,nowait" % sock,
                      "-kernel", os.path.join(arch, "kernel8.elf")], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
fails = []
try:
    out = ""
    for _ in range(100):
        time.sleep(0.1)
        out = open(log, errors="replace").read() if os.path.exists(log) else ""
        if "M1c" in out: break
    if "M1c fb ok" not in out:
        fails.append(f"kernel did not report M1c fb ok, got {out!r}")
    else:
        s = socket.socket(socket.AF_UNIX); s.connect(sock); f = s.makefile("rw")
        f.readline()
        def cmd(c, **a):
            f.write(json.dumps({"execute": c, "arguments": a}) + "\n"); f.flush()
            while True:
                r = json.loads(f.readline())
                if "return" in r or "error" in r: return r
        cmd("qmp_capabilities")
        r = cmd("screendump", filename=shot)
        if "error" in r: fails.append(f"screendump failed: {r['error']}")
        else:
            time.sleep(0.3)
            d = open(shot, "rb").read()
            # P6\n800 600\n255\n then rgb
            parts = d.split(b"\n", 3); w, h = map(int, parts[1].split()); px = parts[3]
            def at(x, y): i = (y * w + x) * 3; return tuple(px[i:i + 3])
            for name, xy, want in [("menu bar", (10, 10), (0xe0, 0xe0, 0xe0)), ("desktop", (50, 300), (0x20, 0x30, 0x40)),
                                   ("window", (400, 300), (0xff, 0xff, 0xff)), ("title bar", (400, 110), (0xb5, 0x50, 0x2c)),
                                   ("dock", (400, 560), (0x50, 0x5a, 0x68))]:
                if (w, h) != (800, 600) or at(*xy) != want: fails.append(f"{name} at {xy}: got {at(*xy) if (w,h)==(800,600) else (w,h)}, want {want}")
                else: print(f"  ok: {name} pixel {want}")
finally:
    q.kill(); q.wait()
    shutil.rmtree(tmp, ignore_errors=True)
    subprocess.run(["make", "-C", arch, "clean"], capture_output=True)
for m in fails: print("FAIL: " + m)
if fails: sys.exit(1)
print("PASS: the aarch64 kernel sets up a ramfb framebuffer and QEMU's own screendump shows the desktop it drew")
