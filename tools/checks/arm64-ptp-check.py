#!/usr/bin/env python3
"""11.0.0, USB precision touchpads (arch/arm64/ptp.h, fed by xhci.c): a Windows-style PTP report descriptor is found
(fingers, contact count, button, the Input Mode feature) and its reports become the multitouch slots trackpad.h reads.

On the host, ptp.h and trackpad.h are compiled together with a fake kinput that feeds one into the other, as boot.c does:
  - a five-finger PTP descriptor (report ID 1, Input Mode on report ID 3) is a touchpad; a boot mouse's and a touch
    screen's descriptors are not;
  - one finger dragged moves the pointer, two fingers dragged scroll, a quick tap clicks, the pad's own button clicks,
    a finger the contact count drops is lifted, and a report with another ID is ignored.
Two broken copies must fail it. The SET_REPORT that switches a real pad to fingers is a board item (docs/TESTING.md);
QEMU has no USB touchpad. arm64-mouse-check.py and arm64-touch-check.py keep the boot mouse and touch screen honest.

Usage: tools/checks/arm64-ptp-check.py   (from the repo root)
"""
import shutil, subprocess, sys
from pathlib import Path
from scratch import scratch_dir

ROOT = Path(__file__).resolve().parents[2]
TMP = Path(scratch_dir("arm64-ptp"))
if not shutil.which("clang"):
    print("SKIP: clang not installed"); sys.exit(0)

def finger():   # one finger: confidence, tip, 6 pad bits, contact ID (8), X and Y (16 each, 0 to 4095)
    return [0x05, 0x0D, 0x09, 0x22, 0xA1, 0x02,
            0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x01, 0x09, 0x47, 0x81, 0x02, 0x09, 0x42, 0x81, 0x02,
            0x95, 0x06, 0x81, 0x03,
            0x75, 0x08, 0x25, 0x7F, 0x95, 0x01, 0x09, 0x51, 0x81, 0x02,
            0x05, 0x01, 0x26, 0xFF, 0x0F, 0x75, 0x10, 0x95, 0x01, 0x09, 0x30, 0x81, 0x02, 0x09, 0x31, 0x81, 0x02,
            0xC0]
PTP = ([0x05, 0x0D, 0x09, 0x05, 0xA1, 0x01, 0x85, 0x01] + finger() * 5 +
       [0x05, 0x0D, 0x27, 0xFF, 0xFF, 0x00, 0x00, 0x75, 0x10, 0x95, 0x01, 0x09, 0x56, 0x81, 0x02,   # scan time
        0x25, 0x7F, 0x75, 0x08, 0x09, 0x54, 0x81, 0x02,                                           # contact count
        0x05, 0x09, 0x09, 0x01, 0x25, 0x01, 0x75, 0x01, 0x95, 0x01, 0x81, 0x02, 0x95, 0x07, 0x81, 0x03,   # button 1
        0xC0,
        0x05, 0x0D, 0x09, 0x0E, 0xA1, 0x01, 0x85, 0x03, 0x09, 0x22, 0xA1, 0x02,
        0x09, 0x52, 0x15, 0x00, 0x25, 0x0A, 0x75, 0x08, 0x95, 0x01, 0xB1, 0x02, 0xC0, 0xC0])          # Input Mode
MOUSE = [0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0x09, 0x01, 0xA1, 0x00, 0x05, 0x09, 0x19, 0x01, 0x29, 0x03, 0x15, 0x00, 0x25, 0x01,
         0x95, 0x03, 0x75, 0x01, 0x81, 0x02, 0x95, 0x01, 0x75, 0x05, 0x81, 0x01, 0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x15, 0x81,
         0x25, 0x7F, 0x75, 0x08, 0x95, 0x02, 0x81, 0x06, 0xC0, 0xC0]
TOUCHSCREEN = [b if b != 0x05 or i != 3 else 0x04 for i, b in enumerate(PTP)]   # the same layout under Touch Screen (0x0D:0x04)
assert TOUCHSCREEN[3] == 0x04
def carr(name, b): return "static const unsigned char %s[] = { %s };" % (name, ", ".join(str(x) for x in b))

