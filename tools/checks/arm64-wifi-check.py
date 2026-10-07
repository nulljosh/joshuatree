#!/usr/bin/env python3
"""ARM64 M4 Wi-Fi stage 1: the Pi image's Wi-Fi bring-up never hangs the boot, with and without the firmware files.

QEMU's raspi4b model has the SD host the chip hangs off but no SDIO card behind it, so the kernel must print, in order,
`M1c fb ok`, `wifi power on`, `wifi FAIL cmd5` (every wait in wifi_init is bounded) and then carry on to the next
driver (`usb pcie absent`). A second build with no firmware files (empty stubs) must boot the same way; QEMU never gets as far as `wifi no firmware`
(that needs a card), so that line is checked in the image. This proves the failure paths, not the radio: the real chip answers only on a real board.
Skips (exit 0) when clang's aarch64 target, ld.lld or qemu-system-aarch64 is missing, or QEMU has no raspi4b model.
Usage: tools/checks/arm64-wifi-check.py   (from the repo root)
"""
import os, shutil, subprocess, sys, tempfile, time

root = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
arch = os.path.join(root, "arch/arm64")
if not all(shutil.which(t) for t in ("clang", "ld.lld", "qemu-system-aarch64")):
    print("SKIP: clang, ld.lld or qemu-system-aarch64 not installed"); sys.exit(0)
if "raspi4b" not in subprocess.run(["qemu-system-aarch64", "-machine", "help"], capture_output=True, text=True).stdout:
    print("SKIP: QEMU here has no raspi4b model (needs QEMU 9 or newer)"); sys.exit(0)
fails = []

def build(nofw):
    subprocess.run(["make", "-C", arch, "clean"], capture_output=True)
    try:
        if not nofw: return subprocess.run(["make", "-C", arch, "pi"], capture_output=True, timeout=600).returncode == 0
        # No firmware: stub wifi_fw.S from an empty directory, and tell make not to refetch (`pi` would download it).
        if subprocess.run(["./wifi_fw_gen.sh", os.path.join(root, "build/wifi-fw.absent")], cwd=arch, capture_output=True, timeout=60).returncode: return False
        return subprocess.run(["make", "-C", arch, "-o", "wifi-fw", "kernel8.img"], capture_output=True, timeout=600).returncode == 0
    except subprocess.TimeoutExpired: return False

def boot(name, want, nofw=False):
    if not build(nofw): fails.append(f"{name}: arch/arm64 `make pi` does not build"); return
    log = tempfile.mktemp()
    q = subprocess.Popen(["qemu-system-aarch64", "-machine", "raspi4b", "-display", "none", "-serial", "file:" + log,
                          "-kernel", os.path.join(arch, "kernel8.img")], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    out = ""
    try:
        for _ in range(300):                                       # 30 s cap: a hung boot never gets the last line
            time.sleep(0.1)
            out = open(log, errors="replace").read() if os.path.exists(log) else ""
            if "usb pcie absent" in out: break
    finally:
        q.kill(); q.wait()
        try: os.remove(log)
        except OSError: pass
    at = -1
    for w in want:
        i = out.find(w, at + 1)
        if i < 0: fails.append(f"{name}: no {w!r} after position {at}, got {out[-400:]!r}"); return
        at = i
    print(f"  ok: {name} printed, in order, {want}")

boot("no SDIO card", ["M1c fb ok", "wifi power on", "wifi FAIL cmd5", "usb pcie absent"])
# Without the three files the build links empty stubs. QEMU has no SDIO card, so fw_load() is never reached here and the
# boot ends at cmd5 as before; the `wifi no firmware` line itself is checked in the image and the source.
boot("no firmware", ["M1c fb ok", "wifi power on", "wifi FAIL cmd5", "usb pcie absent"], nofw=True)
if b"wifi no firmware" not in open(os.path.join(arch, "kernel8.img"), "rb").read() if os.path.exists(os.path.join(arch, "kernel8.img")) else True:
    fails.append("no firmware: the image lost its `wifi no firmware` line")
else: print("  ok: no firmware: image carries the `wifi no firmware` line and the stub reports length 0")
if "wifi_fw_bin_len: .word 0" not in open(os.path.join(arch, "wifi_fw.S")).read(): fails.append("no firmware: wifi_fw.S is not an empty stub")
subprocess.run(["make", "-C", arch, "clean"], capture_output=True)
for m in fails: print("FAIL: " + m)
if fails: sys.exit(1)
print("PASS: on QEMU's raspi4b the Wi-Fi bring-up powers the chip, finds no SDIO card, prints wifi FAIL cmd5 and the boot carries on")
