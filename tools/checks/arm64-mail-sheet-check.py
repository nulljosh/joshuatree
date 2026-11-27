#!/usr/bin/env python3
"""Mail's sign-in sheet on the Pi (13.25.0): sign in from the Mail window, not the Terminal.

The Mail window, with no mail and not signed in, has a Sign in button where it used to list Terminal steps. Enter asks the
kernel for its sheet (JT_SYS_MAIL, arch/arm64/mail_sheet.h), which the kernel draws, so the app password never passes
through the ring-3 app. Against a fixture IMAP server over TLS (a throwaway certificate built in with TLS_TA=, as
arm64-mail-check.py does), on QEMU's virt machine:
  1. Spotlight opens Mail; its reading pane offers Sign in, and Enter asks for the sheet, which opens and is drawn
     (the card's paper colour fills the middle of the screen);
  2. Esc closes it and nothing is signed in;
  3. asked again: the server field starts as imap.mail.me.com; it is cleared and set to the fixture, the address typed,
     a wrong app password is refused by the server and the sheet says so, still open;
  4. the right one signs in: the sheet closes, and Blackcomb opens the inbox (mail:), which ends the Mail window;
  5. neither password ever reaches the UART.
Fails when the sheet is reverted: no `mail sheet open` line. Skips (exit 0) without clang's aarch64 target, ld.lld,
openssl or qemu-system-aarch64.
Usage: tools/checks/arm64-mail-sheet-check.py   (from the repo root)
"""
import json, shutil, socket, socketserver, ssl, subprocess, sys, threading, time
from pathlib import Path

root = Path(__file__).resolve().parents[2]
arch = root / "arch/arm64"
sys.path.insert(0, str(Path(__file__).resolve().parent))
from scratch import scratch_dir
if not all(shutil.which(t) for t in ("clang", "ld.lld", "qemu-system-aarch64", "openssl")):
    print("SKIP: clang, ld.lld, openssl or qemu-system-aarch64 not installed"); sys.exit(0)
tmp = Path(scratch_dir("arm64-mail-sheet"))
cert, key_pem = str(tmp / "cert.pem"), str(tmp / "key.pem")
subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "2", "-subj", "/CN=10.0.2.2",
                "-addext", "subjectAltName=DNS:10.0.2.2", "-keyout", key_pem, "-out", cert], check=True, capture_output=True)

USER, PASS, WRONG = "joshua@example.com", "app-pass-1234", "wrong-pass-99"
HDR = b"From: Sam <sam@example.com>\r\nSubject: Lunch?\r\n\r\n"
logins = []
class Imap(socketserver.StreamRequestHandler):
    def handle(self):
        w = lambda b: self.wfile.write(b)
        w(b"* OK fixture ready\r\n")
        while True:
            line = self.rfile.readline()
            if not line: return
            tag, _, rest = line.decode(errors="replace").rstrip("\r\n").partition(" ")
            cmd = rest.upper()
            if cmd.startswith("LOGIN"):
                logins.append(rest)
                ok = rest == 'LOGIN "%s" "%s"' % (USER, PASS)
                w((tag + (" OK LOGIN done\r\n" if ok else " NO [AUTHENTICATIONFAILED] Invalid credentials\r\n")).encode())
            elif cmd.startswith("SELECT"):
                w(b"* 1 EXISTS\r\n* 0 RECENT\r\n%s OK [READ-WRITE] SELECT done\r\n" % tag.encode())
            elif cmd.startswith("FETCH"):
                w(b"* 1 FETCH (BODY[HEADER.FIELDS (FROM SUBJECT)] {%d}\r\n%s)\r\n%s OK FETCH done\r\n" % (len(HDR), HDR, tag.encode()))
            elif cmd.startswith("LOGOUT"):
                w(b"* BYE\r\n" + tag.encode() + b" OK LOGOUT done\r\n"); return
            else: w(tag.encode() + b" BAD what\r\n")
class TLSServer(socketserver.ThreadingTCPServer):
    allow_reuse_address = True; daemon_threads = True
    def get_request(self):
        s, a = super().get_request(); return ctx.wrap_socket(s, server_side=True), a
ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER); ctx.load_cert_chain(cert, key_pem)
imap = TLSServer(("127.0.0.1", 0), Imap)
port = imap.server_address[1]
threading.Thread(target=imap.serve_forever, daemon=True).start()

fails = []
def check(name, ok, detail=""):
    print(("  ok: " if ok else "  FAIL: ") + name)
    if not ok: fails.append(name + (": " + detail if detail else ""))

