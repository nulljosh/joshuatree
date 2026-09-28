#!/usr/bin/env python3
"""Open Settings, click through General/Assistant/Account in the sidebar,
snap a full framebuffer dump (already 2x logical res: 1920x1080 for a
960x540 logical screen) for each section."""
import json, os, socket, subprocess, sys, time

REPO = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
os.chdir(REPO)
LOG = "/tmp/jt-set2-serial.log"; DUMP = "/tmp/jt-set2.raw"
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
    def snap(name):
        cmd({"execute": "pmemsave", "arguments": {"val": FB, "size": W * H * 4, "filename": DUMP}})
        from PIL import Image
        img = Image.frombytes("RGBA", (W, H), open(DUMP, "rb").read(), "raw", "BGRA").convert("RGB")
        out = f"/tmp/{name}.png"; img.save(out); print("saved", out)

    move(16, 13); time.sleep(0.3); click(); time.sleep(0.5)   # apple logo -> menu open
    move(100, 111); time.sleep(0.3); click(); time.sleep(1.0) # Settings row
    snap("jt-settings-after-desktop")   # General is default section

    # sidebar rows: SETTINGS_SIDEBAR_Y0=60, ROW_H=34, cx mid ~90
    move(90, 60 + 1*34 + 10); time.sleep(0.3); click(); time.sleep(0.6)
    snap("jt-settings-after-assistant")

    move(90, 60 + 2*34 + 10); time.sleep(0.3); click(); time.sleep(0.6)
    snap("jt-settings-after-account")

    print("PASS: three section crops captured")
finally:
    q.terminate()
    try: q.wait(timeout=5)
    except Exception: q.kill()
