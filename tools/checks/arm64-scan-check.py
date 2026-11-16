#!/usr/bin/env python3
"""Scanning (13.14.0: arch/arm64/print.c's eSCL half and cmd.c's `scan [SCANNER]`).

  1. Host, under address and undefined-behaviour sanitizers: escl_settings is well-formed XML with the eSCL and PWG
     namespaces, Platen, RGB24, the resolution asked for and image/jpeg. escl_next turns a Location path into
     PATH/NextDocument and refuses one that is empty, relative or too long. escl_is_jpeg takes a JPEG and not an HTML
     page. mdns_query_svc asks PTR _uscan._tcp.local, and mdns_parse_svc keeps a scanner's rs= root (eSCL when none).
  2. QEMU virt with a card and a network: `scan 10.0.2.2:PORT` against a stub scanner on the host, which checks the
     POST (/eSCL/ScanJobs, text/xml, the settings), answers 201 with the job's Location, says 503 once (the lamp warming
     up), then sends a JPEG: the Terminal says saved PHOTOS/SCAN0001.JPG, the next scan is SCAN0002.JPG, and Photos
     lists both. A scanner that says no, a job with no Location, a reply that is not a JPEG and a port with nothing on
     it each say so and save nothing. With mDNS, plain `scan` finds the stub by _uscan._tcp. After QEMU stops, the
     file on the card is the stub's JPEG byte for byte.
Skips (exit 0) without clang, ld.lld, mtools or qemu-system-aarch64.
"""
import http.server, json, os, shutil, socket, struct, subprocess, sys, threading, time
import xml.etree.ElementTree as ET
from pathlib import Path

root = Path(__file__).resolve().parents[2]
arch = root / "arch/arm64"
sys.path.insert(0, str(Path(__file__).resolve().parent))
from scratch import scratch_dir
from sdcard import PART, build_card
if not all(shutil.which(t) for t in ("clang", "ld.lld", "qemu-system-aarch64", "mmd", "mcopy", "mtype")):
    print("SKIP: clang, ld.lld, qemu-system-aarch64 or mtools not installed"); sys.exit(0)
tmp = Path(scratch_dir("scan"))
fails = []
def check(name, ok, detail=""):
    print(("  ok: " if ok else "  FAIL: ") + name + ("" if ok else "  " + repr(detail)[:600]), flush=True)
    if not ok: fails.append(name)
JPEG = (root / "landing/face/talk-26.jpg").read_bytes()

