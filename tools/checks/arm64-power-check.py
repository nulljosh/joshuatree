#!/usr/bin/env python3
"""Pi keyboard confirmation on the host, and real shutdown/restart under raspi4b."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[2]
source = r'''
#include <assert.h>
#include <string.h>
#define POWER_HOST_TEST
static int ctrl_held, alt_held, busy, front, applied, term_p, prompts;
static int ask_pending(void) { return busy; }
static int term_front(void) { return front; }
static void term_output(int on) { (void)on; }
static void uart_puts(const char *s) { if (strstr(s,"confirms")) prompts++; }
static void spot_close(void) {}
static void clock_close(void) {}
static void calc_close(void) {}
static void pane_open(int *p) { (void)p; front=1; }
#include "arch/arm64/power.h"
static void power_apply(int off) { applied=off ? 1 : 2; }
int main(void) {
    assert(!power_key(111)); assert(!applied);
    ctrl_held=1; assert(!power_key(111));
    alt_held=1;
    assert(power_key(111)); assert(power_pending==2 && !applied && prompts==1);
    assert(power_key(30)); assert(!applied); /* stray letters cannot confirm */
    assert(power_key(1)); assert(!power_pending && !applied);
    busy=1; assert(power_key(107)); assert(!power_pending && !applied);
    busy=0; assert(power_key(107)); assert(power_pending==1);
    busy=1; assert(power_key(28)); assert(!applied && !power_pending);
    busy=0; assert(power_key(111)); front=0;
    assert(!power_key(28)); assert(!applied && !power_pending);
    assert(power_key(107)); assert(power_key(28)); assert(applied==1);
    applied=0; assert(power_key(111)); assert(power_key(96)); assert(applied==2);
}
'''
with tempfile.TemporaryDirectory(prefix="jt-power-") as tmp:
    tmp = Path(tmp)
    c = tmp / "power.c"
    c.write_text(source)
    exe = tmp / "power"
    subprocess.run(["clang", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
                    "-I", str(ROOT), str(c), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True, timeout=10)
    print("PASS: chords, explicit confirmation, cancel, busy request and hidden-prompt refusal")
    if not shutil.which("qemu-system-aarch64"):
        raise SystemExit("FAIL: qemu-system-aarch64 is required for Pi power checks")
    subprocess.run(["make", "-C", str(ROOT / "arch/arm64"),
                    "poweroff-kernel8.img", "restart-kernel8.img"], check=True,
                   stdout=subprocess.DEVNULL)
    for name, action in [("poweroff", "shutdown"), ("restart", "restart")]:
        log = tmp / (name + ".log")
        q = subprocess.Popen(["qemu-system-aarch64", "-machine", "raspi4b", "-display", "none",
                              "-serial", "file:" + str(log), "-kernel",
                              str(ROOT / "arch/arm64" / (name + "-kernel8.img"))],
                             stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        try:
            deadline = time.monotonic() + 30
            while time.monotonic() < deadline:
                out = log.read_text(errors="replace") if log.exists() else ""
                if name == "poweroff" and q.poll() is not None:
                    assert q.returncode == 0, q.stderr.read().decode()
                    break
                if name == "restart" and out.count("Joshua Tree on ARM64") >= 2:
                    assert q.poll() is None, "restart exited instead of booting again"
                    break
                assert q.poll() is None, "QEMU exited before power action completed"
                time.sleep(0.1)
            else:
                raise AssertionError(action + " did not complete: " + out[-600:])
            assert "power: cancellation survived" in out, out[-600:]
            assert "power: " + action in out, out[-600:]
            print("PASS: raspi4b " + action + " after cancellation and keyboard confirmation")
        finally:
            if q.poll() is None:
                q.terminate()
            q.wait(timeout=5)
            q.stderr.close()
