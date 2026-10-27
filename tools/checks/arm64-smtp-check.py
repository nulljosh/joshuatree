#!/usr/bin/env python3
"""Sending mail over SMTP submission on the Pi (6.20.0), against fixture servers on the host.

Fixtures, all for 10.0.2.2 with one throwaway certificate built into the kernel's anchors (TLS_TA=):
  IMAP over TLS (mail login checks the app password there; its inbox has one message, "Sam <sam@example.com>: Lunch?");
  SMTP on a plain port that offers STARTTLS, upgrades, and only then takes AUTH PLAIN, MAIL, RCPT and DATA;
  SMTP on another plain port that never offers STARTTLS.
At the Terminal, after `mail login`:
  through the second server, `mail send` is refused before any AUTH ("offers no STARTTLS"): the password never goes out
  in the clear; through the first, `mail send sam@example.com Lunch plans | .See you at noon.` arrives: AUTH after TLS
  with exactly \\0user\\0password, RCPT sam@example.com, "Subject: Lunch plans", the body's leading dot doubled on the
  wire; `mail reply 1 Sounds good` goes to sam@example.com with "Subject: Re: Lunch?"; a second address after the first
  (a,b) is refused.
Discriminating: drop the STARTTLS refusal in mail.c's smtp_send and the second server sees AUTH in the clear.
Skips (exit 0) when clang's aarch64 target, ld.lld, openssl or qemu-system-aarch64 is missing.
Usage: tools/checks/arm64-smtp-check.py   (from the repo root)
"""
import base64, json, shutil, socket, socketserver, ssl, subprocess, sys, threading, time
from pathlib import Path

root = Path(__file__).resolve().parents[2]
arch = root / "arch/arm64"
sys.path.insert(0, str(Path(__file__).resolve().parent))
from scratch import scratch_dir
if not all(shutil.which(t) for t in ("clang", "ld.lld", "qemu-system-aarch64", "openssl")):
    print("SKIP: clang, ld.lld, openssl or qemu-system-aarch64 not installed"); sys.exit(0)
tmp = Path(scratch_dir("arm64-smtp"))
cert, key = str(tmp / "cert.pem"), str(tmp / "key.pem")
subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "2", "-subj", "/CN=10.0.2.2",
                "-addext", "subjectAltName=DNS:10.0.2.2", "-keyout", key, "-out", cert], check=True, capture_output=True)
ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER); ctx.load_cert_chain(cert, key)
USER, PASS = "joshua@example.com", "app-pass-1234"

HDR = b"From: Sam <sam@example.com>\r\nSubject: Lunch?\r\n\r\n"
class Imap(socketserver.StreamRequestHandler):
    def handle(self):
        w = self.wfile.write
        w(b"* OK ready\r\n")
        while (line := self.rfile.readline()):
            tag, _, rest = line.decode().rstrip("\r\n").partition(" ")
            if rest.startswith("LOGIN"): w((tag + (" OK done\r\n" if rest == 'LOGIN "%s" "%s"' % (USER, PASS) else " NO bad\r\n")).encode())
            elif rest.startswith("SELECT"): w(b"* 1 EXISTS\r\n" + tag.encode() + b" OK done\r\n")
            elif rest.startswith("FETCH"): w(b"* 1 FETCH (BODY[HEADER.FIELDS (FROM SUBJECT)] {%d}\r\n%s)\r\n%s OK done\r\n" % (len(HDR), HDR, tag.encode()))
            elif rest.startswith("LOGOUT"): w(b"* BYE\r\n" + tag.encode() + b" OK done\r\n"); return
            else: w(tag.encode() + b" BAD\r\n")

sessions = []   # one dict a connection: {"server": name, "auth_plain": [...], "auth_tls": [...], "rcpt": [...], "data": [...]}
def smtp_handler(name, offer_tls):
    class Smtp(socketserver.StreamRequestHandler):
        def handle(self):
            st = {"server": name, "auth_plain": [], "auth_tls": [], "rcpt": [], "data": []}; sessions.append(st)
            r, w, tls = self.rfile, self.wfile, False
            def say(b): w.write(b); w.flush()
            say(b"220 fixture ESMTP\r\n")
            while (line := r.readline()):
                cmd = line.decode(errors="replace").rstrip("\r\n")
                up = cmd.upper()
                if up.startswith("EHLO"): say(b"250-fixture\r\n" + (b"250-STARTTLS\r\n" if offer_tls and not tls else b"") + b"250 AUTH PLAIN\r\n")
                elif up == "STARTTLS" and offer_tls and not tls:
                    say(b"220 go ahead\r\n")
                    conn = ctx.wrap_socket(self.request, server_side=True); tls = True
                    r, w = conn.makefile("rb"), conn.makefile("wb")
                elif up.startswith("AUTH PLAIN"):
                    (st["auth_tls"] if tls else st["auth_plain"]).append(base64.b64decode(cmd.split()[2]))
                    say(b"235 ok\r\n" if base64.b64decode(cmd.split()[2]) == b"\0%s\0%s" % (USER.encode(), PASS.encode()) else b"535 no\r\n")
                elif up.startswith("MAIL FROM"): say(b"250 ok\r\n")
                elif up.startswith("RCPT TO"): st["rcpt"].append(cmd[8:].strip("<>")); say(b"250 ok\r\n")
                elif up == "DATA":
                    say(b"354 go\r\n"); body = b""
                    while (l := r.readline()) and l != b".\r\n": body += l
                    st["data"].append(body); say(b"250 queued\r\n")
                elif up == "QUIT": say(b"221 bye\r\n"); return
                else: say(b"500 what\r\n")
    return Smtp
