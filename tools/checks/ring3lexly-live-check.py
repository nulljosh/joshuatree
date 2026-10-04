#!/usr/bin/env python3
"""Lexly pulls real courses through SYS_HTTP_GET and falls back to its Spanish deck.

Headless only (-display none). CI never touches the live site: the replies come from a
loopback stub, the way tools/checks/ring3bookrank-live-check.py serves the shelf. The stub's
good replies are not hand-written. They are what worker.js's own handleLexly() produces when
its upstream fetches return tools/checks/lexly-fixture.json, a captured slice of the real
lexly.heyitsmejosh.com catalog and course packs, so a change to the Worker's wire format changes
what the guest sees and this check notices.

Each scenario boots a fresh guest with `open=lexly` and a rtl8139 NIC (or none) and reads the
program's own serial lines:

  live      the stub serves the Worker's text. Expect the course picker ("courses 3"), Down and
            Up move the selection (serial and pixels), Enter opens Spanish ("course 7 spanish",
            the real first question), every question is answered (one miss on purpose), the
            score screen says how many, a key returns to the picker, a second course opens and
            Esc goes back to the picker, and Esc in the picker closes with exit 0.
  coursefail the list is fine, one course answers 500. Expect the picker to stay (no "course"
            line), then a different course to open.
  hostile   a list with bad ids, 400-character names, control and high bytes, a row with too many
            pipes, 100 rows: expect only the usable rows up to the cap of 80, the picker to scroll
            to row 12, and no fault.
  hostileq  a course whose questions have bad answer indexes, empty fields, repeated choices,
            400-character fields, high bytes and 40 rows: expect only the usable rows up to the
            cap of 24, every question line printable and bounded, no fault.
  html / short / http500 / oversize / offline  the list is junk, has no usable row, is a 500, is a
            12 KB body, or there is no NIC at all. Expect "samples 30" and the Spanish deck:
            option row undecided, "1" is right on round 0, Esc closes.

Discriminating: make the guest never adopt the live list (proved by hand, the live scenario fails
several ways), accept an answer index above 3, or leave the course list on the live buffer after a
failed parse, and a scenario here fails.

Usage: tools/checks/ring3lexly-live-check.py [scenario ...]  (from the repo root, after make kernel.elf)
"""
import http.server, json, os, re, socket, subprocess, sys, threading, time
from PIL import Image
from freeport import free_port

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
os.chdir(ROOT)

FB = 0xfd000000; W, H = 1920, 1080
SCALE = 2
VIEW_X, VIEW_Y = 78, 72   # gui_launch_from_dock: viewport at (x+8, y+32)
LIST_TOP, ITEM_H = 40, 26
VISIBLE_ROWS = (345 - LIST_TOP - 40) // ITEM_H   # 10: the window is 804x345
SEL_COLOR = (0xE2, 0xD8, 0xCC)
OPT_COLOR = (0xF1, 0xED, 0xE7)
RIGHT_COLOR = (0xCF, 0xE8, 0xD2)
OPT_Y0, OPT_H = 124, 34

NODE = r"""
import fs from 'node:fs';
import {handleLexly} from './worker.js';
const fx = JSON.parse(fs.readFileSync('tools/checks/lexly-fixture.json', 'utf8'));
const asked = [];
globalThis.fetch = async (url) => {
  const u = String(url); asked.push(u);
  const m = u.match(/^https:\/\/lexly\.heyitsmejosh\.com\/content\/courses\/([a-z0-9_]+)\.json$/);
  if (m && fx.packs[m[1]]) return Response.json(fx.packs[m[1]]);
  if (u === 'https://lexly.heyitsmejosh.com/content/catalog.json') return Response.json(fx.catalog);
  return new Response('no', {status: 404});
};
const out = {};
for (const c of [null, 'spanish', 'python', 'dsa']) {
  const res = await handleLexly(new URL('https://joshuatree.heyitsmejosh.com/api/lexly' + (c ? '?c=' + c : '')));
  if (res.status !== 200) { console.error('worker answered ' + res.status + ' for ' + c); process.exit(1); }
  out[c || ''] = await res.text();
}
if (!asked.every(u => u.startsWith('https://lexly.heyitsmejosh.com/content/'))) { console.error('worker fetched something else: ' + asked); process.exit(1); }
process.stdout.write(JSON.stringify(out));
"""
r = subprocess.run(["node", "--input-type=module", "-e", NODE], capture_output=True, text=True)
if r.returncode:
    print("FAIL: worker.js handleLexly did not produce the wires:\n" + r.stderr); sys.exit(1)
WIRES = json.loads(r.stdout)
COURSES = [l.split("|") for l in WIRES[""].splitlines()[1:]]
assert WIRES[""].splitlines()[0] == str(len(COURSES)) == "3", "course wire shape"
assert [c[0] for c in COURSES] == ["spanish", "python", "dsa"], COURSES


