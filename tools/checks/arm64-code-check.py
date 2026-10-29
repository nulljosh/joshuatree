#!/usr/bin/env python3
"""Write, build and run a program on the Pi with nothing else (6.98.0): Code, the editor in store/code.pk.

The card carries plankc (tools/plankapp.sh --plankc, as tools/flash-pi.sh puts it there), its runtime as JT/PLANKC/RT.PK,
and Code's own source as JT/CODE.PK. In the Terminal:
  1. userrun plankc build CODE.PK: plankc on the box builds the editor into JT/APPS/CODE.APP.
  2. userrun code HI.PK: Code opens a new file in its window; a program is typed into it (Enter keeps the indent, so the
     lines come out indented as typed), Backspace takes back a wrong letter, and Esc saves it and closes.
  3. userrun plankc build HI.PK: plankc builds what was typed, and userrun hi prints its line.
After QEMU stops the card holds JT/HI.PK with exactly the program typed, and JT/APPS/CODE.APP and JT/APPS/HI.APP.
Discriminating: drop Code's Backspace branch and HI.PK keeps the wrong letter, so the program prints the wrong line; drop
the save on Esc and HI.PK is never written, so plankc has nothing to build.
Skips (exit 0) without clang's aarch64 target, ld.lld, qemu-system-aarch64, mtools, uv or Plank (PLANK=, as plankapp.sh).
"""
import json, os, shutil, socket, subprocess, sys, time
from pathlib import Path
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from scratch import scratch_dir
from sdcard import PART, build_card

root = Path(__file__).resolve().parents[2]
arch = root / "arch/arm64"
plank = Path(os.environ.get("PLANK", Path.home() / "Documents/Code/plank/plank.py"))
if not all(shutil.which(t) for t in ("clang", "ld.lld", "qemu-system-aarch64", "mmd", "mcopy", "mtype", "uv")) or not plank.exists():
    print("SKIP: clang, ld.lld, qemu-system-aarch64, mtools, uv or Plank (%s) missing" % plank); sys.exit(0)
tmp = Path(scratch_dir("arm64-code"))
for p in tmp.iterdir(): p.unlink() if p.is_file() else shutil.rmtree(p)
fails = []
def check(name, ok, detail=""):
    print(("  ok: " if ok else "  FAIL: ") + name + ("" if ok else "  " + repr(detail)[-700:]), flush=True)
    if not ok: fails.append(name)

r = subprocess.run([str(root / "tools/plankapp.sh"), "--plankc", str(root / "user/plankc/plankc.pk"), str(tmp / "PLANKC.APP")], capture_output=True, text=True, timeout=900)
if r.returncode: print((r.stdout + r.stderr)[-1500:]); print("FAIL: plankc does not build into PLANKC.APP"); sys.exit(1)
WANT = 'fn main() {\n  print("hi from the pi")\n}'

