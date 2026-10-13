#!/usr/bin/env python3
"""ARM64 Bluetooth, slices BT1 and BT2 (arch/arm64/bt.c): HCI over a UART against a fake controller and a fake mouse.

QEMU has no Bluetooth chip. Its virt machine grows a second PL011 at 0x09040000 when it gets a second -serial, and that
port is a Unix socket to the fake controller below, which speaks H4 like the Pi 4's CYW43455 and checks every command.
The kernel is built with BT_HCD= a small made-up patch file, so the upload is checked record by record.
  1. good:   HCI Reset, Read Local Version, Download Minidriver, every Write RAM record in file order, the file's own Launch
             RAM, Reset and Read Local Version again (the patched revision), the vendor baud command (921600), Set Event Mask
             with LE Meta on, then `bt ready`. Typed at the Terminal: `bt` says ready with the version; `bt scan` sends
             active scan parameters, enable, and after 5 seconds disable, and prints each device once, in the order heard:
             the shortened name from a later report, the mouse's complete name from its scan response (two reports in
             one event), the latest RSSI, the address byte-reversed. A stray ACL packet in the middle is skipped cleanly.
             BT2, `bt pair`, with ramfb so there is a desktop to click: LE Read Buffer Size says 0, so Read Buffer Size;
             one ACL credit, so every second packet must wait for Number of Completed Packets; Read BD_ADDR; LE Create
             Connection to the mouse (random address) from our public one; then the fake mouse plays SMP legacy Just
             Works. The check computes c1 and s1 itself with AES-128 (pure Python, tested here against FIPS-197 and the
             Core spec's own c1 and s1 samples), so the kernel's Mconfirm must be c1 of its Mrand and the key in LE Start
             Encryption must be s1(0, Srand, Mrand). Then Encryption Change, the mouse's LTK, EDIV and Rand, and a GATT
             database with a battery service before HID, characteristic pages of three, one reply split over two ACL
             fragments, and a user description in front of the boot report's CCCD. The mouse also asks things of its
             own (a parameter update, Exchange MTU, a service search, an indication) and each must be answered. The UART
             says `bt: connected`, `bt: paired`, `bt: mouse ready`; Protocol Mode is written to boot with Write Command
             and the CCCD to 01 00, both after encryption; four boot reports move the pointer onto the Stocks tile
             (`mouse X,Y` at each step, the hover label), press and release: `dock Stocks: not on ARM yet`. Then the mouse
             hangs up and the Terminal still answers.
  2. order:  the controller expects the records in another order, so it goes quiet at the first one: the driver prints
             "bt: no answer from the radio", never "bt: ready", and the Terminal still answers (`bt`, `help`).
  3. silent: no Command Complete for Read Local Version: the same plain line, and the kernel stays up.
  4. badconfirm: the mouse's confirm value does not match its random: `bt: pairing failed`, the kernel tells the mouse
             (Pairing Failed, Confirm Value Failed) and hangs up, never sends LE Start Encryption, and answers `bt`, `help`.
  5. absent: one -serial, so no second UART: the probe's fault is skipped, "bt: no radio found at boot" (serial only,
             so every other virt check's screen is unchanged), `bt` says "bt: no radio", the kernel stays up.
  6. remember (BT3): the same fake mouse and a FAT32 card as a virtio disk (tools/checks/sdcard.py). `bt pair` prints `bt: pairing
             saved` once the mouse's LTK, EDIV and Rand are in, and `bt` says `saved pairing: yes`. Then, each on its own copy of
             that card, as after a reboot: reconnect (no scan, no SMP, LE Create Connection to the saved address, LE Start
             Encryption with the saved LTK, Rand and EDIV, `bt: reconnected`, `bt: mouse ready`, reports move the pointer, no
             second save); a mouse that never answers (a try of about 12 seconds, the record kept, `bt status` says searching with
             the address masked, then `bt forget` erases it); a mouse that rejects the key (Encryption Change 0x06: the record is erased, `saved pairing: no`);
             and a card with no record (the plain ready line, `bt forget` says there was none).
  7. autorc (BT4): a mouse asleep at boot (the first LE Create Connection gets no answer, the kernel cancels it after about 12
             seconds and tries again after a 2 second pause), then connected: no typing, `bt: reconnected`, notifications
             written again, two reports move the pointer by 7,0 and 0,9. The fake then drops the link (reason 0x08, a sleep) and
             the kernel reconnects on its own with the saved key, writes the notifications again and the pointer moves again;
             `bt status` says connected, the last byte of the address and the attempts. Never the full address or a key.
In every boot no random value, confirm value or key (STK, LTK, Rand) shows on the UART, in either byte order.
Skips (exit 0) when clang's aarch64 target, ld.lld or qemu-system-aarch64 is missing; skips boots 1 to 4 when this QEMU's
virt machine builds no second PL011 for a second -serial (a dumpdtb probe), and still runs boot 5.
Usage: tools/checks/arm64-bt-check.py   (from the repo root)
"""
import json, os, re, shutil, socket, struct, subprocess, sys, threading, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from scratch import scratch_dir
from sdcard import build_card

# ---- AES-128 (FIPS-197), encrypt only, and the SMP functions on little-endian values (Core spec Vol 3 Part H 2.2) ----
def _xt(a): return ((a << 1) ^ 0x1B) & 0xFF if a & 0x80 else a << 1
_exp, _log = [0] * 255, [0] * 256
_x = 1
for _i in range(255): _exp[_i] = _x; _log[_x] = _i; _x ^= _xt(_x)
def _sb(a):
    b = _exp[(255 - _log[a]) % 255] if a else 0
    s = b
    for _ in range(4): b = ((b << 1) | (b >> 7)) & 0xFF; s ^= b
    return s ^ 0x63
