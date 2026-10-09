#!/usr/bin/env python3
"""The ARM64 browser never hangs on a dead server (arch/arm64/browser.c). On the real Pi `browse URL` used to sit on
"browser: fetching" for a long time: every stage waited its own 20 s and the DNS query went to SLIRP's 10.0.2.3, which
is nobody on a real LAN. Now each stage has a short budget and prints a line when it passes.

One QEMU boot, three fetches typed at the Console's prompt row, timed from "browser: fetching" on the UART:
  1. https://10.0.2.2:SILENT/   a host socket that accepts and never answers: "dns ok", "tcp ok", then
                                "browser: tls timeout" within 12 s.
  2. http://10.0.2.2:SILENT/    the same over plain http: "browser: reply timeout" within 12 s.
  3. http://10.0.2.2:CLOSED/    nothing listening (SLIRP answers with RST): "browser: connect timeout" within 12 s.
Then `open 9` still answers "browser: no such link", so the prompt is alive after all three.
Skips (exit 0) when clang's aarch64 target, ld.lld or qemu-system-aarch64 is missing.
Usage: tools/checks/arm64-browser-deadserver-check.py   (from the repo root)
"""
import json, os, re, shutil, socket, subprocess, sys, tempfile, threading, time

root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
arch = os.path.join(root, "arch/arm64")
if not all(shutil.which(t) for t in ("clang", "ld.lld", "qemu-system-aarch64")):
    print("SKIP: clang, ld.lld or qemu-system-aarch64 not installed"); sys.exit(0)

LIMIT = 12.0   # seconds from "browser: fetching" to the error line
silent = socket.socket(); silent.bind(("127.0.0.1", 0)); silent.listen(8)
silent_port = silent.getsockname()[1]
def accept_and_hold():   # take every connection and say nothing, so the SYN-ACK comes and nothing after it
    held = []
    while True:
        try: c, _ = silent.accept(); held.append(c)
        except OSError: return
threading.Thread(target=accept_and_hold, daemon=True).start()
probe = socket.socket(); probe.bind(("127.0.0.1", 0)); closed_port = probe.getsockname()[1]; probe.close()

QCODE = {":": ("shift", "semicolon"), "/": ("slash",), ".": ("dot",), " ": ("spc",), "-": ("minus",)}
tmp = tempfile.mkdtemp()
class Boot:
    def __init__(self):
        self.log, sock = tmp + "/uart", tmp + "/qmp"
        self.q = subprocess.Popen(["qemu-system-aarch64", "-machine", "virt", "-cpu", "cortex-a72", "-m", "256",
                                   "-global", "virtio-mmio.force-legacy=false", "-netdev", "user,id=n", "-device", "virtio-net-device,netdev=n",
                                   "-device", "virtio-keyboard-device", "-display", "none", "-serial", "file:" + self.log,
                                   "-qmp", "unix:%s,server,nowait" % sock, "-kernel", os.path.join(arch, "kernel8.elf")],
                                  stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        if not self.wait_for("M2 input ready", 40): raise SystemExit("FAIL: did not boot: %r" % self.uart()[-600:])
        time.sleep(0.5)
        self.s = socket.socket(socket.AF_UNIX); self.s.settimeout(20); self.s.connect(sock); self.f = self.s.makefile("rw")
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
    def key(self, *qcodes):
        self.cmd("send-key", keys=[{"type": "qcode", "data": k} for k in qcodes]); time.sleep(0.1)
    def type(self, text):
        for ch in text: self.key(*QCODE.get(ch, (ch,)))
        self.key("ret")
    def close(self): self.q.kill(); self.q.wait()

fails = []
def check(name, ok, detail=""):
    print(("  ok: " if ok else "  FAIL: ") + name + ("" if ok else "  " + repr(detail)[:600]))
    if not ok: fails.append(name)

PROMPT_RE = re.compile(r"Claude(?: Haiku 5\.5)? \$ ")   # ask.c's prompt: the default model, or Claude alone in a build with no relay token
def fetch(b, n, url, want, lines):
    """Types `browse url`, waits for the n-th fetch to end in `want`, times it, and checks the progress lines before it."""
    b.type("browse " + url)
    b.wait_for("browser: fetching", 10, count=n); t0 = time.time()
    done = b.wait_for(want, LIMIT + 20)
    took = time.time() - t0
    out = PROMPT_RE.split(b.uart().split("browser: fetching\n")[n])[0] if done else b.uart()[-500:]
    check("%s ends in %r" % (url.split("//")[1], want), done, out)
    check("  within %.0f s (took %.1f s)" % (LIMIT, took), done and took <= LIMIT, took)
    for l in lines: check("  printed %r first" % l, done and l in out, out)

b = None
try:
    subprocess.run(["make", "-C", arch, "clean"], capture_output=True, timeout=60)
    r = subprocess.run(["make", "-C", arch, "kernel8.elf"], capture_output=True, text=True, timeout=600)
    if r.returncode: print(r.stdout[-2000:], r.stderr[-2000:]); print("FAIL: arch/arm64 does not build"); sys.exit(1)
    b = Boot()
    check("DHCP leased an address", b.wait_for("net dhcp 10.0.2.15 gw 10.0.2.2", 20), b.uart()[-300:])
    fetch(b, 1, "https://10.0.2.2:%d/" % silent_port, "browser: tls timeout", ["browser: dns ok", "browser: tcp ok"])
    fetch(b, 2, "http://10.0.2.2:%d/" % silent_port, "browser: reply timeout", ["browser: dns ok"])
    fetch(b, 3, "http://10.0.2.2:%d/" % closed_port, "browser: connect timeout", ["browser: dns ok"])
    b.type("open 9")
    check("the prompt is alive afterwards: open 9 says no such link", b.wait_for("browser: no such link", 10), b.uart()[-300:])
    check("nothing went to Claude", "claude:" not in b.uart())
finally:
    if b: b.close()
    silent.close()
    shutil.rmtree(tmp, ignore_errors=True)
    subprocess.run(["make", "-C", arch, "clean"], capture_output=True, timeout=60)
if fails: print("FAIL: " + ", ".join(fails)); sys.exit(1)
print("PASS: the ARM64 browser fails fast on a dead server: tls, reply and connect timeouts within %.0f s, prompt alive" % LIMIT)
