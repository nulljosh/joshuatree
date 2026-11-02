#!/usr/bin/env python3
"""8.5.0, sockets for apps: JT_SYS_CONNECT (412) opens a TCP connection and fd 100 reads, writes and closes it.

Runs a small TCP server on this Mac that answers each line in capitals, boots the ARM64 kernel in QEMU with user networking
(the Mac is 10.0.2.2 to it) and a card holding SOCK (tools/mkapp.sh), waits for the DHCP lease and runs `userrun sock`:
1. sock connects to 10.0.2.2 on the server's port, writes "hello from jt", and reads back "HELLO FROM JT": the bytes made a
   round trip through a real TCP connection the app drove itself. The server saw the same line.
2. After close, a write to fd 100 is -EBADF.
3. A port nobody listens on is -ECONNREFUSED (-111); a name that does not resolve is -EHOSTUNREACH (-113).
Discriminating: drop the fd 100 intercept in exc_el0_sync and the write goes to the file calls (-EBADF) and nothing comes
back; drop park_retry from sock_call and the first read answers before the reply is in, so the line is empty or short.
Skips (exit 0) when clang's aarch64 target, ld.lld, qemu-system-aarch64 or mtools is missing.
Usage: tools/checks/arm64-sock-check.py   (from the repo root)
"""
import json, os, re, shutil, socket, subprocess, sys, threading, time
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from scratch import scratch_dir
from sdcard import PART, build_card

root = Path(__file__).resolve().parents[2]
arch = root / "arch/arm64"
if not all(shutil.which(t) for t in ("clang", "ld.lld", "qemu-system-aarch64", "mmd", "mcopy")):
    print("SKIP: clang, ld.lld, qemu-system-aarch64 or mtools not installed"); sys.exit(0)
tmp = Path(scratch_dir("arm64-sock"))

fails = []
def check(name, ok, detail=""):
    print(("  ok: " if ok else "  FAIL: ") + name + ("" if ok else "  " + repr(detail)[-900:]))
    if not ok: fails.append(name)

subprocess.run(["make", "-C", str(arch), "clean"], capture_output=True, timeout=60)
r = subprocess.run(["make", "-C", str(arch), "kernel8.elf"], capture_output=True, text=True, timeout=600)
if r.returncode: print(r.stdout[-2000:], r.stderr[-2000:]); print("FAIL: arch/arm64 does not build"); sys.exit(1)

seen = []
srv = socket.socket(); srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1); srv.bind(("127.0.0.1", 0)); srv.listen(4)
PORT = srv.getsockname()[1]
def serve():
    while True:
        try: c, _ = srv.accept()
        except OSError: return
        with c:
            c.settimeout(20); buf = b""
            try:
                while b"\n" not in buf:
                    d = c.recv(256)
                    if not d: break
                    buf += d
                seen.append(buf); time.sleep(0.3); c.sendall(buf.upper())   # a beat late, so the app's first read finds nothing yet
            except OSError: pass
threading.Thread(target=serve, daemon=True).start()
dead = socket.socket(); dead.bind(("127.0.0.1", 0)); DEAD = dead.getsockname()[1]; dead.close()
(tmp / "sock.c").write_text(r"""#include "jtsys.h"
static void say(const char *s) { int n = 0; while (s[n]) n++; jt_syscall(JT_SYS_WRITE, 1, (jt_arg)s, (jt_arg)n); }
static void num(int v) { char b[12]; int n = 11; b[n] = 0; int neg = v < 0; unsigned u = neg ? -v : v; do { b[--n] = '0' + u % 10; u /= 10; } while (u); if (neg) b[--n] = '-'; say(b + n); }
__attribute__((section(".text.start"), used))
void _start(void) {
    int fd = jt_connect("10.0.2.2", PORT_HERE);
    const char *msg = "hello from jt\n"; int w = jt_syscall(JT_SYS_WRITE, JT_SOCK_FD, (jt_arg)msg, 14);
    static char got[64]; int n = 0, reads = 0;
    while (n < 63) { int r = jt_syscall(JT_SYS_READ, JT_SOCK_FD, (jt_arg)(got + n), 63 - n); reads++; if (r <= 0) break; n += r; if (got[n - 1] == '\n') break; }
    got[n] = 0; if (n && got[n - 1] == '\n') got[n - 1] = 0;
    int c = jt_syscall(JT_SYS_CLOSE, JT_SOCK_FD, 0, 0);
    int after = jt_syscall(JT_SYS_WRITE, JT_SOCK_FD, (jt_arg)msg, 14);
    int refused = jt_connect("10.0.2.2", DEAD_HERE);
    int noname = jt_connect("no-such-host.invalid", 80);
    say("sock: fd "); num(fd); say(" wrote "); num(w); say(" got ["); say(got); say("] close "); num(c);
    say(" after "); num(after); say(" refused "); num(refused); say(" noname "); num(noname); say("\n");
    jt_exit(0);
}
""".replace("PORT_HERE", str(PORT)).replace("DEAD_HERE", str(DEAD)))
mk = subprocess.run([str(root / "tools/mkapp.sh"), str(tmp / "sock.c"), str(tmp / "SOCK.APP")], capture_output=True, text=True, timeout=300)
if mk.returncode: print(mk.stdout + mk.stderr); print("FAIL: tools/mkapp.sh could not build sock.c"); sys.exit(1)
card = str(tmp / "card.img")
build_card(card, False)
MT = dict(os.environ, MTOOLS_SKIP_CHECK="1")
def mt(*a): return subprocess.run([a[0], "-i", f"{card}@@{PART * 512}", *a[1:]], capture_output=True, env=MT)
if mt("mmd", "::JT").returncode or mt("mmd", "::JT/APPS").returncode: print("FAIL: mtools could not make JT/APPS"); sys.exit(1)
for f in ("SOCK.APP",):
    if mt("mcopy", "-o", str(tmp / f), "::JT/APPS/" + f).returncode: print("FAIL: mtools could not copy " + f); sys.exit(1)

