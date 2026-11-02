#!/usr/bin/env python3
"""8.3.0, pipes: `userrun A | userrun B` in the Terminal, what A writes to fd 1 is what B reads from fd 0 (main.c pipe0).

Boots the ARM64 kernel in QEMU with a card holding three apps built by tools/mkapp.sh and runs two pipelines:
1. `userrun gen | userrun wc`: gen writes 600 numbered lines, about 6 KB, more than the 4 KiB pipe holds, so it has to wait
   for wc; wc reads fd 0 to the end and prints how many lines and bytes it got and a checksum of them, which match gen's own.
   Both exit 0, and gen's last line is wc's last.
2. `userrun gen | userrun head`: head reads one chunk and exits; gen's next write finds nobody reading and gets -EPIPE,
   and gen exits with 32.
Discriminating: drop park_retry (return 0 on a full pipe) and wc gets fewer bytes than gen wrote; never close the writer's
end in app_end and wc never sees the end (no exit line); never close the reader's end and gen never gets -EPIPE.
Skips (exit 0) when clang's aarch64 target, ld.lld, qemu-system-aarch64 or mtools is missing.
Usage: tools/checks/arm64-pipe-check.py   (from the repo root)
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
tmp = Path(scratch_dir("arm64-pipe"))

fails = []
def check(name, ok, detail=""):
    print(("  ok: " if ok else "  FAIL: ") + name + ("" if ok else "  " + repr(detail)[-900:]))
    if not ok: fails.append(name)

subprocess.run(["make", "-C", str(arch), "clean"], capture_output=True, timeout=60)
r = subprocess.run(["make", "-C", str(arch), "kernel8.elf"], capture_output=True, text=True, timeout=600)
if r.returncode: print(r.stdout[-2000:], r.stderr[-2000:]); print("FAIL: arch/arm64 does not build"); sys.exit(1)

COMMON = r"""#include "jtsys.h"
static void out(int fd, const char *s, int n) { jt_syscall(JT_SYS_WRITE, (jt_arg)fd, (jt_arg)s, (jt_arg)n); }
static void say(const char *s) { int n = 0; while (s[n]) n++; out(1, s, n); }
static int fmt(char *b, unsigned v) { char t[12]; int n = 0, k = 0; do t[n++] = '0' + v % 10; while (v /= 10); while (n) b[k++] = t[--n]; return k; }
"""
(tmp / "gen.c").write_text(COMMON + r"""
__attribute__((section(".text.start"), used))
void _start(void) {
    unsigned sum = 0, bytes = 0;
    for (unsigned i = 1; i <= 600; i++) {
        char b[24]; int n = 0;
        b[n++] = 'l'; b[n++] = 'i'; b[n++] = 'n'; b[n++] = 'e'; b[n++] = ' ';
        n += fmt(b + n, i); b[n++] = '\n';
        int r = jt_syscall(JT_SYS_WRITE, 1, (jt_arg)b, (jt_arg)n);
        if (r < 0) jt_exit(-r);
        for (int k = 0; k < n; k++) sum = sum * 31 + (unsigned char)b[k];
        bytes += n;
    }
    char b[64]; int n = 0;
    const char *t = "gen: 600 lines "; while (*t) b[n++] = *t++;
    n += fmt(b + n, bytes); t = " bytes sum "; while (*t) b[n++] = *t++;
    n += fmt(b + n, sum); b[n++] = '\n';
    out(2, b, n);
    jt_exit(0);
}
""")
(tmp / "wc.c").write_text(COMMON + r"""
__attribute__((section(".text.start"), used))
void _start(void) {
    static char buf[100];
    unsigned lines = 0, bytes = 0, sum = 0;
    for (;;) {
        int r = jt_syscall(JT_SYS_READ, 0, (jt_arg)buf, sizeof buf);
        if (r <= 0) break;
        for (int k = 0; k < r; k++) { sum = sum * 31 + (unsigned char)buf[k]; lines += buf[k] == '\n'; }
        bytes += r;
    }
    char b[64]; int n = 0;
    const char *t = "wc: "; while (*t) b[n++] = *t++;
    n += fmt(b + n, lines); t = " lines "; while (*t) b[n++] = *t++;
    n += fmt(b + n, bytes); t = " bytes sum "; while (*t) b[n++] = *t++;
    n += fmt(b + n, sum); b[n++] = '\n';
    out(1, b, n);
    jt_exit(0);
}
""")
(tmp / "head.c").write_text(COMMON + r"""
__attribute__((section(".text.start"), used))
void _start(void) {
    static char buf[64];
    int r = jt_syscall(JT_SYS_READ, 0, (jt_arg)buf, sizeof buf);
    say(r > 0 ? "head: read a chunk\n" : "head: nothing\n");
    jt_exit(0);
}
""")
for app in ("gen", "wc", "head"):
    mk = subprocess.run([str(root / "tools/mkapp.sh"), str(tmp / (app + ".c")), str(tmp / (app.upper() + ".APP"))], capture_output=True, text=True, timeout=300)
    if mk.returncode: print(mk.stdout + mk.stderr); print("FAIL: tools/mkapp.sh could not build " + app + ".c"); sys.exit(1)
card = str(tmp / "card.img")
build_card(card, False)
MT = dict(os.environ, MTOOLS_SKIP_CHECK="1")
def mt(*a): return subprocess.run([a[0], "-i", f"{card}@@{PART * 512}", *a[1:]], capture_output=True, env=MT)
if mt("mmd", "::JT").returncode or mt("mmd", "::JT/APPS").returncode: print("FAIL: mtools could not make JT/APPS"); sys.exit(1)
for f in ("GEN.APP", "WC.APP", "HEAD.APP"):
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
    out = say("userrun gen | userrun wc", "userrun wc: exit")
    g = re.search(r"gen: 600 lines (\d+) bytes sum (\d+)", out); w = re.search(r"wc: (\d+) lines (\d+) bytes sum (\d+)", out)
    check("the Terminal says both are running", "userrun: both running" in out, out[-600:])
    check("wc read every line gen wrote, through a pipe smaller than the output: 600 lines", w and int(w.group(1)) == 600, out[-600:])
    check("the same bytes, in order: wc's count and checksum match gen's", g and w and (w.group(2), w.group(3)) == (g.group(1), g.group(2)) and int(g.group(1)) > 4096,
          (g and g.groups(), w and w.groups()))
    check("both exit 0", "userrun gen: exit 0" in out and "userrun wc: exit 0" in out, out[-600:])
    out = say("userrun gen | userrun head", "userrun gen: exit")
    check("head reads one chunk and ends", "head: read a chunk" in out and "userrun head: exit 0" in out, out[-600:])
    check("gen's next write finds nobody reading: -EPIPE, exit 32", "userrun gen: exit 32" in out, out[-600:])
    check("no kernel crash, nobody stopped by a fault", "KERNEL CRASH" not in uart() and "stopped by a fault" not in uart() and q.poll() is None, uart()[-600:])
finally:
    q.kill(); q.wait()
    subprocess.run(["make", "-C", str(arch), "clean"], capture_output=True, timeout=60)
if fails: print("FAIL: %d of the pipe checks" % len(fails)); sys.exit(1)
print("PASS: userrun A | userrun B carries A's output into B through a 4 KiB pipe, waits when it is full, ends cleanly, and says -EPIPE when B is gone")
