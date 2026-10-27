#!/usr/bin/env python3
"""The app store (6.55.0): `install NAME` fetches name.app and name.sig, checks the signature, then writes JT/APPS.

A throwaway key is built in (APP_KEY) and install points at a stub store on the host (STORE_BASE=http://10.0.2.2:PORT/apps/);
`make clean` runs after, so neither outlives the check. The stub holds, under /apps/:
  good.app, good.sig   an app made by tools/mkapp.sh, signed by the key (it prints a line, exits 0)
  tamp.app, tamp.sig   signed, then one byte of the image flipped
  nosig.app            no signature at all (its .sig is a 404)
Booted with no accounts (so install is anyone's, as any write to JT/ is), the Terminal must: install good and say so, then
`userrun good` runs it to its line; refuse tamp ("signature does not match") and nosig ("has no signature"), writing neither;
say the store has no app called missing; refuse calendar, a built-in name, without asking the store. After QEMU stops the
card holds JT/APPS/GOOD.APP and GOOD.SIG, byte for byte the stub's, and no TAMP.APP or NOSIG.APP.
Discriminating: make store.c write before it checks (drop the appsig_ok test) and TAMP.APP lands on the card.
Skips (exit 0) when clang's aarch64 target, ld.lld, qemu-system-aarch64, mtools or openssl is missing.
Usage: tools/checks/arm64-store-check.py   (from the repo root)
"""
import http.server, json, os, shutil, socket, subprocess, sys, threading, time
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from scratch import scratch_dir
from sdcard import PART, build_card

root = Path(__file__).resolve().parents[2]
arch = root / "arch/arm64"
if not all(shutil.which(t) for t in ("clang", "ld.lld", "qemu-system-aarch64", "mmd", "mtype", "openssl")):
    print("SKIP: clang, ld.lld, qemu-system-aarch64, mtools or openssl not installed"); sys.exit(0)
tmp = Path(scratch_dir("arm64-store"))
for p in tmp.iterdir(): p.unlink() if p.is_file() else shutil.rmtree(p)

fails = []
def check(name, ok, detail=""):
    print(("  ok: " if ok else "  FAIL: ") + name + ("" if ok else "  " + repr(detail)[-600:]), flush=True)
    if not ok: fails.append(name)
def run(*a):
    r = subprocess.run([str(x) for x in a], capture_output=True, text=True, timeout=600)
    if r.returncode: print(r.stdout[-1500:], r.stderr[-1500:]); print("FAIL: " + " ".join(map(str, a[:3]))); sys.exit(1)

files, seen = {}, []
class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        seen.append(self.path)
        body = files.get(self.path)
        self.send_response(200 if body is not None else 404)
        body = body if body is not None else b"not here"
        self.send_header("Content-Type", "application/octet-stream"); self.send_header("Content-Length", str(len(body))); self.end_headers(); self.wfile.write(body)
    def log_message(self, *a): pass
server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
port = server.server_address[1]
threading.Thread(target=server.serve_forever, daemon=True).start()

