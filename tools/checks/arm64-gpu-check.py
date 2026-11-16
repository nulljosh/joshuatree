#!/usr/bin/env python3
"""13.2.0, the V3D GPU brought up by hand (arch/arm64/gpu.c, the `gpu` command).

Part 1, on the host: gpu.c runs against a fake firmware mailbox and fake registers. A Pi 4 that answers every step reads
back "gpu: V3D 4.2, 1 core, MMU, L3 cache 0 KB", and one ordered trace of mailbox tags and register writes is Linux's:
the bridge ID, clock 5 on then off, PM_GRAFX with the password and V3DRSTN while the clock is off, clock on at its top
rate, the bridge's master port, then its slave. A tag whose response bit is clear,
a bridge that is not "brdg" and a bridge that never drops ACK each stop it with a line and no V3D register read. Broken
copies (no response-bit gate, no ACK wait) must fail.
Part 2, QEMU: `gpu` typed in virt's Terminal says "gpu: no V3D on this machine". raspi4b has no keyboard, so the gpu image
(-DGPU_TEST) runs it once the screen is up: it has no bridge, so one line ("gpu: no V3D bridge"), no V3D register read,
and the boot goes on to the desktop with no crash.
The real GPU on a real board is docs/TESTING.md.

Skips (exit 0) when clang, ld.lld or qemu-system-aarch64 is missing.
Usage: tools/checks/arm64-gpu-check.py   (from the repo root)
"""
import json, shutil, socket, subprocess, sys, time
from pathlib import Path
from scratch import scratch_dir

ROOT = Path(__file__).resolve().parents[2]
ARCH = ROOT / "arch/arm64"
TMP = Path(scratch_dir("arm64-gpu"))
if not all(shutil.which(t) for t in ("clang", "ld.lld", "qemu-system-aarch64")):
    print("SKIP: clang, ld.lld or qemu-system-aarch64 not installed"); sys.exit(0)

HOST = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define GPU_HOST_TEST
#define PI_BUILD
#define GPU_FLUSH() (void)0
volatile unsigned mbox[36];
static unsigned deny_bit, bridge = 0x62726467u, ack_stuck, grafx = 0x1000;
static unsigned long tr[24][3], nt, ntag, nw; static int v3d_reads;   /* one ordered trace: 'T' tag, 'W' write, 'B' bridge ID read */
static void rec(unsigned long k, unsigned long a, unsigned long v) { if (nt >= 24) return; tr[nt][0] = k; tr[nt][1] = a; tr[nt][2] = v; nt++; }
static unsigned asb_m = 1, asb_s = 1;   /* stopped: REQ_STOP set */
int mbox_call(void) {
    assert(mbox[5] == 5); rec('T', mbox[2], mbox[6]); ntag++;
    mbox[4] = deny_bit ? 8 : 0x80000000u | 8;
    if (mbox[2] == 0x00030004u || mbox[2] == 0x00038002u) mbox[6] = 500000000u;
    return 1;
}
/* the fake V3D's control list engine, after gpu.c below */
static void job_start(unsigned end); static unsigned job_rfc(void); static void job_l2t(unsigned v);
static unsigned long qba;
void dcache_clean(void *p, unsigned long n);
static unsigned GR(unsigned long a) {
    if (a == 0xFEC04138UL) return job_rfc();
    if (a >= 0xFEC00000UL && a < 0xFEC08000UL) v3d_reads++;
    if (a == 0xFEC11020UL) { rec('B', a, 0); return bridge; }
    if (a == 0xFE10010CUL) return grafx;
    if (a == 0xFEC1100CUL) return asb_m | (ack_stuck || (asb_m & 1) ? 2 : 0);
    if (a == 0xFEC11008UL) return asb_s | (ack_stuck || (asb_s & 1) ? 2 : 0);
    if (a == 0xFEC0000CUL) return 0x124;   /* hub IDENT1: TVER 4, REV 2, 1 core */
    if (a == 0xFEC00010UL) return 1u << 8; /* hub IDENT2: MMU */
    return 0;
}
static void GW(unsigned long a, unsigned v) {
    rec('W', a, v); nw++;
    if (a == 0xFE10010CUL) grafx = v & 0xffffff;
    if (a == 0xFEC1100CUL) asb_m = v & 1;
    if (a == 0xFEC11008UL) asb_s = v & 1;
    if (a == 0xFEC04164UL) qba = v;
    if (a == 0xFEC0416cUL) job_start(v);
    if (a == 0xFEC04030UL) job_l2t(v);
}
static unsigned long now_t;
static unsigned long gpu_now(void) { return now_t += 10; }
static unsigned long gpu_hz(void) { return 1000000; }
#include "gpu.c"
/* The fake CLE walks gpu_cl with its own packet table (V3D 4.2 lengths, Mesa's v3d_packet.xml) and runs it only when the
   list is the one Mesa would send: the right packets in the right order, 64x64, the clear colour, RT0 stored raster
   rgba8 at gpu_px. It also wants the list cleaned out of the CPU cache and the L2T flushed before QEA, and QBA before QEA.
   The frame count moves on the third read after (the GPU takes time), and the pixels reach memory only at the L2T clean. */
