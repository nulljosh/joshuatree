#!/usr/bin/env python3
"""9.3.0, self-update with a fallback kernel (arch/arm64/update.c): signed or not at all, staged beside the kernel that
works, kept only once the new one has run.

A throwaway key (tools/signapp.sh --new-key) is built in as APP_KEY, and update points at a stub release on the host
(UPDATE_BASE and UPDATE_DL=http://10.0.2.2:PORT/rel/) serving version.txt (99.0.0 and a line on what is new), then
99.0.0/kernel8.img (a made-up image) and 99.0.0/kernel8.sig, the tag's own files. The card is tools/checks/sdcard.py's, with CONFIG.TXT and a KERNEL8.IMG at its root.
Boot 1 (this tree's version): a release older than it is never offered, nor one whose kernel8.sig is not up yet; `update` says 99.0.0 is out and opens Software Update, whose Update and Restart button is
drawn; with a signature over other bytes Return refuses and writes nothing; with the right one it stages KERNEL8.NEW
(the image, byte for byte), TRYBOOT.TXT (CONFIG.TXT, then kernel=kernel8.new) and JT/UPDATE.VER, then restarts.
Boot 2 (the same kernel: as if 99.0.0 never started): it drops the staged files and says it kept its own version.
Boot 3a (built as 99.0.0, a short KERNEL8.NEW staged): the second signature check refuses it (it retries a minute later) and
KERNEL8.IMG stays.
Boot 3 (built as 99.0.0, the files staged again): it says it started from KERNEL8.NEW, and after running 20 s KERNEL8.IMG
is the new image and the staged files are gone.
The firmware's tryboot itself (mailbox tag 0x00038064, then TRYBOOT.TXT for one boot) needs the board.
Discriminating: make appsig_ok return 1 and the bad signature stages (and boot 3a commits the short image); drop `newer` and 9.0.0 is offered; take update_boot out of boot.c and boot 2 keeps the
files and boot 3 never commits.
Skips (exit 0) without clang, ld.lld, qemu-system-aarch64, mtools or openssl.
Usage: tools/checks/arm64-update-check.py   (from the repo root)
"""
import http.server, json, os, shutil, socket, subprocess, sys, threading, time
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from scratch import scratch_dir
from sdcard import PART, build_card

root = Path(__file__).resolve().parents[2]
arch = root / "arch/arm64"
if not all(shutil.which(t) for t in ("clang", "ld.lld", "qemu-system-aarch64", "mtype", "mcopy", "openssl")):
    print("SKIP: clang, ld.lld, qemu-system-aarch64, mtools or openssl not installed"); sys.exit(0)
tmp = Path(scratch_dir("arm64-update"))
fails = []
def check(name, ok, detail=""):
    print(("  ok: " if ok else "  FAIL: ") + name + ("" if ok else "  " + repr(detail)[-600:]), flush=True)
    if not ok: fails.append(name)
def run(*a):
    r = subprocess.run([str(x) for x in a], capture_output=True, text=True, timeout=900)
    if r.returncode: print(r.stdout[-1500:], r.stderr[-1500:]); print("FAIL: " + " ".join(map(str, a[:3]))); sys.exit(1)

files = {}
class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        body = files.get(self.path)
        self.send_response(200 if body is not None else 404)
        body = body if body is not None else b"not here"
        self.send_header("Content-Type", "application/octet-stream"); self.send_header("Content-Length", str(len(body))); self.end_headers(); self.wfile.write(body)
    def log_message(self, *a): pass
server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
port = server.server_address[1]
threading.Thread(target=server.serve_forever, daemon=True).start()

run(root / "tools/signapp.sh", "--new-key", tmp / "key.pem", tmp / "key.h")
NEW = bytes((i * 7 + 3) & 0xFF for i in range(300000)) + b"kernel 99.0.0"
def sign(data):
    (tmp / "msg").write_bytes(b"JTAPP1\0os.image\0" + data)
    run("openssl", "dgst", "-sha256", "-sign", tmp / "key.pem", "-out", tmp / "sig", tmp / "msg")
    return (tmp / "sig").read_bytes()
GOOD, BAD = sign(NEW), sign(NEW[:-1] + b"!")
files.update({"/rel/version.txt": b"99.0.0\nA screen reader, twelve languages and safer updates.\n", "/rel/99.0.0/kernel8.img": NEW})

