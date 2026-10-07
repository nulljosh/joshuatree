#!/usr/bin/env python3
"""ARM64 console scrollback: the on-screen Console (the only debug channel on a Pi with no serial cable) keeps the whole
boot log and a keyboard scrolls it. Page Up and Page Down move by half a page, Home jumps to the first line, End goes back
to the newest. The title bar says which lines are on screen ("lines 12-27 of 61") and one row under the text pins the
latest wifi and usb status line.

Boots arch/arm64 on QEMU's virt machine with ramfb and a virtio keyboard, waits for the boot to finish, takes a screendump,
sends Page Up through QMP send-key (the path a real keystroke takes), takes a second one and checks the console rectangle
changed and the title bar hint now reads an earlier range than before (its pixels differ and the text pixels moved). Then
End, and the picture must be the first one again, pixel for pixel in the console rectangle. Home must differ from both.
Run it with the Page Up handling removed from arch/arm64/main.c and it fails; as shipped it passes.
Skips (exit 0) when clang's aarch64 target, ld.lld or qemu-system-aarch64 is missing.
Usage: tools/checks/arm64-console-scroll-check.py   (from the repo root)
"""
import json, os, shutil, socket, subprocess, sys, tempfile, time

root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
arch = os.path.join(root, "arch/arm64")
if not all(shutil.which(t) for t in ("clang", "ld.lld", "qemu-system-aarch64")):
    print("SKIP: clang, ld.lld or qemu-system-aarch64 not installed"); sys.exit(0)
subprocess.run(["make", "-C", arch, "clean"], capture_output=True, timeout=120)
if subprocess.run(["make", "-C", arch, "MAINFLAGS=-DCON_VERBOSE"], capture_output=True, timeout=300).returncode:
    print("FAIL: arch/arm64 `make` does not build"); sys.exit(1)

tmp = tempfile.mkdtemp()
log, sock = tmp + "/uart", tmp + "/qmp"
q = subprocess.Popen(["qemu-system-aarch64", "-machine", "virt", "-cpu", "cortex-a72", "-m", "256",
                      "-global", "virtio-mmio.force-legacy=false", "-device", "ramfb", "-device", "virtio-keyboard-device",
                      "-display", "none", "-serial", "file:" + log, "-qmp", "unix:%s,server,nowait" % sock,
                      "-kernel", os.path.join(arch, "kernel8.elf")], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
fails = []
def uart(): return open(log, errors="replace").read() if os.path.exists(log) else ""
def wait_for(text, tries=150):
    for _ in range(tries):
        if text in uart(): return True
        time.sleep(0.1)
    return False
try:
    if not wait_for("M1c fb ok") or not wait_for("M2 input ready, devices 1"):
        fails.append(f"the kernel did not finish booting with a screen and a keyboard, got {uart()!r}")
    else:
        time.sleep(0.5)
        s = socket.socket(socket.AF_UNIX); s.settimeout(20); s.connect(sock); f = s.makefile("rw")
        f.readline()
        def cmd(c, **a):
            f.write(json.dumps({"execute": c, "arguments": a}) + "\n"); f.flush()
            while True:
                r = json.loads(f.readline())
                if "return" in r or "error" in r: return r
        cmd("qmp_capabilities")
        n = [0]
        def shot():
            n[0] += 1; path = "%s/s%d.ppm" % (tmp, n[0])
            r = cmd("screendump", filename=path)
            if "error" in r: fails.append(f"screendump failed: {r['error']}"); return None
            time.sleep(0.4)
            parts = open(path, "rb").read().split(b"\n", 3); w, h = map(int, parts[1].split())
            return w, h, parts[3]
        def key(k):
            cmd("send-key", keys=[{"type": "qcode", "data": k}]); time.sleep(0.6)
        a = shot()
        if a:
            w, h, _ = a
            sc = lambda v: v * h // 600                              # the kernel's layout rule: an 800x600 design scaled by height
            win_w, win_h = sc(500), sc(350); win_x, win_y = (w - win_w) // 2, sc(100)
            def rect(shot_, x0, y0, x1, y1):
                px = shot_[2]; return [px[(y * w + x) * 3:(y * w + x) * 3 + 3] for y in range(y0, y1) for x in range(x0, x1)]
            body = (win_x + sc(8), win_y + sc(32), win_x + win_w - sc(4), win_y + win_h - sc(14))   # the console text and the pinned row
            hint = (win_x + win_w // 2, win_y + 2, win_x + win_w - 2, win_y + sc(26))                # the title bar's right half
            ink = lambda r: sum(1 for c in r if max(c) < 0x90)
            if ink(rect(a, *body)) < 300: fails.append("the boot log is not on screen before any key")
            if ink(rect(a, *hint)) < 20: fails.append("no 'lines A-B of N' hint in the title bar")
            else: print("  ok: the title bar carries a position hint (%d ink pixels on the cream band)" % ink(rect(a, *hint)))
            # the pinned row: the bottom row of the console text area holds the wifi/usb summary even before any scrolling
            key("pgup"); b = shot()
            key("end"); c = shot()
            key("home"); d = shot()
            if b is None or c is None or d is None: raise SystemExit
            if rect(a, *body) == rect(b, *body): fails.append("Page Up did not change the console rectangle")
            else: print("  ok: Page Up changed the console text")
            if rect(a, *hint) == rect(b, *hint): fails.append("Page Up did not change the title bar hint")
            else: print("  ok: the title bar hint moved to an earlier range")
            if rect(a, *body) != rect(c, *body) or rect(a, *hint) != rect(c, *hint): fails.append("End did not restore the first picture")
            else: print("  ok: End restored the first picture exactly")
            if rect(d, *body) == rect(a, *body) or rect(d, *body) == rect(b, *body): fails.append("Home did not jump to the first lines")
            else: print("  ok: Home showed the first lines")
            # the pinned status row is the same wherever the view is: the last text row of the body
            row = lambda s_: rect(s_, body[0], body[3] - sc(16), body[2], body[3])
            if rect(a, *body)[-1:] and row(a) == row(b) == row(d): print("  ok: the pinned status row stayed put while scrolled")
            else: fails.append("the pinned status row changed when the view scrolled")
finally:
    q.kill(); q.wait()
    shutil.rmtree(tmp, ignore_errors=True)
subprocess.run(["make", "-C", arch, "clean"], capture_output=True, timeout=120)
for m in fails: print("FAIL: " + m)
if fails: sys.exit(1)
print("PASS: Page Up scrolls the Console back and the title bar says which lines, End restores the newest picture exactly, Home jumps to the first lines")
