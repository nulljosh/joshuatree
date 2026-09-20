#!/usr/bin/env python3
"""The desktop is NEVER black.

Real report (live site, screenshot): with Files and Weather both open the
whole desktop was pure black and the menu bar had gone flat grey. Root
cause, proven with tools/checks/wall-neverblack-v86-probe.mjs against the
live proxy: wall_apply() painted a solid black "loading" buffer whenever a
map theme was wanted, no mosaic was on hand, and font_is_fallback() said
"not v86". That signal is only true on a cold v86 boot; after the landing
tour's in-place reboot it reads 0, the second boot logged wallsrc=dark, and
the fetch it was waiting for fails every cycle (wallerr=nomem in the 32MB
guest), so the black was permanent.

This check forces the same state deterministically: native QEMU
(font_is_fallback() == 0, the exact branch the rebooted v86 guest took)
with NO network card at all, so the live fetch can never land. It then
opens two multi-window apps (Files, then Weather, the reported combination),
dumps the real framebuffer, and asserts:
  1. serial never logs wallsrc=dark, and the first wallsrc= is a real image,
  2. a desktop strip no window or dock covers is a textured image, not a
     flat near-black fill, both before and after the windows open,
Before the fix: FAIL (wallsrc=dark, strip is one flat colour). After: PASS.

Usage: tools/checks/wall-neverblack-check.py   (repo root, after make kernel.elf)
"""
import json, os, socket, subprocess, sys, time
from PIL import Image, ImageStat

LOG = "/tmp/jt-neverblack-serial.log"
DUMP = "/tmp/jt-neverblack.raw"
FB = 0xfd000000; W, H = 1920, 1080
PORT = 4467
LOGICAL_W, LOGICAL_H, SCALE = 960, 540, 2
DOCK_ICON, DOCK_GAP, SLOT0_X = 37, 6, 247
PITCH = DOCK_ICON + DOCK_GAP
ICON_ROW_Y = 487
PARK = (480, 200)
# Logical rect left of window 0 (x=70) and above the dock band: pure desktop.
STRIP = (6, 60, 60, 420)
# Right end of the menu bar's left half, clear of the logo and the clock text.
BAR = (300, 4, 600, 18)

os.chdir(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
for p in (LOG, DUMP):
    try: os.remove(p)
    except FileNotFoundError: pass

q = subprocess.Popen(["qemu-system-i386", "-name", "jt-neverblack-check", "-kernel", "kernel.elf",
                      "-display", "none", "-vga", "std", "-nic", "none",
                      "-rtc", "base=2026-09-20T12:00:00",
                      "-qmp", f"tcp:127.0.0.1:{PORT},server,nowait", "-serial", "file:" + LOG],
                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
fails = []
try:
    s = None
    for _ in range(50):
        time.sleep(0.2)
        try: s = socket.create_connection(("127.0.0.1", PORT)); break
        except OSError: pass
    if s is None: raise SystemExit("FAIL: QEMU's QMP socket never came up")
    f = s.makefile("rw")
    def cmd(o):
        f.write(json.dumps(o) + "\n"); f.flush()
        while True:
            r = json.loads(f.readline())
            if "return" in r or "error" in r: return r
    f.readline()
    cmd({"execute": "qmp_capabilities"})
    time.sleep(6.0)  # desktop up, first weather/wallpaper cycle has run and failed (no NIC)

    def move(x, y):
        cmd({"execute": "input-send-event", "arguments": {"events": [
            {"type": "abs", "data": {"axis": "x", "value": int(x * 32768 / LOGICAL_W)}},
            {"type": "abs", "data": {"axis": "y", "value": int(y * 32768 / LOGICAL_H)}}]}})
    def click():
        cmd({"execute": "input-send-event", "arguments": {"events": [{"type": "btn", "data": {"down": True, "button": "left"}}]}})
        time.sleep(0.1)
        cmd({"execute": "input-send-event", "arguments": {"events": [{"type": "btn", "data": {"down": False, "button": "left"}}]}})
    def dump():
        cmd({"execute": "pmemsave", "arguments": {"val": FB, "size": W * H * 4, "filename": DUMP}})
        return Image.frombytes("RGBA", (W, H), open(DUMP, "rb").read(), "raw", "BGRA").convert("RGB")
    def open_slot(slot):
        move(SLOT0_X + slot * PITCH + DOCK_ICON // 2, ICON_ROW_Y); time.sleep(0.3)
        click(); time.sleep(1.5)
        move(*PARK); time.sleep(0.5)
    def region(img, r):
        st = ImageStat.Stat(img.crop(tuple(v * SCALE for v in r)).convert("L"))
        return st.mean[0], st.stddev[0]
    def judge(label, img):
        m, sd = region(img, STRIP); bm, bsd = region(img, BAR)
        print(f"{label}: desktop strip mean={m:.1f} stddev={sd:.1f}   menu bar mean={bm:.1f} stddev={bsd:.1f}")
        if m < 30 or sd < 4: fails.append(f"{label}: desktop strip is a flat/near-black fill (mean {m:.1f}, stddev {sd:.1f}), not a real wallpaper image")

    judge("idle desktop, fetch can never land", dump())
    open_slot(1)   # Files
    open_slot(8)   # Weather, on top of Files: the reported combination
    judge("Files + Weather open", dump())
finally:
    q.kill(); q.wait()

log = open(LOG, errors="replace").read().replace("\r", "") if os.path.exists(LOG) else ""
srcs = [l for l in log.split("\n") if l.startswith("wallsrc=")]
print("serial wallsrc lines:", srcs or "none")
if not srcs: fails.append("no wallsrc= line on serial, wall_apply never ran")
if "wallsrc=dark" in srcs: fails.append("wallsrc=dark: wall_apply selected the solid black buffer")
if srcs and srcs[0] not in ("wallsrc=satfallback", "wallsrc=map"): fails.append(f"first desktop paint was {srcs[0]}, expected the baked satellite (or a live map)")

if fails:
    for x in fails: print("FAIL:", x)
    sys.exit(1)
print("PASS: with the live fetch unable to land, the desktop stayed a real wallpaper image, idle and under two open windows")
