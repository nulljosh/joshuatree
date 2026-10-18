#!/usr/bin/env python3
"""ARM64 boot splash bar (3.9.8): the bar under the logo advances smoothly with boot, not in six jumps.

Boots the -DQUIET_BOOT -DSPLASH_STEP_MS=1500 build headless on QEMU's virt machine with ramfb. The UART carries one `splash bar N`
line (thousandths of the track) for every frame the bar is drawn. It asserts the values only ever rise, end at 1000, that there are
dozens of distinct ones with no jump above 5 percent, and, with a screendump while step 3 holds, that the grey fill on screen is
the width the last line says (half the track) and not a stage boundary short of it.
Skips (exit 0) when clang, ld.lld or qemu-system-aarch64 is missing. Usage: tools/checks/arm64-splashbar-check.py
"""
import json, os, re, shutil, socket, subprocess, sys, tempfile, time

root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
arch = os.path.join(root, "arch/arm64")
if not all(shutil.which(t) for t in ("clang", "ld.lld", "qemu-system-aarch64")):
    print("SKIP: clang, ld.lld or qemu-system-aarch64 not installed"); sys.exit(0)
fails = []
tmp = tempfile.mkdtemp(prefix="jt-splashbar-")
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
        print("FAIL: no splash step 3, got %r" % uart()[-400:]); sys.exit(1)
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
    y = H // 2 + 62 * H // 540 + 1
    fill = sum(1 for x in range(W) if tuple(d[(y * W + x) * 3:(y * W + x) * 3 + 3]) == (0x63, 0x63, 0x66))
    track = fill + sum(1 for x in range(W) if tuple(d[(y * W + x) * 3:(y * W + x) * 3 + 3]) == (0xC8, 0xC8, 0xCC))
    if track < 100 or abs(fill * 1000 // track - 500) > 20: fails.append(f"at step 3 the fill is {fill} of {track} px, not half")
    else: print(f"  ok: at step 3 the bar on screen is {fill} of {track} px, half the track")
    for _ in range(300):
        if "splash step 6 " in uart(): break
        time.sleep(0.1)
    vals = [int(v) for v in re.findall(r"splash bar (\d+)", uart())]
    if "splash step 6 " not in uart(): fails.append("boot never finished")
    elif vals != sorted(vals) or vals[-1:] != [1000]: fails.append(f"the bar does not rise to 1000: {vals[-12:]}")
    elif len(set(vals)) < 40 or max(b - a for a, b in zip(vals, vals[1:])) > 50:
        fails.append(f"the bar steps, not glides: {len(set(vals))} distinct values, biggest jump {max(b - a for a, b in zip(vals, vals[1:]))}")
    else: print(f"  ok: {len(set(vals))} distinct fills from {vals[0]} to 1000, biggest jump {max(b - a for a, b in zip(vals, vals[1:]))}")
    if "KERNEL CRASH" in uart(): fails.append("the kernel crashed")
finally:
    q.kill(); q.wait()
print("PASS" if not fails else "FAIL: " + "; ".join(fails))
sys.exit(1 if fails else 0)
