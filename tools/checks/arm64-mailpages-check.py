#!/usr/bin/env python3
"""Mail as pages in Blackcomb (6.29.0): `mail:` is the inbox, `mail:N` a message with a Reply form, and both forms send.

Fixtures for 10.0.2.2, one throwaway certificate built into the kernel's anchors: IMAP over TLS with two messages
(Sam: Lunch?, Bea: Notes), and SMTP that offers STARTTLS. At the Terminal:
  `browse mail:` before mail login says Mail is not set up; after it, the inbox page lists "Bea: Notes [1]" and
  "Sam: Lunch? [2]" (newest first) and a New message form; `open 2` shows Sam's message and a Reply form; filling its
  body and pressing Reply sends to sam@example.com with "Subject: Re: Lunch?"; on the inbox, filling to, subject and
  body and pressing Send sends to new@example.com; each send comes back to the inbox. In the Blackcomb window, typing
  mail: loads the inbox (not a web search), and Tab then Enter opens the first message; its Reply box (6.40.0, a
  <textarea>) takes Shift+Enter as a line break, and the reply goes out with it. At the Terminal `fill 1 A\nB` puts a
  break in the box too, `top` shows it as " / ", and a second line longer than a one-line field's 63 characters is sent
  whole. Then Mail from Spotlight
  (6.32.0), signed in, opens Blackcomb on the inbox and not the card's MAIL.TXT app.
Discriminating: drop the mail: branch in browser.c's fetch_once and `browse mail:` fails to load; drop the slot 2 line
in windows.c's dock_activate and Spotlight's Mail starts the offline app.
Skips (exit 0) when clang's aarch64 target, ld.lld, openssl or qemu-system-aarch64 is missing.
Usage: tools/checks/arm64-mailpages-check.py   (from the repo root)
"""
import base64, json, os, shutil, socket, socketserver, ssl, subprocess, sys, threading, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from scratch import scratch_dir

root = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
arch = os.path.join(root, "arch", "arm64")
if not all(shutil.which(t) for t in ("clang", "ld.lld", "qemu-system-aarch64", "openssl")):
    print("SKIP: clang, ld.lld, openssl or qemu-system-aarch64 not installed"); sys.exit(0)
tmp = scratch_dir("arm64-mailpages")
cert, key = os.path.join(tmp, "cert.pem"), os.path.join(tmp, "key.pem")
subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "2", "-subj", "/CN=10.0.2.2",
                "-addext", "subjectAltName=DNS:10.0.2.2", "-keyout", key, "-out", cert], check=True, capture_output=True)
ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER); ctx.load_cert_chain(cert, key)
USER, PASS = "joshua@example.com", "app-pass-1234"
MSGS = [(b"From: Sam <sam@example.com>\r\nSubject: Lunch?\r\nContent-Type: text/plain\r\n\r\n", b"Lunch at noon by the lake?\r\n"),
        (b"From: Bea <bea@example.com>\r\nSubject: Notes\r\nContent-Type: text/plain\r\n\r\n", b"The meeting notes are attached.\r\n")]
class Imap(socketserver.StreamRequestHandler):
    def handle(self):
        w = self.wfile.write; w(b"* OK ready\r\n")
        while (line := self.rfile.readline()):
            tag, _, rest = line.decode().rstrip("\r\n").partition(" "); up = rest.upper()
            if up.startswith("LOGIN"): w((tag + (" OK done\r\n" if rest == 'LOGIN "%s" "%s"' % (USER, PASS) else " NO bad\r\n")).encode())
            elif up.startswith("SELECT"): w(b"* %d EXISTS\r\n%s OK done\r\n" % (len(MSGS), tag.encode()))
            elif up.startswith("FETCH") and "HEADER.FIELDS" in up:
                lo, hi = (int(x) for x in rest.split()[1].split(":"))
                for k in range(lo, hi + 1):
                    h = b"\r\n".join(l for l in MSGS[k - 1][0].split(b"\r\n") if l.startswith((b"From:", b"Subject:"))) + b"\r\n\r\n"
                    w(b"* %d FETCH (BODY[HEADER.FIELDS (FROM SUBJECT)] {%d}\r\n%s)\r\n" % (k, len(h), h))
                w(tag.encode() + b" OK done\r\n")
            elif up.startswith("FETCH"):
                k = int(rest.split()[1]); h, t = MSGS[k - 1]
                w(b"* %d FETCH (BODY[HEADER] {%d}\r\n%s BODY[TEXT] {%d}\r\n%s)\r\n%s OK done\r\n" % (k, len(h), h, len(t), t, tag.encode()))
            elif up.startswith("LOGOUT"): w(b"* BYE\r\n" + tag.encode() + b" OK done\r\n"); return
            else: w(tag.encode() + b" BAD\r\n")
