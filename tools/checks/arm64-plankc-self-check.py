#!/usr/bin/env python3
"""plankc, built by itself, compiles on the box (6.69.0, the way to steps 5 and 6 of the true bootstrap in docs/ROADMAP.md).

On the Mac, plank.py builds plankc (stage 1), stage 1 writes plankc's own aarch64 assembly, and tools/plankapp.sh --plankc
links it into JT/APPS/PLANKC.APP (stage 2, plankc built by plankc). The kernel boots in QEMU with plankc's sources on the
card in JT/PC, and the Terminal runs stage 2 through userrun's words (JT_SYS_ARGS) and whole-file reads and writes
(JT_SYS_READFILE, JT_SYS_WRITEFILE): userrun plankc asm PC/FIB.PK PC/RT.PK PC/FIB.S, and the assembly stage 2 writes on the
box is byte for byte what stage 1 wrote on the Mac. Next (with a collector in rt.pk; plankc's self-compile does not fit an
app's 11 MiB heap without one): the same for plankc's own source, the fixed point.
Discriminating: make rt_args drop its last word and stage 2 never finds its output name; make gen.pk's `<` emit `le` and
stage 1, built by plank.py, and stage 2, built by that broken plankc, write different assembly for anything with a `<`.
Skips (exit 0) without clang's aarch64 target, ld.lld, qemu-system-aarch64, mtools, uv or Plank (PLANK=, as plankapp.sh).
"""
import json, os, shutil, socket, subprocess, sys, time
from pathlib import Path
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from scratch import scratch_dir
from sdcard import PART, build_card

root = Path(__file__).resolve().parents[2]
arch = root / "arch/arm64"
src = root / "user/plankc"
plank = Path(os.environ.get("PLANK", Path.home() / "Documents/Code/plank/plank.py"))
if not all(shutil.which(t) for t in ("clang", "ld.lld", "qemu-system-aarch64", "mmd", "mcopy", "mtype", "uv")) or not plank.exists():
    print("SKIP: clang, ld.lld, qemu-system-aarch64, mtools, uv or Plank (%s) missing" % plank); sys.exit(0)
tmp = Path(scratch_dir("arm64-plankc-self"))
for p in tmp.iterdir(): p.unlink() if p.is_file() else shutil.rmtree(p)
fails = []
def check(name, ok, detail=""):
    print(("  ok: " if ok else "  FAIL: ") + name + ("" if ok else "  " + repr(detail)[-700:]), flush=True)
    if not ok: fails.append(name)

stage1 = tmp / "plankc1"
r = subprocess.run(["uv", "run", "-q", "--with", "llvmlite", "python3", str(plank), "build", str(src / "plankc.pk"), "-o", str(stage1)],
                   capture_output=True, text=True, timeout=600)
if r.returncode: print(r.stderr[-1500:]); print("FAIL: plank.py does not build plankc"); sys.exit(1)
fib = Path(plank).parent / "examples/fib.pk"
want = {}
for name, f in (("FIB.S", fib),):
    r = subprocess.run([str(stage1), "asm", str(f), str(src / "rt.pk")], capture_output=True, text=True, timeout=600)
    check("stage 1 (plankc built by plank.py) writes %s's assembly on the Mac" % f.name, r.returncode == 0 and "error" not in r.stdout[:200], r.stdout[-400:])
    want[name] = r.stdout[:-1]   # print's newline
r = subprocess.run([str(root / "tools/plankapp.sh"), "--plankc", str(src / "plankc.pk"), str(tmp / "PLANKC.APP")],
                   capture_output=True, text=True, timeout=600, env=dict(os.environ, PLANKC_EXE=str(stage1)))
check("stage 2: plankc builds itself into PLANKC.APP", r.returncode == 0, (r.stdout + r.stderr)[-600:])
if fails: print("FAIL: " + ", ".join(fails)); sys.exit(1)

