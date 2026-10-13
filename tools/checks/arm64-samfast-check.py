#!/usr/bin/env python3
"""`sam` answers fast: sums on the Pi, a remembered relay, a 30 second limit, and a line that says where the time went.

Built like arm64-relay-discovery-check.py: TLS on, a throwaway private CA, a dead baked relay address (10.0.2.77) and a
Python LAN on QEMU's network hub that plays the broadcast side. The real tools/claude-relay/relay.py (stub `claude`)
answers; the check card (sdcard.py) is a virtio disk so the relay's address can survive a reboot. Two boots:
  1. `sam 10+10` prints `sam: 20` with no relay traffic at all (no probe, no request, no "thinking"); so do a bracketed
     power sum and "what is 1.5+1.5"; a sentence that merely contains digits still goes to the relay.
     `sam hello` finds the relay by one probe and prints the answer and one `sam: N.Ns (find a, tls b, wait c)` line;
     `sam again` goes to the cached relay with 0 new probes; JT/RELAY.TXT on the card holds `10.0.2.2:PORT` and no token.
     The relay is then swapped for a TLS server that takes the request and never answers: the request body asks for one
     or two sentences, and `sam: no answer in 30s (relay at 10.0.2.2)` lands within the budget, the prompt is free again
     (`sam 7*6` prints 42, no "still thinking") and the timing line follows.
  2. a fresh boot of the same card: the first question goes to the relay in RELAY.TXT with 0 probes, because the
     card remembered it.
Discriminating: send sums to the relay and step 1 fails; rediscover per question and the second probe count fails; drop
RELAY.TXT and boot 2 probes; leave ASK_TICKS at 180 s and the timeout line never comes.
Skips (exit 0) when clang's aarch64 target, ld.lld, openssl or qemu-system-aarch64 is missing, or QEMU predates dgram.
Usage: tools/checks/arm64-samfast-check.py   (from the repo root)
"""
import json, os, re, shutil, socket, ssl, struct, subprocess, sys, tempfile, threading, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from freeport import free_port
from sdcard import build_card

root = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
arch = os.path.join(root, "arch/arm64")
relay_py = os.path.join(root, "tools/claude-relay/relay.py")
if not all(shutil.which(t) for t in ("clang", "ld.lld", "qemu-system-aarch64", "openssl")):
    print("SKIP: clang, ld.lld, openssl or qemu-system-aarch64 not installed"); sys.exit(0)
if "dgram" not in subprocess.run(["qemu-system-aarch64", "-machine", "virt", "-netdev", "help"], capture_output=True, text=True).stdout:
    print("SKIP: this QEMU has no dgram netdev (7.2 or later), so the check cannot put a LAN on the hub"); sys.exit(0)

TOKEN = "arm-samfast-check-token-0123456789a"
ANSWER = "found you on the lan"
STUB = r'''#!/usr/bin/env python3
import json, os, sys, uuid
prompt = sys.stdin.read()
with open(os.environ["STUB_LOG"], "a") as f: f.write(json.dumps({"stdin": prompt}) + "\n")
print(json.dumps({"type": "result", "subtype": "success", "is_error": False, "result": os.environ["STUB_ANSWER"], "session_id": str(uuid.uuid4())}))
'''
fails = []
def check(name, ok, detail=""):
    print(("  ok: " if ok else "  FAIL: ") + name + (("  (" + detail[-400:] + ")") if detail and not ok else ""))
    if not ok: fails.append(name)

tmp = tempfile.mkdtemp(prefix="jt-arm-samfast-")
os.makedirs(tmp + "/bin")
with open(tmp + "/bin/claude", "w") as f: f.write(STUB)
os.chmod(tmp + "/bin/claude", 0o755)
token_file = tmp + "/token"
with open(token_file, "w") as f: f.write(TOKEN + "\n")
os.chmod(token_file, 0o600)
stub_log = tmp + "/stub.log"
port = free_port()

def sh(*args, cwd=tmp):
    subprocess.run(args, cwd=cwd, check=True, capture_output=True)
sh("openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "365", "-subj", "/CN=Joshua Tree relay CA",
   "-addext", "basicConstraints=critical,CA:TRUE", "-addext", "keyUsage=critical,keyCertSign,cRLSign",
   "-keyout", "ca.key", "-out", "ca.pem")
