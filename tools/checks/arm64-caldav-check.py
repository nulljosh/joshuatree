#!/usr/bin/env python3
"""Two-way CalDAV on the Pi (6.24.0), against a fixture CalDAV server over TLS on the host.

The fixture holds a calendar at /cal/joshua/ behind Basic auth: two events of its own, one whose SUMMARY carries an
ampersand (XML-escaped in the REPORT reply) and one sent as CDATA, plus whatever the Pi PUTs. The card's JT/EVENTS.TXT has
the person's own day, "Dentist, 10am". At the Terminal:
  `calendar caldav http://...` is refused (https only); `calendar caldav https://10.0.2.2:PORT/cal/joshua/ USER` asks for
  the app password with stars; a wrong one makes `calendar sync` say the server refused it; the right one sends the
  person's day (PUT jt-YYYYMMDD.ics, UID jt-YYYYMMDD@joshuatree, the comma escaped, the right Authorization) and reads
  the three events back into SUBS.TXT; Calendar then shows the two days the person has no event on.
Discriminating: drop the &amp; unescape in ics.c's dav_feed and SUBS.TXT says "Q&amp;A".
Skips (exit 0) when clang's aarch64 target, ld.lld, openssl, qemu-system-aarch64 or mtools is missing.
Usage: tools/checks/arm64-caldav-check.py   (from the repo root)
"""
import base64, datetime, http.server, json, os, shutil, socket, ssl, subprocess, sys, threading, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from scratch import scratch_dir
from sdcard import PART, build_card

root = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
arch = os.path.join(root, "arch", "arm64")
if not all(shutil.which(t) for t in ("clang", "ld.lld", "qemu-system-aarch64", "openssl", "mmd", "mcopy")):
    print("SKIP: clang, ld.lld, openssl, qemu-system-aarch64 or mtools not installed"); sys.exit(0)
tmp = scratch_dir("arm64-caldav")
cert, key = os.path.join(tmp, "cert.pem"), os.path.join(tmp, "key.pem")
subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "2", "-subj", "/CN=10.0.2.2",
                "-addext", "subjectAltName=DNS:10.0.2.2", "-keyout", key, "-out", cert], check=True, capture_output=True)
MT = dict(os.environ, MTOOLS_SKIP_CHECK="1")
card = os.path.join(tmp, "card.img"); build_card(card, False)
subprocess.run(["mmd", "-i", f"{card}@@{PART * 512}", "::JT"], capture_output=True, env=MT)
today = datetime.date.today()
d1, d2, d3 = (today + datetime.timedelta(days=k) for k in (4, 9, 15))
ymd = lambda d: d.strftime("%Y%m%d"); iso = lambda d: d.strftime("%Y-%m-%d")
open(os.path.join(tmp, "EVENTS.TXT"), "w").write("%s|Dentist, 10am\n" % iso(d1))
subprocess.run(["mcopy", "-i", f"{card}@@{PART * 512}", os.path.join(tmp, "EVENTS.TXT"), "::JT/EVENTS.TXT"], capture_output=True, env=MT, check=True)
USER, PASS = "joshua@example.com", "dav-pass-555"
AUTH = "Basic " + base64.b64encode(("%s:%s" % (USER, PASS)).encode()).decode()

store = {   # name -> iCalendar text, the server's own two
    "a.ics": "BEGIN:VCALENDAR\r\nBEGIN:VEVENT\r\nUID:a\r\nDTSTART;VALUE=DATE:%s\r\nSUMMARY:Q&A with team\r\nEND:VEVENT\r\nEND:VCALENDAR\r\n" % ymd(d2),
    "b.ics": "BEGIN:VCALENDAR\r\nBEGIN:VEVENT\r\nUID:b\r\nDTSTART:%sT170000Z\r\nSUMMARY:Design review\r\nEND:VEVENT\r\nEND:VCALENDAR\r\n" % ymd(d3),
}
puts, auths = [], []
def xml_escape(t): return t.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;").replace("\r", "&#13;")
class Dav(http.server.BaseHTTPRequestHandler):
    def body(self): return self.rfile.read(int(self.headers.get("Content-Length", 0)))
    def reply(self, code, data=b"", kind="text/plain"):
        self.send_response(code); self.send_header("Content-Type", kind); self.send_header("Content-Length", str(len(data))); self.end_headers(); self.wfile.write(data)
    def authed(self):
        auths.append(self.headers.get("Authorization", ""))
        if self.headers.get("Authorization") != AUTH: self.reply(401); return False
        return True
    def do_PUT(self):
        data = self.body()
        if not self.authed(): return
        name = self.path.rsplit("/", 1)[-1]; created = name not in store
        store[name] = data.decode(); puts.append((self.path, data.decode()))
        self.reply(201 if created else 204)
    def do_REPORT(self):
        self.body()
        if not self.authed(): return
        parts = []
        for name, ics in sorted(store.items()):
            cd = "<![CDATA[%s]]>" % ics if name == "b.ics" else xml_escape(ics)
            parts.append("<d:response><d:href>/cal/joshua/%s</d:href><d:propstat><d:prop><cal:calendar-data>%s</cal:calendar-data></d:prop><d:status>HTTP/1.1 200 OK</d:status></d:propstat></d:response>" % (name, cd))
        x = '<?xml version="1.0" encoding="utf-8"?><d:multistatus xmlns:d="DAV:" xmlns:cal="urn:ietf:params:xml:ns:caldav">%s</d:multistatus>' % "".join(parts)
        self.reply(207, x.encode(), "application/xml; charset=utf-8")
    def log_message(self, *a): pass
ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER); ctx.load_cert_chain(cert, key)
server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Dav)
server.socket = ctx.wrap_socket(server.socket, server_side=True)
port = server.server_address[1]
threading.Thread(target=server.serve_forever, daemon=True).start()