SBOX = [_sb(a) for a in range(256)]
def aes128(key, block):
    w, rc = list(key), 1
    for i in range(4, 44):
        t = w[4 * i - 4:4 * i]
        if i % 4 == 0: t = [SBOX[t[1]] ^ rc, SBOX[t[2]], SBOX[t[3]], SBOX[t[0]]]; rc = _xt(rc)
        w += [w[4 * i - 16 + j] ^ t[j] for j in range(4)]
    s = [b ^ k for b, k in zip(block, w[:16])]
    for r in range(1, 11):
        s = [SBOX[b] for b in s]
        s = [s[((i // 4 + i % 4) % 4) * 4 + i % 4] for i in range(16)]   # ShiftRows, the state column by column
        if r < 10:
            m = []
            for c in range(4):
                a = s[4 * c:4 * c + 4]; t = a[0] ^ a[1] ^ a[2] ^ a[3]
                m += [a[j] ^ t ^ _xt(a[j] ^ a[(j + 1) % 4]) for j in range(4)]
            s = m
        s = [b ^ k for b, k in zip(s, w[16 * r:16 * r + 16])]
    return bytes(s)
def xor(a, b): return bytes(x ^ y for x, y in zip(a, b))
def e(k, p): return aes128(k[::-1], p[::-1])[::-1]   # the spec's e() is most significant octet first; SMP travels LSB first
def c1(k, r, preq, pres, iat, ia, rat, ra): return e(k, xor(e(k, xor(r, bytes([iat, rat]) + preq + pres)), ra + ia + bytes(4)))
def s1(k, r1, r2): return e(k, r2[:8] + r1[:8])
H = bytes.fromhex
crypto_ok = (aes128(bytes(range(16)), H("00112233445566778899aabbccddeeff")) == H("69c4e0d86a7b0430d8cdb78070b4c55a")
             and c1(bytes(16), H("e02e70c64e2788630e6fad5621d58357"), H("01010000100707"), H("02030000080005"), 1, H("a6a5a4a3a2a1"),
                    0, H("b6b5b4b3b2b1")) == H("863bf1bec54da7d2ea888987ef3f1e1e")
             and s1(bytes(16), H("8877665544332211") + bytes(8), H("00ffeeddccbbaa99") + bytes(8)) == H("62a06d79ae16425b9bf4b0e8f0e11f9a"))
if not crypto_ok: print("FAIL: the check's own AES or SMP c1/s1 does not match FIPS-197 and the Core spec samples"); sys.exit(1)

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
MOUSE = bytes([0x21, 0x43, 0x65, 0x8F, 0x8E, 0xC4])    # printed c4:8e:8f:65:43:21, a random static address
NAME = b"Microsoft Bluetooth Mouse"
BDADDR = bytes([0x11, 0x22, 0x33, 0xEB, 0x27, 0xB8])   # the Pi's own public address
HANDLE = 0x0040
def report(evt, addr, data, rssi): return bytes([evt, 1]) + addr + bytes([len(data)]) + data + bytes([rssi & 255])
def le_meta(*reports): body = bytes([0x02, len(reports)]) + b"".join(reports); return b"\x04\x3e" + bytes([len(body)]) + body
ADVERTS = [
    le_meta(report(0x00, TILE, b"\x02\x01\x06", -71)),                                       # heard first, no name yet
    le_meta(report(0x00, MOUSE, b"\x02\x01\x05\x03\x19\xc2\x03", -62)),                      # the mouse, appearance only
    b"\x02\x40\x00\x05\x00hello",                                                            # a stray ACL packet
    le_meta(report(0x04, MOUSE, bytes([len(NAME) + 1, 0x09]) + NAME, -60),                   # its scan response: the name
            report(0x00, TILE, b"\x02\x01\x06\x05\x08Tile", -70)),                           # two reports in one event
]

# The mouse's GATT database: GAP, a battery service before HID, HID with its characteristics, Device Information.
def u16(v): return struct.pack("<H", v)
def decl(props, vh, uuid): return bytes([props]) + u16(vh) + u16(uuid)
DB = [(0x01, 0x2800, u16(0x1800)), (0x02, 0x2803, decl(0x02, 0x03, 0x2A00)), (0x03, 0x2A00, NAME[:20]),
      (0x10, 0x2800, u16(0x180F)), (0x11, 0x2803, decl(0x12, 0x12, 0x2A19)), (0x12, 0x2A19, b"\x5a"), (0x13, 0x2902, b"\0\0"),
      (0x20, 0x2800, u16(0x1812)),
      (0x21, 0x2803, decl(0x02, 0x22, 0x2A4A)), (0x22, 0x2A4A, b"\x11\x01\x00\x02"),
      (0x23, 0x2803, decl(0x06, 0x24, 0x2A4E)), (0x24, 0x2A4E, b"\x01"),                           # Protocol Mode
      (0x25, 0x2803, decl(0x12, 0x26, 0x2A4D)), (0x26, 0x2A4D, b"\0\0\0\0"), (0x27, 0x2902, b"\0\0"), (0x28, 0x2908, b"\x01\x01"),
      (0x29, 0x2803, decl(0x12, 0x2A, 0x2A33)), (0x2A, 0x2A33, b"\0\0\0"),                         # Boot Mouse Input
      (0x2B, 0x2901, b"boot"), (0x2C, 0x2902, b"\0\0"),                                             # its CCCD is not value + 1
      (0x2D, 0x2803, decl(0x04, 0x2E, 0x2A4C)), (0x2E, 0x2A4C, b"\0"),
      (0x2F, 0x2803, decl(0x02, 0x30, 0x2A4B)), (0x30, 0x2A4B, b"\x05\x01"),
      (0x40, 0x2800, u16(0x180A)), (0x41, 0x2803, decl(0x02, 0x42, 0x2A29)), (0x42, 0x2A29, b"Incase")]
HID_START, HID_END, PM_VH, BOOT_VH, BOOT_CCCD = 0x20, 0x3F, 0x24, 0x2A, 0x2C
def group_end(h):
    later = [x for x, t, _ in DB if t == 0x2800 and x > h]
    return later[0] - 1 if later else 0xFFFF
DB_MAP = b"\x05\x01"
# A mouse that keeps Report protocol even when told boot: buttons 3 bits + 5 pad, X and Y 12 bits each, packed, then an 8 bit wheel.
PACKED_MAP = bytes.fromhex("05010902a1010901a100" "05091901290315002501750195038102" "7505950181" "03"
                           "05010930093116 01f826ff07750c95028106" "093815812 57f750895018106".replace(" ", "") + "c0c0")
def packed(btn, x, y, w):
    x &= 0xFFF; y &= 0xFFF
    return bytes([btn, x & 255, (x >> 8) | (y & 15) << 4, y >> 4 & 255, w & 255])
CONN_UPDATE = struct.pack("<HHHH", 6, 9, 44, 500)   # what the mouse asks for: 7.5 to 11.25 ms, latency 44, timeout 5 s

class Fake(threading.Thread):
    """The radio, and the mouse at the other end of the air. Answers each H4 command, logs (opcode, parameters), and
    checks every ACL packet the kernel sends: flow control, SMP, ATT. Anything wrong goes in self.problems."""
    def __init__(self, path, mode, saved=None):
        super().__init__(daemon=True)
        self.saved = saved   # (ltk, rand, ediv) the mouse gave in an earlier boot: a reconnect must encrypt with these
        self.mode, self.log, self.problems, self.quiet = mode, [], [], False
        self.expect = list(writes) if mode != "order" else [writes[1], writes[0]] + writes[2:]
        self.nwrite, self.patched = 0, False
        self.credits = 1 if mode == "good" else 2
        self.out = self.max_out = 0
        self.later, self.inject = [], []
        self.acl, self.secrets, self.writes, self.answers = [], [], [], []
        self.preq = self.pairing_failed = self.mconf = self.srand = self.stk = None
        self.connected = self.encrypted = False
        self.reports = []
        self.creates = 0
        self.pm, self.ignore_pm, self.rmap = b"\x01", False, DB_MAP   # Protocol Mode as the mouse holds it; a mouse that ignores the write; its report map
        if mode == "packed":
            self.ignore_pm, self.rmap = True, PACKED_MAP
            self.reports = [packed(0, 5, -3, 0), packed(0, 7, 11, 0), packed(0, -5, 3, 0), packed(1, 0, 0, 0)]
        self.srv = socket.socket(socket.AF_UNIX); self.srv.bind(path); self.srv.listen(1); self.srv.settimeout(30)
    def run(self):
        try: self.c, _ = self.srv.accept()
        except OSError: return
        self.c.settimeout(0.01); buf = b""
        while True:
            t = time.time()
            for item in [x for x in self.later if x[0] <= t]:
                self.later.remove(item); self.send(item[1])
                if item[2]: item[2]()
            while self.inject: self.send(self.inject.pop(0))
            try: d = self.c.recv(4096)
            except socket.timeout: continue
            except OSError: return
            if not d: return
            buf += d
            while buf:
                if buf[0] == 1:
                    if len(buf) < 4 or len(buf) < 4 + buf[3]: break
                    op, p = buf[1] | buf[2] << 8, buf[4:4 + buf[3]]; buf = buf[4 + buf[3]:]
                    self.answer(op, p)
                elif buf[0] == 2:
                    if len(buf) < 5: break
                    hdr, n = struct.unpack("<HH", buf[1:5])
                    if len(buf) < 5 + n: break
                    data = buf[5:5 + n]; buf = buf[5 + n:]
                    self.on_acl(hdr, data)
                else: self.problems.append("not a command or ACL packet: %r" % buf[:8]); buf = buf[1:]
    def send(self, b):
        try: self.c.sendall(b)
        except OSError: pass
    def at(self, delay, b, then=None): self.later.append((time.time() + delay, b, then))
    def event(self, code, body): return bytes([4, code, len(body)]) + body
    def complete(self, op, ret=b"\x00"):
        self.send(b"\x04\x0e" + bytes([3 + len(ret)]) + b"\x01" + bytes([op & 255, op >> 8]) + ret)
    def status(self, op, st=0): self.send(self.event(0x0F, bytes([st, 1, op & 255, op >> 8])))
    def l2(self, cid, payload, split=0, delay=0.0):   # an L2CAP frame to the kernel, in one ACL packet or two
        f = struct.pack("<HH", len(payload), cid) + payload
        parts = [f[:split], f[split:]] if split else [f]
        for i, part in enumerate(parts):
            self.at(delay, b"\x02" + struct.pack("<HH", HANDLE | (0x1000 if i else 0x2000), len(part)) + part)
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
        if op == 0x2002:   # LE Read Buffer Size: the good radio shares its classic buffers (0), the other has its own
            self.complete(op, b"\x00" + u16(27) + bytes([0 if self.mode == "good" else self.credits])); return
        if op == 0x1005: self.complete(op, b"\x00" + u16(1021) + b"\x40" + u16(self.credits) + u16(8)); return
        if op == 0x1009: self.complete(op, b"\x00" + BDADDR); return
        if op == 0x200D:
            self.status(op)
            if p[5] != 1 or p[6:12] != MOUSE or p[12] != 0: self.problems.append("LE Create Connection to the wrong peer: %s" % p.hex())
            self.connected = True; self.encrypted = False; self.creates += 1
            if self.mode == "nomouse" or (self.mode == "flaky" and self.creates == 1): return    # the saved mouse is asleep: Create Connection stays pending
            self.at(0.05, b"\x04\x3e\x13\x01\x00" + u16(HANDLE) + b"\x00\x01" + MOUSE + u16(6) + u16(0) + u16(200) + b"\x00")
            self.l2(5, b"\x12\x01\x08\x00" + CONN_UPDATE, delay=0.15)        # the mouse's own asks, right after it connects
            self.l2(4, b"\x02\x40\x00", delay=0.15)                           # Exchange MTU, 64
            self.l2(4, b"\x10\x01\x00\xff\xff\x00\x28", delay=0.15)           # Read By Group Type: our services
            self.l2(4, b"\x1d\x05\x00\x01\x00\xff\xff", delay=0.15)           # an indication (Service Changed)
            return
        if op == 0x2013:
            self.status(op)
            self.at(0.02, b"\x04\x3e\x0a\x03\x00" + u16(HANDLE) + u16(9) + u16(44) + u16(500)); return
        if op == 0x2019 and self.saved:   # a reconnect: the saved key, with no SMP in front of it
            self.status(op)
            ltk, rand, ediv = self.saved
            if p[:2] != u16(HANDLE) or p[2:10] != rand or p[10:12] != ediv or p[12:28] != ltk:
                self.problems.append("LE Start Encryption is not the saved LTK with its Rand and EDIV")
            if self.mode == "rejected": self.at(0.05, self.event(0x08, b"\x06" + u16(HANDLE) + b"\x00")); return   # PIN or key missing
            self.encrypted = True
            self.at(0.05, self.event(0x08, b"\x00" + u16(HANDLE) + b"\x01"))
            return
        if op == 0x2019:
            self.status(op)
            if p[:2] != u16(HANDLE) or p[2:12] != bytes(10) or p[12:28] != self.stk:
                self.problems.append("LE Start Encryption is not the STK s1(0, Srand, Mrand) with Rand and EDIV zero"); return
            self.encrypted = True
            self.ltk, self.rand, self.ediv = os.urandom(16), os.urandom(8), os.urandom(2)
            self.secrets += [self.ltk, self.rand]
            self.at(0.05, self.event(0x08, b"\x00" + u16(HANDLE) + b"\x01"))
            self.l2(6, b"\x06" + self.ltk, delay=0.08); self.l2(6, b"\x07" + self.ediv + self.rand, delay=0.08)
            return
        if op == 0x0406:
            self.status(op)
            self.connected = False
            self.at(0.02, self.event(0x05, b"\x00" + u16(HANDLE) + b"\x16")); return
        if op == 0x200E:
            self.complete(op); self.at(0.02, b"\x04\x3e\x13\x01\x02" + bytes(17)); return
        self.complete(op)
        if op == 0x200C and p[:1] == b"\x01":
            for a in ADVERTS: time.sleep(0.1); self.send(a)
    def on_acl(self, hdr, data):
        self.out += 1; self.max_out = max(self.max_out, self.out)
        if self.out > self.credits: self.problems.append("an ACL packet with no credit left (%d in flight, %d credits)" % (self.out, self.credits))
        def done(): self.out -= 1
        self.at(0.03, self.event(0x13, b"\x01" + u16(HANDLE) + u16(1)), done)   # Number of Completed Packets, a little later
        if hdr != HANDLE: self.problems.append("ACL handle and flags 0x%04x, want 0x%04x (PB 00)" % (hdr, HANDLE))
        if len(data) < 4: self.problems.append("ACL packet too short"); return
        n, cid = struct.unpack("<HH", data[:4]); payload = data[4:]
        if n != len(payload): self.problems.append("L2CAP length %d for %d bytes" % (n, len(payload)))
        self.acl.append((cid, payload))
        if cid == 6: self.on_smp(payload)
        elif cid == 4: self.on_att(payload)
        elif cid == 5: self.answers.append(("sig", payload))
        else: self.problems.append("L2CAP channel %d" % cid)
    def on_smp(self, p):
        if self.saved: self.problems.append("SMP from the kernel in a reconnect (code 0x%02x)" % p[0]); return
        if p[0] == 0x01:
            self.preq = p
            if len(p) != 7 or p[1] != 0x03 or p[3] & 0x09 != 0x01 or p[4] != 16 or not p[6] & 1:
                self.problems.append("Pairing Request is not Just Works, bonding, no SC, a 16 byte key, LTK asked: %s" % p.hex())
            self.pres = bytes([0x02, 0x03, 0x00, 0x01, 0x10, 0x00, 0x01])
            self.l2(6, self.pres)
        elif p[0] == 0x03:
            self.mconf, self.srand = p[1:], os.urandom(16)
            sconf = c1(bytes(16), self.srand, self.preq, self.pres, 0, BDADDR, 1, MOUSE)
            if self.mode == "badconfirm": sconf = bytes([sconf[0] ^ 0x5A]) + sconf[1:]
            self.secrets += [self.mconf, self.srand, sconf]
            self.l2(6, b"\x03" + sconf)
        elif p[0] == 0x04:
            mrand = p[1:]; self.secrets.append(mrand)
            if c1(bytes(16), mrand, self.preq, self.pres, 0, BDADDR, 1, MOUSE) != self.mconf:
                self.problems.append("the kernel's Mconfirm is not c1 of its Mrand"); self.l2(6, b"\x05\x04"); return
            self.stk = s1(bytes(16), self.srand, mrand); self.secrets.append(self.stk)
            self.l2(6, b"\x04" + self.srand)
        elif p[0] == 0x05: self.pairing_failed = p[1] if len(p) > 1 else -1
        else: self.problems.append("SMP code 0x%02x from the kernel" % p[0])
    def att_err(self, op, h, code): self.l2(4, bytes([0x01, op]) + u16(h) + bytes([code]))
    def on_att(self, p):
        op = p[0]
        if op in (0x03, 0x01, 0x1E): self.answers.append(("att", p)); return   # answers to the mouse's own requests
        if op == 0x06:   # Find By Type Value
            s, e_, t = struct.unpack("<HHH", p[1:7]); v = p[7:]
            found = [(h, group_end(h)) for h, ty, val in DB if s <= h <= e_ and ty == t and val == v][:5]
            if found: self.l2(4, b"\x07" + b"".join(u16(a) + u16(b) for a, b in found))
            else: self.att_err(op, s, 0x0A)
        elif op == 0x08:   # Read By Type, three 7-byte entries a page; the first page split over two ACL fragments
            s, e_, t = struct.unpack("<HHH", p[1:7])
            found = [(h, val) for h, ty, val in DB if s <= h <= e_ and ty == t]
            if not found: self.att_err(op, s, 0x0A); return
            ln = len(found[0][1]) + 2; page = [x for x in found if len(x[1]) + 2 == ln][:21 // ln]
            self.l2(4, bytes([0x09, ln]) + b"".join(u16(h) + v for h, v in page), split=10 if s == HID_START else 0)
        elif op == 0x04:   # Find Information, format 1
            s, e_ = struct.unpack("<HH", p[1:5])
            found = [(h, ty) for h, ty, _ in DB if s <= h <= e_][:5]
            if found: self.l2(4, b"\x05\x01" + b"".join(u16(h) + u16(t) for h, t in found))
            else: self.att_err(op, s, 0x0A)
        elif op in (0x0A, 0x0C):   # Read Request, Read Blob: Protocol Mode and the report map
            h = struct.unpack("<H", p[1:3])[0]; off = struct.unpack("<H", p[3:5])[0] if op == 0x0C else 0
            if h == PM_VH: self.l2(4, bytes([op + 1]) + self.pm[off:off + 22])
            elif h == 0x30: self.l2(4, bytes([op + 1]) + self.rmap[off:off + 22])
            else: self.att_err(op, h, 0x02)
        elif op in (0x12, 0x52):   # Write Request, Write Command
            h = struct.unpack("<H", p[1:3])[0]; v = p[3:]
            self.writes.append((op, h, v))
            if h == PM_VH and not self.ignore_pm: self.pm = v
            if HID_START <= h <= HID_END and not self.encrypted:
                self.problems.append("a HID write before encryption")
                if op == 0x12: self.att_err(op, h, 0x0F)
                return
            if op == 0x12:
                self.l2(4, b"\x13")
                if h == BOOT_CCCD and v == b"\x01\x00":
                    for i, r in enumerate(self.reports): self.l2(4, b"\x1b" + u16(BOOT_VH) + r, delay=0.3 + 0.25 * i)
        else: self.problems.append("ATT opcode 0x%02x from the kernel" % op)

QCODE = {" ": ("spc",)}
class Boot:
    def __init__(self, name, mode=None, screen=False, disk=None, saved=None):
        self.name, d = name, os.path.join(tmp, name); os.makedirs(d)
        self.dir, self.log, qmp = d, os.path.join(d, "uart"), os.path.join(d, "qmp")
        radio = []
        self.fake = None
        if mode:
            path = os.path.join(d, "bt.sock")
            self.fake = Fake(path, mode, saved); self.fake.start()
            radio = ["-serial", "unix:" + path]
        self.q = subprocess.Popen(["qemu-system-aarch64", "-machine", "virt", "-cpu", "cortex-a72", "-m", "256", "-nic", "none",
                                   "-global", "virtio-mmio.force-legacy=false", "-device", "virtio-keyboard-device",
                                   *(["-device", "ramfb"] if screen else []),
                                   *(["-drive", "if=none,id=card,file=%s,format=raw" % disk, "-device", "virtio-blk-device,drive=card"] if disk else []),
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
    def screen_size(self):
        shot = os.path.join(self.dir, "shot.ppm")
        if "error" in self.cmd("screendump", filename=shot): return None
        time.sleep(0.3)
        return tuple(map(int, open(shot, "rb").read().split(b"\n", 3)[1].split()))
    def key(self, *q): self.cmd("send-key", keys=[{"type": "qcode", "data": k} for k in q]); time.sleep(0.1)
    def terminal(self, count=1):
        self.key("f2")
        return self.wait_for("terminal open", 10, count)
    def type(self, text):
        for ch in text: self.key(*QCODE.get(ch, (ch,)))
        self.key("ret")
    def close(self): self.q.kill(); self.q.wait()

# The dock's geometry, as in arm64-mouse-check.py (kernel/dock_geom.c and fb_init): Stocks (not ported yet, so its click still opens the Console) is slot 9.
def slot_centre(w, h, slot):
    s = 2 if h >= 1080 else 1; lw, lh = w // s, h // s
    icon = max(16, min(lh * 7 // 100, (min(740, lw - 40) - 2 * 10 - 10 * 6) // 11))
    dock_w = 11 * icon + 10 * 6 + 2 * 10; x0 = (lw - dock_w) // 2; y0 = lh - icon - 2 * 10 - 24
    return (x0 + 10 + slot * (icon + 6) + icon // 2) * s, (y0 + 10 + icon // 2) * s

fails = []
def check(name, ok, detail=""):
    print(("  ok: " if ok else "  FAIL: ") + name + ("" if ok else "  " + repr(detail)[-700:]))
    if not ok: fails.append(name)
def lines(out): return [l for l in out.splitlines() if l.startswith("bt: ")]
def leaked(out, secrets):   # any 8 bytes of a secret, either byte order, as hex: run together, split, 0x per byte, unpadded
    toks = re.findall(r"[0-9a-f]+", re.sub(r"0x", " ", out.lower()))
    flats = ("".join(toks), "".join(t.zfill(2) for t in toks))
    for s in secrets:
        for v in (s, s[::-1]):
            for i in range(len(v) - 7):
                if any(v[i:i + 8].hex() in f for f in flats): return v.hex()
    return None

boots = []
try:
    subprocess.run(["make", "-C", arch, "clean"], capture_output=True, timeout=120)
    r = subprocess.run(["make", "-C", arch, "kernel8.elf", "BT_HCD=" + hcd_path], capture_output=True, text=True, timeout=900)
    if r.returncode: print(r.stdout[-2000:], r.stderr[-2000:]); print("FAIL: arch/arm64 does not build"); sys.exit(1)
    print("  ok: the check's AES-128 matches FIPS-197, and its c1 and s1 the Core spec's samples")

    if not second_uart:
        print("  SKIP: this QEMU builds no second PL011 on virt for a second -serial (needs a newer QEMU), the radio boots skipped")
    else:
        b = Boot("good", "good", screen=True); boots.append(b)
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
        check("bt scan ends", b.wait_for("bt: bt pair connects to number 1", 20), b.uart()[-600:])
        took = time.time() - t0
        check("the scan lasts about 5 seconds", 4.0 < took < 12, took)
        out = b.uart(); after = lines(out.split("bt: scanning for 5 seconds", 1)[1])
        check("each device once, in the order heard, names merged, latest RSSI, address byte-reversed",
              after == ["bt: 1. Microsoft Bluetooth Mouse, near [connectable, discoverable]   <- your mouse? bt pair",   # it advertises the HID service and a mouse Appearance: a mouse is listed first, with its distance in words
                        "bt: 2. Tile, in the room",
                        "bt: 2 devices heard",
                        "bt: bt pair connects to number 1, or bt pair 2 for another"], after)
        sc = [(op, prm) for op, prm in b.fake.log if op in (0x200B, 0x200C)]
        check("active scan parameters, enable, then disable", [op for op, _ in sc] == [0x200B, 0x200C, 0x200C]
              and sc[0][1][:1] == b"\x01" and sc[1][1][:1] == b"\x01" and sc[2][1][:1] == b"\x00", sc)
        b.type("bt")
        check("the stray ACL packet was counted and skipped, nothing lost", b.wait_for("bt: acl in 1, rx errors 0, stray bytes 0", 10), b.uart()[-400:])

        # BT2: pair, encrypt, find the boot mouse report, and four reports onto the Stocks tile and a click there.
        size = b.screen_size()
        check("ramfb gives the desktop a screen", bool(size), size)
        W, Hh = size or (800, 600)
        x0, y0 = W // 2, Hh // 2
        tx, ty = slot_centre(W, Hh, 9)
        dx, dy = tx - x0, ty - y0
        hx, hy = dx // 2, dy // 2
        mid = (x0 + hx, y0 + hy)
        b.fake.reports = [bytes([0, hx & 255, hy & 255]), bytes([0, (dx - hx) & 255, (dy - hy) & 255, 1]),   # the second with a wheel byte
                          b"\x01\x00\x00", b"\x00\x00\x00"]
        b.type("bt pair")
        check("bt pair picks the mouse by name", b.wait_for("bt: connecting to Microsoft Bluetooth Mouse", 10), b.uart()[-400:])
        check("bt: connected", b.wait_for("bt: connected", 15), b.uart()[-400:])
        check("bt: paired", b.wait_for("bt: paired", 20), b.uart()[-400:])
        check("bt: mouse ready", b.wait_for("bt: mouse ready", 20), b.uart()[-600:])
        stage = lines(b.uart().split("bt: connecting to", 1)[-1])
        # no card in this boot: the keys arrive just after "paired" and the line says the pairing will not survive a reboot
        check("one line per stage, in order", stage[:2] == ["bt: connected, link interval 7.5 ms", "bt: paired"]
              and sorted(stage[2:4]) == sorted(["bt: no SD card, this pairing will not survive a reboot", "bt: mouse ready"]), stage)
        pair_ops = [op for op, _ in b.fake.log[len(ops):] if op not in (0x200B, 0x200C)]
        check("buffers (LE says 0, so the shared ones), our address, then LE Create Connection",
              pair_ops[:4] == [0x2002, 0x1005, 0x1009, 0x200D], [hex(o) for o in pair_ops])
        check("LE Start Encryption sent once, with the STK", pair_ops.count(0x2019) == 1, [hex(o) for o in pair_ops])
        upd = [prm for op, prm in b.fake.log if op == 0x2013]
        check("the mouse's parameter update is accepted and asked of the radio as it asked",
              ("sig", b"\x13\x01\x02\x00\x00\x00") in b.fake.answers and upd and upd[0] == u16(HANDLE) + CONN_UPDATE + bytes(4), (b.fake.answers, upd))
        check("the mouse's Exchange MTU, service search and indication are answered (MTU 23, Attribute Not Found, a confirm)",
              all(a in b.fake.answers for a in (("att", b"\x03\x17\x00"), ("att", b"\x01\x10\x01\x00\x0a"), ("att", b"\x1e"))), b.fake.answers)
        check("Protocol Mode written to boot with Write Command, then notifications on with a Write Request, after encryption",
              b.fake.writes == [(0x52, PM_VH, b"\x00"), (0x12, BOOT_CCCD, b"\x01\x00")], b.fake.writes)
        check("ACL flow control: never more packets in flight than the radio's one credit", b.fake.max_out == 1, b.fake.max_out)
        check("the fake mouse found nothing wrong (c1 of the kernel's Mrand, s1 in Start Encryption, every packet)",
              not b.fake.problems, b.fake.problems)
        check("first report: the pointer moves by its deltas", b.wait_for("mouse %d,%d\n" % mid, 15), b.uart()[-400:])
        check("second report (with a wheel byte): onto the Stocks tile", b.wait_for("mouse %d,%d\n" % (tx, ty), 10), b.uart()[-400:])
        check("the hover label follows it", b.wait_for("M1d hover 9 Stocks", 10), b.uart()[-400:])
        check("third report: the left button goes down", b.wait_for("key 272 down", 10), b.uart()[-400:])
        check("the click lands on the dock: Stocks opens the Console", b.wait_for("dock Stocks: not on ARM yet", 10), b.uart()[-400:])
        check("fourth report: the button comes up", b.wait_for("key 272 up", 10), b.uart()[-400:])
        check("F2 brings the Terminal back in front of the Console", b.terminal(2), b.uart()[-300:])
        b.type("bt")
        check("bt says the mouse is ready, by name", b.wait_for("bt: mouse ready, Microsoft Bluetooth Mouse", 10), b.uart()[-400:])
        b.fake.inject.append(b"\x04\x05\x04\x00" + u16(HANDLE) + b"\x13")   # the mouse switches off
        check("the mouse hanging up is one plain line", b.wait_for("bt: the mouse disconnected, reason 0x13", 10), b.uart()[-400:])
        time.sleep(2.5)   # no card, so no saved pairing: the reconnect run ends at once and silently
        b.type("bt")
        check("after it, bt is back to ready", b.wait_for("bt: ready, version", 10, count=3), b.uart()[-400:])
        check("no random, confirm value or key on the UART", not leaked(b.uart(), b.fake.secrets) and len(b.fake.secrets) >= 7,
              (leaked(b.uart(), b.fake.secrets), len(b.fake.secrets)))
        check("no crash", "KERNEL CRASH" not in b.uart(), b.uart()[-300:])
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

        b = Boot("badconfirm", "badconfirm"); boots.append(b)
        check("badconfirm: bring-up ends in bt: ready", b.wait_for("bt: ready, type bt scan", 30), b.uart()[-800:])
        check("badconfirm: the Terminal opens", b.terminal(), b.uart()[-300:])
        b.type("bt scan")
        check("badconfirm: the scan ends", b.wait_for("bt: bt pair connects to number 1", 20), b.uart()[-600:])
        b.type("bt pair")
        check("badconfirm: a wrong confirm value ends in bt: pairing failed", b.wait_for("bt: pairing failed", 30), b.uart()[-600:])
        time.sleep(1.0)
        stage = lines(b.uart().split("bt: connecting to", 1)[-1])
        check("badconfirm: connected, then that one line, nothing after it", len(stage) == 2 and stage[0] == "bt: connected, link interval 7.5 ms"
              and stage[1].startswith("bt: pairing failed"), stage)
        bops = [op for op, _ in b.fake.log]
        check("badconfirm: the radio's own LE buffers are used (no Read Buffer Size)", 0x2002 in bops and 0x1005 not in bops, [hex(o) for o in bops[-12:]])
        check("badconfirm: the kernel tells the mouse (Pairing Failed, Confirm Value Failed)", b.fake.pairing_failed == 0x04, b.fake.pairing_failed)
        check("badconfirm: it hangs up and never starts encryption", 0x0406 in bops and 0x2019 not in bops, [hex(o) for o in bops[-8:]])
        check("badconfirm: the kernel's own confirm was still right", not b.fake.problems, b.fake.problems)
        check("badconfirm: no random, confirm value or key on the UART", not leaked(b.uart(), b.fake.secrets) and len(b.fake.secrets) >= 4,
              leaked(b.uart(), b.fake.secrets))
        b.type("bt")
        check("badconfirm: bt is back to ready", b.wait_for("bt: ready, version", 10), b.uart()[-300:])
        b.type("help")
        check("badconfirm: the kernel answers a command after it", b.wait_for("browse URL", 10), b.uart()[-300:])
        check("badconfirm: no crash", "KERNEL CRASH" not in b.uart(), b.uart()[-300:])
        b.close()

    if second_uart:
        # BT3: save the pairing to a card, then boot again on copies of that card, as after a reboot.
        card = os.path.join(tmp, "card.img"); build_card(card, False)
        def clone(n):   # a private copy of the saved card; the image is sparse, so clone it rather than read 4 GiB
            dst = os.path.join(tmp, "card-%s.img" % n)
            for cp in (["cp", "-c"], ["cp", "--reflink=auto", "--sparse=always"], ["cp", "--sparse=always"]):
                if subprocess.run(cp + [card, dst], capture_output=True).returncode == 0: return dst
            raise SystemExit("FAIL: cannot copy the card image")
        b = Boot("remember", "good", screen=True, disk=card); boots.append(b)
        check("remember: no saved pairing yet, so the plain ready line", b.wait_for("bt: ready, type bt scan", 30), b.uart()[-600:])
        check("remember: the Terminal opens", b.terminal(), b.uart()[-300:])
        b.type("bt")
        check("remember: bt says saved pairing: no", b.wait_for("bt: saved pairing: no", 10), b.uart()[-300:])
        b.type("bt scan")
        check("remember: the scan ends", b.wait_for("bt: bt pair connects to number 1", 20), b.uart()[-600:])
        b.type("bt pair")
        check("remember: paired", b.wait_for("bt: paired", 30), b.uart()[-600:])
        check("remember: bt: pairing saved", b.wait_for("bt: pairing saved", 15), b.uart()[-600:])
        check("remember: bt: mouse ready", b.wait_for("bt: mouse ready", 20), b.uart()[-600:])
        time.sleep(0.5)
        b.type("bt")
        check("remember: bt says saved pairing: yes", b.wait_for("bt: saved pairing: yes", 10), b.uart()[-300:])
        check("remember: the line says only that, once", b.uart().count("bt: pairing saved") == 1, lines(b.uart()))
        check("remember: the fake mouse found nothing wrong", not b.fake.problems, b.fake.problems)
        keys = (b.fake.ltk, b.fake.rand, b.fake.ediv)
        check("remember: no random, confirm value or key on the UART", not leaked(b.uart(), b.fake.secrets), leaked(b.uart(), b.fake.secrets))
        check("remember: no crash", "KERNEL CRASH" not in b.uart(), b.uart()[-300:])
        b.close()
        time.sleep(0.3)

        # reconnect: the saved mouse is dialled at boot, no scan, no pairing
        b = Boot("reconnect", "reconnect", disk=clone("reconnect"), saved=keys); boots.append(b)
        check("reconnect: the radio is ready and dials the saved mouse by name",
              b.wait_for("bt: ready, reconnecting Microsoft Bluetooth Mouse, wake it", 30), b.uart()[-600:])
        check("reconnect: bt: connected", b.wait_for("bt: connected", 15), b.uart()[-400:])
        check("reconnect: bt: reconnected", b.wait_for("bt: reconnected", 20), b.uart()[-400:])
        check("reconnect: bt: mouse ready", b.wait_for("bt: mouse ready", 20), b.uart()[-400:])
        rops = [op for op, _ in b.fake.log]
        check("reconnect: no scan, and LE Start Encryption sent once", 0x200C not in rops and 0x200B not in rops and rops.count(0x2019) == 1, [hex(o) for o in rops[-10:]])
        check("reconnect: LE Create Connection goes to the saved address and type", any(op == 0x200D and p[5] == 1 and p[6:12] == MOUSE for op, p in b.fake.log), [hex(o) for o in rops[-10:]])
        check("reconnect: the same GATT steps run, after encryption", b.fake.writes == [(0x52, PM_VH, b"\x00"), (0x12, BOOT_CCCD, b"\x01\x00")], b.fake.writes)
        rl = lines(b.uart().split("bt: ready, reconnecting", 1)[-1])
        check("reconnect: one line per stage, no second save", rl == ["bt: connected, link interval 7.5 ms", "bt: reconnected", "bt: mouse ready"], rl)
        check("reconnect: the fake mouse found nothing wrong (saved LTK, Rand and EDIV, no SMP)", not b.fake.problems, b.fake.problems)
        check("reconnect: the terminal opens", b.terminal(), b.uart()[-300:])
        b.type("bt")
        check("reconnect: bt says the mouse is ready and the pairing is saved", b.wait_for("bt: mouse ready, Microsoft Bluetooth Mouse", 10)
              and b.wait_for("bt: saved pairing: yes", 10), b.uart()[-400:])
        check("reconnect: no random, confirm value or key on the UART", not leaked(b.uart(), [keys[0], keys[1]]), leaked(b.uart(), [keys[0], keys[1]]))
        check("reconnect: no crash", "KERNEL CRASH" not in b.uart(), b.uart()[-300:])
        b.close()

        # a mouse that never answers: tries of about 12 seconds with a pause between, the record kept, bt status says so, then bt forget
        b = Boot("nomouse", "nomouse", disk=clone("nomouse"), saved=keys); boots.append(b)
        check("nomouse: dials the saved mouse", b.wait_for("bt: ready, reconnecting", 30), b.uart()[-600:])
        t0 = time.time()
        check("nomouse: the first try ends in one plain line", b.wait_for("bt: no answer from the saved mouse, try 1 of 30", 40), b.uart()[-600:])
        took = time.time() - t0
        check("nomouse: after about 12 seconds", 9 < took < 20, took)
        check("nomouse: the try is cancelled", 0x200E in [op for op, _ in b.fake.log], [hex(op) for op, _ in b.fake.log][-6:])
        check("nomouse: the Terminal opens", b.terminal(), b.uart()[-300:])
        b.type("bt status")
        check("nomouse: bt status says searching, the address masked but its last byte, and the attempts",
              b.wait_for("bt: state searching, mouse **:**:**:**:**:21, attempts", 10), b.uart()[-400:])
        check("nomouse: bt status gives the last error", b.wait_for("bt: last error: no answer from the saved mouse, try", 10), b.uart()[-400:])
        check("nomouse: and the pairing is kept", b.wait_for("bt: saved pairing: yes", 10), b.uart()[-400:])
        check("nomouse: the full address is never printed", "c4:8e:8f:65:43" not in b.uart().lower(), lines(b.uart()))
        b.type("bt forget")
        check("nomouse: bt forget says so", b.wait_for("bt: saved pairing forgotten", 10), b.uart()[-300:])
        b.type("bt")
        check("nomouse: then saved pairing: no", b.wait_for("bt: saved pairing: no", 10), b.uart()[-300:])
        check("nomouse: no key on the UART", not leaked(b.uart(), [keys[0], keys[1]]), leaked(b.uart(), [keys[0], keys[1]]))
        check("nomouse: no crash", "KERNEL CRASH" not in b.uart(), b.uart()[-300:])
        b.close()

        # BT5: a mouse that ignores Protocol Mode and sends packed 12 bit X and Y: the layout comes from its report map
        b = Boot("packed", "packed", screen=True, disk=clone("packed"), saved=keys); boots.append(b)
        check("packed: reconnects", b.wait_for("bt: mouse ready", 40), b.uart()[-600:])
        def moves2(out):
            pts = [tuple(map(int, m)) for m in re.findall(r"^mouse (\d+),(\d+)$", out, re.M)]
            return [(q[0] - a[0], q[1] - a[1]) for a, q in zip(pts, pts[1:])]
        end = time.time() + 15
        while time.time() < end and (-5, 3) not in moves2(b.uart()): time.sleep(0.2)
        check("packed: 12 bit X and Y move the pointer by their real deltas, signs kept (7,11 then -5,3)", (7, 11) in moves2(b.uart()) and (-5, 3) in moves2(b.uart()), moves2(b.uart()))
        check("packed: the button comes from bit 0 of the first byte", b.wait_for("key 272 down", 10), b.uart()[-400:])
        check("packed: the map was read in 22 byte pieces", len(PACKED_MAP) > 22, len(PACKED_MAP))
        check("packed: the fake mouse found nothing wrong", not b.fake.problems, b.fake.problems)
        check("packed: the Terminal opens", b.terminal(), b.uart()[-300:])
        b.type("bt status")
        check("packed: bt status shows the last report and where the layout came from",
              b.wait_for("bt: last report 5 bytes: 01 00 00 00 00, layout from the mouse's report map", 10), b.uart()[-400:])
        check("packed: no crash", "KERNEL CRASH" not in b.uart(), b.uart()[-300:])
        b.close()

        # BT4: the saved mouse is asleep at boot, wakes on the second try; then it drops and comes back. No typing, no pairing.
        b = Boot("autorc", "flaky", screen=True, disk=clone("autorc"), saved=keys); boots.append(b)
        b.fake.reports = [bytes([0, 7, 0]), bytes([0, 0, 9])]
        check("autorc: the radio dials the saved mouse at boot", b.wait_for("bt: ready, reconnecting Microsoft Bluetooth Mouse, wake it", 30), b.uart()[-600:])
        check("autorc: the first try gets no answer", b.wait_for("bt: no answer from the saved mouse, try 1 of 30", 40), b.uart()[-600:])
        check("autorc: the second try reconnects on its own", b.wait_for("bt: reconnected", 40), b.uart()[-600:])
        check("autorc: bt: mouse ready", b.wait_for("bt: mouse ready", 20), b.uart()[-400:])
        def moves(out):   # the pointer's steps, from the `mouse X,Y` lines
            pts = [tuple(map(int, m)) for m in re.findall(r"^mouse (\d+),(\d+)$", out, re.M)]
            return [(b2[0] - a[0], b2[1] - a[1]) for a, b2 in zip(pts, pts[1:])]
        def pairs(out): return moves(out).count((0, 9))   # the first report's start is not printed, the second's step is
        end = time.time() + 15
        while time.time() < end and not pairs(b.uart()): time.sleep(0.2)
        check("autorc: pointer events arrive after the reconnect", pairs(b.uart()) == 1, moves(b.uart())[-6:])
        cops = [op for op, _ in b.fake.log]
        check("autorc: two dials, one Start Encryption, no scan, one Cancel", cops.count(0x200D) == 2 and cops.count(0x2019) == 1 and 0x200C not in cops and cops.count(0x200E) == 1, [hex(o) for o in cops[-8:]])
        b.fake.inject.append(b"\x04\x05\x04\x00" + u16(HANDLE) + b"\x08")   # it sleeps: supervision timeout
        check("autorc: the drop is one plain line", b.wait_for("bt: the mouse disconnected, reason 0x8", 10), b.uart()[-400:])
        check("autorc: it comes back with no typing", b.wait_for("bt: reconnected", 30, count=2), b.uart()[-600:])
        check("autorc: bt: mouse ready again", b.wait_for("bt: mouse ready", 20, count=2), b.uart()[-400:])
        end = time.time() + 15
        while time.time() < end and pairs(b.uart()) < 2: time.sleep(0.2)
        check("autorc: pointer events arrive again", pairs(b.uart()) == 2, moves(b.uart())[-8:])
        check("autorc: notifications turned on again after the second encryption",
              b.fake.writes == [(0x52, PM_VH, b"\x00"), (0x12, BOOT_CCCD, b"\x01\x00")] * 2, b.fake.writes)
        check("autorc: the fake mouse found nothing wrong (saved key both times, no SMP)", not b.fake.problems, b.fake.problems)
        check("autorc: the Terminal opens", b.terminal(), b.uart()[-300:])
        b.type("bt status")
        check("autorc: bt status says connected", b.wait_for("bt: state connected, mouse **:**:**:**:**:21, attempts 1", 10), b.uart()[-400:])
        check("autorc: no key and no full address on the UART", not leaked(b.uart(), [keys[0], keys[1]]) and "c4:8e:8f:65:43" not in b.uart().lower() and "c48e8f6543" not in b.uart().lower(), leaked(b.uart(), [keys[0], keys[1]]))
        check("autorc: no crash", "KERNEL CRASH" not in b.uart(), b.uart()[-300:])
        b.close()

        # a mouse that no longer knows the key: Encryption Change 0x06 erases the record
        b = Boot("rejected", "rejected", disk=clone("rejected"), saved=keys); boots.append(b)
        check("rejected: dials the saved mouse", b.wait_for("bt: ready, reconnecting", 30), b.uart()[-600:])
        check("rejected: the rejected key is forgotten, in one plain line", b.wait_for("bt: the mouse no longer knows the saved pairing, forgotten", 30), b.uart()[-600:])
        check("rejected: the Terminal opens", b.terminal(), b.uart()[-300:])
        time.sleep(1.0)
        b.type("bt")
        check("rejected: then saved pairing: no", b.wait_for("bt: saved pairing: no", 10), b.uart()[-400:])
        check("rejected: the fake mouse saw the saved key", not b.fake.problems, b.fake.problems)
        check("rejected: no key on the UART", not leaked(b.uart(), [keys[0], keys[1]]), leaked(b.uart(), [keys[0], keys[1]]))
        check("rejected: no crash", "KERNEL CRASH" not in b.uart(), b.uart()[-300:])
        b.close()

        # nothing saved on this card: the plain flow, and bt forget has nothing to do
        fresh = os.path.join(tmp, "card-fresh.img"); build_card(fresh, False)
        b = Boot("norecord", "good", disk=fresh); boots.append(b)
        check("norecord: the radio is ready the old way", b.wait_for("bt: ready, type bt scan", 30), b.uart()[-600:])
        check("norecord: the Terminal opens", b.terminal(), b.uart()[-300:])
        b.type("bt forget")
        check("norecord: bt forget says there was none", b.wait_for("bt: no saved pairing to forget", 10), b.uart()[-300:])
        check("norecord: no connection was tried", 0x200D not in [op for op, _ in b.fake.log], [hex(op) for op, _ in b.fake.log][-6:])
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
print("PASS: HCI over the second UART: patch upload in order, version, baud, event mask, an LE scan that lists the mouse by "
      "its scan-response name; bt pair connects, pairs with Just Works (c1 and s1 checked with AES), encrypts, finds the "
      "boot mouse report over GATT and its reports move the pointer and click the dock; the pairing is saved to a card "
      "and a second boot reconnects with the saved key, keeps it through a mouse that does not answer, reconnects on its own when the mouse wakes late or drops the link, erases it when the mouse "
      "rejects it, and bt forget erases it; a wrong confirm, a silent or "
      "out-of-order radio each end in one plain line with the kernel up, and no key ever reaches the UART")