HOST = carr("PTP", PTP) + "\n" + carr("MOUSE", MOUSE) + "\n" + carr("TS", TOUCHSCREEN) + r'''
#include <assert.h>
#include <string.h>
static int ev[256][3], nev;
static unsigned now;
static void tp_emit(unsigned t, unsigned c, int v) { assert(nev < 256); ev[nev][0] = (int)t; ev[nev][1] = (int)c; ev[nev][2] = v; nev++; }
#include "trackpad.h"
static void kinput(unsigned t, unsigned c, int v) {   /* boot.c's input_event: the trackpad first; what it does not take is a mouse event */
    if (!tp_event(t, c, v, now, 800, 600)) tp_emit(t, c, v);
}
#include "ptp.h"
static int sum(int t, int c) { int s = 0; for (int i = 0; i < nev; i++) if (ev[i][0] == t && ev[i][1] == c) s += ev[i][2]; return s; }
static int has(int t, int c, int v) { for (int i = 0; i < nev; i++) if (ev[i][0] == t && ev[i][1] == c && ev[i][2] == v) return 1; return 0; }
static struct ptp P;
static void report(int count, int btn, const int (*f)[4], int nf, int rid) {   /* f: tip, id, x, y */
    unsigned char r[64]; memset(r, 0, sizeof r); r[0] = (unsigned char)rid;
    for (int i = 0; i < nf; i++) { unsigned char *q = r + 1 + 6 * i; q[0] = (unsigned char)(1 | f[i][0] << 1); q[1] = (unsigned char)f[i][1];
        q[2] = (unsigned char)f[i][2]; q[3] = (unsigned char)(f[i][2] >> 8); q[4] = (unsigned char)f[i][3]; q[5] = (unsigned char)(f[i][3] >> 8); }
    r[31] = 0; r[32] = 0; r[33] = (unsigned char)count; r[34] = (unsigned char)btn;
    ptp_report(&P, r, 35);
}
int main(void) {
    struct ptp m, t;
    assert(!ptp_layout(&m, MOUSE, sizeof MOUSE));
    assert(!ptp_layout(&t, TS, sizeof TS));
    assert(ptp_layout(&P, PTP, sizeof PTP));
    assert(P.n == 5 && P.rid == 1 && P.has_mode && P.mode_rid == 3 && P.xmax == 4095 && P.ymax == 4095 && P.cc == 256 && P.btn == 264);
    assert(P.f[1].tip == 49 && P.f[1].x == 64 && P.f[4].y == 224);
    /* one finger: a quarter of the pad across, a quarter of the 800 px screen */
    now = 1000;
    { int f[1][4] = {{1, 7, 1024, 2048}}; report(1, 0, f, 1, 1); }
    for (int i = 1; i <= 4; i++) { now += 30; int f[1][4] = {{1, 7, 1024 + i * 256, 2048}}; report(1, 0, f, 1, 1); }
    int dx = sum(2, 0);
    assert(dx >= 198 && dx <= 202 && !has(1, 272, 1));
    { now += 30; int f[1][4] = {{0, 7, 2048, 2048}}; report(1, 0, f, 1, 1); }
    /* two fingers down the pad: positive notches; then the count drops to one and the other finger lifts */
    nev = 0; now += 500;
    { int f[2][4] = {{1, 1, 1000, 1000}, {1, 2, 3000, 1000}}; report(2, 0, f, 2, 1); }
    for (int i = 1; i <= 6; i++) { now += 30; int f[2][4] = {{1, 1, 1000, 1000 + i * 100}, {1, 2, 3000, 1000 + i * 100}}; report(2, 0, f, 2, 1); }
    assert(sum(2, 8) >= 2 && sum(2, 0) == 0);
    { now += 30; int f[1][4] = {{1, 1, 1000, 1600}}; report(1, 0, f, 1, 1); }
    assert(P.active == 1u << 1);
    { now += 30; int f[1][4] = {{0, 1, 1000, 1600}}; report(1, 0, f, 1, 1); }
    assert(P.active == 0);
    /* a quick tap clicks; the pad's own button clicks; another report ID is not the fingers */
    nev = 0; now += 500;
    { int f[1][4] = {{1, 3, 500, 500}}; report(1, 0, f, 1, 1); now += 60; int g[1][4] = {{0, 3, 500, 500}}; report(1, 0, g, 1, 1); }
    assert(has(1, 272, 1) && has(1, 272, 0));
    nev = 0; now += 500;
    { int f[1][4] = {{1, 4, 500, 500}}; report(1, 1, f, 1, 1); } assert(has(1, 272, 1));
    { now += 500; int f[1][4] = {{0, 4, 500, 500}}; report(1, 0, f, 1, 1); } assert(has(1, 272, 0));
    nev = 0; { int f[1][4] = {{1, 5, 4000, 4000}}; report(1, 0, f, 1, 2); } assert(nev == 0 && P.active == 0);
    return 0;
}
'''
c, exe = TMP / "ptp.c", TMP / "ptp"
c.write_text(HOST)
shutil.copy(ROOT / "arch/arm64/trackpad.h", TMP / "trackpad.h")
header = (ROOT / "arch/arm64/ptp.h").read_text()
def host(text):
    (TMP / "ptp.h").write_text(text)
    b = subprocess.run(["clang", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined", str(c), "-o", str(exe)], capture_output=True, text=True)
    if b.returncode: return "build: " + b.stderr[-400:]
    r = subprocess.run([str(exe)], capture_output=True, text=True, timeout=10)
    return r.returncode if not r.returncode else r.stderr[-300:]
r = host(header)
assert r == 0, "precision touchpad failed on the host: %s" % r
for name, a, b in [("another report ID read as fingers", "if (!len || r[0] != p->rid) return;", "if (!len) return;"),
                   ("a dropped finger never lifts", "if (cnt) for (unsigned s = 0;", "if (0) for (unsigned s = 0;")]:
    assert a in header, "control text moved: " + name
    assert host(header.replace(a, b)) != 0, "broken copy passed: " + name
print("PASS: a precision touchpad's descriptor is found (five fingers, contact count, button, Input Mode on report 3) and a "
      "mouse's or touch screen's is not; its reports drag, scroll, tap, click and lift through trackpad.h (two broken copies fail)", flush=True)
