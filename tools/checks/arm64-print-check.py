#!/usr/bin/env python3
"""Printing, step one (13.11.0: arch/arm64/print.c and cmd.c's `print FILE PRINTER`).

  1. Host, under address and undefined-behaviour sanitizers: pdf_make's PDF is well formed (every xref offset lands on
     its object, startxref on the xref, each /Length is its stream's real size), 130 lines make three pages of 60, a
     200-character line wraps to 90, 90 and 20, ( ) \\ are escaped and anything outside ASCII prints as ?. ipp_job's
     request is IPP 2.0 Print-Job with the six operation attributes and the PDF after them. ipp_reply keeps a job-id,
     reports a refusal's status, calls a non-IPP reply -1, and walks truncated and lying lengths without reading past
     the end.
  2. QEMU virt with a card and a network: in the Terminal, `print NOTE.TXT 10.0.2.2:PORT` sends the note to a stub
     printer on the host, which checks the request (POST /ipp/print, application/ipp, the attributes, the note's text in
     the PDF) and answers job 42: the Terminal says so. A printer that refuses gets its status in plain words, a port with
     nothing on it says no printer answered, and a missing file says so before anything is sent.
Skips (exit 0) without clang, ld.lld, mtools or qemu-system-aarch64.
"""
import http.server, json, os, re, shutil, socket, struct, subprocess, sys, threading, time
from pathlib import Path

root = Path(__file__).resolve().parents[2]
arch = root / "arch/arm64"
sys.path.insert(0, str(Path(__file__).resolve().parent))
from scratch import scratch_dir
from sdcard import PART, build_card
if not all(shutil.which(t) for t in ("clang", "ld.lld", "qemu-system-aarch64", "mmd", "mcopy")):
    print("SKIP: clang, ld.lld, qemu-system-aarch64 or mtools not installed"); sys.exit(0)
tmp = Path(scratch_dir("print"))
fails = []
def check(name, ok, detail=""):
    print(("  ok: " if ok else "  FAIL: ") + name + ("" if ok else "  " + repr(detail)[:600]), flush=True)
    if not ok: fails.append(name)

def pdf_ok(pdf):   # structure, as a reader would walk it
    if not pdf.startswith(b"%PDF-1.4\n") or not pdf.endswith(b"%%EOF\n"): return "header or trailer"
    sx = int(re.search(rb"startxref\n(\d+)\n%%EOF\n$", pdf).group(1))
    if not pdf[sx:].startswith(b"xref\n0 "): return "startxref"
    count = int(re.match(rb"xref\n0 (\d+)\n", pdf[sx:]).group(1))
    entries = re.findall(rb"(\d{10}) 00000 n \n", pdf[sx:])
    if len(entries) != count - 1: return "xref count"
    for k, off in enumerate(entries, 1):
        if not pdf[int(off):].startswith(b"%d 0 obj\n" % k): return "object %d offset" % k
    for m in re.finditer(rb"/Length (\d{10}) >>\nstream\n", pdf):
        end = m.end() + int(m.group(1))
        if not pdf[end:].startswith(b"endstream\n"): return "stream length at %d" % m.start()
    return None
def ipp_parse(b):
    ver, op, rid = b[0:2], struct.unpack(">H", b[2:4])[0], struct.unpack(">I", b[4:8])[0]
    i, attrs, group = 8, {}, None
    while True:
        tag = b[i]; i += 1
        if tag == 0x03: break
        if tag < 0x10: group = tag; continue
        nl = struct.unpack(">H", b[i:i + 2])[0]; name = b[i + 2:i + 2 + nl].decode(); i += 2 + nl
        vl = struct.unpack(">H", b[i:i + 2])[0]; val = b[i + 2:i + 2 + vl]; i += 2 + vl
        attrs[name] = (tag, val.decode(errors="replace"))
    return ver, op, rid, attrs, b[i:]
def reply(status, job=None, extra=b""):
    b = b"\x02\x00" + struct.pack(">H", status) + b"\x00\x00\x00\x01" + b"\x01"
    b += b"\x47\x00\x12attributes-charset\x00\x05utf-8"
    if job is not None: b += b"\x02" + b"\x21\x00\x06job-id\x00\x04" + struct.pack(">I", job)
    return b + extra + b"\x03"

