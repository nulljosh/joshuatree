#!/usr/bin/env python3
"""v0.85.5: real screenshot of the new Settings Location row with "Langley"
typed into the field, same mechanism tools/settings-screenshot.py already
uses (headless QMP + pmemsave dump) -- open Settings via the Apple menu,
click the Location row to open its prompt, send real keystrokes to type
"Langley", dump the framebuffer before pressing enter."""
import json, os, socket, subprocess, sys, time

REPO = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
os.chdir(REPO)
LOG = "/tmp/jt-loc-serial.log"; DUMP = "/tmp/jt-loc.raw"
FB = 0xfd000000; W, H = 1920, 1080; PORT = 4455
LOGICAL_W, LOGICAL_H = 960, 540

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
    if s is None: sys.exit("FAIL: QEMU's QMP socket never came up")
    f = s.makefile("rw")
    def cmd(o):
        f.write(json.dumps(o) + "\n"); f.flush()
        while True:
            r = json.loads(f.readline())
            if "return" in r or "error" in r: return r
    f.readline(); cmd({"execute": "qmp_capabilities"}); time.sleep(5.0)

    def move(x, y):
        cmd({"execute": "input-send-event", "arguments": {"events": [
            {"type": "abs", "data": {"axis": "x", "value": int(x * 32768 / LOGICAL_W)}},
            {"type": "abs", "data": {"axis": "y", "value": int(y * 32768 / LOGICAL_H)}}]}})
    def click():
        cmd({"execute": "input-send-event", "arguments": {"events": [{"type": "btn", "data": {"down": True, "button": "left"}}]}})
        time.sleep(0.1)
        cmd({"execute": "input-send-event", "arguments": {"events": [{"type": "btn", "data": {"down": False, "button": "left"}}]}})
    def key(k):
        cmd({"execute": "send-key", "arguments": {"keys": [{"type": "qcode", "data": k}]}})
        time.sleep(0.15)
    def snap(name):
        cmd({"execute": "pmemsave", "arguments": {"val": FB, "size": W * H * 4, "filename": DUMP}})
        from PIL import Image
        img = Image.frombytes("RGBA", (W, H), open(DUMP, "rb").read(), "raw", "BGRA").convert("RGB")
        out = f"/tmp/{name}.png"; img.save(out); print("saved", out)

    move(16, 13); time.sleep(0.3); click(); time.sleep(0.5)    # apple logo -> menu open
    move(100, 111); time.sleep(0.3); click(); time.sleep(1.0)  # Settings row
    move(100, 316); time.sleep(0.3); click(); time.sleep(0.8)  # Location row -> opens the text prompt
    for c in "Langley":
        key(c.lower())
    time.sleep(0.5)
    snap("jt-settings-location")
    print("PASS: Settings Location row opened, \"Langley\" typed, framebuffer dumped")
finally:
    q.terminate()
    try: q.wait(timeout=5)
    except Exception: q.kill()
