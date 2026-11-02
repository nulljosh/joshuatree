#!/usr/bin/env python3
"""10.2.0, the trackpad (arch/arm64/trackpad.h): fingers in, mouse events out.

Part 1, on the host: trackpad.h compiled with a fake tp_emit. One finger moves the pointer by its share of the screen,
a finger landing does not jump it, two fingers scroll (natural: fingers down is a positive notch), a quick tap is a
left click, a two-finger tap a right click, and a long or moving touch is no click. A finger's x and y may come in separate frames (QEMU does). Three broken copies must fail it.
Part 2, in QEMU virt with a virtio-multitouch device driven over QMP: a drag moves the pointer to the right, a tap
clicks the left button and a two-finger tap the right one.

Skips (exit 0) when clang, ld.lld or qemu-system-aarch64 is missing.
Usage: tools/checks/arm64-trackpad-check.py   (from the repo root)
"""
import json, re, shutil, socket, subprocess, sys, time
from pathlib import Path
from scratch import scratch_dir

ROOT = Path(__file__).resolve().parents[2]
ARCH = ROOT / "arch/arm64"
TMP = Path(scratch_dir("arm64-trackpad"))
if not all(shutil.which(t) for t in ("clang", "ld.lld", "qemu-system-aarch64")):
    print("SKIP: clang, ld.lld or qemu-system-aarch64 not installed"); sys.exit(0)

