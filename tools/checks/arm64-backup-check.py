#!/usr/bin/env python3
"""Backups to the Mac (5.2.0): the Pi's `backup` (arch/arm64/ask.c backup_command, snap.c snap_pack) and the relay's
/api/backup (tools/claude-relay/relay.py backup_unpack, backup_write).

Host, against the real relay code:
  1. Hostile containers are refused in plain words and write nothing: bad magic, `..`, an empty name (an absolute path),
     a name that is not 8.3, too deep, a file over 32 KB, a length cut short, non-ASCII, a repeated name, a name that is
     both a file and a folder. A good one lands whole in <user>/<time>/ and leaves no .incoming folder.
  2. The real relay.py on loopback: no token and a wrong token are 401, a relay started without --backup-dir is 404 for
     backups, a bad user name is 400, a hostile container is 400 with nothing on disk, a good one is 200 "N files".
  3. Mutation: with the 8.3 name rule loosened, a name with a space and a ! gets through, so the rule is what refuses it.
QEMU virt, the real relay with --backup-dir and a stub `claude`, a card whose JT/USERS.TXT holds ann and whose
JT/HOME/ann holds three small files and one of 40 KB:
  4. Sign in as ann, F2, `backup`: "backup: 3 files, ..." and "1 left out"; the Mac's copy matches the card byte for byte;
     the stub `claude` never ran; the token never reached the serial log.
Restore from the Mac (5.3.0, ask.c restore_mac_command, snap.c snap_unpack, relay.py backup_latest):
  5. Host: latest is None with no backup; it packs the newest run and leaves out what the Pi would refuse (a name that is
     not 8.3, a link, a file over 32 KB, SNAP and _DONE at the top, a path over 7 names, an .incoming run). Loopback:
     200 with the bytes, 404 for a name with none or a relay without --backup-dir, 401 with no token.
  6. Pi: the Mac's copy is edited (one file changed, one new); `restore mac` only warns; `restore mac yes` says 4 files came
     back after snapshot 1; after shutdown the card holds the edit and the new file, keeps BIG.BIN, and SNAP/1 holds the old note.

Usage: tools/checks/arm64-backup-check.py   (from the repo root)
"""
import hashlib, importlib.util, json, os, re, shutil, socket, subprocess, sys, time, urllib.request, urllib.error
from pathlib import Path
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from scratch import scratch_dir
from freeport import free_port

ROOT = Path(__file__).resolve().parents[2]
os.chdir(ROOT)
TMP = Path(scratch_dir("backup"))
RELAY_PY = ROOT / "tools/claude-relay/relay.py"
fails = []
def check(name, ok, detail=""):
    print(("  ok: " if ok else "  FAIL: ") + name + (f" ({detail})" if detail and not ok else ""))
    if not ok: fails.append(name)

spec = importlib.util.spec_from_file_location("relay", RELAY_PY); relay = importlib.util.module_from_spec(spec); spec.loader.exec_module(relay)

def box(*files):
    out = b"JTB1"
    for path, data in files:
        p = path.encode() if isinstance(path, str) else path
        out += bytes([len(p)]) + p + len(data).to_bytes(4, "little") + data
    return out

def tree(d):
    return {str(p.relative_to(d)): p.read_bytes() for p in Path(d).rglob("*") if p.is_file()}

# ---- 1. the relay's own rules ----
HOSTILE = {
    "bad magic": b"NOPE" + box(("A.TXT", b"x"))[4:],
    "a .. in the path": box(("../ESCAPE.TXT", b"x")),
    "an absolute path": box(("/ETC.TXT", b"x")),
    "a name that is not 8.3": box(("bad name!.TXT", b"x")),
    "too deep": box(("/".join(["D"] * 9) + "/F.TXT", b"x")),
    "a file over 32 KB": box(("BIG.BIN", b"x" * 40000)),
    "a length cut short": box(("A.TXT", b"hello"))[:-2],
    "non-ASCII": box(("CAF\xc9.TXT".encode("latin-1"), b"x")),
    "a repeated name": box(("A.TXT", b"1"), ("a.txt", b"2")),
    "a file that is also a folder": box(("NOTES", b"1"), ("NOTES/N1.TXT", b"2")),
}
store = TMP / "store"; shutil.rmtree(store, ignore_errors=True); store.mkdir()
for why, raw in HOSTILE.items():
    try: relay.backup_write(str(store), "ann", relay.backup_unpack(raw)); refused = False
    except ValueError: refused = True
    check("refused: " + why, refused)
