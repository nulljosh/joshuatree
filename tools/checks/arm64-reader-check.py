#!/usr/bin/env python3
"""10.0.0, the screen reader: Cmd+F5 turns it on, the arrows along the dock are read out, and it waits for the focus to rest.

Boots the ARM64 kernel on QEMU's virt machine with a virtio keyboard and an 800x600 ramfb. Then:
1. Cmd+F5 says "reader on", and the menu bar reads "Screen reader on" (dark ink left of the status icons);
2. three quick right arrows give three lines, "Apps, app, 1 of 12. Press Return to open." through Mail, 3 of 12;
3. after the focus rests (600 ms), Mail alone is spoken ("reader speak Mail"), not Apps or Files;
4. the caption is a wide pill centred over the dock, not the short label over Mail's tile;
5. `reader off` in the Terminal turns it off: the menu bar mark goes, and the next arrow says nothing.
Discriminating: drop reader_focus from dock_select (keys.c) and steps 2 to 4 fail; drop the rest test in reader_tick
(desk.c) and Apps and Files are spoken too.
Skips (exit 0) without clang, ld.lld or qemu-system-aarch64.
Usage: tools/checks/arm64-reader-check.py   (from the repo root)
"""
import json, os, shutil, socket, subprocess, sys, time
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from scratch import scratch_dir

root = Path(__file__).resolve().parents[2]
arch = root / "arch/arm64"
if not all(shutil.which(t) for t in ("clang", "ld.lld", "qemu-system-aarch64")):
    print("SKIP: clang, ld.lld or qemu-system-aarch64 not installed"); sys.exit(0)
tmp = Path(scratch_dir("arm64-reader"))
fails = []
def check(name, ok, detail=""):
    print(("  ok: " if ok else "  FAIL: ") + name + ("" if ok else "  " + repr(detail)[-600:]))
    if not ok: fails.append(name)

subprocess.run(["make", "-C", str(arch), "clean"], capture_output=True, timeout=60)
r = subprocess.run(["make", "-C", str(arch), "kernel8.elf"], capture_output=True, text=True, timeout=600)
if r.returncode: print(r.stdout[-2000:], r.stderr[-2000:]); print("FAIL: arch/arm64 does not build"); sys.exit(1)

# The dock on QEMU's 800x600 ramfb at scale 1, from kernel/dock_geom.c (as in arm64-touch-check.py): y0 is the tray's top.
W, H, PCT, ICONS, GAP, PAD, MBOT = 800, 600, 7, 12, 6, 10, 24
icon = min(H * PCT // 100, (min(740, W - 40) - 2 * PAD - (ICONS - 1) * GAP) // ICONS)
y0 = H - icon - 2 * PAD - MBOT
LABEL_BG = (0xF4, 0xF1, 0xEC)   # DOCK_LABEL_BG, kernel/gui_paint.h

log, sock, shot = tmp / "uart", tmp / "qmp", tmp / "shot.ppm"
q = subprocess.Popen(["qemu-system-aarch64", "-machine", "virt", "-cpu", "cortex-a72", "-m", "256", "-global", "virtio-mmio.force-legacy=false",
                      "-device", "ramfb", "-device", "virtio-keyboard-device", "-nic", "none",
                      "-display", "none", "-serial", "file:" + str(log), "-qmp", "unix:%s,server,nowait" % sock, "-kernel", str(arch / "kernel8.elf")],
                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
def uart(): return log.read_text(errors="replace") if log.exists() else ""
def wait_for(text, secs, after=0):
    end = time.time() + secs
    while time.time() < end:
        if text in uart()[after:]: return True
        if q.poll() is not None: return False
        time.sleep(0.1)
    return False
try:
    if not wait_for("M2 input ready", 60): print("FAIL: did not boot: %r" % uart()[-600:]); sys.exit(1)
    time.sleep(3.0)   # any boot notice in the menu bar has gone
    s = socket.socket(socket.AF_UNIX); s.settimeout(30); s.connect(str(sock)); f = s.makefile("rw"); f.readline()
    def command(name, **a):
        f.write(json.dumps({"execute": name, "arguments": a}) + "\n"); f.flush()
        while True:
            rr = json.loads(f.readline())
            if "return" in rr or "error" in rr: return rr
    command("qmp_capabilities")
    def key(*qcodes, gap=0.3): command("send-key", keys=[{"type": "qcode", "data": k} for k in qcodes]); time.sleep(gap)
    def screen():
        command("screendump", filename=str(shot)); time.sleep(0.3)
        parts = shot.read_bytes().split(b"\n", 3); sw, _ = map(int, parts[1].split()); d = parts[3]
        return lambda x, y: tuple(d[(y * sw + x) * 3:(y * sw + x) * 3 + 3])
    def near(c, want): return all(abs(a - b) <= 3 for a, b in zip(c, want))
    def caption(px): return sum(near(px(x, y), LABEL_BG) for y in range(y0 - 30, y0) for x in range(250, 550))
    def mark(px): return sum(max(px(x, y)) < 0x50 for y in range(0, 24) for x in range(420, 620))

    n = len(uart()); key("meta_l", "f5")
    check("Cmd+F5 turns the reader on", wait_for("reader on", 5, n), uart()[n:][-300:])
    time.sleep(0.5); off_mark = None
    n = len(uart())
    for _ in range(3): key("right", gap=0.12)
    time.sleep(1.5)
    out = uart()[n:]
    lines = [l for l in out.splitlines() if l.startswith("reader: ")]
    want = ["reader: Apps, app, 1 of 12. Press Return to open.", "reader: Files, app, 2 of 12. Press Return to open.", "reader: Mail, app, 3 of 12. Press Return to open."]
    check("three arrows read Apps, Files and Mail with their places", lines == want, lines)
    spoken = [l for l in out.splitlines() if l.startswith("reader speak")]
    check("only the resting focus is spoken (Mail)", spoken == ["reader speak Mail"], spoken)
    px = screen(); on_cap, on_mark = caption(px), mark(px)
    check("the caption is a wide pill centred over the dock (%d label pixels)" % on_cap, on_cap > 2500, on_cap)
    check("the menu bar says Screen reader on (%d ink pixels)" % on_mark, on_mark > 40, on_mark)

    key("esc"); key("f2"); time.sleep(0.8)
    n = len(uart())
    for ch in "reader":
        key(ch, gap=0.08)
    key("spc", gap=0.08)
    for ch in "off": key(ch, gap=0.08)
    key("ret")
    check("`reader off` in the Terminal turns it off", wait_for("reader off", 5, n), uart()[n:][-300:])
    key("meta_l", "w"); time.sleep(0.8)
    n = len(uart()); key("right"); time.sleep(1.2)
    out = uart()[n:]
    check("with it off, an arrow says nothing", "reader: " not in out and "reader speak" not in out, out[-300:])
    px = screen(); off_cap, off_mark = caption(px), mark(px)
    check("with it off, no wide caption (%d) and no menu bar mark (%d)" % (off_cap, off_mark), off_cap < on_cap // 4 and off_mark < on_mark // 4, (off_cap, off_mark))
    check("no kernel crash", "KERNEL CRASH" not in uart() and q.poll() is None, uart()[-400:])
finally:
    q.kill(); q.wait()
    subprocess.run(["make", "-C", str(arch), "clean"], capture_output=True, timeout=60)
if fails: print("FAIL: %d of the reader checks" % len(fails)); sys.exit(1)
print("PASS: Cmd+F5 starts the screen reader, the dock is read as the arrows move, only the resting focus is spoken, and `reader off` stops it")