HOST = r'''
#include <assert.h>
#include <string.h>
static int ev[64][3], nev;
static void tp_emit(unsigned t, unsigned c, int v) { assert(nev < 64); ev[nev][0] = (int)t; ev[nev][1] = (int)c; ev[nev][2] = v; nev++; }
#include "trackpad.h"
static unsigned now;
static void fin(int slot, int id, int x, int y) {   /* one finger's update; id -1 lifts it */
    tp_event(3, 0x2f, slot, now, 800, 600); tp_event(3, 0x39, id, now, 800, 600);
    if (id >= 0) { tp_event(3, 0x35, x, now, 800, 600); tp_event(3, 0x36, y, now, 800, 600); }
}
static void syn(void) { assert(tp_event(0, 0, 0, now, 800, 600) == 1); }
static int sum(int t, int c) { int s = 0; for (int i = 0; i < nev; i++) if (ev[i][0] == t && ev[i][1] == c) s += ev[i][2]; return s; }
static int has(int t, int c, int v) { for (int i = 0; i < nev; i++) if (ev[i][0] == t && ev[i][1] == c && ev[i][2] == v) return 1; return 0; }
int main(void) {
    /* one finger: a quarter of the pad across is a quarter of the screen (200 of 800 px), no click after a long drag */
    now = 1000; fin(0, 1, 8192, 16384); syn(); assert(nev == 0);
    for (int i = 1; i <= 8; i++) { now += 40; fin(0, 1, 8192 + i * 1024, 16384); syn(); }
    assert(sum(2, 0) == 200 && sum(2, 1) == 0);
    now += 40; fin(0, -1, 0, 0); syn(); assert(!has(1, 272, 1));
    /* a second finger landing far away moves nothing; the two then scroll: fingers down 3 notches is +3 */
    nev = 0; now += 500; fin(0, 2, 16384, 8000); syn(); fin(1, 3, 30000, 8000); syn();
    assert(sum(2, 0) == 0 && sum(2, 1) == 0);
    for (int i = 1; i <= 6; i++) { now += 30; fin(0, 2, 16384, 8000 + i * 600); fin(1, 3, 30000, 8000 + i * 600); syn(); }
    assert(sum(2, 8) == 3 && sum(2, 0) == 0 && sum(2, 1) == 0);
    now += 30; fin(0, -1, 0, 0); fin(1, -1, 0, 0); syn(); assert(!has(1, 273, 1));
    /* a quick tap: left click; two fingers: right click; held too long: nothing */
    nev = 0; now += 500; fin(0, 4, 5000, 5000); syn(); now += 80; fin(0, -1, 0, 0); syn();
    assert(has(1, 272, 1) && has(1, 272, 0) && !has(1, 273, 1));
    nev = 0; now += 500; fin(0, 5, 5000, 5000); fin(1, 6, 9000, 5000); syn(); now += 90; fin(0, -1, 0, 0); fin(1, -1, 0, 0); syn();
    assert(has(1, 273, 1) && has(1, 273, 0) && !has(1, 272, 1));
    nev = 0; now += 500; fin(0, 7, 5000, 5000); syn(); now += 400; fin(0, -1, 0, 0); syn(); assert(nev == 0);
    /* QEMU's way: the x in one frame, the y in the next, from a stale y: no move, and still a tap */
    nev = 0; now += 500; tp_event(3, 0x2f, 0, now, 800, 600); tp_event(3, 0x39, 8, now, 800, 600); tp_event(3, 0x35, 20000, now, 800, 600); syn();
    tp_event(3, 0x36, 30000, now, 800, 600); syn(); now += 60; fin(0, -1, 0, 0); syn();
    assert(sum(2, 0) == 0 && sum(2, 1) == 0 && has(1, 272, 1));
    /* not the trackpad's: a mouse report passes through */
    assert(tp_event(2, 0, 5, now, 800, 600) == 0 && tp_event(0, 0, 0, now, 800, 600) == 0);
    return 0;
}
'''
c, exe = TMP / "tp.c", TMP / "tp"
c.write_text(HOST)
header = (ARCH / "trackpad.h").read_text()
def host(text):
    (TMP / "trackpad.h").write_text(text)
    b = subprocess.run(["clang", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined", str(c), "-o", str(exe)], capture_output=True, text=True)
    if b.returncode: return "build: " + b.stderr[-300:]
    return subprocess.run([str(exe)], capture_output=True, text=True, timeout=10).returncode
r = host(header)
assert r == 0, "trackpad gestures failed on the host: %s" % r
for name, a, b in [("a landing finger jumps the pointer", "if (n && n == tp_prev_n) {", "if (n) {"),
                   ("a long touch clicks", "now - tp_t0 < TP_TAP_MS && ", ""),
                   ("a half-landed finger counts", "if (tp_f[i].on && !tp_f[i].need)", "if (tp_f[i].on)")]:
    assert a in header, "control text moved: " + name
    assert host(header.replace(a, b)) != 0, "broken copy passed: " + name
print("PASS: host: one finger moves by its share of the screen, a landing finger does not jump it, two fingers scroll "
      "naturally, a tap clicks left, two fingers right, a long touch nothing, a finger whose y comes a frame late neither moves nor spoils a tap (three broken copies fail)", flush=True)

subprocess.run(["make", "-s", "-C", str(ARCH), "kernel8.elf"], check=True)
log, sock = TMP / "uart", TMP / "qmp"
q = subprocess.Popen(["qemu-system-aarch64", "-machine", "virt", "-cpu", "cortex-a72", "-m", "256", "-global", "virtio-mmio.force-legacy=false",
                      "-device", "ramfb", "-device", "virtio-keyboard-device", "-device", "virtio-multitouch-device", "-display", "none",
                      "-serial", "file:" + str(log), "-qmp", "unix:%s,server,nowait" % sock, "-kernel", str(ARCH / "kernel8.elf")],
                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
def uart(): return log.read_text(errors="replace") if log.exists() else ""
def wait(text, secs, start=0):
    t = time.time()
    while time.time() - t < secs:
        if text in uart()[start:]: return True
        if q.poll() is not None: return False
        time.sleep(0.1)
    return False
f = s = None
try:
    assert wait("M2 input ready", 90), "virt did not boot: " + uart()[-500:]
    s = socket.socket(socket.AF_UNIX); s.settimeout(20); s.connect(str(sock)); f = s.makefile("rw"); f.readline()
    def cmd(name, **a):
        f.write(json.dumps({"execute": name, **({"arguments": a} if a else {})}) + "\n"); f.flush()
        while True:
            r = json.loads(f.readline())
            if "error" in r: raise AssertionError(r)
            if "return" in r: return r
    cmd("qmp_capabilities")
    def mt(kind, slot, tid, axis, v): return {"type": "mtt", "data": {"type": kind, "slot": slot, "tracking-id": tid, "axis": axis, "value": v}}
    def touch(kind, slot, tid, x, y): return [mt(kind, slot, tid, "x", x), mt("data", slot, tid, "x", x), mt("data", slot, tid, "y", y)]
    # a drag to the right
    n = len(uart())
    cmd("input-send-event", events=touch("begin", 0, 1, 8000, 16000))
    for i in range(1, 6): time.sleep(0.04); cmd("input-send-event", events=touch("update", 0, 1, 8000 + i * 2000, 16000))
    cmd("input-send-event", events=[mt("end", 0, 1, "x", 18000)])
    time.sleep(0.6)
    xs = [int(m) for m in re.findall(r"^mouse (\d+),\d+$", uart()[n:], re.M)]
    ok = len(xs) >= 3 and xs == sorted(xs) and xs[-1] - xs[0] >= 150
    print("  %s: a one-finger drag moves the pointer right (%s)" % ("ok" if ok else "FAIL", xs)); assert ok
    assert "key 272 down" not in uart()[n:], "a drag clicked"
    # a tap, then a two-finger tap
    n = len(uart())
    cmd("input-send-event", events=touch("begin", 0, 2, 9000, 9000)); time.sleep(0.05); cmd("input-send-event", events=[mt("end", 0, 2, "x", 9000)])
    ok = wait("key 272 down", 3, n) and wait("key 272 up", 3, n)
    print("  %s: a tap is a left click" % ("ok" if ok else "FAIL")); assert ok, uart()[n:][-400:]
    time.sleep(0.4); n = len(uart())
    cmd("input-send-event", events=touch("begin", 0, 3, 9000, 9000) + touch("begin", 1, 4, 14000, 9000)); time.sleep(0.05)
    cmd("input-send-event", events=[mt("end", 0, 3, "x", 9000), mt("end", 1, 4, "x", 14000)])
    ok = wait("key 273 down", 3, n) and "key 272 down" not in uart()[n:]
    print("  %s: a two-finger tap is a right click" % ("ok" if ok else "FAIL")); assert ok, uart()[n:][-400:]
    assert "KERNEL CRASH" not in uart()
    print("PASS: QEMU virt with virtio-multitouch: drag moves the pointer, tap clicks left, two-finger tap clicks right", flush=True)
finally:
    if f: f.close()
    if s: s.close()
    q.kill(); q.wait()
