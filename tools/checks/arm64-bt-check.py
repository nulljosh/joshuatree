#!/usr/bin/env python3
"""ARM64 Bluetooth, slice BT1 (arch/arm64/bt.c): HCI over a UART against a fake controller.

QEMU has no Bluetooth chip. Its virt machine grows a second PL011 at 0x09040000 when it gets a second -serial, and that
port is a Unix socket to the fake controller below, which speaks H4 like the Pi 4's CYW43455 and checks every command.
The kernel is built with BT_HCD= a small made-up patch file, so the upload is checked record by record.
  1. good:   HCI Reset, Read Local Version, Download Minidriver, every Write RAM record in file order, the file's own Launch
             RAM, Reset and Read Local Version again (the patched revision), the vendor baud command (921600), Set Event Mask
             with LE Meta on, then `bt ready`. Typed at the Terminal: `bt` says ready with the version; `bt scan` sends
             active scan parameters, enable, and after 5 seconds disable, and prints each device once, in the order heard:
             the shortened name from a later report, the mouse's complete name from its scan response (two reports in
             one event), the latest RSSI, the address byte-reversed. A stray ACL packet in the middle is skipped cleanly.
  2. order:  the controller expects the records in another order, so it goes quiet at the first one: the driver prints
             "bt: no answer from the radio", never "bt: ready", and the Terminal still answers (`bt`, `help`).
  3. silent: no Command Complete for Read Local Version: the same plain line, and the kernel stays up.
  4. absent: one -serial, so no second UART: the probe's fault is skipped, "bt: no radio found at boot" (serial only,
             so every other virt check's screen is unchanged), `bt` says "bt: no radio", the kernel stays up.
Skips (exit 0) when clang's aarch64 target, ld.lld or qemu-system-aarch64 is missing; skips boots 1 to 3 when this QEMU's
virt machine builds no second PL011 for a second -serial (a dumpdtb probe), and still runs boot 4.
Usage: tools/checks/arm64-bt-check.py   (from the repo root)
"""
import json, os, shutil, socket, subprocess, sys, threading, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from scratch import scratch_dir

root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
arch = os.path.join(root, "arch/arm64")
if not all(shutil.which(t) for t in ("clang", "ld.lld", "qemu-system-aarch64")):
    print("SKIP: clang, ld.lld or qemu-system-aarch64 not installed"); sys.exit(0)
tmp = scratch_dir("bt")
# The radio needs virt's second PL011 (0x09040000), which a QEMU only builds for a second -serial in newer releases.
dtb = os.path.join(tmp, "virt.dtb")
subprocess.run(["qemu-system-aarch64", "-machine", "virt,dumpdtb=" + dtb, "-cpu", "cortex-a72", "-m", "256", "-display", "none",
                "-serial", "null", "-serial", "null"], capture_output=True, timeout=60)
second_uart = os.path.exists(dtb) and b"pl011@9040000" in open(dtb, "rb").read()

# A made-up patch in the .hcd layout: HCI commands back to back (opcode LE, length, parameters). Six Write RAM records of
# different sizes (each an address and data), then Launch RAM, the way BCM4345C0.hcd ends.
writes = []
for i, n in enumerate((70, 204, 4, 204, 108, 33)):
    writes.append(bytes([(0x00 + i * 0x40) & 255, 0x10 + i, 0x21, 0x00]) + bytes((i * 37 + k * 11) & 255 for k in range(n - 4)))
launch = b"\xff\xff\xff\xff"
hcd = b"".join(b"\x4c\xfc" + bytes([len(w)]) + w for w in writes) + b"\x4e\xfc\x04" + launch
hcd_path = os.path.join(tmp, "test.hcd")
open(hcd_path, "wb").write(hcd)

