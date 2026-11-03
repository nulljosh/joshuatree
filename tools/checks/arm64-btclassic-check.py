#!/usr/bin/env python3
"""Classic Bluetooth (12.1, arch/arm64/btc.c): `bt find` lists headphones and keyboards, `bt connect N` pairs with Secure
Simple Pairing and keeps the link key in JT/BTLINK.BIN, and a later `bt connect` uses it.

As arm64-bt-check.py does, QEMU's virt machine gets a second PL011 whose -serial is a Unix socket to a fake radio, and the
build has no patch file (BT_HCD names nothing), so bring-up is Reset, Read Local Version, the baud command and Set Event
Mask. The opcodes and event layouts the fake checks are typed in here from Linux's include/net/bluetooth/hci.h (v6.12),
not from btc.c. A FAT32 card (sdcard.py) is the virtio disk. Three boots on one card:
  1. pair: `bt find` sends Set Event Mask with events 0x29-0x36 on and LE Meta kept, Write Simple Pairing Mode 1, Write
     Inquiry Mode 2, then Inquiry (GIAC 0x9E8B33, 6 x 1.28 s). The radio answers with an Extended Inquiry Result for "JT
     Headphones" (Class of Device 0x240418, headphones) and an Inquiry Result with RSSI for a nameless keyboard (0x002540),
     then Inquiry Complete: the list names "JT Headphones, headphones" first and "Unnamed keyboard" second. `bt connect 1`:
     Create Connection to the headphones' address (packet types 0xCC18, its page scan mode, role switch allowed),
     Connection Complete, Authentication Requested on that handle; the radio's Link Key Request gets the negative reply
     (nothing saved), IO Capability Request gets NoInputNoOutput, no OOB, general bonding (0x03 0x00 0x04), User
     Confirmation Request gets the yes, Simple Pairing Complete, Link Key Notification with a random key: `bt: pairing
     saved`; Authentication Complete, Set Connection Encryption on, Encryption Change: `bt: JT Headphones connected`. The
     key is never on the UART, and BTLINK.BIN on the card holds "JTL1", the address and that key.
  2. reconnect: `bt connect` with no number dials the saved address, and the Link Key Request gets Link Key Reply with
     the saved key; no IO Capability exchange is needed; `connected`. `bt` shows the saved device.
  3. forgotten: the headphones answer Authentication Complete 0x06 (key missing): the pairing is forgotten (BTLINK.BIN
     gone) and the link hung up with Disconnect.
  4. keyboard (12.2): `bt find`, `bt connect 2` pairs the keyboard the same way, then, because its Class of Device says
     peripheral, HID over L2CAP: a Connection Request on the signalling channel (CID 1) for PSM 0x11 (control), the
     keyboard's Information Request answered "not supported", a Configuration Request each way (ours asks MTU 672), then
     PSM 0x13 (interrupt) the same, then SET_PROTOCOL boot (0x70) on control: `bt: keyboard ready`. Two boot reports on
     interrupt (DATA Input 0xA1, report ID 1: H down, then nothing held) type `key 35 down` and `key 35 up`.
Discriminating: answer every Link Key Request with the negative reply and boot 2's Link Key Reply never comes; drop the
interrupt channel's reports in btc.c's on_data and boot 4 never types the H.
Skips (exit 0) without clang, ld.lld, qemu-system-aarch64 or mtools, or a QEMU that builds no second PL011.
Usage: tools/checks/arm64-btclassic-check.py   (from the repo root)
"""
import json, os, re, shutil, socket, struct, subprocess, sys, threading, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from scratch import scratch_dir
from sdcard import PART, build_card

root = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
arch = os.path.join(root, "arch/arm64")
if not all(shutil.which(t) for t in ("clang", "ld.lld", "qemu-system-aarch64", "mtype", "mdir")):
    print("SKIP: clang, ld.lld, qemu-system-aarch64 or mtools not installed"); sys.exit(0)
tmp = scratch_dir("btclassic")
dtb = os.path.join(tmp, "virt.dtb")
subprocess.run(["qemu-system-aarch64", "-machine", "virt,dumpdtb=" + dtb, "-cpu", "cortex-a72", "-m", "256", "-display", "none",
                "-serial", "null", "-serial", "null"], capture_output=True, timeout=60)
