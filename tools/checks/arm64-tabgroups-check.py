#!/usr/bin/env python3
"""Tab groups in the ARM64 browser (12.0, browser.c group_command), as in Safari: named sets of tabs kept on the card.

A stub server on the host serves /one, /two and /three. Two boots of the virt build on one card (sdcard.py), at the Terminal:
  1. /one in tab 1, /two in tab 2: `tab group save work` -> "group work saved, 2 tabs". `tab new`, /three:
     `tab group save home` -> 3 tabs. `tab group save work` again rewrites work, it does not add to it. A bad name and an
     unknown group are refused. `tab group work` -> "group work, 2 tabs", /one is fetched again and `tabs` lists /one and /two.
     `tab group rm home`, then `tab groups` lists work alone. After QEMU stops, JT/TABGROUP.TXT (read with mtools) holds
     exactly two lines, "work<TAB>.../one" and "work<TAB>.../two".
  2. Second boot: `tab group work` brings both tabs back from the card.
Discriminating: make tg_drop keep the group's own lines and the second `tab group save work` leaves four work lines.
Skips (exit 0) when clang's aarch64 target, ld.lld, qemu-system-aarch64 or mtools is missing.
Usage: tools/checks/arm64-tabgroups-check.py   (from the repo root)
"""
import http.server, json, os, shutil, socket, subprocess, sys, threading, time
from pathlib import Path

root = Path(__file__).resolve().parents[2]
arch = root / "arch/arm64"
sys.path.insert(0, str(Path(__file__).resolve().parent))
from scratch import scratch_dir
from sdcard import PART, build_card
if not all(shutil.which(t) for t in ("clang", "ld.lld", "qemu-system-aarch64", "mtype")):
    print("SKIP: clang, ld.lld, qemu-system-aarch64 or mtools not installed"); sys.exit(0)
tmp = Path(scratch_dir("arm64-tabgroups"))

gets = []
class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        gets.append(self.path)
        name = self.path.strip("/").capitalize()
        body = ("<html><head><title>%s</title></head><body><p>This is page %s.</p></body></html>" % (name, name.lower())).encode()
        self.send_response(200); self.send_header("Content-Length", str(len(body))); self.end_headers(); self.wfile.write(body)
    def log_message(self, *a): pass
server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
port = server.server_address[1]
threading.Thread(target=server.serve_forever, daemon=True).start()
base = "http://10.0.2.2:%d" % port

fails = []
def check(name, ok, detail=""):
    print(("  ok: " if ok else "  FAIL: ") + name + ("" if ok else "  " + repr(detail)[-500:]))
    if not ok: fails.append(name)

card = tmp / "card.img"
build_card(card, False)
MT = dict(os.environ, MTOOLS_SKIP_CHECK="1")
def mt(*a): return subprocess.run([a[0], "-i", f"{card}@@{PART * 512}", *a[1:]], capture_output=True, env=MT)

subprocess.run(["make", "-C", str(arch), "clean"], capture_output=True, timeout=60)
r = subprocess.run(["make", "-C", str(arch), "kernel8.elf"], capture_output=True, text=True, timeout=600)
if r.returncode: print(r.stderr[-2000:]); print("FAIL: arch/arm64 does not build"); sys.exit(1)

