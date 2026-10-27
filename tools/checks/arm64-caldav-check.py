#!/usr/bin/env python3
"""Two-way CalDAV on the Pi (6.24.0), against a fixture CalDAV server over TLS on the host.

The fixture holds a calendar at /cal/joshua/ behind Basic auth: two events of its own, one whose SUMMARY carries an
ampersand (XML-escaped in the REPORT reply) and one sent as CDATA, plus whatever the Pi PUTs. The card's JT/EVENTS.TXT has
the person's own day, "Dentist, 10am". At the Terminal:
  `calendar caldav http://...` is refused (https only); `calendar caldav https://10.0.2.2:PORT/cal/joshua/ USER` asks for
  the app password with stars; a wrong one makes `calendar sync` say the server refused it; the right one sends the
  person's day (PUT jt-YYYYMMDD.ics, UID jt-YYYYMMDD@joshuatree, the comma escaped, the right Authorization) and reads
  the three events back into SUBS.TXT; Calendar then shows the two days the person has no event on.
6.41.0, deletes both ways: the card's JT/CALSYNC.TXT says the last sync had d0 and d5. EVENTS.TXT has no d0 (deleted
here) and has "Gym" on d5; the server has jt-d0.ics but no d5 (deleted there). The good sync DELETEs jt-d0.ics, takes
Gym off EVENTS.TXT and never PUTs it, and leaves CALSYNC.TXT listing d1 alone.
6.42.0, discovery: the good password is given with the server's root, https://10.0.2.2:PORT/. PROPFIND there names a
principal, the principal names the calendar home by its full https address, and the home holds an inbox, a tasks list
(VTODO only) and the event calendar; the Pi says it found /cal/joshua/ and syncs there.
6.43.0: once CalDAV is set, opening Calendar from Spotlight syncs first (the deletes above happen there) and the app
shows what came down; a `calendar sync` after it, and a second open, do not sync again from the open.
6.48.0: the home also holds a Work calendar; discovery lists Personal and Work and uses 1; `calendar use 2` and a sync
there deletes nothing (CALSYNC.TXT names Personal) and sends Dentist to Work; `calendar use 1` and a sync come back.
Discriminating: drop the &amp; unescape in ics.c's dav_feed and SUBS.TXT says "Q&amp;A"; skip the CALSYNC.TXT read in
dav_sync and jt-d0.ics stays on the server while Gym goes back up.
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
d1, d2, d3, d0, d5 = (today + datetime.timedelta(days=k) for k in (4, 9, 15, 6, 12))
ymd = lambda d: d.strftime("%Y%m%d"); iso = lambda d: d.strftime("%Y-%m-%d")
open(os.path.join(tmp, "EVENTS.TXT"), "w").write("%s|Dentist, 10am\n%s|Gym\n" % (iso(d1), iso(d5)))
subprocess.run(["mcopy", "-i", f"{card}@@{PART * 512}", os.path.join(tmp, "EVENTS.TXT"), "::JT/EVENTS.TXT"], capture_output=True, env=MT, check=True)
USER, PASS = "joshua@example.com", "dav-pass-555"
AUTH = "Basic " + base64.b64encode(("%s:%s" % (USER, PASS)).encode()).decode()

store = {   # path -> iCalendar text: the Personal calendar's own two, and a day the Pi sent last time (6.48.0: by path, so
             # each calendar holds its own; Work starts empty)
    "/cal/joshua/a.ics": "BEGIN:VCALENDAR\r\nBEGIN:VEVENT\r\nUID:a\r\nDTSTART;VALUE=DATE:%s\r\nSUMMARY:Q&A with team\r\nEND:VEVENT\r\nEND:VCALENDAR\r\n" % ymd(d2),
    "/cal/joshua/b.ics": "BEGIN:VCALENDAR\r\nBEGIN:VEVENT\r\nUID:b\r\nDTSTART:%sT170000Z\r\nSUMMARY:Design review\r\nEND:VEVENT\r\nEND:VCALENDAR\r\n" % ymd(d3),
    "/cal/joshua/jt-%s.ics" % ymd(d0): "BEGIN:VCALENDAR\r\nBEGIN:VEVENT\r\nUID:jt-%s@joshuatree\r\nDTSTART;VALUE=DATE:%s\r\nSUMMARY:Old plan\r\nEND:VEVENT\r\nEND:VCALENDAR\r\n" % (ymd(d0), ymd(d0)),
}
puts, auths, deletes, finds = [], [], [], []
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
        created = self.path not in store
        store[self.path] = data.decode(); puts.append((self.path, data.decode()))
        self.reply(201 if created else 204)
    def do_DELETE(self):
        if not self.authed(): return
        deletes.append(self.path)
        self.reply(204 if store.pop(self.path, None) is not None else 404)
    def do_PROPFIND(self):
        self.body()
        if not self.authed(): return
        finds.append((self.path, self.headers.get("Depth")))
        R = lambda href, props: "<d:response><d:href>%s</d:href><d:propstat><d:prop>%s</d:prop><d:status>HTTP/1.1 200 OK</d:status></d:propstat></d:response>" % (href, props)
        cal = lambda comps: "<d:resourcetype><d:collection/><cal:calendar/></d:resourcetype><cal:supported-calendar-component-set>%s</cal:supported-calendar-component-set>" % "".join('<cal:comp name="%s"/>' % c for c in comps)
        if self.path == "/":
            x = R("/", "<d:resourcetype><d:collection/></d:resourcetype><d:current-user-principal><d:href>/principals/joshua/</d:href></d:current-user-principal>") \
                .replace("</d:propstat>", "</d:propstat><d:propstat><d:prop><cal:calendar-home-set/></d:prop><d:status>HTTP/1.1 404 Not Found</d:status></d:propstat>", 1)
        elif self.path == "/principals/joshua/":
            x = R(self.path, "<cal:calendar-home-set><d:href>https://10.0.2.2:%d/cal/</d:href></cal:calendar-home-set>" % port)
        elif self.path == "/cal/":
            x = R("/cal/", "<d:resourcetype><d:collection/></d:resourcetype>") + R("/cal/inbox/", "<d:resourcetype><d:collection/><cal:schedule-inbox/></d:resourcetype>") \
                + R("/cal/tasks/", cal(["VTODO"])) + R("/cal/joshua/", cal(["VEVENT", "VTODO"]) + "<d:displayname>Personal</d:displayname>") \
                + R("/cal/work/", cal(["VEVENT"]) + "<d:displayname>Work</d:displayname>")
        elif self.path in ("/cal/joshua/", "/cal/work/"):
            x = R(self.path, cal(["VEVENT"]))
        else: self.reply(404); return
        self.reply(207, ('<?xml version="1.0" encoding="utf-8"?><d:multistatus xmlns:d="DAV:" xmlns:cal="urn:ietf:params:xml:ns:caldav">%s</d:multistatus>' % x).encode(), "application/xml; charset=utf-8")
    def do_REPORT(self):
        self.body()
        if not self.authed(): return
        parts = []
        for path, ics in sorted(store.items()):
            if not path.startswith(self.path): continue
            cd = "<![CDATA[%s]]>" % ics if path.endswith("b.ics") else xml_escape(ics)
            parts.append("<d:response><d:href>%s</d:href><d:propstat><d:prop><cal:calendar-data>%s</cal:calendar-data></d:prop><d:status>HTTP/1.1 200 OK</d:status></d:propstat></d:response>" % (path, cd))
        x = '<?xml version="1.0" encoding="utf-8"?><d:multistatus xmlns:d="DAV:" xmlns:cal="urn:ietf:params:xml:ns:caldav">%s</d:multistatus>' % "".join(parts)
        self.reply(207, x.encode(), "application/xml; charset=utf-8")
    def log_message(self, *a): pass
ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER); ctx.load_cert_chain(cert, key)
server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Dav)
server.socket = ctx.wrap_socket(server.socket, server_side=True)
port = server.server_address[1]
# 6.41.0's last-sync list, naming the calendar it belongs to (6.48.0): d0 and d5 were on both sides last time
open(os.path.join(tmp, "CALSYNC.TXT"), "w").write("https://10.0.2.2:%d/cal/joshua/\n%s\n%s\n" % (port, ymd(d0), ymd(d5)))
subprocess.run(["mcopy", "-i", f"{card}@@{PART * 512}", os.path.join(tmp, "CALSYNC.TXT"), "::JT/CALSYNC.TXT"], capture_output=True, env=MT, check=True)
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
    n = len(uart()); line("calendar caldav https://10.0.2.2:%d/ %s" % (port, USER)); time.sleep(0.5); line(PASS); wait_for("calendar: CalDAV", 20, n)
    work = "https://10.0.2.2:%d/cal/work/" % port
    check("given the server's root, discovery lists both event calendars by name and uses the first (6.42.0, 6.48.0)",
          "calendar: 2 calendars hold events:\n" in uart()[n:] and "  1  Personal  %s\n" % url in uart()[n:] and "  2  Work  %s\n" % work in uart()[n:]
          and "calendar: using 1; calendar use N picks another" in uart()[n:]
          and [p for p, _ in finds[-3:]] == ["/", "/principals/joshua/", "/cal/"] and finds[-1][1] == "1", (finds, uart()[n:][-300:]))
    key("esc"); time.sleep(0.5)
    n = na = len(uart()); key("f1"); [key(c) for c in "calendar"]; key("ret")
    check("opening Calendar syncs first, and the app shows what came down (6.43.0)", wait_for("calendar: syncing before Calendar opens", 20, n)
          and wait_for("calendar: 1 of your days sent", 30, n) and wait_for("calendar: subscribed events 2", 30, n), uart()[n:][-400:])
    key("esc"); time.sleep(0.5); key("f2"); wait_for("terminal open", 10, n)
    n = len(uart()); line("calendar sync")
    check("sync sends the person's day and reads the calendar back", wait_for("calendar: 1 of your days sent", 30, n) and wait_for("calendar: 3 events read, 3 days kept in SUBS.TXT", 30, n), uart()[n:][-400:])
    sent = puts[-1] if puts else ("", "")
    check("the day went up as jt-YYYYMMDD.ics with its UID and the comma escaped", sent[0] == "/cal/joshua/jt-%s.ics" % ymd(d1) and "UID:jt-%s@joshuatree" % ymd(d1) in sent[1]
          and "DTSTART;VALUE=DATE:%s" % ymd(d1) in sent[1] and "SUMMARY:Dentist\\, 10am" in sent[1], sent)
    check("with exactly the user and app password", auths[-1] == AUTH, auths[-1:])
    check("a day deleted here is deleted on the server (6.41.0)", deletes == ["/cal/joshua/jt-%s.ics" % ymd(d0)] and "/cal/joshua/jt-%s.ics" % ymd(d0) not in store
          and "calendar: 1 day deleted here, taken off the server" in uart()[na:], (deletes, uart()[na:][-300:]))
    check("a day deleted on the server is taken off here, not sent back", "calendar: 1 day deleted on the server, taken off here" in uart()[na:]
          and not any(ymd(d5) in p for p, _ in puts), (puts, uart()[na:][-300:]))
    key("esc"); time.sleep(0.5)
    n = len(uart()); key("f1"); [key(c) for c in "calendar"]; key("ret")
    check("Calendar shows the two days the person has no event on", wait_for("calendar: subscribed events 2", 20, n), uart()[n:][-300:])
    check("and does not sync again within 15 minutes", "syncing before Calendar opens" not in uart()[n:], uart()[n:][-300:])
    key("esc"); time.sleep(0.5); key("f2"); wait_for("terminal open", 10, n)
    n = len(uart()); line("calendar use 2")
    check("calendar use 2 switches to Work", wait_for("calendar: using Work", 10, n), uart()[n:][-300:])
    k = len(deletes); n = len(uart()); line("calendar sync")
    ok = wait_for("calendar: 1 of your days sent", 30, n)
    check("a sync with Work, whose last-sync list names Personal, deletes nothing and sends Dentist there (6.48.0)",
          ok and len(deletes) == k and "/cal/work/jt-%s.ics" % ymd(d1) in store and "deleted on the server" not in uart()[n:], (deletes[k:], uart()[n:][-300:]))
    n = len(uart()); line("calendar use 1"); wait_for("calendar: using Personal", 10, n)
    n = len(uart()); line("calendar sync"); wait_for("calendar: 3 events read", 30, n)
    check("the app password never reaches the UART", PASS not in uart() and "wrong-pass-1" not in uart())
finally:
    q.kill(); q.wait(); server.shutdown()
    subprocess.run(["make", "-C", arch, "clean"], capture_output=True)
out = os.path.join(tmp, "subs.txt")
subprocess.run(["mcopy", "-n", "-i", f"{card}@@{PART * 512}", "::JT/SUBS.TXT", out], capture_output=True, env=MT)
got = open(out).read() if os.path.exists(out) else ""
want = "%s|Dentist, 10am\n%s|Q&A with team\n%s|Design review\n" % (iso(d1), iso(d2), iso(d3))
check("SUBS.TXT holds the three days, escapes undone (XML and iCalendar)", got == want, (got, want))
def card_file(name):
    o = os.path.join(tmp, name.lower() + ".out")
    subprocess.run(["mcopy", "-n", "-i", f"{card}@@{PART * 512}", "::JT/" + name, o], capture_output=True, env=MT)
    return open(o).read() if os.path.exists(o) else ""
check("EVENTS.TXT keeps Dentist and lost Gym", card_file("EVENTS.TXT") == "%s|Dentist, 10am\n" % iso(d1), card_file("EVENTS.TXT"))
check("CALSYNC.TXT names Personal and lists the day both sides now have", card_file("CALSYNC.TXT") == "https://10.0.2.2:%d/cal/joshua/\n%s\n" % (port, ymd(d1)), card_file("CALSYNC.TXT"))
if fails: print("FAIL: " + ", ".join(fails)); sys.exit(1)
print("PASS: CalDAV over https only, the app password with stars, the person's day sent as an iCalendar object and the calendar read back through REPORT, XML and CDATA undone")
