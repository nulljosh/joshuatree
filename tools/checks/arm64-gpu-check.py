#!/usr/bin/env python3
"""13.2.0, the V3D GPU brought up by hand (arch/arm64/gpu.c, the `gpu` command).

Part 1, on the host: gpu.c runs against a fake firmware mailbox and fake registers. A Pi 4 that answers every step reads
back "gpu: V3D 4.2, 1 core, MMU, L3 cache 0 KB" and the exact writes Linux makes (clock 5 on, off, on, then its top rate;
PM_GRAFX with the password and V3DRSTN; the bridge's master port before its slave). A tag whose response bit is clear,
a bridge that is not "brdg" and a bridge that never drops ACK each stop it with a line and no V3D register read. Broken
copies (no response-bit gate, no ACK wait) must fail.
Part 2, QEMU: virt says "gpu: no V3D on this machine". raspi4b's firmware answers the clock (700 MHz) but has no bridge,
so the image says "gpu: no V3D bridge", reads no V3D register, and boots on to the desktop with no crash.
The real GPU on a real board is docs/TESTING.md.

Skips (exit 0) when clang, ld.lld or qemu-system-aarch64 is missing.
Usage: tools/checks/arm64-gpu-check.py   (from the repo root)
"""
import shutil, subprocess, sys, time
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
volatile unsigned mbox[36];
static unsigned tags[16][3], ntag, deny_bit, bridge = 0x62726467u, ack_stuck, grafx = 0x1000;
static unsigned long writes[8][2], nw; static int v3d_reads;
static unsigned asb_m = 1, asb_s = 1;   /* stopped: REQ_STOP set */
int mbox_call(void) {
    assert(ntag < 16); tags[ntag][0] = mbox[2]; tags[ntag][1] = mbox[5]; tags[ntag][2] = mbox[6]; ntag++;
    mbox[4] = deny_bit ? 8 : 0x80000000u | 8;
    if (mbox[2] == 0x00030004u || mbox[2] == 0x00038002u) mbox[6] = 500000000u;
    return 1;
}
static unsigned GR(unsigned long a) {
    if (a >= 0xFEC00000UL && a < 0xFEC08000UL) v3d_reads++;
    if (a == 0xFEC11020UL) return bridge;
    if (a == 0xFE10010CUL) return grafx;
    if (a == 0xFEC1100CUL) return asb_m | (ack_stuck || (asb_m & 1) ? 2 : 0);
    if (a == 0xFEC11008UL) return asb_s | (ack_stuck || (asb_s & 1) ? 2 : 0);
    if (a == 0xFEC0000CUL) return 0x124;   /* hub IDENT1: TVER 4, REV 2, 1 core */
    if (a == 0xFEC00010UL) return 1u << 8; /* hub IDENT2: MMU */
    return 0;
}
static void GW(unsigned long a, unsigned v) {
    assert(nw < 8); writes[nw][0] = a; writes[nw][1] = v; nw++;
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
static void reset(void) { ntag = nw = 0; v3d_reads = 0; said[0] = 0; gpu_up = 0; asb_m = asb_s = 1; grafx = 0x1000; deny_bit = ack_stuck = 0; bridge = 0x62726467u; }
int main(void) {
    reset();
    assert(gpu_bringup(say) == 1);
    assert(strstr(said, "gpu: V3D clock 500 MHz\n") && strstr(said, "gpu: V3D 4.2, 1 core, MMU, L3 cache 0 KB") && strstr(said, "gpu: ready\n"));
    unsigned want[5][3] = { {0x00030004u,5,0}, {0x00038001u,5,1}, {0x00038001u,5,0}, {0x00038001u,5,1}, {0x00038002u,5,500000000u} };
    assert(ntag == 5 && !memcmp(tags, want, sizeof want));
    assert(writes[0][0] == 0xFE10010CUL && writes[0][1] == (0x5a000000u | 0x1000 | 0x40));
    assert(writes[1][0] == 0xFEC1100CUL && writes[1][1] == (0x5a000000u | 2) && writes[2][0] == 0xFEC11008UL && nw == 3);
    assert(gpu_bringup(say) == 1 && strstr(said, "already up"));
    reset(); deny_bit = 1;
    assert(gpu_bringup(say) == 0 && strstr(said, "refused the V3D clock") && nw == 0 && v3d_reads == 0);
    reset(); bridge = 0;
    assert(gpu_bringup(say) == 0 && strstr(said, "no V3D bridge (id 0x00000000)") && nw == 0 && v3d_reads == 0);
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

subprocess.run(["make", "-s", "-C", str(ARCH), "gpu-kernel8.elf", "gpu-kernel8.img"], check=True, capture_output=True)

def boot(machine, kernel, want, extra=()):
    log = TMP / ("uart-" + machine)
    if log.exists(): log.unlink()
    serial = ["-serial", "null", "-serial", "file:" + str(log)] if machine == "raspi4b" else ["-serial", "file:" + str(log)]
    q = subprocess.Popen(["qemu-system-aarch64", "-machine", machine, *extra, "-display", "none", *serial, "-kernel", str(ARCH / kernel)],
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        t, out = time.time(), ""
        while time.time() - t < 90:
            out = log.read_text(errors="replace") if log.exists() else ""
            if all(w in out for w in want): break
            assert q.poll() is None, "QEMU exited"
            time.sleep(0.3)
        return out
    finally:
        q.kill(); q.wait()

out = boot("virt", "gpu-kernel8.elf", ["gpu: no V3D on this machine"],
           ["-cpu", "cortex-a72", "-m", "256", "-global", "virtio-mmio.force-legacy=false", "-device", "ramfb"])
ok = "gpu: no V3D on this machine" in out and "KERNEL CRASH" not in out
print("  %s: virt says it has no V3D" % ("ok" if ok else "FAIL")); assert ok, out[-600:]

out = boot("raspi4b", "gpu-kernel8.img", ["gpu: no V3D bridge", "boot done"])
lines = [l for l in out.splitlines() if l.startswith("gpu:")]
ok = lines == ["gpu: V3D clock 700 MHz", "gpu: no V3D bridge (id 0x00000000)"]
print("  %s: raspi4b answers the clock and has no bridge: %s" % ("ok" if ok else "FAIL", lines)); assert ok
ok = "boot done" in out and "KERNEL CRASH" not in out
print("  %s: the Pi image boots on to the desktop" % ("ok" if ok else "FAIL")); assert ok
print("PASS: gpu.c brings the V3D up in Linux's order on a fake Pi 4 and stops safely at every refusal; virt has none, raspi4b has no bridge and boots on", flush=True)
