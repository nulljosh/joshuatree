#!/usr/bin/env python3
"""One-off visual proof (not part of the regression suite) that
joshuatree.iso reaches the real desktop when booted -cdrom, same
QMP + pmemsave pattern the app-interact checks use. Writes a PNG next
to the ISO for a human to look at.

Usage: tools/checks/iso_fb_dump.py [out.png]
"""
import json, os, socket, subprocess, sys, time
from PIL import Image

LOG = "/tmp/jt-isodump-serial.log"
DUMP = "/tmp/jt-isodump.raw"
FB = 0xfd000000; W, H = 1920, 1080
PORT = 4460
OUT = sys.argv[1] if len(sys.argv) > 1 else "/tmp/jt-iso-boot.png"

os.chdir(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
for f in (LOG, DUMP):
    try: os.remove(f)
    except FileNotFoundError: pass

q = subprocess.Popen(
    ["qemu-system-i386", "-cdrom", "joshuatree.iso", "-display", "none", "-vga", "std",
     "-qmp", f"tcp:127.0.0.1:{PORT},server,nowait", "-serial", "file:" + LOG],
    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
try:
    s = None
    for _ in range(50):
        try:
            s = socket.create_connection(("127.0.0.1", PORT), timeout=1)
            break
        except OSError:
            time.sleep(0.2)
    if not s:
        print("FAIL: no QMP connection"); sys.exit(1)
    buf = s.recv(65536)
    s.sendall(json.dumps({"execute": "qmp_capabilities"}).encode() + b"\n")
    time.sleep(0.3); s.recv(65536)

    time.sleep(6)  # reach gui_run and paint at least one full frame
    s.sendall(json.dumps({"execute": "pmemsave",
                           "arguments": {"val": FB, "size": W * H * 4, "filename": DUMP}}).encode() + b"\n")
    time.sleep(1); s.recv(65536)

    if not os.path.exists(DUMP) or os.path.getsize(DUMP) < W * H * 4:
        print("FAIL: framebuffer dump missing/short"); sys.exit(1)

    with open(DUMP, "rb") as f:
        raw = f.read(W * H * 4)
    img = Image.frombytes("RGBA", (W, H), raw, "raw", "BGRA")
    img.convert("RGB").save(OUT)
    print(f"PASS: wrote {OUT}")
finally:
    q.terminate()
    try: q.wait(timeout=5)
    except Exception: q.kill()
