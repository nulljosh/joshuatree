#!/usr/bin/env python3
"""Bookrank pulls the real shelf through SYS_HTTP_GET and falls back to its samples.

Headless only (-display none). CI never touches the live site: the reply comes from a
loopback stub, the way tools/checks/httpstress-check.py serves Samantha's face frames. The
stub's good reply is not hand-written. It is what worker.js's own handleBooks() produces when
its upstream fetch returns tools/checks/bookrank-fixture.json, a captured answer of the real
bookrank.heyitsmejosh.com/api/books, so a change to the Worker's wire format changes what the
guest sees and this check notices.

Each scenario boots a fresh guest with `open=bookr` and a rtl8139 NIC (or none) and reads the
program's own serial lines:

  live      the stub serves the Worker's text. Expect "bookrank: live 12", the first and
            second real titles shown, Down x11 to scroll to row 12 ("shown God Is Not
            Great"), and the last visible list row drawn selected (the list scrolls).
  html      the stub answers 200 with an HTML page. Expect the ten samples.
  short     the stub answers 200 with rows that have too few fields. Expect the samples.
  http500   the stub answers 500. Expect the samples.
  oversize  the stub answers a 12 KB valid-looking body. Expect the samples, no fault.
  hostile   control bytes, high bytes, 400-character fields, 30 rows, a row with no title.
            Expect only the 12 usable rows, every shown line printable ASCII, no fault.
  offline   no NIC at all. Expect "fetch -19" and the samples, as before.

Discriminating: make the guest never adopt the live rows (proved by hand, the live scenario
fails four ways), change a field order in the Worker's wire, accept any first line, or leave
br_rows on the live buffer after a failed parse, and a scenario here fails.

Usage: tools/checks/ring3bookrank-live-check.py   (from the repo root, after make kernel.elf)
"""
import http.server, json, os, re, socket, subprocess, sys, threading, time
from PIL import Image
from freeport import free_port

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
os.chdir(ROOT)

FB = 0xfd000000; W, H = 1920, 1080
SCALE = 2
VIEW_X, VIEW_Y = 78, 72   # gui_launch_from_dock: viewport at (x+8, y+32)
BR_LIST_X, BR_LIST_TOP, BR_ITEM_H = 20, 40, 28
SEL_COLOR = (0xE2, 0xD8, 0xCC)
VISIBLE_ROWS = (345 - BR_LIST_TOP - 40) // BR_ITEM_H   # 9: the window is 804x345

# The Worker's real output for the captured upstream answer.
NODE = r"""
import fs from 'node:fs';
import {handleBooks} from './worker.js';
const fixture = JSON.parse(fs.readFileSync('tools/checks/bookrank-fixture.json', 'utf8'));
let asked = null;
globalThis.fetch = async (url) => { asked = String(url); return Response.json(fixture); };
const res = await handleBooks();
if (res.status !== 200 || !/^https:\/\/bookrank\.heyitsmejosh\.com\/api\/books\?/.test(asked)) { console.error('worker did not fetch the real API: ' + asked + ' ' + res.status); process.exit(1); }
process.stdout.write(await res.text());
"""
r = subprocess.run(["node", "--input-type=module", "-e", NODE], capture_output=True, text=True)
if r.returncode:
    print("FAIL: worker.js handleBooks did not produce the wire:\n" + r.stderr); sys.exit(1)
GOOD = r.stdout
fixture = json.load(open("tools/checks/bookrank-fixture.json"))
TITLES = [b["title"] for b in fixture["results"]]
assert GOOD.splitlines()[0] == str(fixture["total"]) and len(GOOD.splitlines()) == 13, "wire shape"

HOSTILE_ROWS = ["99999999999"]
HOSTILE_ROWS.append("7|9999999|" + "r" * 400 + "|" + "b" * 400 + "|T\x01\x7f\xffitle|" + "a" * 400 + "|" + "n" * 400)
HOSTILE_ROWS.append("1|400|x|y||No Title Here|a|n")                  # empty title: dropped
HOSTILE_ROWS.append("only|three|fields")                              # too few fields: dropped
HOSTILE_ROWS += [f"{i}|{300 + i}|r|b|Book {i}|Author {i}|Note {i} \x02\x03 end" for i in range(1, 30)]
HOSTILE = ("\n".join(HOSTILE_ROWS) + "\n").encode("latin-1")

