#!/usr/bin/env python3
"""Calendar subscriptions (6.19.0): `calendar subscribe URL` turns an iCalendar feed into SUBS.TXT, and Calendar shows it.

A stub server on the host serves /team.ics: four VEVENTs over three days ahead of today, one SUMMARY folded across two
lines with an escaped comma, two events on one day, one DTSTART;VALUE=DATE and one with a TZID time. The card's JT/EVENTS.TXT
already has the person's own event on the third day. At the Terminal:
  `calendar subscribe http://10.0.2.2:PORT/team.ics` -> "4 events read, 3 days kept in SUBS.TXT";
  a page that is not a feed is refused; then Spotlight opens Calendar, which logs "calendar: subscribed events 2"
  (the person's own day keeps theirs).
Off the card afterwards: SUBS.TXT holds the three days, the folded summary whole and unescaped, the shared day's two
events joined with "; ", and EVENTS.TXT is untouched.
Discriminating: drop the unfolding in ics.c's parse() and the folded summary is cut at its first line.
Skips (exit 0) when clang's aarch64 target, ld.lld, qemu-system-aarch64 or mtools is missing.
Usage: tools/checks/arm64-ics-check.py   (from the repo root)
"""
import datetime, http.server, json, os, shutil, socket, subprocess, sys, threading, time
from pathlib import Path

root = Path(__file__).resolve().parents[2]
arch = root / "arch/arm64"
sys.path.insert(0, str(Path(__file__).resolve().parent))
from scratch import scratch_dir
if not all(shutil.which(t) for t in ("clang", "ld.lld", "qemu-system-aarch64", "mcopy", "mmd")):
    print("SKIP: clang, ld.lld, qemu-system-aarch64 or mtools not installed"); sys.exit(0)
from sdcard import PART, build_card
tmp = Path(scratch_dir("arm64-ics"))
MT = dict(os.environ, MTOOLS_SKIP_CHECK="1")
card = str(tmp / "card.img"); build_card(card, False)
subprocess.run(["mmd", "-i", f"{card}@@{PART * 512}", "::JT"], capture_output=True, env=MT)

today = datetime.date.today()
d1, d2, d3 = (today + datetime.timedelta(days=k) for k in (5, 10, 20))
ymd = lambda d: d.strftime("%Y%m%d")
iso = lambda d: d.strftime("%Y-%m-%d")
mine = iso(d3) + "|Dentist\n"
(tmp / "EVENTS.TXT").write_text(mine)
subprocess.run(["mcopy", "-i", f"{card}@@{PART * 512}", str(tmp / "EVENTS.TXT"), "::JT/EVENTS.TXT"], capture_output=True, env=MT, check=True)
FEED = ("BEGIN:VCALENDAR\r\nVERSION:2.0\r\nX-WR-CALNAME:Team\r\n"
        "BEGIN:VEVENT\r\nUID:a\r\nDTSTART;VALUE=DATE:%s\r\nSUMMARY:Planning\\, all hands in the\r\n  big room\r\nEND:VEVENT\r\n"
        "BEGIN:VEVENT\r\nUID:b\r\nDTSTART;TZID=America/Vancouver:%sT090000\r\nSUMMARY:Standup\r\nEND:VEVENT\r\n"
        "BEGIN:VEVENT\r\nUID:c\r\nDTSTART:%sT230000Z\r\nSUMMARY:Retro\r\nEND:VEVENT\r\n"
        "BEGIN:VEVENT\r\nUID:d\r\nDTSTART;VALUE=DATE:%s\r\nSUMMARY:Offsite\r\nEND:VEVENT\r\n"
        "END:VCALENDAR\r\n") % (ymd(d1), ymd(d2), ymd(d2), ymd(d3))
class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        body, kind = (FEED.encode(), "text/calendar") if self.path == "/team.ics" else (b"<html><body>not a feed</body></html>", "text/html")
        self.send_response(200); self.send_header("Content-Type", kind); self.send_header("Content-Length", str(len(body))); self.end_headers(); self.wfile.write(body)
    def log_message(self, *a): pass
server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
port = server.server_address[1]
threading.Thread(target=server.serve_forever, daemon=True).start()

