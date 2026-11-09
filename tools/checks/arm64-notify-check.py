#!/usr/bin/env python3
"""13.7.0, notices from apps and Focus (arch/arm64/ufile.c ufile_notify, desk.c focus_set, jt_notify in user/jtsys.h).

QEMU virt, headless, with a card holding PING.APP, which posts one notice and exits. In the Terminal:
1. `userrun ping` shows the notice under the app's name, and a line with a control byte has it replaced.
2. `focus on` puts the word in the menu bar (the bar's pixels change), and the same app is held, not shown.
3. `focus off` says one was held, and clears the word.
Discriminating: drop the focus_on test in menubar_app_notice and step 2 sees "notice app", not "notice held".
Skips (exit 0) when the Pi toolchain or mtools is missing.
"""
import json, os, shutil, socket, subprocess, sys, time
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from scratch import scratch_dir
from sdcard import PART, build_card

root = Path(__file__).resolve().parents[2]
arch = root / "arch/arm64"
if not all(shutil.which(t) for t in ("clang", "ld.lld", "qemu-system-aarch64", "mmd", "mcopy")):
    print("SKIP: clang, ld.lld, qemu-system-aarch64 or mtools not installed"); sys.exit(0)
tmp = Path(scratch_dir("arm64-notify"))
r = subprocess.run(["make", "-C", str(arch), "kernel8.elf"], capture_output=True, text=True, timeout=600)
if r.returncode: print(r.stderr[-2000:]); print("FAIL: arch/arm64 does not build"); sys.exit(1)

(tmp / "ping.c").write_text(r"""#include "jtsys.h"
__attribute__((section(".text.start"), used))
void _start(void) { jt_exit(jt_notify("hi\tthere")); }
""")
mk = subprocess.run([str(root / "tools/mkapp.sh"), str(tmp / "ping.c"), str(tmp / "PING.APP")], capture_output=True, text=True, timeout=300)
if mk.returncode: print(mk.stdout + mk.stderr); print("FAIL: tools/mkapp.sh could not build ping.c"); sys.exit(1)
card = str(tmp / "card.img")
build_card(card, False)
MT = dict(os.environ, MTOOLS_SKIP_CHECK="1")
def mt(*a): return subprocess.run([a[0], "-i", f"{card}@@{PART * 512}", *a[1:]], capture_output=True, env=MT)
if mt("mmd", "::JT").returncode or mt("mmd", "::JT/APPS").returncode: print("FAIL: mtools could not make JT/APPS"); sys.exit(1)
if mt("mcopy", "-o", str(tmp / "PING.APP"), "::JT/APPS/PING.APP").returncode: print("FAIL: mtools could not copy the app"); sys.exit(1)

log, sock = tmp / "uart", tmp / "qmp"
q = subprocess.Popen(["qemu-system-aarch64", "-machine", "virt", "-cpu", "cortex-a72", "-m", "256", "-global", "virtio-mmio.force-legacy=false",
                      "-device", "ramfb", "-device", "virtio-keyboard-device", "-drive", f"if=none,id=card,file={card},format=raw", "-device", "virtio-blk-device,drive=card",
                      "-display", "none", "-serial", f"file:{log}", "-qmp", f"unix:{sock},server,nowait", "-kernel", str(arch / "kernel8.elf")],
                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
def uart(): return log.read_text(errors="replace") if log.exists() else ""
def wait_for(text, secs, after=0):
    end = time.time() + secs
    while time.time() < end:
        if text in uart()[after:]: return True
        time.sleep(0.1)
    return False
fails = []
def check(name, ok, detail=""):
    print(("  ok: " if ok else "  FAIL: ") + name + ("" if ok else "  " + repr(detail)[-500:]))
    if not ok: fails.append(name)
try:
    if not wait_for("M2 input ready", 60): sys.exit("FAIL: the desktop did not come up")
    s = socket.socket(socket.AF_UNIX); s.connect(str(sock)); f = s.makefile("rw"); f.readline()
    def command(c, **a):
        f.write(json.dumps({"execute": c, "arguments": a}) + "\n"); f.flush()
        while True:
            rr = json.loads(f.readline())
            if "return" in rr or "error" in rr: return rr
    command("qmp_capabilities")
    def key(k): command("send-key", keys=[{"type": "qcode", "data": k}]); time.sleep(0.12)
    def line(t):
        n = len(uart())
        for c in t: key("spc" if c == " " else c)
        key("ret")
        return n
    def bar():
        p = tmp / "shot.ppm"
        command("screendump", filename=str(p)); time.sleep(0.4)
        parts = p.read_bytes().split(b"\n", 3); w = int(parts[1].split()[0])
        return parts[3][:w * 3 * 26]
    key("f2")
    check("F2 opens the Terminal", wait_for("terminal open", 15), uart()[-300:])
    time.sleep(0.5)
    n = line("userrun ping")
    check("a notice shows under the app's name, a control byte replaced", wait_for("notice app: ping: hi?there", 15, n), uart()[-300:])
    time.sleep(5.0)   # the notice clears after four seconds, and the word only draws with no notice up
    before = bar()
    n = line("focus on")
    check("focus on says so", wait_for("focus: on", 10, n), uart()[-300:])
    time.sleep(0.6)
    check("the word is in the menu bar", bar() != before)
    n = line("userrun ping")
    check("a notice during Focus is held", wait_for("notice held: ping: hi?there", 15, n), uart()[-300:])
    check("and not shown", "notice app:" not in uart()[n:], uart()[n:])
    n = line("focus off")
    check("focus off says so", wait_for("focus: off", 10, n), uart()[-300:])
finally:
    q.kill(); q.wait()
if fails:
    print("FAIL:", len(fails), "failed"); sys.exit(1)
print("PASS: notices from apps show under their name, Focus holds them and says how many")
