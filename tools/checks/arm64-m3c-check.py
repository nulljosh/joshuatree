#!/usr/bin/env python3
"""ARM64 M3c part 1, the app loader: user/calendar.c, about 34 KB with relocated pointer tables, a data page and a zeroed
pool, runs at EL0 on the ARM kernel, and user/hello.c still does.

Both programs are the unchanged user/*.c, built for aarch64 by arch/arm64/Makefile (-fPIE -pie, linked flat by user_c.ld) and
baked into the kernel by userapps.S. main.c's user_load maps an image into the app window (code pages read-only and
executable, then data pages and a 16 KiB zeroed pool read-write and never executable, then an app stack), applies the
R_AARCH64_RELATIVE entries to the copy, and the boot self-test runs hello, calendar, and user.S's probe against calendar's
pages twice (the second time after a reload). Two boots on QEMU's virt machine, read from the UART:

Boot 1, the default kernel (the clock is not set, so JT_SYS_TIME answers 0):
  - hello still loads and runs: "jt-hello: start", its open comes back -38 (M3b), exit 1.
  - calendar loads ("M3c load calendar: N bytes, 8 code pages, 30 relocations, code C data D bss B"), asks for its window
    first: "M3c svc 384 ... -> -38" with the pointer it passed inside its own data page, then writes "calendar: no window" to fd 2
    and exits 1 ("M3 EL0 exit 1", "M3c userland calendar ok"). That is calendar.c's real failure path, with nothing stubbed.
  - the probe runs at EL0 against those pages, in this order: the first data word is a relocated pointer that points
    into the image; the pool past the image reads zero; the data page and pool are writable; JT_SYS_TIME stores through a
    pointer into the pool and refuses one into a code page with -14 (the kernel must not store into a read-only page);
    JT_SYS_GETPID answers; a store to the first code byte is "data abort, permission fault level 3" at that very address and the
    kernel survives. Then calendar is loaded again and the probe repeats: the pool reads zero again (the first probe dirtied it)
    and the relocated pointer is back (the first probe overwrote it), which only a loader that zeroes and relocates a fresh copy
    does. Last, a branch into the data page, into the first whole page of the zeroed pool and into the stack is each an instruction
    abort, permission fault level 3, at the address branched to: data is never executable. The same boot keeps the M3a faults
    (kernel page, kernel text) from arm64-m3-check.py.
Boot 2, the caltest kernel (the clock fixed at CAL_UTC): JT_SYS_TIME answers that exact number, in the program's return value and
  through its pointer. Then, on the Terminal, over QMP keys: `userrun calendar` and `userrun hello` load and run again with
  the program's own lines and "userrun NAME: exit 1"; `userrun nope` is refused in one line; `/help` after all of that still
  answers, so the kernel is up after five EL0 programs, ten deliberate faults and two reloads.
"EL0: reached code after the blocked access", "M3c probe FAIL", "M3c load FAIL", "M3c exec FAIL" and "M3c userland calendar FAIL" must never appear.
Skips (exit 0) when clang's aarch64 target, ld.lld or qemu-system-aarch64 is missing.
Usage: tools/checks/arm64-m3c-check.py   (from the repo root)
"""
import json, os, re, shutil, socket, subprocess, sys, tempfile, time

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
    def __init__(self, name, elf, keys):
        self.log, sock = "%s/%s.uart" % (tmp, name), "%s/%s.qmp" % (tmp, name)
        dev = ["-global", "virtio-mmio.force-legacy=false", "-nic", "none", "-device", "ramfb", "-device", "virtio-keyboard-device",
               "-qmp", "unix:%s,server,nowait" % sock] if keys else []
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
        for ch in text: self.key({" ": "spc", "/": "slash"}.get(ch, ch))
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
            re.compile(r"M3c svc 5 a0=0x[0-9a-f]{16} -> -38$"), "jt-hello: open failed -38", "M3 EL0 exit 1", "M3b userland C ok",
            "M3c EL0 calendar starts", re.compile(r"M3c load calendar: "),
            re.compile(r"M3c svc 384 a0=(0x[0-9a-f]{16}) -> -38$"), "calendar: no window", "M3 EL0 exit 1", "M3c userland calendar ok",
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
    win = re.search(r"M3c svc 384 a0=(0x[0-9a-f]{16}) -> -38", out)
    check("calendar's window argument is a full 64 bit pointer into its own data page", win and di <= int(win.group(1), 16) < bi, win.group(0) if win else out[-400:])
    for bad in ("EL0: reached code after the blocked access", "M3c probe FAIL", "M3c load FAIL", "M3c exec FAIL", "M3c userland calendar FAIL", "M3b userland C FAIL",
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
    print("arm64 M3c: boot 2, the caltest kernel (clock fixed at %d), then the Terminal" % CAL_UTC)
    b = Boot("cal", os.path.join(arch, "cal-kernel8.elf"), True)
    try:
        trace(b.uart(), CAL_UTC)
        b.key("f2")
        if not b.wait_for("terminal open", 10): raise SystemExit("FAIL: F2 did not open the Terminal: %r" % b.uart()[-300:])
        time.sleep(0.5)
        out = b.say("userrun calendar", "userrun calendar: exit")
        check("userrun calendar loads it again and runs it: its own failure line, then exit 1",
              re.search(r"M3c load calendar: .*\nM3c svc 384 .* -> -38\ncalendar: no window\nM3 EL0 exit 1\nuserrun calendar: exit 1\n", out), out[-500:])
        out = b.say("userrun hello", "userrun hello: exit")
        check("userrun hello runs hello again from the same window: open -38, exit 1",
              re.search(r"M3c load hello: .*\njt-hello: start\n.*\njt-hello: open failed -38\nM3 EL0 exit 1\nuserrun hello: exit 1\n", out, re.S), out[-500:])
        out = b.say("userrun nope", "userrun: no program")
        check("an unknown program is refused in one line", "userrun: no program called that, try hello or calendar\n" in out and "M3c load" not in out, out[-300:])
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
      "relocated pointers) and runs it at EL0 with hello.c still passing; its window call gets -38, TIME answers the clock and refuses a "
      "read-only pointer, a store to its code faults, a reload zeroes and relocates a fresh copy, and `userrun` and the kernel keep answering")