def build(*extra):
    subprocess.run(["make", "-C", str(arch), "clean"], capture_output=True, timeout=60)
    run("make", "-C", arch, "kernel8.elf", "APP_KEY=%s" % (tmp / "key.h"), "UPDATE_BASE=http://10.0.2.2:%d/rel/" % port, "UPDATE_DL=http://10.0.2.2:%d/rel/" % port, *extra)
    shutil.copy(arch / "kernel8.elf", tmp / ("kernel%s.elf" % (extra[0].split("=")[1] if extra else "")))
    return tmp / ("kernel%s.elf" % (extra[0].split("=")[1] if extra else ""))

card = str(tmp / "card.img")
build_card(card, False)
MT = dict(os.environ, MTOOLS_SKIP_CHECK="1")
def mt(*a): return subprocess.run([a[0], "-i", f"{card}@@{PART * 512}", *map(str, a[1:])], capture_output=True, env=MT)
mt("mmd", "::JT")
config = mt("mtype", "::CONFIG.TXT").stdout
old_kernel = mt("mtype", "::KERNEL8.IMG").stdout

class Boot:
    def __init__(self, elf, tag):
        self.log, self.sock = tmp / (tag + ".uart"), tmp / (tag + ".qmp")
        self.q = subprocess.Popen(["qemu-system-aarch64", "-machine", "virt", "-cpu", "cortex-a72", "-m", "256", "-global", "virtio-mmio.force-legacy=false",
                                   "-netdev", "user,id=n", "-device", "virtio-net-device,netdev=n", "-device", "ramfb", "-device", "virtio-keyboard-device",
                                   "-drive", "if=none,id=card,file=%s,format=raw" % card, "-device", "virtio-blk-device,drive=card",
                                   "-display", "none", "-serial", "file:%s" % self.log, "-qmp", "unix:%s,server,nowait" % self.sock, "-kernel", str(elf)],
                                  stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.f = None
    def uart(self): return self.log.read_text(errors="replace") if self.log.exists() else ""
    def wait_for(self, text, secs, after=0):
        end = time.time() + secs
        while time.time() < end:
            if text in self.uart()[after:]: return True
            if self.q.poll() is not None: return False
            time.sleep(0.1)
        return False
    def connect(self):
        s = socket.socket(socket.AF_UNIX); s.settimeout(30); s.connect(str(self.sock)); self.f = s.makefile("rw"); self.f.readline(); self.command("qmp_capabilities")
    def command(self, name, **a):
        self.f.write(json.dumps({"execute": name, "arguments": a}) + "\n"); self.f.flush()
        while True:
            rr = json.loads(self.f.readline())
            if "return" in rr or "error" in rr: return rr
    def key(self, *k): self.command("send-key", keys=[{"type": "qcode", "data": x} for x in k]); time.sleep(0.12)
    def say(self, t, until, secs=30):
        n = len(self.uart())
        for ch in t: self.key({" ": "spc"}.get(ch, ch))
        self.key("ret"); self.wait_for(until, secs, n); time.sleep(0.3)
        return self.uart()[n:]
    def shot(self):
        p = tmp / "shot.ppm"; self.command("screendump", filename=str(p)); time.sleep(0.3)
        d = p.read_bytes().split(b"\n", 3)[3]
        return lambda x, y: tuple(d[(y * 800 + x) * 3:(y * 800 + x) * 3 + 3])
    def stop(self):
        if self.q.poll() is None: self.q.kill()
        self.q.wait()

try:
    k1 = build()
    B = Boot(k1, "boot1")
    try:
        if not B.wait_for("M2 input ready", 60): raise SystemExit("FAIL: did not boot: %r" % B.uart()[-600:])
        B.wait_for("net dhcp 10.0.2.15", 30); time.sleep(0.5); B.connect()
        B.key("f2"); B.wait_for("terminal open", 10); time.sleep(0.5)
        files["/rel/version.txt"] = b"9.0.0\nAn old release.\n"
        out = B.say("update", "update: ")
        check("an older release (9.0.0) is never offered", "older than this; nothing to do" in out and "software update" not in out, out)
        files["/rel/version.txt"] = b"99.0.0\nA screen reader, twelve languages and safer updates.\n"
        out = B.say("update", "update: ")
        check("a release whose kernel8.sig is not published yet is not offered", "is not signed yet" in out and "software update" not in out, out)
        files["/rel/99.0.0/kernel8.sig"] = BAD
        out = B.say("update", "software update: ")
        check("update says 99.0.0 is out and opens Software Update", "update: 99.0.0 is out" in out and "software update: 99.0.0" in out, out)
        time.sleep(0.5); px = B.shot()
        terra = sum(1 for y in range(300) for x in range(250, 550) if px(x, y + 150) == (0xb5, 0x50, 0x2c))
        check("its Update and Restart button is drawn (%d terracotta pixels)" % terra, terra > 3000, terra)
        grey = sum(1 for y in range(300) for x in range(150, 650) if px(x, y + 150) == (0xe6, 0xe1, 0xd9))
        check("and a Cancel beside it (%d grey pixels)" % grey, grey > 1500, grey)
        n = len(B.uart()); B.key("ret")
        check("with a signature over other bytes, Return refuses", B.wait_for("update: refused", 20, n), B.uart()[n:][-300:])
        check("and nothing is staged", b"KERNEL8" not in mt("mdir", "-b", "::/").stdout.upper().replace(b"KERNEL8.IMG", b""), mt("mdir", "-b", "::/").stdout)
        files["/rel/99.0.0/kernel8.sig"] = GOOD
        B.say("update", "software update: ")
        n = len(B.uart()); B.key("ret")
        check("with the right signature it stages and restarts", B.wait_for("update: restarting into 99.0.0 (tryboot)", 30, n) and B.wait_for("power: restart", 10, n), B.uart()[n:][-300:])
    finally:
        B.stop()
    check("KERNEL8.NEW is the image, byte for byte", mt("mtype", "::KERNEL8.NEW").stdout == NEW)
    check("KERNEL8.SIG is its signature, kept for the second check", mt("mtype", "::KERNEL8.SIG").stdout == GOOD)
    tb = mt("mtype", "::TRYBOOT.TXT").stdout
    check("TRYBOOT.TXT is CONFIG.TXT, then kernel=kernel8.new", tb.startswith(config) and tb.endswith(b"\n[all]\nkernel=kernel8.new\n"), tb[-80:])
    check("JT/UPDATE.VER names 99.0.0", mt("mtype", "::JT/UPDATE.VER").stdout == b"99.0.0")
    check("KERNEL8.IMG is still the old one", mt("mtype", "::KERNEL8.IMG").stdout == old_kernel)

    B = Boot(k1, "boot2")
    try:
        check("boot 2, the old kernel: 99.0.0 did not start, this version is kept", B.wait_for("update: 99.0.0 did not start", 60), B.uart()[-400:])
        time.sleep(1.0)
    finally:
        B.stop()
    left = mt("mdir", "-b", "::/").stdout.upper()
    check("and the staged files are gone", b"KERNEL8.NEW" not in left and b"KERNEL8.SIG" not in left and b"TRYBOOT" not in left and mt("mtype", "::JT/UPDATE.VER").returncode != 0, left)

    def stage(img):
        (tmp / "NEW").write_bytes(img); (tmp / "SIG").write_bytes(GOOD); (tmp / "VER").write_bytes(b"99.0.0")
        mt("mcopy", "-o", tmp / "NEW", "::KERNEL8.NEW"); mt("mcopy", "-o", tmp / "SIG", "::KERNEL8.SIG"); mt("mcopy", "-o", tmp / "VER", "::JT/UPDATE.VER")
    k3 = build("JT_VERSION=99.0.0")
    stage(NEW[:200000])   # a short KERNEL8.NEW, as a bad read or a torn copy would leave it
    B = Boot(k3, "boot3short")
    try:
        check("boot 3a, a short KERNEL8.NEW: it is checked against KERNEL8.SIG again and not kept", B.wait_for("did not read back whole", 45), B.uart()[-400:])
        time.sleep(1.0)
    finally:
        B.stop()
    check("and KERNEL8.IMG is still the old one", mt("mtype", "::KERNEL8.IMG").stdout == old_kernel)
    stage(NEW)
    B = Boot(k3, "boot3")
    try:
        check("boot 3, built as 99.0.0: it started from KERNEL8.NEW", B.wait_for("update: 99.0.0 started from KERNEL8.NEW", 60), B.uart()[-400:])
        check("and after 20 s it commits", B.wait_for("update: committed, KERNEL8.IMG is 99.0.0", 45), B.uart()[-400:])
        time.sleep(1.0)
    finally:
        B.stop()
    check("KERNEL8.IMG is now the new image", mt("mtype", "::KERNEL8.IMG").stdout == NEW)
    left = mt("mdir", "-b", "::/").stdout.upper()
    check("and the staged files are gone", b"KERNEL8.NEW" not in left and b"KERNEL8.SIG" not in left and mt("mtype", "::JT/UPDATE.VER").returncode != 0, left)
finally:
    subprocess.run(["make", "-C", str(arch), "clean"], capture_output=True, timeout=60)
if fails: print("FAIL: %d of the update checks" % len(fails)); sys.exit(1)
print("PASS: a signed update is staged beside the working kernel, a bad signature writes nothing, an update that never started is dropped, and one that ran is kept")