fails = []
def check(name, ok, detail=""):
    print(("  ok: " if ok else "  FAIL: ") + name + ("" if ok or not detail else " -- " + str(detail)[-400:]))
    if not ok: fails.append(name)
subprocess.run(["make", "-C", arch, "clean"], capture_output=True)
b = subprocess.run(["make", "-C", arch, "kernel8.elf", "TLS_TA=" + cert], capture_output=True, text=True, timeout=900)
if b.returncode: print((b.stdout + b.stderr)[-1500:]); print("FAIL: arch/arm64 does not build"); sys.exit(1)
log, sock = os.path.join(tmp, "uart"), os.path.join(tmp, "qmp")
q = subprocess.Popen(["qemu-system-aarch64", "-machine", "virt", "-cpu", "cortex-a72", "-m", "256", "-global", "virtio-mmio.force-legacy=false",
                      "-netdev", "user,id=n", "-device", "virtio-net-device,netdev=n", "-device", "ramfb", "-device", "virtio-keyboard-device",
                      "-drive", "if=none,id=card,file=%s,format=raw" % card, "-device", "virtio-blk-device,drive=card",
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
    QK = {" ": ("spc",), "-": ("minus",), ".": ("dot",), "/": ("slash",), ":": ("shift", "semicolon"), "@": ("shift", "2")}
    def key(*k): command("send-key", keys=[{"type": "qcode", "data": x} for x in k]); time.sleep(0.12)
    def line(t):
        for ch in t: key(*QK.get(ch, (ch,)))
        key("ret")
    command("qmp_capabilities")
    check("DHCP leased an address", wait_for("net dhcp 10.0.2.15 gw 10.0.2.2", 30), uart()[-300:])
    key("f2"); wait_for("terminal open", 10)
    url = "https://10.0.2.2:%d/cal/joshua/" % port
    n = len(uart()); line("calendar caldav http://10.0.2.2:%d/cal/joshua/ %s" % (port, USER))
    check("plain http is refused", wait_for("calendar: CalDAV takes https only", 10, n), uart()[n:][-300:])
    n = len(uart()); line("calendar caldav %s %s" % (url, USER)); time.sleep(0.5); line("wrong-pass-1")
    check("the app password is asked with stars", wait_for("Calendar app password for %s: \n" % USER, 10, n) and wait_for("calendar: CalDAV", 10, n), uart()[n:][-300:])
    n = len(uart()); line("calendar sync")
    check("a wrong password is refused by the server", wait_for("calendar: the server refused the user or the app password", 30, n) and not puts, (puts, uart()[n:][-300:]))
    n = len(uart()); line("calendar caldav %s %s" % (url, USER)); time.sleep(0.5); line(PASS); wait_for("calendar: CalDAV", 10, n)
    n = len(uart()); line("calendar sync")
    check("sync sends the person's day and reads the calendar back", wait_for("calendar: 1 of your days sent", 30, n) and wait_for("calendar: 3 events read, 3 days kept in SUBS.TXT", 30, n), uart()[n:][-400:])
    sent = puts[-1] if puts else ("", "")
    check("the day went up as jt-YYYYMMDD.ics with its UID and the comma escaped", sent[0] == "/cal/joshua/jt-%s.ics" % ymd(d1) and "UID:jt-%s@joshuatree" % ymd(d1) in sent[1]
          and "DTSTART;VALUE=DATE:%s" % ymd(d1) in sent[1] and "SUMMARY:Dentist\\, 10am" in sent[1], sent)
    check("with exactly the user and app password", auths[-1] == AUTH, auths[-1:])
    key("esc"); time.sleep(0.5)
    n = len(uart()); key("f1"); [key(c) for c in "calendar"]; key("ret")
    check("Calendar shows the two days the person has no event on", wait_for("calendar: subscribed events 2", 20, n), uart()[n:][-300:])
    check("the app password never reaches the UART", PASS not in uart() and "wrong-pass-1" not in uart())
finally:
    q.kill(); q.wait(); server.shutdown()
    subprocess.run(["make", "-C", arch, "clean"], capture_output=True)
out = os.path.join(tmp, "subs.txt")
subprocess.run(["mcopy", "-n", "-i", f"{card}@@{PART * 512}", "::JT/SUBS.TXT", out], capture_output=True, env=MT)
got = open(out).read() if os.path.exists(out) else ""
want = "%s|Dentist, 10am\n%s|Q&A with team\n%s|Design review\n" % (iso(d1), iso(d2), iso(d3))
check("SUBS.TXT holds the three days, escapes undone (XML and iCalendar)", got == want, (got, want))
if fails: print("FAIL: " + ", ".join(fails)); sys.exit(1)
print("PASS: CalDAV over https only, the app password with stars, the person's day sent as an iCalendar object and the calendar read back through REPORT, XML and CDATA undone")
