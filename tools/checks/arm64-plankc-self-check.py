#!/usr/bin/env python3
"""plankc, built by itself, rebuilds itself on the box (6.69.0, 6.70.0: steps 5 and 6 of the true bootstrap in docs/ROADMAP.md).

On the Mac, plank.py builds plankc (stage 1), stage 1 writes plankc's own aarch64 assembly, and tools/plankapp.sh --plankc
links it into JT/APPS/PLANKC.APP (stage 2, plankc built by plankc). The kernel boots in QEMU with plankc's sources on the
card in JT/PC, and the Terminal runs stage 2 through userrun's words (JT_SYS_ARGS) and whole-file reads and writes
(JT_SYS_READFILE, JT_SYS_WRITEFILE): userrun plankc asm PC/FIB.PK PC/RT.PK PC/FIB.S, and the assembly stage 2 writes on the
box is byte for byte what stage 1 wrote on the Mac. 6.70.0: then userrun plankc asm PC/PLANKC.PK PC/RT.PK PC/SELF.S, plankc
compiling its own source on the box, writes exactly what stage 1 wrote: the fixed point, the compiler built by itself making
the same compiler. Without rt.pk's collector that run ends in out of memory.
6.71.0, no clang and no linker: plankc img writes fib's app image on the box, and it runs; plankc img writes plankc itself as
PLANKC2, which compiles plankc to the same assembly again and writes PLANKC3, byte for byte PLANKC2. Discriminating: a cset
that does not invert its condition fails fib's image, the self-compile and the next image.
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
floats = src / "tests/floats.pk"   # 6.76.0: the encoder's float instructions, run on the box
floats_want = [l for l in subprocess.run(["uv", "run", "-q", "--with", "llvmlite", "python3", str(plank), "run", str(floats)],
                                         capture_output=True, text=True, timeout=600).stdout.split("\n") if l]
rnd = src / "tests/random.pk"   # 6.92.0: random's eor, lslv and lsrv, encoded on the box (the Mac's clang knew them, plankc did not)
rnd_want = [l for l in subprocess.run(["uv", "run", "-q", "--with", "llvmlite", "python3", str(plank), "run", str(rnd)],
                                      capture_output=True, text=True, timeout=600).stdout.split("\n") if l]
want = {}
for name, f in (("FIB.S", fib), ("SELF.S", src / "plankc.pk")):
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
    for n in ("plankc.pk", "gen.pk", "enc.pk", "parse.pk", "lex.pk", "rt.pk"): mt("mcopy", "-o", src / n, "::JT/PC/" + n.upper())
    mt("mcopy", "-o", fib, "::JT/PC/FIB.PK")
    mt("mcopy", "-o", floats, "::JT/PC/FLOATS.PK")
    mt("mcopy", "-o", rnd, "::JT/PC/RANDOM.PK")
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
    def wait_any(texts, secs, after=0):   # an app's end, however it ends: a stopped one must not cost the whole timeout
        end = time.time() + secs
        while time.time() < end:
            seg = uart()[after:]
            if any(t in seg for t in texts): return True
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
    if not wait_for("terminal open", 30): raise SystemExit("FAIL: F2 did not open the Terminal: %r" % uart()[-600:])   # 6.79.0: 30 s, a loaded Mac took more than 10
    time.sleep(0.5)
    for out, prog, secs in (("FIB.S", "FIB.PK", 120), ("SELF.S", "PLANKC.PK", 1800)):
        n = len(uart()); t0 = time.time()
        typed("userrun plankc asm PC/%s PC/RT.PK PC/%s" % (prog, out))
        done = wait_for("M3c app plankc exit", secs, n) or wait_for("M3c app plankc stopped", 1, n)
        seg = uart()[n:]
        check("stage 2 runs on the box and writes PC/%s (%d s)" % (out, time.time() - t0), done and ("plankc: wrote PC/%s" % out) in seg and "M3c app plankc exit 0" in seg, seg[-600:])
    # 6.71.0: no clang, no linker. plankc writes app images itself (enc.pk); they run, and the compiler it writes makes itself.
    def run(line, prog, secs):
        n = len(uart()); t0 = time.time()
        typed(line)
        done = wait_any(["M3c app %s exit" % prog, "M3c app %s stopped" % prog], secs, n)
        return uart()[n:], done, time.time() - t0
    seg, done, secs = run("userrun plankc img PC/FIB.PK PC/RT.PK APPS/FIBX.APP", "plankc", 300)
    check("stage 2 writes fib's app image on the box (%d s)" % secs, done and "plankc: wrote APPS/FIBX.APP" in seg and "M3c app plankc exit 0" in seg, seg[-500:])
    seg, done, secs = run("userrun fibx", "fibx", 60)
    body = seg[seg.find("M3c app fibx starts"):] if "M3c app fibx starts" in seg else ""
    got = [l for l in body.split("\n")[1:] if l and not l.startswith(("M3c ", "M3 ", "key ", "userrun ", "slow step ")) and "joshua@tree" not in l]
    check("that image, built with no clang and no linker, runs and prints fib.out", got == [l for l in fib.with_suffix(".out").read_text().split("\n") if l] and "M3c app fibx exit 0" in seg, {"got": got[:6], "tail": seg[-400:]})
    seg, done, secs = run("userrun plankc img PC/FLOATS.PK PC/RT.PK APPS/FLTX.APP", "plankc", 300)
    check("stage 2 writes floats.pk's app image on the box (%d s)" % secs, done and "plankc: wrote APPS/FLTX.APP" in seg and "M3c app plankc exit 0" in seg, seg[-500:])
    seg, done, secs = run("userrun fltx", "fltx", 60)
    body = seg[seg.find("M3c app fltx starts"):] if "M3c app fltx starts" in seg else ""
    got = [l for l in body.split("\n")[1:] if l and not l.startswith(("M3c ", "M3 ", "key ", "userrun ", "slow step ")) and "joshua@tree" not in l]
    check("its float instructions, encoded by plankc, print what plank.py's build prints", got == floats_want and len(got) > 5 and "M3c app fltx exit 0" in seg, {"got": got[:8], "want": floats_want[:8]})
    seg, done, secs = run("userrun plankc img PC/RANDOM.PK PC/RT.PK APPS/RNDX.APP", "plankc", 300)
    check("stage 2 writes random.pk's app image on the box (%d s)" % secs, done and "plankc: wrote APPS/RNDX.APP" in seg and "M3c app plankc exit 0" in seg, seg[-500:])
    seg, done, secs = run("userrun rndx", "rndx", 60)
    body = seg[seg.find("M3c app rndx starts"):] if "M3c app rndx starts" in seg else ""
    got = [l for l in body.split("\n")[1:] if l and not l.startswith(("M3c ", "M3 ", "key ", "userrun ", "slow step ")) and "joshua@tree" not in l]
    check("its xor and shifts, encoded by plankc, print what plank.py's build prints", got == rnd_want and len(got) == 4 and "M3c app rndx exit 0" in seg, {"got": got[:6], "want": rnd_want})
    seg, done, secs = run("userrun plankc img PC/PLANKC.PK PC/RT.PK APPS/PLANKC2.APP", "plankc", 1800)
    check("stage 2 writes its own app image, PLANKC2 (%d s)" % secs, done and "plankc: wrote APPS/PLANKC2.APP" in seg and "M3c app plankc exit 0" in seg, seg[-500:])
    seg, done, secs = run("userrun plankc2 asm PC/PLANKC.PK PC/RT.PK PC/SELF2.S", "plankc2", 1800)
    check("PLANKC2, plankc encoded by plankc, compiles plankc (%d s)" % secs, done and "M3c app plankc2 exit 0" in seg, seg[-500:])
    seg, done, secs = run("userrun plankc2 img PC/PLANKC.PK PC/RT.PK APPS/PLANKC3.APP", "plankc2", 1800)
    check("PLANKC2 writes the next one, PLANKC3 (%d s)" % secs, done and "M3c app plankc2 exit 0" in seg, seg[-500:])
    check("no kernel crash", "KERNEL CRASH" not in uart(), uart()[-600:])
    q.kill(); q.wait(); q = None
    want["SELF2.S"] = want["SELF.S"]
    two, three = mt("mtype", "::JT/APPS/PLANKC2.APP").stdout, mt("mtype", "::JT/APPS/PLANKC3.APP").stdout
    check("PLANKC3 is byte for byte PLANKC2 (%d bytes): the compiler, written by itself with nothing else, makes itself" % len(two), two == three and len(two) > 1000, (len(two), len(three)))
    for out in ("FIB.S", "SELF.S", "SELF2.S"):
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
print("PASS: plankc rebuilds itself on the box, to the same assembly and, with no clang or linker, to the same app image")
