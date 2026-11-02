#!/usr/bin/env python3
"""8.12.0, swap: a heap page with no frame left in the pool goes to JT/SWAP.BIN on the card and comes back on its next touch.

Builds the ARM64 kernel with a 64-page pool (KFLAGS=-DSWAP_POOL_PAGES=64u -DSWAP_FILE_PAGES=2048u) and boots it on QEMU virt
with a card holding SWAPT.APP (tools/mkapp.sh). The app grows its heap to 4 MiB (1,024 pages, sixteen times the pool), stamps
every page with its own number, reads them all back, then writes 128 KiB of its heap to a file and reads it back into another
part of the heap: the kernel's own access to a buffer pins it, so faulting the buffer in never pushes out its start. Checks:
1. every page reads back its own stamp, and the file round trip matches;
2. `mem` says pages went to the card and came back;
3. no kernel crash and no fault.
Discriminating: take the pool out (heap pages on their own addresses, as in 8.7.0) and `mem` is gone and nothing goes to the
card; drop the eviction's TLB flush or the slot in the entry and the stamps come back wrong; drop the pin and the file is wrong.
Skips (exit 0) when clang's aarch64 target, ld.lld, qemu-system-aarch64 or mtools is missing.
Usage: tools/checks/arm64-swap-check.py   (from the repo root)
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
tmp = Path(scratch_dir("arm64-swap"))

fails = []
def check(name, ok, detail=""):
    print(("  ok: " if ok else "  FAIL: ") + name + ("" if ok else "  " + repr(detail)[-900:]))
    if not ok: fails.append(name)

(tmp / "swapt.c").write_text(r"""#include "jtsys.h"
static void say(const char *s) { int n = 0; while (s[n]) n++; jt_syscall(JT_SYS_WRITE, 1, (jt_arg)s, (jt_arg)n); }
static void num(long v) { char b[24]; int n = 23; b[n] = 0; unsigned long u = v < 0 ? -v : v; do { b[--n] = '0' + u % 10; u /= 10; } while (u); if (v < 0) b[--n] = '-'; say(b + n); }
#define PAGES 1024
__attribute__((section(".text.start"), used))
void _start(void) {
    unsigned long base = (unsigned long)jt_syscall(JT_SYS_BRK, 0, 0, 0);
    if (jt_syscall(JT_SYS_BRK, base + PAGES * 4096UL, 0, 0) != (long)(base + PAGES * 4096UL)) { say("swapt: no heap\n"); jt_exit(1); }
    for (unsigned long i = 0; i < PAGES; i++) { volatile unsigned long *p = (void *)(base + i * 4096); p[0] = i * 0x9E3779B97F4A7C15UL + 7; p[511] = ~i; }
    long bad = 0;
    for (unsigned long i = 0; i < PAGES; i++) { volatile unsigned long *p = (void *)(base + i * 4096); bad += p[0] != i * 0x9E3779B97F4A7C15UL + 7 || p[511] != ~i; }
    unsigned char *src = (unsigned char *)(base + 100 * 4096UL), *dst = (unsigned char *)(base + 600 * 4096UL);
    for (unsigned i = 0; i < 131072; i++) src[i] = (unsigned char)(i * 31 + 5);
    long fbad = -1;
    if (jt_writefile("SWAPT.BIN", src, 131072) == 131072 && jt_readfile("SWAPT.BIN", dst, 131072) == 131072) {   /* the whole-file calls: open stops at 8 KiB */
        fbad = 0; for (unsigned i = 0; i < 131072; i++) fbad += dst[i] != (unsigned char)(i * 31 + 5);
    }
    say("swapt: bad "); num(bad); say(" file "); num(fbad); say("\n");
    jt_exit(0);
}
""")
mk = subprocess.run([str(root / "tools/mkapp.sh"), str(tmp / "swapt.c"), str(tmp / "SWAPT.APP")], capture_output=True, text=True, timeout=300)
if mk.returncode: print(mk.stdout + mk.stderr); print("FAIL: tools/mkapp.sh could not build swapt.c"); sys.exit(1)
card = str(tmp / "card.img")
build_card(card, False)
MT = dict(os.environ, MTOOLS_SKIP_CHECK="1")
def mt(*a): return subprocess.run([a[0], "-i", f"{card}@@{PART * 512}", *a[1:]], capture_output=True, env=MT)
if mt("mmd", "::JT").returncode or mt("mmd", "::JT/APPS").returncode: print("FAIL: mtools could not make JT/APPS"); sys.exit(1)
if mt("mcopy", "-o", str(tmp / "SWAPT.APP"), "::JT/APPS/SWAPT.APP").returncode: print("FAIL: mtools could not copy SWAPT.APP"); sys.exit(1)

LINE = re.compile(r"swapt: bad (-?\d+) file (-?\d+)")
MEM = re.compile(r"mem: (\d+) of (\d+) heap pages in RAM, (\d+) sent to the card, (\d+) read back")

def run(kflags):
    """Build with kflags, boot, run swapt then `mem`; returns (its output, mem's, the whole UART)."""
    subprocess.run(["make", "-C", str(arch), "clean"], capture_output=True, timeout=60)
    r = subprocess.run(["make", "-C", str(arch), "kernel8.elf", "KFLAGS=" + kflags], capture_output=True, text=True, timeout=600)
    if r.returncode: print(r.stdout[-2000:], r.stderr[-2000:]); print("FAIL: arch/arm64 does not build (KFLAGS=%r)" % kflags); sys.exit(1)
    log, sock = tmp / "uart", tmp / "qmp"
    if log.exists(): log.unlink()
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
        out = say("userrun swapt", "userrun swapt: exit", 300)
        cores = say("mem", "read back", 10)
        return out, cores, uart()
    finally:
        q.kill(); q.wait()

try:
    out, mem, whole = run("-DSWAP_POOL_PAGES=64u -DSWAP_FILE_PAGES=2048u")
    m = LINE.search(out)
    check("all 1,024 pages read back their own stamps through a 64-page pool (%s bad)" % (m.group(1) if m else "?"), m and m.group(1) == "0", out[-900:])
    check("a 128 KiB heap buffer goes to a file and back intact (%s bad)" % (m.group(2) if m else "?"), m and m.group(2) == "0", out[-900:])
    k = MEM.search(mem)
    check("`mem` says pages went to the card and came back (%s)" % (k.group(0) if k else "?"), k and k.group(2) == "64" and int(k.group(3)) > 900 and int(k.group(4)) > 900, mem[-600:])
    check("no kernel crash and no fault", "KERNEL CRASH" not in whole and "stopped by a fault" not in whole and "out of memory" not in whole, whole[-600:])
finally:
    subprocess.run(["make", "-C", str(arch), "clean"], capture_output=True, timeout=60)
if fails: print("FAIL: %d of the swap checks" % len(fails)); sys.exit(1)
print("PASS: with a 64-page pool a 4 MiB heap goes out to JT/SWAP.BIN and back intact, and a buffer the kernel reads stays in while it does")
