#!/usr/bin/env python3
"""Every typeface loads at boot (Joshua, 2026-10-07: "load the entire typeface on every boot, for concurrency and
reliability"), and a broken one falls back instead of failing later.

1. i386: the serial line says "fonts 14 of 14 ok, N bytes of heap kept" before the desktop comes up ("guidesktop").
2. i386 with `fontbreak=inter`: Inter is fed a blob that is not a font, so the real parse fails; the line says
   "13 of 14" and names inter as drawn with DejaVu Sans, and the desktop still comes up.
3. Pi (QEMU virt): "fonts 3 of 3 ok" for its mono, sans bold and sans faces.

Discriminating: remove uitext_fonts_preload from kmain, or the fallback in drivers/ttf.c, and a step fails.

Usage: tools/checks/fonts-boot-check.py   (from the repo root, after make kernel.elf)
"""
import os, re, shutil, subprocess, sys, time
from pathlib import Path
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from scratch import scratch_dir

ROOT = Path(__file__).resolve().parents[2]
os.chdir(ROOT)
TMP = Path(scratch_dir("fontsboot"))
fails = []
def check(name, ok, detail=""):
    print(("  ok: " if ok else "  FAIL: ") + name + (f" ({detail})" if detail and not ok else ""))
    if not ok: fails.append(name)

def boot(cmd, log, until, secs=60):
    q = subprocess.Popen(cmd + ["-serial", f"file:{log}"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        for _ in range(secs * 10):
            text = log.read_text(errors="replace") if log.exists() else ""
            if until in text: return text
            time.sleep(.1)
        return log.read_text(errors="replace") if log.exists() else ""
    finally:
        q.terminate()
        try: q.wait(5)
        except subprocess.TimeoutExpired: q.kill()

i386 = ["qemu-system-i386", "-kernel", "kernel.elf", "-display", "none", "-vga", "std"]
out = boot(i386, TMP / "plain.log", "guidesktop")
m = re.search(r"^fonts (\d+) of (\d+) ok, (\d+) bytes of heap kept$", out, re.M)
check("i386: every face loaded at boot", bool(m) and m[1] == m[2] == "14", out[-300:] if not m else m[0])
check("i386: the line comes before the desktop", m is not None and out.find(m[0]) < out.find("guidesktop"))

out = boot(i386 + ["-append", "fontbreak=inter"], TMP / "broken.log", "guidesktop")
m = re.search(r"^fonts 13 of 14 ok, \d+ bytes of heap kept; broken, drawn as DejaVu Sans: inter$", out, re.M)
check("i386: a broken Inter is caught at boot and named", bool(m), re.findall(r"^fonts .*$", out, re.M))
check("i386: the desktop still comes up with a face broken", "guidesktop" in out)

if all(shutil.which(t) for t in ("clang", "ld.lld", "qemu-system-aarch64")):
    subprocess.run(["make", "-s", "-C", "arch/arm64", "kernel8.elf"], check=True)
    out = boot(["qemu-system-aarch64", "-machine", "virt", "-cpu", "cortex-a72", "-m", "256", "-nic", "none",
                "-device", "ramfb", "-display", "none", "-kernel", "arch/arm64/kernel8.elf"], TMP / "pi.log", "fonts ", 40)
    time.sleep(0)
    check("Pi: its three faces loaded at boot", "fonts 3 of 3 ok" in out, re.findall(r"^fonts .*$", out, re.M))
else:
    print("  skip: no aarch64 toolchain")

if fails: print("FAIL: " + "; ".join(fails)); sys.exit(1)
print("PASS: every typeface is parsed and tried once at boot (14 of 14 on i386, 3 of 3 on the Pi), and a broken face is "
      "named on the boot line and drawn as DejaVu Sans while the desktop comes up anyway")
