#!/usr/bin/env python3
"""The bench (13.16.0): `bench` on the Pi lets the Mac run Terminal commands there (ask.c bench_tick, relay.py
/api/bench, tools/pi-bench.py), so a board test needs no second keyboard.

  1. The real relay.py on loopback: no token and a wrong token are 401; a relay started without --bench-dir is 404;
     with it, an ask with nothing queued gets an empty 200; a queued line is handed out once, cut to one printable
     line; what the Pi sends lands in the log.
  2. QEMU virt, built for that relay: before anyone types `bench`, a queued command is NOT taken (pi-bench.py gives
     up); after `bench`, pi-bench.py runs `man bench` and gets its text back, a word that is no command comes back as
     zsh's refusal, and `bench` sent from the Mac turns it off, after which nothing more is taken.
Skips (exit 0) without clang, ld.lld or qemu-system-aarch64.
"""
import json, os, shutil, socket, subprocess, sys, time, urllib.request, urllib.error
from pathlib import Path
ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(Path(__file__).resolve().parent))
from scratch import scratch_dir
if not all(shutil.which(t) for t in ("clang", "ld.lld", "qemu-system-aarch64")):
    print("SKIP: clang, ld.lld or qemu-system-aarch64 not installed"); sys.exit(0)
os.chdir(ROOT)
TMP = Path(scratch_dir("bench"))
RELAY_PY, TOOL = ROOT / "tools/claude-relay/relay.py", ROOT / "tools/pi-bench.py"
TOKEN = "bench-check-token-0123456789abcdef"
fails = []
def check(name, ok, detail=""):
    print(("  ok: " if ok else "  FAIL: ") + name + ("" if ok else "  " + repr(detail)[-600:]), flush=True)
    if not ok: fails.append(name)
def free_port():
    s = socket.socket(); s.bind(("127.0.0.1", 0)); p = s.getsockname()[1]; s.close(); return p
procs = []
def start(port, bench_dir):
    env = {k: v for k, v in os.environ.items() if not k.startswith("CLAUDE_RELAY")}
    env.update(CLAUDE_RELAY_TOKEN=TOKEN)
    err = open(TMP / ("relay-%d.log" % port), "w+")
    p = subprocess.Popen([sys.executable, str(RELAY_PY), "--port", str(port), "--cwd", str(TMP), "--timeout", "20",
                          *(["--bench-dir", str(bench_dir)] if bench_dir else [])], env=env, stdout=subprocess.DEVNULL, stderr=err)
    procs.append(p)
    for _ in range(100):
        time.sleep(0.05); err.seek(0)
        if "listening on" in err.read(): return p
    raise SystemExit("FAIL: the relay did not start: " + open(err.name).read())
def post(port, body=b"", token=TOKEN):
    req = urllib.request.Request("http://127.0.0.1:%d/api/bench" % port, data=body, method="POST", headers={"Authorization": "Bearer " + token} if token else {})
    try:
        with urllib.request.urlopen(req, timeout=10) as r: return r.status, r.read().decode()
    except urllib.error.HTTPError as e: return e.code, e.read().decode()
def tool(d, *a, wait=40):
    r = subprocess.run([sys.executable, str(TOOL), "--dir", str(d), "--wait", str(wait), *a], capture_output=True, text=True, timeout=wait + 20)
    return r.returncode, r.stdout + r.stderr