TILE = bytes([0x56, 0x34, 0x12, 0xEF, 0xCD, 0xAB])     # printed ab:cd:ef:12:34:56
MOUSE = bytes([0x21, 0x43, 0x65, 0x8F, 0x8E, 0xC4])    # printed c4:8e:8f:65:43:21
NAME = b"Microsoft Bluetooth Mouse"
def report(evt, addr, data, rssi): return bytes([evt, 1]) + addr + bytes([len(data)]) + data + bytes([rssi & 255])
def le_meta(*reports): body = bytes([0x02, len(reports)]) + b"".join(reports); return b"\x04\x3e" + bytes([len(body)]) + body
ADVERTS = [
    le_meta(report(0x00, TILE, b"\x02\x01\x06", -71)),                                       # heard first, no name yet
    le_meta(report(0x00, MOUSE, b"\x02\x01\x05\x03\x19\xc2\x03", -62)),                      # the mouse, appearance only
    b"\x02\x40\x00\x05\x00hello",                                                            # a stray ACL packet
    le_meta(report(0x04, MOUSE, bytes([len(NAME) + 1, 0x09]) + NAME, -60),                   # its scan response: the name
            report(0x00, TILE, b"\x02\x01\x06\x05\x08Tile", -70)),                           # two reports in one event
]

class Fake(threading.Thread):
    """The radio. Answers each H4 command with a Command Complete and logs (opcode, parameters)."""
    def __init__(self, path, mode):
        super().__init__(daemon=True)
        self.mode, self.log, self.problems, self.quiet = mode, [], [], False
        self.expect = list(writes) if mode != "order" else [writes[1], writes[0]] + writes[2:]
        self.nwrite, self.patched = 0, False
        self.srv = socket.socket(socket.AF_UNIX); self.srv.bind(path); self.srv.listen(1); self.srv.settimeout(30)
    def run(self):
        try: self.c, _ = self.srv.accept()
        except OSError: return
        self.c.settimeout(0.2); buf = b""
        while True:
            try: d = self.c.recv(4096)
            except socket.timeout: continue
            except OSError: return
            if not d: return
            buf += d
            while buf:
                if buf[0] != 1: self.problems.append("not a command packet: %r" % buf[:8]); buf = buf[1:]; continue
                if len(buf) < 4 or len(buf) < 4 + buf[3]: break
                op, p = buf[1] | buf[2] << 8, buf[4:4 + buf[3]]; buf = buf[4 + buf[3]:]
                self.answer(op, p)
    def send(self, b):
        try: self.c.sendall(b)
        except OSError: pass
    def complete(self, op, ret=b"\x00"):
        self.send(b"\x04\x0e" + bytes([3 + len(ret)]) + b"\x01" + bytes([op & 255, op >> 8]) + ret)
    def answer(self, op, p):
        self.log.append((op, p))
        if self.quiet: return
        if op == 0xFC4C:
            want = self.expect[self.nwrite] if self.nwrite < len(self.expect) else None
            self.nwrite += 1
            if p != want: self.problems.append("record %d out of order" % self.nwrite); self.quiet = True; return
        if op == 0xFC4E:
            if self.nwrite != len(writes) or p != launch: self.problems.append("Launch RAM early or wrong"); self.quiet = True; return
            self.patched = True
        if op == 0x1001:
            if self.mode == "silent": self.quiet = True; return
            rev = 0x01A3 if self.patched else 0x0000
            self.complete(op, bytes([0, 9, rev & 255, rev >> 8, 9, 0x0F, 0x00, 0x19, 0x61])); return
        self.complete(op)
        if op == 0x200C and p[:1] == b"\x01":
            for a in ADVERTS: time.sleep(0.1); self.send(a)

