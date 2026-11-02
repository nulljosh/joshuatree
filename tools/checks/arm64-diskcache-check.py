#!/usr/bin/env python3
"""8.8.0, the block cache: fat.c's sector reads come from a 128-sector write-through cache in emmc.c when they can.

Boots the ARM64 kernel in QEMU with the check card as a virtio disk and drives the Terminal:
1. `write cache.txt ...` then `cat jt/cache.txt` and `disk`: the counts so far. Then `cat jt/cache.txt` five more times and `disk`
   again: not one more sector came from the card, and the cache answered more (a repeated read costs the card nothing).
2. `write keep.txt ...` and QEMU is killed at once, with no shutdown: the card image, read back with mtools, has KEEP.TXT
   with exactly those bytes, so a write through the cache is on the card when the call returns (4.13.0's crash safety holds).
3. A second boot from that card reads both files back with `cat` (the cache starts empty and the card is the truth).
Discriminating: take the bc_find lookup out of blockdev_read_sector and every cat reads the card again (1); make the
cache write-back (bc_put without emmc_write) and KEEP.TXT is missing or empty on the card (2).
Skips (exit 0) when clang, ld.lld, qemu-system-aarch64 or mtools is missing.
Usage: tools/checks/arm64-diskcache-check.py   (from the repo root)
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
tmp = Path(scratch_dir("arm64-diskcache"))

fails = []
def check(name, ok, detail=""):
    print(("  ok: " if ok else "  FAIL: ") + name + ("" if ok else "  " + repr(detail)[-900:]))
    if not ok: fails.append(name)

subprocess.run(["make", "-C", str(arch), "clean"], capture_output=True, timeout=60)
r = subprocess.run(["make", "-C", str(arch), "kernel8.elf"], capture_output=True, text=True, timeout=600)
if r.returncode: print(r.stdout[-2000:], r.stderr[-2000:]); print("FAIL: arch/arm64 does not build"); sys.exit(1)

card = str(tmp / "card.img")
build_card(card, False)
MT = dict(os.environ, MTOOLS_SKIP_CHECK="1")
def mt(*a): return subprocess.run([a[0], "-i", f"{card}@@{PART * 512}", *a[1:]], capture_output=True, env=MT)

QK = {" ": ("spc",), ".": ("dot",), "/": ("slash",)}
def boot_once(tag):
    log, sock = tmp / ("uart-" + tag), tmp / ("qmp-" + tag)
    for p_ in (log, sock):
        if p_.exists(): p_.unlink()
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
    if not wait_for("M2 input ready", 60): q.kill(); raise SystemExit("FAIL: did not boot: %r" % uart()[-600:])
    time.sleep(0.5)
    s = socket.socket(socket.AF_UNIX); s.settimeout(30); s.connect(str(sock)); f = s.makefile("rw"); f.readline()
    def command(name, **a):
        f.write(json.dumps({"execute": name, "arguments": a}) + "\n"); f.flush()
        while True:
            rr = json.loads(f.readline())
            if "return" in rr or "error" in rr: return rr
    command("qmp_capabilities")
    def key(*k): command("send-key", keys=[{"type": "qcode", "data": x} for x in k]); time.sleep(0.12)
    def say(t, until, secs=20):
        n = len(uart())
        for ch in t: key(*QK.get(ch, (ch,)))
        key("ret"); wait_for(until, secs, n); time.sleep(0.4)
        return uart()[n:]
    key("f2")
    if not wait_for("terminal open", 10): q.kill(); raise SystemExit("FAIL: F2 did not open the Terminal: %r" % uart()[-300:])
    time.sleep(0.5)
    return q, uart, say
DISK = re.compile(r"disk: (\d+) sectors from the cache, (\d+) read from the card, (\d+) written through")
q, uart, say = boot_once("one")
try:
    say("write cache.txt one two three", "wrote JT/")
    o1 = say("cat jt/cache.txt", "one two three")
    d1 = DISK.search(say("disk", "written through"))
    for _ in range(5): say("cat jt/cache.txt", "one two three")
    d2 = DISK.search(say("disk", "written through"))
    check("cat reads the file back", "one two three" in o1, o1[-300:])
    if d1 and d2:
        h1, r1, w1 = map(int, d1.groups()); h2, r2, w2 = map(int, d2.groups())
        check("five more cats read nothing more from the card (%d then %d)" % (r1, r2), r2 == r1, (d1.groups(), d2.groups()))
        check("and the cache answered them (%d then %d hits)" % (h1, h2), h2 > h1 + 5, (d1.groups(), d2.groups()))
        check("the write went through to the card (%d sectors written)" % w1, w1 > 0, d1.groups())
    else: check("disk reports its counts", False, uart()[-400:])
    say("write keep.txt kept through the cut", "wrote JT/")
    q.kill(); q.wait()   # the plug, pulled: no shutdown, nothing flushed
    got = subprocess.run(["mtype", "-i", f"{card}@@{PART * 512}", "::JT/KEEP.TXT"], capture_output=True, env=MT, text=True)
    check("after the cut the card holds KEEP.TXT with its bytes", got.returncode == 0 and got.stdout.strip() == "kept through the cut", (got.returncode, got.stdout, got.stderr))
    q, uart, say = boot_once("two")
    o = say("cat jt/keep.txt", "kept through") + say("cat jt/cache.txt", "one two three")
    check("a second boot reads both files from the card", "kept through the cut" in o and "one two three" in o, o[-400:])
    check("no kernel crash", "KERNEL CRASH" not in uart() and q.poll() is None, uart()[-400:])
finally:
    q.kill(); q.wait()
    subprocess.run(["make", "-C", str(arch), "clean"], capture_output=True, timeout=60)
if fails: print("FAIL: %d of the block cache checks" % len(fails)); sys.exit(1)
print("PASS: repeated reads come from the block cache, not the card, and every write is on the card when it returns")
