#!/usr/bin/env python3
"""Download-to-install (13.4.0): Blackcomb follows a link to NAME.app, fetches NAME.sig beside it, and installs the app into
JT/APPS only when the signature checks out. Anything else downloads as before.

A throwaway key is built in (APP_KEY); `make clean` runs after, so it never outlives the check. A stub on the host serves:
  /dl/good.app, good.sig   an app made by tools/mkapp.sh, signed by the key (it prints a line, exits 0)
  /dl/tamp.app, tamp.sig   signed, then one byte of the image flipped
  /dl/nosig.app            no signature (its .sig is a 404)
  /dl/notes.bin            plain bytes, not an app
Booted with no accounts (installing is anyone's, as any write to JT/ is), the Terminal must: `browse .../good.app` says
installed and `userrun good` runs it; tamp and nosig are refused and `userrun` finds neither; notes.bin still saves to
JT/DOWNLOAD. Boot 2 puts accounts on the card and signs in as guest, no admin: the same link is refused before its .sig is
asked for, and `userrun good` finds nothing.
Discriminating: drop the store_from_page call in browser.c and good.app saves to DOWNLOAD, so userrun good fails; drop its
appsig_ok test and tamp installs; drop its admin test and guest installs good.
Usage: tools/checks/arm64-browser-install-check.py   (from the repo root)
"""
import hashlib, http.server, json, os, shutil, socket, subprocess, sys, threading, time
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from scratch import scratch_dir
from sdcard import PART, build_card

root = Path(__file__).resolve().parents[2]
arch = root / "arch/arm64"
if not all(shutil.which(t) for t in ("clang", "ld.lld", "qemu-system-aarch64", "mmd", "mcopy", "openssl")):
    print("SKIP: clang, ld.lld, qemu-system-aarch64 mtools or openssl not installed"); sys.exit(0)