with open(tmp + "/server.ext", "w") as f:
    f.write("basicConstraints=critical,CA:FALSE\nkeyUsage=critical,digitalSignature,keyEncipherment\n"
            "extendedKeyUsage=serverAuth\nsubjectAltName=DNS:jt-relay\n")
sh("openssl", "req", "-new", "-newkey", "rsa:2048", "-nodes", "-subj", "/CN=jt-relay", "-keyout", "jt-relay.key", "-out", "server.csr")
sh("openssl", "x509", "-req", "-in", "server.csr", "-CA", "ca.pem", "-CAkey", "ca.key", "-CAcreateserial", "-days", "30",
   "-sha256", "-extfile", "server.ext", "-out", "jt-relay.pem")
cert = (tmp + "/jt-relay.pem", tmp + "/jt-relay.key")

base_env = {k: v for k, v in os.environ.items() if not k.startswith("CLAUDE_RELAY")}
def stub_calls():
    try: return [json.loads(l) for l in open(stub_log)]
    except FileNotFoundError: return []

relay = None
def start_relay():
    global relay
    stop_relay()
    env = dict(base_env, PATH=tmp + "/bin" + os.pathsep + os.environ["PATH"], STUB_LOG=stub_log, STUB_ANSWER=ANSWER, CLAUDE_RELAY_TOKEN=TOKEN)
    err = open(tmp + "/relay.log", "w+")
    relay = subprocess.Popen([sys.executable, relay_py, "--port", str(port), "--cwd", tmp, "--timeout", "20",
                              "--tls-cert", cert[0], "--tls-key", cert[1]], env=env, stdout=subprocess.DEVNULL, stderr=err)
    for _ in range(100):
        time.sleep(0.05)
        err.seek(0)
        if re.search(r"listening on 127\.0\.0\.1:\d+.*answering discovery", err.read()): return
        if relay.poll() is not None: break
    raise SystemExit("FAIL: the relay did not start: " + open(err.name).read())
def stop_relay():
    global relay
    if relay: relay.kill(); relay.wait(); relay = None

bodies, stall_done = [], threading.Event()
def start_staller():
    """A TLS server on the relay's port that reads the whole request and then says nothing."""
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER); ctx.load_cert_chain(*cert)
    srv = socket.socket(); srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", port)); srv.listen(4); srv.settimeout(0.3)
    def handle(c):
        try:
            c = ctx.wrap_socket(c, server_side=True); data = b""
            while b"\r\n\r\n" not in data: data += c.recv(4096)
            head, _, rest = data.partition(b"\r\n\r\n")
            m = re.search(rb"(?i)content-length:\s*(\d+)", head); need = int(m.group(1)) if m else 0
            while len(rest) < need: rest += c.recv(4096)
            bodies.append(rest.decode("utf-8", "replace")); stall_done.wait(60)
        except Exception: pass
        finally:
            try: c.close()
            except Exception: pass
    def serve():
        while not stall_done.is_set():
            try: c, _ = srv.accept()
            except socket.timeout: continue
            except OSError: break
            threading.Thread(target=handle, args=(c,), daemon=True).start()
        srv.close()
    th = threading.Thread(target=serve, daemon=True); th.start()
    return th

