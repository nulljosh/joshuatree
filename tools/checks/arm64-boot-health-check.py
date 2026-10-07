#!/usr/bin/env python3
"""ARM64 boot health: the Pi image on QEMU's raspi4b model must show no new failure on any boot step.

Boots `make -C arch/arm64 pi` on the raspi4b model and reads the UART. It fails on:
  - any line containing FAIL that is not in EXPECTED_FAIL below (so a new step that fails can never slip in silently),
  - any `oom` line (the heap ran out),
  - a crash report or an unhandled exception line,
  - no "M1c fb ok" (the desktop was not reached) or no "M3 userland ok".
Skips (exit 0) when clang, ld.lld, qemu-system-aarch64 or its raspi4b model is missing.
Usage: tools/checks/arm64-boot-health-check.py   (from the repo root)
"""
import os, shutil, subprocess, sys, tempfile, time

root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
arch = os.path.join(root, "arch/arm64")
if not all(shutil.which(t) for t in ("clang", "ld.lld", "qemu-system-aarch64")):
    print("SKIP: clang, ld.lld or qemu-system-aarch64 not installed"); sys.exit(0)
if "raspi4b" not in subprocess.run(["qemu-system-aarch64", "-machine", "help"], capture_output=True, text=True).stdout:
    print("SKIP: this QEMU has no raspi4b model (needs QEMU 9 or newer)"); sys.exit(0)

# FAIL lines that are expected on QEMU's raspi4b model, each with why. Anything else with FAIL in it is a regression.
# Today the boot prints none of them (QEMU has no PCIe, so USB stops at "usb pcie absent", which is not a FAIL line);
# they are listed so the day a step starts running on QEMU the check does not cry wolf about what QEMU cannot do.
EXPECTED_FAIL = [
    "usb vl805 FAIL: firmware load refused",   # QEMU's mailbox does not model the VL805 firmware-load tag; only a real GPU answers it
    "wifi FAIL cmd5",                          # the Wi-Fi bring-up (SDIO CMD5 to the CYW43455): QEMU's raspi4b has no such card
]

subprocess.run(["make", "-C", arch, "clean"], capture_output=True)
if subprocess.run(["make", "-C", arch, "pi"], capture_output=True).returncode:
    print("FAIL: arch/arm64 `make pi` does not build"); sys.exit(1)
log = tempfile.mktemp()
q = subprocess.Popen(["qemu-system-aarch64", "-machine", "raspi4b", "-display", "none", "-serial", "file:" + log,
                      "-kernel", os.path.join(arch, "kernel8.img")], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
out = ""
try:
    for _ in range(200):
        time.sleep(0.1)
        out = open(log, errors="replace").read() if os.path.exists(log) else ""
        if out.count("\nusb ") >= 1 and "@0x" in out: break   # the last thing the boot prints: the USB probe's verdict
    time.sleep(1)
    out = open(log, errors="replace").read()
finally:
    q.kill(); q.wait()
    try: os.remove(log)
    except OSError: pass
    subprocess.run(["make", "-C", arch, "clean"], capture_output=True)

fails = []
lines = out.splitlines()
for l in lines:
    if "FAIL" in l and not any(e in l for e in EXPECTED_FAIL): fails.append(f"unexpected failure line: {l!r}")
    if l.startswith("oom") or " oom " in l: fails.append(f"out of memory: {l!r}")
    if "CRASH" in l or "exception" in l: fails.append(f"crash or unhandled exception: {l!r}")
for need in ("M1c fb ok", "M3 userland ok"):
    if need not in lines: fails.append(f"{need!r} never printed (boot stopped early?), got {out[-300:]!r}")
seen = [e for e in EXPECTED_FAIL if e in out]
if seen: print("  expected on QEMU and seen:", seen)
if not fails: print(f"  ok: {len(lines)} UART lines, no FAIL, no oom, no crash, desktop and EL0 reached")
for m in fails: print("FAIL: " + m)
if fails: sys.exit(1)
print("PASS: the Pi image boots on QEMU's raspi4b model with no failure line beyond the listed, expected ones, no oom, and the desktop up")
