#!/usr/bin/env python3
"""Keyboard-only control of the ARM desktop (arch/arm64/main.c ui_key): no mouse or tablet is plugged in, only a
virtio keyboard, and keys go in through QMP send-key, the path arm64-claude-console-check.py uses.

  1. Ctrl+Space opens Spotlight ("spotlight open"); the bar is on the screen: its off-white surface and one accent mark.
  2. typing "term" narrows it to one name ("spotlight: term 1"); Enter opens the Terminal ("terminal open") and the
     Terminal has the keyboard: "hi" and Enter reach ask.c, which answers with a claude: line.
  3. Esc hands the keys back to the desktop ("console open"); Right three times moves the dock's label to Calendar
     ("M1d hover 2 Mail" on the way) and Enter opens that tile ("dock Calendar: not on ARM yet").
  4. F2 opens the Terminal at once; Esc back; F1 opens Spotlight and Esc closes it ("spotlight close").
Every step proves the Terminal is reachable with no mouse. Skips (exit 0) when the tools are missing.
"""
import json, os, shutil, socket, subprocess, sys, tempfile, time

root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."); arch = os.path.join(root, "arch", "arm64")
if not all(shutil.which(t) for t in ("clang", "ld.lld", "qemu-system-aarch64")):
    print("SKIP: clang, ld.lld or qemu-system-aarch64 not installed"); sys.exit(0)
if subprocess.run(["make", "-C", arch, "kernel8.elf"], capture_output=True, text=True, timeout=900).returncode:
    print("FAIL: make -C arch/arm64 kernel8.elf failed"); sys.exit(1)

tmp = tempfile.mkdtemp(); log, sock = tmp + "/uart", tmp + "/qmp"; fails = []
q = subprocess.Popen(["qemu-system-aarch64", "-machine", "virt", "-cpu", "cortex-a72", "-m", "256", "-nic", "none",
                      "-global", "virtio-mmio.force-legacy=false", "-device", "ramfb", "-device", "virtio-keyboard-device",
                      "-display", "none", "-serial", "file:" + log, "-qmp", "unix:%s,server,nowait" % sock,
                      "-kernel", os.path.join(arch, "kernel8.elf")], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
def uart(): return open(log, errors="replace").read() if os.path.exists(log) else ""
def wait_for(text, tries=100, count=1):
    for _ in range(tries):
        if uart().count(text) >= count: return True
        time.sleep(0.1)
    return False
def step(name, ok, detail=""):
    print(("  ok: " if ok else "  FAIL: ") + name + ("" if ok or not detail else " -- " + detail))
    if not ok: fails.append(name)
try:
    if not wait_for("M1c fb ok", 600): raise SystemExit("FAIL: the desktop did not come up: %r" % uart()[-600:])
    time.sleep(1.0)
    s_ = socket.socket(socket.AF_UNIX); s_.connect(sock); f = s_.makefile("rw"); f.readline()
    def cmd(c, **a):
        f.write(json.dumps({"execute": c, "arguments": a}) + "\n"); f.flush()
        while True:
            r = json.loads(f.readline())
            if "return" in r or "error" in r: return r
    cmd("qmp_capabilities")
    def key(*qcodes): cmd("send-key", keys=[{"type": "qcode", "data": k} for k in qcodes]); time.sleep(0.15)
    def shot():
        p = tmp + "/shot%d.ppm" % time.monotonic_ns(); cmd("screendump", filename=p); time.sleep(0.4)
        parts = open(p, "rb").read().split(b"\n", 3); w, h = map(int, parts[1].split()); return w, h, parts[3]
    def count(img, rgb):
        w, h, b = img; return sum(1 for i in range(0, len(b), 3) if b[i:i + 3] == bytes(rgb))
    def expect(text, name, tries=60):
        n = uart().count(text); return lambda: step(name, wait_for(text, tries, n + 1), uart()[-300:])
    step("no pointer device: the boot log has no mouse line", "mouse " not in uart())
    base = shot()
    done = expect("spotlight open", "Ctrl+Space opens Spotlight"); key("ctrl", "spc"); done()
    img = shot()
    step("the Spotlight bar is drawn: off-white surface and the accent mark appear",
         count(img, (0xfa, 0xf8, 0xf4)) > count(base, (0xfa, 0xf8, 0xf4)) + 2000 and count(img, (0xb5, 0x50, 0x2c)) > count(base, (0xb5, 0x50, 0x2c)))
    done = expect("spotlight: term 1", "typing 'term' leaves one match"); key("t", "e", "r", "m"); done()
    done = expect("terminal open", "Enter opens the Terminal"); key("ret"); done()
    done = expect("claude:", "the Terminal has the keyboard: 'hi' and Enter get a claude: line", 150); key("h", "i", "ret"); done()
    done = expect("console open", "Esc hands the keys back to the desktop"); key("esc"); done()
    done = expect("M1d hover 2 Mail", "Right moves the dock's label along"); key("right", "right", "right"); done()
    done = expect("M1d hover 3 Calendar", "to Calendar"); key("right"); done()
    done = expect("dock Calendar: not on ARM yet", "Enter opens the selected dock tile"); key("ret"); done()
    done = expect("terminal open", "F2 opens the Terminal at once"); key("f2"); done()
    done = expect("console open", "Esc again"); key("esc"); done()
    done = expect("spotlight open", "F1 opens Spotlight too"); key("f1"); done()
    done = expect("spotlight close", "Esc closes it"); key("esc"); done()
    after = shot()
    step("closing Spotlight puts the desktop back (no accent mark left where the bar was)",
         count(after, (0xb5, 0x50, 0x2c)) <= count(base, (0xb5, 0x50, 0x2c)))
finally:
    q.kill(); q.wait(); shutil.rmtree(tmp, ignore_errors=True)
if fails: print("FAIL: " + "; ".join(fails)); sys.exit(1)
print("PASS: arm64-keys-check: Spotlight, dock keys, F2 and Esc reach the Terminal with no mouse")
