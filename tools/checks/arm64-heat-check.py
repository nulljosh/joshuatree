#!/usr/bin/env python3
"""13.3.0, how hot the chip is (arch/arm64/heat.c, the `temp` command).

Part 1, on the host: heat.c against a fake firmware mailbox: 52143 and 85000 thousandths read "temp: 52.1 C, the chip
slows down at 85.0 C"; a refused temperature tag says the firmware did not say; a refused maximum leaves its clause off.
The copy with no response-bit gate must fail.
Part 2, QEMU: `temp` typed in virt's Terminal says "temp: no sensor on this machine". raspi4b has no keyboard, so the heat
image (-DHEAT_TEST) prints the same line once the screen is up: QEMU's firmware answers a temperature, and the boot goes
on with no crash. The real chip is docs/TESTING.md.

Skips (exit 0) when clang, ld.lld or qemu-system-aarch64 is missing.
Usage: tools/checks/arm64-heat-check.py   (from the repo root)
"""
import json, re, shutil, socket, subprocess, sys, time
from pathlib import Path
from scratch import scratch_dir

ROOT = Path(__file__).resolve().parents[2]
ARCH = ROOT / "arch/arm64"
TMP = Path(scratch_dir("arm64-heat"))
if not all(shutil.which(t) for t in ("clang", "ld.lld", "qemu-system-aarch64")):
    print("SKIP: clang, ld.lld or qemu-system-aarch64 not installed"); sys.exit(0)

HOST = r'''
#include <assert.h>
#include <string.h>
#define HEAT_HOST_TEST
volatile unsigned mbox[36];
static unsigned deny_now, deny_max, calls;
int mbox_call(void) {
    calls++;
    unsigned deny = mbox[2] == 0x00030006u ? deny_now : deny_max;
    mbox[4] = deny ? 8 : 0x80000000u | 8;
    mbox[6] = mbox[2] == 0x00030006u ? 52143 : 85000;
    return 1;
}
#include "heat.c"
int main(void) {
    assert(!strcmp(heat_line(), "temp: 52.1 C, the chip slows down at 85.0 C\n"));
    deny_max = 1; assert(!strcmp(heat_line(), "temp: 52.1 C\n"));
    deny_now = 1; assert(!strcmp(heat_line(), "temp: the firmware did not say\n"));
    return 0;
}
'''
c, exe = TMP / "heat_host.c", TMP / "heat_host"
c.write_text(HOST)
src = (ARCH / "heat.c").read_text()
def host(text):
    (TMP / "heat.c").write_text(text)
    b = subprocess.run(["clang", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined", "-I", str(TMP), str(c), "-o", str(exe)],
                       capture_output=True, text=True)
    if b.returncode: return "build: " + b.stderr[-400:]
    return subprocess.run([str(exe)], capture_output=True, text=True, timeout=10).returncode
r = host(src)
assert r == 0, "heat.c failed on the host: %r" % r
gate = "!(mbox[4] & 0x80000000u) || "
assert gate in src and host(src.replace(gate, "", 1)) != 0, "the copy with no response-bit gate passed"
print("PASS: host: the chip's temperature and its slow-down point to a tenth of a degree; a refused tag says so (the copy with no response-bit gate fails)", flush=True)

subprocess.run(["make", "-s", "-C", str(ARCH), "kernel8.elf", "heat-kernel8.img"], check=True, capture_output=True)

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
    for ch in "temp": key(ch)
    key("ret")
    ok = wait_for("temp: no sensor on this machine", 15, n)
    print("  %s: virt: `temp` in the Terminal says it has no sensor" % ("ok" if ok else "FAIL")); assert ok, uart()[n:][-600:]
    assert "KERNEL CRASH" not in uart()
finally:
    q.kill(); q.wait()

# raspi4b: no keyboard there, so the heat image prints the line once the screen is up
log = TMP / "uart-raspi4b"
if log.exists(): log.unlink()
q = subprocess.Popen(["qemu-system-aarch64", "-machine", "raspi4b", "-display", "none", "-serial", "null", "-serial", "file:" + str(log),
                      "-kernel", str(ARCH / "heat-kernel8.img")], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
try:
    wait_for("boot done", 90)
    out = uart()
finally:
    q.kill(); q.wait()
lines = [l for l in out.splitlines() if l.startswith("temp:")]
ok = len(lines) == 1 and re.fullmatch(r"temp: \d+\.\d C(, the chip slows down at \d+\.\d C)?", lines[0]) is not None
print("  %s: raspi4b's firmware answers a temperature: %s" % ("ok" if ok else "FAIL", lines)); assert ok
ok = "boot done" in out and "KERNEL CRASH" not in out
print("  %s: the Pi image boots on to the desktop" % ("ok" if ok else "FAIL")); assert ok
print("PASS: `temp` reads the chip's temperature from the firmware on raspi4b, says virt has no sensor, and a refused tag says so", flush=True)
