#!/usr/bin/env python3
"""x86-64, slice 2 (13.22.0): the IDT, the PIC and the PIT timer, and the keyboard IRQ, in qemu-system-x86_64.

`make -C arch/x86_64` links kernel64.bin. The serial log must say, in order: M0 done, idt ok, then a timer line once 20
ticks have counted ("JT64 timer 100 Hz, 20 ticks"). Then the check presses the letter a and b through QMP (send-key), and
the log must hold the make scancodes "JT64 key 0x1e" (a) and "JT64 key 0x30" (b), read from port 0x60 by IRQ1. No
exception line may appear.

Discriminating: the timer line is only printed after ticks reach 20, so a PIT or PIC that never fires times out, and the
two keys must give two different scancodes, so a constant does not pass. Skips (exit 0) without clang, ld.lld or
qemu-system-x86_64.
"""
import json, os, shutil, socket, subprocess, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from scratch import scratch_dir
root = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
arch = os.path.join(root, "arch/x86_64")
if not all(shutil.which(t) for t in ("clang", "ld.lld", "qemu-system-x86_64")):
    print("SKIP: clang, ld.lld or qemu-system-x86_64 not installed"); sys.exit(0)
tmp = scratch_dir("x86_64-irq")
fails = []
def check(name, ok, detail=""):
    print(("  ok: " if ok else "  FAIL: ") + name + ("" if ok else "  " + repr(detail)[-500:]), flush=True)
    if not ok: fails.append(name)
r = subprocess.run(["make", "-C", arch, "-B", "kernel64.bin"], capture_output=True, text=True, timeout=300)
if r.returncode: print(r.stdout[-1500:], r.stderr[-1500:]); print("FAIL: arch/x86_64 does not build"); sys.exit(1)
img = os.path.join(arch, "kernel64.bin")

log, sock_path = os.path.join(tmp, "serial"), os.path.join(tmp, "qmp")
q = subprocess.Popen(["qemu-system-x86_64", "-kernel", img, "-vga", "std", "-display", "none", "-no-reboot", "-m", "256",
                      "-serial", "file:" + log, "-qmp", "unix:%s,server,nowait" % sock_path],
                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
def serial():
    return open(log, errors="replace").read() if os.path.exists(log) else ""
def wait_for(text, secs):
    end = time.time() + secs
    while time.time() < end:
        out = serial()
        if text in out or q.poll() is not None: return out
        time.sleep(0.1)
    return serial()
try:
    out = wait_for("JT64 timer 100 Hz", 30)
    lines = [l for l in out.splitlines() if l.startswith("JT64")]
    timer = [l for l in lines if l.startswith("JT64 timer")]
    check("the timer line says 100 Hz after 20 ticks", len(timer) == 1 and timer[0] == "JT64 timer 100 Hz, 20 ticks", lines[-3:])
    order = ["JT64 M0 done", "JT64 idt ok", "JT64 timer 100 Hz, 20 ticks"]
    check("M0 done, idt ok and the timer come in that order", all(w in lines for w in order) and
          [lines.index(w) for w in order] == sorted(lines.index(w) for w in order), lines)
    check("no exception was reported", "JT64 exception" not in out, out[-300:])

    s = socket.socket(socket.AF_UNIX); s.settimeout(20); s.connect(sock_path); f = s.makefile("rw"); f.readline()
    def cmd(c, **a):
        f.write(json.dumps({"execute": c, **({"arguments": a} if a else {})}) + "\n"); f.flush()
        while True:
            rr = json.loads(f.readline())
            if "return" in rr or "error" in rr: return rr
    cmd("qmp_capabilities")
    for key, code in (("a", "0x1e"), ("b", "0x30")):
        cmd("send-key", keys=[{"type": "qcode", "data": key}])
        out = wait_for("JT64 key " + code, 10)
        check("pressing %s gives make scancode %s on port 0x60" % (key, code), ("JT64 key " + code) in out, out[-300:])
finally:
    q.kill(); q.wait()
subprocess.run(["make", "-s", "-C", arch, "clean"], capture_output=True)
if fails: print("FAIL: " + ", ".join(fails)); sys.exit(1)
print("PASS: x86-64 slice 2: the IDT loads, the PIC and PIT run at 100 Hz for 20 ticks, and IRQ1 reads the keyboard (a is 0x1e, b is 0x30)")
