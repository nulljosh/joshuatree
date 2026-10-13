#!/usr/bin/env python3
"""The Pi finds the Claude relay by itself, and TLS checks the relay's fixed name, not its address (2.46).

The kernel is built with TLS on, a throwaway private CA as TLS_TA and CLAUDE_RELAY_HOST set to 10.0.2.77, an address
nothing on QEMU's user network answers: the stale address a card keeps after the router moves the Mac. The certificates
are made with the exact openssl commands in docs/GUIDE.md. The relay is the real tools/claude-relay/relay.py on
127.0.0.1 with a stub `claude`. QEMU's user network drops broadcasts, so the guest's card sits on a QEMU hub with two
other ports: the user network (DHCP, and TCP to the relay at 10.0.2.2) and a UDP socket to this script, which plays
the rest of the LAN. Every broadcast probe it sees goes to the relay's own discovery socket, and the relay's answer
goes back on the wire from 10.0.2.2, the Mac as the guest sees it. One boot, four questions in the Terminal:
  0. a bare TCP listener on the relay's port and no probe answerer: "samantha: no relay answered on the LAN", and the
     listener never saw a connection. This is the control: the baked 10.0.2.77 really does not reach the host.
  1. the relay with a certificate for DNS:jt-relay: the probe finds it ("samantha: relay found at 10.0.2.2") and the
     stub's answer comes back. The address in the certificate is nowhere near 10.0.2.77.
  2. a follow-up goes straight to the relay that answered last: answered, with no second "relay found" line.
  3. the relay restarted with a certificate from the same CA for the old IP name (DNS:10.0.2.2, IP:10.0.2.2), the
     relay itself answering the probe: "samantha: TLS failed", the stub is never run and the relay logs no request, so
     the token never left the Pi. The token is never on the UART.
Discriminating: drop the relay_discover call in arch/arm64/ask.c (or answer_probes in relay.py) and step 1 fails;
verify CLAUDE_HOST instead of jt-relay and step 1 fails on the name; drop the name check and step 3 is answered.
Skips (exit 0) when clang's aarch64 target, ld.lld, openssl or qemu-system-aarch64 is missing, or QEMU predates dgram.
Usage: tools/checks/arm64-relay-discovery-check.py   (from the repo root)
"""
import json, os, re, shutil, socket, struct, subprocess, sys, tempfile, threading, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from freeport import free_port

root = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
arch = os.path.join(root, "arch/arm64")
relay_py = os.path.join(root, "tools/claude-relay/relay.py")
if not all(shutil.which(t) for t in ("clang", "ld.lld", "qemu-system-aarch64", "openssl")):
    print("SKIP: clang, ld.lld, openssl or qemu-system-aarch64 not installed"); sys.exit(0)
if "dgram" not in subprocess.run(["qemu-system-aarch64", "-machine", "virt", "-netdev", "help"], capture_output=True, text=True).stdout:
    print("SKIP: this QEMU has no dgram netdev (7.2 or later), so the check cannot put a LAN on the hub"); sys.exit(0)

TOKEN = "arm-discovery-check-token-0123456789"
ANSWER = "found you on the lan"
STUB = r'''#!/usr/bin/env python3
import json, os, sys, uuid
prompt = sys.stdin.read()
with open(os.environ["STUB_LOG"], "a") as f: f.write(json.dumps({"stdin": prompt}) + "\n")
print(json.dumps({"type": "result", "subtype": "success", "is_error": False, "result": os.environ["STUB_ANSWER"], "session_id": str(uuid.uuid4())}))
'''
fails = []
def check(name, ok, detail=""):
    print(("  ok: " if ok else "  FAIL: ") + name + (("  (" + detail + ")") if detail and not ok else ""))
    if not ok: fails.append(name)

tmp = tempfile.mkdtemp(prefix="jt-arm-discovery-")
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
# docs/GUIDE.md, word for word: the private CA, then a server certificate for the fixed name
sh("openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "365", "-subj", "/CN=Joshua Tree relay CA",
   "-addext", "basicConstraints=critical,CA:TRUE", "-addext", "keyUsage=critical,keyCertSign,cRLSign",
   "-keyout", "ca.key", "-out", "ca.pem")
