#!/usr/bin/env python3
"""v85 4b/4f real screenshot: headless QMP + pmemsave framebuffer dump of
the new Chat window (proves gui_draw_app_titlebar chrome, matching
Contacts/Calculator/Settings) and the Settings screen with the two new
LLM rows. Same mechanism app-interact-check.py already uses, worktree-
agent-safe (no -display cocoa window popped), no test disk needed since
this doesn't touch persistence. Saves PNGs to /tmp for a real, non-
QEMU-screendump visual check."""
import json, os, socket, subprocess, sys, time

REPO = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
os.chdir(REPO)
LOG = "/tmp/jt-chatshot-serial.log"
DUMP = "/tmp/jt-chatshot.raw"
FB = 0xfd000000; W, H = 1920, 1080
PORT = 4453
LOGICAL_W, LOGICAL_H = 960, 540
DOCK_ICON, DOCK_GAP, SLOT0_X = 37, 6, 268
PITCH = DOCK_ICON + DOCK_GAP
ICON_ROW_Y = 487
CHAT_SLOT = 7

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
    f.readline()
    cmd({"execute": "qmp_capabilities"})
    time.sleep(5.0)

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
        out = f"/tmp/{name}.png"
        img.save(out)
        print(f"saved {out}")

    centre = lambda slot: SLOT0_X + slot * PITCH + DOCK_ICON // 2
    move(centre(CHAT_SLOT), ICON_ROW_Y); time.sleep(0.3)
    click(); time.sleep(1.2)
    snap("jt-chat-window")
    print("PASS: Chat window opened from its dock icon, framebuffer dumped")
finally:
    q.terminate()
    try: q.wait(timeout=5)
    except Exception: q.kill()