subprocess.run(["make", "-C", str(arch), "clean"], capture_output=True, timeout=60)
q = None
try:
    r = subprocess.run(["make", "-C", str(arch), "kernel8.elf"], capture_output=True, text=True, timeout=900)
    if r.returncode: print(r.stderr[-1500:]); print("FAIL: arch/arm64 does not build"); sys.exit(1)
    card = str(tmp / "card.img")
    build_card(card, False)
    MT = dict(os.environ, MTOOLS_SKIP_CHECK="1")
    def mt(*a): return subprocess.run([a[0], "-i", f"{card}@@{PART * 512}", *map(str, a[1:])], capture_output=True, env=MT)
    mt("mmd", "::JT"); mt("mmd", "::JT/APPS"); mt("mmd", "::JT/PC")
    mt("mcopy", "-o", tmp / "PLANKC.APP", "::JT/APPS/PLANKC.APP")
    for n in ("plankc.pk", "gen.pk", "parse.pk", "lex.pk", "rt.pk"): mt("mcopy", "-o", src / n, "::JT/PC/" + n.upper())
    mt("mcopy", "-o", fib, "::JT/PC/FIB.PK")
    log, sock = tmp / "uart", tmp / "qmp"
    q = subprocess.Popen(["qemu-system-aarch64", "-machine", "virt", "-cpu", "cortex-a72", "-m", "256", "-global", "virtio-mmio.force-legacy=false",
                          "-device", "ramfb", "-device", "virtio-keyboard-device", "-drive", "if=none,id=card,file=%s,format=raw" % card, "-device", "virtio-blk-device,drive=card",
                          "-display", "none", "-serial", "file:%s" % log, "-qmp", "unix:%s,server,nowait" % sock, "-kernel", str(arch / "kernel8.elf")],
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    def uart(): return log.read_text(errors="replace") if log.exists() else ""
    def wait_for(text, secs, after=0):
        end = time.time() + secs
        while time.time() < end:
            if text in uart()[after:]: return True
            if q.poll() is not None: return False
            time.sleep(0.2)
        return False
    if not wait_for("M2 input ready", 60): raise SystemExit("FAIL: did not boot: %r" % uart()[-600:])
    time.sleep(0.5)
    s = socket.socket(socket.AF_UNIX); s.settimeout(30); s.connect(str(sock)); f = s.makefile("rw"); f.readline()
    def command(name, **a):
        f.write(json.dumps({"execute": name, "arguments": a}) + "\n"); f.flush()
        while True:
            rr = json.loads(f.readline())
            if "return" in rr or "error" in rr: return rr
    command("qmp_capabilities")
    KEYS = {" ": "spc", "/": "slash", ".": "dot"}
    def key(*k): command("send-key", keys=[{"type": "qcode", "data": x} for x in k]); time.sleep(0.12)
    def typed(line):
        for ch in line:
            if ch.isupper(): key("shift", ch.lower())
            else: key(KEYS.get(ch, ch))
        key("ret")
    key("f2")
    if not wait_for("terminal open", 10): raise SystemExit("FAIL: F2 did not open the Terminal")
    time.sleep(0.5)
    for out, prog, secs in (("FIB.S", "FIB.PK", 120),):
        n = len(uart()); t0 = time.time()
        typed("userrun plankc asm PC/%s PC/RT.PK PC/%s" % (prog, out))
        done = wait_for("M3c app plankc exit", secs, n) or wait_for("M3c app plankc stopped", 1, n)
        seg = uart()[n:]
        check("stage 2 runs on the box and writes PC/%s (%d s)" % (out, time.time() - t0), done and ("plankc: wrote PC/%s" % out) in seg and "M3c app plankc exit 0" in seg, seg[-600:])
    check("no kernel crash", "KERNEL CRASH" not in uart(), uart()[-600:])
    q.kill(); q.wait(); q = None
    for out in ("FIB.S",):
        got = mt("mtype", "::JT/PC/" + out).stdout.decode(errors="replace")
        same = got == want[out]
        if not same:
            gl, wl = got.split("\n"), want[out].split("\n")
            i = next((k for k in range(min(len(gl), len(wl))) if gl[k] != wl[k]), min(len(gl), len(wl)))
            detail = {"line": i, "want": wl[i] if i < len(wl) else None, "got": gl[i] if i < len(gl) else None, "sizes": (len(want[out]), len(got))}
        check("%s written on the box is byte for byte stage 1's" % out, same, None if same else detail)
finally:
    if q: q.kill(); q.wait()
    subprocess.run(["make", "-C", str(arch), "clean"], capture_output=True)
if fails: print("FAIL: " + ", ".join(fails)); sys.exit(1)
print("PASS: plankc, built by itself, compiles on the box into exactly what plank.py's build of it writes on the Mac")
