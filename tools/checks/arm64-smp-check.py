#!/usr/bin/env python3
"""8.2.0, all four cores: core 0 wakes cores 1 to 3 (main.c smp_start), each turns its MMU on with core 0's tables, says it
is up, and counts the wake-ups core 0 sends every desktop pass.

1. QEMU virt with -smp 4 (PSCI CPU_ON): the boot says core 1, 2 and 3 are up, each with its own MPIDR; `cores` in the
   Terminal says 4 of 4 running, and run twice a second apart, every one of cores 1 to 3 has woken more times.
2. QEMU virt with one CPU: the boot says cores 1 to 3 are not there and carries on to the desktop (no crash).
3. The Pi image on QEMU's raspi4b (the spin table at 0xe0, 0xe8 and 0xf0, as the real firmware leaves it): all three come up.
Each core's count is on its own cache line, and the timer's event stream is off, so they sleep (8.9.0).
Discriminating: take smp_start out of desk.c and no core comes up (1, 3); leave mmu_on_core out of core_main and a core
never says it is up (its writes sit below the caches); drop the sev in app_tick and the counts stop moving (1).
A timing check (four threads in a quarter of the time) would flake on shared CI runners, so this proves each core ran instead.
Skips (exit 0) when clang, ld.lld or qemu-system-aarch64 is missing; part 3 skips without the raspi4b model.
Usage: tools/checks/arm64-smp-check.py   (from the repo root)
"""
import json, os, re, shutil, socket, subprocess, sys, time
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from scratch import scratch_dir

root = Path(__file__).resolve().parents[2]
arch = root / "arch/arm64"
if not all(shutil.which(t) for t in ("clang", "ld.lld", "qemu-system-aarch64")):
    print("SKIP: clang, ld.lld or qemu-system-aarch64 not installed"); sys.exit(0)
tmp = Path(scratch_dir("arm64-smp"))
fails = []
def check(name, ok, detail=""):
    print(("  ok: " if ok else "  FAIL: ") + name + ("" if ok else "  " + repr(detail)[-700:]))
    if not ok: fails.append(name)

subprocess.run(["make", "-C", str(arch), "clean"], capture_output=True, timeout=60)
r = subprocess.run(["make", "-C", str(arch), "kernel8.elf"], capture_output=True, text=True, timeout=600)
if r.returncode: print(r.stdout[-2000:], r.stderr[-2000:]); print("FAIL: arch/arm64 does not build"); sys.exit(1)
UP = re.compile(r"^core (\d) up, mpidr (0x[0-9a-f]+)$", re.M)

