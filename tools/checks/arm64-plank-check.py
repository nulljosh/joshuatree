#!/usr/bin/env python3
"""Plank programs run on Joshua Tree (6.57.0): Plank's own examples, built by tools/plankapp.sh, print what they print on a Mac.

Each example in Plank's examples/ folder listed below is compiled by `plank emit` and built into an app for the card
(JT/APPS/PKn.APP); the kernel boots in QEMU with no accounts, the Terminal runs each with `userrun pkN`, and what it writes
must equal the example's .out file line for line. Together they cover printing, ints and floats, strings, lists, dicts,
structs, enums with match, closures, optionals, and try and catch (setjmp and longjmp in user/pklibc).
6.58.0: then the store's Slide (store/slide.pk, a Plank program calling jtw, user/pklibc's window calls), as landing/apps
serves it, opens its window, slides tiles on the arrows and closes on Esc.
6.62.0: fib and fizzbuzz again, built by plankc (user/plankc/gen.pk, the compiler written in Plank) with no LLVM IR;
6.63.0: primes, and user/plankc/tests/strings.pk on plankc's runtime written in Plank (rt.pk), against plank.py's own output;
6.64.0: user/plankc/tests/lists.pk; 6.65.0: user/plankc/tests/structs.pk; 6.66.0: more.pk (optionals, string methods, slices); 6.67.0: dicts.pk; 6.68.0: errors.pk (try, throw, and leaving a try by return or break); 6.69.0: syscalls.pk, userrun's words and
whole-file writes, with bad buffers, a bad path and a length past the cap (its answer key is syscalls.out); 6.70.0: gc.pk, many
times 11 MiB of garbage around live data, and every plankc program again with the collector firing every 64 KB. 6.72.0:
methods.pk (struct methods, defaults and labels, optionals, get, x!, string order); 6.73.0: enums.pk (enums and match); 6.74.0: tuples.pk.
Discriminating: break user/pklibc's %.15g (fmt_double) and the float lines differ; make setjmp.S's longjmp return 0 and the
caught errors are lost; give gen.pk's < the condition le and fib by plankc prints a different line.
Skips (exit 0) without clang's aarch64 target, ld.lld, qemu-system-aarch64, mtools, uv or Plank (PLANK=path/to/plank.py,
default ~/Documents/Code/plank/plank.py; CI fetches it).
Usage: tools/checks/arm64-plank-check.py   (from the repo root)
"""
import json, os, shutil, socket, subprocess, sys, time
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from scratch import scratch_dir
from sdcard import PART, build_card

root = Path(__file__).resolve().parents[2]
arch = root / "arch/arm64"
plank = Path(os.environ.get("PLANK", Path.home() / "Documents/Code/plank/plank.py"))
if not all(shutil.which(t) for t in ("clang", "ld.lld", "qemu-system-aarch64", "mmd", "mcopy", "uv")) or not plank.exists():
    print("SKIP: clang, ld.lld, qemu-system-aarch64, mtools, uv or Plank (%s) missing" % plank); sys.exit(0)
EXAMPLES = ["hello", "fib", "fizzbuzz", "loops", "lists", "dicts", "structs", "enums", "closures", "optionals", "errors", "equality", "sqrt"]
tmp = Path(scratch_dir("arm64-plank"))
for p in tmp.iterdir(): p.unlink() if p.is_file() else shutil.rmtree(p)

fails = []
def check(name, ok, detail=""):
    print(("  ok: " if ok else "  FAIL: ") + name + ("" if ok else "  " + repr(detail)[-900:]), flush=True)
    if not ok: fails.append(name)

ex = plank.parent / "examples"
built = []
for i, n in enumerate(EXAMPLES):
    if not (ex / (n + ".pk")).exists(): print("  (no example %s, skipped)" % n); continue
    r = subprocess.run([str(root / "tools/plankapp.sh"), str(ex / (n + ".pk")), str(tmp / ("PK%d.APP" % i))], capture_output=True, text=True, timeout=600)
    check("%s.pk builds into an app" % n, r.returncode == 0, (r.stdout + r.stderr)[-800:])
    if r.returncode == 0: built.append((i, n))
