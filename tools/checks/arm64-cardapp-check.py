#!/usr/bin/env python3
"""Apps from the card (6.51.0, the first piece of 8.0: an app that is not built into the kernel).

Builds the virt kernel, puts three files in JT/APPS/ on the check card (sdcard.py) with mtools, boots with the card as a
virtio disk, and at the Terminal:
  1. `userrun hi` runs HI.APP, a copy of the Makefile's uapp_hello.bin that the kernel's own table does not list: the card
     app is read (`M3c card app JT/APPS/HI.APP`), loads and runs at EL0 (`jt-hello: start`) and exits.
  1b. 6.52.0, the SDK: tools/mkapp.sh builds sun.c, a C file outside the tree that calls libjt's strlen, into SUN.APP;
     `userrun sun` prints its line and exits 7. mkapp refuses a name over eight characters.
  2. `userrun bad` is refused: BAD.APP has a good trailer but a relocation that points 4 GB past its image. uimg_ok says no
     before a page is touched (`M3c load FAIL bad: not a user image`).
  2b. `userrun evil` is refused: each of EVIL.APP's relocations is in bounds, but the first rewrites the second.
  3. `userrun big` is refused: BIG.APP is over the 256 KiB an app may be.
  4. `userrun a.b` is no program at all: a card name is letters, digits, - and _, eight at most.
  5. `userrun hi` again: the kernel carried on.
  6. 6.53.0, Spotlight: F1 and "su" lists the card's Sun (SUN.APP) and Enter runs it to exit 7. TERMINAL.APP and CALENDAR.APP,
     on the card too, are neither listed beside the Terminal and Calendar nor run by `userrun terminal`: a name Spotlight
     already lists is that app's.
  7. 6.56.0, the store's apps as landing/apps serves them, signed by the shipped key: `userrun fortune` prints its line; `userrun dice`
     opens its window, Space rolls ("dice: rolled"), Esc closes it. No KERNEL CRASH.
Discriminating: drop the card loop in keys.c's spot_paint and step 6 finds no Sun; make main.c's name_taken return 0 and
the Terminal is listed twice; drop the uapp_sdk.o rule from the Makefile and step 1b fails; make card_find in main.c return 0 and step 1 fails; take the relocation test out of uimg_ok and step 2
writes past the app's pages and the kernel crashes; take out its list rule and step 2b does.
Skips (exit 0) when clang's aarch64 target, ld.lld, qemu-system-aarch64 or mtools is missing.
Usage: tools/checks/arm64-cardapp-check.py   (from the repo root)
"""
import json, os, shutil, socket, struct, subprocess, sys, time
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from scratch import scratch_dir
from sdcard import PART, build_card

root = Path(__file__).resolve().parents[2]
arch = root / "arch/arm64"
if not all(shutil.which(t) for t in ("clang", "ld.lld", "qemu-system-aarch64", "mmd", "mcopy")):
    print("SKIP: clang, ld.lld, qemu-system-aarch64 or mtools not installed"); sys.exit(0)
tmp = Path(scratch_dir("arm64-cardapp"))

fails = []
def check(name, ok, detail=""):
    print(("  ok: " if ok else "  FAIL: ") + name + ("" if ok else "  " + repr(detail)[-600:]))
    if not ok: fails.append(name)

subprocess.run(["make", "-C", str(arch), "clean"], capture_output=True, timeout=60)
r = subprocess.run(["make", "-C", str(arch), "kernel8.elf"], capture_output=True, text=True, timeout=600)
if r.returncode: print(r.stdout[-2000:], r.stderr[-2000:]); print("FAIL: arch/arm64 does not build"); sys.exit(1)

card = str(tmp / "card.img")
build_card(card, False)
MT = dict(os.environ, MTOOLS_SKIP_CHECK="1")
def mt(*a): return subprocess.run([a[0], "-i", f"{card}@@{PART * 512}", *a[1:]], capture_output=True, env=MT)
bad = bytes(4096) + struct.pack("<QQQ", 0xFFFFFFF0, 1027, 0) + struct.pack("<IIII", 0x3155544a, 4096, 4096, 4096 + 24)
(tmp / "BAD.APP").write_bytes(bad)
# EVIL.APP: every relocation in bounds, but the first rewrites the second's offset to base + 8 (the base is the app's
# address, not 0), so applying the second would write far outside the app (what uimg_ok's list rule stops; fuzz_uimg.c found it)
evil = bytes(4096) + struct.pack("<QQQQQQ", 4096 + 24, 1027, 8, 0, 1027, 0) + struct.pack("<IIII", 0x3155544a, 4096, 4096, 4096 + 48)
(tmp / "EVIL.APP").write_bytes(evil)
(tmp / "BIG.APP").write_bytes(bytes(300 * 1024))
shutil.copy(arch / "uapp_hello.bin", tmp / "HI.APP")
for f in ("TERMINAL.APP", "CALENDAR.APP"): shutil.copy(arch / "uapp_hello.bin", tmp / f)   # 6.53.0: names that are taken
STORE = [("FORTUNE", "fortune"), ("DICE", "dice")]   # 6.56.0: the store's apps, the very files landing/apps serves, signed by the shipped key
for up, low in STORE:
    for ext in ("app", "sig"): shutil.copy(root / "landing/apps" / (low + "." + ext), tmp / (up + "." + ext.upper()))
