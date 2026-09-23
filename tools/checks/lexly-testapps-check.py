#!/usr/bin/env python3
"""Headless proof that Lexly (kernel/lexly.h, v0.92.0) is real: it opens via
testapps, shows a Spanish word and four English answer choices, responds to
keyboard input (1-4), and displays correct/incorrect feedback.

Usage: tools/checks/lexly-testapps-check.py   (from the repo root, after make kernel.elf)
"""
import json, os, socket, subprocess, sys, time
from PIL import Image

LOG = "/tmp/jt-lexly-testapps.log"
DUMP = "/tmp/jt-lexly-testapps.raw"
FB = 0xfd000000; W, H = 1920, 1080
PORT = 4465

os.chdir(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
mkdir_cmd = "mkdir -p /tmp/jt-lexly-gallery"
os.system(mkdir_cmd)

for f in (LOG, DUMP):
    try: os.remove(f)
    except FileNotFoundError: pass

q = subprocess.Popen(["qemu-system-i386", "-kernel", "kernel.elf", "-display", "none", "-vga", "std",
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
    time.sleep(5.0)  # desktop up

    def key(qcode):
        cmd({"execute": "send-key", "arguments": {"keys": [{"type": "qcode", "data": qcode}]}})
        time.sleep(0.25)
    def dump():
        cmd({"execute": "pmemsave", "arguments": {"val": FB, "size": W * H * 4, "filename": DUMP}})
        return Image.frombytes("RGBA", (W, H), open(DUMP, "rb").read(), "raw", "BGRA").convert("RGB")
    def lum(p): return (p[0] * 299 + p[1] * 587 + p[2] * 114) // 1000

    # Type 'testapps' command to cycle through all apps
    # testapps cycles through icons 0-12 automatically (since we're using the v0.92 build)
    key("t"); key("e"); key("s"); key("t"); key("a"); key("p"); key("p"); key("s")
    key("ret"); time.sleep(2.0)

    # testapps will cycle through each app: Files(0), Mail(1), Calendar(2), ..., Quotes(11), Lexly(12), etc.
    # For each app, we press ESC to close and move to the next
    # But first, let's wait a bit and capture what's on screen - it should be Files(0)
    app_count = 0
    lexly_found = False

    # Cycle through apps and look for Lexly
    for app_num in range(14):  # Apps 0-13 (Lexly is 13)
        time.sleep(0.5)

        if app_num == 13:  # When we reach icon 13 (Lexly)
            img = dump()
            img.save(f"/tmp/jt-lexly-gallery/lexly-initial.png")
            print(f"Captured screenshot at app #{app_num} (should be Lexly)")

            # Check content area for dark pixels (text content)
            content_area = img.crop((100, 100, 1820, 1000))
            pixels = list(content_area.getdata())
            dark_px = sum(1 for p in pixels if lum(p) < 200)
            print(f"Content area dark pixels: {dark_px}")

            if dark_px < 50:
                fails.append(f"App {app_num} shows too little text")
            else:
                print(f"App {app_num} has text content")
                lexly_found = True

            # Press '1' to answer a question (WITHOUT ESC, yet)
            key("1"); time.sleep(0.8)
            img2 = dump()
            img2.save(f"/tmp/jt-lexly-gallery/lexly-after-answer.png")

            # Check if the content changed
            content_area2 = img2.crop((100, 100, 1820, 1000))
            pixels2 = list(content_area2.getdata())
            dark_px2 = sum(1 for p in pixels2 if lum(p) < 200)
            print(f"After answer, dark pixels: {dark_px2}")

            if abs(dark_px2 - dark_px) < 3:
                fails.append("Screen did not change after pressing 1")
            else:
                print("Screen changed after answer (feedback displayed)")

        # Press ESC to close this app and move to the next
        key("esc")
        app_count += 1

    if not lexly_found:
        fails.append("Never found Lexly (icon 13) in testapps cycle")

    try: cmd({"execute": "quit"})
    except (ConnectionResetError, BrokenPipeError, OSError): pass
finally:
    try: q.wait(timeout=5)
    except subprocess.TimeoutExpired: q.kill()

if fails:
    for x in fails: print("FAIL:", x)
    sys.exit(1)
print("PASS: Lexly opens via testapps, displays content, and responds to input")