if not built: print("FAIL: nothing built"); sys.exit(1)
# 6.62.0: the same examples built by plankc, the compiler written in Plank (step 3 of the true bootstrap), as aarch64 it writes
# itself, with no LLVM IR and no Plank runtime: tools/plankapp.sh --plankc.
# 6.63.0: primes, and plankc's own string test, whose answer key is what plank.py prints for it on this Mac.
PLANKC = [("pcfib", ex / "fib.pk"), ("pcfz", ex / "fizzbuzz.pk"), ("pcprime", ex / "primes.pk"), ("pcstr", root / "user/plankc/tests/strings.pk"), ("pclist", root / "user/plankc/tests/lists.pk"), ("pcst", root / "user/plankc/tests/structs.pk"), ("pcmore", root / "user/plankc/tests/more.pk"), ("pcdict", root / "user/plankc/tests/dicts.pk"), ("pcerr", root / "user/plankc/tests/errors.pk"), ("pcsys", root / "user/plankc/tests/syscalls.pk"), ("pcgc", root / "user/plankc/tests/gc.pk"), ("pcmeth", root / "user/plankc/tests/methods.pk"), ("pcenum", root / "user/plankc/tests/enums.pk"), ("pctup", root / "user/plankc/tests/tuples.pk"), ("pcclo", root / "user/plankc/tests/closures.pk"), ("pcflt", root / "user/plankc/tests/floats.pk"), ("pxenum", ex / "enums.pk"), ("pxlist", ex / "lists.pk"), ("pxsqrt", ex / "sqrt.pk"), ("pxstr", ex / "strings.pk"), ("pxstc", ex / "structs.pk")]
# 6.70.0: each again with the collector firing every 64 KB (plankc --gc=65536), the way Plank tests its own; same answers.
# (Every 4 KB is too slow for QEMU: the watchdog stops an app 3 s without a call, and a collection makes none.)
PLANKC += [(a + "g", f, 65536) for a, f in PLANKC]
PLANKC = [(t[0], t[1], t[2] if len(t) > 2 else 0) for t in PLANKC]
WORDS = {"pcsys": " one two", "pcsysg": " one two"}   # 6.69.0: userrun NAME WORDS reaches the app (JT_SYS_ARGS)
wants = {}
pc_env = dict(os.environ, PLANKC_EXE=str(tmp / "plankc"))   # plankc built once, by plank.py, for all four
subprocess.run(["uv", "run", "-q", "--with", "llvmlite", "python3", str(plank), "build", str(root / "user/plankc/plankc.pk"), "-o", str(tmp / "plankc")], capture_output=True, timeout=600)
for app, f, every in PLANKC:
    env = dict(pc_env, PLANKC_GC=str(every)) if every else pc_env
    r = subprocess.run([str(root / "tools/plankapp.sh"), "--plankc", str(f), str(tmp / (app.upper() + ".APP"))], capture_output=True, text=True, timeout=600, env=env)
    check("plankc builds %s into an app%s" % (f.name, " collecting every 64 KB" if every else ""), r.returncode == 0, (r.stdout + r.stderr)[-800:])
    if f.with_suffix(".out").exists():
        wants[app] = f.with_suffix(".out").read_text()
    else:
        wants[app] = next((wants[a] for a, g2, _ in PLANKC if g2 == f and a in wants), None) or subprocess.run(["uv", "run", "-q", "--with", "llvmlite", "python3", str(plank), "run", str(f)], capture_output=True, text=True, timeout=600).stdout

