#!/usr/bin/env python3
"""ARM64 M3c part 1, the app loader: user/calendar.c, about 34 KB with relocated pointer tables, a data page and a zeroed
pool, runs at EL0 on the ARM kernel, and user/hello.c still does.

Both programs are the unchanged user/*.c, built for aarch64 by arch/arm64/Makefile (-fPIE -pie, linked flat by user_c.ld) and
baked into the kernel by userapps.S. main.c's user_load maps an image into the app window (code pages read-only and
executable, then data pages and a 32 KiB zeroed pool read-write and never executable, then an app stack), applies the
R_AARCH64_RELATIVE entries to the copy, and the boot self-test runs hello, calendar, and user.S's probe against calendar's
pages twice (the second time after a reload). Two boots on QEMU's virt machine, read from the UART:

Boot 1, the default kernel (the clock is not set, so JT_SYS_TIME answers 0):
  - hello still loads and runs: "jt-hello: start"; no card is mounted yet, so its open comes back -2 (-ENOENT, M3d), exit 1.
  - M3d, the file calls abused from EL0 (user.S's entry E): open of a kernel pointer and a read into the kernel-only page
    are -14, open, mkdir and unlink of "NOTES/../BOOT.TXT" are -22, a readfile of 4 GB is -22, and a readdir whose 64
    records run past the stack page is -14. It exits 0 ("M3d file call abuse ok") and the boot carries on.
  - calendar loads ("M3c load calendar: N bytes, 8 code pages, 30 relocations, code C data D bss B"), asks for its window
    first: "M3c svc 384 ... -> -19" with the pointer it passed inside its own data page (-ENODEV: the boot self-test runs it
    outside the scheduler, so there is no window to give, as i386 answers a program run from the text shell), then writes
    "calendar: no window" to fd 2 and exits 1 ("M3 EL0 exit 1", "M3c userland calendar ok"). That is calendar.c's real
    failure path, with nothing stubbed.
  - the probe runs at EL0 against those pages, in this order: the first data word is a relocated pointer that points
    into the image; the pool past the image reads zero; the data page and pool are writable; JT_SYS_TIME stores through a
    pointer into the pool and refuses one into a code page with -14 (the kernel must not store into a read-only page);
    JT_SYS_GETPID answers; a store to the first code byte is "data abort, permission fault level 3" at that very address and the
    kernel survives. Then calendar is loaded again and the probe repeats: the pool reads zero again (the first probe dirtied it)
    and the relocated pointer is back (the first probe overwrote it), which only a loader that zeroes and relocates a fresh copy
    does. Last, a branch into the data page, into the first whole page of the zeroed pool and into the stack is each an instruction
    abort, permission fault level 3, at the address branched to: data is never executable. The same boot keeps the M3a faults
    (kernel page, kernel text) from arm64-m3-check.py.
Boot 2, the caltest kernel (the clock fixed at CAL_UTC), with arm64-sd-check.py's FAT32 card (tools/checks/sdcard.py) as a
  virtio disk: JT_SYS_TIME answers that exact number, in the program's return value and through its pointer, and emmc.c
  mounts the card's partition through the virtio disk ("sd fat32 partition at block 2048"). Then, on the Terminal, over QMP
  keys: `userrun calendar` loads it again under the scheduler (M3c part 2), so its window opens ("userrun calendar: running
  in its window") and Esc closes it ("calendar: closed", exit 0; the window itself is arm64-calendar-check.py's);
  `write hello.txt hello from the card` puts JT/HELLO.TXT on the card, and `userrun hello` runs the unchanged user/hello.c
  all the way to exit 9 (M3d): open gives fd 3, read brings the 20 bytes back, a second read is end of file, close works
  once and not twice, and its own abuse (open and read of 0xC0000000, write of NULL, time into 0xC0000000, read of fd 6)
  is -14, -14, -14, -14, -9; `userrun nope` is refused in one line; `/help` after all of that still answers, so the
  kernel is up after six EL0 programs, ten deliberate faults and two reloads.
"EL0: reached code after the blocked access", "M3c probe FAIL", "M3c load FAIL", "M3c exec FAIL", "M3c userland calendar FAIL" and
"M3d file call abuse FAIL" must never appear.
Skips (exit 0) when clang's aarch64 target, ld.lld or qemu-system-aarch64 is missing.
Usage: tools/checks/arm64-m3c-check.py   (from the repo root)
"""
import json, os, re, shutil, socket, subprocess, sys, tempfile, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from sdcard import build_card   # arm64-sd-check.py's FAT32 card, here as a virtio disk

root = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
arch = os.path.join(root, "arch/arm64")
if not all(shutil.which(t) for t in ("clang", "ld.lld", "qemu-system-aarch64")):
    print("SKIP: clang, ld.lld or qemu-system-aarch64 not installed"); sys.exit(0)

CAL_UTC = 1775000000
tmp = tempfile.mkdtemp(prefix="jt-arm-m3c-")
fails = []
def check(name, ok, detail=""):
    print(("  ok: " if ok else "  FAIL: ") + name + (("  (" + detail[-500:] + ")") if detail and not ok else ""))
    if not ok: fails.append(name)

def make(*args):
    r = subprocess.run(["make", "-C", arch, *args], capture_output=True, text=True, timeout=600)
    if r.returncode: print("FAIL: arch/arm64 `make %s` does not build:\n%s%s" % (" ".join(args), r.stdout[-1500:], r.stderr[-1500:])); sys.exit(1)

class Boot:
    def __init__(self, name, elf, keys, disk=None):
        self.log, sock = "%s/%s.uart" % (tmp, name), "%s/%s.qmp" % (tmp, name)
        dev = ["-global", "virtio-mmio.force-legacy=false", "-nic", "none", "-device", "ramfb", "-device", "virtio-keyboard-device",
               "-qmp", "unix:%s,server,nowait" % sock] if keys else []
        if disk: dev += ["-drive", "if=none,id=card,file=%s,format=raw" % disk, "-device", "virtio-blk-device,drive=card"]
        self.q = subprocess.Popen(["qemu-system-aarch64", "-machine", "virt", "-cpu", "cortex-a72", "-m", "256", *dev,
                                   "-display", "none", "-serial", "file:" + self.log, "-kernel", elf],
                                  stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.f = None
        self.wait_for("M3c exec stack", 40)   # the last step of the self-test
        self.wait_for("M3c exec ", 20, 6) and time.sleep(0.5)   # three starts, three answers
        if keys:
            if not self.wait_for("M2 input ready", 30): raise SystemExit("FAIL: %s did not reach the desktop: %r" % (name, self.uart()[-500:]))
            time.sleep(0.5)
            s = socket.socket(socket.AF_UNIX); s.settimeout(20); s.connect(sock); self.f = s.makefile("rw")
            self.f.readline(); self.cmd("qmp_capabilities")
    def uart(self): return open(self.log, errors="replace").read() if os.path.exists(self.log) else ""
    def wait_for(self, text, secs, count=1):
        end = time.time() + secs
        while time.time() < end:
            if self.uart().count(text) >= count: return True
            time.sleep(0.1)
        return False
    def cmd(self, c, **a):
        self.f.write(json.dumps({"execute": c, "arguments": a}) + "\n"); self.f.flush()
        while True:
            r = json.loads(self.f.readline())
            if "return" in r or "error" in r: return r
    def key(self, *qcodes):
        self.cmd("send-key", keys=[{"type": "qcode", "data": k} for k in qcodes]); time.sleep(0.12)
    def say(self, text, expect, secs=20):
        """Type one line at the Terminal and return what the UART gained once `expect` shows up after it."""
        before = len(self.uart())
        for ch in text: self.key({" ": "spc", "/": "slash", ".": "dot"}.get(ch, ch))
        self.key("ret")
        end = time.time() + secs
        while time.time() < end and expect not in self.uart()[before:]: time.sleep(0.1)
        time.sleep(0.5)
        return self.uart()[before:]
    def close(self): self.q.kill(); self.q.wait()

def trace(out, want_time):
    """The ordered serial lines of the self-test. `want_time` is the number JT_SYS_TIME must answer."""
    lines = out.splitlines()
    load = [re.match(r"M3c load calendar: (\d+) bytes, (\d+) code pages, (\d+) relocations, code (0x[0-9a-f]{16}) data (0x[0-9a-f]{16}) bss (0x[0-9a-f]{16})$", l)
            for l in lines]
    load = [m for m in load if m]
    check("calendar loaded with 8 code pages and 30 relocations, twice", len(load) == 2 and all(m.group(2) == "8" and m.group(3) == "30" for m in load), repr(out[-700:]))
    if len(load) < 2: return
    size, ncode, nrel, code, data, bss = load[0].groups()
    ci, di, bi = int(code, 16), int(data, 16), int(bss, 16)
    check("the image is bigger than one page", int(size) > 4096 * 8, size)
    check("data pages start right after the 8 code pages, page aligned", di == ci + 8 * 4096 and ci % 4096 == 0, "%s %s" % (code, data))
    check("the pool starts inside the last data page, past the image", di <= bi < di + 4096 and bi % 16 == 0 and bi - ci == (int(size) + 15) // 16 * 16, "%s %s" % (data, bss))
    fault = "M3 EL0 fault: data abort, permission fault level 3, address " + code
    probe = [r"EL0: relocated pointer in the data page points into the image", "EL0: pool reads zero", "EL0: data page and pool are writable",
             re.compile(r"M3c svc 13 a0=%s -> %d$" % (bss, want_time)), "EL0: time stored through a pool pointer",
             "M3c svc 13 a0=%s -> -14" % code, "EL0: time into a code page refused, returned -14",
             re.compile(r"M3c svc 20 a0=0x0{16} -> [1-9]\d*$"), "EL0: getpid answered", fault, "M3c probe survived the code write"]
    want = ["M3b EL0 C task starts", re.compile(r"M3c load hello: \d+ bytes, 1 code pages, 0 relocations, "), "jt-hello: start",
            re.compile(r"M3c svc 5 a0=0x[0-9a-f]{16} -> -2$"), "jt-hello: open failed -2", "M3 EL0 exit 1", "M3b userland C ok",
            "M3d file call abuse starts", "M3c svc 5 a0=0x0000000044004000 -> -14", "EL0: open of a kernel pointer refused, -14",
            "EL0: read into kernel memory refused, -14", re.compile(r"M3c svc 5 a0=0x[0-9a-f]{16} -> -22$"), "EL0: open of a path with .. refused, -22",
            re.compile(r"M3c svc 389 a0=0x[0-9a-f]{16} -> -22$"), re.compile(r"M3c svc 390 a0=0x[0-9a-f]{16} -> -22$"),
            "EL0: mkdir and unlink of a path with .. refused, -22", re.compile(r"M3c svc 401 a0=0x[0-9a-f]{16} -> -22$"), "EL0: readfile of 4 GB refused, -22",
            re.compile(r"M3c svc 388 a0=0x[0-9a-f]{16} -> -14$"), "EL0: readdir past its buffer refused, -14", "M3 EL0 exit 0", "M3d file call abuse ok",
            "M3c EL0 calendar starts", re.compile(r"M3c load calendar: "),
            re.compile(r"M3c svc 384 a0=(0x[0-9a-f]{16}) -> -19$"), "calendar: no window", "M3 EL0 exit 1", "M3c userland calendar ok",
            "M3c probe 1 starts", *probe, re.compile(r"M3c load calendar: "), "M3c probe 2 starts", *probe]
    for what, addr in (("data", di), ("pool", ci + (int(size) + 4095) // 4096 * 4096), ("stack", ci + 0x70000)):   # 0x70000 is UP_ASTACK - UP_APP in main.c
        want += ["M3c exec %s 0x%016x" % (what, addr), "M3 EL0 fault: instruction abort, permission fault level 3, address 0x%016x" % addr, "M3c exec refused"]
    at = -1
    for w in want:
        hit = lambda l: l == w if isinstance(w, str) else w.match(l)
        nxt = next((i for i in range(at + 1, len(lines)) if hit(lines[i])), -1)
        if nxt < 0: check("serial line %r in order" % (w if isinstance(w, str) else w.pattern), False, out[-900:]); return
        at = nxt
    check("every line of the self-test came in order: hello, calendar's real failure path, the probe, the reload, the probe again", True)
    win = re.search(r"M3c svc 384 a0=(0x[0-9a-f]{16}) -> -19", out)
    check("calendar's window argument is a full 64 bit pointer into its own data page", win and di <= int(win.group(1), 16) < bi, win.group(0) if win else out[-400:])
    for bad in ("EL0: reached code after the blocked access", "M3c probe FAIL", "M3c load FAIL", "M3c exec FAIL", "M3c userland calendar FAIL", "M3b userland C FAIL", "M3d file call abuse FAIL",
                "M3 userland FAIL", "jt-hello: nosys=", "KERNEL CRASH"):
        check("%r never printed" % bad, bad not in out, bad)
    check("the M3a deliberate faults still fault (kernel page, kernel text)",
          "M3 EL0 fault: data abort, permission fault level 3, address 0x0000000044004000" in out
          and "M3 EL0 fault: instruction abort, permission fault level 2, address 0x0000000040000000" in out, out[:900])

try:
    make("clean")
    make("CAL_UTC=%d" % CAL_UTC, "caltest")   # cal-kernel8.elf; its other objects are the default kernel's
    make()                                    # kernel8.elf: only main.o differs
    print("arm64 M3c: boot 1, the default kernel (clock unset)")
    b = Boot("default", os.path.join(arch, "kernel8.elf"), False)
    try: out1 = b.uart()
    finally: b.close()
    trace(out1, 0)
    print("arm64 M3c: boot 2, the caltest kernel (clock fixed at %d) with the check card as a virtio disk, then the Terminal" % CAL_UTC)
    card = os.path.join(tmp, "card.img")
    build_card(card, False)
    b = Boot("cal", os.path.join(arch, "cal-kernel8.elf"), True, card)
    try:
        trace(b.uart(), CAL_UTC)
        check("the virtio disk is the card: emmc.c mounts its FAT32 partition on virt",
              "sd virtio disk 4096 MB\nsd fat32 partition at block 2048, 64 MB, writes go in JT/\n" in b.uart(), b.uart()[-600:])
        b.key("f2")
        if not b.wait_for("terminal open", 10): raise SystemExit("FAIL: F2 did not open the Terminal: %r" % b.uart()[-300:])
        time.sleep(0.5)
        out = b.say("userrun calendar", "userrun calendar: running in its window")
        check("userrun calendar loads it again under the scheduler: now its window opens and it keeps running",
              re.search(r"M3c load calendar: .*\nM3c app calendar starts\nM3c window calendar \d+x\d+ at \d+,\d+\nM3c svc 384 .* -> 0\n"
                        r"calendar: ring-3 window \d+x\d+\n", out) and "userrun calendar: running in its window\n" in out, out[-500:])
        n = len(b.uart()); b.key("esc")
        check("Esc closes it: calendar says so and exits 0", b.wait_for("M3c app calendar exit 0", 10) and "calendar: closed\n" in b.uart()[n:], b.uart()[n:][-300:])
        time.sleep(0.5)
        out = b.say("write hello.txt hello from the card", "wrote JT/")
        check("the Terminal's write puts JT/HELLO.TXT on the virtio card", "wrote JT/hello.txt, 20 bytes\n" in out, out[-300:])
        n = len(b.uart())
        b.say("userrun hello", "userrun hello: ")
        b.wait_for("M3c app hello exit", 10)
        out = b.uart()[n:]
        want = ["jt-hello: start", "jt-hello: fd=3", "hello from the card", "jt-hello: read=20", "jt-hello: eof=0", "jt-hello: time ok",
                "jt-hello: yielded", "jt-hello: nosys=-38", "jt-hello: openfault=-14", "jt-hello: readfault=-14", "jt-hello: writefault=-14",
                "jt-hello: timefault=-14", "jt-hello: badfd=-9", "jt-hello: done", "M3 EL0 exit 9"]
        at, ok = 0, True
        for w in want:
            at = out.find(w + "\n", at)
            if at < 0: ok = False; break
        check("userrun hello reads JT/HELLO.TXT through open, read and close (fd 3, 20 bytes, then end of file, a double close refused), "
              "its own abuse is -14 and -9, and it exits 9", ok and "M3c app hello exit 9, ran " in out and "M3 EL0 fault" not in out, out[-900:])
        out = b.say("userrun nope", "userrun: no program")
        check("an unknown program is refused in one line", "userrun: no program called that, try hello, calendar, notes, music or loop\n" in out and "M3c load" not in out, out[-300:])
        out = b.say("/help", "/clear  new conversation")
        check("the kernel still answers a command after all of that", "/model [auto|haiku|sonnet|opus]" in out and "/clear  new conversation" in out, out[-300:])
        check("no kernel crash and no oom after the Terminal runs", "KERNEL CRASH" not in b.uart() and "oom" not in b.uart().split("terminal open")[-1])
    finally:
        b.close()
finally:
    make("clean")
    shutil.rmtree(tmp, ignore_errors=True)
if fails:
    print("FAIL: %d of the M3c checks failed" % len(fails)); sys.exit(1)
print("PASS: the ARM kernel loads a 34 KB user/calendar.c image (code pages read-only and executable, data and a zeroed pool read-write, a stack, "
      "relocated pointers) and runs it at EL0 with hello.c still passing; its window call gets -19 outside the scheduler, TIME answers the clock and refuses a "
      "read-only pointer, a store to its code faults, a reload zeroes and relocates a fresh copy; the file calls refuse kernel pointers, '..' and huge "
      "lengths with errnos, and with a card on the virtio disk hello.c reads its file to exit 9; `userrun` and the kernel keep answering")
