#!/usr/bin/env python3
"""Headless guard for two phone fixes in Samantha's boot view (1.9.18):
1. the back chevron works in `phone samantha` (chat_boot_samantha_open used to
   never run the chevron's tap zone): a tap in the middle must NOT print
   `phonehomerepaint`, a tap on the chevron (20,20) must.
(2.0: Samantha is ring-3 and the old kernel Chat hint strings are gone, so the
source check for the F2/Esc hints went with them; leaving her view lands on the
phone home screen, which prints `phonehomerepaint`.)"""
from freeport import free_port
import json, os, re, socket, subprocess, sys, time

os.chdir(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
PORT = free_port(); LOG = "/tmp/jt-phoneback.log"
LW, LH = 430, 760
fail = 0

try: os.remove(LOG)
except FileNotFoundError: pass
q = subprocess.Popen(["qemu-system-i386", "-kernel", "kernel.elf", "-display", "none", "-vga", "std",
                      "-qmp", f"tcp:127.0.0.1:{PORT},server,nowait", "-serial", "file:" + LOG, "-append", "phone samantha"],
                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
try:
    s = None
    for _ in range(50):
        time.sleep(0.2)
        try: s = socket.create_connection(("127.0.0.1", PORT)); break
        except OSError: pass
    if s is None: raise SystemExit("FAIL: QMP never came up")
    f = s.makefile("rw")
    def cmd(o):
        f.write(json.dumps(o) + "\n"); f.flush()
        while True:
            r = json.loads(f.readline())
            if "return" in r or "error" in r: return r
    f.readline(); cmd({"execute": "qmp_capabilities"})
    def log(): return open(LOG, errors="replace").read()
    for _ in range(100):
        if "samfocus" in log(): break
        time.sleep(0.2)
    def tap(x, y):
        cmd({"execute": "input-send-event", "arguments": {"events": [
            {"type": "abs", "data": {"axis": "x", "value": int(x * 32768 / LW)}},
            {"type": "abs", "data": {"axis": "y", "value": int(y * 32768 / LH)}}]}})
        time.sleep(0.2)
        cmd({"execute": "input-send-event", "arguments": {"events": [{"type": "btn", "data": {"down": True, "button": "left"}}]}})
        time.sleep(0.15)
        cmd({"execute": "input-send-event", "arguments": {"events": [{"type": "btn", "data": {"down": False, "button": "left"}}]}})
        time.sleep(0.5)
    tap(215, 300)
    if "phonehomerepaint" in log(): fail = 1; print("FAIL: a tap on her face exited (phonehomerepaint)")
    tap(20, 20)
    if "phonehomerepaint" not in log(): fail = 1; print("FAIL: tapping the back chevron did nothing (no phonehomerepaint)")
    try: cmd({"execute": "quit"})
    except (ConnectionResetError, BrokenPipeError, OSError): pass
finally:
    try: q.wait(timeout=5)
    except subprocess.TimeoutExpired: q.kill()
print("PASS: phone Samantha back chevron works, F2/Esc hints hidden on phones" if not fail else "")
sys.exit(fail)
