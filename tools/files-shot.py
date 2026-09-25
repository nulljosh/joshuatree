#!/usr/bin/env python3
"""Recapture landing/shots/app-files.webp with the Files app in Icons view
(seeded ramfs files visible), matching the settings-screenshot.py /
filesview-check.py mechanism: headless QMP + pmemsave framebuffer dump.
The old shot was captured before the Icons view existed and shows an
almost-empty List screen; this opens Files from the dock and presses '2'
to switch to Icons, same as tools/checks/filesview-check.py."""
import json, os, socket, subprocess, sys, time
from PIL import Image

REPO = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
os.chdir(REPO)
LOG = "/tmp/jt-files-serial.log"; DUMP = "/tmp/jt-files.raw"
FB = 0xfd000000; W, H = 1920, 1080; PORT = 4455
LOGICAL_W, LOGICAL_H = 960, 540
DOCK_ICON, DOCK_GAP, SLOT0_X = 37, 6, 247
PITCH = DOCK_ICON + DOCK_GAP
ICON_ROW_Y = 487
FILES_SLOT = 1

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
    def key(qcode):
        cmd({"execute": "input-send-event", "arguments": {"events": [{"type": "key", "data": {"down": True, "key": {"type": "qcode", "data": qcode}}}]}})
        time.sleep(0.05)
        cmd({"execute": "input-send-event", "arguments": {"events": [{"type": "key", "data": {"down": False, "key": {"type": "qcode", "data": qcode}}}]}})
    def full_frame():
        cmd({"execute": "pmemsave", "arguments": {"val": FB, "size": W * H * 4, "filename": DUMP}})
        return Image.frombytes("RGBA", (W, H), open(DUMP, "rb").read(), "raw", "BGRA").convert("RGB")

    files_x = SLOT0_X + FILES_SLOT * PITCH + DOCK_ICON // 2
    move(files_x, ICON_ROW_Y); time.sleep(0.3); click(); time.sleep(1.0)  # Files from dock
    key("2"); time.sleep(0.5)  # switch to Icons view

    img = full_frame()

    # Window: gui_launch_from_dock places non-Apps-folder windows at
    # logical (70,40,820,385); framebuffer is physical 1920x1080 over a
    # 960x540 logical canvas, so scale factor is 2.
    lx, ly, lw, lh = 70, 40, 820, 385
    scale = W / LOGICAL_W
    box = (int(lx * scale), int(ly * scale), int((lx + lw) * scale), int((ly + lh) * scale))
    crop = img.crop(box)

    # Match the existing shots' published size (960x553, same as
    # app-terminal.webp) so the tile grid stays uniform.
    crop = crop.resize((960, 553), Image.LANCZOS)
    out = "landing/shots/app-files.webp"
    for q_ in (80, 70, 60, 50, 40):
        crop.save(out, "WEBP", quality=q_, method=6)
        size = os.path.getsize(out)
        if size < 120 * 1024:
            print(f"saved {out} ({size} bytes, quality={q_})")
            break
    else:
        print(f"WARNING: {out} still {size} bytes at lowest tried quality")
finally:
    q.terminate()
    try: q.wait(timeout=5)
    except Exception: q.kill()
