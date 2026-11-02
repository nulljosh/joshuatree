#!/usr/bin/env python3
"""9.2.0, languages: Settings has a Language page of twelve, the pick is drawn, spoken by the screen reader, and kept.

Boots the ARM64 kernel on QEMU's virt machine with a virtio card (tools/checks/sdcard.py), keyboard and 800x600 ramfb. Then:
1. at boot every word of all twelve rows has a glyph and the plain rows read as as many characters as Python counts: Latin, Greek and Vietnamese from
   Tree Sans and Inter, Japanese, Chinese and Korean from Noto, and Hindi shaped ahead of time;
2. Spotlight opens Settings; Up wraps to the ninth section, Language, whose right column (Tiếng Việt, 日本語, 中文, 한국어,
   हिन्दी, Ελληνικά) is drawn with ink in every row;
3. Right picks Español: the UART says so, JT/LANG.TXT on the card reads lang=es, and the sidebar is drawn in other words;
4. after a reboot on the same card the desktop is still in Spanish ("lang loaded es"), and the screen reader reads the dock
   in Spanish ("Apps, app, 1 de 12. Pulsa Retorno para abrir.");
5. six more Rights give 日本語, and the reader's line is Japanese.
Discriminating: take the UTF-8 decode out of text.c (bytes read as Latin-1) or drop the world faces, and step 1 says fewer
than 12; take lang_load out of boot.c and step 4 reads the dock in English.
Skips (exit 0) without clang, ld.lld, qemu-system-aarch64 or mtools.
Usage: tools/checks/arm64-lang-check.py   (from the repo root)
"""
import json, os, re, shutil, socket, subprocess, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from scratch import scratch_dir
from sdcard import PART, build_card

root = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
arch = os.path.join(root, "arch", "arm64")
if not all(shutil.which(t) for t in ("clang", "ld.lld", "qemu-system-aarch64", "mmd", "mtype")):
    print("SKIP: clang, ld.lld, qemu-system-aarch64 or mtools not installed"); sys.exit(0)
fails = []
def check(name, ok, detail=""):
    print(("  ok: " if ok else "  FAIL: ") + name + ("" if ok else "  " + repr(detail)[-500:]))
    if not ok: fails.append(name)

subprocess.run(["make", "-C", arch, "clean"], capture_output=True)
b = subprocess.run(["make", "-C", arch, "kernel8.elf"], capture_output=True, text=True, timeout=900)
if b.returncode: print("FAIL: make -C arch/arm64 kernel8.elf failed\n" + (b.stdout + b.stderr)[-1500:]); sys.exit(1)
tmp = scratch_dir("arm64-lang")
card = os.path.join(tmp, "card.img")
build_card(card, False)
MT = dict(os.environ, MTOOLS_SKIP_CHECK="1")
def mt(*a): return subprocess.run([a[0], "-i", f"{card}@@{PART * 512}", *a[1:]], capture_output=True, env=MT)
mt("mmd", "::JT")

