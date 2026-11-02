#!/usr/bin/env python3
"""8.6.0, the desktop comes first: every app runs in 10 ms slices, so a busy one never holds the core for longer than that.

Boots the ARM64 kernel in QEMU with a card holding BUSY (tools/mkapp.sh), a single-threaded app that computes for a second
or more and only asks the time now and then, never yielding. `sched` in the Terminal resets the longest-turn record, then
`userrun busy` runs it to the end, and `sched` again reads the record:
1. busy finishes with the right sum (being cut off and resumed loses nothing),
2. it was cut off at least 10 times (so it ran long enough for the test to mean something),
3. and no app held the core for more than 60 ms in one turn. Before 8.6.0 a single-threaded app kept the core until it
   yielded or the 3 s watchdog fired, so the pointer and the sound stopped for that long.
4. (8.9.0) quiet, a windowless app that computes for more than 3 s with no call at all, is not stopped: only an app with a
   window is frozen after 3 s; one without (plankc compiling) gets 30 s. Before, a slow CI runner stopped plankc mid-build.
Discriminating: put wd_arm back to 3 s for a lone thread (8.1.0's rule) and busy holds the core for its whole run (3).
Skips (exit 0) when clang's aarch64 target, ld.lld, qemu-system-aarch64 or mtools is missing.
Usage: tools/checks/arm64-sched-check.py   (from the repo root)
"""
import json, os, re, shutil, socket, subprocess, sys, time
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from scratch import scratch_dir
from sdcard import PART, build_card

root = Path(__file__).resolve().parents[2]
arch = root / "arch/arm64"
if not all(shutil.which(t) for t in ("clang", "ld.lld", "qemu-system-aarch64", "mmd", "mcopy")):
    print("SKIP: clang, ld.lld, qemu-system-aarch64 or mtools not installed"); sys.exit(0)
tmp = Path(scratch_dir("arm64-sched"))

fails = []
def check(name, ok, detail=""):
    print(("  ok: " if ok else "  FAIL: ") + name + ("" if ok else "  " + repr(detail)[-900:]))
    if not ok: fails.append(name)

subprocess.run(["make", "-C", str(arch), "clean"], capture_output=True, timeout=60)
r = subprocess.run(["make", "-C", str(arch), "kernel8.elf"], capture_output=True, text=True, timeout=600)
if r.returncode: print(r.stdout[-2000:], r.stderr[-2000:]); print("FAIL: arch/arm64 does not build"); sys.exit(1)

(tmp / "busy.c").write_text(r"""#include "jtsys.h"
static void say(const char *s) { int n = 0; while (s[n]) n++; jt_syscall(JT_SYS_WRITE, 1, (jt_arg)s, (jt_arg)n); }
static void num(unsigned v) { char b[12]; int n = 11; b[n] = 0; do { b[--n] = '0' + v % 10; v /= 10; } while (v); say(b + n); }
__attribute__((section(".text.start"), used))
void _start(void) {
    unsigned sum = 0;
    for (unsigned i = 0; i < 120000000u; i++) {
        sum += i ^ (i >> 3);
        if (!(i & 0xFFFFF)) jt_syscall(JT_SYS_TIME, 0, 0, 0);   /* a call now and then, never a yield: the watchdog stays quiet */
    }
    say("busy: sum "); num(sum); say("\n");
    jt_exit(0);
}
""")
(tmp / "quiet.c").write_text(r"""#include "jtsys.h"
static void say(const char *s) { int n = 0; while (s[n]) n++; jt_syscall(JT_SYS_WRITE, 1, (jt_arg)s, (jt_arg)n); }
__attribute__((section(".text.start"), used))
void _start(void) {
    volatile unsigned sum = 0;
    for (unsigned i = 0; i < QUIET_N; i++) sum += i;   /* no call at all, for longer than 3 s */
    say(sum == (unsigned)((unsigned long)QUIET_N * (QUIET_N - 1) / 2) ? "quiet: sum ok\n" : "quiet: sum bad\n");
    jt_exit(0);
}
""".replace("QUIET_N", "2400000000u"))
def busy_sum():   # busy.c's sum, in chunks so it stays a few seconds
    total = 0
    for base in range(0, 120000000, 4000000):
        total += sum(i ^ (i >> 3) for i in range(base, base + 4000000))
    return total & 0xFFFFFFFF
WANT = busy_sum()
for app in ("busy", "quiet"):
    mk = subprocess.run([str(root / "tools/mkapp.sh"), str(tmp / (app + ".c")), str(tmp / (app.upper() + ".APP"))], capture_output=True, text=True, timeout=300)
    if mk.returncode: print(mk.stdout + mk.stderr); print("FAIL: tools/mkapp.sh could not build " + app + ".c"); sys.exit(1)