q = None
try:
    # ---- 1. the relay ----
    d1 = TMP / "b1"; shutil.rmtree(d1, ignore_errors=True)
    p_on, p_off = free_port(), free_port()
    start(p_on, d1); start(p_off, None)
    check("no token is 401", post(p_on, token=None)[0] == 401)
    check("a wrong token is 401", post(p_on, token="nope")[0] == 401)
    check("a relay without --bench-dir is 404", post(p_off)[0] == 404)
    check("nothing queued: an empty 200", post(p_on) == (200, ""))
    (d1 / "queue").write_text("gpu\x07 clear\n" + "x" * 300 + "\n")
    check("a queued line is handed out, control characters dropped", post(p_on) == (200, "gpu clear"), post)
    check("... once: the next ask gets the next line, cut to 200", post(p_on, b"gpu: ready\n") == (200, "x" * 200))
    check("then nothing", post(p_on) == (200, ""))
    logtxt = (d1 / "log").read_text()
    check("the log has each command and what the Pi sent back", "$ gpu clear\ngpu: ready\n" in logtxt, logtxt)
    check("the tool reports a Pi that just asked", tool(d1, "--status")[0] == 0)

    # ---- 2. the Pi side, in QEMU ----
    d2 = TMP / "b2"; shutil.rmtree(d2, ignore_errors=True)
    port = free_port(); start(port, d2)
    tokf = TMP / "token"; tokf.write_text(TOKEN); tokf.chmod(0o600)
    env = {k: v for k, v in os.environ.items() if not k.startswith("CLAUDE_RELAY")}
    env.update(CLAUDE_RELAY_HOST="10.0.2.2", CLAUDE_RELAY_PORT=str(port), CLAUDE_RELAY_TOKEN_FILE=str(tokf), CLAUDE_RELAY_TLS="0")
    r = subprocess.run(["make", "-C", "arch/arm64", "TLS_TA="], env=env, capture_output=True, text=True, timeout=600)
    if r.returncode: print(r.stdout[-1500:], r.stderr[-1500:]); raise SystemExit("FAIL: arch/arm64 does not build")
    log, sock = TMP / "uart", TMP / "qmp"
    for f in (log, sock):
        if f.exists(): f.unlink()
    q = subprocess.Popen(["qemu-system-aarch64", "-machine", "virt", "-cpu", "cortex-a72", "-m", "256", "-global", "virtio-mmio.force-legacy=false",
                          "-netdev", "user,id=n", "-device", "virtio-net-device,netdev=n", "-device", "ramfb", "-device", "virtio-keyboard-device",
                          "-display", "none", "-serial", "file:%s" % log, "-qmp", "unix:%s,server,nowait" % sock,
                          "-kernel", "arch/arm64/kernel8.elf"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    uart = lambda: log.read_text(errors="replace") if log.exists() else ""
    def wait(text, secs, after=0):
        for _ in range(int(secs * 10)):
            if text in uart()[after:]: return True
            time.sleep(0.1)
        return False
    if not wait("M2 input ready", 60) or not wait("net dhcp 10.0.2.15", 30): raise SystemExit("FAIL: no boot or no DHCP: " + uart()[-400:])
    s = socket.socket(socket.AF_UNIX); s.settimeout(30); s.connect(str(sock)); f = s.makefile("rw"); f.readline()
    def cmd(c, **a):
        f.write(json.dumps({"execute": c, **({"arguments": a} if a else {})}) + "\n"); f.flush()
        while True:
            rr = json.loads(f.readline())
            if "return" in rr or "error" in rr: return rr
    cmd("qmp_capabilities")
    def key(k): cmd("send-key", keys=[{"type": "qcode", "data": k}], **{"hold-time": 50}); time.sleep(0.15)
    key("f2"); check("F2 opens the Terminal", wait("terminal open", 10), uart()[-300:]); time.sleep(0.5)
    rc, out = tool(d2, "man", "bench", wait=6)
    check("before `bench` is typed, the Mac's line is not taken", rc == 1 and "did not take" in out and "bench % " not in uart(), out)
    (d2 / "queue").write_text("")
    for ch in "bench": key(ch)
    key("ret")
    check("`bench` at the keyboard turns it on", wait("bench: on", 10), uart()[-300:])
    rc, out = tool(d2, "man", "bench")
    check("the Mac runs `man bench` and reads its text back", rc == 0 and "Lets the Mac run Terminal commands" in out, out)
    check("... and the Pi's own screen shows the line it ran", "bench % man bench" in uart(), uart()[-400:])
    rc, out = tool(d2, "zzznope")
    check("a word that is no command comes back as the refusal", rc == 0 and "command not found: zzznope" in out, out)
    rc, out = tool(d2, "bench")
    check("`bench` from the Mac turns it off", rc == 0 and "bench: off" in out, out)
    rc, out = tool(d2, "man", "scan", wait=6)
    check("and nothing more is taken", rc == 1 and "did not take" in out, out)
    check("no fault, no crash", "KERNEL CRASH" not in uart() and "oom" not in uart(), uart()[-300:])
finally:
    if q: q.kill(); q.wait()
    for p in procs: p.terminate()
    subprocess.run(["make", "-s", "-C", "arch/arm64", "clean"], capture_output=True, timeout=60)
if fails: print("FAIL: %d bench check(s) failed" % len(fails)); sys.exit(1)
print("PASS: the bench: the relay hands a queued line to a Pi with the token, the Pi runs it only after `bench` was typed there, and the output comes back to the Mac")
