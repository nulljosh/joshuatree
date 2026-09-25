#!/usr/bin/env python3
"""Recapture landing/shots/app-notes.webp and app-terminal.webp with real
TrueType type visible: opens Notes from the dock, types a short line,
bumps the size up (F2) and switches to the Serif family (F1) so the tile
shows off real type instead of the old small default. Terminal is
recaptured too for parity. Same headless QMP + pmemsave mechanism as
tools/files-shot.py and tools/checks/notessharp-check.py."""
import json, os, socket, subprocess, sys, time
from PIL import Image

REPO = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
os.chdir(REPO)
LOG = "/tmp/jt-notesshot-serial.log"; DUMP = "/tmp/jt-notesshot.raw"
FB = 0xfd000000; W, H = 1920, 1080; PORT = 4456
LOGICAL_W, LOGICAL_H = 960, 540

NOTES_DOCK = (458, 487)   # same coordinate notessharp-check.py's open_notes() clicks
# app-interact-check.py's own DOCK_SLOTS/SLOT0_X formula: Terminal is dock
# index 6 (Apps, Files, Mail, Calendar, Notes, Reminders, Terminal, ...).
_SLOT0_X, _PITCH = 247, 37 + 6
TERMINAL_DOCK = (_SLOT0_X + 6 * _PITCH + 37 // 2, 487)


def run(dock_xy, text, out_path, size_presses=0, family_presses=0):
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
            cmd({"execute": "human-monitor-command", "arguments": {"command-line": f"sendkey {qcode} 30"}})
            time.sleep(0.08)
        def type_text(t):
            punctuation = {' ': 'spc', '.': 'dot', ',': 'comma'}
            for ch in t:
                if ch.isupper():
                    key('shift-' + ch.lower())
                else:
                    key(punctuation.get(ch, ch))
                time.sleep(0.05)
        def full_frame():
            cmd({"execute": "pmemsave", "arguments": {"val": FB, "size": W * H * 4, "filename": DUMP}})
            return Image.frombytes("RGBA", (W, H), open(DUMP, "rb").read(), "raw", "BGRA").convert("RGB")

        move(*dock_xy); time.sleep(0.3); click(); time.sleep(1.2)
        if family_presses or size_presses:
            # Notes needs a click inside the text area to place the caret
            # before typing; Terminal takes keyboard input immediately and
            # a click in its body instead closes the window, so only do
            # this for the Notes capture (family/size presses only apply there).
            move(480, 300); click(); time.sleep(0.3)
        if text:
            type_text(text)
            time.sleep(0.3)
        for _ in range(family_presses):
            key('f1'); time.sleep(0.15)
        for _ in range(size_presses):
            key('f2'); time.sleep(0.15)
        time.sleep(0.5)

        img = full_frame()
        lx, ly, lw, lh = 70, 40, 820, 385
        scale = W / LOGICAL_W
        box = (int(lx * scale), int(ly * scale), int((lx + lw) * scale), int((ly + lh) * scale))
        crop = img.crop(box).resize((960, 553), Image.LANCZOS)
        for q_ in (80, 70, 60, 50, 40):
            crop.save(out_path, "WEBP", quality=q_, method=6)
            size = os.path.getsize(out_path)
            if size < 120 * 1024:
                print(f"saved {out_path} ({size} bytes, quality={q_})")
                break
        else:
            print(f"WARNING: {out_path} still {size} bytes at lowest tried quality")
    finally:
        q.terminate()
        try: q.wait(timeout=5)
        except Exception: q.kill()


if __name__ == "__main__":
    # Bump size a few clicks and switch family once so the tile shows off
    # real, larger TrueType type instead of the old 12pt default.
    run(NOTES_DOCK, "Sharp at every size now.", "landing/shots/app-notes.webp",
        size_presses=4, family_presses=1)
    run(TERMINAL_DOCK, "help", "landing/shots/app-terminal.webp")