subprocess.run(["make", "-C", str(arch), "clean"], capture_output=True, timeout=60)
q = None
try:
    r = subprocess.run(["make", "-C", str(arch), "kernel8.elf"], capture_output=True, text=True, timeout=900)
    if r.returncode: print(r.stderr[-1500:]); print("FAIL: arch/arm64 does not build"); sys.exit(1)
    card = str(tmp / "card.img"); build_card(card, False)
    MT = dict(os.environ, MTOOLS_SKIP_CHECK="1")
    def mt(*a): return subprocess.run([a[0], "-i", f"{card}@@{PART * 512}", *map(str, a[1:])], capture_output=True, env=MT)
    mt("mmd", "::JT"); mt("mmd", "::JT/APPS"); mt("mmd", "::JT/PLANKC")
    mt("mcopy", "-o", tmp / "PLANKC.APP", "::JT/APPS/PLANKC.APP")
    mt("mcopy", "-o", root / "user/plankc/rt.pk", "::JT/PLANKC/RT.PK")
    mt("mcopy", "-o", root / "store/code.pk", "::JT/CODE.PK")
    log, sock = tmp / "uart", tmp / "qmp"
    q = subprocess.Popen(["qemu-system-aarch64", "-machine", "virt", "-cpu", "cortex-a72", "-m", "256", "-global", "virtio-mmio.force-legacy=false",
                          "-device", "ramfb", "-device", "virtio-keyboard-device", "-drive", "if=none,id=card,file=%s,format=raw" % card, "-device", "virtio-blk-device,drive=card",
                          "-display", "none", "-serial", "file:%s" % log, "-qmp", "unix:%s,server,nowait" % sock, "-kernel", str(arch / "kernel8.elf")],
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    def uart(): return log.read_text(errors="replace") if log.exists() else ""
    def wait_any(texts, secs, after=0):
        end = time.time() + secs
        while time.time() < end:
            seg = uart()[after:]
            if any(t in seg for t in texts): return True
            if q.poll() is not None: return False
            time.sleep(0.2)
        return False
    if not wait_any(["M2 input ready"], 60): raise SystemExit("FAIL: did not boot: %r" % uart()[-600:])
    time.sleep(0.5)
    s = socket.socket(socket.AF_UNIX); s.settimeout(30); s.connect(str(sock)); f = s.makefile("rw"); f.readline()
    def command(name, **a):
        f.write(json.dumps({"execute": name, "arguments": a}) + "\n"); f.flush()
        while True:
            rr = json.loads(f.readline())
            if "return" in rr or "error" in rr: return rr
    command("qmp_capabilities")
    KEYS = {" ": ("spc",), "/": ("slash",), ".": ("dot",), "(": ("shift", "9"), ")": ("shift", "0"), "{": ("shift", "bracket_left"),
            "}": ("shift", "bracket_right"), '"': ("shift", "apostrophe"), "\n": ("ret",), "\b": ("backspace",)}
    def key(*k): command("send-key", keys=[{"type": "qcode", "data": x} for x in k]); time.sleep(0.12)
    def typed(text):
        for ch in text:
            if ch in KEYS: key(*KEYS[ch])
            elif ch.isupper(): key("shift", ch.lower())
            else: key(ch)
    def run(line, prog, secs):
        n = len(uart())
        typed(line + "\n")
        done = wait_any(["M3c app %s exit" % prog, "M3c app %s stopped" % prog], secs, n)
        return uart()[n:], done
    key("f2")
    if not wait_any(["terminal open"], 30): raise SystemExit("FAIL: F2 did not open the Terminal: %r" % uart()[-600:])
    time.sleep(0.5)

    seg, done = run("userrun plankc build CODE.PK", "plankc", 600)
    check("plankc builds the editor on the box: APPS/CODE.APP", done and "plankc: wrote APPS/CODE.APP" in seg and "M3c app plankc exit 0" in seg, seg[-500:])

    n = len(uart())
    typed("userrun code HI.PK\n")
    check("Code opens a new file in its window", wait_any(["code: open HI.PK"], 30, n) and wait_any(["M3c window code first frame"], 10, n), uart()[n:][-400:])
    time.sleep(0.5)
    typed('fn main() {\n  print("hi from the piX\b")\n')   # an X typed by mistake and taken back
    key("backspace"); key("backspace")   # the new line came out indented; the closing brace goes at the start
    typed("}")
    n = len(uart()); key("esc")
    check("Esc saves the file and closes", wait_any(["code: closed"], 20, n) and "code: saved HI.PK" in uart()[n:] and wait_any(["M3c app code exit 0"], 10, n), uart()[n:][-400:])

    seg, done = run("userrun plankc build HI.PK", "plankc", 300)
    check("plankc builds the program typed in Code", done and "plankc: wrote APPS/HI.APP" in seg and "M3c app plankc exit 0" in seg, seg[-500:])
    seg, done = run("userrun hi", "hi", 60)
    check("and it runs: hi from the pi", done and "hi from the pi" in seg and "M3c app hi exit 0" in seg, seg[-400:])
    check("no kernel crash", "KERNEL CRASH" not in uart(), uart()[-600:])
    q.kill(); q.wait(); q = None
    got = mt("mtype", "::JT/HI.PK").stdout.decode(errors="replace").replace("\r", "")
    check("the card holds JT/HI.PK, exactly the program typed", got.rstrip("\n") == WANT, got)
    check("and JT/APPS/CODE.APP and JT/APPS/HI.APP", mt("mtype", "::JT/APPS/CODE.APP").returncode == 0 and mt("mtype", "::JT/APPS/HI.APP").returncode == 0)
finally:
    if q: q.kill(); q.wait()
    subprocess.run(["make", "-C", str(arch), "clean"], capture_output=True, timeout=120)
if fails: print("FAIL: " + ", ".join(fails)); sys.exit(1)
print("PASS: on the Pi alone, plankc builds Code, Code writes a program, plankc builds it and it runs")