# 6.52.0, the SDK: tools/mkapp.sh builds one C file of nobody's into SUN.APP (a libjt call, a write, exit 7)
(tmp / "sun.c").write_text("""#include "jtsys.h"
#include "libjt/string.h"
__attribute__((section(".text.start"), used))
void _start(void) {
    static const char m[] = "sun: made with the sdk\\n";
    jt_write(1, m, (unsigned)strlen(m));
    jt_exit(7);
}
""")
mk = subprocess.run([str(root / "tools/mkapp.sh"), str(tmp / "sun.c"), str(tmp / "SUN.APP")], capture_output=True, text=True, timeout=300)
check("tools/mkapp.sh builds sun.c into SUN.APP", mk.returncode == 0 and (tmp / "SUN.APP").exists() and (tmp / "SUN.APP").read_bytes()[-16:-12] == b"JTU1", mk.stdout + mk.stderr)
mk = subprocess.run([str(root / "tools/mkapp.sh"), str(tmp / "sun.c"), str(tmp / "TOOLONGNAME.APP")], capture_output=True, text=True, timeout=60)
check("tools/mkapp.sh refuses a name the card cannot take", mk.returncode != 0 and "1 to 8" in mk.stdout, mk.stdout + mk.stderr)
if mt("mmd", "::JT").returncode or mt("mmd", "::JT/APPS").returncode: print("FAIL: mtools could not make JT/APPS on the check card"); sys.exit(1)
for f in ("HI.APP", "SUN.APP", "BAD.APP", "EVIL.APP", "BIG.APP", "TERMINAL.APP", "CALENDAR.APP", "FORTUNE.APP", "FORTUNE.SIG", "DICE.APP", "DICE.SIG"):
    if mt("mcopy", "-o", str(tmp / f), "::JT/APPS/" + f).returncode: print("FAIL: mtools could not copy " + f); sys.exit(1)

