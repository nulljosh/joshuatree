#!/usr/bin/env python3
"""7.5.5: mouse reports that queue up are drawn once, at the latest position (arch/arm64/boot.c pointer_flush).

On the 27 inch ASUS the dock magnify and a dragged window trailed the hand: every report redrew the whole dock band
(22 to 31 ms a frame in QEMU at 1080p), so a fast mouse queued reports faster than they were drawn. Now a report only
marks the pointer moved and the loop draws once per pass.

The ptrburst build feeds 20 made-up mouse reports in one go at boot, then flushes, and says how many times
pointer_moved ran. It must be 1. Discriminating: call pointer_moved() on each EV_SYN again (the pre-7.5.5 code) and
it says 20.

7.6.0: then the pointer goes onto the dock and the build says how many heap bytes the magnify took. It must be 0: the
dock's buffers are kept from the desktop's first paint, so a heap mark held around a magnify (voice mode's, a `sam`
reply's) cannot free them. Discriminating: take them lazily in dock_magnify again (7.5.5) and it says about 3 MB.

Usage: tools/checks/arm64-ptrburst-check.py   (from the repo root)
"""
import os, re, shutil, subprocess, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from scratch import scratch_dir

root = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
arch = os.path.join(root, "arch", "arm64")
if not all(shutil.which(t) for t in ("clang", "ld.lld", "qemu-system-aarch64")):
    print("SKIP: no aarch64 toolchain"); sys.exit(0)
subprocess.run(["make", "-s", "-C", arch, "ptrbursttest"], check=True)
log = scratch_dir("arm64-ptrburst") + "/uart"
q = subprocess.Popen(["qemu-system-aarch64", "-machine", "virt", "-cpu", "cortex-a72", "-m", "256", "-nic", "none", "-device", "ramfb",
                      "-device", "pcie-root-port,id=rp,chassis=1", "-device", "qemu-xhci,id=x,bus=rp",
                      "-device", "usb-kbd,bus=x.0", "-device", "usb-mouse,bus=x.0",
                      "-display", "none", "-serial", "file:" + log, "-kernel", os.path.join(arch, "ptrburst-kernel8.elf")],
                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
out = ""
try:
    for _ in range(600):
        out = open(log, errors="replace").read() if os.path.exists(log) else ""
        if "ptrburst:" in out or "KERNEL CRASH" in out: break
        time.sleep(0.1)
    time.sleep(0.3)
    out = open(log, errors="replace").read()
finally:
    q.kill(); q.wait()
m = re.search(r"ptrburst: 20 reports, (\d+) draws", out)
if not m: print("FAIL: no ptrburst line\n" + out[-600:]); sys.exit(1)
if "KERNEL CRASH" in out: print("FAIL: kernel crash\n" + out[-600:]); sys.exit(1)
if m.group(1) != "1": print(f"FAIL: 20 queued mouse reports were drawn {m.group(1)} times, not once"); sys.exit(1)
h = re.search(r"ptrburst: dock magnify took (\d+) heap bytes", out)
if not h or "dock magnify 5 size" not in out: print("FAIL: the dock did not magnify under the pointer\n" + out[-600:]); sys.exit(1)
if h.group(1) != "0": print(f"FAIL: magnifying the dock took {h.group(1)} heap bytes; a heap mark held around it would free them"); sys.exit(1)
print("PASS: 20 queued mouse reports are drawn once, at the latest position, and magnifying the dock takes nothing from the heap")
