# SPDX-License-Identifier: CC-BY-NC-SA-4.0
# Copyright (c) 2026 Joshua Trommel. Hardware design, see docs/hardware/LICENSE-NOTICE.md
# Refreshes the picture on Nimbus's screen: builds the Pi image (`make -C arch/arm64 pi`), boots it headless on QEMU's raspi4b model at
# 1920 x 1080 the way tools/checks/arm64-m1c-check.py does, and saves a screendump to docs/hardware/nimbus/nimbus-desktop.png.
#   python3 docs/hardware/nimbus_desktop.py
# Then compose() clears the Hamurapi window and the motto off that real desktop and sets three native apps (Notes, Calendar, Weather) on it from
# landing/shots, with the current brand mark in the menu bar. `--compose` skips the build and QEMU and only redoes that step on the saved picture.
# Needs clang, ld.lld, qemu-system-aarch64, rsvg-convert and Pillow.
import os, subprocess, socket, json, time, tempfile
from PIL import Image
import sys

root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
arch = os.path.join(root, "arch", "arm64")
out = os.path.join(root, "docs", "hardware", "nimbus", "nimbus-desktop.png")

def compose(path):
    """grey wallpaper, real menu bar and real dock from the ARM64 screendump; three real app screenshots as windows between them"""
    from PIL import ImageDraw, ImageFilter
    img = Image.open(path).convert("RGB"); bg = img.getpixel((20, 600))
    img.paste(bg, (0, 53, 1920, 905))                                   # the old window, motto and apple glyph: all of the desktop between bar and dock
    img.paste(img.getpixel((700, 20)), (24, 6, 72, 46))                 # the old menu bar mark, drawn again below from the current ink mark
    m = Image.open(os.path.join(root, "docs", "brand", "mark-ink.png")).convert("RGBA"); m = m.crop(m.getbbox()); m = m.resize((round(m.width * 38 / m.height), 38), Image.LANCZOS)
    img.paste(m, (28, 7), m)
    shots = os.path.join(root, "landing", "shots")
    def win(name):
        w = Image.open(os.path.join(shots, "app-%s.webp" % name)).convert("RGB"); k = Image.new("L", w.size, 0)
        ImageDraw.Draw(k).rounded_rectangle((0, 0, w.width - 1, w.height - 1), 22, fill=255); w = w.convert("RGBA"); w.putalpha(k); return w
    base = img.convert("RGBA")
    for name, x, y in (("weather", 70, 110), ("calendar", 868, 150), ("notes", 400, 420)):
        w = win(name); sh = Image.new("RGBA", base.size, (0, 0, 0, 0)); sh.paste((0, 0, 0, 120), (x, y + 14, x + w.width, y + w.height + 14), w.getchannel("A"))
        base.alpha_composite(sh.filter(ImageFilter.GaussianBlur(18))); base.alpha_composite(w, (x, y))
    base.convert("RGB").save(path)
    print("composed", path)
if "--compose" in sys.argv:
    compose(out); raise SystemExit
subprocess.run(["make", "-C", arch, "clean"], capture_output=True)
if subprocess.run(["make", "-C", arch, "pi"], capture_output=True).returncode: raise SystemExit("make pi failed")
tmp = tempfile.mkdtemp(); log, sock, shot = tmp + "/uart", tmp + "/qmp", tmp + "/shot.ppm"
q = subprocess.Popen(["qemu-system-aarch64", "-machine", "raspi4b", "-serial", "null", "-global", "bcm2835-fb.xres=1920", "-global", "bcm2835-fb.yres=1080",
                      "-display", "none", "-serial", "file:" + log, "-qmp", "unix:%s,server,nowait" % sock, "-kernel", os.path.join(arch, "kernel8.img")],
                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
try:
    for _ in range(300):
        time.sleep(0.1)
        if os.path.exists(log) and "M1c fb ok" in open(log, errors="replace").read(): break
    time.sleep(2.5)
    s = socket.socket(socket.AF_UNIX); s.connect(sock); f = s.makefile("rw"); f.readline()
    def cmd(c, **a):
        f.write(json.dumps({"execute": c, "arguments": a}) + "\n"); f.flush()
        while True:
            r = json.loads(f.readline())
            if "return" in r or "error" in r: return r
    cmd("qmp_capabilities")
    if "error" in cmd("screendump", filename=shot): raise SystemExit("screendump failed")
    time.sleep(0.5)
    Image.open(shot).convert("RGB").save(out)
finally:
    q.kill()
    subprocess.run(["make", "-C", arch, "clean"], capture_output=True)
compose(out)
print("wrote", out)
