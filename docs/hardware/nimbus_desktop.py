# SPDX-License-Identifier: CC-BY-NC-SA-4.0
# Copyright (c) 2026 Joshua Trommel. Hardware design, see docs/hardware/LICENSE-NOTICE.md
# Refreshes the picture on Nimbus's screen: builds the Pi image (`make -C arch/arm64 pi`), boots it headless on QEMU's raspi4b model at
# 1920 x 1080 the way tools/checks/arm64-m1c-check.py does, and saves a screendump to docs/hardware/nimbus/nimbus-desktop.png.
#   python3 docs/hardware/nimbus_desktop.py
# render_nimbus.py paints over the motto line before it maps the picture onto the glass. Needs clang, ld.lld, qemu-system-aarch64 and Pillow.
import os, subprocess, socket, json, time, tempfile
from PIL import Image

root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
arch = os.path.join(root, "arch", "arm64")
out = os.path.join(root, "docs", "hardware", "nimbus", "nimbus-desktop.png")
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
print("wrote", out)