subprocess.run(["make", "-C", str(arch), "clean"], capture_output=True, timeout=60)
q = None
try:
    r = subprocess.run(["make", "-C", str(arch), "kernel8.elf"], capture_output=True, text=True, timeout=900)
    if r.returncode: print(r.stderr[-1500:]); print("FAIL: arch/arm64 does not build"); sys.exit(1)
    card = str(tmp / "card.img")
    build_card(card, False)
    MT = dict(os.environ, MTOOLS_SKIP_CHECK="1")
    def mt(*a): return subprocess.run([a[0], "-i", f"{card}@@{PART * 512}", *map(str, a[1:])], capture_output=True, env=MT)
    mt("mmd", "::JT"); mt("mmd", "::JT/APPS"); mt("mmd", "::JT/PC")   # PC: syscalls.pk's files
    for i, n in built: mt("mcopy", "-o", tmp / ("PK%d.APP" % i), "::JT/APPS/PK%d.APP" % i)
    for app, _, _ in PLANKC:
        if (tmp / (app.upper() + ".APP")).exists(): mt("mcopy", "-o", tmp / (app.upper() + ".APP"), "::JT/APPS/%s.APP" % app.upper())
    for e in ("app", "sig"): mt("mcopy", "-o", root / "landing/apps" / ("slide." + e), "::JT/APPS/SLIDE." + e.upper())   # 6.58.0
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
            time.sleep(0.1)
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
    def key(*k): command("send-key", keys=[{"type": "qcode", "data": x} for x in k]); time.sleep(0.12)
    key("f2")
    if not wait_for("terminal open", 10): raise SystemExit("FAIL: F2 did not open the Terminal")
    time.sleep(0.5)
    for i, n in built: wants["pk%d" % i] = (ex / (n + ".out")).read_text()
    for app, n in [("pk%d" % i, n) for i, n in built] + [(a, f.stem + " by plankc" + (" collecting every 64 KB" if every else "")) for a, f, every in PLANKC if (tmp / (a.upper() + ".APP")).exists()]:
        start = len(uart())
        for ch in "userrun " + app + WORDS.get(app, ""): key({" ": "spc"}.get(ch, ch))
        key("ret")
        wait_for("M3c app %s exit" % app, 60, start) or wait_for("M3c app %s stopped" % app, 1, start)
        time.sleep(0.3)
        seg = uart()[start:]
        a = seg.find("M3c app %s starts\n" % app); b = seg.find("M3c app %s " % app, a + 1 if a >= 0 else 0)
        body = seg[a:].split("\n", 1)[1] if a >= 0 else ""
        body = body[:body.find("M3c app %s exit" % app)] if "M3c app %s exit" % app in body else body
        got = [l for l in body.split("\n") if l and not l.startswith(("M3c ", "M3 ", "key ", "userrun ")) and "joshua@tree" not in l]
        want = [l for l in wants[app].split("\n") if l]
        check("%s on the box prints its .out" % n, got == want and ("M3c app %s exit 0" % app) in seg,
              {"want": want[:6], "got": got[:6], "tail": seg[-300:]} if got != want else seg[-300:])
    start = len(uart())   # 6.58.0: the store's Slide, written in Plank, in a window drawn through jtw
    for ch in "userrun slide": key({" ": "spc"}.get(ch, ch))
    key("ret")
    check("the store's Slide (Plank, signed) opens its window", wait_for("slide: window", 20, start) and "bytes, signed" in uart()[start:]
          and wait_for("userrun slide: running in its window", 10, start), uart()[start:][-400:])
    n = len(uart())
    for k in ("up", "left", "down", "right"): key(k); time.sleep(0.3)
    check("the arrows slide tiles", wait_for("slide: moved", 10, n), uart()[n:][-300:])
    n = len(uart()); key("esc")
    check("Esc closes Slide", wait_for("slide: closed", 10, n) and wait_for("M3c app slide exit 0", 10, n), uart()[n:][-300:])
    check("no kernel crash", "KERNEL CRASH" not in uart(), uart()[-600:])
finally:
    if q: q.kill(); q.wait()
    subprocess.run(["make", "-C", str(arch), "clean"], capture_output=True)
if fails: print("FAIL: " + ", ".join(fails)); sys.exit(1)
print("PASS: Plank's examples, compiled for the Pi by tools/plankapp.sh, print exactly what their .out files say")
