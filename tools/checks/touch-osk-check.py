#!/usr/bin/env python3
"""Headless proof of roadmap 1.9's touch + on-screen keyboard (user/notes.c via libjt/osk.h).

Boots "phone samantha" under -display none, then drives only absolute-pointer
taps (QMP input-send-event abs + left button, which is exactly what a
touchscreen reports): Esc leaves Samantha, then tap the
Notes icon, then the keyboard (Enter opens the folder, 'n' starts a note,
both through QMP key events since the list has no tap targets yet). The
check passes only if the OSK draws ("notes: osk shown"), a tap on its 'q' and 'i'
keys is answered ("notes: osk key q" / "notes: osk key i") (ring-3 markers, sent as the key goes to the editor).

Usage: tools/checks/touch-osk-check.py   (from the repo root, after make kernel.elf)
"""
from freeport import free_port
import json, os, shutil, socket, subprocess, sys, time

PORT = free_port()
LW, LH = 430, 760
LOG = "/tmp/jt-touch-osk.log"
DUMP = "/tmp/jt-touch-osk.bin"
os.chdir(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))

try: os.remove(LOG)
except FileNotFoundError: pass
q = subprocess.Popen(["qemu-system-i386", "-kernel", "kernel.elf", "-display", "none", "-vga", "std",
                      "-qmp", f"tcp:127.0.0.1:{PORT},server,nowait", "-serial", "file:" + LOG,
                      "-append", "phone samantha"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
fail = 0
try:
    time.sleep(1.0)
    s = socket.create_connection(("127.0.0.1", PORT)); f = s.makefile("rw")
    def cmd(o):
        f.write(json.dumps(o) + "\n"); f.flush()
        while True:
            r = json.loads(f.readline())
            if "return" in r or "error" in r: return r
    f.readline(); cmd({"execute": "qmp_capabilities"})
    def key(k):
        cmd({"execute": "send-key", "arguments": {"keys": [{"type": "qcode", "data": k}], "hold-time": 40}}); time.sleep(0.4)
    def move(x, y):
        cmd({"execute": "input-send-event", "arguments": {"events": [
            {"type": "abs", "data": {"axis": "x", "value": int(x * 32768 / LW)}},
            {"type": "abs", "data": {"axis": "y", "value": int(y * 32768 / LH)}}]}})
        time.sleep(0.3)
    def tap(x, y):
        move(x, y)
        cmd({"execute": "input-send-event", "arguments": {"events": [{"type": "btn", "data": {"down": True, "button": "left"}}]}})
        time.sleep(0.15)
        cmd({"execute": "input-send-event", "arguments": {"events": [{"type": "btn", "data": {"down": False, "button": "left"}}]}})
        time.sleep(0.6)
    def wait_log(text, n=1, secs=20):
        end = time.time() + secs
        while time.time() < end:
            if open(LOG, errors="replace").read().count(text) >= n: return True
            time.sleep(0.3)
        return False
    wait_log("samfocus")
    time.sleep(1.5)
    key("esc")             # leave Samantha for the home grid; her chat loop takes keys, not taps
    wait_log("phonehomerepaint", 1, 10)
    time.sleep(1.0)
    move(200, 300)         # first pointer packet primes the backdoor; a touch panel sends one before every tap
    def on_home():
        cmd({"execute": "pmemsave", "arguments": {"val": 0xfd000000 + (80 * 2 * 860 + 301 * 2) * 4, "size": 4, "filename": DUMP}})
        return open(DUMP, "rb").read()[:3] != bytes([0xF6, 0xF8, 0xFA])  # not the cream field: an icon is still there
    for _ in range(4):     # Notes icon (5 columns, 86 px cells, row 0); retried, a first tap can land before the backdoor has a position
        tap(3 * 86 + 43, 90); time.sleep(0.8)
        if not on_home(): break
    time.sleep(1.5)
    key("n")   # new note: the editor (ring-3 Notes lists notes first, so a bare Enter would open an existing note and n would only type)
    wait_log("notes: new=", 1, 8)   # ring-3 Notes made the note and opened its editor
    time.sleep(1.0)
    tap(20, 760 - 200 + 60)   # 'q'
    tap(43 * 7 + 20, 760 - 200 + 60)   # 'i'
    time.sleep(0.5)
    try: cmd({"execute": "quit"})
    except (ConnectionResetError, BrokenPipeError, OSError, ValueError): pass  # QEMU closes the socket on quit; same teardown phone-boot-check uses
finally:
    try: q.wait(timeout=5)
    except subprocess.TimeoutExpired: q.kill()

log = open(LOG, errors="replace").read()
if "bootphone" not in log: fail = 1; print("FAIL: phone flag not parsed")
if "notes: osk shown" not in log: fail = 1; print("FAIL: a tap never opened the on-screen keyboard")
for k in "qi":
    if f"notes: osk key {k}" not in log: fail = 1; print(f"FAIL: tap on '{k}' never reached osk_tap")
if "notes: new=" not in log: fail = 1; print("FAIL: ring-3 Notes never opened a new note")
if not fail: print("PASS: taps opened the OSK and tapped keys reached the editor buffer")
sys.exit(fail)