log, sock, shot = tmp / "uart.log", tmp / "qmp.sock", tmp / "shot.ppm"
q = None
try:
    subprocess.run(["make", "-C", str(arch), "clean"], capture_output=True, timeout=60)
    r = subprocess.run(["make", "-C", str(arch), "kernel8.elf", "TLS_TA=" + cert], capture_output=True, text=True, timeout=600)
    if r.returncode: print(r.stderr[-2000:]); print("FAIL: arch/arm64 does not build"); sys.exit(1)
    q = subprocess.Popen(["qemu-system-aarch64", "-machine", "virt", "-cpu", "cortex-a72", "-m", "256",
                          "-global", "virtio-mmio.force-legacy=false", "-device", "ramfb", "-netdev", "user,id=n", "-device", "virtio-net-device,netdev=n",
                          "-device", "virtio-keyboard-device", "-display", "none", "-serial", "file:" + str(log),
                          "-qmp", "unix:%s,server,nowait" % sock, "-kernel", str(arch / "kernel8.elf")],
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    def uart(): return log.read_text(errors="replace") if log.exists() else ""
    def wait_for(text, secs, after=0):
        end = time.time() + secs
        while time.time() < end:
            if text in uart()[after:]: return True
            time.sleep(0.1)
        return False
    if not wait_for("M2 input ready", 40): raise SystemExit("FAIL: did not boot: %r" % uart()[-600:])
    time.sleep(0.5)
    s = socket.socket(socket.AF_UNIX); s.settimeout(30); s.connect(str(sock)); f = s.makefile("rw"); f.readline()
    def command(name, args=None):
        f.write(json.dumps({"execute": name, **({"arguments": args} if args else {})}) + "\n"); f.flush()
        while True:
            reply = json.loads(f.readline())
            if "error" in reply: raise SystemExit("FAIL: QMP " + repr(reply))
            if "return" in reply: return reply["return"]
    def key(*names):
        command("send-key", {"keys": [{"type": "qcode", "data": k} for k in names], "hold-time": 50}); time.sleep(0.12)
    QCODE = {":": ("shift", "semicolon"), "/": ("slash",), ".": ("dot",), "@": ("shift", "2"), "-": ("minus",)}
    def type_text(text):
        for ch in text: key(*QCODE.get(ch, (ch,)))
    command("qmp_capabilities")
    check("DHCP leased an address", wait_for("net dhcp 10.0.2.15 gw 10.0.2.2", 30), uart()[-300:])

    n = len(uart()); key("f1"); type_text("mail"); key("ret")
    check("Spotlight opens the Mail window, with no mail", wait_for("mail: n=0", 15, n), uart()[n:][-300:])
    time.sleep(0.5)
    n = len(uart()); key("ret")
    check("Enter on the Sign in pane asks the kernel for the sheet", wait_for("mail: sign-in asked", 10, n), uart()[n:][-300:])
    check("the kernel opens its sign-in sheet", wait_for("mail sheet open", 10, n), uart()[n:][-300:])
    time.sleep(0.5)
    command("screendump", {"filename": str(shot)})
    try:
        from PIL import Image
        im = Image.open(shot).convert("RGB"); W, H = im.size
        paper = sum(1 for y in range(H // 2 - 120, H // 2 + 100, 4) for x in range(W // 2 - 150, W // 2 + 150, 4)
                    if all(abs(a - b) <= 3 for a, b in zip(im.getpixel((x, y)), (0xFA, 0xF8, 0xF4))))
        check("the sheet is drawn: the card's paper fills the middle of the screen (%d samples)" % paper, paper > 400)
    except ImportError:
        print("  skip: no Pillow for the screendump")

    n = len(uart()); key("esc")
    check("Esc closes the sheet", wait_for("mail sheet close", 10, n), uart()[n:][-300:])
    check("... and nothing tried to sign in", not logins, repr(logins))

    n = len(uart()); key("ret")
    check("asked again, the sheet opens again", wait_for("mail sheet open", 10, n), uart()[n:][-300:])
    key("tab"); key("tab")                                   # Email, then App password, then round to Server
    for _ in range(len("imap.mail.me.com")): key("backspace")
    type_text("10.0.2.2:%d" % port); key("tab")
    type_text(USER); key("ret")                              # Return moves on from the address
    n = len(uart()); type_text(WRONG); key("ret")
    check("a wrong app password is refused by the server, and the sheet says so", wait_for("mail sheet: refused", 30, n), uart()[n:][-300:])
    check("... after a LOGIN with that address", any(USER in l for l in logins), repr(logins))
    check("... and the sheet stays up (no close line)", "mail sheet close" not in uart()[n:])
    n = len(uart()); type_text(PASS); key("ret")
    check("the right one signs in", wait_for("mail sheet: signed in", 30, n), uart()[n:][-400:])
    check("the sheet closes", "mail sheet close" in uart()[n:])
    check("Blackcomb opens the inbox", wait_for("browser: load mail:", 15, n), uart()[n:][-400:])
    check("... which ends the Mail window", wait_for("M3c app mail closed for the Browser", 15, n), uart()[n:][-400:])
    check("the server saw the right login", any(l == 'LOGIN "%s" "%s"' % (USER, PASS) for l in logins), repr(logins))
    out = uart()
    check("neither password ever reached the UART", PASS not in out and WRONG not in out)
finally:
    if q:
        q.terminate()
        try: q.wait(5)
        except subprocess.TimeoutExpired: q.kill()
    imap.shutdown()
    subprocess.run(["make", "-C", str(arch), "clean"], capture_output=True, timeout=60)

if fails:
    print("FAIL:"); [print("  - " + x) for x in fails]; print("--- uart tail ---"); print(uart()[-1500:]); sys.exit(1)
print("PASS: Mail signs in from its own window: the kernel's sheet, Esc cancels, a wrong app password is refused, "
      "the right one opens the inbox in Blackcomb, and no password reaches the UART")
