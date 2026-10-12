#!/usr/bin/env python3
"""The Pi boots to a quiet desktop: the boot log steps aside and nothing plays by itself.

Boots the Pi image (`make -C arch/arm64 pi`, PI_BUILD) on QEMU's raspi4b model at 1080p and reads the mini UART. Passes when:
  - `console quiet` is printed (since 3.6.2 the boot log's window is never drawn at all: the log is kept and a dock click shows it;
    arm64-quietboot-check.py clicks it open);
  - the screen, 14 seconds on (the old boot demo's dock-label lap started 3 s after the loop and ran about 11), shows no console
    window (no big field of pure white where its well was) and no hover label (the pale capsule over the dock);
(Spotlight's `term` opening the Terminal as a window of its own is the dock Terminal tile's path, pane_open, the same code the
console-close and focus checks already walk; QEMU's raspi4b has no USB, so a Pi image cannot be typed at here.)
Skips (exit 0) when clang, ld.lld, qemu-system-aarch64 or its raspi4b model is missing.

Usage: tools/checks/arm64-boot-quiet-check.py   (from the repo root)
"""
import json, os, shutil, socket, subprocess, sys, tempfile, time

root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
arch = os.path.join(root, "arch/arm64")
if not all(shutil.which(t) for t in ("clang", "ld.lld", "qemu-system-aarch64")):
    print("SKIP: clang, ld.lld or qemu-system-aarch64 not installed"); sys.exit(0)
if "raspi4b" not in subprocess.run(["qemu-system-aarch64", "-machine", "help"], capture_output=True, text=True).stdout:
    print("SKIP: this QEMU has no raspi4b model (needs QEMU 9 or newer)"); sys.exit(0)

subprocess.run(["make", "-C", arch, "clean"], capture_output=True)
if subprocess.run(["make", "-C", arch, "pi"], capture_output=True).returncode:
    print("FAIL: arch/arm64 `make pi` does not build"); sys.exit(1)
tmp = tempfile.mkdtemp()
log, sock, shot = tmp + "/uart", tmp + "/qmp", tmp + "/shot.ppm"
q = subprocess.Popen(["qemu-system-aarch64", "-machine", "raspi4b", "-serial", "null", "-display", "none", "-serial", "file:" + log,
                      "-global", "bcm2835-fb.xres=1920", "-global", "bcm2835-fb.yres=1080", "-qmp", "unix:%s,server,nowait" % sock,
                      "-kernel", os.path.join(arch, "kernel8.img")], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
fails = []


def uart(): return open(log, errors="replace").read() if os.path.exists(log) else ""


def wait(needle, secs):
    for _ in range(int(secs * 10)):
        if needle in uart(): return True
        time.sleep(0.1)
    return False


def white_and_pale(f, cmd):
    r = cmd("screendump", filename=shot)
    if "error" in r: fails.append(f"screendump failed: {r['error']}"); return 0, 0
    data = open(shot, "rb").read()
    hdr = data.split(b"\n", 3)
    w, h = map(int, hdr[1].split()); px = hdr[3]
    white = pale = 0
    for y in range(300, 800, 4):
        for x in range(400, 1500, 4):
            o = (y * w + x) * 3
            c = (px[o], px[o + 1], px[o + 2])
            if c == (255, 255, 255): white += 1
    for y in range(h - 260, h - 140, 2):                  # the band just above the dock, where the hover label sits
        for x in range(500, 1400, 2):
            o = (y * w + x) * 3
            if (px[o], px[o + 1], px[o + 2]) == (0xf4, 0xf1, 0xec): pale += 1
    return white, pale


try:
    if not wait("M1c fb ok", 30): fails.append(f"the desktop never came up: {uart()[-300:]!r}")
    elif not wait("console quiet", 40): fails.append(f"the boot log was not kept quiet (no 'console quiet'): {uart()[-300:]!r}")
    else:
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
        time.sleep(14)
        white, pale = white_and_pale(f, cmd)
        print(f"  14 s after boot: {white} pure-white samples in the middle of the screen, {pale} hover-capsule samples above the dock")
        if white > 400: fails.append(f"a window is still on screen ({white} white samples where the Console's well was)")
        if pale > 4: fails.append(f"a dock hover label is showing with nothing hovered ({pale} capsule samples): the boot demo is back")
finally:
    q.kill(); q.wait()
    subprocess.run(["make", "-C", arch, "clean"], capture_output=True)

if fails:
    print("FAIL:"); [print("  - " + x) for x in fails]; sys.exit(1)
print("PASS: the Pi boots to a quiet desktop: the boot log steps aside, no dock label cycles by itself, and nothing plays by itself")