W, H = 800, 600
SX, SY = (W - 510) // 2, 36              # Settings' window (settings_ui.h: SET_WIN_W 510, at y 36)
CX, CY = SX + 8 + 150 + 16, SY + 74      # its card
class Session:
    def __init__(self, tag):
        self.log, self.sock = os.path.join(tmp, tag + ".uart"), os.path.join(tmp, tag + ".qmp")
        self.q = subprocess.Popen(["qemu-system-aarch64", "-machine", "virt", "-cpu", "cortex-a72", "-m", "256", "-global", "virtio-mmio.force-legacy=false",
                                   "-nic", "none", "-device", "ramfb", "-device", "virtio-keyboard-device",
                                   "-drive", "if=none,id=card,file=%s,format=raw" % card, "-device", "virtio-blk-device,drive=card",
                                   "-display", "none", "-serial", "file:" + self.log, "-qmp", "unix:%s,server,nowait" % self.sock,
                                   "-kernel", os.path.join(arch, "kernel8.elf")], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.f = None
    def uart(self): return open(self.log, encoding="utf-8", errors="replace").read() if os.path.exists(self.log) else ""
    def wait_for(self, text, secs=8.0, after=0):
        end = time.time() + secs
        while time.time() < end:
            if text in self.uart()[after:]: return True
            time.sleep(0.05)
        return False
    def connect(self):
        s_ = socket.socket(socket.AF_UNIX); s_.settimeout(30)
        for _ in range(100):
            try: s_.connect(self.sock); break
            except OSError: time.sleep(0.1)
        self.f = s_.makefile("rw"); self.f.readline(); self.cmd("qmp_capabilities")
    def cmd(self, c, **a):
        self.f.write(json.dumps({"execute": c, "arguments": a}) + "\n"); self.f.flush()
        while True:
            r = json.loads(self.f.readline())
            if "return" in r or "error" in r: return r
    def key(self, *qcodes): self.cmd("send-key", keys=[{"type": "qcode", "data": k} for k in qcodes]); time.sleep(0.15)
    def press(self, qcode, want, secs=8):
        n = len(self.uart()); self.key(qcode); return self.wait_for(want, secs, n)
    def shot(self):
        path = os.path.join(tmp, "shot%d.ppm" % time.monotonic_ns())
        self.cmd("screendump", filename=path)
        for _ in range(40):
            if os.path.exists(path) and os.path.getsize(path) >= W * H * 3: break
            time.sleep(0.1)
        d = open(path, "rb").read().split(b"\n", 3)[3]
        if os.environ.get("JT_LANG_SHOTS"): shutil.copy(path, os.environ["JT_LANG_SHOTS"])   # a folder, for looking at
        return lambda x, y: tuple(d[(y * W + x) * 3:(y * W + x) * 3 + 3])
    def settings(self):
        self.key("f1"); self.key("s", "e", "t", "t"); time.sleep(0.3)
        return self.press("ret", "settings open\n", 6)
    def close(self):
        if self.q.poll() is None: self.q.kill()
        self.q.wait()
def ink(px, x, y, w, h): return sum(1 for j in range(y, y + h) for i in range(x, x + w) if max(px(i, j)) < 0x80)
def sidebar(px): return [px(x, y) for y in range(SY + 40, SY + 40 + 9 * 30, 2) for x in range(SX + 46, SX + 150, 2)]

S = Session("boot1")
try:
    if not S.wait_for("M2 input ready", 60): print("FAIL: did not boot: %r" % S.uart()[-600:]); sys.exit(1)
    src = open(os.path.join(arch, "lang.c"), encoding="utf-8").read()
    table = src[src.index("L[LANG_COUNT][S_COUNT] = {"):]; table = table[table.index("{") + 1: table.index("\n};")]
    chars = sum(len(m.replace('\\"', '"')) for m in re.findall(r'"((?:[^"\\]|\\.)*)"', table))   # the eleven plain rows, in characters
    want = "lang rows drawable 12 of 12, %05d characters\n" % chars
    check("at boot every word of all twelve rows has a glyph, read as %d characters" % chars, want in S.uart(), [l for l in S.uart().splitlines() if "lang rows" in l])
    time.sleep(1.0); S.connect()
    check("Spotlight opens Settings", S.settings(), S.uart()[-300:])
    check("Up wraps to the ninth section, Language", S.press("up", "settings section 8 Language\n"))
    time.sleep(0.5); px = S.shot(); en = sidebar(px)
    rows = [ink(px, CX + 156 + 14, CY + r * 30 + 5, 120, 20) for r in range(6)]
    check("the right column draws all six names, Tiếng Việt to Ελληνικά (ink %s)" % rows, all(n > 25 for n in rows), rows)
    check("Right picks Español", S.press("right", "settings lang 1 es\n") and S.wait_for("lang es\n", 3))
    time.sleep(0.5); es = sidebar(S.shot())
    check("the sidebar is drawn in other words", sum(a != b for a, b in zip(en, es)) > 200, sum(a != b for a, b in zip(en, es)))
finally:
    S.close()
got = mt("mtype", "::JT/LANG.TXT").stdout
check("JT/LANG.TXT on the card reads lang=es", got == b"lang=es\n", got)

S = Session("boot2")
try:
    if not S.wait_for("M2 input ready", 60): print("FAIL: did not boot again: %r" % S.uart()[-600:]); sys.exit(1)
    check("the second boot loads Spanish", S.wait_for("lang loaded es\n", 10), S.uart()[-300:])
    time.sleep(1.0); S.connect()
    S.key("meta_l", "f5"); n = len(S.uart()); S.key("right")
    check("the screen reader reads the dock in Spanish", S.wait_for("reader: Apps, app, 1 de 12. Pulsa Retorno para abrir.\n", 5, n), S.uart()[n:][-300:])
    S.key("esc"); S.settings(); S.press("up", "settings section 8 Language\n")
    for _ in range(6): S.key("right"); time.sleep(0.2)
    check("six more Rights give 日本語", S.wait_for("settings lang 7 ja\n", 5), S.uart()[-300:])
    S.key("esc"); time.sleep(0.5); n = len(S.uart()); S.key("right")
    check("and the reader's line is Japanese", S.wait_for("reader: アプリ, アプリ, 1 / 12. Returnで開きます。\n", 5, n), S.uart()[n:][-300:])
    check("no kernel crash", "KERNEL CRASH" not in S.uart() and S.q.poll() is None, S.uart()[-300:])
finally:
    S.close()
    subprocess.run(["make", "-C", arch, "clean"], capture_output=True)
if fails: print("FAIL: %d of the language checks" % len(fails)); sys.exit(1)
print("PASS: twelve languages with a glyph for every word, a Language page in Settings, the pick kept on the card across a reboot, and the screen reader speaking it")
