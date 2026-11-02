#!/usr/bin/env python3
"""8.11.0, app threads on all four cores: on QEMU virt with -smp 4, a Pi app's threads run on cores 1 to 3 as well as core 0.

Boots the ARM64 kernel with a card holding SMPTHR.APP (tools/mkapp.sh): three threads each add one to a shared count 2,000,000
times under a jt_mutex while the main thread sleeps on a jt_cond, then joins them. Checks:
1. the locked total is exact (6,000,000): the futex calls answered on cores 1 to 3 (main.c core_futex) and on core 0 lose no wake;
2. the kernel says the app ran on a core above 0 ("M3c app smpthr ran on cores 0 1 ...");
3. `cores` in the Terminal says a core ran app slices;
4. the control: the same app on a kernel built with KFLAGS=-DSMP_APPS_OFF (core_pick does nothing) runs on core 0 alone, so
   check 2 is not luck.
Run three times in a row locally: QEMU runs the four vCPUs at once, so a race shows as a flake.
Skips (exit 0) when clang's aarch64 target, ld.lld, qemu-system-aarch64 or mtools is missing.
Usage: tools/checks/arm64-smp-threads-check.py   (from the repo root)
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
tmp = Path(scratch_dir("arm64-smp-threads"))

fails = []
def check(name, ok, detail=""):
    print(("  ok: " if ok else "  FAIL: ") + name + ("" if ok else "  " + repr(detail)[-900:]))
    if not ok: fails.append(name)

ITER = 2000000
(tmp / "smpthr.c").write_text(r"""#include "jtsys.h"
#include "libjt/thread.h"
#define ITER %d
static jt_mutex m = JT_MUTEX_INIT;
static jt_cond cv = JT_COND_INIT;
static volatile int locked, finished;
static unsigned char st[3][4096] __attribute__((aligned(16)));
static void say(const char *s) { int n = 0; while (s[n]) n++; jt_syscall(JT_SYS_WRITE, 1, (jt_arg)s, (jt_arg)n); }
static void num(int v) { char b[12]; int n = 11; b[n] = 0; unsigned u = v; do { b[--n] = '0' + u %% 10; u /= 10; } while (u); say(b + n); }
static void work(void *arg) {
    (void)arg;
    for (int i = 0; i < ITER; i++) {
        jt_mutex_lock(&m);
        int v = locked;
        for (volatile int k = 0; k < 3; k++) { }
        locked = v + 1;
        jt_mutex_unlock(&m);
        for (volatile int k = 0; k < 20; k++) { }   /* work outside the lock, which the cores can do at once */
        if (!(i & 0xFFFF)) jt_syscall(JT_SYS_GETPID, 0, 0, 0);
    }
    jt_mutex_lock(&m); finished++; jt_cond_signal(&cv); jt_mutex_unlock(&m);
}
__attribute__((section(".text.start"), used))
void _start(void) {
    struct jt_thread t[3];
    for (int i = 0; i < 3; i++) jt_thread_start(&t[i], work, 0, st[i], sizeof st[i]);
    jt_mutex_lock(&m);
    while (finished < 3) jt_cond_wait(&cv, &m);
    jt_mutex_unlock(&m);
    for (int i = 0; i < 3; i++) jt_thread_join(&t[i]);
    say("smpthr: locked "); num(locked); say("\n");
    jt_exit(0);
}
""" % ITER)
mk = subprocess.run([str(root / "tools/mkapp.sh"), str(tmp / "smpthr.c"), str(tmp / "SMPTHR.APP")], capture_output=True, text=True, timeout=300)
if mk.returncode: print(mk.stdout + mk.stderr); print("FAIL: tools/mkapp.sh could not build smpthr.c"); sys.exit(1)
card = str(tmp / "card.img")
build_card(card, False)
MT = dict(os.environ, MTOOLS_SKIP_CHECK="1")
def mt(*a): return subprocess.run([a[0], "-i", f"{card}@@{PART * 512}", *a[1:]], capture_output=True, env=MT)
if mt("mmd", "::JT").returncode or mt("mmd", "::JT/APPS").returncode: print("FAIL: mtools could not make JT/APPS"); sys.exit(1)
if mt("mcopy", "-o", str(tmp / "SMPTHR.APP"), "::JT/APPS/SMPTHR.APP").returncode: print("FAIL: mtools could not copy SMPTHR.APP"); sys.exit(1)

LINE = re.compile(r"smpthr: locked (\d+)")
CORES = re.compile(r"M3c app smpthr ran on cores([ \d]+)")
SLICES = re.compile(r"core [123]: up, woken \d+ times, ran (\d+) app slices")

def run(kflags):
    """Build with kflags, boot -smp 4, run smpthr then `cores`; returns (smpthr's output, cores's output, the whole UART)."""
    subprocess.run(["make", "-C", str(arch), "clean"], capture_output=True, timeout=60)
    r = subprocess.run(["make", "-C", str(arch), "kernel8.elf", "KFLAGS=" + kflags], capture_output=True, text=True, timeout=600)
    if r.returncode: print(r.stdout[-2000:], r.stderr[-2000:]); print("FAIL: arch/arm64 does not build (KFLAGS=%r)" % kflags); sys.exit(1)
    log, sock = tmp / "uart", tmp / "qmp"
    if log.exists(): log.unlink()
    q = subprocess.Popen(["qemu-system-aarch64", "-machine", "virt", "-cpu", "cortex-a72", "-smp", "4", "-m", "256", "-global", "virtio-mmio.force-legacy=false",
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
        if not wait_for("M2 input ready", 60): print("FAIL: did not boot: %r" % uart()[-600:]); sys.exit(1)
        time.sleep(0.5)
        s = socket.socket(socket.AF_UNIX); s.settimeout(30); s.connect(str(sock)); f = s.makefile("rw"); f.readline()
        def command(name, **a):
            f.write(json.dumps({"execute": name, "arguments": a}) + "\n"); f.flush()
            while True:
                rr = json.loads(f.readline())
                if "return" in rr or "error" in rr: return rr
        command("qmp_capabilities")
        def key(*k): command("send-key", keys=[{"type": "qcode", "data": x} for x in k]); time.sleep(0.12)
        def say(t, until, secs=120):
            n = len(uart())
            for ch in t: key("spc" if ch == " " else ch)
            key("ret"); wait_for(until, secs, n); time.sleep(0.5)
            return uart()[n:]
        key("f2")
        if not wait_for("terminal open", 10): print("FAIL: F2 did not open the Terminal: %r" % uart()[-300:]); sys.exit(1)
        time.sleep(0.5)
        out = say("userrun smpthr", "userrun smpthr: exit", 300)
        cores = say("cores", "core 3", 10)
        return out, cores, uart()
    finally:
        q.kill(); q.wait()

try:
    out, cores, whole = run("")
    m = LINE.search(out)
    check("the locked total is exact: %d (%s)" % (3 * ITER, m.group(1) if m else "?"), m and int(m.group(1)) == 3 * ITER, out[-900:])
    c = CORES.search(out); on = c.group(1).split() if c else ["0"]
    check("its threads ran on a core above 0 (cores %s)" % " ".join(on), any(x != "0" for x in on), out[-600:])
    sl = [int(x) for x in SLICES.findall(cores)]
    check("`cores` says cores 1 to 3 ran app slices (%s)" % sl, sum(sl) > 0, cores[-600:])
    check("no kernel crash and no fault", "KERNEL CRASH" not in whole and "stopped by a fault" not in whole, whole[-600:])

    out, cores, whole = run("-DSMP_APPS_OFF")
    m = LINE.search(out)
    check("control (SMP_APPS_OFF): the total is still exact", m and int(m.group(1)) == 3 * ITER, out[-900:])
    check("control (SMP_APPS_OFF): it ran on core 0 alone, so the line above is the change's", "userrun smpthr: exit" in out and not CORES.search(out), out[-600:])
finally:
    subprocess.run(["make", "-C", str(arch), "clean"], capture_output=True, timeout=60)
if fails: print("FAIL: %d of the four-core thread checks" % len(fails)); sys.exit(1)
print("PASS: a Pi app's threads run on cores 1 to 3 too, a jt_mutex keeps their shared count exact, and with the pickup built out they stay on core 0")
