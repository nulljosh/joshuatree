#!/usr/bin/env python3
"""8.7.0, demand paging for the heap: JT_SYS_BRK only moves the top, and a heap page is mapped and zeroed on its first touch.

Boots the ARM64 kernel in QEMU with a card holding HEAP (tools/mkapp.sh) and runs `userrun heap`. The app asks for 8 MiB
of heap and then, in order: writes at the start, at 1 MiB and at the last word; reads a word at 4 MiB that nothing wrote;
hands the kernel a pointer at 2 MiB for JT_SYS_TIME to store through (so the kernel, not the app, makes the first touch);
shrinks the heap to one page and grows it back to 8 MiB, then reads 1 MiB again. Checked:
1. everything it wrote reads back, the untouched word and the word past the shrink read 0, and the time call succeeded;
2. the kernel says it touched exactly 6 heap pages (start, 1 MiB, the last word, 4 MiB, 2 MiB, and 1 MiB again after the
   shrink), not the 2048 an 8 MiB heap would cost if it were all mapped up front.
Discriminating: map the heap in app_brk_set as before 8.7.0 and the count is 0, not 6; drop heap_fault_span from
user_writable and the time call is a kernel fault; skip the zeroing in heap_fault and the word after the shrink is not 0.
Skips (exit 0) when clang's aarch64 target, ld.lld, qemu-system-aarch64 or mtools is missing.
Usage: tools/checks/arm64-heap-check.py   (from the repo root)
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
tmp = Path(scratch_dir("arm64-heap"))

fails = []
def check(name, ok, detail=""):
    print(("  ok: " if ok else "  FAIL: ") + name + ("" if ok else "  " + repr(detail)[-900:]))
    if not ok: fails.append(name)

subprocess.run(["make", "-C", str(arch), "clean"], capture_output=True, timeout=60)
r = subprocess.run(["make", "-C", str(arch), "kernel8.elf"], capture_output=True, text=True, timeout=600)
if r.returncode: print(r.stdout[-2000:], r.stderr[-2000:]); print("FAIL: arch/arm64 does not build"); sys.exit(1)

(tmp / "heap.c").write_text(r"""#include "jtsys.h"
static void say(const char *s) { int n = 0; while (s[n]) n++; jt_syscall(JT_SYS_WRITE, 1, (jt_arg)s, (jt_arg)n); }
static void num(long v) { char b[24]; int n = 23; b[n] = 0; int neg = v < 0; unsigned long u = neg ? -v : v; do { b[--n] = '0' + u % 10; u /= 10; } while (u); if (neg) b[--n] = '-'; say(b + n); }
#define MB (1024UL * 1024UL)
__attribute__((section(".text.start"), used))
void _start(void) {
    unsigned long base = (unsigned long)jt_syscall(JT_SYS_BRK, 0, 0, 0);
    long top = jt_syscall(JT_SYS_BRK, base + 8 * MB, 0, 0);
    volatile unsigned long *a = (void *)base, *b = (void *)(base + MB), *c = (void *)(base + 8 * MB - 8), *d = (void *)(base + 4 * MB);
    *a = 0x1111; *b = 0x2222; *c = 0x3333;
    unsigned long untouched = *d;
    int t = jt_syscall(JT_SYS_TIME, base + 2 * MB, 0, 0);
    unsigned kt = *(volatile unsigned *)(base + 2 * MB);
    int ok = *a == 0x1111 && *b == 0x2222 && *c == 0x3333;
    jt_syscall(JT_SYS_BRK, base + 4096, 0, 0);
    jt_syscall(JT_SYS_BRK, base + 8 * MB, 0, 0);
    unsigned long again = *b;
    say("heap: grew "); num(top == (long)(base + 8 * MB)); say(" kept "); num(ok); say(" untouched "); num((long)untouched);
    say(" time "); num(t >= 0 && kt == (unsigned)t); say(" after shrink "); num((long)again); say("\n");
    jt_exit(0);
}
""")
mk = subprocess.run([str(root / "tools/mkapp.sh"), str(tmp / "heap.c"), str(tmp / "HEAP.APP")], capture_output=True, text=True, timeout=300)
if mk.returncode: print(mk.stdout + mk.stderr); print("FAIL: tools/mkapp.sh could not build heap.c"); sys.exit(1)
card = str(tmp / "card.img")
build_card(card, False)
MT = dict(os.environ, MTOOLS_SKIP_CHECK="1")
def mt(*a): return subprocess.run([a[0], "-i", f"{card}@@{PART * 512}", *a[1:]], capture_output=True, env=MT)
if mt("mmd", "::JT").returncode or mt("mmd", "::JT/APPS").returncode: print("FAIL: mtools could not make JT/APPS"); sys.exit(1)
for f in ("HEAP.APP",):
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
    out = say("userrun heap", "userrun heap: exit", 60)
    m = re.search(r"heap: grew (\d) kept (\d) untouched (-?\d+) time (\d) after shrink (-?\d+)", out)
    check("heap runs to its exit 0 and reports", m and "userrun heap: exit 0" in out, out[-900:])
    if m:
        grew, kept, untouched, tm, after = m.groups()
        check("brk grows the heap to 8 MiB", grew == "1", grew)
        check("what it wrote at the start, at 1 MiB and at the last word reads back", kept == "1", kept)
        check("a word nothing wrote reads 0", untouched == "0", untouched)
        check("the kernel stored the time into a page the app never touched", tm == "1", tm)
        check("after a shrink and a grow, the old page reads 0 again", after == "0", after)
    p = re.search(r"M3c app heap touched (\d+) heap pages", out)
    check("6 heap pages touched, not 2048 (%s)" % (p.group(1) if p else "none said"), p and p.group(1) == "6", out[-900:])
    check("no kernel crash, no fault", "KERNEL CRASH" not in uart() and "stopped by a fault" not in uart() and q.poll() is None, uart()[-600:])
finally:
    q.kill(); q.wait()
    subprocess.run(["make", "-C", str(arch), "clean"], capture_output=True, timeout=60)
if fails: print("FAIL: %d of the heap checks" % len(fails)); sys.exit(1)
print("PASS: an 8 MiB heap costs only the pages the app or the kernel touches, each zeroed on its first touch")