def questions(wire):
    rows = [l.split("|") for l in wire.splitlines()[1:]]
    return [(int(r[0]), r[1], r[2:]) for r in rows]


SPANISH = questions(WIRES["spanish"])
PYTHON = questions(WIRES["python"])
assert len(SPANISH) == 7 and len(PYTHON) == 6

HOSTILE_COURSES = ["99999999999"]
HOSTILE_COURSES.append("bad id!|Space in the id|x")
HOSTILE_COURSES.append("UPPER|Capital id|x")
HOSTILE_COURSES.append("ok_1|" + "N" * 400 + "|" + "c" * 400)
HOSTILE_COURSES.append("noname||x")
HOSTILE_COURSES.append("two|fields")
HOSTILE_COURSES.append("p|too|many|pipes")
HOSTILE_COURSES.append("ctl|Na\x01\x7f\xffme|cat")
HOSTILE_COURSES += [f"c{i}|Course {i}|cat" for i in range(1, 100)]
HOSTILE_LIST = ("\n".join(HOSTILE_COURSES) + "\n").encode("latin-1")

HQ = ["9999999"]
HQ.append("7|Answer seven|a|b|c|d")
HQ.append("1x|Answer one x|a|b|c|d")
HQ.append("-1|Negative|a|b|c|d")
HQ.append("0||a|b|c|d")
HQ.append("0|Empty choice|a||c|d")
HQ.append("0|Repeated choices|a|a|c|d")
HQ.append("0|Too few|a|b")
HQ.append("0|Too many|a|b|c|d|e")
HQ.append("2|T\x01\x7f\xffitle " + "q" * 400 + "|" + "a" * 400 + "|" + "b" * 400 + "|ok|" + "d" * 400)
HQ += [f"{i % 4}|Question {i}|alpha {i}|beta {i}|gamma {i}|delta {i}" for i in range(1, 40)]
HOSTILE_Q = ("\n".join(HQ) + "\n").encode("latin-1")

LIST = WIRES[""].encode()
SCENARIOS = {
    "live": {"/api/lexly": (200, LIST), "/api/lexly?c=spanish": (200, WIRES["spanish"].encode()),
             "/api/lexly?c=python": (200, WIRES["python"].encode()), "/api/lexly?c=dsa": (200, WIRES["dsa"].encode())},
    "coursefail": {"/api/lexly": (200, LIST), "/api/lexly?c=spanish": (500, b"nope"),
                   "/api/lexly?c=python": (200, WIRES["python"].encode())},
    "hostile": {"/api/lexly": (200, HOSTILE_LIST)},
    "hostileq": {"/api/lexly": (200, LIST), "/api/lexly?c=spanish": (200, HOSTILE_Q)},
    "html": {"/api/lexly": (200, b"<!doctype html><html><body><h1>Just a moment...</h1></body></html>")},
    "short": {"/api/lexly": (200, b"3\nspanish|Spanish\nonly\n")},
    "http500": {"/api/lexly": (500, b"nope")},
    "oversize": {"/api/lexly": (200, b"40\n" + b"".join(f"c{i}|Course {i}|".encode() + b"k" * 300 + b"\n" for i in range(60))[:12000])},
    "offline": None,
}

current = {"routes": {}, "paths": []}
class Stub(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def do_GET(self):
        current["paths"].append(self.path)
        st, body = current["routes"].get(self.path, (404, b""))
        self.send_response(st); self.send_header("Content-Type", "text/plain")
        self.send_header("Content-Length", str(len(body))); self.end_headers(); self.wfile.write(body)
srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Stub); srv.daemon_threads = True
threading.Thread(target=srv.serve_forever, daemon=True).start()
STUB_PORT = srv.server_address[1]

