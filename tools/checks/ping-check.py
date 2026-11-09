#!/usr/bin/env python3
"""13.8.0, ping on the Pi (drivers/net.c net_ping). QEMU's user network answers an echo to its own gateway, 10.0.2.2, so the
reply is real ICMP from outside the kernel.
`ping 10.0.2.2` in the Terminal says three replies. And `ping 10.0.2.77`, where nothing answers,
says no reply, so a kernel that always claims success fails. Skips (exit 0) with no aarch64 QEMU.
"""
import json, os, re, shutil, socket, subprocess, sys, tempfile, time
from freeport import free_port

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
os.chdir(ROOT)
if not shutil.which("qemu-system-aarch64"):
    print("SKIP: qemu-system-aarch64 is missing"); sys.exit(0)
subprocess.run(["make", "-s", "-C", "arch/arm64", "kernel8.elf"], check=True, capture_output=True)
tmp = tempfile.mkdtemp()
fails = []
def check(name, ok, detail=""):
    print(("  ok: " if ok else "  FAIL: ") + name + ("" if ok else "  " + repr(detail)[-400:]))
    if not ok: fails.append(name)
KEY = {" ": "spc", ".": "dot"}

def run(tag, argv, ready, open_keys, line_wait):
    log, port = os.path.join(tmp, tag), free_port()
    q = subprocess.Popen(argv + ["-display", "none", "-serial", f"file:{log}", "-qmp", f"tcp:127.0.0.1:{port},server,nowait"],
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    def uart(): return open(log, errors="replace").read() if os.path.exists(log) else ""
    def wait_for(t, secs, after=0):
        end = time.time() + secs
        while time.time() < end:
            if t in uart()[after:]: return True
            time.sleep(0.1)
        return False
    try:
        if not wait_for(ready, 90): check(f"{tag}: comes up", False, uart()[-300:]); return
        s = None
        for _ in range(50):
            try: s = socket.create_connection(("127.0.0.1", port)); break
            except OSError: time.sleep(0.2)
        f = s.makefile("rw"); f.readline()
        def cmd(c, **a):
            f.write(json.dumps({"execute": c, "arguments": a}) + "\n"); f.flush()
            while True:
                r = json.loads(f.readline())
                if "return" in r or "error" in r: return r
        cmd("qmp_capabilities")
        def keys(*k): cmd("human-monitor-command", **{"command-line": "sendkey " + "-".join(k)}); time.sleep(0.2)   # the text shell misses QMP's send-key
        def line(t):
            n = len(uart())
            for c in t: keys(KEY.get(c, c))
            keys("ret")
            return n
        time.sleep(6)
        for k in open_keys: keys(*k)
        time.sleep(2)
        line_wait(line, wait_for, uart)
    finally:
        q.kill(); q.wait()

def pi(line, wait_for, uart):
    n = line("ping 10.0.2.2")
    check("Pi: the gateway answers three times", wait_for("ping: 3 sent, 3 received", 30, n), uart()[-400:])
    n = line("ping 10.0.2.77")
    check("Pi: an address nobody holds gets no reply", wait_for("received", 40, n) and "0 received" in uart()[n:], uart()[n:][-300:])
run("pi", ["qemu-system-aarch64", "-machine", "virt", "-cpu", "cortex-a72", "-m", "256", "-global", "virtio-mmio.force-legacy=false", "-device", "ramfb",
           "-device", "virtio-keyboard-device", "-netdev", "user,id=n", "-device", "virtio-net-device,netdev=n", "-kernel", "arch/arm64/kernel8.elf"],
    "M2 input ready", [("f2",)], pi)
shutil.rmtree(tmp, ignore_errors=True)
if fails: print("FAIL:", len(fails), "failed"); sys.exit(1)
print("PASS: ping gets real ICMP replies on the Pi and reports silence as silence")