sent = []
class Smtp(socketserver.StreamRequestHandler):
    def handle(self):
        r, w, tls = self.rfile, self.wfile, False; st = {"rcpt": [], "data": b""}
        def say(b): w.write(b); w.flush()
        say(b"220 fixture\r\n")
        while (line := r.readline()):
            cmd = line.decode(errors="replace").rstrip("\r\n"); up = cmd.upper()
            if up.startswith("EHLO"): say(b"250-fixture\r\n" + (b"" if tls else b"250-STARTTLS\r\n") + b"250 AUTH PLAIN\r\n")
            elif up == "STARTTLS": say(b"220 go\r\n"); c = ctx.wrap_socket(self.request, server_side=True); tls = True; r, w = c.makefile("rb"), c.makefile("wb")
            elif up.startswith("AUTH PLAIN"): say(b"235 ok\r\n" if base64.b64decode(cmd.split()[2]) == b"\0%s\0%s" % (USER.encode(), PASS.encode()) else b"535 no\r\n")
            elif up.startswith("MAIL FROM"): say(b"250 ok\r\n")
            elif up.startswith("RCPT TO"): st["rcpt"].append(cmd[8:].strip("<>")); say(b"250 ok\r\n")
            elif up == "DATA":
                say(b"354 go\r\n")
                while (l := r.readline()) and l != b".\r\n": st["data"] += l
                sent.append(st); st = {"rcpt": [], "data": b""}; say(b"250 queued\r\n")
            elif up == "QUIT": say(b"221 bye\r\n"); return
            else: say(b"500 what\r\n")
class TLSServer(socketserver.ThreadingTCPServer):
    allow_reuse_address = True; daemon_threads = True
    def get_request(self): s, a = super().get_request(); return ctx.wrap_socket(s, server_side=True), a
class PlainServer(socketserver.ThreadingTCPServer):
    allow_reuse_address = True; daemon_threads = True
imap, smtp = TLSServer(("127.0.0.1", 0), Imap), PlainServer(("127.0.0.1", 0), Smtp)
for srv in (imap, smtp): threading.Thread(target=srv.serve_forever, daemon=True).start()
fails = []
def check(name, ok, detail=""):
    print(("  ok: " if ok else "  FAIL: ") + name + ("" if ok or not detail else " -- " + str(detail)[-500:]))
    if not ok: fails.append(name)
