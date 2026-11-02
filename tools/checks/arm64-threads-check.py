#!/usr/bin/env python3
"""8.1.0, threads: a Pi app starts threads that take turns every 10 ms, with locks and condition variables (user/libjt/thread.h).

Boots the ARM64 kernel in QEMU with a card holding two apps built from one C file by tools/mkapp.sh, and runs them from the Terminal:
1. THREADS: two threads each add one to a shared counter 4,000,000 times under a jt_mutex, and to a second counter with no lock;
   the main thread sleeps on a jt_cond until both say they are done, then joins them. The locked total is exactly 8,000,000,
   and the app's line says how many times its threads were cut off. (Its unlocked count is printed, not checked: when a
   slice ends inside the lock the other thread sleeps at once, so how many slices land mid add varies from run to run.)
   It also asks for a thread outside its own code and one on a crooked stack, and both are refused with -EFAULT.
2. NOLOCK: the same file with the lock calls taken out. Its "locked" total is short too, which is what proves the mutex,
   and not luck, made the first total exact.
Discriminating: drop app_preempt (main.c) and there are no slices, so check 1 fails on the slice count and NOLOCK comes
out exact; drop the NZCV restore in resume_user and the counts go wrong or the app faults; make
jt_mutex_lock a no-op and check 1 fails on the locked total.
Skips (exit 0) when clang's aarch64 target, ld.lld, qemu-system-aarch64 or mtools is missing.
Usage: tools/checks/arm64-threads-check.py   (from the repo root)
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
tmp = Path(scratch_dir("arm64-threads"))

fails = []
def check(name, ok, detail=""):
    print(("  ok: " if ok else "  FAIL: ") + name + ("" if ok else "  " + repr(detail)[-900:]))
    if not ok: fails.append(name)

subprocess.run(["make", "-C", str(arch), "clean"], capture_output=True, timeout=60)
r = subprocess.run(["make", "-C", str(arch), "kernel8.elf"], capture_output=True, text=True, timeout=600)
if r.returncode: print(r.stdout[-2000:], r.stderr[-2000:]); print("FAIL: arch/arm64 does not build"); sys.exit(1)

ITER = 4000000
(tmp / "threads.c").write_text(r"""#include "jtsys.h"
#include "libjt/thread.h"
#define ITER %d
static jt_mutex m = JT_MUTEX_INIT;
static jt_cond cv = JT_COND_INIT;
static volatile int locked, racy, finished;
static unsigned char st1[4096] __attribute__((aligned(16))), st2[4096] __attribute__((aligned(16)));
static void say(const char *s) { int n = 0; while (s[n]) n++; jt_syscall(JT_SYS_WRITE, 1, (jt_arg)s, (jt_arg)n); }
static void num(int v) { char b[12]; int n = 11; b[n] = 0; int neg = v < 0; unsigned u = neg ? -v : v; do { b[--n] = '0' + u %% 10; u /= 10; } while (u); if (neg) b[--n] = '-'; say(b + n); }
static void work(void *arg) {
    (void)arg;
    for (int i = 0; i < ITER; i++) {
#ifndef NO_LOCK
        jt_mutex_lock(&m);
#endif
        int v = locked;
        for (volatile int k = 0; k < 3; k++) { }   /* work inside the section: QEMU takes an interrupt only between blocks of code, and this puts block ends between the read and the write */
        locked = v + 1;
#ifndef NO_LOCK
        jt_mutex_unlock(&m);
#endif
        v = racy;
        for (volatile int k = 0; k < 3; k++) { }
        racy = v + 1;
        if (!(i & 0xFFFF)) jt_syscall(JT_SYS_GETPID, 0, 0, 0);   /* a call now and then, or a slow machine's watchdog ends the app */
    }
    jt_mutex_lock(&m); finished++; jt_cond_signal(&cv); jt_mutex_unlock(&m);
}
__attribute__((section(".text.start"), used))
void _start(void) {
    struct jt_thread t1, t2;
    int bad_entry = jt_syscall(JT_SYS_THREAD_SPAWN, 8, (jt_arg)(st1 + sizeof st1), 0);
    int bad_stack = jt_syscall(JT_SYS_THREAD_SPAWN, (jt_arg)(void *)work, (jt_arg)(st1 + sizeof st1 - 8), 0);
    int a = jt_thread_start(&t1, work, 0, st1, sizeof st1), b = jt_thread_start(&t2, work, 0, st2, sizeof st2);
    jt_mutex_lock(&m);
    while (finished < 2) jt_cond_wait(&cv, &m);
    jt_mutex_unlock(&m);
    jt_thread_join(&t1); jt_thread_join(&t2);
    say("threads: spawn "); num(a); say(" "); num(b); say(" bad "); num(bad_entry); say(" "); num(bad_stack);
    say(" locked "); num(locked); say(" racy "); num(racy); say("\n");
    jt_exit(0);
}
""" % ITER)
(tmp / "nolock.c").write_text('#define NO_LOCK 1\n#include "%s"\n' % (tmp / "threads.c"))
for app in ("threads", "nolock"):
    mk = subprocess.run([str(root / "tools/mkapp.sh"), str(tmp / (app + ".c")), str(tmp / (app.upper() + ".APP"))], capture_output=True, text=True, timeout=300)
    if mk.returncode: print(mk.stdout + mk.stderr); print("FAIL: tools/mkapp.sh could not build " + app + ".c"); sys.exit(1)
card = str(tmp / "card.img")
build_card(card, False)
MT = dict(os.environ, MTOOLS_SKIP_CHECK="1")
def mt(*a): return subprocess.run([a[0], "-i", f"{card}@@{PART * 512}", *a[1:]], capture_output=True, env=MT)
if mt("mmd", "::JT").returncode or mt("mmd", "::JT/APPS").returncode: print("FAIL: mtools could not make JT/APPS"); sys.exit(1)
for f in ("THREADS.APP", "NOLOCK.APP"):
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
    QK = {" ": ("spc",)}
    def key(*k): command("send-key", keys=[{"type": "qcode", "data": x} for x in k]); time.sleep(0.12)
    def say(t, until, secs=120):
        n = len(uart())
        for ch in t: key(*QK.get(ch, (ch,)))
        key("ret"); wait_for(until, secs, n); time.sleep(0.5)
        return uart()[n:]
    key("f2")
    if not wait_for("terminal open", 10): raise SystemExit("FAIL: F2 did not open the Terminal: %r" % uart()[-300:])
    time.sleep(0.5)
    LINE = re.compile(r"threads: spawn (-?\d+) (-?\d+) bad (-?\d+) (-?\d+) locked (\d+) racy (\d+)")
    END = re.compile(r"M3c app (\w+) exit (\d+), ran \d+ times")
    CUT = re.compile(r"M3c app threads was cut off (\d+) times")

    out = say("userrun threads", "userrun threads: exit", 300)
    m, e = LINE.search(out), END.search(out)
    check("threads runs to its own exit 0", e and e.group(1) == "threads" and e.group(2) == "0", out[-900:])
    if m:
        a, b, be, bs, lk, rc = map(int, m.groups())
        check("two threads start, numbered 1 and 2", (a, b) == (1, 2), (a, b))
        check("a thread outside the app's code and one on a crooked stack are refused (-EFAULT)", (be, bs) == (-14, -14), (be, bs))
        check("the locked total is exact: %d" % (2 * ITER), lk == 2 * ITER, lk)
    else: check("threads prints its totals", False, out[-900:])
    c = CUT.search(out); sl = int(c.group(1)) if c else 0
    check("its threads took turns: at least 5 slices (%d)" % sl, sl >= 5, out[-400:])

    out = say("userrun nolock", "userrun nolock: exit", 300)
    m = LINE.search(out)
    check("with the lock taken out the 'locked' total is short too (%s)" % (m.group(5) if m else "?"), m and int(m.group(5)) < 2 * ITER, out[-900:])
    check("no kernel crash, and neither app was stopped by a fault", "KERNEL CRASH" not in uart() and "stopped by a fault" not in uart() and q.poll() is None, uart()[-600:])
finally:
    q.kill(); q.wait()
    subprocess.run(["make", "-C", str(arch), "clean"], capture_output=True, timeout=60)
if fails: print("FAIL: %d of the threads checks" % len(fails)); sys.exit(1)
print("PASS: a Pi app's threads take turns every 10 ms; a jt_mutex keeps a shared count exact, and without it the count is short")