def server_cert(name, san):
    with open(tmp + "/server.ext", "w") as f:
        f.write("basicConstraints=critical,CA:FALSE\nkeyUsage=critical,digitalSignature,keyEncipherment\n"
                "extendedKeyUsage=serverAuth\nsubjectAltName=%s\n" % san)
    sh("openssl", "req", "-new", "-newkey", "rsa:2048", "-nodes", "-subj", "/CN=" + name, "-keyout", name + ".key", "-out", "server.csr")
    sh("openssl", "x509", "-req", "-in", "server.csr", "-CA", "ca.pem", "-CAkey", "ca.key", "-CAcreateserial", "-days", "30",
       "-sha256", "-extfile", "server.ext", "-out", name + ".pem")
    return tmp + "/" + name + ".pem", tmp + "/" + name + ".key"
good = server_cert("jt-relay", "DNS:jt-relay")
wrong = server_cert("10.0.2.2", "DNS:10.0.2.2,IP:10.0.2.2")   # the old style: right CA, an address for a name

base_env = {k: v for k, v in os.environ.items() if not k.startswith("CLAUDE_RELAY")}
def stub_calls():
    try: return [json.loads(l) for l in open(stub_log)]
    except FileNotFoundError: return []

relay, relay_logs = None, []
def start_relay(cert):
    global relay
    stop_relay()
    env = dict(base_env, PATH=tmp + "/bin" + os.pathsep + os.environ["PATH"], STUB_LOG=stub_log, STUB_ANSWER=ANSWER,
               CLAUDE_RELAY_TOKEN=TOKEN)
    err = open(tmp + "/relay-%d.log" % time.time_ns(), "w+"); relay_logs.append(err.name)
    relay = subprocess.Popen([sys.executable, relay_py, "--port", str(port), "--cwd", tmp, "--timeout", "20",
                              "--tls-cert", cert[0], "--tls-key", cert[1]], env=env, stdout=subprocess.DEVNULL, stderr=err)
    for _ in range(100):
        time.sleep(0.05)
        err.seek(0); text = err.read()
        if re.search(r"listening on 127\.0\.0\.1:\d+.*answering discovery", text): return
        if relay.poll() is not None: break
    raise SystemExit("FAIL: the relay did not start answering discovery: " + open(err.name).read())