if not (os.path.exists(dtb) and b"pl011@9040000" in open(dtb, "rb").read()):
    print("SKIP: this QEMU builds no second PL011 on virt"); sys.exit(0)

# Linux hci.h
OP_INQUIRY, OP_CREATE_CONN, OP_DISCONNECT, OP_LINK_KEY_REPLY, OP_LINK_KEY_NEG = 0x0401, 0x0405, 0x0406, 0x040B, 0x040C
OP_AUTH_REQUESTED, OP_SET_CONN_ENCRYPT, OP_IO_CAP_REPLY, OP_USER_CONFIRM = 0x0411, 0x0413, 0x042B, 0x042C
OP_EVENT_MASK, OP_INQUIRY_MODE, OP_SSP_MODE = 0x0C01, 0x0C45, 0x0C56
EV_INQ_COMPLETE, EV_CONN_COMPLETE, EV_DISCONN, EV_AUTH_COMPLETE, EV_ENCRYPT = 0x01, 0x03, 0x05, 0x06, 0x08
EV_LINK_KEY_REQ, EV_LINK_KEY_NOTIFY, EV_INQ_RSSI, EV_EXT_INQ = 0x17, 0x18, 0x22, 0x2F
EV_IO_CAPA_REQ, EV_IO_CAPA_REPLY, EV_USER_CONFIRM, EV_SSP_COMPLETE = 0x31, 0x32, 0x33, 0x36

HEAD = bytes([0x66, 0x55, 0x44, 0x33, 0x22, 0x11])   # 11:22:33:44:55:66
KEYB = bytes([0x01, 0x02, 0x03, 0x04, 0x05, 0x06])
HEAD_COD, KEYB_COD = bytes([0x18, 0x04, 0x24]), bytes([0x40, 0x25, 0x00])
HANDLE = 0x000B
KEY = os.urandom(16)
def u16(v): return struct.pack("<H", v)