SCENARIOS = {
    "live": (200, GOOD.encode()),
    "html": (200, b"<!doctype html><html><body><h1>Just a moment...</h1></body></html>"),
    "short": (200, b"110\n1|438|x\n2|1|2|3\n"),
    "http500": (500, b"nope"),
    "oversize": (200, b"110\n" + b"".join(f"{i}|400|r|b|Title {i}|Author|".encode() + b"n" * 300 + b"\n" for i in range(30))[:12000]),
    "hostile": (200, HOSTILE),
    "offline": None,
}

current = {"status": 200, "body": b"", "paths": []}
class Stub(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def do_GET(self):
        current["paths"].append(self.path)
        body = current["body"] if self.path == "/api/books" else b""
        st = current["status"] if self.path == "/api/books" else 404
        self.send_response(st); self.send_header("Content-Type", "text/plain")
        self.send_header("Content-Length", str(len(body))); self.end_headers(); self.wfile.write(body)
srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Stub); srv.daemon_threads = True
threading.Thread(target=srv.serve_forever, daemon=True).start()
STUB_PORT = srv.server_address[1]

subprocess.run(["make", "-s", "kernel.elf"], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
fails = []

def run(name):
    scen = SCENARIOS[name]
    log = f"/tmp/jt-bookrank-live-{name}.log"; dump = f"/tmp/jt-bookrank-live-{name}.raw"
    for f in (log, dump):
        try: os.remove(f)
        except FileNotFoundError: pass
    if scen: current["status"], current["body"] = scen
    current["paths"].clear()
    port = free_port()
    args = ["qemu-system-i386", "-kernel", "kernel.elf", "-display", "none", "-vga", "std", "-no-reboot",
            "-qmp", f"tcp:127.0.0.1:{port},server,nowait", "-serial", "file:" + log]
    cmdline = "open=bookr"
    if scen:
        args += ["-net", "nic,model=rtl8139", "-net", "user"]
        cmdline += f" facehost=10.0.2.2:{STUB_PORT}"
    q = subprocess.Popen(args + ["-append", cmdline], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    def serial():
        try: return open(log, "rb").read().decode("latin-1")
        except OSError: return ""
    # Poll serial against a wall-clock deadline, never a fixed sleep: a loaded CI
    # runner can take many seconds to deliver a key and run the guest's redraw.
    def poll(cond, secs=30):
        end = time.time() + secs
        while True:
            if cond(): return True
            if time.time() >= end: return False
            time.sleep(0.1)
    def wait(needle, secs=30): return poll(lambda: needle in serial(), secs)
    def wait_line(text, secs=30):   # a whole line, so "sel 1" does not match "sel 12"
        return poll(lambda: re.search(re.escape(text) + r"\r?$", serial(), re.M), secs)
    def down_to(n):                  # Down once, then wait for the guest to select row n
        key("down"); return wait_line(f"bookrank: sel {n}")
    def bad(msg): fails.append(f"[{name}] {msg}")
    try:
        sock = None
        for _ in range(50):
            time.sleep(0.2)
            try: sock = socket.create_connection(("127.0.0.1", port)); break
            except OSError: pass
        if sock is None: bad("QEMU's QMP socket never came up"); return
        f = sock.makefile("rw")
        def cmd(o):
            f.write(json.dumps(o) + "\n"); f.flush()
            while True:
                r = json.loads(f.readline())
                if "return" in r or "error" in r: return r
        f.readline(); cmd({"execute": "qmp_capabilities"})
        def key(q_):
            r = cmd({"execute": "send-key", "arguments": {"keys": [{"type": "qcode", "data": q_}]}})
            if "error" in r: raise SystemExit(f"FAIL: QMP rejected send-key {q_}: {r['error']}")
        def frame():
            cmd({"execute": "pmemsave", "arguments": {"val": FB, "size": W * H * 4, "filename": dump}})
            return Image.frombytes("RGBA", (W, H), open(dump, "rb").read(), "raw", "BGRA").convert("RGB")
        def near(p, c, tol=12): return max(abs(p[i] - c[i]) for i in range(3)) <= tol

        if not wait("bookrank: ring-3 window 804x345", 60): bad("Bookrank never opened its window"); return
        if not poll(lambda: re.search(r"bookrank: (live|samples) \d+\r?\n.*bookrank: shown ", serial())):
            bad("the program never said live or samples and its first shown row"); return
        s = serial()
        fetch = re.search(r"bookrank: fetch (-?\d+)", s)
        verdict = re.search(r"bookrank: (live|samples) (\d+)", s)
        shown = [x.rstrip("\r") for x in re.findall(r"bookrank: shown ([^\n]*)", s)]
        print(f"{name}: fetch {fetch and fetch.group(1)}, {verdict and verdict.group(0)}, first shown {shown[:1]}")
        if "exception: ring-0" in s or "panic in" in s or "ring3app: BUG" in s or "exception: ring-3" in s:
            bad("a fault in the kernel or in the program")
        for line in shown:
            if any(ord(c) < 0x20 or ord(c) > 0x7e for c in line): bad(f"a shown line holds non-printable bytes: {line!r}")

        if name == "live":
            if not verdict or verdict.group(1) != "live" or verdict.group(2) != "12": bad(f"expected 'live 12', got {verdict and verdict.group(0)}")
            if not shown or shown[0] != TITLES[0]: bad(f"first row is not the real #1 ({TITLES[0]!r}): {shown[:1]}")
            if "/api/books" not in current["paths"]: bad("the stub never saw GET /api/books")
            if not down_to(2): bad("no 'bookrank: sel 2' after Down")
            if not wait_line(f"bookrank: shown {TITLES[1]}"): bad(f"Down did not show the real #2 ({TITLES[1]!r})")
            for n in range(3, 13):
                if not down_to(n): bad(f"no 'bookrank: sel {n}' after Down"); break
            if not wait_line(f"bookrank: shown {TITLES[11]}"): bad(f"Down x11 did not reach row 12 ({TITLES[11]!r})")
            px = ((VIEW_X + BR_LIST_X + 30) * SCALE + 1, (VIEW_Y + BR_LIST_TOP + (VISIBLE_ROWS - 1) * BR_ITEM_H + 2) * SCALE + 1)
            last = [None]
            def scrolled():   # the redraw lands after the serial line, so poll the pixel too
                last[0] = frame().getpixel(px); return near(last[0], SEL_COLOR)
            ok = poll(scrolled, 15)
            print(f"live: last visible list row after scrolling to row 12: {last[0]}")
            if not ok: bad(f"row 12 is selected but the list did not scroll to show it (last visible row {last[0]})")
        elif name == "hostile":
            if not verdict or verdict.group(1) != "live": bad(f"the usable rows were not kept: {verdict and verdict.group(0)}")
            elif int(verdict.group(2)) != 12: bad(f"expected the 12-row cap, got {verdict.group(2)}")
            if shown and not shown[0].startswith("T"): bad(f"hostile first title not kept: {shown[:1]}")
            if shown and len(shown[0]) > 90: bad("a shown title was not bounded")
            for n in range(2, 13):
                if not down_to(n): bad(f"could not walk the hostile rows to row 12 (stuck before {n})"); break
        else:
            want_fetch = "-19" if name == "offline" else None
            if verdict is None or verdict.group(1) != "samples" or verdict.group(2) != "10": bad(f"expected the ten samples, got {verdict and verdict.group(0)}")
            if not shown or shown[0] != "Thinking, Fast and Slow": bad(f"the first sample is not drawn: {shown[:1]}")
            if want_fetch and (fetch is None or fetch.group(1) != want_fetch): bad(f"fetch should be {want_fetch} with no NIC, got {fetch and fetch.group(1)}")
            if name != "offline" and "/api/books" not in current["paths"]: bad("the guest never asked the stub for /api/books")
            key("down")
            if not wait_line("bookrank: shown Sapiens"): bad("selection no longer works on the samples")
        key("esc")
        if not wait("bookrank: closed"): bad("Esc did not close the program")
        if not wait("BOOKRANK.BIN exited 0"): bad("the program did not exit 0")
    finally:
        try: cmd({"execute": "quit"})
        except Exception: pass
        q.terminate()
        try: q.wait(5)
        except subprocess.TimeoutExpired: q.kill()
        if fails and any(x.startswith(f"[{name}]") for x in fails):
            print(f"--- {name} serial tail ---"); print(serial()[-1200:])

for name in (sys.argv[1:] or SCENARIOS):   # optional: scenario names, to rerun one
    run(name)

if fails:
    print("FAIL:")
    for x in fails: print("  - " + x)
    sys.exit(1)
print("PASS: Bookrank shows the real shelf from the Worker's text (scrolls to row 12), keeps only the usable rows of a hostile reply, and shows the ten samples for HTML, short rows, a 500, an oversize body and no NIC")