def stop_relay():
    global relay
    if relay: relay.kill(); relay.wait(); relay = None

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
    def __init__(self, name, lan):
        self.log, sock = "%s/%s.uart" % (tmp, name), "%s/%s.qmp" % (tmp, name)
        self.q = subprocess.Popen(["qemu-system-aarch64", "-machine", "virt", "-cpu", "cortex-a72", "-m", "256",
                                   "-global", "virtio-mmio.force-legacy=false", *lan.netdev(),
                                   "-device", "virtio-net-device,netdev=n", "-device", "ramfb",
                                   "-device", "virtio-keyboard-device", "-display", "none", "-serial", "file:" + self.log,
                                   "-qmp", "unix:%s,server,nowait" % sock, "-kernel", os.path.join(arch, "kernel8.elf")],
                                  stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        try:   # a boot or QMP failure here must not leave the QEMU running after the check exits
            if not self.wait_for("M2 input ready", 30): raise SystemExit("FAIL: %s did not boot: %r" % (name, self.uart()[-600:]))
            time.sleep(0.5)
            self.s = socket.socket(socket.AF_UNIX); self.s.settimeout(20); self.s.connect(sock); self.f = self.s.makefile("rw")
            self.f.readline(); self.cmd("qmp_capabilities")
        except BaseException:
            self.close(); raise
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
    def ask(self, text):   # type `sam QUESTION` at the Terminal's prompt and press Enter; returns how many asks came before
        n = self.uart().count("samantha: thinking")
        for ch in "sam " + text: self.key("spc" if ch == " " else ch)
        self.key("ret")
        if not self.wait_for("samantha: thinking", 10, n + 1): raise SystemExit("FAIL: the question was not sent: %r" % self.uart()[-300:])
        return n
    def after(self, n):    # what the n-th question printed so far
        return self.uart().split("samantha: thinking\n")[n + 1] if self.uart().count("samantha: thinking") > n else ""
    def outcome(self, n, secs=40):   # waits for this question's own answer or refusal, then what it printed
        end = time.time() + secs
        while time.time() < end and ANSWER not in self.after(n) and "TLS failed, no HTTP fallback" not in self.after(n):
            time.sleep(0.1)
        time.sleep(0.5)
        return self.after(n)
    def close(self): self.q.kill(); self.q.wait()

try:
    print("arm64 relay discovery: baked host 10.0.2.77 (dead), relay on 127.0.0.1:%d" % port)
    subprocess.run(["make", "-C", arch, "clean"], check=True, capture_output=True, timeout=120)
    env = dict(base_env, CLAUDE_RELAY_HOST="10.0.2.77", CLAUDE_RELAY_PORT=str(port), CLAUDE_RELAY_TOKEN_FILE=token_file,
               CLAUDE_RELAY_TLS="1")
    r = subprocess.run(["make", "-C", arch, "TLS_TA=" + tmp + "/ca.pem"], env=env, capture_output=True, text=True, timeout=600)
    if r.returncode: print("FAIL: arch/arm64 does not build:\n" + r.stdout[-2000:] + r.stderr[-2000:]); sys.exit(1)
    check("the build bakes the dead address and a token, and never prints it",
          "relay 10.0.2.77:%d, token set" % port in r.stderr and TOKEN not in r.stdout + r.stderr, r.stderr[-300:])

    # 0. the control: something listens on the relay's TCP port, nothing answers the probe
    listener = socket.socket(); listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind(("127.0.0.1", port)); listener.listen(); listener.settimeout(0.2)
    conns, done = [], threading.Event()
    def accept():
        while not done.is_set():
            try: c, _ = listener.accept(); conns.append(1); c.close()
            except socket.timeout: continue
            except OSError: break
    th = threading.Thread(target=accept, daemon=True); th.start()   # daemon: a SystemExit before the try below must end the run, not leave this loop holding the interpreter open
    lan = Lan()
    b = Boot("discovery", lan)
    try:
        check("DHCP leased an address", "net dhcp 10.0.2.15 gw 10.0.2.2" in b.uart(), b.uart()[-300:])
        n = b.uart().count("terminal open"); b.key("f2")
        if not b.wait_for("terminal open", 10, n + 1): raise SystemExit("FAIL: F2 did not open the Terminal")
        time.sleep(0.4)
        out = b.outcome(b.ask("hi"))
        check("with no probe answer: samantha: no relay answered on the LAN, and TLS failed",
              "samantha: no relay answered on the LAN\n" in out and "TLS failed" in out, out[-300:])
        done.set(); th.join(3); listener.close()
        check("... and the dead baked address never reached the host", not conns, "%d connections" % len(conns))
        check("... though the probe did go out on the LAN as a broadcast", lan.probes > 0)

        # 1. the real relay, certificate for jt-relay: found by its probe
        start_relay(good)
        out = b.outcome(b.ask("where are you"))
        check("the probe finds the relay: samantha: relay found at 10.0.2.2", "samantha: relay found at 10.0.2.2\n" in out, out[-300:])
        check("... and the stub's answer comes back through it", ANSWER in out, out[-300:])
        calls = stub_calls()
        check("the stub got exactly the question", len(calls) == 1 and calls[0]["stdin"] == "where are you", repr(calls))

        # 2. the follow-up uses the address that answered, no probe line
        out = b.outcome(b.ask("again"))
        check("a follow-up goes straight to the relay that answered last", ANSWER in out and "relay found" not in out, out[-300:])

        # 3. same CA, the old address name: refused before the token goes out
        start_relay(wrong)
        n0 = len(stub_calls())
        out = b.outcome(b.ask("hi"))
        check("a certificate from the right CA for the wrong name is refused", "TLS failed" in out and ANSWER not in out, out[-300:])
        check("... the stub never ran", len(stub_calls()) == n0)
        log = open(relay_logs[-1]).read()
        check("... and the relay logged no request: the token never left the Pi", "POST" not in log and "/api/claude" not in log, log[-300:])
        check("the token never appears on the UART", TOKEN not in b.uart())
    finally:
        b.close(); done.set(); lan.close()
finally:
    stop_relay()
    subprocess.run(["make", "-C", arch, "clean"], capture_output=True, timeout=120)
    shutil.rmtree(tmp, ignore_errors=True)
if fails:
    print("FAIL: %d of the relay discovery checks failed" % len(fails)); sys.exit(1)
print("PASS: with a dead baked address the Pi finds the relay by a LAN probe and checks the certificate's jt-relay name, "
      "not the address; a right-CA certificate for the wrong name is refused before the token is sent")