class Fake(threading.Thread):
    def __init__(self, path, mode):
        super().__init__(daemon=True)
        self.mode, self.log, self.problems = mode, [], []
        self.peer = KEYB if mode == "keyboard" else HEAD
        self.sig, self.chan, self.ctrl_data = [], {}, []   # signalling commands from the kernel; PSM -> (kernel CID, our CID)
        self.srv = socket.socket(socket.AF_UNIX); self.srv.bind(path); self.srv.listen(1); self.srv.settimeout(60)
    def run(self):
        try: self.c, _ = self.srv.accept()
        except OSError: return
        buf = b""
        while True:
            try: d = self.c.recv(4096)
            except OSError: return
            if not d: return
            buf += d
            while buf:
                if buf[0] == 1:
                    if len(buf) < 4 or len(buf) < 4 + buf[3]: break
                    op, p = buf[1] | buf[2] << 8, buf[4:4 + buf[3]]; buf = buf[4 + buf[3]:]
                    self.answer(op, p)
                elif buf[0] == 2:
                    if len(buf) < 5 or len(buf) < 5 + struct.unpack("<H", buf[3:5])[0]: break
                    hdr, n = struct.unpack("<HH", buf[1:5]); data = buf[5:5 + n]; buf = buf[5 + n:]
                    self.on_acl(hdr, data)
                else: self.problems.append("stray byte %02x" % buf[0]); buf = buf[1:]
    def send(self, b):
        time.sleep(0.03)
        try: self.c.sendall(b)
        except OSError: pass
    def event(self, code, body): self.send(bytes([4, code, len(body)]) + body)
    def complete(self, op, ret=b"\x00"): self.event(0x0E, b"\x01" + u16(op) + ret)
    def status(self, op, st=0): self.event(0x0F, bytes([st, 1]) + u16(op))
    def l2(self, cid, payload):   # one L2CAP frame to the kernel, a start packet on our handle
        f = struct.pack("<HH", len(payload), cid) + payload
        self.send(b"\x02" + struct.pack("<HH", HANDLE | 0x2000, len(f)) + f)
    def on_acl(self, hdr, data):
        if hdr & 0xFFF != HANDLE or hdr >> 12 & 3 != 2 or len(data) < 4: self.problems.append("ACL not a start on our handle: %04x" % hdr); return
        n, cid = struct.unpack("<HH", data[:4]); p = data[4:4 + n]
        self.event(0x13, b"\x01" + u16(HANDLE) + u16(1))   # Number of Completed Packets: that buffer is free again
        if cid == 1: self.on_sig(p); return
        if cid == 0x0070:
            self.ctrl_data.append(p)
            if p == b"\x70":   # SET_PROTOCOL boot: HANDSHAKE success, then H down and up on interrupt
                self.l2(self.chan[0x11][0], b"\x00")
                intr = self.chan[0x13][0]
                self.l2(intr, b"\xa1\x01\x00\x00\x0b\x00\x00\x00\x00\x00"); time.sleep(0.2)
                self.l2(intr, b"\xa1\x01" + bytes(8))
    def on_sig(self, p):
        code, ident, ln = p[0], p[1], p[2] | p[3] << 8; d = p[4:4 + ln]
        self.sig.append((code, d))
        if code == 0x02:   # Connection Request: PSM, the kernel's CID
            psm, scid = struct.unpack("<HH", d[:4]); ours = 0x0070 if psm == 0x11 else 0x0071
            self.chan[psm] = (scid, ours)
            if psm == 0x11: self.l2(1, b"\x0a\x40\x02\x00\x02\x00")   # its own Information Request first (extended features)
            self.l2(1, bytes([0x03, ident, 8, 0]) + struct.pack("<HHHH", ours, scid, 0, 0))
            self.l2(1, bytes([0x04, 0x41 + len(self.chan), 8, 0]) + struct.pack("<HH", scid, 0) + b"\x01\x02\xa0\x02")
        elif code == 0x04:   # the kernel's Configuration Request: must name our CID
            dcid = struct.unpack("<H", d[:2])[0]
            k = [scid for scid, ours in self.chan.values() if ours == dcid]
            if not k: self.problems.append("Configuration Request for CID %04x" % dcid); return
            self.l2(1, bytes([0x05, ident, 6, 0]) + struct.pack("<HHH", k[0], 0, 0))
    def answer(self, op, p):
        self.log.append((op, p))
        if op == 0x1001: self.complete(op, bytes([0, 9, 0, 0, 9, 0x0F, 0, 0x19, 0x61])); return
        if op == 0x1005: self.complete(op, b"\x00" + u16(1021) + b"\x40" + u16(2) + u16(8)); return   # two ACL buffers: credits must come back
        if op == OP_INQUIRY:
            self.status(op)
            eir = bytes([14, 0x09]) + b"JT Headphones"
            self.event(EV_EXT_INQ, b"\x01" + HEAD + b"\x01\x00" + HEAD_COD + u16(0x1234) + bytes([256 - 50]) + eir + bytes(240 - len(eir)))
            self.event(EV_INQ_RSSI, b"\x01" + KEYB + b"\x01\x00" + KEYB_COD + u16(0) + bytes([256 - 70]))
            self.event(EV_INQ_COMPLETE, b"\x00"); return
        if op == OP_CREATE_CONN:
            self.status(op)
            self.event(EV_CONN_COMPLETE, b"\x00" + u16(HANDLE) + p[:6] + b"\x01\x00"); return
        if op == OP_AUTH_REQUESTED:
            self.status(op)
            self.event(EV_LINK_KEY_REQ, self.peer); return
        if op == OP_LINK_KEY_NEG:
            self.complete(op, b"\x00" + p[:6])
            self.event(EV_IO_CAPA_REQ, self.peer); return
        if op == OP_IO_CAP_REPLY:
            self.complete(op, b"\x00" + p[:6])
            self.event(EV_IO_CAPA_REPLY, self.peer + b"\x03\x00\x04")
            self.event(EV_USER_CONFIRM, self.peer + struct.pack("<I", 123456)); return
        if op == OP_USER_CONFIRM:
            self.complete(op, b"\x00" + p[:6])
            self.event(EV_SSP_COMPLETE, b"\x00" + self.peer)
            self.event(EV_LINK_KEY_NOTIFY, self.peer + KEY + b"\x04")
            self.event(EV_AUTH_COMPLETE, b"\x00" + u16(HANDLE)); return
        if op == OP_LINK_KEY_REPLY:
            self.complete(op, b"\x00" + p[:6])
            self.event(EV_AUTH_COMPLETE, (b"\x06" if self.mode == "forgotten" else b"\x00") + u16(HANDLE)); return
        if op == OP_SET_CONN_ENCRYPT:
            self.status(op)
            self.event(EV_ENCRYPT, b"\x00" + u16(HANDLE) + b"\x01"); return
        if op == OP_DISCONNECT:
            self.status(op)
            self.event(EV_DISCONN, b"\x00" + u16(HANDLE) + b"\x16"); return
        self.complete(op)