# ---- 1. the builders on the host ----
harness = tmp / "h.c"
harness.write_text(r'''
#include <stdio.h>
#include <stdlib.h>
unsigned pdf_make(const char *text, unsigned len, char *out, unsigned cap);
unsigned ipp_job(const char *uri, const char *user, const char *job, const char *doc, unsigned doclen, unsigned char *out, unsigned cap);
int ipp_reply(const unsigned char *r, unsigned len, unsigned *job_id);
static unsigned char buf[1 << 20], req[1 << 20];
int main(int argc, char **argv) { (void)argc;
    FILE *f = fopen(argv[1], "rb"); char *t = malloc(1 << 20); size_t n = fread(t, 1, 1 << 20, f); fclose(f);
    if (argv[2][0] == 'p') { unsigned l = pdf_make(t, (unsigned)n, (char *)buf, sizeof buf); fwrite(buf, 1, l, stdout); }
    else if (argv[2][0] == 'j') { unsigned l = pdf_make(t, (unsigned)n, (char *)buf, sizeof buf); unsigned r = ipp_job("ipp://p:631/ipp/print", "ann", "NOTE.TXT", (char *)buf, l, req, sizeof req); fwrite(req, 1, r, stdout); }
    else { unsigned id = 7; int s = ipp_reply((unsigned char *)t, (unsigned)n, &id); printf("%d %u\n", s, id); }
    free(t); return 0;
}
''')
exe = tmp / "h"
r = subprocess.run(["clang", "-Wall", "-Wextra", "-Werror", "-DPRINT_HOST_TEST", "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                    str(harness), str(arch / "print.c"), "-o", str(exe)], capture_output=True, text=True)
if r.returncode: print("FAIL: print.c does not build on the host\n" + r.stderr[-1500:]); sys.exit(1)
def run(text, mode):
    p = tmp / "in"; p.write_bytes(text)
    out = subprocess.run([str(exe), str(p), mode], capture_output=True, timeout=20)
    if out.returncode: print("FAIL: the host harness crashed\n" + out.stderr.decode()[-1500:]); sys.exit(1)
    return out.stdout
note = b"Hello from Joshua Tree\nA (paren) and a back\\slash.\nCaf\xc3\xa9 costs \xe2\x82\xac3.\n"
pdf = run(note, "p")
check("the PDF is well formed (xref offsets, startxref, stream lengths)", pdf_ok(pdf) is None, pdf_ok(pdf))
check("one page for a short note", b"/Count 1 " in pdf, pdf[:400])
check("the text is there, ( ) and \\ escaped", b"(Hello from Joshua Tree) Tj" in pdf and b"(A \\(paren\\) and a back\\\\slash.) Tj" in pdf)
check("anything outside ASCII prints as ?", b"(Caf?? costs ???3.) Tj" in pdf)
long = b"".join(b"line %d\n" % i for i in range(130))
pdf3 = run(long, "p")
check("130 lines make three pages of 60", pdf_ok(pdf3) is None and b"/Count 3 " in pdf3 and pdf3.count(b"/Type /Page ") == 3, pdf_ok(pdf3))
wide = b"x" * 200 + b"\nend\n"
pdfw = run(wide, "p")
check("a 200-character line wraps to 90, 90 and 20", b"(" + b"x" * 90 + b") Tj" in pdfw and b"(" + b"x" * 20 + b") Tj" in pdfw and b"(end) Tj" in pdfw)
check("an empty file is still one well-formed page", pdf_ok(run(b"", "p")) is None)
ver, op, rid, attrs, doc = ipp_parse(run(note, "j"))
check("the request is IPP 2.0 Print-Job, request 1", ver == b"\x02\x00" and op == 2 and rid == 1, (ver, op, rid))
want = {"attributes-charset": (0x47, "utf-8"), "attributes-natural-language": (0x48, "en"), "printer-uri": (0x45, "ipp://p:631/ipp/print"),
        "requesting-user-name": (0x42, "ann"), "job-name": (0x42, "NOTE.TXT"), "document-format": (0x49, "application/pdf")}
check("the six operation attributes, tagged right", attrs == want, attrs)
check("the PDF follows the attributes, whole", doc == pdf)
def rep(b): s, i = run(b, "r").split(); return int(s), int(i)
check("a reply with job 42 reads as success, job 42", rep(reply(0, 42)) == (0, 42))
check("a refusal reports its status", rep(reply(0x0400)) == (0x0400, 0))
check("an HTML page is not a printer", rep(b"<html><body>no</body></html>")[0] == -1)
check("a reply too short to be IPP is not a printer", rep(b"\x02\x00\x00")[0] == -1)
lying = reply(0, 9)[:-1] + b"\x21\x00\x06job-id\xff\xff"   # a value length past the end
check("a lying length stops the walk inside the reply (under ASan)", rep(lying) == (0, 9))
check("a cut-off reply stops the walk inside the reply", rep(reply(0, 5)[:30])[0] == 0)
print("PASS: print.c on the host: PDF structure, pages, wrapping, escaping, the IPP request, and replies good, refused and hostile", flush=True)

# ---- 2. the Terminal, against stub printers ----
seen, stub_status = [], [0]
class Printer(http.server.BaseHTTPRequestHandler):
    def do_POST(self):
        n = int(self.headers.get("Content-Length", "0")); body = self.rfile.read(n)
        seen.append((self.path, self.headers.get("Content-Type"), body))
        out = reply(stub_status[0], 42 if stub_status[0] == 0 else None)
        self.send_response(200); self.send_header("Content-Type", "application/ipp"); self.send_header("Content-Length", str(len(out)))
        self.send_header("Connection", "close"); self.end_headers(); self.wfile.write(out)
    def log_message(self, *a): pass
stub = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Printer); port = stub.server_address[1]
threading.Thread(target=stub.serve_forever, daemon=True).start()
dead = socket.socket(); dead.bind(("127.0.0.1", 0)); dead_port = dead.getsockname()[1]; dead.close()   # nothing listens here

b = subprocess.run(["make", "-s", "-C", str(arch), "kernel8.elf"], capture_output=True, text=True, timeout=900)
if b.returncode: print("FAIL: arch/arm64 does not build\n" + (b.stdout + b.stderr)[-1500:]); sys.exit(1)
card = str(tmp / "card.img"); build_card(card, False)
MT = dict(os.environ, MTOOLS_SKIP_CHECK="1")
def mt(*a): return subprocess.run([a[0], "-i", f"{card}@@{PART * 512}", *a[1:]], capture_output=True, env=MT)
(tmp / "note").write_bytes(note)
if mt("mcopy", str(tmp / "note"), "::NOTE.TXT").returncode: print("FAIL: mtools could not write the note"); sys.exit(1)
log, sock = str(tmp / "uart"), str(tmp / "qmp")
q = subprocess.Popen(["qemu-system-aarch64", "-machine", "virt", "-cpu", "cortex-a72", "-m", "256", "-global", "virtio-mmio.force-legacy=false",
                      "-device", "ramfb", "-device", "virtio-keyboard-device", "-display", "none",
                      "-drive", "if=none,id=card,file=%s,format=raw" % card, "-device", "virtio-blk-device,drive=card",
                      "-netdev", "user,id=n", "-device", "virtio-net-device,netdev=n",
                      "-serial", "file:" + log, "-qmp", "unix:%s,server,nowait" % sock, "-kernel", str(arch / "kernel8.elf")],
                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
def uart(): return open(log, errors="replace").read() if os.path.exists(log) else ""
def said(): return "\n".join(l for l in uart().splitlines() if l.startswith(("print", "net", "tcp", "http")))[-600:]
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
    typ("print NOTE.TXT 10.0.2.2:%d" % port); key("ret")
    check("the note goes to the printer and the Terminal says job 42", wait_for("print: sent to 10.0.2.2, job 42", 30), said())
    if seen:
        path, ctype, body = seen[0]; ver, op, rid, attrs, doc = ipp_parse(body)
        check("the stub saw POST /ipp/print as application/ipp", path == "/ipp/print" and ctype == "application/ipp", (path, ctype))
        check("... a Print-Job for NOTE.TXT, a PDF with the note in it", op == 2 and attrs.get("job-name", (0, ""))[1] == "NOTE.TXT"
              and attrs.get("document-format", (0, ""))[1] == "application/pdf" and pdf_ok(doc) is None and b"(Hello from Joshua Tree) Tj" in doc, attrs)
    stub_status[0] = 0x0400
    typ("print NOTE.TXT 10.0.2.2:%d" % port); key("ret")
    check("a printer that refuses: its status in plain words", wait_for("print: the printer said no (0x0400)", 30), said())
    typ("print NOTE.TXT 10.0.2.2:%d" % dead_port); key("ret")
    check("nothing listening: no printer answered", wait_for("print: no printer answered at 10.0.2.2", 40), said())
    n0 = len(seen)
    typ("print NOPE.TXT 10.0.2.2:%d" % port); key("ret")
    check("a missing file says so, and nothing is sent", wait_for("print: no file NOPE.TXT", 15) and len(seen) == n0, uart()[-300:])
    check("no fault, no crash", "KERNEL CRASH" not in uart() and "oom" not in uart(), uart()[-300:])
finally:
    if f: f.close()
    q.terminate()
    try: q.wait(timeout=5)
    except subprocess.TimeoutExpired: q.kill(); q.wait()
    stub.shutdown()
if fails: print("FAIL: %d print check(s) failed" % len(fails)); sys.exit(1)
print("PASS: print FILE PRINTER on virt: a PDF as an IPP Print-Job, job 42 reported; a refusal, no printer and no file each said plainly")