# ---- 1. the builders on the host ----
harness = tmp / "h.c"
harness.write_text(r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
unsigned escl_settings(char *o, unsigned cap, unsigned dpi);
unsigned escl_next(const char *loc, char *out, unsigned cap);
int escl_is_jpeg(const unsigned char *d, unsigned n);
unsigned mdns_query_svc(unsigned char *q, const char *svc);
int mdns_parse_svc(const unsigned char *r, unsigned len, char *name, unsigned ncap, unsigned *ip, unsigned *port, char *path, unsigned pcap, const char *key, const char *dflt);
int main(int argc, char **argv) { (void)argc;
    FILE *f = fopen(argv[1], "rb"); unsigned char *t = malloc(1 << 20); size_t n = fread(t, 1, 1 << 20, f); fclose(f);
    char buf[4096];
    if (argv[2][0] == 's') { unsigned l = escl_settings(buf, sizeof buf, (unsigned)atoi(argv[3])); fwrite(buf, 1, l, stdout); }
    else if (argv[2][0] == 'S') { printf("%u\n", escl_settings(buf, 100, 300)); }
    else if (argv[2][0] == 'n') { t[n] = 0; unsigned l = escl_next((char *)t, buf, (unsigned)atoi(argv[3])); printf("%u|%s\n", l, l ? buf : ""); }
    else if (argv[2][0] == 'j') { printf("%d\n", escl_is_jpeg(t, (unsigned)n)); }
    else if (argv[2][0] == 'q') { unsigned l = mdns_query_svc((unsigned char *)buf, "_uscan"); fwrite(buf, 1, l, stdout); }
    else { char nm[64], pa[48]; unsigned ip, port; int ok = mdns_parse_svc(t, (unsigned)n, nm, sizeof nm, &ip, &port, pa, sizeof pa, "rs", "eSCL");
        printf("%d|%s|%u.%u.%u.%u|%u|%s\n", ok, nm, ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255, port, pa); }
    free(t); return 0;
}
''')
exe = tmp / "h"
r = subprocess.run(["clang", "-Wall", "-Wextra", "-Werror", "-DPRINT_HOST_TEST", "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                    str(harness), str(arch / "print.c"), "-o", str(exe)], capture_output=True, text=True)
if r.returncode: print("FAIL: print.c does not build on the host\n" + r.stderr[-1500:]); sys.exit(1)
def run(data, mode, *a):
    p = tmp / "in"; p.write_bytes(data)
    out = subprocess.run([str(exe), str(p), mode, *a], capture_output=True, timeout=20)
    if out.returncode: print("FAIL: the host harness crashed\n" + out.stderr.decode()[-1500:]); sys.exit(1)
    return out.stdout
NS = {"scan": "http://schemas.hp.com/imaging/escl/2011/05/03", "pwg": "http://www.pwg.org/schemas/2010/12/sm"}
def settings_ok(x):
    try: e = ET.fromstring(x)
    except ET.ParseError as err: return str(err)
    if e.tag != "{%s}ScanSettings" % NS["scan"]: return e.tag
    got = {c.tag.split("}")[1]: c.text for c in e}
    return got
xs = run(b"", "s", "150")
g = settings_ok(xs)
check("the settings are well-formed eSCL ScanSettings", isinstance(g, dict), g)
if isinstance(g, dict):
    check("... Platen, RGB24, 150 dpi both ways, image/jpeg", g.get("InputSource") == "Platen" and g.get("ColorMode") == "RGB24" and g.get("XResolution") == "150"
          and g.get("YResolution") == "150" and g.get("DocumentFormat") == "image/jpeg" and g.get("Version") == "2.6", g)
check("300 dpi asks for 300", settings_ok(run(b"", "s", "300")).get("XResolution") == "300")
check("settings that do not fit come back 0, not cut short", run(b"", "S").strip() == b"0")
def nx(loc, cap=128): return run(loc, "n", str(cap)).decode().strip()
check("a Location path becomes PATH/NextDocument", nx(b"/eSCL/ScanJobs/42").endswith("|/eSCL/ScanJobs/42/NextDocument"))
check("a trailing slash is not doubled", nx(b"/eSCL/ScanJobs/42/").endswith("|/eSCL/ScanJobs/42/NextDocument"))
check("no Location is refused", nx(b"") == "0|")
check("a relative Location is refused", nx(b"eSCL/ScanJobs/1") == "0|")
check("one too long for the buffer is refused", nx(b"/" + b"a" * 200, 64) == "0|")
check("a JPEG is a JPEG", run(JPEG, "j").strip() == b"1")
check("an HTML page is not", run(b"<html>busy</html>", "j").strip() == b"0")
check("two bytes are not", run(b"\xff\xd8", "j").strip() == b"0")
def dn(name): return b"".join(bytes([len(p)]) + p.encode() for p in name.split(".")) + b"\0"
def rr(name_b, typ, data): return name_b + struct.pack(">HHIH", typ, 0x8001, 120, len(data)) + data
def mdns_answer(port, rs="eSCL", name="Desk Scanner"):
    hdr = struct.pack(">HHHHHH", 0x1337, 0x8400, 0, 4, 0, 0)
    ptr_name = dn("_uscan._tcp.local")
    body = rr(ptr_name, 12, bytes([len(name)]) + name.encode() + b"\xc0\x0c")
    inst_at = 12 + len(ptr_name) + 10
    body += rr(b"\xc0" + bytes([inst_at]), 33, struct.pack(">HHH", 0, 0, port) + dn("scan.local"))
    txt = [b"txtvers=1"] + ([("rs=" + rs).encode()] if rs is not None else [])
    body += rr(b"\xc0" + bytes([inst_at]), 16, b"".join(bytes([len(x)]) + x for x in txt))
    body += rr(dn("scan.local"), 1, socket.inet_aton("10.0.2.2"))
    return hdr + body
q = run(b"", "q")
check("the mDNS question is PTR _uscan._tcp.local with the unicast bit", q[12:] == dn("_uscan._tcp.local") + b"\x00\x0c\x80\x01", q)
def m(b): return run(b, "m").decode().strip().split("|")
check("a scanner's answer gives its name, address, port and rs= root", m(mdns_answer(8080, "eSCL2")) == ["1", "Desk Scanner", "10.0.2.2", "8080", "eSCL2"], m(mdns_answer(8080, "eSCL2")))
check("no rs= means the root is eSCL", m(mdns_answer(80, None))[4] == "eSCL")
check("a cut-off answer stops inside it (under ASan)", m(mdns_answer(80)[:60])[0] == "0")
print("PASS: print.c's eSCL half on the host: settings, the job path, JPEG test and the _uscan answer", flush=True)

# ---- 2. the Terminal, against a stub scanner ----
posts, gets, mode = [], [], ["ok"]
warm = [1]
class Scanner(http.server.BaseHTTPRequestHandler):
    def reply(self, code, body=b"", ctype="text/plain", loc=None):
        self.send_response(code); self.send_header("Content-Type", ctype); self.send_header("Content-Length", str(len(body)))
        if loc: self.send_header("Location", loc)
        self.send_header("Connection", "close"); self.end_headers(); self.wfile.write(body)
    def do_POST(self):
        n = int(self.headers.get("Content-Length", "0")); posts.append((self.path, self.headers.get("Content-Type"), self.rfile.read(n)))
        if mode[0] == "busy": return self.reply(503)
        if mode[0] == "noloc": return self.reply(201)
        warm[0] = 1
        self.reply(201, loc="http://10.0.2.2:%d/eSCL/ScanJobs/job%d" % (port, len(posts)))
    def do_GET(self):
        gets.append(self.path)
        if not self.path.endswith("/NextDocument"): return self.reply(404)
        if warm[0]: warm[0] = 0; return self.reply(503)   # the lamp warms up once per job
        if mode[0] == "html": return self.reply(200, b"<html>not a page</html>", "text/html")
        self.reply(200, JPEG, "image/jpeg")
    def log_message(self, *a): pass
stub = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Scanner); port = stub.server_address[1]
threading.Thread(target=stub.serve_forever, daemon=True).start()
dead = socket.socket(); dead.bind(("127.0.0.1", 0)); dead_port = dead.getsockname()[1]; dead.close()
mdns = None
try:
    mdns = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
    mdns.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    if hasattr(socket, "SO_REUSEPORT"): mdns.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEPORT, 1)
    mdns.bind(("", 5353))
    mdns.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP, struct.pack("4s4s", socket.inet_aton("224.0.0.251"), socket.inet_aton("0.0.0.0")))
    def answer():
        while True:
            try: d, a = mdns.recvfrom(2000)
            except OSError: return
            if d[:2] == b"\x13\x37" and not d[2] & 0x80 and b"_uscan" in d: mdns.sendto(mdns_answer(port), a)
    threading.Thread(target=answer, daemon=True).start()
except OSError as e:
    print("  (no mDNS here, %s: the discovery step is skipped)" % e); mdns = None

b = subprocess.run(["make", "-s", "-C", str(arch), "kernel8.elf"], capture_output=True, text=True, timeout=900)
if b.returncode: print("FAIL: arch/arm64 does not build\n" + (b.stdout + b.stderr)[-1500:]); sys.exit(1)
card = str(tmp / "card.img"); build_card(card, False)
MT = dict(os.environ, MTOOLS_SKIP_CHECK="1")
def mt(*a): return subprocess.run([a[0], "-i", f"{card}@@{PART * 512}", *a[1:]], capture_output=True, env=MT)
log, sock = str(tmp / "uart"), str(tmp / "qmp")
q = subprocess.Popen(["qemu-system-aarch64", "-machine", "virt", "-cpu", "cortex-a72", "-m", "256", "-global", "virtio-mmio.force-legacy=false",
                      "-device", "ramfb", "-device", "virtio-keyboard-device", "-display", "none",
                      "-drive", "if=none,id=card,file=%s,format=raw" % card, "-device", "virtio-blk-device,drive=card",
                      "-netdev", "user,id=n", "-device", "virtio-net-device,netdev=n",
                      "-serial", "file:" + log, "-qmp", "unix:%s,server,nowait" % sock, "-kernel", str(arch / "kernel8.elf")],
                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
def uart(): return open(log, errors="replace").read() if os.path.exists(log) else ""
def said(): return "\n".join(l for l in uart().splitlines() if l.startswith(("scan", "photos", "net", "tcp", "http")))[-600:]
def wait_for(text, secs, count=1):
    end = time.time() + secs
    while time.time() < end:
        if uart().count(text) >= count: return True
        if q.poll() is not None: return False
        time.sleep(0.1)
    return False
f = None
try:
    if not wait_for("M2 input ready", 40) or not wait_for("net dhcp 10.0.2.15", 30): raise SystemExit("FAIL: no boot or no DHCP: " + uart()[-600:])
    s = socket.socket(socket.AF_UNIX); s.settimeout(30); s.connect(sock); f = s.makefile("rw"); f.readline()
    def qmp(c, **a):
        f.write(json.dumps({"execute": c, **({"arguments": a} if a else {})}) + "\n"); f.flush()
        while "return" not in (r := json.loads(f.readline())): assert "error" not in r, r
    qmp("qmp_capabilities")
    def key(*names): qmp("send-key", keys=[{"type": "qcode", "data": k} for k in names], **{"hold-time": 50}); time.sleep(0.15)
    def typ(t):
        for ch in t:
            if ch == ":": key("shift", "semicolon")
            elif ch.isupper(): key("shift", ch.lower())
            else: key({" ": "spc", ".": "dot"}.get(ch, ch))
    key("f2"); check("F2 opens the Terminal", wait_for("terminal open", 10), uart()[-300:]); time.sleep(0.4)
    typ("scan 10.0.2.2:%d" % port); key("ret")
    kb = (len(JPEG) + 1023) // 1024
    check("a scan through a warm-up lands as PHOTOS/SCAN0001.JPG", wait_for("scan: saved PHOTOS/SCAN0001.JPG, %d KB" % kb, 40), said())
    if posts:
        path, ctype, body = posts[0]; g = settings_ok(body)
        check("the stub saw POST /eSCL/ScanJobs as text/xml with the settings", path == "/eSCL/ScanJobs" and ctype == "text/xml" and isinstance(g, dict) and g.get("DocumentFormat") == "image/jpeg", (path, ctype, g))
    check("it asked the job's NextDocument again after the 503", gets[-2:] == ["/eSCL/ScanJobs/job1/NextDocument"] * 2, gets)
    typ("scan 10.0.2.2:%d" % port); key("ret")
    check("the next scan is SCAN0002.JPG", wait_for("scan: saved PHOTOS/SCAN0002.JPG", 40), said())
    for m_, want in (("busy", "scan: the scanner said no (HTTP 503)"), ("noloc", "scan: the scanner did not say where the scan is"), ("html", "scan: the scanner sent something that is not a JPEG")):
        mode[0] = m_; typ("scan 10.0.2.2:%d" % port); key("ret")
        check("%s: %s" % (m_, want.split(": ", 1)[1]), wait_for(want, 40), said())
    mode[0] = "ok"
    typ("scan 10.0.2.2:%d" % dead_port); key("ret")
    check("nothing listening: no scanner answered", wait_for("scan: no scanner answered at 10.0.2.2", 40), said())
    if mdns:
        typ("scan"); key("ret")
        check("plain scan finds the scanner over mDNS and saves SCAN0003.JPG", wait_for("scan: found Desk Scanner", 20) and wait_for("scan: saved PHOTOS/SCAN0003.JPG", 40), said())
    want_n = 3 if mdns else 2
    key("f1"); time.sleep(0.5); typ("photos"); time.sleep(0.3); key("ret")
    check("Photos lists the scans", wait_for("photos=list n=%d" % want_n, 20), said())
    check("no fault, no crash", "KERNEL CRASH" not in uart() and "oom" not in uart(), uart()[-300:])
finally:
    if f: f.close()
    q.terminate()
    try: q.wait(timeout=5)
    except subprocess.TimeoutExpired: q.kill(); q.wait()
    stub.shutdown()
r = mt("mtype", "::JT/PHOTOS/SCAN0001.JPG")
check("the card holds the stub's JPEG byte for byte", r.stdout == JPEG, (len(r.stdout), len(JPEG)))
check("the refused scans saved nothing", mt("mtype", "::JT/PHOTOS/SCAN%04d.JPG" % ((3 if mdns else 2) + 1)).returncode != 0)
if fails: print("FAIL: %d scan check(s) failed" % len(fails)); sys.exit(1)
print("PASS: scan on virt: eSCL ScanSettings, a 201 job, a warm-up 503, the JPEG into PHOTOS where Photos lists it; refusals and no scanner said plainly")