subprocess.run(["make", "-C", arch, "clean"], capture_output=True)
b = subprocess.run(["make", "-C", arch, "kernel8.elf", "TLS_TA=" + cert], capture_output=True, text=True, timeout=900)
if b.returncode: print((b.stdout + b.stderr)[-1500:]); print("FAIL: arch/arm64 does not build"); sys.exit(1)
log, sock = os.path.join(tmp, "uart"), os.path.join(tmp, "qmp")
q = subprocess.Popen(["qemu-system-aarch64", "-machine", "virt", "-cpu", "cortex-a72", "-m", "256", "-global", "virtio-mmio.force-legacy=false",
                      "-netdev", "user,id=n", "-device", "virtio-net-device,netdev=n", "-device", "ramfb", "-device", "virtio-keyboard-device",
                      "-display", "none", "-serial", "file:" + log, "-qmp", "unix:%s,server,nowait" % sock,
                      "-kernel", os.path.join(arch, "kernel8.elf")], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
def uart(): return open(log, errors="replace").read() if os.path.exists(log) else ""
def wait_for(text, secs, after=0):
    end = time.time() + secs
    while time.time() < end:
        if text in uart()[after:]: return True
        time.sleep(0.1)
    return False
try:
    if not wait_for("M2 input ready", 60): raise SystemExit("FAIL: did not boot: %r" % uart()[-600:])
    time.sleep(0.5)
    s = socket.socket(socket.AF_UNIX); s.settimeout(30); s.connect(sock); f = s.makefile("rw"); f.readline()
    def command(name, **a):
        f.write(json.dumps({"execute": name, "arguments": a}) + "\n"); f.flush()
        while True:
            r = json.loads(f.readline())
            if "return" in r or "error" in r: return r
    QK = {" ": ("spc",), ".": ("dot",), "/": ("slash",), ":": ("shift", "semicolon"), "@": ("shift", "2"), "-": ("minus",), "\\": ("backslash",)}
    def key(*k): command("send-key", keys=[{"type": "qcode", "data": x} for x in k]); time.sleep(0.12)
    def typ(t):
        for ch in t: key(*(("shift", ch.lower()) if ch.isupper() else QK.get(ch, (ch,))))
    def run(t, want, secs=30):
        n = len(uart()); typ(t); key("ret"); ok = wait_for(want, secs, n); time.sleep(0.3); return ok, uart()[n:]
    command("qmp_capabilities")
    check("DHCP leased an address", wait_for("net dhcp 10.0.2.15 gw 10.0.2.2", 30), uart()[-300:])
    key("f2"); wait_for("terminal open", 10)
    ok, out = run("browse mail:", "Mail is not set up", 20)
    check("mail: before a login says Mail is not set up", ok, out[-300:])
    n = len(uart()); typ("mail login 10.0.2.2:%d %s" % (imap.server_address[1], USER)); key("ret"); time.sleep(0.5); typ(PASS); key("ret")
    check("mail login", wait_for("mail: signed in as", 30, n), uart()[n:][-300:])
    run("mail smtp 10.0.2.2:%d" % smtp.server_address[1], "mail: sending through", 10)
    ok, out = run("browse mail:", "Send [6]", 30)
    flat = out.replace("\n", " ")
    check("the inbox page lists the newest first as links, and a New message form", ok and "Bea: Notes [1]" in flat and "Sam: Lunch? [2]" in flat and "to: ________ [3]" in flat, out[-500:])
    ok, out = run("open 2", "Reply [2]", 30)
    check("a message page: its text and a Reply form", ok and "Lunch at noon by the lake?" in out, out[-500:])
    LONG = "See you there at noon and I will bring the sandwiches and the blanket too"
    ok, out = run("fill 1 Sounds good\\n" + LONG, "browser: field body set", 20)
    ok, out = run("top", "Reply [2]", 10)
    check("the Reply box shows both lines, the break as / (6.40.0)", ok and "Sounds good / See you" in out, out[-400:])
    k = len(sent); ok, out = run("open 2", "Send [6]", 40)
    check("Reply sends to the sender, Re: the subject, and comes back to the inbox", ok and len(sent) > k and sent[-1]["rcpt"] == ["sam@example.com"]
          and b"Subject: Re: Lunch?\r\n" in sent[-1]["data"] and b"Sounds good" in sent[-1]["data"], (sent[-1:], out[-300:]))
    check("the reply body keeps its line break and all of its second line (6.40.0)", len(sent) > k and ("Sounds good\r\n" + LONG).encode() in sent[-1]["data"], sent[-1:])
    run("fill 3 new@example.com", "field to set", 10); run("fill 4 Hello there", "field subject set", 10); run("fill 5 First note", "field body set", 10)
    k = len(sent); ok, out = run("open 6", "Send [6]", 40)
    check("Send on the inbox sends a new message", ok and len(sent) > k and sent[-1]["rcpt"] == ["new@example.com"] and b"Subject: Hello there\r\n" in sent[-1]["data"] and b"First note" in sent[-1]["data"], sent[-1:])
    key("esc"); time.sleep(0.5)
    key("f1"); [key(c) for c in "blackcomb"]; key("ret"); wait_for("browser open", 10)
    n = len(uart()); typ("mail:"); key("ret")
    check("typing mail: in Blackcomb loads the inbox, not a search", wait_for("browser: load mail:", 10, n) and wait_for("browser: page ", 30, n) and "duckduckgo" not in uart()[n:], uart()[n:][-300:])
    n = len(uart()); key("tab"); key("ret")
    check("Tab and Enter open the first message", wait_for("browser: link 1", 10, n) and wait_for("browser: page ", 30, n), uart()[n:][-300:])
    time.sleep(0.5); n = len(uart()); key("tab"); key("ret")
    ok = wait_for("browser: editing field 1", 10, n)
    typ("Hi Bea"); key("shift", "ret"); ok = ok and wait_for("browser: new line in field", 10, n); typ("Talk soon"); key("ret")
    ok = ok and wait_for("browser: filled field 1", 10, n); time.sleep(0.5)
    k = len(sent); key("tab"); key("ret")
    end = time.time() + 40
    while len(sent) <= k and time.time() < end: time.sleep(0.2)
    check("in Blackcomb, Shift+Enter in the Reply box sends a two-line reply (6.40.0)", ok and len(sent) > k and b"Hi Bea\r\nTalk soon" in sent[-1]["data"], (sent[-1:], uart()[n:][-300:]))
    key("esc"); time.sleep(0.5)
    n = len(uart()); key("f1"); [key(c) for c in "mail"]; key("ret")
    check("Mail from Spotlight, signed in, is the inbox in Blackcomb", wait_for("browser: load mail:", 15, n) and wait_for("browser: page ", 30, n) and "mail: ring-3 window" not in uart()[n:], uart()[n:][-300:])
finally:
    q.kill(); q.wait()
    for srv in (imap, smtp): srv.shutdown()
    subprocess.run(["make", "-C", arch, "clean"], capture_output=True)
if fails: print("FAIL: " + ", ".join(fails)); sys.exit(1)
print("PASS: Mail is pages in Blackcomb: the inbox as links with a New message form, a message with a Reply form, both sending through SMTP, and mail: typed in the address bar")
