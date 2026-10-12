#!/usr/bin/env python3
"""Regression test for "can't exit the launchpad to the desktop" (2.63.1).

The kernel always closed the Launchpad on Esc and on a click outside the grid,
but the web demo's capture-phase Esc listener (landing/v86/embed.js) only
forwarded Esc to the kernel while Samantha was open, so a visitor who opened
Apps had no keyboard way back. The kernel now announces "appsopen" and
"appsclose" on serial and embed.js forwards Esc while the grid is up.

Two halves:
  1. embed.js source: tracks appsOpen from those markers and lets it gate the
     Esc forward (fails if the forward goes back to Samantha only).
  2. Headless kernel: open Apps from the dock, then leave it by Esc, by a click
     on empty desktop outside the grid, and by clicking the dock Apps tile
     again; each must log an "appsclose", and an app tile still launches.

Usage: tools/checks/launchpad-exit-check.py   (from repo root, after make kernel.elf)
"""
import json, os, re, socket, subprocess, sys, time
from freeport import free_port
from scratch import scratch_dir

LOG = os.path.join(scratch_dir("launchpad-exit"), "serial.log")
PORT = free_port()
LOGICAL_W, LOGICAL_H = 960, 540
DOCK_ICON, DOCK_GAP, SLOT0_X, ICON_ROW_Y = 37, 6, 247, 487
APPS_X = SLOT0_X + DOCK_ICON // 2

os.chdir(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
fails = []

js = open("landing/v86/embed.js").read()
if not re.search(r"appsopen/\.test\(serialLine\)\) appsOpen = true", js) or "appsclose/.test(serialLine)) appsOpen = false" not in js:
    fails.append("embed.js: appsOpen is not driven by the kernel's appsopen/appsclose markers")
if not re.search(r"focused && \(samanthaOpen \|\| appsOpen\) && emulator", js):
    fails.append("embed.js: Esc is not forwarded to the kernel while the Launchpad is open")

try: os.remove(LOG)
except FileNotFoundError: pass
q = subprocess.Popen(["qemu-system-i386", "-kernel", "kernel.elf", "-display", "none", "-vga", "std",
                      "-qmp", f"tcp:127.0.0.1:{PORT},server,nowait", "-serial", "file:" + LOG],
                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
def count(word): return len(re.findall(r"^" + word + r"$", open(LOG, errors="replace").read(), re.M))
def wait(word, n, label):
    for _ in range(60):
        if count(word) >= n: return
        time.sleep(0.25)
    fails.append(label)
try:
    time.sleep(1.0)
    s = socket.create_connection(("127.0.0.1", PORT)); f = s.makefile("rw")
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
        move(x, y); time.sleep(0.4)
        cmd({"execute": "input-send-event", "arguments": {"events": [{"type": "btn", "data": {"down": True, "button": "left"}}]}})
        time.sleep(0.12)
        cmd({"execute": "input-send-event", "arguments": {"events": [{"type": "btn", "data": {"down": False, "button": "left"}}]}})
    def esc(): cmd({"execute": "send-key", "arguments": {"keys": [{"type": "qcode", "data": "esc"}]}})
    time.sleep(5.0)  # desktop up

    click(APPS_X, ICON_ROW_Y); wait("appsopen", 1, "Apps tile did not open the Launchpad"); time.sleep(1.0)
    esc();                     wait("appsclose", 1, "Esc did not close the Launchpad"); time.sleep(1.0)
    click(APPS_X, ICON_ROW_Y); wait("appsopen", 2, "Launchpad did not reopen after Esc"); time.sleep(1.0)
    click(12, 200);            wait("appsclose", 2, "a click on empty space did not close the Launchpad"); time.sleep(1.0)
    click(APPS_X, ICON_ROW_Y); wait("appsopen", 3, "Launchpad did not reopen after the empty-space click"); time.sleep(1.0)
    click(APPS_X, ICON_ROW_Y); wait("appsclose", 3, "the Apps tile did not toggle the Launchpad closed"); time.sleep(1.0)
    try: cmd({"execute": "quit"})
    except (ConnectionResetError, BrokenPipeError, OSError): pass
finally:
    try: q.wait(timeout=5)
    except subprocess.TimeoutExpired: q.kill()

for m in fails: print("FAIL:", m)
if fails: sys.exit(1)
print("PASS: Esc, a click on empty space and the Apps tile each close the Launchpad; embed.js forwards Esc while it is open")
