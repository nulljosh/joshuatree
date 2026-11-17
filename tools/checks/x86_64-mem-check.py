#!/usr/bin/env python3
"""x86-64, slice 3 (13.24.0): the memory map, the frame allocator and kmalloc, and the PS/2 mouse on IRQ12, in qemu-system-x86_64.

`make -C arch/x86_64` links kernel64.bin. The check boots it with -m 256 and reads the serial log: "JT64 memory N MB" with N
between 200 and 256 (QEMU's map gives 255), then "JT64 heap ok" after the frame and kmalloc self-test, and "JT64 mouse
ready" once the mouse streams on IRQ12. Then QMP input-send-event moves the pointer 20 right and 10 down (rel events). The
log must give "JT64 mouse 532 y" with y moved off 384, and a screendump must show the white square at that spot and the
old spot back to blue. Ten more right moves must clamp x to 1023, the last column of the 1024-wide screen.

Discriminating: the memory number is whatever the map says, so it must fall in the band for 256 MiB; the mouse line has to
follow the injected moves, and the clamp to 1023 can only come from the clamp in the code. Skips (exit 0) without clang,
ld.lld or qemu-system-x86_64.
"""
import json, os, re, shutil, socket, subprocess, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from scratch import scratch_dir
root = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
arch = os.path.join(root, "arch/x86_64")
if not all(shutil.which(t) for t in ("clang", "ld.lld", "qemu-system-x86_64")):
    print("SKIP: clang, ld.lld or qemu-system-x86_64 not installed"); sys.exit(0)
tmp = scratch_dir("x86_64-mem")
fails = []
def check(name, ok, detail=""):
    print(("  ok: " if ok else "  FAIL: ") + name + ("" if ok else "  " + repr(detail)[-500:]), flush=True)
    if not ok: fails.append(name)
r = subprocess.run(["make", "-C", arch, "-B", "kernel64.bin"], capture_output=True, text=True, timeout=300)
if r.returncode: print(r.stdout[-1500:], r.stderr[-1500:]); print("FAIL: arch/x86_64 does not build"); sys.exit(1)
img = os.path.join(arch, "kernel64.bin")

log, sock_path = os.path.join(tmp, "serial"), os.path.join(tmp, "qmp")
q = subprocess.Popen(["qemu-system-x86_64", "-kernel", img, "-vga", "std", "-display", "none", "-no-reboot", "-m", "256",
                      "-serial", "file:" + log, "-qmp", "unix:%s,server,nowait" % sock_path],
                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
def serial():
    return open(log, errors="replace").read() if os.path.exists(log) else ""
def wait_until(pred, secs):   # poll the serial log until pred(text) holds or the time is up
    end = time.time() + secs
    while time.time() < end:
        out = serial()
        if pred(out) or q.poll() is not None: return out
        time.sleep(0.1)
    return serial()
def last_pos(out):   # the newest "JT64 mouse x y" line, as a pair of ints
    found = re.findall(r"^JT64 mouse (\d+) (\d+)$", out, re.M)
    return (int(found[-1][0]), int(found[-1][1])) if found else None
def idx(lines, prefix):
    return next((i for i, l in enumerate(lines) if l.startswith(prefix)), -1)
try:
    out = wait_until(lambda o: "JT64 mouse ready" in o or "JT64 mouse none" in o, 30)
    lines = [l for l in out.splitlines() if l.startswith("JT64")]
    mem = [int(m.group(1)) for m in (re.fullmatch(r"JT64 memory (\d+) MB", l) for l in lines) if m]
    check("the memory line gives 200 to 256 MB of usable RAM under -m 256", len(mem) == 1 and 200 <= mem[0] <= 256, [l for l in lines if "memory" in l])
    check("the frame and kmalloc self-test says heap ok", "JT64 heap ok" in lines and "JT64 heap FAIL" not in out, lines[:14])
    order = [idx(lines, p) for p in ("JT64 M0 done", "JT64 memory", "JT64 heap ok", "JT64 idt ok")]
    check("M0 done, memory, heap ok and idt ok come in that order", all(i >= 0 for i in order) and order == sorted(order), lines[:14])
    check("the mouse is ready on IRQ12", "JT64 mouse ready" in lines, lines[-4:])
    check("no exception was reported", "JT64 exception" not in out, out[-300:])

    s = socket.socket(socket.AF_UNIX); s.settimeout(20); s.connect(sock_path); f = s.makefile("rw"); f.readline()
    def cmd(c, **a):
        f.write(json.dumps({"execute": c, **({"arguments": a} if a else {})}) + "\n"); f.flush()
        while True:
            rr = json.loads(f.readline())
            if "return" in rr or "error" in rr: return rr
    cmd("qmp_capabilities")
    def rel(dx, dy):
        evs = []
        if dx: evs.append({"type": "rel", "data": {"axis": "x", "value": dx}})
        if dy: evs.append({"type": "rel", "data": {"axis": "y", "value": dy}})
        return cmd("input-send-event", events=evs)

    rr = rel(20, 10)
    check("the QMP move is accepted", "return" in rr, rr)
    out = wait_until(lambda o: last_pos(o) not in (None, (512, 384)), 10)
    pos = last_pos(out)
    check("a mouse line shows the pointer moved 20 right from the middle (x 532, y changed)", pos is not None and pos[0] == 532 and pos[1] != 384, pos)

    time.sleep(0.5)
    ppm = os.path.join(tmp, "shot.ppm"); cmd("screendump", filename=ppm); time.sleep(0.5)
    parts = open(ppm, "rb").read().split(b"\n", 3)
    w, h = (int(x) for x in parts[1].split()); px = parts[3]
    at = lambda x, y: tuple(px[(y * w + x) * 3:(y * w + x) * 3 + 3])
    if pos:
        check("the white square is drawn where the line says", at(pos[0] + 3, pos[1] + 3) == (255, 255, 255), at(pos[0] + 3, pos[1] + 3))
        check("the old spot in the middle is put back to blue", at(512 + 3, 384 + 3) == (0x3B, 0x6E, 0xA5), at(515, 387))

    for _ in range(10):
        rel(100, 0)
    out = wait_until(lambda o: last_pos(o) is not None and last_pos(o)[0] == 1023, 15)
    pos = last_pos(out)
    check("moves past the right edge clamp x to 1023", pos is not None and pos[0] == 1023, pos)
finally:
    q.kill(); q.wait()
subprocess.run(["make", "-s", "-C", arch, "clean"], capture_output=True)
if fails: print("FAIL: " + ", ".join(fails)); sys.exit(1)
print("PASS: x86-64 slice 3: the memory map reports 200 to 256 MB under -m 256, the frame allocator and kmalloc pass their self-test, and the PS/2 mouse on IRQ12 moves a white cursor that clamps at the screen edge")
