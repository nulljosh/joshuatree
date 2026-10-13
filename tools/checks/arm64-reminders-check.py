#!/usr/bin/env python3
"""ARM64 3.17.0: Reminders runs on the Pi, and an item added and ticked there is on the card after a reboot.

QEMU's Raspberry Pi 4B model with arm64-sd-check.py's clean FAT32 card (tools/checks/sdcard.py) in EMMC2, and the notes
check's way in: made-up boot keyboard reports through xhci.c's hid_kbd, one key every 150 ms (`make -C arch/arm64
reminderstest` builds remw and remr, the NOTES_SELFTEST=3 and 4 variants).
  1. remw-kernel8.img: Right six times and Enter walk the dock to its sixth tile, Reminders (it used to log "not on ARM
     yet"); user/remindpi.c starts at EL0 in its own window. "buy milk" Enter and "call mom" Enter add two items, Up Up
     selects the first and Space ticks it, which sinks it under the other. The card holds JT/REMINDER.TXT, i386's line
     format, exactly "0 call mom\n1 buy milk\n"; the screen has the terracotta (#b5502c) done circle.
  2. remr-kernel8.img on the same card, as after a reboot: Reminders says "loaded 2" and "checked 1", and the window is
     pixel for pixel the one boot 1 left.
  3. remr-kernel8.img with no card: "no card, memory only", and the window says so on screen (terracotta ink under the title).
No EL0 fault, no KERNEL CRASH and no `sd FAIL` once Reminders starts. Skips (exit 0) without clang, ld.lld, qemu-system-aarch64,
its raspi4b model, or mtools. REMINDERS_SHOT=DIR keeps the window screendumps (PPM) there.
Usage: tools/checks/arm64-reminders-check.py   (from the repo root)
"""
import json, os, re, shutil, socket, subprocess, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from scratch import scratch_dir
from sdcard import PART, build_card, fat_pass, fsck

root = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
arch = os.path.join(root, "arch/arm64")
if not all(shutil.which(t) for t in ("clang", "ld.lld", "qemu-system-aarch64")):
    print("SKIP: clang, ld.lld or qemu-system-aarch64 not installed"); sys.exit(0)
if "raspi4b" not in subprocess.run(["qemu-system-aarch64", "-machine", "help"], capture_output=True, text=True).stdout:
    print("SKIP: QEMU here has no raspi4b model (needs QEMU 9 or newer)"); sys.exit(0)
if not shutil.which("mtype"):
    print("SKIP: mtools (mtype, mdir) is needed to read the card from the host"); sys.exit(0)
TMP = scratch_dir("reminders")
fails = []


def check(name, ok, detail=""):
    print(("  ok: " if ok else "  FAIL: ") + name + (("  (" + str(detail)[-500:] + ")") if detail and not ok else ""))
    if not ok: fails.append(name)