fails = []
def check(name, ok, detail=""):
    print(("  ok: " if ok else "  FAIL: ") + name + ("" if ok else "  " + repr(detail)[-500:]))
    if not ok: fails.append(name)

log, sock = tmp / "uart", tmp / "qmp"
q = None
try:
    subprocess.run(["make", "-C", str(arch), "clean"], capture_output=True, timeout=60)
    r = subprocess.run(["make", "-C", str(arch), "kernel8.elf"], capture_output=True, text=True, timeout=600)
    if r.returncode: print(r.stderr[-2000:]); print("FAIL: arch/arm64 does not build"); sys.exit(1)
    q = subprocess.Popen(["qemu-system-aarch64", "-machine", "virt", "-cpu", "cortex-a72", "-m", "256",
                          "-global", "virtio-mmio.force-legacy=false", "-device", "ramfb",
                          "-netdev", "user,id=n", "-device", "virtio-net-device,netdev=n",
                          "-device", "virtio-keyboard-device", "-device", "virtio-tablet-device",
                          "-drive", "if=none,id=card,file=%s,format=raw" % card, "-device", "virtio-blk-device,drive=card",
                          "-display", "none", "-serial", "file:" + str(log), "-qmp", "unix:%s,server,nowait" % sock,
                          "-kernel", str(arch / "kernel8.elf")], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    def uart(): return log.read_text(errors="replace") if log.exists() else ""
    def wait_for(text, secs, after=0):
        end = time.time() + secs
        while time.time() < end:
            if text in uart()[after:]: return True
            time.sleep(0.1)
        return False
    if not wait_for("M2 input ready", 60): raise SystemExit("FAIL: did not boot: %r" % uart()[-600:])
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
    QCODE = {":": ("shift", "semicolon"), "/": ("slash",), ".": ("dot",), " ": ("spc",)}
    def type_text(text):
        for ch in text: key(*QCODE.get(ch, (ch,)))
    def line(text): type_text(text); key("ret")
    command("qmp_capabilities")
    check("DHCP leased an address", wait_for("net dhcp 10.0.2.15 gw 10.0.2.2", 30), uart()[-300:])
    key("f2")
    check("the Terminal opened", wait_for("terminal open", 10), uart()[-300:])
    n = len(uart()); line("calendar subscribe http://10.0.2.2:%d/page" % port)
    check("a page that is not a feed is refused", wait_for("calendar: that is not an iCalendar feed", 20, n), uart()[n:][-300:])
    n = len(uart()); line("calendar subscribe http://10.0.2.2:%d/team.ics" % port)
    check("the feed's four events make three days", wait_for("calendar: 4 events read, 3 days kept in SUBS.TXT", 20, n), uart()[n:][-300:])
    key("esc"); time.sleep(0.5)
    n = len(uart()); key("f1"); type_text("calendar"); key("ret")
    check("Calendar shows the subscribed days the person has no event on", wait_for("calendar: subscribed events 2", 20, n), uart()[n:][-400:])
    q.kill(); q.wait(); q = None
    def card_file(name):
        out = tmp / ("got-" + name)
        subprocess.run(["mcopy", "-n", "-i", f"{card}@@{PART * 512}", "::JT/" + name, str(out)], capture_output=True, env=MT)
        return out.read_text() if out.exists() else ""
    want = "%s|Planning, all hands in the big room\n%s|Standup; Retro\n%s|Offsite\n" % (iso(d1), iso(d2), iso(d3))
    check("SUBS.TXT holds the three days, unfolded, unescaped and joined", card_file("SUBS.TXT") == want, (card_file("SUBS.TXT"), want))
    check("EVENTS.TXT, the person's own, is untouched", card_file("EVENTS.TXT") == mine, card_file("EVENTS.TXT"))
finally:
    if q: q.kill(); q.wait()
    server.shutdown()
    subprocess.run(["make", "-C", str(arch), "clean"], capture_output=True, timeout=60)
if fails: print("FAIL: " + ", ".join(fails)); sys.exit(1)
print("PASS: calendar subscribe reads an iCalendar feed into SUBS.TXT (folded, escaped and same-day events right), Calendar shows it beside the person's own events and never overwrites them")