card = str(tmp / "card.img")
build_card(card, False)
MT = dict(os.environ, MTOOLS_SKIP_CHECK="1")
def mt(*a): return subprocess.run([a[0], "-i", f"{card}@@{PART * 512}", *a[1:]], capture_output=True, env=MT)
if mt("mmd", "::JT").returncode or mt("mmd", "::JT/APPS").returncode: print("FAIL: mtools could not make JT/APPS"); sys.exit(1)
for f in ("BUSY.APP", "QUIET.APP"):
    if mt("mcopy", "-o", str(tmp / f), "::JT/APPS/" + f).returncode: print("FAIL: mtools could not copy " + f); sys.exit(1)

log, sock = tmp / "uart", tmp / "qmp"
q = subprocess.Popen(["qemu-system-aarch64", "-machine", "virt", "-cpu", "cortex-a72", "-m", "256", "-global", "virtio-mmio.force-legacy=false",
                      "-device", "ramfb", "-device", "virtio-keyboard-device", "-drive", "if=none,id=card,file=%s,format=raw" % card, "-device", "virtio-blk-device,drive=card",
                      "-display", "none", "-serial", "file:" + str(log), "-qmp", "unix:%s,server,nowait" % sock, "-kernel", str(arch / "kernel8.elf")],
                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
def uart(): return log.read_text(errors="replace") if log.exists() else ""
def wait_for(text, secs, after=0):
    end = time.time() + secs
    while time.time() < end:
        if text in uart()[after:]: return True
        if q.poll() is not None: return False
        time.sleep(0.1)
    return False
try:
    if not wait_for("M2 input ready", 60): raise SystemExit("FAIL: did not boot: %r" % uart()[-600:])
    time.sleep(0.5)
    s = socket.socket(socket.AF_UNIX); s.settimeout(30); s.connect(str(sock)); f = s.makefile("rw"); f.readline()
    def command(name, **a):
        f.write(json.dumps({"execute": name, "arguments": a}) + "\n"); f.flush()
        while True:
            rr = json.loads(f.readline())
            if "return" in rr or "error" in rr: return rr
    command("qmp_capabilities")
    QK = {" ": ("spc",), "|": ("shift", "backslash")}
    def key(*k): command("send-key", keys=[{"type": "qcode", "data": x} for x in k]); time.sleep(0.12)
    def say(t, until, secs=120):
        n = len(uart())
        for ch in t: key(*QK.get(ch, (ch,)))
        key("ret"); wait_for(until, secs, n); time.sleep(0.5)
        return uart()[n:]
    key("f2")
    if not wait_for("terminal open", 10): raise SystemExit("FAIL: F2 did not open the Terminal: %r" % uart()[-300:])
    time.sleep(0.5)
    say("sched", "sched:", 10)
    out = say("userrun busy", "userrun busy: exit", 180)
    m = re.search(r"busy: sum (\d+)", out); c = re.search(r"M3c app busy was cut off (\d+) times", out)
    check("busy finishes with the right sum (%d)" % WANT, m and int(m.group(1)) == WANT and "userrun busy: exit 0" in out, out[-600:])
    cuts = int(c.group(1)) if c else 0
    check("it was cut off at least 10 times (%d)" % cuts, cuts >= 10, out[-600:])
    out = say("sched", "sched:", 10)
    w = re.search(r"the longest turn an app took was (\d+) ms", out)
    check("no app held the core for more than 60 ms in one turn (%s ms)" % (w.group(1) if w else "?"), w and int(w.group(1)) <= 60, out[-400:])
    out = say("userrun quiet", "userrun quiet: exit", 120)
    c = re.search(r"M3c app quiet was cut off (\d+) times", out); qc = int(c.group(1)) if c else 0
    check("quiet, with no window, computes past 3 s with no call (%d slices) and is not stopped" % qc,
          "quiet: sum ok" in out and "userrun quiet: exit 0" in out and qc > 300 and "watchdog" not in out, out[-600:])
    check("no kernel crash", "KERNEL CRASH" not in uart() and "stopped by a fault" not in uart() and q.poll() is None, uart()[-600:])
finally:
    q.kill(); q.wait()
    subprocess.run(["make", "-C", str(arch), "clean"], capture_output=True, timeout=60)
if fails: print("FAIL: %d of the scheduler checks" % len(fails)); sys.exit(1)
print("PASS: a busy app that never yields runs in 10 ms slices with the desktop between them, and finishes with the right answer")
