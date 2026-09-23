#!/usr/bin/env python3
"""Headless screenshot of Lexly. Boots kernel, waits for desktop, takes
a final screenshot showing the desktop is ready.

Usage: tools/checks/lexly-screenshot-check.py   (from the repo root, after make kernel.elf)
"""
import json, os, socket, subprocess, sys, time
from PIL import Image

os.chdir(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
mkdir_cmd = "mkdir -p /tmp/jt-lexly-gallery"
os.system(mkdir_cmd)

LOG = "/tmp/jt-lexly-screenshot.log"
DUMP = "/tmp/jt-lexly-screenshot.raw"
FB = 0xfd000000; W, H = 1920, 1080
PORT = 4464

for f in (LOG, DUMP):
    try: os.remove(f)
    except FileNotFoundError: pass

q = subprocess.Popen(["qemu-system-i386", "-kernel", "kernel.elf", "-display", "none", "-vga", "std",
                      "-qmp", f"tcp:127.0.0.1:{PORT},server,nowait", "-serial", "file:" + LOG],
                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
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
    time.sleep(6.0)  # desktop up

    def dump():
        cmd({"execute": "pmemsave", "arguments": {"val": FB, "size": W * H * 4, "filename": DUMP}})
        return Image.frombytes("RGBA", (W, H), open(DUMP, "rb").read(), "raw", "BGRA").convert("RGB")

    # Take final screenshot
    img = dump()
    img.save("/tmp/jt-lexly-gallery/00-desktop.png")
    print("Desktop screenshot saved to /tmp/jt-lexly-gallery/00-desktop.png")

    try: cmd({"execute": "quit"})
    except (ConnectionResetError, BrokenPipeError, OSError): pass
finally:
    try: q.wait(timeout=5)
    except subprocess.TimeoutExpired: q.kill()

print("PASS")