class TLSServer(socketserver.ThreadingTCPServer):
    allow_reuse_address = True; daemon_threads = True
    def get_request(self): s, a = super().get_request(); return ctx.wrap_socket(s, server_side=True), a
class PlainServer(socketserver.ThreadingTCPServer):
    allow_reuse_address = True; daemon_threads = True
imap = TLSServer(("127.0.0.1", 0), Imap)
smtp = PlainServer(("127.0.0.1", 0), smtp_handler("starttls", True))
bare = PlainServer(("127.0.0.1", 0), smtp_handler("bare", False))
for srv in (imap, smtp, bare): threading.Thread(target=srv.serve_forever, daemon=True).start()

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
    QCODE = {":": ("shift", "semicolon"), "/": ("slash",), ".": ("dot",), " ": ("spc",), "@": ("shift", "2"), "-": ("minus",),
             "|": ("shift", "backslash"), "?": ("shift", "slash"), ",": ("comma",)}
    def type_text(text):
        for ch in text: key(*(("shift", ch.lower()) if ch.isupper() else QCODE.get(ch, (ch,))))
    def line(text): type_text(text); key("ret")
    command("qmp_capabilities")
    check("DHCP leased an address", wait_for("net dhcp 10.0.2.15 gw 10.0.2.2", 30), uart()[-300:])
    key("f2")
    check("the Terminal opened", wait_for("terminal open", 10), uart()[-300:])
    n = len(uart()); line("mail login 10.0.2.2:%d %s" % (imap.server_address[1], USER)); time.sleep(0.5); line(PASS)
    check("mail login signs in over IMAP", wait_for("mail: signed in as", 30, n), uart()[n:][-300:])

    n = len(uart()); line("mail smtp 10.0.2.2:%d" % bare.server_address[1])
    check("mail smtp sets the server", wait_for("mail: sending through 10.0.2.2", 10, n), uart()[n:][-200:])
    n = len(uart()); line("mail send a@example.com Hi | hello")
    check("a server with no STARTTLS is refused", wait_for("mail: not sent, the server offers no STARTTLS", 30, n), uart()[n:][-300:])
    bare_s = [x for x in sessions if x["server"] == "bare"]
    check("and never sees AUTH", bool(bare_s) and not any(x["auth_plain"] or x["auth_tls"] for x in bare_s), bare_s)

    n = len(uart()); line("mail smtp 10.0.2.2:%d" % smtp.server_address[1]); wait_for("sending through", 10, n)
    n = len(uart()); line("mail send sam@example.com Lunch plans | .See you at noon.")
    check("mail send sends", wait_for("mail: sent to sam@example.com", 40, n), uart()[n:][-300:])
    st = [x for x in sessions if x["server"] == "starttls"][-1:]
    st = st[0] if st else {}
    check("AUTH only after STARTTLS, with exactly the user and app password", st.get("auth_plain") == [] and st.get("auth_tls") == [b"\0%s\0%s" % (USER.encode(), PASS.encode())], st)
    data = (st.get("data") or [b""])[0]
    check("the message: recipient, subject, and the body's leading dot doubled", st.get("rcpt") == ["sam@example.com"] and b"Subject: Lunch plans\r\n" in data and b"\r\n\r\n..See you at noon.\r\n" in data, (st.get("rcpt"), data))
    check("the password never reaches the UART", PASS not in uart())

    n = len(uart()); line("mail"); wait_for("mail N opens one", 30, n)
    n = len(uart()); line("mail reply 1 Sounds good")
    check("mail reply sends", wait_for("mail: sent to sam@example.com", 40, n), uart()[n:][-300:])
    st = [x for x in sessions if x["server"] == "starttls"][-1]
    check("to the sender, Re: the subject", st["rcpt"] == ["sam@example.com"] and b"Subject: Re: Lunch?\r\n" in st["data"][0] and b"Sounds good" in st["data"][0], st)
    n = len(uart()); line("mail send a@example.com,b@example.com Hi | x")
    check("two addresses at once are refused", wait_for("mail: that is not one plain address", 10, n), uart()[n:][-300:])
finally:
    if q: q.kill(); q.wait()
    for srv in (imap, smtp, bare): srv.shutdown()
    subprocess.run(["make", "-C", str(arch), "clean"], capture_output=True, timeout=60)
if fails: print("FAIL: " + ", ".join(fails)); sys.exit(1)
print("PASS: the Pi sends mail over SMTP submission only after STARTTLS (a server without it is refused before any AUTH), with AUTH PLAIN, dot-stuffing, and replies to the listed sender")
