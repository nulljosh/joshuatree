#!/usr/bin/env python3
"""10.3.0, the battery (arch/arm64/battery.h): a MAX17048 fuel gauge at I2C 0x36 on BSC1, shown in the menu bar.

Part 1, on the host: bat_parse turns the gauge's VCELL and SOC bytes into millivolts and a rounded percent, caps a full
charge at 100 and reads a zero VCELL as no cell. A broken copy (no cap) must fail it.
Part 2, QEMU raspi4b: an at24c EEPROM at 0x36 on i2c-bus.1 stands in for the gauge (the same protocol: write the
register, read on from it), filled with 3980 mV and 87.8 percent. The Pi image reads "battery: 88 percent, 3980 mV"
through its own I2C driver, and the menu bar's right end differs from a boot with no gauge, which says "battery: none".
The real chip on a real board is in docs/TESTING.md.

Skips (exit 0) when clang, ld.lld or qemu-system-aarch64 is missing.
Usage: tools/checks/arm64-battery-check.py   (from the repo root)
"""
import json, shutil, socket, subprocess, sys, time
from pathlib import Path
from scratch import scratch_dir

ROOT = Path(__file__).resolve().parents[2]
ARCH = ROOT / "arch/arm64"
TMP = Path(scratch_dir("arm64-battery"))
if not all(shutil.which(t) for t in ("clang", "ld.lld", "qemu-system-aarch64")):
    print("SKIP: clang, ld.lld or qemu-system-aarch64 not installed"); sys.exit(0)

HOST = r'''
#include <assert.h>
#include "battery.h"
int main(void) {
    int mv = 0;
    unsigned char a[4] = { 0xC7, 0x00, 87, 200 }; assert(bat_parse(a, &mv) == 88 && mv == 3980);
    unsigned char b[4] = { 0xC7, 0x00, 87, 100 }; assert(bat_parse(b, &mv) == 87);
    unsigned char c[4] = { 0xD6, 0x66, 103, 0 };  assert(bat_parse(c, &mv) == 100 && mv == 4287);
    unsigned char d[4] = { 0, 0, 50, 0 };         assert(bat_parse(d, &mv) == -1);
    return 0;
}
'''
c, exe = TMP / "bat.c", TMP / "bat"
c.write_text(HOST)
header = (ARCH / "battery.h").read_text()
def host(text):
    (TMP / "battery.h").write_text(text)
    b = subprocess.run(["clang", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined", str(c), "-o", str(exe)], capture_output=True, text=True)
    if b.returncode: return "build: " + b.stderr[-300:]
    return subprocess.run([str(exe)], capture_output=True, text=True, timeout=10).returncode
assert host(header) == 0, "battery parse failed on the host"
cap = "return p > 100 ? 100 : p;"
assert cap in header and host(header.replace(cap, "return p;")) != 0, "the uncapped copy passed"
print("PASS: host: millivolts, a rounded percent, a full charge capped at 100, no cell read as none (the uncapped copy fails)", flush=True)

subprocess.run(["make", "-s", "-C", str(ARCH), "kernel8.img"], check=True)
gauge = TMP / "gauge.bin"
img = bytearray(512); img[2:6] = bytes([0xC7, 0x00, 87, 200]); gauge.write_bytes(img)   # QEMU wants the backing file a whole sector

def boot(tag, with_gauge):
    log, sock = TMP / ("uart-" + tag), TMP / ("qmp-" + tag)
    extra = ["-drive", "if=none,id=g,format=raw,file=" + str(gauge),
             "-device", "at24c-eeprom,bus=i2c-bus.1,address=0x36,rom-size=512,address-size=1,drive=g"] if with_gauge else []
    q = subprocess.Popen(["qemu-system-aarch64", "-machine", "raspi4b", "-display", "none", "-serial", "null", "-serial", "file:" + str(log),
                          "-qmp", "unix:%s,server,nowait" % sock, *extra, "-kernel", str(ARCH / "kernel8.img")],
                         stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    try:
        t = time.time(); out = ""
        while time.time() - t < 60:
            out = log.read_text(errors="replace") if log.exists() else ""
            if "boot done" in out: break
            assert q.poll() is None, "QEMU exited: " + q.stderr.read().decode()[-300:]
            time.sleep(0.2)
        assert "boot done" in out, "raspi4b did not finish booting: " + out[-400:]
        time.sleep(1.0)
        s = socket.socket(socket.AF_UNIX); s.settimeout(20); s.connect(str(sock)); f = s.makefile("rw"); f.readline()
        def cmd(name, **a):
            f.write(json.dumps({"execute": name, **({"arguments": a} if a else {})}) + "\n"); f.flush()
            while True:
                r = json.loads(f.readline())
                if "error" in r: raise AssertionError(r)
                if "return" in r: return
        cmd("qmp_capabilities"); shot = TMP / (tag + ".ppm"); cmd("screendump", filename=str(shot))
        f.close(); s.close()
        return out, shot.read_bytes()
    finally:
        q.kill(); q.wait(); q.stderr.close()

def bar(ppm):   # the right half of the top 20 rows, as raw RGB rows
    parts = ppm.split(b"\n", 3); w, h = map(int, parts[1].split()); px = parts[3]
    return [px[(y * w + w // 2) * 3:(y * w + w) * 3] for y in range(min(20, h))]

out_g, shot_g = boot("gauge", True)
ok = "battery: 88 percent, 3980 mV" in out_g
print("  %s: the Pi's I2C driver reads the gauge: %s" % ("ok" if ok else "FAIL", [l for l in out_g.splitlines() if l.startswith("battery")])); assert ok
out_n, shot_n = boot("none", False)
ok = "battery: none" in out_n and "battery: 8" not in out_n
print("  %s: with no gauge the boot says none" % ("ok" if ok else "FAIL")); assert ok
ok = bar(shot_g) != bar(shot_n)
print("  %s: the menu bar shows the battery in place of the power symbol" % ("ok" if ok else "FAIL")); assert ok
assert "KERNEL CRASH" not in out_g + out_n
print("PASS: raspi4b reads a gauge at 0x36 on BSC1 (88 percent, 3980 mV), says none without one, and the menu bar shows it", flush=True)