def boot(extra, tag):
    log, sock = tmp / ("uart-" + tag), tmp / ("qmp-" + tag)
    for p in (log, sock):
        if p.exists(): p.unlink()
    q = subprocess.Popen(["qemu-system-aarch64", "-machine", "virt", "-cpu", "cortex-a72", "-m", "256", *extra, "-global", "virtio-mmio.force-legacy=false",
                          "-device", "ramfb", "-device", "virtio-keyboard-device", "-display", "none", "-serial", "file:" + str(log),
                          "-qmp", "unix:%s,server,nowait" % sock, "-kernel", str(arch / "kernel8.elf")], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return q, log, sock

def uart_of(log): return log.read_text(errors="replace") if log.exists() else ""
def wait_for(q, log, text, secs, after=0):
    end = time.time() + secs
    while time.time() < end:
        if text in uart_of(log)[after:]: return True
        if q.poll() is not None: return False
        time.sleep(0.1)
    return False

# 1. four cores on virt
q, log, sock = boot(["-smp", "4"], "four")
try:
    ok = wait_for(q, log, "M2 input ready", 60)
    out = uart_of(log)
    ups = dict((int(c), m) for c, m in UP.findall(out))
    check("virt -smp 4: cores 1, 2 and 3 say they are up", ok and sorted(ups) == [1, 2, 3], out[-600:])
    check("each with its own MPIDR", len(set(ups.values())) == 3 and all(int(m, 16) & 3 == c for c, m in ups.items()), ups)
    s = socket.socket(socket.AF_UNIX); s.settimeout(30); s.connect(str(sock)); f = s.makefile("rw"); f.readline()
    def command(name, **a):
        f.write(json.dumps({"execute": name, "arguments": a}) + "\n"); f.flush()
        while True:
            rr = json.loads(f.readline())
            if "return" in rr or "error" in rr: return rr
    command("qmp_capabilities")
    def key(*k): command("send-key", keys=[{"type": "qcode", "data": x} for x in k]); time.sleep(0.12)
    key("f2")
    check("F2 opens the Terminal", wait_for(q, log, "terminal open", 10), uart_of(log)[-300:])
    time.sleep(0.5)
    WOKE = re.compile(r"^core (\d): up, woken (\d+) times$", re.M)
    def cores():
        n = len(uart_of(log))
        for ch in "cores": key(ch)
        key("ret"); wait_for(q, log, "core 3:", 10, n); time.sleep(0.3)
        o = uart_of(log)[n:]
        return o, dict((int(c), int(w)) for c, w in WOKE.findall(o))
    o1, w1 = cores(); time.sleep(1.0); o2, w2 = cores()
    check("`cores` says 4 of 4 running and core 0 runs the kernel", "cores: 4 of 4 running" in o1 and "core 0: up, runs the kernel" in o1, o1[-400:])
    check("cores 1 to 3 kept waking: every count went up (%s then %s)" % (w1, w2), sorted(w1) == [1, 2, 3] and all(w2.get(c, 0) > w1[c] for c in w1), (o1[-300:], o2[-300:]))
    # 8.9.0: they sleep between wake-ups. Core 0 sends a sev at most every 10 ms, so a second's count is about a hundred, not the
    # million a second a core spinning on the timer's event stream showed on the board.
    check("cores 1 to 3 sleep between wake-ups: at most about 100 a second, under 400 in about a second (%s)" % {c: w2.get(c, 0) - w1.get(c, 0) for c in w1},
          w1 and all(0 < w2.get(c, 0) - w1[c] < 400 for c in w1), (w1, w2))
    check("no kernel crash", "KERNEL CRASH" not in uart_of(log) and q.poll() is None, uart_of(log)[-400:])
finally:
    q.kill(); q.wait()

# 2. one core on virt
q, log, sock = boot([], "one")
try:
    ok = wait_for(q, log, "M2 input ready", 60)
    out = uart_of(log)
    check("virt with one CPU: cores 1 to 3 are not there, and the desktop still comes up", ok and all("core %d not there" % c in out for c in (1, 2, 3))
          and not UP.search(out) and "KERNEL CRASH" not in out, out[-600:])
finally:
    q.kill(); q.wait()
subprocess.run(["make", "-C", str(arch), "clean"], capture_output=True, timeout=60)

# 3. the Pi image on raspi4b: the spin table
if "raspi4b" in subprocess.run(["qemu-system-aarch64", "-machine", "help"], capture_output=True, text=True).stdout:
    if subprocess.run(["make", "-C", str(arch), "pi"], capture_output=True, timeout=600).returncode:
        check("arch/arm64 `make pi` builds", False)
    else:
        log = tmp / "uart-pi"
        if log.exists(): log.unlink()
        q = subprocess.Popen(["qemu-system-aarch64", "-machine", "raspi4b", "-serial", "null", "-display", "none", "-serial", "file:" + str(log),
                              "-kernel", str(arch / "kernel8.img")], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        try:
            wait_for(q, log, "core 3 ", 60)
            time.sleep(0.5)
            out = uart_of(log)
            ups = dict((int(c), m) for c, m in UP.findall(out))
            check("the Pi image on raspi4b: cores 1, 2 and 3 come up through the spin table", sorted(ups) == [1, 2, 3], out[-600:])
        finally:
            q.kill(); q.wait()
    subprocess.run(["make", "-C", str(arch), "clean"], capture_output=True, timeout=60)
else:
    print("  skip: this QEMU has no raspi4b model, part 3 not run")
if fails: print("FAIL: %d of the four-core checks" % len(fails)); sys.exit(1)
print("PASS: cores 1 to 3 come up on virt (PSCI) and the Pi image (spin table), each with its own MPIDR, and keep counting; one CPU boots too")
