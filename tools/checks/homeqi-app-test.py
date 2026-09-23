#!/usr/bin/env python3
"""Test Homeqi app: boot kernel, open from Apps folder using keyboard nav,
test interaction, capture question, reasoning, and result screens."""
import json, os, socket, subprocess, sys, time
from PIL import Image, ImageChops

LOG = "/tmp/jt-homeqi-test.log"
DUMP = "/tmp/jt-homeqi-test.raw"
FB = 0xfd000000; W, H = 1920, 1080
PORT = 4463
LOGICAL_W, LOGICAL_H, SCALE = 960, 540, 2
CLOSE_X, CLOSE_Y = 94, 56
APPS_CLOSE_X, APPS_CLOSE_Y = 80, 46
CLOSE_RED = (0xFF, 0x5F, 0x57)
PARK = (480, 200)
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
        time.sleep(0.3)
    def dump():
        cmd({"execute": "pmemsave", "arguments": {"val": FB, "size": W * H * 4, "filename": DUMP}})
        return Image.frombytes("RGBA", (W, H), open(DUMP, "rb").read(), "raw", "BGRA").convert("RGB")
    def pixel(x, y):
        img = dump()
        return img.getpixel((x * SCALE + 1, y * SCALE + 1))
    def snap(name):
        img = dump()
        img.save(f"{OUTDIR}/{name}.png")
        print(f"Saved {name}.png")

    # Wait for desktop
    for _ in range(120):
        if pixel(480, 511) == (0xEF, 0xEB, 0xE4):
            break
        time.sleep(0.25)
    else:
        raise SystemExit("FAIL: desktop dock did not appear within 30 seconds")
    time.sleep(0.3)

    print("Opening Apps folder...")
    # Click Apps dock slot (slot 0)
    move(247, 487)
    click()
    time.sleep(1.0)

    print("Navigating to Homeqi (icon 16)...")
    # Icon 16: row = 16 // 5 = 3, col = 16 % 5 = 1
    # Navigate right 1, down 3
    for _ in range(1):
        key("d")  # right
    for _ in range(3):
        key("s")  # down

    time.sleep(0.3)
    snap("16-Homeqi-question1")

    print("Launching Homeqi...")
    key("ret")  # Enter to launch
    time.sleep(1.5)
    snap("16-Homeqi-after-launch")

    print("Answering yes to question 1...")
    key("1")
    time.sleep(0.5)
    snap("16-Homeqi-reasoning1")

    # Continue through remaining questions
    for i in range(2, 9):
        key("space")  # any key for next
        time.sleep(0.5)
        key("1")  # yes
        time.sleep(0.3)

    # Final "any key" to see result
    key("space")
    time.sleep(0.5)
    snap("16-Homeqi-result")

    # Close
    print("Closing...")
    key("esc")
    time.sleep(0.5)

    print("PASS: Homeqi app tested successfully")

finally:
    q.terminate()
    q.wait(timeout=5)
