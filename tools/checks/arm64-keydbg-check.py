#!/usr/bin/env python3
"""Mac-style USB keyboards on the ARM desktop (arch/arm64/xhci.c hid_kbd, main.c ui_key and KEY_DEBUG). Every key goes
through QEMU's usb-kbd behind an xHCI controller, the Pi's path (the other key checks use virtio).

keydbg-kernel8.elf (make keydbgtest: the dev card's debug line, plus made-up reports at boot):
  1. a 9-byte report with report ID 2 in front, Ctrl held and T down, opens the Terminal; Esc in the same form hands the
     keys back. Reverting the report-ID skip reads the ID as Shift and drops the real Ctrl byte: T then opens Spotlight.
  2. plain 8-byte boot reports: a bare Space and Ctrl+Space do nothing, Cmd+Space opens it (the skip leaves boot
     reports alone, and only Cmd+Space or F1 open Spotlight, like a Mac: Joshua hit Space on the board, 2026-10-11).
  3. Cmd+Space over USB (meta_l, the GUI modifier 0x08) opens Spotlight, and the debug line shows the raw report
     ("keydbg: key 08 00 2c ...") and the name ("Cmd+Space").
  4. made-up media reports (consumer page 0x70, 0x6F, a Mac-mode F1 and F2) open Spotlight and the Terminal.
     QEMU's usb-kbd has no media interface, so the walk that opens one on a real board is not covered here.
  5. a plain letter on the bare desktop opens Spotlight with it typed ("spotlight: j"); Esc closes it; Esc again does nothing.
kernel8.elf (release): Cmd+Space works too; a held j repeats (8 to 14 times in 1.3 s, none after the release, a tap gives one) and Caps Lock
makes Caps, j, t type "JT" with Shift+j giving "j", all over USB, where the keyboard sends one report per change; and pi-main.o has the line only with JT_KEY_DEBUG=1 (not JT_WIFI_DEV=1 since 2.65.0), and there is no debug line: no "keydbg:" on the UART or in the image.
Skips (exit 0) when the tools are missing. QEMU runs headless and every step has a timeout.
"""
import json, os, re, shutil, socket, subprocess, sys, tempfile, time

root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."); arch = os.path.join(root, "arch", "arm64")
if not all(shutil.which(t) for t in ("clang", "ld.lld", "qemu-system-aarch64")):
    print("SKIP: clang, ld.lld or qemu-system-aarch64 not installed"); sys.exit(0)
if subprocess.run(["make", "-C", arch, "kernel8.elf", "keydbgtest"], capture_output=True, text=True, timeout=900).returncode:
    print("FAIL: make -C arch/arm64 kernel8.elf keydbgtest failed"); sys.exit(1)
fails = []
def step(name, ok, detail=""):
    print(("  ok: " if ok else "  FAIL: ") + name + ("" if ok or not detail else " -- " + detail[-400:]))
    if not ok: fails.append(name)