subprocess.run(["make", "-s", "kernel.elf"], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
fails = []


def run(name):
    scen = SCENARIOS[name]
    log = f"/tmp/jt-lexly-live-{name}.log"; dump = f"/tmp/jt-lexly-live-{name}.raw"
    for f in (log, dump):
        try: os.remove(f)
        except FileNotFoundError: pass
    current["routes"] = scen or {}
    current["paths"].clear()
    port = free_port()
    args = ["qemu-system-i386", "-kernel", "kernel.elf", "-display", "none", "-vga", "std", "-no-reboot",
            "-qmp", f"tcp:127.0.0.1:{port},server,nowait", "-serial", "file:" + log]
    cmdline = "open=lexly"
    if scen is not None:
        args += ["-net", "nic,model=rtl8139", "-net", "user"]
        cmdline += f" facehost=10.0.2.2:{STUB_PORT}"
    q = subprocess.Popen(args + ["-append", cmdline], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    def serial():
        try: return open(log, "rb").read().decode("latin-1")
        except OSError: return ""
    def wait(needle, secs):
        for _ in range(int(secs * 10)):
            if needle in serial(): return True
            time.sleep(0.1)
        return False
    def bad(msg): fails.append(f"[{name}] {msg}")
    def count(prefix): return len(re.findall(re.escape(prefix), serial()))
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
        def key(q_, pause=0.3):
            r = cmd({"execute": "send-key", "arguments": {"keys": [{"type": "qcode", "data": q_}]}})
            if "error" in r: raise SystemExit(f"FAIL: QMP rejected send-key {q_}: {r['error']}")
            time.sleep(pause)
        def frame():
            cmd({"execute": "pmemsave", "arguments": {"val": FB, "size": W * H * 4, "filename": dump}})
            return Image.frombytes("RGBA", (W, H), open(dump, "rb").read(), "raw", "BGRA").convert("RGB")
        def pix(x, y, img): return img.getpixel(((VIEW_X + x) * SCALE + 1, (VIEW_Y + y) * SCALE + 1))
        def near(p, c, tol=12): return max(abs(p[i] - c[i]) for i in range(3)) <= tol
        def row_color(k, img): return pix(22, LIST_TOP + k * ITEM_H + 2, img)

        if not wait("lexly: ring-3 window 804x345", 60): bad("Lexly never opened its window"); return
        if not (wait("lexly: courses ", 30) or wait("lexly: samples ", 5)): bad("the program never said courses or samples"); return
        time.sleep(0.5)
        s = serial()
        lines = [x.rstrip("\r") for x in re.findall(r"lexly: [^\n]*", s)]
        fetch = re.search(r"lexly: fetch (-?\d+)", s)
        print(f"{name}: fetch {fetch and fetch.group(1)}, {[l for l in lines if l.startswith(('lexly: courses', 'lexly: samples'))][:1]}")
        if "exception: ring-0" in s or "panic in" in s or "ring3app: BUG" in s or "exception: ring-3" in s:
            bad("a fault in the kernel or in the program")

        def printable_ok():
            for l in re.findall(r"lexly: [^\n]*", serial()):
                l = l.rstrip("\r")
                if any(ord(c) < 0x20 or ord(c) > 0x7e for c in l): bad(f"a serial line holds non-printable bytes: {l!r}")
                if len(l) > 130: bad(f"a serial line is not bounded: {len(l)} bytes")

        def closes():
            key("esc")
            if not wait("lexly: closed", 5): bad("Esc did not close the program")
            if not wait("LEXLY.BIN exited 0", 5): bad("the program did not exit 0")

        if name == "live":
            if not re.search(r"lexly: courses 3\b", s): bad(f"expected 'courses 3', got {[l for l in lines if 'courses' in l][:1]}")
            if "/api/lexly" not in current["paths"]: bad("the stub never saw GET /api/lexly")
            for _ in range(50):   # the first present can trail the serial line on a slow runner
                img = frame()
                if near(row_color(0, img), SEL_COLOR): break
                time.sleep(0.2)
            if not near(row_color(0, img), SEL_COLOR): bad(f"row 1 is not drawn selected: {row_color(0, img)}")
            if not near(row_color(1, img), OPT_COLOR): bad(f"row 2 is not drawn plain: {row_color(1, img)}")
            key("down")
            if not wait("lexly: sel 2 " + COURSES[1][1], 5): bad("Down did not select the real second course")
            img = frame()
            if not near(row_color(1, img), SEL_COLOR) or not near(row_color(0, img), OPT_COLOR): bad("the selection did not move on screen")
            key("down"); key("up"); key("up")
            if not wait("lexly: sel 1 " + COURSES[0][1], 5): bad("Up did not come back to the first course")
            key("ret")
            if not wait(f"lexly: course {len(SPANISH)} spanish", 8): bad("Enter did not load the Spanish course"); return
            if "/api/lexly?c=spanish" not in current["paths"]: bad("the stub never saw GET /api/lexly?c=spanish")
            score = 0
            for i, (ans, qtext, choices) in enumerate(SPANISH):
                if not wait(f"lexly: q {i + 1} {qtext}", 5): bad(f"question {i + 1} ({qtext!r}) was not shown"); break
                pick = ans if i != 1 else (ans + 1) % 4   # miss question 2 on purpose
                key(str(pick + 1))
                verdict = "right" if pick == ans else "miss"
                if not wait(f"lexly: pick {pick + 1} {verdict}", 5): bad(f"question {i + 1}: expected 'pick {pick + 1} {verdict}'")
                if verdict == "right": score += 1
                if i == 0:
                    img = frame()
                    if not near(pix(30, OPT_Y0 + ans * OPT_H + 8, img), RIGHT_COLOR): bad(f"the right answer did not turn green: {pix(30, OPT_Y0 + ans * OPT_H + 8, img)}")
                key("a")   # any key: next
            if not wait(f"lexly: done {score}", 8): bad(f"the score screen did not say 'done {score}'")
            n_courses = count("lexly: courses ")
            key("a")
            if not wait(f"lexly: courses 3", 5) or count("lexly: courses ") != n_courses + 1: bad("a key on the score screen did not return to the picker")
            key("down"); key("ret")
            if not wait(f"lexly: course {len(PYTHON)} python", 8): bad("the second course (Python) did not load")
            if not wait(f"lexly: q 1 {PYTHON[0][1]}", 5): bad("Python's first question was not shown")
            n_courses = count("lexly: courses ")
            key("esc")
            if not wait("lexly: courses 3", 5) or count("lexly: courses ") != n_courses + 1: bad("Esc in the drill did not return to the picker")
            closes()
        elif name == "coursefail":
            if not re.search(r"lexly: courses 3\b", s): bad("the picker did not open")
            key("ret")
            if not wait("lexly: fetch -500", 8): bad("the guest never asked for the failing course")
            time.sleep(1.0)
            if "lexly: course " in serial().replace("lexly: courses", ""): bad("a course opened although its reply was a 500")
            key("down"); key("ret")
            if not wait(f"lexly: course {len(PYTHON)} python", 8): bad("the picker did not recover and open the next course")
            key("esc")
            if not wait("lexly: courses 3", 5): bad("Esc did not return to the picker")
            closes()
        elif name == "hostile":
            if not re.search(r"lexly: courses 80\b", s): bad(f"expected the 80-row cap, got {[l for l in lines if 'courses' in l][:1]}")
            for _ in range(11): key("down", 0.25)
            if not wait("lexly: sel 12 ", 10): bad("could not walk the hostile rows to row 12")
            time.sleep(0.4)
            img = frame()
            last = row_color(VISIBLE_ROWS - 1, img)
            print(f"hostile: last visible row after scrolling to row 12: {last}")
            if not near(last, SEL_COLOR): bad(f"row 12 is selected but the list did not scroll to show it (last visible row {last})")
            printable_ok()
            closes()
        elif name == "hostileq":
            key("ret")
            m = None
            for _ in range(80):
                m = re.search(r"lexly: course (\d+) spanish", serial())
                if m: break
                time.sleep(0.1)
            if not m: bad("the usable hostile rows were not kept"); return
            if int(m.group(1)) != 24: bad(f"expected the 24-row cap, got {m.group(1)}")
            if not wait("lexly: q 1 T", 5): bad("the first hostile question was not shown")
            key("1"); key("a")
            if not wait("lexly: q 2 Question 1", 5): bad("the second kept row was not the first clean one (bad rows leaked in)")
            printable_ok()
            key("esc"); closes()
        else:
            if not re.search(r"lexly: samples 30\b", s): bad(f"expected the Spanish deck, got {[l for l in lines if 'samples' in l or 'courses' in l][:1]}")
            if "lexly: courses " in s: bad("a junk reply opened the picker")
            want_fetch = "-19" if name == "offline" else None
            if want_fetch and (fetch is None or fetch.group(1) != want_fetch): bad(f"fetch should be {want_fetch} with no NIC, got {fetch and fetch.group(1)}")
            if name != "offline" and "/api/lexly" not in current["paths"]: bad("the guest never asked the stub for /api/lexly")
            if any(p.startswith("/api/lexly?c=") for p in current["paths"]): bad("the guest asked for a course although the list was unusable")
            img = frame()
            if not near(pix(30, OPT_Y0 + 10, img), OPT_COLOR): bad(f"the Spanish option row is not drawn: {pix(30, OPT_Y0 + 10, img)}")
            key("1")
            if not wait("lexly: pick 1 right", 5): bad("the Spanish deck no longer scores round 0 slot 1 as right")
            closes()
    finally:
        try: cmd({"execute": "quit"})
        except Exception: pass
        q.terminate()
        try: q.wait(5)
        except subprocess.TimeoutExpired: q.kill()
        if fails and any(x.startswith(f"[{name}]") for x in fails):
            print(f"--- {name} serial tail ---"); print(serial()[-1500:])


for name in (sys.argv[1:] or SCENARIOS):   # optional: scenario names, to rerun one
    run(name)

if fails:
    print("FAIL:")
    for x in fails: print("  - " + x)
    sys.exit(1)
print("PASS: Lexly lists the real courses from the Worker's text, drills a chosen one with a score and goes back to the picker, keeps only the usable rows of a hostile reply, and falls back to the Spanish deck for junk, a 500, an oversize body and no NIC")
