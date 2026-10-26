#!/usr/bin/env python3
"""Downloads (6.17.0): a reply that is not a page is saved to the card, and `fetch URL` saves any reply.

A stub server on the host: /page.html links to /files/Report-2026.pdf (application/pdf, 70,000 seeded bytes);
/notes is text/plain. On a FAT card image with a JT folder, at the Terminal:
  `fetch .../notes` -> "browser: saved NOTES.BIN, N bytes in JT" (no extension: .BIN);
  `browse .../page.html`, `open 1` -> "browser: saved REPORT-2.PDF, 70000 bytes in JT" and the page stays (`reload`
  fetches /page.html again, not the PDF); a 404 is "not saved".
In Blackcomb, Tab and Enter on the same link put "saved REPORT-2.PDF, 70000 bytes in JT" in the status line.
Off the card image afterwards (mtools), JT/REPORT-2.PDF and JT/NOTES.BIN hold exactly the bytes the server sent.
Discriminating: drop the content-type test in browser.c's show() and the PDF is shown as a page, not saved.
Skips (exit 0) when clang's aarch64 target, ld.lld, qemu-system-aarch64 or mtools is missing.
Usage: tools/checks/arm64-downloads-check.py   (from the repo root)
"""
import http.server, json, os, random, shutil, socket, subprocess, sys, threading, time
from pathlib import Path

root = Path(__file__).resolve().parents[2]
arch = root / "arch/arm64"
sys.path.insert(0, str(Path(__file__).resolve().parent))
from scratch import scratch_dir
if not all(shutil.which(t) for t in ("clang", "ld.lld", "qemu-system-aarch64", "mcopy", "mmd")):
    print("SKIP: clang, ld.lld, qemu-system-aarch64 or mtools not installed"); sys.exit(0)
from sdcard import PART, build_card
tmp = Path(scratch_dir("arm64-downloads"))
MT = dict(os.environ, MTOOLS_SKIP_CHECK="1")
card = str(tmp / "card.img"); build_card(card, False)
subprocess.run(["mmd", "-i", f"{card}@@{PART * 512}", "::JT"], capture_output=True, env=MT)

PDF = random.Random(11).randbytes(70000)
NOTES = b"shopping: eggs, bread, coffee\n" * 40
PAGE = b"<html><head><title>Files</title></head><body><p><a href=/files/Report-2026.pdf>the report</a> <a href=/gone.zip>gone</a></p></body></html>"
gets = []
class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        gets.append(self.path)
        body, kind = {"/page.html": (PAGE, "text/html"), "/files/Report-2026.pdf": (PDF, "application/pdf"), "/notes": (NOTES, "text/plain")}.get(self.path, (b"no", None))
        self.send_response(200 if kind else 404); self.send_header("Content-Type", kind or "application/zip")
        self.send_header("Content-Length", str(len(body))); self.end_headers(); self.wfile.write(body)
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
        command("send-key", {"keys": [{"type": "qcode", "data": k} for k in names], "hold-time": 50}); time.sleep(0.15)
    QCODE = {":": ("shift", "semicolon"), "/": ("slash",), ".": ("dot",), " ": ("spc",)}
    def type_text(text):
        for ch in text: key(*QCODE.get(ch, (ch,)))
    def line(text): type_text(text); key("ret")
    command("qmp_capabilities")
    check("DHCP leased an address", wait_for("net dhcp 10.0.2.15 gw 10.0.2.2", 30), uart()[-300:])
    key("f2")
    check("the Terminal opened", wait_for("terminal open", 10), uart()[-300:])
    base = "http://10.0.2.2:%d" % port

    n = len(uart()); line("fetch %s/notes" % base)
    check("fetch saves a text reply as a file", wait_for("browser: saved NOTES.BIN, %d bytes in JT" % len(NOTES), 20, n), uart()[n:][-300:])
    n = len(uart()); line("browse %s/page.html" % base)
    check("the page with the link loads", wait_for("the report [1]", 20, n), uart()[n:][-300:])
    n = len(uart()); line("open 1")
    check("a PDF link is saved, not shown", wait_for("browser: saved REPORT-2.PDF, 70000 bytes in JT", 30, n), uart()[n:][-300:])
    k = len(gets); n = len(uart()); line("reload")
    check("and the page stays: reload fetches it, not the PDF", wait_for("the report [1]", 20, n) and gets[k:] == ["/page.html"], gets[k:])
    n = len(uart()); line("open 2")
    check("a 404 is not saved", wait_for("browser: not saved, the server said 404", 20, n), uart()[n:][-300:])

    key("esc"); time.sleep(0.5)
    key("f1"); type_text("blackcomb"); key("ret")
    check("Spotlight opens Blackcomb", wait_for("browser open", 10), uart()[-400:])
    n = len(uart()); type_text("%s/page.html" % base); key("ret")
    check("the window loads the page", wait_for("browser: page ", 20, n), uart()[n:][-300:])
    n = len(uart()); key("tab"); key("ret")
    check("Tab and Enter on the PDF link say where it went", wait_for("browser: status saved REPORT-2.PDF, 70000 bytes in JT", 30, n), uart()[n:][-300:])
    q.kill(); q.wait(); q = None
    def card_file(name):
        out = tmp / name
        subprocess.run(["mcopy", "-n", "-i", f"{card}@@{PART * 512}", "::JT/" + name, str(out)], capture_output=True, env=MT)
        return out.read_bytes() if out.exists() else b""
    check("JT/REPORT-2.PDF on the card is the server's bytes", card_file("REPORT-2.PDF") == PDF, len(card_file("REPORT-2.PDF")))
    check("JT/NOTES.BIN on the card is the server's bytes", card_file("NOTES.BIN") == NOTES, len(card_file("NOTES.BIN")))
finally:
    if q: q.kill(); q.wait()
    server.shutdown()
    subprocess.run(["make", "-C", str(arch), "clean"], capture_output=True, timeout=60)
if fails: print("FAIL: " + ", ".join(fails)); sys.exit(1)
print("PASS: a link to a file saves it to the card under an 8.3 name with the page left in place, fetch URL saves any reply, a 404 is not saved, and the card holds the exact bytes")