log, sock = tmp / "uart", tmp / "qmp"
q = subprocess.Popen(["qemu-system-aarch64", "-machine", "virt", "-cpu", "cortex-a72", "-m", "256", "-global", "virtio-mmio.force-legacy=false",
                      "-device", "ramfb", "-device", "virtio-keyboard-device", "-netdev", "user,id=n", "-device", "virtio-net-device,netdev=n", "-drive", "if=none,id=card,file=%s,format=raw" % card, "-device", "virtio-blk-device,drive=card",
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
    if not wait_for("net dhcp 10.0.2.15", 30): raise SystemExit("FAIL: no DHCP lease: %r" % uart()[-600:])
    time.sleep(0.5)
    s = socket.socket(socket.AF_UNIX); s.settimeout(30); s.connect(str(sock)); f = s.makefile("rw"); f.readline()
    def command(name, **a):
        f.write(json.dumps({"execute": name, "arguments": a}) + "\n"); f.flush()
        while True:
            rr = json.loads(f.readline())
            if "return" in rr or "error" in rr: return rr
    command("qmp_capabilities")
    QK = {" ": ("spc",), "|": ("shift", "backslash")}
    def key(*k): command("send-key", keys=[{"type": "qcode", "data": x} for x in k]); time.sleep(0.12)
    def say(t, until, secs=120):
        n = len(uart())
        for ch in t: key(*QK.get(ch, (ch,)))
        key("ret"); wait_for(until, secs, n); time.sleep(0.5)
        return uart()[n:]
    key("f2")
    if not wait_for("terminal open", 10): raise SystemExit("FAIL: F2 did not open the Terminal: %r" % uart()[-300:])
    time.sleep(0.5)
    out = say("userrun sock", "userrun sock: exit", 90)
    m = re.search(r"sock: fd (-?\d+) wrote (-?\d+) got \[([^\]]*)\] close (-?\d+) after (-?\d+) refused (-?\d+) noname (-?\d+)", out)
    check("sock runs to its exit 0 and reports", m and "userrun sock: exit 0" in out, out[-900:])
    if m:
        fd, w, got, c, after, refused, noname = m.groups()
        check("connect answers fd 100, and the write takes all 14 bytes", (fd, w) == ("100", "14"), (fd, w))
        check("the server's answer came back: HELLO FROM JT", got == "HELLO FROM JT", got)
        check("close is 0, and a write after it is -EBADF", (c, after) == ("0", "-9"), (c, after))
        check("a port nobody listens on: -ECONNREFUSED; a name that does not resolve: -EHOSTUNREACH", (refused, noname) == ("-111", "-113"), (refused, noname))
    check("the server got the app's line", seen[:1] == [b"hello from jt\n"], seen)
    check("no kernel crash", "KERNEL CRASH" not in uart() and "stopped by a fault" not in uart() and q.poll() is None, uart()[-600:])
finally:
    q.kill(); q.wait(); srv.close()
    subprocess.run(["make", "-C", str(arch), "clean"], capture_output=True, timeout=60)
if fails: print("FAIL: %d of the socket checks" % len(fails)); sys.exit(1)
print("PASS: an app opens a TCP connection, writes a line and reads the server's answer through fd 100, and gets plain errors for a dead port or a bad name")