check("nothing was written for any of them", not any(Path(store).rglob("*.*")) and not list(Path(store).rglob("*.TXT")), tree(store))
good = box(("NOTE.TXT", b"hello"), ("NOTES/N1.TXT", b"one"), ("NOTES/F0001/N2.TXT", b"two"))
n, total, folder = relay.backup_write(str(store), "ann", relay.backup_unpack(good))
check("a good backup lands whole", (n, total) == (3, 11) and tree(store / "ann" / folder) == {"NOTE.TXT": b"hello", "NOTES/N1.TXT": b"one", "NOTES/F0001/N2.TXT": b"two"}, tree(store))
check("and leaves no .incoming folder", not [p for p in (store / "ann").iterdir() if p.name.startswith(".incoming")])

# ---- 3. mutation: the 8.3 rule is the gate ----
keep = relay.BACKUP_NAME_RE
relay.BACKUP_NAME_RE = re.compile(r"[^/]{1,40}")
try: relay.backup_unpack(HOSTILE["a name that is not 8.3"]); let_through = True
except ValueError: let_through = False
relay.BACKUP_NAME_RE = keep
check("mutation: a loosened name rule lets 'bad name!.TXT' through", let_through)

# ---- 5. restore from the Mac (5.3.0): /api/backup/latest packs the newest run ----
check("no backup for a name is None", relay.backup_latest(str(store), "bob") is None)
newest = store / "ann" / "9999-99-99-999999"; (newest / "SUB").mkdir(parents=True)
(newest / "NEW.TXT").write_bytes(b"newest"); (newest / "SUB" / "S.TXT").write_bytes(b"s")
(newest / "bad name.TXT").write_bytes(b"x"); (newest / "HUGE.BIN").write_bytes(b"h" * 40000); (newest / "SNAP").mkdir()
(newest / "_DONE").write_bytes(b"x"); os.symlink("/etc/hosts", newest / "LINK.TXT")
deep = newest / "/".join("D%d" % k for k in range(7)); deep.mkdir(parents=True); (deep / "F.TXT").write_bytes(b"x"); (deep.parent / "OK.TXT").write_bytes(b"x")
(store / "ann" / ".incoming-zzzz").mkdir()
got = dict(("/".join(p), d) for p, d in relay.backup_unpack(relay.backup_latest(str(store), "ann")))
check("latest packs the newest run, a name the Pi refuses left out", got == {"NEW.TXT": b"newest", "SUB/S.TXT": b"s", "D0/D1/D2/D3/D4/D5/OK.TXT": b"x"}, sorted(got))

# ---- 2. the real relay on loopback ----
TOKEN = "backup-check-token-0123456789abcdef"
bindir = TMP / "bin"; bindir.mkdir(exist_ok=True)
stub_mark = TMP / "claude-ran"
if stub_mark.exists(): stub_mark.unlink()
(bindir / "claude").write_text("#!/bin/sh\ntouch %s\necho '{\"result\":\"stub\",\"session_id\":\"x\"}'\n" % stub_mark); (bindir / "claude").chmod(0o755)
procs = []
def start(port, backup_dir):
    env = {k: v for k, v in os.environ.items() if not k.startswith("CLAUDE_RELAY")}
    env.update(PATH=str(bindir) + os.pathsep + os.environ["PATH"], CLAUDE_RELAY_TOKEN=TOKEN)
    err = open(TMP / ("relay-%d.log" % port), "w+")
    p = subprocess.Popen([sys.executable, str(RELAY_PY), "--port", str(port), "--cwd", str(TMP), "--timeout", "20",
                          *(["--backup-dir", str(backup_dir)] if backup_dir else [])], env=env, stdout=subprocess.DEVNULL, stderr=err)
    procs.append(p)
    for _ in range(100):
        time.sleep(0.05); err.seek(0)
        if "listening on" in err.read(): return p
    raise SystemExit("FAIL: the relay did not start: " + open(err.name).read())
def post(port, body, user="ann", token=TOKEN, path="/api/backup", raw=False):
    req = urllib.request.Request("http://127.0.0.1:%d%s?user=%s" % (port, path, user), data=body, method="POST",
                                 headers={"Authorization": "Bearer " + token} if token else {})
    try:
        with urllib.request.urlopen(req, timeout=10) as r: b = r.read(); return r.status, b if raw else b.decode()
    except urllib.error.HTTPError as e: return e.code, e.read().decode()

