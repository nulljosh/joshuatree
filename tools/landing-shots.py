#!/usr/bin/env python3
"""Recapture every app tile in landing/shots/ the same way, so they match:
same boot, same dock launch, same window crop, same output size.

Each tile is gui_launch_from_dock's window rect (x=70,y=40,w=820,h=385 in
the 960x540 logical canvas, 1640x770 physical at 2x), scaled by exactly
0.6 to 984x462. Same ratio in and out, so nothing is stretched. The old
tiles were saved at 960x553, which squashed every one of them, and two
(Weather, Stocks) were never cropped at all.

Headless only: QEMU -display none, QMP input, pmemsave of the framebuffer.

Usage: python3 tools/landing-shots.py [name ...]   (from anywhere; default: all)
"""
import json, os, socket, subprocess, sys, time
from PIL import Image

REPO = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
os.chdir(REPO)
LOG = "/tmp/jt-landingshots-serial.log"; DUMP = "/tmp/jt-landingshots.raw"
FB = 0xfd000000; W, H = 1920, 1080; PORT = 4458
LOGICAL_W, LOGICAL_H, SCALE = 960, 540, 2
WIN = (70, 40, 820, 385)            # logical x, y, w, h
OUT = (984, 462)                    # 820x385 * 0.6, same ratio
DESKTOP_PX, DESKTOP_RGB = (480, 511), (0xEF, 0xEB, 0xE4)

# kernel.c GUI_DOCK_DEFAULT: Apps,Files,Mail,Calendar,Notes,Reminders,Terminal,Chat,Weather,Stocks,Trash
SLOT0_X, PITCH, ICON, DOCK_Y = 247, 43, 37, 487
SLOT = {"files": 1, "calendar": 3, "notes": 4, "terminal": 6, "chat": 7, "weather": 8, "stocks": 9}
FILE = {"chat": "samantha-chat", "notes": "app-notes", "calendar": "app-calendar", "weather": "app-weather",
        "files": "app-files", "terminal": "app-terminal", "stocks": "app-stocks"}


def shoot(name):
    for f in (LOG, DUMP):
        try: os.remove(f)
        except FileNotFoundError: pass
    q = subprocess.Popen(["qemu-system-i386", "-kernel", "kernel.elf", "-display", "none", "-vga", "std",
                          "-qmp", f"tcp:127.0.0.1:{PORT},server,nowait", "-serial", "file:" + LOG,
                          "-net", "nic,model=rtl8139", "-net", "user"],
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        s = None
        for _ in range(100):
            time.sleep(0.2)
            try: s = socket.create_connection(("127.0.0.1", PORT)); break
            except OSError: pass
        if s is None: sys.exit(f"FAIL {name}: QMP never came up")
        f = s.makefile("rw")
        def cmd(o):
            f.write(json.dumps(o) + "\n"); f.flush()
            while True:
                r = json.loads(f.readline())
                if "return" in r or "error" in r: return r
        f.readline(); cmd({"execute": "qmp_capabilities"})

        def move(x, y):
            cmd({"execute": "input-send-event", "arguments": {"events": [
                {"type": "abs", "data": {"axis": "x", "value": int(x * 32768 / LOGICAL_W)}},
                {"type": "abs", "data": {"axis": "y", "value": int(y * 32768 / LOGICAL_H)}}]}})
        def click(x, y):
            move(x, y); time.sleep(0.3)
            for down in (True, False):
                cmd({"execute": "input-send-event", "arguments": {"events": [{"type": "btn", "data": {"down": down, "button": "left"}}]}})
                time.sleep(0.1)
        def key(k):
            cmd({"execute": "human-monitor-command", "arguments": {"command-line": f"sendkey {k} 30"}})
            time.sleep(0.12)
        def type_text(t):
            for ch in t:
                key("shift-" + ch.lower() if ch.isupper() else {" ": "spc", ".": "dot", ",": "comma"}.get(ch, ch))
        def frame():
            cmd({"execute": "pmemsave", "arguments": {"val": FB, "size": W * H * 4, "filename": DUMP}})
            return Image.frombytes("RGBA", (W, H), open(DUMP, "rb").read(), "raw", "BGRA").convert("RGB")
        def serial():
            try: return open(LOG, errors="replace").read()
            except FileNotFoundError: return ""

        for _ in range(160):
            if frame().getpixel((DESKTOP_PX[0] * SCALE + 1, DESKTOP_PX[1] * SCALE + 1)) == DESKTOP_RGB: break
            time.sleep(0.25)
        else:
            sys.exit(f"FAIL {name}: desktop never appeared")
        time.sleep(1.0)

        click(SLOT0_X + SLOT[name] * PITCH + ICON // 2, DOCK_Y)
        time.sleep(1.5)

        if name == "notes":
            click(480, 300)                    # caret into the text area
            type_text("Sharp at every size now.")
            for _ in range(4): key("f2")       # bigger
            key("f3")                          # Bold; family stays Sans (house rule: no serif)
        elif name == "files":
            key("2")                           # Icons view
        elif name == "terminal":
            type_text("help")
        elif name == "weather":
            # Real fetch through QEMU's NAT; the free forecast API sometimes 503s, so retry until serial says ok.
            for _ in range(8):
                if "wxstate=ok" in serial(): break
                time.sleep(4); key("r")
            else:
                sys.exit("FAIL weather: never got a live reading (wxstate=ok)")
        elif name == "stocks":
            time.sleep(4)
        move(LOGICAL_W - 2, 2)                 # park the pointer outside the window crop
        time.sleep(1.0)

        x, y, w, h = WIN
        img = frame().crop((x * SCALE, y * SCALE, (x + w) * SCALE, (y + h) * SCALE)).resize(OUT, Image.LANCZOS)
        out = f"landing/shots/{FILE[name]}.webp"
        for qual in (85, 80, 70, 60):
            img.save(out, "WEBP", quality=qual, method=6)
            if os.path.getsize(out) < 120 * 1024: break
        print(f"saved {out} {OUT[0]}x{OUT[1]} ({os.path.getsize(out)} bytes, q={qual})")
    finally:
        q.terminate()
        try: q.wait(timeout=5)
        except Exception: q.kill()


if __name__ == "__main__":
    subprocess.run(["make", "-s", "kernel.elf"], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    for n in (sys.argv[1:] or list(SLOT)):
        shoot(n)