QK = {" ": "spc", ".": "dot", "-": "minus"}
class Boot:
    def __init__(self, name, mode, card):
        d = os.path.join(tmp, name); os.makedirs(d, exist_ok=True)
        self.log, qmp, sock = os.path.join(d, "uart"), os.path.join(d, "qmp"), os.path.join(d, "bt.sock")
        for f in (self.log, qmp, sock):
            if os.path.exists(f): os.remove(f)
        self.fake = Fake(sock, mode); self.fake.start()
        self.q = subprocess.Popen(["qemu-system-aarch64", "-machine", "virt", "-cpu", "cortex-a72", "-m", "256", "-nic", "none",
                                   "-global", "virtio-mmio.force-legacy=false", "-device", "virtio-keyboard-device", "-device", "ramfb",
                                   "-drive", "if=none,id=card,file=%s,format=raw" % card, "-device", "virtio-blk-device,drive=card",
                                   "-display", "none", "-serial", "file:" + self.log, "-serial", "unix:" + sock,
                                   "-qmp", "unix:%s,server,nowait" % qmp, "-kernel", os.path.join(arch, "kernel8.elf")],
                                  stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        if not self.wait_for("M2 input ready", 60): raise SystemExit("FAIL: %s did not boot: %r" % (name, self.uart()[-600:]))
        time.sleep(0.5)
        s = socket.socket(socket.AF_UNIX); s.settimeout(20); s.connect(qmp); self.f = s.makefile("rw")
        self.f.readline(); self.cmd("qmp_capabilities")
    def uart(self): return open(self.log, errors="replace").read() if os.path.exists(self.log) else ""
    def wait_for(self, text, secs, after=0):
        end = time.time() + secs
        while time.time() < end:
            if text in self.uart()[after:]: return True
            time.sleep(0.1)
        return False
    def cmd(self, c, **a):
        self.f.write(json.dumps({"execute": c, "arguments": a}) + "\n"); self.f.flush()
        while True:
            r = json.loads(self.f.readline())
            if "return" in r or "error" in r: return r
    def key(self, *k): self.cmd("send-key", keys=[{"type": "qcode", "data": x} for x in k]); time.sleep(0.1)
    def run(self, line, want, secs=20):
        n = len(self.uart())
        for ch in line: self.key(QK.get(ch, ch))
        self.key("ret"); ok = self.wait_for(want, secs, n); time.sleep(0.3)
        return ok, self.uart()[n:]
    def ops(self): return [op for op, _ in self.fake.log]
    def p(self, op): return [p for o, p in self.fake.log if o == op]
    def close(self): self.q.kill(); self.q.wait()

fails = []
def check(name, ok, detail=""):
    print(("  ok: " if ok else "  FAIL: ") + name + ("" if ok else "  " + repr(detail)[-700:]))
    if not ok: fails.append(name)
MT = dict(os.environ, MTOOLS_SKIP_CHECK="1")
def mt(card, *a): return subprocess.run([a[0], "-i", f"{card}@@{PART * 512}", *a[1:]], capture_output=True, env=MT)
def link_file(card):
    out = os.path.join(tmp, "link.bin")
    if os.path.exists(out): os.remove(out)
    mt(card, "mcopy", "::JT/BTLINK.BIN", out)
    return open(out, "rb").read() if os.path.exists(out) else None
def leaked(text): t = re.sub(r"[^0-9a-f]", "", text.lower()); return any(v[i:i + 6].hex() in t for v in (KEY, KEY[::-1]) for i in range(11))

subprocess.run(["make", "-C", arch, "clean"], capture_output=True, timeout=120)
r = subprocess.run(["make", "-C", arch, "kernel8.elf", "BT_HCD=" + os.path.join(tmp, "none.hcd")], capture_output=True, text=True, timeout=900)
if r.returncode: print(r.stdout[-1500:], r.stderr[-1500:]); print("FAIL: arch/arm64 does not build"); sys.exit(1)
card = os.path.join(tmp, "card.img"); build_card(card, False); mt(card, "mmd", "::JT")

b = Boot("pair", "pair", card)
try:
    check("the radio comes up", b.wait_for("bt: ready", 30), b.uart()[-500:])
    b.key("f2"); b.wait_for("terminal open", 10)
    ok, out = b.run("bt find", "bt: bt connect 1 pairs number 1", 20)
    check("bt find lists the headphones first, then the nameless keyboard",
          ok and "bt: 1. JT Headphones, headphones" in out and "bt: 2. Unnamed keyboard" in out, out[-600:])
    ops = b.ops()
    cl = [o for o in ops if o in (OP_EVENT_MASK, OP_SSP_MODE, OP_INQUIRY_MODE, OP_INQUIRY)]
    check("then Set Event Mask, Simple Pairing Mode, Inquiry Mode, Inquiry, in that order", cl == [OP_EVENT_MASK, OP_EVENT_MASK, OP_SSP_MODE, OP_INQUIRY_MODE, OP_INQUIRY], [hex(o) for o in cl])
    m = int.from_bytes(b.p(OP_EVENT_MASK)[-1], "little")
    check("the second event mask turns on IO Capability, User Confirmation, Link Key and extended inquiry, LE Meta kept",
          all(m >> (e - 1) & 1 for e in (EV_LINK_KEY_REQ, EV_LINK_KEY_NOTIFY, EV_EXT_INQ, EV_IO_CAPA_REQ, EV_USER_CONFIRM, EV_SSP_COMPLETE, 0x3E)), hex(m))
    check("Simple Pairing Mode 1, Inquiry Mode 2, Inquiry on the general access code for 6 x 1.28 s",
          b.p(OP_SSP_MODE) == [b"\x01"] and b.p(OP_INQUIRY_MODE) == [b"\x02"] and b.p(OP_INQUIRY) == [bytes([0x33, 0x8B, 0x9E, 6, 0])], b.fake.log[-4:])
    ok, out = b.run("bt connect 1", "bt: JT Headphones connected", 20)
    check("bt connect 1 pairs and connects the headphones", ok and "bt: pairing saved" in out, out[-600:])
    cc = b.p(OP_CREATE_CONN)
    check("Create Connection to the headphones: DM1-DH5, its page scan mode, role switch allowed",
          cc == [HEAD + bytes([0x18, 0xCC, 1, 0, 0, 0, 1])], [x.hex() for x in cc])
    seq = [o for o in b.ops() if o in (OP_AUTH_REQUESTED, OP_LINK_KEY_NEG, OP_LINK_KEY_REPLY, OP_IO_CAP_REPLY, OP_USER_CONFIRM, OP_SET_CONN_ENCRYPT)]
    check("Authentication Requested, the negative key reply, IO Capability, the yes, then encryption",
          seq == [OP_AUTH_REQUESTED, OP_LINK_KEY_NEG, OP_IO_CAP_REPLY, OP_USER_CONFIRM, OP_SET_CONN_ENCRYPT], [hex(o) for o in seq])
    check("IO Capability Reply is NoInputNoOutput, no OOB, general bonding",
          b.p(OP_IO_CAP_REPLY) == [HEAD + b"\x03\x00\x04"] and b.p(OP_AUTH_REQUESTED) == [u16(HANDLE)] and b.p(OP_SET_CONN_ENCRYPT) == [u16(HANDLE) + b"\x01"], b.fake.log[-6:])
    check("the link key is never on the UART", not leaked(b.uart()))
    check("the fake radio saw nothing malformed", not b.fake.problems, b.fake.problems)
finally: b.close()
f = link_file(card)
check("JT/BTLINK.BIN holds JTL1, the headphones' address and the key", f is not None and f[:4] == b"JTL1" and f[4:10] == HEAD and f[10:26] == KEY and b"JT Headphones" in f, f)

b = Boot("reconnect", "reconnect", card)
try:
    b.wait_for("bt: ready", 30); b.key("f2"); b.wait_for("terminal open", 10)
    ok, out = b.run("bt connect", "bt: JT Headphones connected", 20)
    check("bt connect with no number reconnects the saved headphones", ok and "pairing saved" not in out, out[-600:])
    check("it dials the saved address and answers the Link Key Request with the saved key, no IO Capability exchange",
          [p[:6] for p in b.p(OP_CREATE_CONN)] == [HEAD] and b.p(OP_LINK_KEY_REPLY) == [HEAD + KEY] and not b.p(OP_IO_CAP_REPLY) and not b.p(OP_LINK_KEY_NEG),
          [(hex(o), p.hex()[:16]) for o, p in b.fake.log[-6:]])
    ok, out = b.run("bt", "saved headphones or keyboard", 10)
    check("bt shows the saved device, connected", ok and "JT Headphones, connected" in out, out[-400:])
    check("the key is never on the UART after a reboot either", not leaked(b.uart()))
finally: b.close()

b = Boot("forgotten", "forgotten", card)
try:
    b.wait_for("bt: ready", 30); b.key("f2"); b.wait_for("terminal open", 10)
    ok, out = b.run("bt connect", "its pairing is forgotten", 20)
    time.sleep(0.5)
    check("a key the headphones no longer know is forgotten and the link hung up", ok and b.p(OP_DISCONNECT) == [u16(HANDLE) + b"\x13"], (out[-400:], b.fake.log[-3:]))
finally: b.close()
check("and BTLINK.BIN is gone from the card", link_file(card) is None)

b = Boot("keyboard", "keyboard", card)
try:
    b.wait_for("bt: ready", 30); b.key("f2"); b.wait_for("terminal open", 10)
    b.run("bt find", "bt: bt connect 1 pairs number 1", 20)
    ok, out = b.run("bt connect 2", "bt: keyboard ready", 25)
    check("bt connect 2 pairs the keyboard and opens its HID channels", ok and "keyboard connected" in out, (out[-500:], b.fake.sig))
    reqs = [(c, d) for c, d in b.fake.sig if c == 0x02]
    check("Connection Requests for PSM 0x11 (control) then 0x13 (interrupt), from CIDs 0x40 and 0x41",
          [struct.unpack("<HH", d[:4]) for c, d in reqs] == [(0x11, 0x40), (0x13, 0x41)], reqs)
    cfg = [d for c, d in b.fake.sig if c == 0x04]
    check("a Configuration Request on each channel, to the keyboard's CID, MTU 672",
          cfg == [struct.pack("<HH", 0x70, 0) + b"\x01\x02\xa0\x02", struct.pack("<HH", 0x71, 0) + b"\x01\x02\xa0\x02"], cfg)
    check("the keyboard's Information Request is answered not supported", (0x0B, b"\x02\x00\x01\x00") in b.fake.sig, b.fake.sig)
    check("SET_PROTOCOL boot on control", b.fake.ctrl_data[:1] == [b"\x70"], b.fake.ctrl_data)
    check("its boot reports type H: key 35 down, then up", b.wait_for("key 35 down", 5) and b.wait_for("key 35 up", 5), b.uart()[-600:])
    check("the fake keyboard saw nothing malformed", not b.fake.problems, b.fake.problems)
finally: b.close()

if fails: print("FAIL: " + "; ".join(fails)); sys.exit(1)
print("PASS: bt find lists classic headphones and keyboards by name and kind, bt connect pairs with Secure Simple Pairing and keeps the link key in JT/BTLINK.BIN, a later bt connect uses it, a key the device forgot is erased, and a classic keyboard opens HID control and interrupt over L2CAP in boot protocol and types")