static int hang, cl_clean, l2t_flushed, pending, done, tmu; static unsigned rfc, rfc_left, job_rgba;
void dcache_clean(void *p, unsigned long n) { if (p == gpu_cl && n == sizeof gpu_cl) cl_clean = 1; }
static unsigned le(const unsigned char *p, int n) { unsigned v = 0; while (n--) v = v << 8 | p[n]; return v; }
static int cl_ok(unsigned end) {
    static const unsigned char want[] = { 121, 121, 121, 121, 122, 124, 26, 29, 25, 27, 124, 26, 29, 27, 19, 13 };
    if (hang || qba != (unsigned)(unsigned long)gpu_cl || end <= qba || end - qba > sizeof gpu_cl || !cl_clean || !l2t_flushed) return 0;
    const unsigned char *p = gpu_cl, *e = gpu_cl + (end - qba); unsigned k = 0;
    while (p < e) {
        int len = *p == 121 || *p == 122 ? 9 : *p == 124 ? 4 : *p == 29 ? 13 : *p == 25 ? 2 : 1;
        if (p + len > e || k >= sizeof want || *p != want[k]) return 0;
        if (k == 0 && ((p[1] & 15) || le(p + 2, 2) != 64 || le(p + 4, 2) != 64)) return 0;   /* common: 64x64 */
        if (k == 1) { if ((p[1] & 15) != 3) return 0; job_rgba = le(p + 2, 4); }               /* clear colours */
        if ((k == 2 && (p[1] & 15) != 1) || (k == 3 && (p[1] & 15) != 2)) return 0;           /* colour, Z/S last */
        if (k == 7 && (p[1] & 15) != 8) return 0;                                               /* first store: none */
        if (k == 8 && p[1] != 3) return 0;                                                      /* clear RTs and Z/S */
        if (k == 12) {                                                                          /* RT0 raster rgba8 */
            unsigned long long w = (unsigned long long)le(p + 5, 4) << 32 | le(p + 1, 4);
            if ((w & 15) || ((w >> 4) & 7) || ((w >> 12) & 63) != 27 || ((w >> 28) & 0xfffff) != 256 || (w >> 48) != 64) return 0;
            if (le(p + 9, 4) != (unsigned)(unsigned long)gpu_px) return 0;
        }
        p += len; k++;
    }
    return k == sizeof want;
}
static void job_start(unsigned end) { if (cl_ok(end)) { pending = 1; rfc_left = 3; } qba = 0; cl_clean = l2t_flushed = 0; }
static unsigned job_rfc(void) { if (pending && !--rfc_left) { pending = 0; done = 1; rfc++; } return rfc; }
static void job_l2t(unsigned v) {
    if (v == 1) l2t_flushed = 1;
    if (v == 0x100 && done) tmu = 1;
    if (v == 5 && done && tmu) { for (unsigned i = 0; i < 64 * 64; i++) gpu_px[i] = job_rgba; done = tmu = 0; }
}
static char said[512];
static void say(const char *s) { strncat(said, s, sizeof said - strlen(said) - 1); }
static void reset(void) { nt = ntag = nw = 0; v3d_reads = 0; said[0] = 0; gpu_up = 0; asb_m = asb_s = 1; grafx = 0x1000; deny_bit = ack_stuck = 0; bridge = 0x62726467u; }
int main(void) {
    reset();
    assert(gpu_bringup(say) == 1);
    assert(strstr(said, "gpu: V3D clock 500 MHz, opening the bridge\n") && strstr(said, "gpu: V3D 4.2, 1 core, MMU, L3 cache 0 KB") && strstr(said, "gpu: ready\n"));
    /* Linux's order: the bridge ID at probe; clock on, off; the reset lifted with the clock off; clock on at its top rate; master, slave */
    unsigned long want[8][3] = { {'B', 0xFEC11020UL, 0}, {'T', 0x00030004u, 0}, {'T', 0x00038001u, 1}, {'T', 0x00038001u, 0},
        {'W', 0xFE10010CUL, 0x5a000000u | 0x1000 | 0x40}, {'T', 0x00038001u, 1}, {'T', 0x00038002u, 500000000u},
        {'W', 0xFEC1100CUL, 0x5a000000u | 2} };
    assert(nt == 12 && !memcmp(tr, want, sizeof want) && tr[8][0] == 'W' && tr[8][1] == 0xFEC11008UL);
    assert(tr[10][1] == 0xFEC04034UL && tr[10][2] == 0 && tr[11][1] == 0xFEC04038UL && tr[11][2] == ~0u);   /* L2T flush range */
    assert(gpu_bringup(say) == 1 && strstr(said, "already up"));
    /* the clear job: twice, two colours, and the writes in Linux's order: L2T flush, slices, QBA, QEA last, TMU, L2T clean */
    for (unsigned c = 0; c < 2; c++) {
        unsigned rgba = c ? 0xff0000ffu : 0xff2c50b5u;
        nt = 0; said[0] = 0;
        assert(gpu_clear(say, rgba) == 1);
        char line[64]; snprintf(line, sizeof line, "gpu: the V3D cleared 64x64 pixels to 0x%08x\n", rgba); assert(strstr(said, line));
        for (unsigned i = 0; i < 64 * 64; i++) assert(gpu_px[i] == rgba);
        unsigned long order[6][2] = { {0xFEC04030UL, 1}, {0xFEC04024UL, 0x0f0f0f0fu}, {0xFEC04164UL, (unsigned)(unsigned long)gpu_cl},
            {0xFEC0416cUL, (unsigned)(unsigned long)gpu_cl + gcn}, {0xFEC04030UL, 0x100}, {0xFEC04030UL, 5} };
        assert(nt == 6);
        for (int i = 0; i < 6; i++) assert(tr[i][0] == 'W' && tr[i][1] == order[i][0] && tr[i][2] == order[i][1]);
    }
    hang = 1; said[0] = 0;   /* a V3D that never finishes: the poll times out and says where the CLE stopped */
    assert(gpu_clear(say, 1) == 0 && strstr(said, "gpu: the clear job hung (CT1CS ")); hang = 0;
    reset();
    assert(gpu_clear(say, 1) == 0 && strstr(said, "gpu: not up, run gpu first") && nw == 0 && v3d_reads == 0);
    reset(); deny_bit = 1;
    assert(gpu_bringup(say) == 0 && strstr(said, "refused the V3D clock") && nw == 0 && v3d_reads == 0);
    reset(); bridge = 0;
    assert(gpu_bringup(say) == 0 && strstr(said, "no V3D bridge (id 0x00000000)") && nw == 0 && ntag == 0 && v3d_reads == 0);
    reset(); ack_stuck = 1;
    assert(gpu_bringup(say) == 0 && strstr(said, "bridge did not answer") && v3d_reads == 0);
    assert(gpu_ver(0x124) == 42 && gpu_ver(0x117) == 71);
    puts("host ok");
    return 0;
}
'''
c, exe = TMP / "gpu_host.c", TMP / "gpu_host"
c.write_text(HOST)
src = (ARCH / "gpu.c").read_text()
def host(text):
    (TMP / "gpu.c").write_text(text)
    b = subprocess.run(["clang", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function", "-fsanitize=address,undefined",
                        "-I", str(TMP), str(c), "-o", str(exe)], capture_output=True, text=True)
    if b.returncode: return "build: " + b.stderr[-400:]
    return subprocess.run([str(exe)], capture_output=True, text=True, timeout=10).returncode
r = host(src)
assert r == 0, "gpu.c failed on the host: %r" % r
for name, a, b in (("no response-bit gate", "!(mbox[4] & 0x80000000u) || ", ""),
                   ("no ACK wait", "for (unsigned long t0 = gpu_now(); GR(reg) & 2u;) if (gpu_now() - t0 > gpu_hz() / 1000) return 0;", ""),
                   ("QEA before QBA", "GW(GPU_CORE0 + GPU_CT1QBA, GPU_BUS(gpu_cl));\n    GW(GPU_CORE0 + GPU_CT1QEA, GPU_BUS(gpu_cl) + gcn);",
                    "GW(GPU_CORE0 + GPU_CT1QEA, GPU_BUS(gpu_cl) + gcn);\n    GW(GPU_CORE0 + GPU_CT1QBA, GPU_BUS(gpu_cl));"),
                   ("no END_OF_RENDERING", "cb(27); cb(19); cb(13);", "cb(27); cb(19);"),
                   ("no Z/S clear values", "cb(121); cw(2 | (0x3f800000ull << 16), 8);", ""),
                   ("the list left in the CPU cache", "dcache_clean(gpu_cl, sizeof gpu_cl);", ""),
                   ("no L2T flush first", "GW(GPU_CORE0 + GPU_L2TCACTL, 1);   /* L2TFLS, mode flush */", ""),
                   ("no wait for the frame", "for (unsigned long t0 = gpu_now(); GR(GPU_CORE0 + GPU_RFC) == f0;)", "for (unsigned long t0 = gpu_now(); 0;)"),
                   ("no L2T clean after", "GW(GPU_CORE0 + GPU_L2TCACTL, 1 | (2u << 1));", ""),
                   ("the store at the wrong address", "cw(GPU_BUS(gpu_px), 4);", "cw(GPU_BUS(gpu_px) + 256, 4);"),
                   ("no up check", "if (!gpu_up) { show(\"gpu: not up, run gpu first\\n\"); return 0; }", "")):
    assert a in src, "mutation anchor gone: " + name
    assert host(src.replace(a, b, 1)) != 0, "the copy with %s passed" % name
print("PASS: host: Linux's sequence on a fake Pi 4 reads back V3D 4.2; a refused tag, a missing bridge and a stuck ACK stop "
      "before any V3D register is read; `gpu clear` hands a fake V3D Mesa's clear list and reads its colour back, and refuses "
      "before bring-up (ten broken copies fail)", flush=True)

subprocess.run(["make", "-s", "-C", str(ARCH), "kernel8.elf", "gpu-kernel8.img"], check=True, capture_output=True)

# virt: the real command, typed in the Terminal
log, qmp = TMP / "uart-virt", TMP / "qmp-virt"
for f in (log, qmp):
    if f.exists(): f.unlink()
q = subprocess.Popen(["qemu-system-aarch64", "-machine", "virt", "-cpu", "cortex-a72", "-m", "256", "-nic", "none",
                      "-global", "virtio-mmio.force-legacy=false", "-device", "virtio-keyboard-device", "-device", "ramfb",
                      "-display", "none", "-serial", "file:" + str(log), "-qmp", "unix:%s,server,nowait" % qmp, "-kernel", str(ARCH / "kernel8.elf")],
                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
uart = lambda: log.read_text(errors="replace") if log.exists() else ""
def wait_for(text, secs, after=0):
    end = time.time() + secs
    while time.time() < end:
        if text in uart()[after:]: return True
        time.sleep(0.1)
    return False
try:
    assert wait_for("M2 input ready", 60), "virt did not boot: " + uart()[-400:]
    time.sleep(0.5)
    s = socket.socket(socket.AF_UNIX); s.settimeout(20); s.connect(str(qmp)); f = s.makefile("rw"); f.readline()
    def cmd(c, **a):
        f.write(json.dumps({"execute": c, "arguments": a}) + "\n"); f.flush()
        while True:
            r = json.loads(f.readline())
            if "return" in r or "error" in r: return r
    def key(*k): cmd("send-key", keys=[{"type": "qcode", "data": x} for x in k]); time.sleep(0.1)
    cmd("qmp_capabilities")
    key("f2"); wait_for("terminal open", 10)
    n = len(uart())
    for ch in "gpu": key(ch)
    key("ret")
    ok = wait_for("gpu: no V3D on this machine", 15, n)
    print("  %s: virt: `gpu` in the Terminal says it has no V3D" % ("ok" if ok else "FAIL")); assert ok, uart()[n:][-600:]
    n = len(uart())
    for ch in ("g", "p", "u", "spc", "c", "l", "e", "a", "r"): key(ch)
    key("ret")
    ok = wait_for("gpu: no V3D on this machine", 15, n)
    print("  %s: virt: `gpu clear` says so too" % ("ok" if ok else "FAIL")); assert ok, uart()[n:][-600:]
    assert "KERNEL CRASH" not in uart()
finally:
    q.kill(); q.wait()

# raspi4b: no keyboard there, so the gpu image runs it once the screen is up
log = TMP / "uart-raspi4b"
if log.exists(): log.unlink()
q = subprocess.Popen(["qemu-system-aarch64", "-machine", "raspi4b", "-display", "none", "-serial", "null", "-serial", "file:" + str(log),
                      "-kernel", str(ARCH / "gpu-kernel8.img")], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
try:
    wait_for("boot done", 90)
    out = uart()
finally:
    q.kill(); q.wait()
lines = [l for l in out.splitlines() if l.startswith("gpu:")]
ok = len(lines) == 1 and (lines[0].startswith("gpu: no V3D bridge") or "refused" in lines[0])
print("  %s: raspi4b stops at its first refusal and reads no V3D register: %s" % ("ok" if ok else "FAIL", lines)); assert ok
ok = "boot done" in out and "KERNEL CRASH" not in out
print("  %s: the Pi image boots on to the desktop" % ("ok" if ok else "FAIL")); assert ok
print("PASS: gpu.c brings the V3D up in Linux's order on a fake Pi 4 and stops safely at every refusal; `gpu` on virt says none; raspi4b stops at its first refusal and boots on", flush=True)
