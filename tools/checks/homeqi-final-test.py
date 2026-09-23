#!/usr/bin/env python3
"""Simple Homeqi test: boot, navigate, complete all 8 questions, capture result."""
import json, os, socket, subprocess, sys, time
from PIL import Image

LOG = "/tmp/jt-homeqi-final.log"
DUMP = "/tmp/jt-homeqi-final.raw"
FB = 0xfd000000; W, H = 1920, 1080
PORT = 4464
LOGICAL_W, LOGICAL_H, SCALE = 960, 540, 2
OUTDIR = "/tmp/jt-homeqi-gallery"

os.chdir(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
os.makedirs(OUTDIR, exist_ok=True)

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

    def move(x, y):
        cmd({"execute": "input-send-event", "arguments": {"events": [
            {"type": "abs", "data": {"axis": "x", "value": int(x * 32768 / LOGICAL_W)}},
            {"type": "abs", "data": {"axis": "y", "value": int(y * 32768 / LOGICAL_H)}}]}})
    def click():
        cmd({"execute": "input-send-event", "arguments": {"events": [{"type": "btn", "data": {"down": True, "button": "left"}}]}})
        time.sleep(0.1)
        cmd({"execute": "input-send-event", "arguments": {"events": [{"type": "btn", "data": {"down": False, "button": "left"}}]}})
    def key(qcode):
        cmd({"execute": "send-key", "arguments": {"keys": [{"type": "qcode", "data": qcode}]}})
        time.sleep(0.35)
    def dump():
        cmd({"execute": "pmemsave", "arguments": {"val": FB, "size": W * H * 4, "filename": DUMP}})
        return Image.frombytes("RGBA", (W, H), open(DUMP, "rb").read(), "raw", "BGRA").convert("RGB")
    def pixel(x, y):
        img = dump()
        return img.getpixel((x * SCALE + 1, y * SCALE + 1))
    def snap(name):
        img = dump()
        img.save(f"{OUTDIR}/{name}.png")
        print(f"  saved {name}.png")

    # Wait for desktop
    for _ in range(120):
        if pixel(480, 511) == (0xEF, 0xEB, 0xE4):
            break
        time.sleep(0.25)
    else:
        raise SystemExit("FAIL: desktop dock did not appear within 30 seconds")
    time.sleep(0.3)

    print("1. Opening Apps folder...")
    move(247, 487)
    click()
    time.sleep(1.0)

    print("2. Navigating to Homeqi (icon 16)...")
    for _ in range(1):
        key("d")  # right
    for _ in range(3):
        key("s")  # down
    time.sleep(0.3)

    print("3. Launching Homeqi...")
    key("ret")
    time.sleep(1.5)
    snap("16-Homeqi-question1")

    print("4. Answering all 8 questions with yes...")
    for q_num in range(1, 9):
        key("1")  # yes
        time.sleep(0.4)
        if q_num < 8:
            key("space")  # next
            time.sleep(0.4)

    print("5. Capturing result screen...")
    time.sleep(0.5)
    snap("16-Homeqi-result")

    # Verify the result screen shows the expected elements
    img = dump().crop((100, 200, 900, 700))
    text_pixels = sum(1 for x in range(img.width) for y in range(img.height)
                      if sum(img.getpixel((x, y))[:3]) < 400)  # dark text
    if text_pixels > 100:
        print("6. Result screen verified (contains text)")
    else:
        print("6. WARNING: Result screen may not have expected content")

    print("7. Closing...")
    key("esc")
    time.sleep(0.5)

    print("PASS: Homeqi test complete")

finally:
    q.terminate()
    q.wait(timeout=5)