def boot(elf, body):
    tmp = tempfile.mkdtemp(); log, sock = tmp + "/uart", tmp + "/qmp"
    q = subprocess.Popen(["qemu-system-aarch64", "-machine", "virt", "-cpu", "cortex-a72", "-m", "256", "-nic", "none",
                          "-device", "ramfb", "-device", "pcie-root-port,id=rp,chassis=1", "-device", "qemu-xhci,id=x,bus=rp",
                          "-device", "usb-kbd,bus=x.0,port=1", "-display", "none", "-serial", "file:" + log,
                          "-qmp", "unix:%s,server,nowait" % sock, "-kernel", os.path.join(arch, elf)],
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    def uart(): return open(log, errors="replace").read() if os.path.exists(log) else ""
    def wait_for(text, tries=100, count=1):
        for _ in range(tries):
            if uart().count(text) >= count: return True
            time.sleep(0.1)
        return False
    try:
        if not (wait_for("M1c fb ok", 600) and wait_for("usb ready: 1 kbd", 300)):
            step(elf + " boots with a USB keyboard", False, uart()); return
        time.sleep(1.0)
        s_ = socket.socket(socket.AF_UNIX); s_.settimeout(30); s_.connect(sock); f = s_.makefile("rw"); f.readline()
        def cmd(c, **a):
            f.write(json.dumps({"execute": c, "arguments": a}) + "\n"); f.flush()
            while True:
                r = json.loads(f.readline())
                if "return" in r or "error" in r: return r
        cmd("qmp_capabilities")
        def key(*qcodes): cmd("send-key", keys=[{"type": "qcode", "data": k} for k in qcodes]); time.sleep(0.3)
        def expect(text, name, tries=60):
            n = uart().count(text); return lambda: step(name, wait_for(text, tries, n + 1), uart())
        def hold(qcode, secs):   # send-key taps for 100 ms; a held key is a press, a wait, a release (the keyboard sends no repeats itself)
            ev = lambda down: [{"type": "key", "data": {"down": down, "key": {"type": "qcode", "data": qcode}}}]
            cmd("input-send-event", events=ev(True)); time.sleep(secs); cmd("input-send-event", events=ev(False)); time.sleep(0.3)
        body(uart, wait_for, key, expect, hold)
    finally:
        q.kill(); q.wait(); shutil.rmtree(tmp, ignore_errors=True)

def dev(uart, wait_for, key, expect, hold):
    wait_for("keytest: done", 100); out = uart()
    def between(a, b): i = out.find(a); j = out.find(b, i); return out[i:j] if i >= 0 and j >= 0 else ""
    step("a 9-byte report with a report ID: Ctrl+T opens the Terminal", "terminal open" in between("keytest: id9 ctrl+t", "keytest: id9 esc"))
    step("a 9-byte report with a report ID: Esc hands the keys back", "console open" in between("keytest: id9 esc", "keytest: boot8"))
    step("a plain 8-byte boot report: a bare Space does not open Spotlight", "spotlight" not in between("keytest: boot8 space", "keytest: boot8 ctrl+space"))
    step("a plain 8-byte boot report: Ctrl+Space does not open Spotlight either", "spotlight" not in between("keytest: boot8 ctrl+space", "keytest: boot8 cmd+space"))
    step("a plain 8-byte boot report: Cmd+Space opens Spotlight", "spotlight open" in between("keytest: boot8 cmd+space", "keytest: boot8 esc"))
    step("a plain 8-byte boot report: Esc closes it", "spotlight close" in between("keytest: boot8 esc", "keytest: media f1"))
    step("a media report, brightness down (a Mac-mode F1), opens Spotlight", "spotlight open" in between("keytest: media f1", "keytest: media esc"))
    step("a media report, brightness up (a Mac-mode F2), opens the Terminal", "terminal open" in between("keytest: media f2", "keytest: media esc2"))
    done = expect("spotlight open", "Cmd+Space over USB opens Spotlight"); key("meta_l", "spc"); done()
    step("the debug line shows the raw report and the key name",
         wait_for("keydbg: key 08 00 2c 00 00 00 00 00 Cmd+Space", 30), uart())
    done = expect("spotlight close", "Esc closes it"); key("esc"); done()
    done = expect("spotlight: j", "a plain letter on the bare desktop opens Spotlight with it typed"); key("j"); done()
    done = expect("spotlight close", "Esc closes it again"); key("esc"); done()
    before = uart(); key("esc"); time.sleep(1.0); extra = uart()[len(before):]
    step("Esc with Spotlight closed does nothing", "spotlight" not in extra and "console open" not in extra, extra)

def release(uart, wait_for, key, expect, hold):
    done = expect("spotlight open", "release build: Cmd+Space over USB opens Spotlight"); key("meta_l", "spc"); done()
    time.sleep(0.5)
    step("release build: no debug line on the UART", "keydbg:" not in uart())
    # Key repeat and Caps Lock over USB, the Pi's path: the keyboard sends one report for the press (SET_IDLE(0)) and
    # none until the release, so every repeat is main.c's key_repeat_tick. The Spotlight text is on the UART as typed.
    def text(since):
        t = re.findall(r"spotlight: (\S*) \d+", uart()[since:]); return t[-1] if t else None
    def text_is(since, want):
        for _ in range(30):
            if text(since) == want: return True
            time.sleep(0.1)
        return False
    m = len(uart()); hold("j", 1.3); held = text(m) or ""
    step("USB: a held j in Spotlight repeats 8 to 14 times (9 to 15 j in all; got %d)" % len(held), set(held) == {"j"} and 9 <= len(held) <= 15, held)
    time.sleep(0.6); m = len(uart()); time.sleep(0.6)
    step("USB: nothing repeats after the release", text(m) is None and "key 36 down" not in uart()[m:], uart()[m:])
    done = expect("spotlight close", "Esc closes it"); key("esc"); done()
    done = expect("spotlight open", "F1 opens it again, empty"); key("f1"); done()
    m = len(uart()); key("j"); time.sleep(0.8)
    step("USB: a short tap types exactly one j", text(m) == "j" and uart()[m:].count("key 36 down") == 1, uart()[m:])
    done = expect("spotlight close", "Esc closes it again"); key("esc"); done()
    done = expect("spotlight open", "F1 opens it again, empty"); key("f1"); done()
    m = len(uart()); key("caps_lock"); key("j"); key("t")
    step("USB: Caps Lock, j, t types JT", text_is(m, "JT"), repr(text(m)))
    key("shift", "j")
    step("USB: Shift+j under Caps Lock types a lowercase j", text_is(m, "JTj"), repr(text(m)))
    key("caps_lock"); key("j")
    step("USB: Caps Lock again gives lowercase", text_is(m, "JTjj"), repr(text(m)))
    done = expect("spotlight close", "Esc closes it for the last time"); key("esc"); done()

boot("keydbg-kernel8.elf", dev)
boot("kernel8.elf", release)
step("the release image carries no debug line", b"keydbg: " not in open(os.path.join(arch, "kernel8.elf"), "rb").read())
step("the dev image does", b"keydbg: " in open(os.path.join(arch, "keydbg-kernel8.elf"), "rb").read())
# The Pi's switch: pi-main.o has the line only with JT_KEY_DEBUG=1 (2.65.0 moved it off JT_WIFI_DEV=1, the dev card no longer shows raw reports); -B both ways, then a plain rebuild
# so a later `make pi` never picks up the dev object.
def pi_main(dev, flag="JT_KEY_DEBUG"):
    env = dict(os.environ); env.pop("JT_WIFI_DEV", None); env.pop("JT_KEY_DEBUG", None)
    if dev: env[flag] = "1"
    ok = subprocess.run(["make", "-B", "-C", arch, "pi-main.o"], env=env, capture_output=True, timeout=300).returncode == 0
    return ok and b"keydbg: " in open(os.path.join(arch, "pi-main.o"), "rb").read()
step("Pi key-debug build (JT_KEY_DEBUG=1) has the debug line", pi_main(True))
step("Pi dev-card build (JT_WIFI_DEV=1 alone) does not", not pi_main(True, "JT_WIFI_DEV"))
step("Pi release build does not", not pi_main(False))
if fails: print("FAIL: " + "; ".join(fails)); sys.exit(1)
print("PASS: arm64-keydbg-check: report IDs, Cmd+Space, letter-opens-Spotlight, key repeat and Caps Lock over USB, and the dev-only key debug line")
