#!/usr/bin/env python3
"""Samantha tells the time and the date from this machine's own clock (2.48.1): the server cannot see the visitor's
time zone, so "what time is it" used to come back "I couldn't find anything on that". The same boot carries `pagebar`,
which the landing page sends on a desktop: the page draws her input (crisp at any size) and she draws none, but she
still reads every key; a boot without it keeps her own bar.

Boots kernel.elf headless with the "samantha" flag and a fixed RTC (-rtc base=2026-10-11T15:07:00, the same local wall
time the landing demo hands the guest), types two questions over QMP, and reads her own chatclock= serial line. No
network: the answer must arrive before any request is made.

Usage: tools/checks/samantha-clock-check.py   (from the repo root, after make kernel.elf)
"""
from freeport import free_port
from scratch import scratch_dir
import json, os, socket, subprocess, sys, time

os.chdir(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
TMP = scratch_dir("samclock")
PORT, LOG = free_port(), os.path.join(TMP, "serial.log")
q = subprocess.Popen(["qemu-system-i386", "-kernel", "kernel.elf", "-display", "none", "-vga", "std", "-append", "samantha pagebar",
                      "-rtc", "base=2026-10-11T15:07:00", "-qmp", f"tcp:127.0.0.1:{PORT},server,nowait", "-serial", "file:" + LOG],
                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
serial = lambda: open(LOG, encoding="latin-1").read() if os.path.exists(LOG) else ""


def wait(pred, secs):
    end = time.time() + secs
    while time.time() < end:
        if pred(): return True
        time.sleep(0.1)
    return False


fail = 0
try:
    wait(lambda: True, 1.0)
    s = None
    for _ in range(50):
        try: s = socket.create_connection(("127.0.0.1", PORT)); break
        except OSError: time.sleep(0.1)
    f = s.makefile("rw"); f.readline()
    def cmd(o):
        f.write(json.dumps(o) + "\n"); f.flush()
        while True:
            r = json.loads(f.readline())
            if "return" in r or "error" in r: return r
    cmd({"execute": "qmp_capabilities"})
    keys = lambda *qc: cmd({"execute": "send-key", "arguments": {"keys": [{"type": "qcode", "data": k} for k in qc]}})
    def ask(text):
        for ch in text: keys({" ": "spc", "'": "apostrophe"}.get(ch, ch)); time.sleep(0.06)
        keys("ret")
    if not wait(lambda: "samfocus" in serial(), 60): fail = 1; print("FAIL: her window never opened (no samfocus)")
    elif "sampagebar=1" not in serial(): fail = 1; print("FAIL: `pagebar` on the boot line never reached her (no sampagebar=1)")
    else:
        ask("what time is it")
        if not wait(lambda: "chatclock=" in serial(), 20): fail = 1; print("FAIL: 'what time is it' got no chatclock= answer")
        else:
            line = serial().split("chatclock=", 1)[1].splitlines()[0]
            if line not in ("It's 3:07 PM.", "It's 3:08 PM."): fail = 1; print(f"FAIL: the time read {line!r}, want It's 3:07 PM.")
            else: print("time:", line)
        ask("what is the date")
        if not wait(lambda: serial().count("chatclock=") >= 2, 20): fail = 1; print("FAIL: 'what is the date' got no chatclock= answer")
        else:
            line = serial().split("chatclock=")[2].splitlines()[0]
            if line != "It's Sunday, October 11, 2026.": fail = 1; print(f"FAIL: the date read {line!r}")
            else: print("date:", line)
    try: cmd({"execute": "quit"})
    except OSError: pass
finally:
    try: q.wait(timeout=5)
    except subprocess.TimeoutExpired: q.kill()

# Without `pagebar` she keeps her own bar.
LOG2 = os.path.join(TMP, "serial-nobar.log")
q = subprocess.Popen(["qemu-system-i386", "-kernel", "kernel.elf", "-display", "none", "-vga", "std", "-append", "samantha", "-serial", "file:" + LOG2],
                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
try:
    seen = wait(lambda: os.path.exists(LOG2) and "samfocus" in open(LOG2, encoding="latin-1").read(), 60)
    text = open(LOG2, encoding="latin-1").read() if os.path.exists(LOG2) else ""
    if not seen: fail = 1; print("FAIL: without pagebar her window never opened")
    elif "sampagebar=" in text: fail = 1; print("FAIL: without pagebar she still hid her own bar")
finally:
    q.kill(); q.wait()

if not fail: print("PASS: Samantha answers the time and the date from her own clock, with no request; pagebar hides only her bar")
sys.exit(fail)
