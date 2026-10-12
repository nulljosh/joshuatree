#!/usr/bin/env python3
"""Keyboard-only control of the ARM desktop (arch/arm64/main.c ui_key): no mouse or tablet is plugged in, only a
virtio keyboard, and keys go in through QMP send-key, the path arm64-claude-console-check.py uses.

  1. a bare Space and Ctrl+Space do nothing; Cmd+Space opens Spotlight ("spotlight open"); the bar is on the screen:
     its off-white surface and one accent mark.
  2. typing "term" narrows it to one name ("spotlight: term 1"); Enter opens the Terminal ("terminal open") and the
     Terminal has the keyboard: "sam hi" and Enter reach ask.c, which answers with a samantha: line.
  3. Esc hands the keys back to the desktop ("console open"); Right three times moves the dock's label to Calendar
     ("M1d hover 2 Mail" on the way) and Enter opens that tile: Calendar, user/calendar.c in its own window ("M3c window
     calendar first frame"), which takes the keys, so Esc closes it ("M3c app calendar exit 0").
  4. F2 opens the Terminal at once; Esc back; F1 opens Spotlight and Esc closes it ("spotlight close").
  5. Every Spotlight shortcut toggles it closed and restores the covered pixels exactly. F2 and Ctrl+T also dismiss
     an open Spotlight before giving the Terminal the keyboard.
  6. Key repeat (main.c key_repeat_tick). A key held with QMP input-send-event for 1.3 s types its character once, then
     again after 400 ms and every 80 ms: 12 repeats, so 9 to 15 characters in all (8 to 14 repeats), and none after the
     release. A 100 ms tap types exactly one. It holds in Spotlight, the Terminal's line ("% jjjj" echoed on Enter) and
     the Calculator ("calc: 9999 = ..."); Backspace repeats too. The virtio keyboard has no repeat of its own.
  7. Caps Lock (ask.c): Caps, j, t types "JT"; Shift+j under Caps types "j"; Caps again gives lowercase; Shift alone still
     capitalises, in Spotlight and on the Terminal's line.
Every step proves the Terminal is reachable with no mouse. Skips (exit 0) when the tools are missing.
"""
import json, os, re, shutil, socket, subprocess, sys, tempfile, time

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
    def mark_edges(img):   # accent pixels with the bar's off-white 3 px to their left: the Spotlight caret mark. The wallpaper is the accent
        w, h, b = img                       # itself since 3.0.0, so counting accent pixels would see the bar cover them and fall.
        acc, off = bytes((0xb5, 0x50, 0x2c)), bytes((0xfa, 0xf8, 0xf4)); n = 0
        for y in range(h):
            row = y * w * 3
            for x in range(3, w):
                if b[row + x * 3:row + x * 3 + 3] == acc and b[row + (x - 3) * 3:row + (x - 3) * 3 + 3] == off: n += 1
        return n
    def expect(text, name, tries=60):
        n = uart().count(text); return lambda: step(name, wait_for(text, tries, n + 1), uart()[-300:])
    step("no pointer device: the boot log has no mouse line", "mouse " not in uart())
    base = shot()
    before = uart(); key("spc"); key("ctrl", "spc"); time.sleep(1.0)
    step("a bare Space and Ctrl+Space do not open Spotlight", "spotlight" not in uart()[len(before):])
    done = expect("spotlight open", "Cmd+Space opens Spotlight"); key("meta_l", "spc"); done()
    img = shot()
    step("the Spotlight bar is drawn: off-white surface and the accent mark appear",
         count(img, (0xfa, 0xf8, 0xf4)) > count(base, (0xfa, 0xf8, 0xf4)) + 2000 and mark_edges(img) > mark_edges(base) + 10)
    done = expect("spotlight: term 1", "typing 'term' leaves one match"); key("t", "e", "r", "m"); done()
    done = expect("terminal open", "Enter opens the Terminal"); key("ret"); done()
    done = expect("samantha:", "the Terminal has the keyboard: 'sam hi' and Enter get a samantha: line", 150); key("s", "a", "m", "spc", "h", "i", "ret"); done()
    done = expect("console open", "Esc hands the keys back to the desktop"); key("esc"); done()
    done = expect("M1d hover 2 Mail", "Right moves the dock's label along"); key("right", "right", "right"); done()
    done = expect("M1d hover 3 Calendar", "to Calendar"); key("right"); done()
    done = expect("M3c window calendar first frame", "Enter opens the selected dock tile: Calendar, at EL0 in its own window"); key("ret"); done()
    done = expect("M3c app calendar exit 0", "Esc goes to Calendar, which closes itself"); key("esc"); done()
    done = expect("terminal open", "F2 opens the Terminal at once"); key("f2"); done()
    done = expect("console open", "Esc again"); key("esc"); done()
    done = expect("spotlight open", "F1 opens Spotlight too"); key("f1"); done()
    done = expect("spotlight close", "Esc closes it"); key("esc"); done()
    after = shot()
    for shortcut in [("f1",), ("meta_l", "spc")]:
        before = shot()
        done = expect("spotlight open", "shortcut opens Spotlight: " + "+".join(shortcut)); key(*shortcut); done()
        done = expect("spotlight close", "same shortcut closes Spotlight: " + "+".join(shortcut)); key(*shortcut); done()
        closed = shot()
        w, h, pixels = before; scale = 2 if h >= 1080 else 1
        start, end = 40 * scale * w * 3, min(h, 220 * scale) * w * 3
        step("toggle restores the pixels under Spotlight: " + "+".join(shortcut), pixels[start:end] == closed[2][start:end])
        key("esc")
    for shortcut in [("f2",), ("ctrl", "t")]:
        key("f1")
        done = expect("spotlight close", "Terminal shortcut dismisses Spotlight: " + "+".join(shortcut))
        opened = expect("terminal open", "Terminal shortcut opens Terminal: " + "+".join(shortcut))
        key(*shortcut); done(); opened()
        key("esc")
    step("closing Spotlight puts the desktop back (no accent mark left where the bar was)",
         count(after, (0xb5, 0x50, 0x2c)) <= count(base, (0xb5, 0x50, 0x2c)))

    # ---- 6 and 7: key repeat and Caps Lock ----
    def hold(qcode, secs):   # send-key taps for only 100 ms; this presses, waits, then releases
        ev = lambda down: [{"type": "key", "data": {"down": down, "key": {"type": "qcode", "data": qcode}}}]
        cmd("input-send-event", events=ev(True)); time.sleep(secs); cmd("input-send-event", events=ev(False)); time.sleep(0.3)
    def fresh(text, *qcodes, tries=30):   # press the keys, then wait for one more `text` line on the UART
        n = uart().count(text); key(*qcodes); return wait_for(text, tries, n + 1)
    def spot_text(since):    # Spotlight's text after UART offset `since`: the last "spotlight: TEXT N" line, or None
        t = re.findall(r"spotlight: (\S*) \d+", uart()[since:]); return t[-1] if t else None
    def downs(code, since): return len(re.findall(r"key %d down" % code, uart()[since:]))
    def text_is(since, want):
        for _ in range(30):
            if spot_text(since) == want: return True
            time.sleep(0.1)
        return False
    HOLD = 1.3   # 400 ms, then every 80 ms: 12 repeats, so 13 characters; the check allows 8 to 14 repeats, 9 to 15 characters
    def settled(code, name):   # nothing more arrives once the key is up
        time.sleep(0.6); mark = len(uart())
        time.sleep(0.6); step(name, downs(code, mark) == 0 and spot_text(mark) is None, uart()[mark:])
    fresh("spotlight open", "f1")
    m = len(uart()); hold("j", HOLD); txt = spot_text(m) or ""
    step("a held j in Spotlight types j and repeats it 8 to 14 times (9 to 15 j in all; got %d)" % len(txt), set(txt) == {"j"} and 9 <= len(txt) <= 15 and downs(36, m) == len(txt), "%r, %d downs" % (txt, downs(36, m)))
    settled(36, "no j repeats after the release")
    hold("j", HOLD); n0 = len(spot_text(m) or "")   # 23 at most: Spotlight's text stops there
    m = len(uart()); hold("backspace", HOLD); left = spot_text(m) or ""; d = downs(14, m)
    step("a held Backspace deletes one character, then repeats 8 to 14 times (9 to 15 in all; got %d)" % d, 9 <= d <= 15 and len(left) == n0 - d, "%d downs, %d chars before, %d after" % (d, n0, len(left)))
    key("esc"); fresh("spotlight open", "f1")
    m = len(uart()); key("j"); time.sleep(0.8)
    step("a short tap types exactly one j", spot_text(m) == "j" and downs(36, m) == 1, "%r, %d downs" % (spot_text(m), downs(36, m)))
    key("esc"); fresh("spotlight open", "f1")
    m = len(uart()); key("caps_lock"); key("j"); key("t")
    step("Caps Lock, j, t types JT", text_is(m, "JT"), repr(spot_text(m)))
    key("shift", "j")
    step("Shift+j under Caps Lock types a lowercase j", text_is(m, "JTj"), repr(spot_text(m)))
    key("caps_lock"); key("j"); key("t")
    step("Caps Lock again gives lowercase", text_is(m, "JTjjt"), repr(spot_text(m)))
    key("shift", "j")
    step("Shift alone still capitalises", text_is(m, "JTjjtJ"), repr(spot_text(m)))
    key("esc")
    # the Terminal's line is read back from the UART when Enter runs it ("joshua@tree % jjjj"); a word that is no command comes back as zsh: command not found
    def sent(since):
        s = re.findall(r"% (\w+)", uart()[since:]); return s[0] if s else None
    def answered(since, name):   # the line ran: zsh said the word is no command, then the prompt takes the next line
        step(name, wait_for("zsh: command not found:", 150, uart()[:since].count("zsh: command not found:") + 1), uart()[since:]); time.sleep(0.5)
    fresh("terminal open", "f2")
    m = len(uart()); key("caps_lock"); key("j"); key("t"); key("shift", "j"); key("ret"); time.sleep(0.5)
    step("the Terminal's line follows Caps Lock and Shift: JTj", sent(m) == "JTj", repr(sent(m)))
    answered(m, "that line is run (command not found)"); key("caps_lock")
    m = len(uart()); hold("j", HOLD); key("ret"); time.sleep(0.5); line = sent(m) or ""
    step("a held j on the Terminal's line repeats there too (9 to 15 j; got %d)" % len(line), set(line) == {"j"} and 9 <= len(line) <= 15, repr(line))
    answered(m, "that line is run too (command not found)"); key("esc")
    fresh("spotlight open", "f1")
    for c in "apps": key(c)
    done = expect("calc open", "Enter opens the Calculator"); key("ret"); done()
    m = len(uart()); hold("9", HOLD); key("ret"); time.sleep(0.5)
    sums = re.findall(r"calc: (9+) =", uart()[m:]); s = sums[0] if sums else ""
    step("a held 9 in the Calculator repeats there too (9 to 15 nines; got %d)" % len(s), 9 <= len(s) <= 15, repr(s))
    key("esc")
finally:
    q.kill(); q.wait(); shutil.rmtree(tmp, ignore_errors=True)
if fails: print("FAIL: " + "; ".join(fails)); sys.exit(1)
print("PASS: arm64-keys-check: Spotlight, dock keys, F2 and Esc reach the Terminal with no mouse; held keys repeat; Caps Lock")
