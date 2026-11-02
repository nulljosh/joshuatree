#!/usr/bin/env python3
"""8.4.0, apps that start apps: JT_SYS_SPAWN (410) starts another app with words, JT_SYS_WAIT (411) waits for its exit status.

Boots the ARM64 kernel in QEMU with a card holding PARENT and KID (tools/mkapp.sh) and runs `userrun parent`:
1. parent spawns kid with the words "7" and waits: kid prints the words it got, exits 7, and wait returns 7.
2. parent spawns two kids at once ("41" and "42") and waits for them in the opposite order: 42 then 41, each its own.
3. A name nothing answers to is -ENOENT (-2), and waiting for a pid that is not its child is -ECHILD (-10).
4. A kid that faults is reported to its parent as -1.
The kids' lines reach the Terminal, and parent's own summary line comes after every kid's.
Discriminating: leave reap_note out of app_end and the first wait never returns (no summary); drop park_retry from app_wait
and a wait on a running kid answers 0, not its status; skip `ap = me` in app_spawn and the summary is never printed (the
parent's registers go to the kid's slot).
Skips (exit 0) when clang's aarch64 target, ld.lld, qemu-system-aarch64 or mtools is missing.
Usage: tools/checks/arm64-spawn-check.py   (from the repo root)
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
tmp = Path(scratch_dir("arm64-spawn"))

fails = []
def check(name, ok, detail=""):
    print(("  ok: " if ok else "  FAIL: ") + name + ("" if ok else "  " + repr(detail)[-900:]))
    if not ok: fails.append(name)

subprocess.run(["make", "-C", str(arch), "clean"], capture_output=True, timeout=60)
r = subprocess.run(["make", "-C", str(arch), "kernel8.elf"], capture_output=True, text=True, timeout=600)
if r.returncode: print(r.stdout[-2000:], r.stderr[-2000:]); print("FAIL: arch/arm64 does not build"); sys.exit(1)

COMMON = r"""#include "jtsys.h"
static void say(const char *s) { int n = 0; while (s[n]) n++; jt_syscall(JT_SYS_WRITE, 1, (jt_arg)s, (jt_arg)n); }
static void num(int v) { char b[12]; int n = 11; b[n] = 0; int neg = v < 0; unsigned u = neg ? -v : v; do { b[--n] = '0' + u % 10; u /= 10; } while (u); if (neg) b[--n] = '-'; say(b + n); }
"""
(tmp / "kid.c").write_text(COMMON + r"""
__attribute__((section(".text.start"), used))
void _start(void) {
    char w[32]; int n = jt_args(w, sizeof w - 1); if (n < 0) n = 0; w[n] = 0;
    say("kid: got "); say(w); say("\n");
    if (w[0] == 'f') { *(volatile int *)8 = 1; }   /* "fault": a store to a page it does not have */
    int v = 0; for (int i = 0; w[i] >= '0' && w[i] <= '9'; i++) v = v * 10 + (w[i] - '0');
    for (int i = 0; i < 3; i++) jt_syscall(JT_SYS_SCHED_YIELD, 0, 0, 0);   /* still running when its parent first waits */
    jt_exit(v);
}
""")
(tmp / "parent.c").write_text(COMMON + r"""
__attribute__((section(".text.start"), used))
void _start(void) {
    int p1 = jt_spawn("kid", "7"); int s1 = jt_wait(p1);
    int pa = jt_spawn("kid", "41"), pb = jt_spawn("kid", "42");
    int sb = jt_wait(pb), sa = jt_wait(pa);
    int bad = jt_spawn("nosuch", 0), bogus = jt_wait(9999);
    int pf = jt_spawn("kid", "fault"); int sf = jt_wait(pf);
    say("parent: first "); num(s1); say(" then "); num(sb); say(" "); num(sa);
    say(" nosuch "); num(bad); say(" bogus "); num(bogus); say(" fault "); num(sf);
    say(" pids "); num(p1 > 0 && pa > p1 && pb > pa && pf > pb); say("\n");
    jt_exit(0);
}
""")
for app in ("kid", "parent"):
    mk = subprocess.run([str(root / "tools/mkapp.sh"), str(tmp / (app + ".c")), str(tmp / (app.upper() + ".APP"))], capture_output=True, text=True, timeout=300)
    if mk.returncode: print(mk.stdout + mk.stderr); print("FAIL: tools/mkapp.sh could not build " + app + ".c"); sys.exit(1)
card = str(tmp / "card.img")
build_card(card, False)
MT = dict(os.environ, MTOOLS_SKIP_CHECK="1")
def mt(*a): return subprocess.run([a[0], "-i", f"{card}@@{PART * 512}", *a[1:]], capture_output=True, env=MT)
if mt("mmd", "::JT").returncode or mt("mmd", "::JT/APPS").returncode: print("FAIL: mtools could not make JT/APPS"); sys.exit(1)
for f in ("KID.APP", "PARENT.APP"):
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
    out = say("userrun parent", "userrun parent: exit", 60)
    m = re.search(r"parent: first (-?\d+) then (-?\d+) (-?\d+) nosuch (-?\d+) bogus (-?\d+) fault (-?\d+) pids (\d)", out)
    check("parent prints its summary and exits 0", m and "userrun parent: exit 0" in out, out[-900:])
    if m:
        s1, sb, sa, bad, bogus, sf, pids = map(int, m.groups())
        check("a kid spawned with '7' exits 7, and wait says so", s1 == 7, s1)
        check("two kids at once, waited in the other order: 42 then 41", (sb, sa) == (42, 41), (sb, sa))
        check("no such app: -ENOENT; not its child: -ECHILD", (bad, bogus) == (-2, -10), (bad, bogus))
        check("a kid that faults is reported as -1", sf == -1, sf)
        check("each spawn has a new, larger pid", pids == 1, pids)
    check("each kid got its words and printed them in the Terminal", all("kid: got " + w in out for w in ("7", "41", "42", "fault")), out[-900:])
    check("the parent's summary comes after every kid's line", m and out.index("parent: first") > max(out.index("kid: got " + w) for w in ("7", "41", "42")), out[-900:])
    check("no kernel crash", "KERNEL CRASH" not in uart() and q.poll() is None, uart()[-600:])
finally:
    q.kill(); q.wait()
    subprocess.run(["make", "-C", str(arch), "clean"], capture_output=True, timeout=60)
if fails: print("FAIL: %d of the spawn checks" % len(fails)); sys.exit(1)
print("PASS: an app spawns apps with words and waits for each exit status, in any order, and gets plain errors for a bad name or pid")
