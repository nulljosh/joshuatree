#!/usr/bin/env python3
"""7.2.0, the debugger: `debug NAME [STEPS]` in the Terminal runs an app one instruction at a time (main.c dbg_step).

Boots the ARM64 kernel in QEMU with a card holding three apps, opens the Terminal and checks:
1. `debug hi 6` shows six steps, numbered, each with where the app is (+offset into its image), the instruction there and the
   registers that changed, then every register, the bytes at sp, and the app runs on to its own exit.
2. `debug peek 5`: PEEK.APP points sp at the kernel's own page beside it (UP_KERN, where KERNEL-ONLY-SECRET lives) and jumps
   there. The step after the jump shows ???????? for the instruction and ?? for the stack, never the secret's bytes, and the
   app is stopped by its fault while the kernel carries on.
3. `debug quit`: QUIT.APP exits on its third instruction, with steps to spare, and the debugger says it ended. `userrun hi`
   straight after is not stepped: no step lines, and it exits as it always does (MDSCR's SS is off again).
Discriminating: take SS out of resume_user's SPSR (vectors.S) and the first step comes before any instruction ran (steps 1
and 2); read memory with no dbg_ok and step 2 prints the secret; leave dbg_end out of app_tick and step 3's hi is stepped.
Skips (exit 0) when clang's aarch64 target, ld.lld, qemu-system-aarch64 or mtools is missing.
Usage: tools/checks/arm64-debug-check.py   (from the repo root)
"""
import json, os, re, shutil, socket, subprocess, sys, time
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from scratch import scratch_dir
from sdcard import PART, build_card

root = Path(__file__).resolve().parents[2]
arch = root / "arch/arm64"
if not all(shutil.which(t) for t in ("clang", "ld.lld", "qemu-system-aarch64", "mmd", "mcopy")):
    print("SKIP: clang, ld.lld, qemu-system-aarch64 or mtools not installed"); sys.exit(0)
tmp = Path(scratch_dir("arm64-debug"))

fails = []
def check(name, ok, detail=""):
    print(("  ok: " if ok else "  FAIL: ") + name + ("" if ok else "  " + repr(detail)[-900:]))
    if not ok: fails.append(name)

subprocess.run(["make", "-C", str(arch), "clean"], capture_output=True, timeout=60)
r = subprocess.run(["make", "-C", str(arch), "kernel8.elf"], capture_output=True, text=True, timeout=600)
if r.returncode: print(r.stdout[-2000:], r.stderr[-2000:]); print("FAIL: arch/arm64 does not build"); sys.exit(1)

# PEEK: its image starts at the app window, UP_APP (0x10000) into its arena; the kernel's page is UP_KERN (0x4000) into it
(tmp / "peek.c").write_text("""__attribute__((section(".text.start"), used, naked))
void _start(void) {
    __asm__ volatile ("adr x9, _start\\n sub x9, x9, #0x10000\\n add x9, x9, #0x4000\\n mov sp, x9\\n br x9\\n");
}
""")
# QUIT: exit 3 (JT_SYS_EXIT is 1) on its third instruction
(tmp / "quit.c").write_text("""__attribute__((section(".text.start"), used, naked))
void _start(void) {
    __asm__ volatile ("mov x0, #3\\n mov x8, #1\\n svc #0\\n");
}
""")
for app in ("peek", "quit"):
    mk = subprocess.run([str(root / "tools/mkapp.sh"), str(tmp / (app + ".c")), str(tmp / (app.upper() + ".APP"))], capture_output=True, text=True, timeout=300)
    if mk.returncode: print(mk.stdout + mk.stderr); print("FAIL: tools/mkapp.sh could not build " + app + ".c"); sys.exit(1)