def boot(steps):
    log, sock = tmp / "uart", tmp / "qmp"
    log.unlink(missing_ok=True)
    q = subprocess.Popen(["qemu-system-aarch64", "-machine", "virt", "-cpu", "cortex-a72", "-m", "256",
                          "-global", "virtio-mmio.force-legacy=false", "-device", "ramfb",
                          "-netdev", "user,id=n", "-device", "virtio-net-device,netdev=n",
                          "-drive", "if=none,id=card,file=%s,format=raw" % card, "-device", "virtio-blk-device,drive=card",
                          "-device", "virtio-keyboard-device", "-device", "virtio-tablet-device", "-display", "none",
                          "-serial", "file:" + str(log), "-qmp", "unix:%s,server,nowait" % sock,
                          "-kernel", str(arch / "kernel8.elf")], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        def uart(): return log.read_text(errors="replace") if log.exists() else ""
        def wait_for(text, secs, after=0):
            end = time.time() + secs
            while time.time() < end:
                if text in uart()[after:]: return True
                time.sleep(0.1)
            return False
        if not wait_for("M2 input ready", 40): check("the virt build boots", False, uart()[-600:]); return
        time.sleep(0.5)
        s = socket.socket(socket.AF_UNIX); s.settimeout(30); s.connect(str(sock)); f = s.makefile("rw"); f.readline()
        def command(name, args=None):
            f.write(json.dumps({"execute": name, **({"arguments": args} if args else {})}) + "\n"); f.flush()
            while True:
                reply = json.loads(f.readline())
                if "error" in reply: raise SystemExit("FAIL: QMP " + repr(reply))
                if "return" in reply: return reply["return"]
        def key(*names):
            command("send-key", {"keys": [{"type": "qcode", "data": k} for k in names], "hold-time": 50}); time.sleep(0.2)
        QCODE = {":": ("shift", "semicolon"), "/": ("slash",), ".": ("dot",), " ": ("spc",), "!": ("shift", "1")}
        def line(text):
            for ch in text: key(*QCODE.get(ch, (ch,)))
            key("ret")
        command("qmp_capabilities")
        check("DHCP leased an address", wait_for("net dhcp 10.0.2.15 gw 10.0.2.2", 30), uart()[-300:])
        key("f2")
        check("the Terminal opened", wait_for("terminal open", 10), uart()[-300:])
        def run(text, want, secs=10):
            n = len(uart()); line(text); ok = wait_for(want, secs, n); return ok, uart()[n:][-400:]
        steps(run, check)
    finally:
        q.terminate()
        try: q.wait(5)
        except subprocess.TimeoutExpired: q.kill()

def first(run, check):
    check("tab 1 loads page one", *run("browse %s/one" % base, "This is page one.", 20))
    run("tab new", "browser: tab 2 of 2")
    check("tab 2 loads page two", *run("browse %s/two" % base, "This is page two.", 20))
    check("tab group save work keeps both tabs", *run("tab group save work", "browser: group work saved, 2 tabs"))
    run("tab new", "browser: tab 3 of 3")
    check("tab 3 loads page three", *run("browse %s/three" % base, "This is page three.", 20))
    check("tab group save home keeps all three", *run("tab group save home", "browser: group home saved, 3 tabs"))
    run("tab close", "of 2")
    check("saving work again rewrites it", *run("tab group save work", "browser: group work saved, 2 tabs"))
    check("a bad name is refused", *run("tab group save no.good!", "browser: a group name is 1 to 12 of a-z, 0-9 and -"))
    check("an unknown group is refused", *run("tab group nope", "browser: no such group"))
    k = len(gets)
    ok, out = run("tab group work", "browser: group work, 2 tabs")
    time.sleep(2)
    check("tab group work swaps in its two tabs and fetches the first again", ok and "/one" in gets[k:], (gets[k:], out))
    check("tabs lists /one and /two", *run("tabs", "  2 %s/two" % base))
    check("tab group rm home drops it", *run("tab group rm home", "browser: group home removed"))
    ok, out = run("tab groups", "  work, 2 tabs")
    check("tab groups lists work alone", ok and "home" not in out, out)

boot(first)
data = mt("mtype", "::JT/TABGROUP.TXT").stdout.decode(errors="replace")
want = "work\t%s/one\nwork\t%s/two\n" % (base, base)
check("JT/TABGROUP.TXT holds work's two lines and nothing else", data == want, data)

def second(run, check):
    k = len(gets)
    ok, out = run("tab group work", "browser: group work, 2 tabs", 15)
    time.sleep(2)
    check("a second boot brings work back from the card", ok and "/one" in gets[k:], (gets[k:], out))
    check("with both its tabs", *run("tabs", "  2 %s/two" % base))
boot(second)

server.shutdown()
if fails: print("FAIL: " + "; ".join(fails)); sys.exit(1)
print("PASS: tab group save, open, list and rm; the groups live in JT/TABGROUP.TXT and come back on the next boot")