sign = root / "tools/signapp.sh"
run(sign, "--new-key", tmp / "key.pem", tmp / "key.h")
subprocess.run(["make", "-C", str(arch), "clean"], capture_output=True, timeout=60)
q = None
try:
    run("make", "-C", arch, "kernel8.elf", "APP_KEY=%s" % (tmp / "key.h"), "STORE_BASE=http://10.0.2.2:%d/apps/" % port)
    (tmp / "good.c").write_text('#include "jtsys.h"\n__attribute__((section(".text.start"), used))\n'
                                'void _start(void) { static const char m[] = "store: good ran\\n"; jt_write(1, m, sizeof m - 1); jt_exit(0); }\n')
    run(root / "tools/mkapp.sh", tmp / "good.c", tmp / "GOOD.APP")
    img = (tmp / "GOOD.APP").read_bytes()
    (tmp / "TAMP.APP").write_bytes(img)
    for n in ("GOOD", "TAMP"): run(sign, tmp / (n + ".APP"), tmp / "key.pem")
    t = bytearray(img); t[len(t) // 2] ^= 1
    files.update({"/apps/good.app": img, "/apps/good.sig": (tmp / "GOOD.SIG").read_bytes(),
                  "/apps/tamp.app": bytes(t), "/apps/tamp.sig": (tmp / "TAMP.SIG").read_bytes(), "/apps/nosig.app": img})

    card = str(tmp / "card.img")
    build_card(card, False)
    MT = dict(os.environ, MTOOLS_SKIP_CHECK="1")
    def mt(*a): return subprocess.run([a[0], "-i", f"{card}@@{PART * 512}", *map(str, a[1:])], capture_output=True, env=MT)
    log, sock = tmp / "uart", tmp / "qmp"
    q = subprocess.Popen(["qemu-system-aarch64", "-machine", "virt", "-cpu", "cortex-a72", "-m", "256", "-global", "virtio-mmio.force-legacy=false",
                          "-netdev", "user,id=n", "-device", "virtio-net-device,netdev=n", "-device", "virtio-keyboard-device",
                          "-drive", "if=none,id=card,file=%s,format=raw" % card, "-device", "virtio-blk-device,drive=card",
                          "-display", "none", "-serial", "file:%s" % log, "-qmp", "unix:%s,server,nowait" % sock, "-kernel", str(arch / "kernel8.elf")],
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    def uart(): return log.read_text(errors="replace") if log.exists() else ""
    def wait_for(text, secs, after=0):
        end = time.time() + secs
        while time.time() < end:
            if text in uart()[after:]: return True
            if q.poll() is not None: return False
            time.sleep(0.1)
        return False
    if not wait_for("M2 input ready", 60): raise SystemExit("FAIL: did not boot: %r" % uart()[-600:])
    time.sleep(0.5)
    s = socket.socket(socket.AF_UNIX); s.settimeout(30); s.connect(str(sock)); f = s.makefile("rw"); f.readline()
    def command(name, **a):
        f.write(json.dumps({"execute": name, "arguments": a}) + "\n"); f.flush()
        while True:
            rr = json.loads(f.readline())
            if "return" in rr or "error" in rr: return rr
    command("qmp_capabilities")
    def key(*k): command("send-key", keys=[{"type": "qcode", "data": x} for x in k]); time.sleep(0.12)
    def say(t, until, secs=30):
        n = len(uart())
        for ch in t: key({" ": "spc"}.get(ch, ch))
        key("ret"); wait_for(until, secs, n); time.sleep(0.3)
        return uart()[n:]
    check("DHCP leased an address", wait_for("net dhcp 10.0.2.15 gw 10.0.2.2", 30), uart()[-300:])
    key("f2")
    if not wait_for("terminal open", 10): raise SystemExit("FAIL: F2 did not open the Terminal")
    time.sleep(0.5)
    out = say("install good", "install: good")
    check("install good fetches, checks and writes it", "install: good, %d bytes, signed, is in JT/APPS" % len(img) in out
          and "/apps/good.app" in seen and "/apps/good.sig" in seen, out)
    out = say("userrun good", "M3c app good exit")
    check("and userrun good runs what was installed (signed)", "bytes, signed" in out and "store: good ran" in out and "M3c app good exit 0" in out, out)
    out = say("install tamp", "install: tamp")
    check("a changed image is refused, not installed", "install: tamp's signature does not match it, so it was not installed" in out, out)
    out = say("install nosig", "install: nosig")
    check("an app with no signature is refused", "install: nosig has no signature in the store, so it was not installed" in out, out)
    out = say("install missing", "install: the store")
    check("an app the store does not have is named as such", "install: the store has no app called missing" in out, out)
    n = len(seen)
    out = say("install calendar", "install: calendar")
    check("a built-in name is refused without asking the store", "install: calendar is a built-in app's name" in out and len(seen) == n, out)
    check("no kernel crash", "KERNEL CRASH" not in uart(), uart()[-600:])
    q.kill(); q.wait(); q = None
    got = {}
    for name in ("GOOD.APP", "GOOD.SIG", "TAMP.APP", "NOSIG.APP"):
        r = mt("mtype", "::JT/APPS/" + name)
        got[name] = r.stdout if r.returncode == 0 else None
    check("the card holds GOOD.APP and GOOD.SIG, byte for byte the store's", got["GOOD.APP"] == img and got["GOOD.SIG"] == files["/apps/good.sig"], [len(got["GOOD.APP"] or b"")])
    check("and neither TAMP.APP nor NOSIG.APP", got["TAMP.APP"] is None and got["NOSIG.APP"] is None)
finally:
    if q: q.kill(); q.wait()
    server.shutdown()
    subprocess.run(["make", "-C", str(arch), "clean"], capture_output=True)
if fails: print("FAIL: " + ", ".join(fails)); sys.exit(1)
print("PASS: install fetches a signed app from the store into JT/APPS and refuses a changed, unsigned, missing or built-in one")
