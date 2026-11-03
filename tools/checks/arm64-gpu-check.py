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
static void rec(unsigned long k, unsigned long a, unsigned long v) { assert(nt < 24); tr[nt][0] = k; tr[nt][1] = a; tr[nt][2] = v; nt++; }
static unsigned asb_m = 1, asb_s = 1;   /* stopped: REQ_STOP set */
int mbox_call(void) {
    assert(mbox[5] == 5); rec('T', mbox[2], mbox[6]); ntag++;
    mbox[4] = deny_bit ? 8 : 0x80000000u | 8;
    if (mbox[2] == 0x00030004u || mbox[2] == 0x00038002u) mbox[6] = 500000000u;
    return 1;
}
static unsigned GR(unsigned long a) {
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
}
static unsigned long now_t;
static unsigned long gpu_now(void) { return now_t += 10; }
static unsigned long gpu_hz(void) { return 1000000; }
#include "gpu.c"
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
    assert(nt == 9 && !memcmp(tr, want, sizeof want) && tr[8][0] == 'W' && tr[8][1] == 0xFEC11008UL);
    assert(gpu_bringup(say) == 1 && strstr(said, "already up"));
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
                   ("no ACK wait", "for (unsigned long t0 = gpu_now(); GR(reg) & 2u;) if (gpu_now() - t0 > gpu_hz() / 1000) return 0;", "")):
    assert a in src, "mutation anchor gone: " + name
    assert host(src.replace(a, b, 1)) != 0, "the copy with %s passed" % name
print("PASS: host: Linux's sequence on a fake Pi 4 reads back V3D 4.2; a refused tag, a missing bridge and a stuck ACK stop "
      "before any V3D register is read (the copies with no response-bit gate and no ACK wait fail)", flush=True)

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
