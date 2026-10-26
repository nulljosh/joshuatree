#!/usr/bin/env python3
"""Mail over IMAP and TLS on the Pi (6.18.0), against a fixture IMAP server on the host.

The fixture speaks IMAP over TLS with a throwaway certificate for 10.0.2.2, built into the kernel's anchors with TLS_TA=.
Its inbox holds three messages: plain text whose body carries a line "<this command's tag> OK fake ending" (a literal must not end a
reply early), multipart with a quoted-printable HTML part holding a heading and a remote image, and base64 plain text.
At the Terminal:
  `mail` before a login says so; `mail login 10.0.2.2:PORT joshua@example.com` asks for the app password with stars;
  a wrong one is refused by the server and forgotten, the right one signs in; the password never reaches the UART;
  `mail` lists the three newest first with sender names and subjects; `mail 1` shows the plain message whole, past
  its fake ending; `mail 2` shows the HTML part's heading and its quoted-printable "!" and never fetches the image;
  `mail 3` decodes base64; `mail logout` forgets it all.
Discriminating: drop the literal skip in mail.c's imap() and `mail 1` stops at the fake ending.
Skips (exit 0) when clang's aarch64 target, ld.lld, openssl or qemu-system-aarch64 is missing.
Usage: tools/checks/arm64-mail-check.py   (from the repo root)
"""
import base64, http.server, json, shutil, socket, socketserver, ssl, subprocess, sys, threading, time
from pathlib import Path

root = Path(__file__).resolve().parents[2]
arch = root / "arch/arm64"
sys.path.insert(0, str(Path(__file__).resolve().parent))
from scratch import scratch_dir
if not all(shutil.which(t) for t in ("clang", "ld.lld", "qemu-system-aarch64", "openssl")):
    print("SKIP: clang, ld.lld, openssl or qemu-system-aarch64 not installed"); sys.exit(0)
tmp = Path(scratch_dir("arm64-mail"))
cert, key = str(tmp / "cert.pem"), str(tmp / "key.pem")
subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "2", "-subj", "/CN=10.0.2.2",
                "-addext", "subjectAltName=DNS:10.0.2.2", "-keyout", key, "-out", cert], check=True, capture_output=True)

USER, PASS = "joshua@example.com", "app-pass-1234"
tracked = []
class Tracker(http.server.BaseHTTPRequestHandler):
    def do_GET(self): tracked.append(self.path); self.send_response(204); self.end_headers()
    def log_message(self, *a): pass
web = http.server.HTTPServer(("127.0.0.1", 0), Tracker)
threading.Thread(target=web.serve_forever, daemon=True).start()

MSGS = [   # sequence 1, 2, 3: oldest first
    (b"From: Base Sixty <b64@example.com>\r\nSubject: Encoded\r\nContent-Type: text/plain\r\nContent-Transfer-Encoding: base64\r\n\r\n",
     base64.encodebytes(b"Hello from base64, decoded on the Pi.\n")),
    (b"From: \"News Desk\" <news@example.com>\r\nSubject: Big news\r\nContent-Type: multipart/alternative; boundary=\"zz\"\r\n\r\n",
     b"--zz\r\nContent-Type: text/plain\r\n\r\nplain version\r\n--zz\r\nContent-Type: text/html\r\nContent-Transfer-Encoding: quoted-printable\r\n\r\n"
     b"<h1>Big news</h1><p>The Pi reads mail=21</p><img src=3D\"http://10.0.2.2:%d/track.png\">\r\n--zz--\r\n" % web.server_address[1]),
    (b"From: Sam <sam@example.com>\r\nSubject: Lunch?\r\nContent-Type: text/plain\r\n\r\n",
     b"Lunch at noon?\r\nTAG OK fake ending\r\nThe real last line.\r\n"),
]
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
                w(b"* %d EXISTS\r\n* 0 RECENT\r\n%s OK [READ-WRITE] SELECT done\r\n" % (len(MSGS), tag.encode()))
            elif cmd.startswith("FETCH") and "HEADER.FIELDS" in cmd:
                lo, hi = (int(x) for x in rest.split()[1].split(":"))
                for k in range(lo, hi + 1):
                    h = b"\r\n".join(l for l in MSGS[k - 1][0].split(b"\r\n") if l.startswith((b"From:", b"Subject:"))) + b"\r\n\r\n"
                    w(b"* %d FETCH (BODY[HEADER.FIELDS (FROM SUBJECT)] {%d}\r\n%s)\r\n" % (k, len(h), h))
                w(tag.encode() + b" OK FETCH done\r\n")
            elif cmd.startswith("FETCH"):
                k = int(rest.split()[1]); h, t = MSGS[k - 1]
                t = t.replace(b"TAG", tag.encode())   # this command's own tag: only skipping the literal keeps it from ending the reply
                out = b"* %d FETCH (BODY[HEADER] {%d}\r\n%s BODY[TEXT] {%d}\r\n%s)\r\n%s OK FETCH done\r\n" % (k, len(h), h, len(t), t, tag.encode())
                cut = out.find(b"fake ending\r\n")
                if cut >= 0:   # sent in two pieces: a reader that took the fake line for the end has the rest still to come
                    w(out[:cut + 13]); self.wfile.flush(); time.sleep(0.5); out = out[cut + 13:]
                w(out)
            elif cmd.startswith("LOGOUT"):
                w(b"* BYE\r\n" + tag.encode() + b" OK LOGOUT done\r\n"); return
            else: w(tag.encode() + b" BAD what\r\n")