class Boot:
    """One raspi4b boot on the card, with QMP for screendumps. Runs until `notestest: done` or a minute."""
    def __init__(self, img, card):  # card None: no drive at all
        Boot.n = getattr(Boot, "n", 0) + 1
        self.log, sock = os.path.join(TMP, "uart%d" % Boot.n), os.path.join(TMP, "qmp%d" % Boot.n)   # short: a unix socket path has a 104 byte limit on macOS
        self.q = subprocess.Popen(["qemu-system-aarch64", "-machine", "raspi4b", "-display", "none", "-serial", "null", "-serial", "file:" + self.log,   # serial 0 is the PL011, the radio's; the console is the mini UART
                                   "-qmp", "unix:%s,server,nowait" % sock, "-kernel", img] +
                                  (["-drive", "if=none,id=card,file=%s,format=raw" % card, "-device", "sd-card,drive=card,bus=/generic-sdhci/sd-bus"] if card else []),
                                  stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        for _ in range(100):
            if os.path.exists(sock): break
            time.sleep(0.05)
        s = socket.socket(socket.AF_UNIX); s.settimeout(20); s.connect(sock); self.f = s.makefile("rw")
        self.f.readline(); self.cmd("qmp_capabilities")
    def uart(self): return open(self.log, errors="replace").read() if os.path.exists(self.log) else ""
    def wait_for(self, text, secs):
        end = time.time() + secs
        while time.time() < end:
            if text in self.uart(): return True
            time.sleep(0.1)
        return False
    def cmd(self, c, **a):
        self.f.write(json.dumps({"execute": c, "arguments": a}) + "\n"); self.f.flush()
        while True:
            r = json.loads(self.f.readline())
            if "return" in r or "error" in r: return r
    def shot(self):
        path = os.path.join(TMP, "shot-%d.ppm" % time.monotonic_ns())
        self.cmd("screendump", filename=path)
        for _ in range(50):
            if os.path.exists(path) and os.path.getsize(path) > 15: break
            time.sleep(0.05)
        head, w_h, _, pix = open(path, "rb").read().split(b"\n", 3)
        w, h = map(int, w_h.split())
        return w, h, pix
    def close(self): self.q.kill(); self.q.wait()


def in_order(out, want):
    at = 0
    for w in want:
        at = out.find(w, at)
        if at < 0: return False
    return True


def window(out):
    m = re.search(r"M3c window reminders (\d+)x(\d+) at (\d+),(\d+)\n", out)
    return tuple(map(int, m.groups())) if m else None


def crop(img, x0, y0, w, h):   # rows of (r, g, b) bytes; window_scale is 1 at QEMU's 800x600
    W, _, pix = img
    return [pix[((y0 + j) * W + x0) * 3:((y0 + j) * W + x0 + w) * 3] for j in range(h)]


def accent(rows):   # pixels within a few levels of terracotta #b5502c: the done circle's fill (the title is the same ink, so count a box below it)
    return sum(1 for r in rows for i in range(len(r) // 3) if abs(r[i * 3] - 0xB5) < 12 and abs(r[i * 3 + 1] - 0x50) < 12 and abs(r[i * 3 + 2] - 0x2C) < 12)


def keep(img, name):
    d = os.environ.get("REMINDERS_SHOT")
    if d and img:
        os.makedirs(d, exist_ok=True)
        W, H, pix = img
        open(os.path.join(d, name + ".ppm"), "wb").write(b"P6\n%d %d\n255\n" % (W, H) + pix)


WANT = b"0 call mom\n1 buy milk\n"
MT = dict(os.environ, MTOOLS_SKIP_CHECK="1")
card = os.path.join(TMP, "card.img")
build_card(card, False)
subprocess.run(["make", "-C", arch, "clean"], capture_output=True)
try:
    r = subprocess.run(["make", "-C", arch, "reminderstest"], capture_output=True, text=True, timeout=900)
    if r.returncode: print("FAIL: arch/arm64 `make reminderstest` does not build:\n" + (r.stdout + r.stderr)[-1500:]); sys.exit(1)
    print("arm64 reminders: boot 1, remw-kernel8.img on a clean card")
    b = Boot(os.path.join(arch, "remw-kernel8.img"), card)
    try:
        up = b.wait_for("notestest: notes is up", 60)
        empty = b.shot() if up else None
        done = b.wait_for("notestest: done", 90)
        saved = b.shot() if done else None
        out1 = b.uart()
    finally:
        b.close()
    check("the dock's sixth tile opens Reminders from the keyboard (Right six times, Enter) and the kernel starts the app at EL0 in its own window, not the Console",
          up and out1.count("key 106 down\n") == 6 and in_order(out1, ["usb key 0x28\n", "M3c load reminders: ", "M3c app reminders starts\n", "M3c window reminders ", "reminders: ring-3 window "]) and "Reminders: not on ARM yet" not in out1, out1[-800:])
    check("no card yet: Reminders loads an empty list", "reminders: loaded 0\n" in out1 and "reminders: no card" not in out1, out1[-600:])
    check("two adds, one tick: counts 1, 2, then done 1 (the first item, sunk under the second)",
          in_order(out1, ["reminders: count 1\n", "reminders: count 2\n", "reminders: sel 1\n", "reminders: sel 0\n", "reminders: done 1\n", "notestest: done"]) and done, out1[-900:])
    for bad in ("M3 EL0 fault", "KERNEL CRASH", "sd FAIL", "M3c app reminders exit"):
        check("boot 1 never printed %r once Reminders started" % bad, bad not in out1.partition("M3c app reminders starts")[2], out1[-400:])
    wb = window(out1)
    if wb and empty and saved:
        keep(saved, "reminders"); keep(empty, "reminders-empty")
        cw, chh, cx, cy = wb
        e, s = crop(empty, cx, cy, cw, chh), crop(saved, cx, cy, cw, chh)
        below = lambda rows: rows[60:chh - 50]   # under the title and subtitle, above the text field
        check("on screen, the list has no terracotta before the adds and a done circle in it after the tick", accent(below(e)) == 0 and accent(below(s)) > 120,
              "before %d, after %d" % (accent(below(e)), accent(below(s))))

    print("arm64 reminders: the card from the host")
    def mt(*a): return subprocess.run([a[0], "-i", f"{card}@@{PART * 512}", *a[1:]], capture_output=True, env=MT)
    got = mt("mtype", "::JT/REMINDER.TXT").stdout
    check("JT/REMINDER.TXT holds i386's line format, the done item sunk to the bottom", got == WANT, repr(got))
    bad = []
    clean = fat_pass(card, "after Reminders saved", bad) is not None
    said = fsck(card, "after Reminders saved", bad, TMP)
    check("the FAT32 pass finds the card clean (%s)" % said, clean and not bad, bad)

    print("arm64 reminders: boot 2, remr-kernel8.img on the same card")
    b = Boot(os.path.join(arch, "remr-kernel8.img"), card)
    try:
        up = b.wait_for("notestest: notes is up", 60)
        time.sleep(1)
        listed = b.shot() if up else None
        out2 = b.uart()
    finally:
        b.close()
    check("after the reboot the item is still there, and still ticked", "reminders: loaded 2\n" in out2 and "reminders: checked 1\n" in out2, out2[-600:])
    for bad in ("M3 EL0 fault", "KERNEL CRASH", "sd FAIL", "M3c app reminders exit"):
        check("boot 2 never printed %r once Reminders started" % bad, bad not in out2.partition("M3c app reminders starts")[2], out2[-400:])
    wr = window(out2)
    if wb and wr and saved and listed:
        check("the window is where boot 1 had it", wr == wb, "%r %r" % (wb, wr))
        mask = lambda img, w: [(j, i) for j, r in enumerate(crop(img, *w[2:], w[0], w[1])) for i in range(w[0]) if 60 < j < w[1] - 70 and r[i * 3:i * 3 + 3] == b"\xb5\x50\x2c"]   # fully terracotta pixels under the title: the circle's core, the same on either row background
        check("on screen, the done circle after the reboot is exactly where boot 1 drew it (the row highlight differs: boot 1 left the row selected)",
              mask(listed, wr) == mask(saved, wb) and len(mask(listed, wr)) > 120, "%d vs %d" % (len(mask(listed, wr)), len(mask(saved, wb))))

    print("arm64 reminders: boot 3, remr-kernel8.img with no card")
    b = Boot(os.path.join(arch, "remr-kernel8.img"), None)
    try:
        up = b.wait_for("notestest: notes is up", 60)
        time.sleep(1)
        bare = b.shot() if up else None
        out3 = b.uart()
    finally:
        b.close()
    check("with no card Reminders says so on the UART and keeps running", "reminders: no card, memory only\n" in out3 and "M3c app reminders exit" not in out3, out3[-600:])
    w3 = window(out3)
    if w3 and bare:
        keep(bare, "reminders-nocard")
        cw, chh, cx, cy = w3
        sub = crop(bare, cx, cy, cw, chh)[18 + 14:18 + 40]
        check("on screen, the no-card line is in terracotta under the title", accent(sub) > 60 or accent(crop(bare, cx, cy, cw, chh)[40:90]) > 60, accent(sub))
finally:
    subprocess.run(["make", "-C", arch, "clean"], capture_output=True)

if fails:
    print("FAIL: %d of the Reminders checks failed" % len(fails)); sys.exit(1)
print("PASS: on QEMU's raspi4b the Reminders tile opens user/remindpi.c at EL0, two adds and a tick land in JT/REMINDER.TXT on the FAT32 card "
      "(mtools reads the lines, the card is clean), after a reboot the item is still there and ticked, and with no card the window says memory only")