log, sock = tmp / "uart", tmp / "qmp"
q = subprocess.Popen(["qemu-system-aarch64", "-machine", "virt", "-cpu", "cortex-a72", "-m", "256", "-global", "virtio-mmio.force-legacy=false",
                      "-device", "ramfb", "-device", "virtio-keyboard-device", "-drive", "if=none,id=card,file=%s,format=raw" % card, "-device", "virtio-blk-device,drive=card",
                      "-display", "none", "-serial", "file:" + str(log), "-qmp", "unix:%s,server,nowait" % sock, "-kernel", str(arch / "kernel8.elf")],
                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
def uart(): return log.read_text(errors="replace") if log.exists() else ""
def wait_for(text, secs, after=0):
    end = time.time() + secs
    while time.time() < end:
        if text in uart()[after:]: return True
        if q.poll() is not None: return False
        time.sleep(0.1)
    return False
try:
    if not wait_for("M2 input ready", 60): raise SystemExit("FAIL: did not boot: %r" % uart()[-600:])
    time.sleep(0.5)
    s = socket.socket(socket.AF_UNIX); s.settimeout(30); s.connect(str(sock)); f = s.makefile("rw"); f.readline()
    def command(name, **a):
        f.write(json.dumps({"execute": name, "arguments": a}) + "\n"); f.flush()
        while True:
            rr = json.loads(f.readline())
            if "return" in rr or "error" in rr: return rr
    command("qmp_capabilities")
    QK = {" ": ("spc",), ".": ("dot",)}
    def key(*k): command("send-key", keys=[{"type": "qcode", "data": x} for x in k]); time.sleep(0.12)
    def say(t, until, secs=15):
        n = len(uart())
        for ch in t: key(*QK.get(ch, (ch,)))
        key("ret"); wait_for(until, secs, n); time.sleep(0.3)
        return uart()[n:]
    key("f2")
    if not wait_for("terminal open", 10): raise SystemExit("FAIL: F2 did not open the Terminal: %r" % uart()[-300:])
    time.sleep(0.5)
    out = say("userrun hi", "M3c app hi exit")
    check("userrun hi reads JT/APPS/HI.APP, loads it and runs it at EL0 to its exit",
          "M3c card app JT/APPS/HI.APP: %d bytes" % (arch / "uapp_hello.bin").stat().st_size in out and "M3c load hi: " in out
          and "jt-hello: start" in out and "M3c app hi exit" in out, out)
    out = say("userrun sun", "M3c app sun exit")
    check("userrun sun runs the SDK's app: its line, then exit 7", "sun: made with the sdk\n" in out and "M3c app sun exit 7" in out, out)
    out = say("userrun bad", "userrun bad: ")
    check("a bad relocation is refused before it loads", "M3c load FAIL bad: not a user image" in out and "userrun bad: would not load" in out, out)
    out = say("userrun evil", "userrun evil: ")
    check("a relocation that rewrites the next one is refused", "M3c load FAIL evil: not a user image" in out and "userrun evil: would not load" in out, out)
    out = say("userrun big", "userrun big: ")
    check("an image over 256 KiB is refused", "BIG.APP: too big" in out and "userrun big: would not load" in out, out)
    out = say("userrun a.b", "userrun: no program")
    check("a name that is not a card name is no program", "userrun: no program called that" in out and "M3c card app" not in out, out)
    out = say("userrun hi", "M3c app hi exit")
    check("the kernel carried on: hi runs again", "jt-hello: start" in out and "M3c app hi exit" in out, out)
    out = say("userrun terminal", "userrun: no program")
    check("a card app may not take a name Spotlight lists (TERMINAL.APP is not the Terminal)", "userrun: no program called that" in out and "M3c card app" not in out, out)
    key("esc"); time.sleep(0.5)
    def spot(text, until):
        n = len(uart()); key("f1"); wait_for("spotlight open", 10, n)
        for ch in text: key(ch)
        wait_for(until, 10, n); time.sleep(0.3)
        return n
    n = spot("termin", "spotlight: termin ")
    check("Spotlight lists the Terminal once, not TERMINAL.APP beside it", "spotlight: termin 1\n" in uart()[n:], uart()[n:][-300:])
    key("esc"); time.sleep(0.3)
    n = spot("calend", "spotlight: calend ")
    check("and Calendar once, not CALENDAR.APP", "spotlight: calend 1\n" in uart()[n:], uart()[n:][-300:])
    key("esc"); time.sleep(0.3)
    n = spot("su", "spotlight: su ")
    hits = uart()[n:]
    key("ret"); wait_for("M3c app sun exit", 15, n); time.sleep(0.3)
    out = uart()[n:]
    check("Spotlight lists the card's Sun, and Enter runs it to exit 7", "spotlight: su " in hits and "M3c card app JT/APPS/SUN.APP" in out
          and "M3c app sun starts" in out and "M3c app sun exit 7" in out, out[-600:])
    key("f2"); time.sleep(0.5)
    out = say("userrun fortune", "M3c app fortune exit")
    check("the store's fortune, as served, runs signed by the shipped key", "JT/APPS/FORTUNE.APP" in out and "bytes, signed" in out and "fortune: " in out and "M3c app fortune exit 0" in out, out)
    out = say("userrun dice", "userrun dice: ")
    check("the store's dice opens its window", "dice: ring-3 window" in out and "userrun dice: running in its window" in out and "bytes, signed" in out, out)
    n = len(uart()); key("spc"); wait_for("dice: rolled", 10, n); time.sleep(0.5)
    command("screendump", filename=str(tmp / "dice.ppm"))
    check("Space rolls the dice", "dice: rolled " in uart()[n:], uart()[n:][-300:])
    n = len(uart()); key("esc")
    check("Esc closes dice", wait_for("dice: closed", 10, n) and wait_for("M3c app dice exit 0", 10, n), uart()[n:][-300:])
    check("no kernel crash", "KERNEL CRASH" not in uart(), uart()[-600:])
finally:
    q.kill(); q.wait()
    subprocess.run(["make", "-C", str(arch), "clean"], capture_output=True)
if fails: print("FAIL: " + ", ".join(fails)); sys.exit(1)
print("PASS: an app on the card in JT/APPS runs at EL0, and a bad, oversized or misnamed one is refused")