class TLSServer(socketserver.ThreadingTCPServer):
    allow_reuse_address = True; daemon_threads = True
    def get_request(self):
        s, a = super().get_request(); return ctx.wrap_socket(s, server_side=True), a
ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER); ctx.load_cert_chain(cert, key)
imap = TLSServer(("127.0.0.1", 0), Imap)
port = imap.server_address[1]
threading.Thread(target=imap.serve_forever, daemon=True).start()

fails = []
def check(name, ok, detail=""):
    print(("  ok: " if ok else "  FAIL: ") + name + ("" if ok else "  " + repr(detail)[-500:]))
    if not ok: fails.append(name)

log, sock = tmp / "uart", tmp / "qmp"
q = None
try:
    subprocess.run(["make", "-C", str(arch), "clean"], capture_output=True, timeout=60)
    r = subprocess.run(["make", "-C", str(arch), "kernel8.elf", "TLS_TA=" + cert], capture_output=True, text=True, timeout=600)
    if r.returncode: print(r.stderr[-2000:]); print("FAIL: arch/arm64 does not build"); sys.exit(1)
    q = subprocess.Popen(["qemu-system-aarch64", "-machine", "virt", "-cpu", "cortex-a72", "-m", "256",
                          "-global", "virtio-mmio.force-legacy=false", "-netdev", "user,id=n", "-device", "virtio-net-device,netdev=n",
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
    QCODE = {":": ("shift", "semicolon"), "/": ("slash",), ".": ("dot",), " ": ("spc",), "@": ("shift", "2"), "-": ("minus",)}
    def type_text(text):
        for ch in text: key(*QCODE.get(ch, (ch,)))
    def line(text): type_text(text); key("ret")
    command("qmp_capabilities")
    check("DHCP leased an address", wait_for("net dhcp 10.0.2.15 gw 10.0.2.2", 30), uart()[-300:])
    key("f2")
    check("the Terminal opened", wait_for("terminal open", 10), uart()[-300:])

    n = len(uart()); line("mail")
    check("mail before a login says so", wait_for("mail: not signed in", 10, n), uart()[n:][-300:])
    n = len(uart()); line("mail login 10.0.2.2:%d %s" % (port, USER)); time.sleep(0.5)
    line("wrong-pass-99")
    check("mail login asks for the app password (the prompt alone is echoed)", wait_for("App password for %s: \n" % USER, 10, n), uart()[n:][-300:])
    check("a wrong password is refused by the server", wait_for("mail: the server refused the user or the app password", 30, n), uart()[n:][-300:])
    n = len(uart()); line("mail")
    check("and forgotten", wait_for("mail: not signed in", 10, n), uart()[n:][-300:])
    n = len(uart()); line("mail login 10.0.2.2:%d %s" % (port, USER)); time.sleep(0.5); line(PASS)
    check("the right one signs in", wait_for("mail: signed in as %s at 10.0.2.2" % USER, 30, n), uart()[n:][-300:])
    check("the password never reaches the UART", PASS not in uart() and "wrong-pass-99" not in uart())
    check("and went to the server quoted, over TLS", logins[-1:] == ['LOGIN "%s" "%s"' % (USER, PASS)], logins)

    n = len(uart()); line("mail")
    check("mail lists the three, newest first", wait_for("mail N opens one", 30, n) and "1  Sam: Lunch?" in uart()[n:] and "2  News Desk: Big news" in uart()[n:] and "3  Base Sixty: Encoded" in uart()[n:], uart()[n:][-400:])
    n = len(uart()); line("mail 1")
    check("a literal holding a fake ending does not end the reply", wait_for("The real last line.", 30, n) and "OK fake ending" in uart()[n:], uart()[n:][-400:])
    n = len(uart()); line("mail 2")
    check("the HTML part shows, quoted-printable decoded", wait_for("Big news", 30, n) and wait_for("The Pi reads mail!", 10, n) and "plain version" not in uart()[n:], uart()[n:][-400:])
    time.sleep(2)
    check("its remote image is never fetched", tracked == [], tracked)
    n = len(uart()); line("mail 3")
    check("base64 is decoded", wait_for("Hello from base64, decoded on the Pi.", 30, n), uart()[n:][-400:])
    n = len(uart()); line("mail logout")
    check("mail logout forgets it", wait_for("mail: signed out", 10, n), uart()[n:][-300:])
    n = len(uart()); line("mail")
    check("so mail is not signed in again", wait_for("mail: not signed in", 10, n), uart()[n:][-300:])
finally:
    if q: q.kill(); q.wait()
    imap.shutdown(); web.shutdown()
    subprocess.run(["make", "-C", str(arch), "clean"], capture_output=True, timeout=60)
if fails: print("FAIL: " + ", ".join(fails)); sys.exit(1)
print("PASS: the Pi signs in to IMAP over TLS with a hidden app password, lists the newest mail, shows plain, quoted-printable HTML and base64 messages whole, never fetches a mail's image, and forgets the password on logout")
