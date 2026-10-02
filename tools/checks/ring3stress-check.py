#!/usr/bin/env python3
"""1.9.23: ring-3 window beside the desktop, both hammering the heap and
the filesystem at once, and the heap must come out intact.

Boots with `open=remi stress=r3`. Reminders runs as a ring-3 window task
(preemptive, int $32 timer) and calls SYS_WINDOW_POLL in a tight loop;
with stress=r3 every one of those calls does a kmalloc/stamp/verify/free
round and a FAT replace+read of R3SYS.TXT under the gate (kernel/
r3stress.c). The desktop loop (task 0, interrupts on) does 256 heap rounds
and a FAT write inside NOTES/ on every frame for 250 frames, then walks
the heap (kheap_check) and prints a verdict.

Asserts, from serial:
  1. both sides actually ran ("r3stress: syscall side ran");
  2. no stamp mismatch (a block handed to two owners at once);
  3. "kheap: ok", never "kheap: CORRUPT";
  4. the syscall's file landed at the root and never inside NOTES/, where
     task 0 was standing when the tick hit;
  5. no ring-0 exception, no BUG, no panic, and the desktop reaches
     "r3stress: done" so it is alive at the end.

Discriminating: without the cli sections in kheap.c/vfs.c (and the cwd
save in syscall.c) the pre-fix kernel corrupts the free list or lands
R3SYS.TXT in NOTES/ within the first few seconds.

Usage: tools/checks/ring3stress-check.py   (from the repo root, after make kernel.elf)
"""
import os, subprocess, sys, time

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
os.chdir(ROOT)
LOG = "/tmp/jt-ring3stress-serial.log"
DISK = "/tmp/jt-ring3stress-disk.img"
try: os.remove(LOG)
except FileNotFoundError: pass
subprocess.run(["bash", "tools/mkdisk.sh", DISK], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)  # a real FAT disk: the cwd hazard needs directories, ramfs has none
q = subprocess.Popen(["qemu-system-i386", "-kernel", "kernel.elf", "-append", "open=remi stress=r3", "-hda", DISK,
                      "-display", "none", "-vga", "std", "-serial", "file:" + LOG],
                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
fails = []
def serial():
    try: return open(LOG, errors="replace").read()
    except OSError: return ""
def wait_serial(needle, secs):
    for _ in range(int(secs * 10)):
        s = serial()
        if needle in s: return True
        if "kheap: CORRUPT" in s or "BUG" in s or "PANIC" in s: return False
        time.sleep(0.1)
    return False
try:
    if not wait_serial("r3stress: armed", 40): fails.append("stress=r3 was not armed")
    if not wait_serial("ring3app: launching REMINDERS.BIN at ring 3 as a window", 40): fails.append("Reminders was not launched on the window path")
    # 2.0's compositor frame paces the loop at a few frames a second, so the run
    # is bound by frame count, not by heap work: 400 frames took 85 s solo on a
    # fast Mac (and 120 s never sufficed on a CI runner under TCG, no KVM). 250
    # frames of 256 heap rounds plus a FAT write finish in about 55 s solo. The
    # wait is only a ceiling; every assertion below still reads the same serial verdicts.
    if not wait_serial("r3stress: done", 240): fails.append("the desktop never finished its 250 rounds (hung, crashed or corrupt)")
    s = serial()
    if "r3stress: syscall side ran" not in s: fails.append("the ring-3 side never entered the stress hook")
    if "r3stress: stamp mismatches seen" in s or "kheap: CORRUPT stamp" in s: fails.append("a heap block was handed to two owners")
    if "kheap: ok" not in s: fails.append("the heap walk never reported ok")
    if "kheap: CORRUPT" in s: fails.append("heap walk: " + [l for l in s.splitlines() if "kheap: CORRUPT" in l][0])
    if "r3stress: R3SYS.TXT at root" not in s: fails.append("the syscall's file did not land at the root")
    if "r3stress: NOTES clean" not in s: fails.append("the syscall's file leaked into NOTES/ (desktop cwd seen through the gate)")
    for bad in ("exception at ring 0", "BUG", "PANIC", "Kernel panic"):
        if bad in s: fails.append("serial has " + bad); break
finally:
    q.kill(); q.wait()
if fails:
    print("FAIL: ring3stress"); [print("  - " + f) for f in fails]; sys.exit(1)
print("OK: ring3stress: ring-3 window and desktop hammered heap and FAT together, heap intact, desktop alive")