kept = TMP / "kept"; shutil.rmtree(kept, ignore_errors=True)
try:
    p_on, p_off = free_port(), free_port()
    start(p_on, kept); start(p_off, None)
    check("no token is 401", post(p_on, good, token=None)[0] == 401)
    check("a wrong token is 401", post(p_on, good, token="nope")[0] == 401)
    check("a relay without --backup-dir keeps no backups (404)", post(p_off, good)[0] == 404)
    check("a bad user name is 400", post(p_on, good, user="../ann")[0] == 400)
    st, txt = post(p_on, HOSTILE["a .. in the path"])
    check("a hostile container is 400 and nothing lands", st == 400 and not tree(kept), (st, txt, tree(kept)))
    st, txt = post(p_on, good)
    check("a good one is 200 and says what it kept", st == 200 and txt.startswith("3 files, 11 bytes, kept on the Mac in ann/"), (st, txt))
    st, raw = post(p_on, b"", path="/api/backup/latest", raw=True)
    check("latest is 200 and gives that backup back", st == 200 and raw == box(("NOTE.TXT", b"hello"), ("NOTES/N1.TXT", b"one"), ("NOTES/F0001/N2.TXT", b"two")), (st, raw[:80]))
    check("latest for a name with none is 404", post(p_on, b"", user="bob", path="/api/backup/latest")[0] == 404)
    check("latest with no token is 401", post(p_on, b"", token=None, path="/api/backup/latest")[0] == 401)
    check("latest on a relay without --backup-dir is 404", post(p_off, b"", path="/api/backup/latest")[0] == 404)
finally:
    for p in procs: p.kill(); p.wait()
    procs.clear()

# ---- 4. the Pi, end to end ----
if not all(shutil.which(t) for t in ("clang", "ld.lld", "qemu-system-aarch64", "mmd", "mcopy")):
    print("  skip: no aarch64 toolchain or mtools")
