#!/usr/bin/env python3
"""1.9.24: a kernel page table that appears AFTER a ring-3 task was created
must be visible on that task's own CR3.

Every ring-3 window task gets a by-value copy of the kernel page directory
at task_create. Before this fix, kernel heap growth into a 4 MB region that
had no page table at that moment added a PDE only to the shared directory;
the task's copy stayed stale and the first syscall or IRQ on its CR3 that
touched the new memory page-faulted at ring 0 (Mail's close inside
r3win_release). kernel/paging.c now writes every kernel PDE change through
to every live task directory (paging_sync_task_dirs) and
paging_check_task_dirs logs a BUG line if any copy drifts.

Boots `open=remi stress=pde` (kernel/r3stress.c, dead unless armed). Once
Reminders is up as a ring-3 window the desktop kmallocs 4 KB blocks until
one lands past a 4 MB line, hands it to the syscall side, and every
SYS_WINDOW_POLL from the window writes and reads it on the task's CR3.
Then Esc closes the app, the normal release path.

Asserts, from serial:
  1. the flag armed and Reminders launched at ring 3 as a window;
  2. the heap crossed into a new region and the task directories were in
     sync right after (no BUG, no "drifted");
  3. the ring-3 side touched the new region through the gate, readback
     matched, "pdestress: done";
  4. Esc releases the window ("syscall: window released, task gone");
  5. no ring-0 exception, no BUG, no panic, desktop still presenting.

Usage: tools/checks/pdesync-check.py   (from the repo root, after make kernel.elf)
"""
import json, os, socket, subprocess, sys, time

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
os.chdir(ROOT)
LOG = "/tmp/jt-pdesync-serial.log"
try: os.remove(LOG)
except FileNotFoundError: pass
def free_port():
    s = socket.socket(); s.bind(("127.0.0.1", 0)); p = s.getsockname()[1]; s.close(); return p
port = free_port()
q = subprocess.Popen(["qemu-system-i386", "-kernel", "kernel.elf", "-append", "open=remi stress=pde",
                      "-display", "none", "-vga", "std",
                      "-qmp", f"tcp:127.0.0.1:{port},server,nowait", "-serial", "file:" + LOG],
                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
fails = []
def serial():
    try: return open(LOG, errors="replace").read()
    except OSError: return ""
def wait_serial(needle, secs):
    for _ in range(int(secs * 10)):
        s = serial()
        if needle in s: return True
        if "exception at ring 0" in s or "BUG" in s or "PANIC" in s: return False
        time.sleep(0.1)
    return False
try:
    s = None
    for _ in range(50):
        time.sleep(0.2)
        try: s = socket.create_connection(("127.0.0.1", port)); break
        except OSError: pass
    if s is None: sys.exit("FAIL: QEMU's QMP socket never came up")
    f = s.makefile("rw")
    def cmd(o):
        f.write(json.dumps(o) + "\n"); f.flush()
        while True:
            r = json.loads(f.readline())
            if "return" in r or "error" in r: return r
    f.readline(); cmd({"execute": "qmp_capabilities"})
    def press(k):
        cmd({"execute": "send-key", "arguments": {"keys": [{"type": "qcode", "data": k}]}}); time.sleep(0.3)
    if not wait_serial("pdestress: armed", 40): fails.append("stress=pde was not armed")
    if not wait_serial("ring3app: launching REMINDERS.BIN at ring 3 as a window", 40): fails.append("Reminders was not launched on the window path")
    if not wait_serial("pdestress: heap crossed into a new 4 MB region", 60): fails.append("the desktop never grew the heap past a 4 MB line")
    if not wait_serial("pdestress: done", 30): fails.append("the ring-3 side never finished touching the new region (page fault on a stale task directory?)")
    s_ = serial()
    if "pdestress: task directories in sync" not in s_: fails.append("paging_check_task_dirs found a stale task directory")
    if "pdestress: syscall touched the new region" not in s_: fails.append("the syscall side never touched the new region")
    if "pdestress: readback mismatch" in s_: fails.append("the task read back a different value than it wrote")
    press("esc")
    if not wait_serial("syscall: window released, task gone", 10): fails.append("Esc did not release the window cleanly")
    time.sleep(1.0)
    s_ = serial()
    for bad in ("exception at ring 0", "BUG", "PANIC", "Kernel panic", "drifted"):
        if bad in s_: fails.append("serial has " + bad); break
    if "autoopen: back on the desktop" not in s_: fails.append("the desktop loop never resumed")
finally:
    q.kill(); q.wait()
if fails:
    print("FAIL: pdesync"); [print("  - " + f) for f in fails]; sys.exit(1)
print("OK: pdesync: kernel PDE born after the ring-3 task was visible on its CR3, close clean, desktop alive")
