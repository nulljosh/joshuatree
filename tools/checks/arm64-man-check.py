#!/usr/bin/env python3
"""13.6.0, man NAME in the Pi Terminal (arch/arm64/man.h, cmd.c man_show). Static first: every command `help` lists and every word
cmd_run matches has a page. Then QEMU virt, headless: F2 opens the Terminal, `man cat` prints cat's page, `man zzz` says there is no
page. Discriminating: delete a page, or the man line in cmd_run, and it fails. Skips (exit 0) with no aarch64 QEMU.
"""
import json, re, shutil, socket, subprocess, sys, tempfile, time
from pathlib import Path

root = Path(__file__).resolve().parents[2]
arch = root / "arch/arm64"
cmd_c, man_h = (arch / "cmd.c").read_text(), (arch / "man.h").read_text()
pages = set(re.findall(r'^    \{"([a-z]+)", "', man_h, re.M))
text = "".join(re.findall(r'"((?:[^"\\]|\\.)*)"', cmd_c[cmd_c.index("static const char help[]"):cmd_c.index("static unsigned ls_count")].split(";")[0]))
listed = {m.group(1) for seg in re.sub(r"\([^)]*\)", "", text).split(",") for m in [re.match(r"\s*([a-z]+)(?=[\s.]|$)", seg)] if m}
matched = {w.split()[0] for w in re.findall(r'\b(?:word_is|is)\(q, n, "([^"]+)"\)', cmd_c) if not w.startswith("-")}
missing = sorted((listed | matched) - pages)
if missing or not {"cat", "ls", "sudo", "man"} <= pages:
    print("FAIL: no man page for", missing); sys.exit(1)
print(f"ok: {len(pages)} pages cover the {len(listed | matched)} commands help lists and cmd_run matches")

if not shutil.which("qemu-system-aarch64"):
    print("SKIP: qemu-system-aarch64 not installed"); sys.exit(0)
r = subprocess.run(["make", "-C", str(arch), "kernel8.elf"], capture_output=True, text=True, timeout=600)
if r.returncode: print(r.stderr[-2000:]); print("FAIL: arch/arm64 does not build"); sys.exit(1)
tmp = Path(tempfile.mkdtemp())
log, sock = tmp / "uart", tmp / "qmp"
q = subprocess.Popen(["qemu-system-aarch64", "-machine", "virt", "-cpu", "cortex-a72", "-m", "256", "-global", "virtio-mmio.force-legacy=false",
                      "-device", "ramfb", "-device", "virtio-keyboard-device", "-display", "none", "-serial", f"file:{log}",
                      "-qmp", f"unix:{sock},server,nowait", "-kernel", str(arch / "kernel8.elf")], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
def uart(): return log.read_text(errors="replace") if log.exists() else ""
def wait_for(text, secs, after=0):
    end = time.time() + secs
    while time.time() < end:
        if text in uart()[after:]: return True
        time.sleep(0.1)
    return False
fails = []
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
    key("f2")
    if not wait_for("terminal open", 15): fails.append("F2 did not open the Terminal")
    time.sleep(0.5)
    if not wait_for("Prints a file.", 10, line("man cat")): fails.append(f"man cat printed no page: {uart()[-300:]!r}")
    if not wait_for("man: no page for zzz", 10, line("man zzz")): fails.append(f"man zzz did not refuse: {uart()[-300:]!r}")
finally:
    q.kill(); q.wait()
if fails:
    for x in fails: print("FAIL:", x)
    sys.exit(1)
print("PASS: every command has a man page, man cat prints it and an unknown name is refused")