QCODE = {" ": ("spc",)}
class Boot:
    def __init__(self, name, mode=None):
        self.name, d = name, os.path.join(tmp, name); os.makedirs(d)
        self.log, qmp = os.path.join(d, "uart"), os.path.join(d, "qmp")
        radio = []
        self.fake = None
        if mode:
            path = os.path.join(d, "bt.sock")
            self.fake = Fake(path, mode); self.fake.start()
            radio = ["-serial", "unix:" + path]
        self.q = subprocess.Popen(["qemu-system-aarch64", "-machine", "virt", "-cpu", "cortex-a72", "-m", "256", "-nic", "none",
                                   "-global", "virtio-mmio.force-legacy=false", "-device", "virtio-keyboard-device",
                                   "-display", "none", "-serial", "file:" + self.log, *radio,
                                   "-qmp", "unix:%s,server,nowait" % qmp, "-kernel", os.path.join(arch, "kernel8.elf")],
                                  stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        if not self.wait_for("M2 input ready", 40): raise SystemExit("FAIL: %s did not boot: %r" % (name, self.uart()[-600:]))
        time.sleep(0.5)
        self.s = socket.socket(socket.AF_UNIX); self.s.settimeout(20); self.s.connect(qmp); self.f = self.s.makefile("rw")
        self.f.readline(); self.cmd("qmp_capabilities")
    def uart(self): return open(self.log, errors="replace").read() if os.path.exists(self.log) else ""
    def wait_for(self, text, secs, count=1):
        end = time.time() + secs
        while time.time() < end:
            if self.uart().count(text) >= count: return True
            time.sleep(0.1)
        return False
    def cmd(self, c, **a):
        self.f.write(json.dumps({"execute": c, "arguments": a}) + "\n"); self.f.flush()
        while True:
            r = json.loads(self.f.readline())
            if "return" in r or "error" in r: return r
    def key(self, *q): self.cmd("send-key", keys=[{"type": "qcode", "data": k} for k in q]); time.sleep(0.1)
    def terminal(self):
        self.key("f2")
        return self.wait_for("terminal open", 10)
    def type(self, text):
        for ch in text: self.key(*QCODE.get(ch, (ch,)))
        self.key("ret")
    def close(self): self.q.kill(); self.q.wait()

fails = []
def check(name, ok, detail=""):
    print(("  ok: " if ok else "  FAIL: ") + name + ("" if ok else "  " + repr(detail)[-700:]))
    if not ok: fails.append(name)
def lines(out): return [l for l in out.splitlines() if l.startswith("bt: ")]

boots = []
try:
    subprocess.run(["make", "-C", arch, "clean"], capture_output=True, timeout=120)
    r = subprocess.run(["make", "-C", arch, "kernel8.elf", "BT_HCD=" + hcd_path], capture_output=True, text=True, timeout=900)
    if r.returncode: print(r.stdout[-2000:], r.stderr[-2000:]); print("FAIL: arch/arm64 does not build"); sys.exit(1)

    if not second_uart:
        print("  SKIP: this QEMU builds no second PL011 on virt for a second -serial (needs a newer QEMU), the radio boots skipped")
    else:
        b = Boot("good", "good"); boots.append(b)
        check("bring-up ends in bt: ready", b.wait_for("bt: ready, type bt scan", 30), b.uart()[-800:])
        ops = [op for op, _ in b.fake.log]
        want = [0x0C03, 0x1001, 0xFC2E] + [0xFC4C] * len(writes) + [0xFC4E, 0x0C03, 0x1001, 0xFC18, 0x0C01]
        check("commands in Linux btbcm's order, the file's records once each, Launch RAM not sent twice", ops == want, [hex(o) for o in ops])
        check("every record arrived whole and in file order", not b.fake.problems and b.fake.nwrite == len(writes), b.fake.problems)
        p = dict((op, prm) for op, prm in b.fake.log)
        check("vendor baud command asks for 921600", p.get(0xFC18) == b"\x00\x00" + (921600).to_bytes(4, "little"), p.get(0xFC18))
        mask = int.from_bytes(p.get(0x0C01, b"\0" * 8), "little")
        check("Set Event Mask turns LE Meta (bit 61) on", mask >> 61 & 1 and mask & 0x1FFFFFFFFFFF == 0x1FFFFFFFFFFF, hex(mask))
        boot_lines = lines(b.uart())
        check("the patch lines and the patched version on the UART, in order",
              boot_lines[:4] == ["bt: loading the radio's patch", "bt: patch loaded, 7 records",
                                 "bt: version hci 9 lmp 9 sub 0x6119 rev 0x1a3", "bt: ready, type bt scan to look for devices"], boot_lines)
        check("the Terminal opens", b.terminal(), b.uart()[-300:])
        b.type("bt")
        check("bt says ready with the version", b.wait_for("bt: ready, version hci 9 lmp 9 sub 0x6119 rev 0x1a3", 10), b.uart()[-400:])
        b.type("bt scan")
        check("bt scan starts at once", b.wait_for("bt: scanning for 5 seconds", 10), b.uart()[-400:])
        t0 = time.time()
        check("bt scan ends", b.wait_for("bt: scan done", 20), b.uart()[-600:])
        took = time.time() - t0
        check("the scan lasts about 5 seconds", 4.0 < took < 12, took)
        out = b.uart(); after = lines(out.split("bt: scanning for 5 seconds", 1)[1])
        check("each device once, in the order heard, names merged, latest RSSI, address byte-reversed",
              after == ["bt: Tile  rssi -70  addr ab:cd:ef:12:34:56",
                        "bt: Microsoft Bluetooth Mouse  rssi -60  addr c4:8e:8f:65:43:21",
                        "bt: scan done, 2 devices"], after)
        sc = [(op, prm) for op, prm in b.fake.log if op in (0x200B, 0x200C)]
        check("active scan parameters, enable, then disable", [op for op, _ in sc] == [0x200B, 0x200C, 0x200C]
              and sc[0][1][:1] == b"\x01" and sc[1][1][:1] == b"\x01" and sc[2][1][:1] == b"\x00", sc)
        b.type("bt")
        check("the stray ACL packet was counted and skipped, nothing lost", b.wait_for("bt: acl in 1, rx errors 0, stray bytes 0", 10), b.uart()[-400:])
        b.close()

        for mode, why in (("order", "a record out of order"), ("silent", "no Command Complete for Read Local Version")):
            b = Boot(mode, mode); boots.append(b)
            check(f"{mode}: {why} ends in one plain line", b.wait_for("bt: no answer from the radio", 20), b.uart()[-600:])
            time.sleep(0.5)
            check(f"{mode}: never ready", "bt: ready" not in b.uart(), lines(b.uart()))
            check(f"{mode}: the command did leave the UART, so no CTS hint", "held CTS" not in b.uart(), lines(b.uart()))
            check(f"{mode}: the controller saw what it should", (mode == "order") == bool(b.fake.problems), b.fake.problems)
            check(f"{mode}: the Terminal still opens", b.terminal(), b.uart()[-300:])
            b.type("bt")
            check(f"{mode}: bt reports the failure", b.wait_for("bt: no answer from the radio", 10, count=2), b.uart()[-300:])
            b.type("help")
            check(f"{mode}: the kernel answers a command after it", b.wait_for("browse URL", 10), b.uart()[-300:])
            check(f"{mode}: no crash", "KERNEL CRASH" not in b.uart(), b.uart()[-300:])
            b.close()

    b = Boot("absent"); boots.append(b)
    check("absent: no second UART says so at boot", b.wait_for("bt: no radio found at boot", 10), b.uart()[-400:])
    check("absent: the probe's fault was skipped, no crash", "KERNEL CRASH" not in b.uart(), b.uart()[-300:])
    check("absent: the Terminal opens", b.terminal(), b.uart()[-300:])
    b.type("bt")
    check("absent: bt says so", b.wait_for("bt: no radio\n", 10), b.uart()[-300:])
    b.close()
finally:
    for x in boots: x.close()
    subprocess.run(["make", "-C", arch, "clean"], capture_output=True, timeout=120)

if fails: print("FAIL: %d of the Bluetooth checks" % len(fails)); sys.exit(1)
print("PASS: HCI over the second UART: patch upload in order, version, baud, event mask, an active LE scan that lists the mouse "
      "by its scan-response name, and a silent or out-of-order radio ends in one plain line with the kernel still up")
