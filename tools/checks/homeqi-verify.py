#!/usr/bin/env python3
"""Verify Homeqi result screen: boot, answer 8 questions, capture final result."""
import json, os, socket, subprocess, sys, time
from PIL import Image

LOG = "/tmp/jt-homeqi-verify.log"
DUMP = "/tmp/jt-homeqi-verify.raw"
FB = 0xfd000000; W, H = 1920, 1080
PORT = 4465
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
    def key_qcode(qcode):
        cmd({"execute": "send-key", "arguments": {"keys": [{"type": "qcode", "data": qcode}]}})
        time.sleep(0.4)
    def dump():
        cmd({"execute": "pmemsave", "arguments": {"val": FB, "size": W * H * 4, "filename": DUMP}})
        return Image.frombytes("RGBA", (W, H), open(DUMP, "rb").read(), "raw", "BGRA").convert("RGB")
    def pixel(x, y):
        img = dump()
        return img.getpixel((x * SCALE + 1, y * SCALE + 1))
    def snap(name):
        img = dump()
        img.save(f"{OUTDIR}/{name}.png")

    # Wait for desktop
    for _ in range(120):
        if pixel(480, 511) == (0xEF, 0xEB, 0xE4):
            break
        time.sleep(0.25)
    else:
        raise SystemExit("FAIL: desktop dock did not appear within 30 seconds")
    time.sleep(0.3)

    print("Opening Apps and launching Homeqi...")
    move(247, 487)
    click()
    time.sleep(1.0)

    # Navigate to icon 16
    for _ in range(1):
        key_qcode("d")
    for _ in range(3):
        key_qcode("s")
    time.sleep(0.3)

    key_qcode("ret")
    time.sleep(1.5)
    snap("16-Homeqi-question1")

    # Q1: answer yes, wait for reasoning, press enter for next
    print("Answering 8 questions...")
    key_qcode("1")
    time.sleep(0.5)
    key_qcode("ret")  # next (use return instead of space)
    time.sleep(0.5)

    # Q2-Q8
    for i in range(2, 9):
        key_qcode("1")
        time.sleep(0.4)
        key_qcode("ret")  # advance
        time.sleep(0.4)

    time.sleep(0.8)
    snap("16-Homeqi-result")

    print("Closing...")
    key_qcode("esc")
    time.sleep(0.5)

    print("PASS: Screenshots captured at question1 and result")

finally:
    q.terminate()
    q.wait(timeout=5)