card = str(tmp / "card.img")
build_card(card, False)
MT = dict(os.environ, MTOOLS_SKIP_CHECK="1")
def mt(*a): return subprocess.run([a[0], "-i", f"{card}@@{PART * 512}", *a[1:]], capture_output=True, env=MT)
shutil.copy(arch / "uapp_hello.bin", tmp / "HI.APP")
if mt("mmd", "::JT").returncode or mt("mmd", "::JT/APPS").returncode: print("FAIL: mtools could not make JT/APPS"); sys.exit(1)
for f in ("HI.APP", "PEEK.APP", "QUIT.APP"):
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
    QK = {" ": ("spc",)}
    def key(*k): command("send-key", keys=[{"type": "qcode", "data": x} for x in k]); time.sleep(0.12)
    def say(t, until, secs=20):
        n = len(uart())
        for ch in t: key(*QK.get(ch, (ch,)))
        key("ret"); wait_for(until, secs, n); time.sleep(0.5)
        return uart()[n:]
    key("f2")
    if not wait_for("terminal open", 10): raise SystemExit("FAIL: F2 did not open the Terminal: %r" % uart()[-300:])
    time.sleep(0.5)
    STEP = re.compile(r"^step (\d+)  (\+[0-9a-f]{4}|0x[0-9a-f]+)  ([0-9a-f]{8}|\?{8})(.*)$", re.M)

    out = say("debug hi 6", "M3c app hi exit")
    steps = STEP.findall(out)
    check("debug hi 6 shows six numbered steps, the first after hi's first instruction ran", [int(x[0]) for x in steps] == [1, 2, 3, 4, 5, 6]
          and steps[0][1] != "+0000", out)
    check("each step is in hi's own image, with its instruction", all(x[1].startswith("+") and "?" not in x[2] for x in steps)
          and len(set(x[1] for x in steps)) > 1, steps)
    check("steps name the registers that changed", any(re.search(r" (x\d+|sp)=[0-9a-f]+", x[3]) for x in steps), steps)
    check("the last step shows the registers and the bytes at sp", "registers:" in out and " pc=+" in out
          and re.search(r"^at sp\+00:( [0-9a-f]{2}){16}$", out, re.M) and re.search(r"^at sp\+10:", out, re.M), out)
    check("then hi runs on to its own exit", "debug: done stepping, it runs on" in out and "jt-hello: start" in out
          and out.index("debug: done stepping") < out.index("M3c app hi exit"), out)

    out = say("debug peek 5", "kernel carried on")
    steps = STEP.findall(out)
    check("debug peek 5 steps to its jump into the kernel's page", [int(x[0]) for x in steps] == [1, 2, 3, 4, 5]
          and steps[-1][1].startswith("0x") and steps[-1][2] == "????????", steps)
    check("the kernel's page is never shown: ?? at sp, no secret", re.search(r"^at sp\+00:( \?\?){16}$", out, re.M)
          and "KERNEL-ONLY-SECRET" not in out and "4b45524e" not in out and "4e52454b" not in out
          and "4b 45 52 4e" not in out, out)
    check("peek is stopped by its fault and the kernel carries on", "M3 EL0 fault" in out and "KERNEL CRASH" not in uart(), out[-900:])

    out = say("debug quit", "M3c app quit exit")
    check("debug quit: two steps, then it exits with steps to spare and the debugger says so", [int(x[0]) for x in STEP.findall(out)] == [1, 2]
          and "debug: it ended after 2 steps, stepping stops" in out and "M3c app quit exit 3" in out, out)
    out = say("userrun hi", "M3c app hi exit")
    check("userrun hi after debugging is not stepped and exits clean", "jt-hello: start" in out and "M3c app hi exit" in out
          and not STEP.search(out) and "exception class" not in out, out)
    check("no kernel crash", "KERNEL CRASH" not in uart() and q.poll() is None, uart()[-600:])
finally:
    q.kill(); q.wait()
    subprocess.run(["make", "-C", str(arch), "clean"], capture_output=True, timeout=60)
if fails: print("FAIL: %d of the debugger's checks" % len(fails)); sys.exit(1)
print("PASS: debug steps an app, shows its registers and stack, never the kernel's memory, and leaves the next run alone")