else:
    from sdcard import PART, build_card
    MT = dict(os.environ, MTOOLS_SKIP_CHECK="1")
    salt = bytes(range(32, 48))
    record = "ann:p2:100000:%s:%s\n" % (salt.hex(), hashlib.pbkdf2_hmac("sha256", b"hunter22", salt, 100000, 32).hex())
    card = str(TMP / "card.img"); build_card(card, False)
    def mt(*a): return subprocess.run([a[0], "-i", f"{card}@@{PART * 512}", *a[1:]], capture_output=True, env=MT)
    ON_CARD = {"NOTE.TXT": b"buy milk\n", "NOTES/N1.TXT": b"first note\n", "NOTES/N2.TXT": bytes(range(256)) * 8}
    for d in ("::JT", "::JT/HOME", "::JT/HOME/ann", "::JT/HOME/ann/NOTES"): mt("mmd", d)
    (TMP / "users").write_text(record); mt("mcopy", str(TMP / "users"), "::JT/USERS.TXT")
    for name, data in list(ON_CARD.items()) + [("BIG.BIN", b"b" * 40000)]:
        f = TMP / "f.bin"; f.write_bytes(data); mt("mcopy", "-o", str(f), "::JT/HOME/ann/" + name)
    tokf = TMP / "token"; tokf.write_text(TOKEN); tokf.chmod(0o600)
    port = free_port()
    env = {k: v for k, v in os.environ.items() if not k.startswith("CLAUDE_RELAY")}
    env.update(CLAUDE_RELAY_HOST="10.0.2.2", CLAUDE_RELAY_PORT=str(port), CLAUDE_RELAY_TOKEN_FILE=str(tokf), CLAUDE_RELAY_TLS="0")
    r = subprocess.run(["make", "-C", "arch/arm64", "TLS_TA="], env=env, capture_output=True, text=True, timeout=600)
    if r.returncode: print(r.stdout[-1500:], r.stderr[-1500:]); fails.append("build")
    kept2 = TMP / "kept2"; shutil.rmtree(kept2, ignore_errors=True)
    log, sock = TMP / "uart", TMP / "qmp"
    for f in (log, sock):
        if f.exists(): f.unlink()
    q = None
    try:
        start(port, kept2)
        q = subprocess.Popen(["qemu-system-aarch64", "-machine", "virt", "-cpu", "cortex-a72", "-m", "256", "-global", "virtio-mmio.force-legacy=false",
                              "-netdev", "user,id=n", "-device", "virtio-net-device,netdev=n", "-device", "ramfb", "-device", "virtio-keyboard-device",
                              "-drive", "if=none,id=card,file=%s,format=raw" % card, "-device", "virtio-blk-device,drive=card",
                              "-display", "none", "-serial", "file:%s" % log, "-qmp", "unix:%s,server,nowait" % sock,
                              "-kernel", "arch/arm64/kernel8.elf"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        uart = lambda: log.read_text(errors="replace") if log.exists() else ""
        def wait(text, secs, after=0):
            for _ in range(int(secs * 10)):
                if text in uart()[after:]: return True
                time.sleep(0.1)
            return False
        if not wait("login: screen up", 120): raise SystemExit("FAIL: no login screen: " + uart()[-400:])
        s = socket.socket(socket.AF_UNIX); s.settimeout(30); s.connect(str(sock)); f = s.makefile("rw"); f.readline()
        def cmd(c, **a):
            f.write(json.dumps({"execute": c, **({"arguments": a} if a else {})}) + "\n"); f.flush()
            while True:
                rr = json.loads(f.readline())
                if "return" in rr or "error" in rr: return rr
        cmd("qmp_capabilities")
        def key(k): cmd("send-key", keys=[{"type": "qcode", "data": k}], **{"hold-time": 50}); time.sleep(0.15)
        def typ(t):
            for ch in t: key("spc" if ch == " " else ch)
        typ("ann"); key("ret"); typ("hunter22"); key("ret")
        check("ann signs in", wait("login: desktop up", 150), uart()[-300:])
        if not wait("net dhcp", 30): print("  note: no dhcp line seen")
        key("f2"); wait("terminal open", 10); time.sleep(0.5)
        n = len(uart())
        typ("backup"); key("ret")
        check("backup sends the home folder", wait("backup: sending 3 files to the Mac", 30, n), uart()[n:][-400:])
        check("the relay says what it kept", wait("kept on the Mac in ann/", 60, n), uart()[n:][-400:])
        check("the 40 KB file is left out and counted", wait("backup: 1 left out", 10, n), uart()[n:][-400:])
        runs = [p for p in (kept2 / "ann").iterdir()] if (kept2 / "ann").exists() else []
        got = tree(runs[0]) if len(runs) == 1 else {}
        check("the Mac's copy matches the card byte for byte", got == ON_CARD, sorted(got))
        if runs:   # the Mac's copy changes, then comes back: one file edited, one new, the card's others left
            (runs[0] / "NOTE.TXT").write_bytes(b"from the mac\n"); (runs[0] / "MAC.TXT").write_bytes(b"made on the mac\n")
        n = len(uart())
        typ("restore mac"); key("ret")
        check("restore mac alone only warns", wait("Type restore mac yes to go on", 10, n), uart()[n:][-300:])
        n = len(uart())
        typ("restore mac yes"); key("ret")
        check("restore mac yes brings 4 files back after a snapshot", wait("restore mac: 4 files back from the Mac, snapshot 1 taken first", 60, n), uart()[n:][-400:])
        check("the stub claude never ran", not stub_mark.exists())
        check("the token never reached the serial log", TOKEN not in uart())
    finally:
        if q: q.terminate(); q.wait(5)
        for p in procs: p.kill(); p.wait()
    def on_card(path):
        r = mt("mtype", "::JT/HOME/ann/" + path); return r.stdout if r.returncode == 0 else None
    check("the card holds the Mac's edit and new file", on_card("NOTE.TXT") == b"from the mac\n" and on_card("MAC.TXT") == b"made on the mac\n", (on_card("NOTE.TXT"), on_card("MAC.TXT")))
    check("the card's own files are kept, the big one too", on_card("NOTES/N1.TXT") == ON_CARD["NOTES/N1.TXT"] and on_card("BIG.BIN") == b"b" * 40000)
    check("the snapshot taken first holds the card as it was", on_card("SNAP/1/NOTE.TXT") == b"buy milk\n")

if fails: print("FAIL: " + "; ".join(fails)); sys.exit(1)
print("PASS: the relay refuses every hostile backup in plain words and writes nothing, keeps a good one whole, and the Pi's "
      "`backup` sends ann's home folder to the Mac byte for byte, the 40 KB file counted and left out, and `restore mac yes` "
      "brings the Mac's edited copy back after a snapshot, keeping what the backup lacks")
