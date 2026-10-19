#!/usr/bin/env python3
"""Pi keyboard confirmation on the host, and real shutdown/restart under raspi4b.

4.16.0 adds the typed `reboot` and `shutdown` with a file on a real FAT32 card (powerfile build): boot 1 leaves a file
unsaved in an open descriptor (as Notes is mid-save) and types `reboot`; boot 2 reads it back (it must have survived) and
types `shutdown`, which paints the "safe to unplug" screen and exits QEMU. The powernoflush build skips the flush and must
lose the file, so the check fails when the flush is taken out. The tree menu's Restart and Shut Down rows are clicked in
arm64-treemenu-check.py; they and the chords and the commands all end in the same power_apply."""
from pathlib import Path
import shutil
import sys
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(Path(__file__).resolve().parent))
from sdcard import build_card
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
static void brick_close(void) {}
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
        q = subprocess.Popen(["qemu-system-aarch64", "-machine", "raspi4b", "-serial", "null", "-display", "none",
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

    # ---- the typed commands and the flush, on a FAT32 card that lives across the reboot ----
    subprocess.run(["make", "-C", str(ROOT / "arch/arm64"), "powerfile-kernel8.img", "powernoflush-kernel8.img"],
                   check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    def typed(name):
        free = shutil.disk_usage(str(tmp)).free
        assert free > 5 << 30, "disk under 5 GB free, stopping"
        card = tmp / (name + "-card.img")
        build_card(str(card), False)
        log = tmp / (name + "-file.log")
        q = subprocess.Popen(["qemu-system-aarch64", "-machine", "raspi4b", "-serial", "null", "-display", "none",
                              "-serial", "file:" + str(log),
                              "-drive", "if=none,id=card,file=%s,format=raw" % card,
                              "-device", "sd-card,drive=card,bus=/generic-sdhci/sd-bus",
                              "-kernel", str(ROOT / "arch/arm64" / (name + "-kernel8.img"))],
                             stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        try:
            q.wait(timeout=240)
        except subprocess.TimeoutExpired:
            q.kill()
            raise AssertionError(name + " never reached shutdown: " + log.read_text(errors="replace")[-800:])
        finally:
            if q.poll() is None:
                q.kill()
            q.wait(timeout=5)
        return q.returncode, log.read_text(errors="replace")

    rc, out = typed("powerfile")
    assert rc == 0, out[-800:]
    assert out.count("Joshua Tree on ARM64") == 2, "reboot did not come back to a fresh boot: " + out[-800:]
    assert "power: restart" in out and out.index("power: restart") < out.rindex("Joshua Tree on ARM64"), out[-800:]
    print("PASS: the reboot command comes back to a fresh boot (the banner prints twice)")
    assert "power: saved open files 1" in out, out[-800:]
    assert "power: file survived the reboot" in out and "LOST" not in out, out[-800:]
    print("PASS: a file left unsaved in an open descriptor is flushed and survives the reboot")
    assert "power: shutdown" in out and "power: safe to unplug" in out, out[-800:]
    assert out.index("power: safe to unplug") > out.rindex("Joshua Tree on ARM64"), out[-800:]
    print("PASS: the shutdown command paints the safe-to-unplug screen and QEMU exits 0")

    rc, out = typed("powernoflush")
    assert "power: file LOST in the reboot" in out and "power: file survived" not in out and "saved open files" not in out, out[-800:]
    print("PASS: mutation caught: without the flush the unsaved file is lost across the reboot")