def csum(b):
    if len(b) % 2: b += b"\0"
    v = sum(struct.unpack("!%dH" % (len(b) // 2), b))
    while v >> 16: v = (v & 0xFFFF) + (v >> 16)
    return ~v & 0xFFFF

class Lan:
    """The other end of the hub's UDP port: one Ethernet frame per datagram. A broadcast UDP frame to the relay's port
    is handed to the relay's discovery socket on 127.0.0.1, and its answer is sent back to the guest as a frame from
    10.0.2.2. Everything else on the wire is ignored, so TCP and ARP stay with the user network."""
    def __init__(self):
        self.s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); self.s.bind(("127.0.0.1", 0)); self.s.settimeout(0.2)
        self.port, self.qemu_port, self.probes, self.done = self.s.getsockname()[1], free_port(), 0, threading.Event()
        self.th = threading.Thread(target=self.run, daemon=True); self.th.start()
    def run(self):
        while not self.done.is_set():
            try: f, _ = self.s.recvfrom(2048)
            except socket.timeout: continue
            except OSError: return
            if len(f) < 42 or f[:6] != b"\xff" * 6 or f[12:14] != b"\x08\x00" or f[23] != 17: continue
            ihl = (f[14] & 15) * 4
            src_ip, dst_ip = f[26:30], f[30:34]
            sport, dport, ulen = struct.unpack("!HHH", f[14 + ihl:14 + ihl + 6])
            if dport != port or dst_ip[3] != 255: continue
            self.probes += 1
            ask = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); ask.settimeout(1)
            try:
                ask.sendto(f[14 + ihl + 8:14 + ihl + ulen], ("127.0.0.1", port)); reply = ask.recv(512)
            except OSError: continue
            finally: ask.close()
            udp = struct.pack("!HHHH", port, sport, 8 + len(reply), 0) + reply
            ip = struct.pack("!BBHHHBBH4s4s", 0x45, 0, 20 + len(udp), 0, 0, 64, 17, 0, socket.inet_aton("10.0.2.2"), src_ip)
            ip = ip[:10] + struct.pack("!H", csum(ip)) + ip[12:]
            self.s.sendto(f[6:12] + bytes.fromhex("52550a000202") + b"\x08\x00" + ip + udp, ("127.0.0.1", self.qemu_port))
    def netdev(self):
        return ["-netdev", "user,id=u", "-netdev", "hubport,id=hu,hubid=0,netdev=u",
                "-netdev", "dgram,id=l,local.type=inet,local.host=127.0.0.1,local.port=%d,remote.type=inet,"
                           "remote.host=127.0.0.1,remote.port=%d" % (self.qemu_port, self.port),
                "-netdev", "hubport,id=hl,hubid=0,netdev=l", "-netdev", "hubport,id=n,hubid=0"]
    def close(self): self.done.set(); self.s.close()

class Boot:
    def __init__(self, name, lan, disk):
        self.log, sock = "%s/%s.uart" % (tmp, name), "%s/%s.qmp" % (tmp, name)
        self.q = subprocess.Popen(["qemu-system-aarch64", "-machine", "virt", "-cpu", "cortex-a72", "-m", "256",
                                   "-global", "virtio-mmio.force-legacy=false", *lan.netdev(),
                                   "-device", "virtio-net-device,netdev=n", "-device", "ramfb",
                                   "-drive", "if=none,id=card,file=%s,format=raw" % disk, "-device", "virtio-blk-device,drive=card",
                                   "-device", "virtio-keyboard-device", "-display", "none", "-serial", "file:" + self.log,
                                   "-qmp", "unix:%s,server,nowait" % sock, "-kernel", os.path.join(arch, "kernel8.elf")],
                                  stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        if not self.wait_for("M2 input ready", 30): raise SystemExit("FAIL: %s did not boot: %r" % (name, self.uart()[-600:]))
        time.sleep(0.5)
        self.s = socket.socket(socket.AF_UNIX); self.s.settimeout(20); self.s.connect(sock); self.f = self.s.makefile("rw")
        self.f.readline(); self.cmd("qmp_capabilities")
        n = self.uart().count("terminal open"); self.key("f2")
        if not self.wait_for("terminal open", 10, n + 1): raise SystemExit("FAIL: F2 did not open the Terminal")
        time.sleep(0.4)
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
    def key(self, *qcodes):
        self.cmd("send-key", keys=[{"type": "qcode", "data": k} for k in qcodes]); time.sleep(0.12)
    SHIFT = {"+": ("shift", "equal"), "*": ("shift", "8"), "^": ("shift", "6"), "(": ("shift", "9"), ")": ("shift", "0"), "?": ("shift", "slash"),
             "-": ("minus",), "/": ("slash",), ".": ("dot",), " ": ("spc",)}
    def type(self, text):
        for ch in text: self.key(*self.SHIFT.get(ch, (ch,)))
        self.key("ret")
    def line(self, text, until, secs=60):   # types `sam TEXT`, waits for `until` to show after it; returns what printed since
        mark = len(self.uart()); self.type("sam " + text)
        end = time.time() + secs
        while time.time() < end and until not in self.uart()[mark:]: time.sleep(0.1)
        time.sleep(0.8)
        return self.uart()[mark:]
    def close(self): self.q.kill(); self.q.wait()

TIMING = re.compile(r"sam: \d+\.\ds \(find \d+\.\d, tls \d+\.\d, wait \d+\.\d\)")
try:
    print("arm64 samfast: baked host 10.0.2.77 (dead), relay on 127.0.0.1:%d, card as a virtio disk" % port)
    subprocess.run(["make", "-C", arch, "clean"], check=True, capture_output=True, timeout=120)
    env = dict(base_env, CLAUDE_RELAY_HOST="10.0.2.77", CLAUDE_RELAY_PORT=str(port), CLAUDE_RELAY_TOKEN_FILE=token_file, CLAUDE_RELAY_TLS="1")
    r = subprocess.run(["make", "-C", arch, "TLS_TA=" + tmp + "/ca.pem"], env=env, capture_output=True, text=True, timeout=600)
    if r.returncode: print("FAIL: arch/arm64 does not build:\n" + r.stdout[-2000:] + r.stderr[-2000:]); sys.exit(1)
    card = tmp + "/card.img"; build_card(card, False)
    start_relay()
    lan = Lan()
    b = Boot("one", lan, card)
    try:
        check("DHCP leased an address", "net dhcp 10.0.2.15 gw 10.0.2.2" in b.uart(), b.uart()[-300:])
        # 1. sums stay on the Pi
        for text, want in (("10+10", "sam: 20\n"), ('"2*(3+4)^2"', "sam: 98\n"), ("what is 1.5+1.5?", "sam: 3\n")):
            out = b.line(text, want, 15)
            check("sam %s prints %s on the Pi" % (text, want.strip()), want in out and "thinking" not in out, out[-200:])
        check("... with no probe and no request sent", lan.probes == 0 and not stub_calls() and "agent: step" not in b.uart(), "probes=%d" % lan.probes)
        # 2. a real question: one probe, answer, timing line
        out = b.line("hello there 2 friends", ANSWER)
        p1 = lan.probes
        check("a sentence with digits goes to the relay and is answered", ANSWER in out and len(stub_calls()) == 1, out[-300:])
        check("it found the relay with a probe", p1 >= 1 and "relay found at 10.0.2.2" in out, "probes=%d" % p1)
        check("the timing line prints once", len(TIMING.findall(out)) == 1, out[-300:])
        out = b.line("again", ANSWER)
        check("the follow-up goes to the cached relay: 0 new probes", ANSWER in out and lan.probes == p1 and "relay found" not in out, "probes=%d" % lan.probes)
        got = subprocess.run(["mtype", "-i", card + "@@1M", "::JT/RELAY.TXT"], capture_output=True, text=True).stdout
        check("JT/RELAY.TXT on the card holds 10.0.2.2:%d and no token" % port, got.strip() == "10.0.2.2:%d" % port and TOKEN not in got, repr(got))
        # 3. a relay that never answers
        stop_relay(); stall_th = start_staller(); time.sleep(0.3)
        t0 = time.time(); mark = len(b.uart())
        b.type("sam stuck question")
        ok = b.wait_for("sam: no answer in 30s (relay at 10.0.2.2)", 60)
        took = time.time() - t0
        time.sleep(0.8); out = b.uart()[mark:]
        check("a stalled relay ends with sam: no answer in 30s (relay at 10.0.2.2)", ok, out[-300:])
        check("... inside the budget (30 s plus connect time)", ok and 25 < took < 45, "%.1fs" % took)
        check("... and the timing line follows", bool(TIMING.search(out)), out[-300:])
        check("the request asked for one or two sentences", bool(bodies) and "one or two sentences" in bodies[0] and TOKEN not in bodies[0], repr(bodies[:1]))
        out = b.line("7*6", "sam: 42\n", 15)
        check("the prompt is free again: sam 7*6 prints 42 and not still thinking", "sam: 42\n" in out and "still thinking" not in out, out[-200:])
        check("the token never appears on the UART", TOKEN not in b.uart())
    finally:
        b.close(); stall_done.set(); stall_th.join(3); time.sleep(0.3)
    # 4. a fresh boot: the card remembers the relay
    start_relay(); before = lan.probes
    b = Boot("two", lan, card)
    try:
        out = b.line("good morning 3 times", ANSWER)
        check("after a reboot the card's relay is used first: 0 probes", ANSWER in out and lan.probes == before and "relay found" not in out, "probes=%d" % (lan.probes - before))
    finally:
        b.close(); lan.close()
finally:
    stop_relay(); stall_done.set()
    subprocess.run(["make", "-C", arch, "clean"], capture_output=True, timeout=120)
    shutil.rmtree(tmp, ignore_errors=True)
if fails:
    print("FAIL: %d of the samfast checks failed" % len(fails)); sys.exit(1)
print("PASS: sums run on the Pi, the relay's address is remembered in memory and on the card (0 probes after the first), "
      "a stalled relay ends at 30 seconds with the relay's address, and every question prints where its time went")