tmp = Path(scratch_dir("arm64-browser-install"))
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
try:
    run("make", "-C", arch, "kernel8.elf", "APP_KEY=%s" % (tmp / "key.h"))
    (tmp / "good.c").write_text('#include "jtsys.h"\n__attribute__((section(".text.start"), used))\n'
                                'void _start(void) { static const char m[] = "store: good ran\\n"; jt_write(1, m, sizeof m - 1); jt_exit(0); }\n')
    run(root / "tools/mkapp.sh", tmp / "good.c", tmp / "GOOD.APP")
    img = (tmp / "GOOD.APP").read_bytes()
    (tmp / "TAMP.APP").write_bytes(img)
    for n in ("GOOD", "TAMP"): run(sign, tmp / (n + ".APP"), tmp / "key.pem")
    t = bytearray(img); t[len(t) // 2] ^= 1
    files.update({"/dl/good.app": img, "/dl/good.sig": (tmp / "GOOD.SIG").read_bytes(), "/dl/tamp.app": bytes(t),
                  "/dl/tamp.sig": (tmp / "TAMP.SIG").read_bytes(), "/dl/nosig.app": img, "/dl/notes.bin": b"just some bytes\n" * 8})

    st = {}
    def boot(card, tag):
        log, sock = tmp / ("uart" + tag), tmp / ("qmp" + tag)
        st["q"] = subprocess.Popen(["qemu-system-aarch64", "-machine", "virt", "-cpu", "cortex-a72", "-m", "256", "-global", "virtio-mmio.force-legacy=false",
                              "-device", "ramfb", "-netdev", "user,id=n", "-device", "virtio-net-device,netdev=n", "-device", "virtio-keyboard-device",
                              "-drive", "if=none,id=card,file=%s,format=raw" % card, "-device", "virtio-blk-device,drive=card",
                              "-display", "none", "-serial", "file:%s" % log, "-qmp", "unix:%s,server,nowait" % sock, "-kernel", str(arch / "kernel8.elf")],
                             stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        st["log"], st["sock"] = log, sock
    def uart(): return st["log"].read_text(errors="replace") if st["log"].exists() else ""
    def wait_for(text, secs, after=0):
        end = time.time() + secs
        while time.time() < end:
            if text in uart()[after:]: return True
            if st["q"].poll() is not None: return False
            time.sleep(0.1)
        return False
    def qmp():
        for _ in range(100):
            if st["sock"].exists(): break
            time.sleep(0.1)
        s = socket.socket(socket.AF_UNIX); s.settimeout(60); s.connect(str(st["sock"])); st["f"] = s.makefile("rw"); st["f"].readline()
        command("qmp_capabilities")
    def command(name, **a):
        f = st["f"]; f.write(json.dumps({"execute": name, "arguments": a}) + "\n"); f.flush()
        while True:
            rr = json.loads(f.readline())
            if "return" in rr or "error" in rr: return rr
    def key(*k): command("send-key", keys=[{"type": "qcode", "data": x} for x in k]); time.sleep(0.12)
    def typ(t):
        for ch in t:
            if ch == ":": key("shift", "semicolon")
            else: key({" ": "spc", ".": "dot", "/": "slash"}.get(ch, ch))
    def say(t, until, secs=30):
        n = len(uart()); typ(t); key("ret"); wait_for(until, secs, n); time.sleep(0.3)
        return uart()[n:]
    def stop():
        if st.get("q"): st["q"].kill(); st["q"].wait(); st["q"] = None

    card = str(tmp / "card.img")
    build_card(card, False)
    boot(card, "")
    if not wait_for("M2 input ready", 60): raise SystemExit("FAIL: did not boot: %r" % uart()[-600:])
    time.sleep(0.5); qmp()
    check("DHCP leased an address", wait_for("net dhcp 10.0.2.15 gw 10.0.2.2", 30), uart()[-300:])
    key("f2")
    if not wait_for("terminal open", 10): raise SystemExit("FAIL: F2 did not open the Terminal")
    time.sleep(0.5)
    base = "browse http://10.0.2.2:%d/dl/" % port
    out = say(base + "good.app", "browser: installed good")
    check("following good.app fetches its signature and installs it", "browser: installed good, %d bytes, signed, in JT/APPS" % len(img) in out
          and "/dl/good.sig" in seen and "browser: saved" not in out, out)
    out = say("userrun good", "M3c app good exit")
    check("and userrun good runs it", "store: good ran" in out and "M3c app good exit 0" in out, out)
    out = say(base + "tamp.app", "not installed")
    check("a changed image is refused", "browser: tamp's signature does not match it, not installed" in out and "browser: saved" not in out, out)
    out = say(base + "nosig.app", "not installed")
    check("an app with no signature is refused", "browser: nosig has no signature beside it, not installed" in out and "browser: saved" not in out, out)
    for name in ("tamp", "nosig"):
        out = say("userrun " + name, "joshua@tree", 10)
        check("and userrun %s finds nothing" % name, "app %s exit 0" % name not in out and "store: good ran" not in out, out)
    out = say(base + "notes.bin", "browser: saved")
    check("a file that is not an app still downloads", "browser: saved NOTES.BIN, 128 bytes in JT/DOWNLOAD" in out, out)
    check("no kernel crash", "KERNEL CRASH" not in uart(), uart()[-600:])
    stop()

    # Boot 2: the card has accounts and guest, who is no admin, signs in. The link is refused before its .sig is asked for.
    card2 = str(tmp / "card2.img")
    build_card(card2, False)
    def record(name, pw, salt_byte):
        salt = bytes(range(salt_byte, salt_byte + 16))
        return "%s:p2:1000:%s:%s\n" % (name, salt.hex(), hashlib.pbkdf2_hmac("sha256", pw.encode(), salt, 1000, 32).hex())
    (tmp / "USERS.TXT").write_text(record("joshua", "treepass1", 1) + record("guest", "guestpw22", 40))
    (tmp / "SUDOERS.TXT").write_text("joshua\n")
    MT = dict(os.environ, MTOOLS_SKIP_CHECK="1")
    def mt(*a): return subprocess.run([a[0], "-i", f"{card2}@@{PART * 512}", *map(str, a[1:])], capture_output=True, env=MT)
    if mt("mmd", "::JT").returncode: raise SystemExit("FAIL: mtools could not make JT")
    for name in ("USERS.TXT", "SUDOERS.TXT"): mt("mcopy", "-o", tmp / name, "::JT/" + name)
    boot(card2, "2")
    if not wait_for("login: screen up", 90): raise SystemExit("FAIL: no login screen: %r" % uart()[-600:])
    qmp(); time.sleep(0.5)
    n = len(uart()); typ("guest"); key("ret"); typ("guestpw22"); key("ret")
    if not wait_for("login: desktop up", 90, n): raise SystemExit("FAIL: guest did not sign in: %r" % uart()[n:][-600:])
    wait_for("net dhcp", 30); time.sleep(0.5); key("f2")
    if not wait_for("terminal open", 10): raise SystemExit("FAIL: F2 did not open the Terminal")
    time.sleep(0.5)
    sigs = seen.count("/dl/good.sig")
    out = say(base + "good.app", "not installed")
    check("guest, no admin: following good.app is refused", "browser: installing an app is an admin's, not installed" in out
          and seen.count("/dl/good.sig") == sigs, out)
    out = say("userrun good", "joshua@tree", 10)
    check("and userrun good finds nothing", "store: good ran" not in out, out)
    check("no kernel crash with accounts", "KERNEL CRASH" not in uart(), uart()[-600:])
finally:
    stop()
    server.shutdown()
    subprocess.run(["make", "-C", str(arch), "clean"], capture_output=True)
if fails: print("FAIL: " + ", ".join(fails)); sys.exit(1)
print("PASS: Blackcomb installs a signed app it follows a link to, refuses a changed or unsigned one, and still downloads other files")
