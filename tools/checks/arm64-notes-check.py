#!/usr/bin/env python3
"""ARM64 M3d: Notes runs on the Pi, and a note typed there is on the card after a reboot.

QEMU's Raspberry Pi 4B model with arm64-sd-check.py's clean FAT32 card (tools/checks/sdcard.py) in EMMC2. QEMU's raspi4b
has no USB, so the two test builds (`make -C arch/arm64 notestest`) feed made-up boot keyboard reports through xhci.c's
hid_kbd, the parser a real keyboard's reports go through, one key every 150 ms from the desktop's loop:
  1. notesw-kernel8.img: Right five times and Enter walk the dock to its fifth tile, Notes. The kernel makes JT/NOTES/NOTES
     (the default folder, as i386 does) and starts the unchanged user/notes.c at EL0; its window opens with an empty list
     ("notes: folders=01 notes=00", a screendump with no ink in the notes column). Then n (a new note), "hello from the
     pi" and Esc: Notes saves through open, write and close ("notes: saved=17") and lists the note by its first line.
  2. From the host, with mtools: JT/NOTES/NOTES/N0000001.TXT holds exactly "hello from the pi", and the FAT32 pass (and
     fsck.fat -n where installed) finds the card clean.
  3. notesr-kernel8.img on the same card, as after a reboot: the same keys open Notes, which lists the note
     ("notes: folders=01 notes=01"); the window is pixel for pixel the one boot 1 left after saving; then Enter opens it
     in the editor ("notes: edit=N0000001.TXT"), with ink on its first line and none on the second.
  4. Autosave with no network (4.11.2): notesa-kernel8.img types a line and never presses Esc, waits four seconds, then clicks
     the window's red dot, which ends the program the kill way. The Pi has no clock until the network sets one, so JT_SYS_TIME
     stays 0 here and Notes must time its two seconds on the uptime ticks (SYS_TASKS) instead. The program saves the line by
     itself ("notes: saved=23" once, no "notes: closed", "closed by its red dot"), mtools finds it on the card, and notesr on
     the same card lists it and opens it with ink on the first line. `--mutate` puts the old JT_SYS_TIME timing back in
     user/notes.c, rebuilds, runs only this case and passes only if it FAILS (the file is restored afterwards).
Once Notes starts, no EL0 fault, no KERNEL CRASH, no `sd FAIL` and no exit, in either boot. Skips (exit 0) without clang, ld.lld, qemu-system-aarch64,
its raspi4b model, or mtools.
Usage: tools/checks/arm64-notes-check.py   (from the repo root)
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
TMP = scratch_dir("notes")
LINE = b"hello from the pi"
fails = []


def check(name, ok, detail=""):
    print(("  ok: " if ok else "  FAIL: ") + name + (("  (" + str(detail)[-500:] + ")") if detail and not ok else ""))
    if not ok: fails.append(name)


class Boot:
    """One raspi4b boot on the card, with QMP for screendumps. Runs until `notestest: done` or a minute."""
    def __init__(self, img, card):
        Boot.n = getattr(Boot, "n", 0) + 1
        self.log, sock = os.path.join(TMP, "uart%d" % Boot.n), os.path.join(TMP, "qmp%d" % Boot.n)   # short: a unix socket path has a 104 byte limit on macOS
        self.q = subprocess.Popen(["qemu-system-aarch64", "-machine", "raspi4b", "-display", "none", "-serial", "null", "-serial", "file:" + self.log,   # serial 0 is the PL011, the radio's; the console is the mini UART
                                   "-qmp", "unix:%s,server,nowait" % sock, "-kernel", img,
                                   "-drive", "if=none,id=card,file=%s,format=raw" % card, "-device", "sd-card,drive=card,bus=/generic-sdhci/sd-bus"],
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
    m = re.search(r"M3c window notes (\d+)x(\d+) at (\d+),(\d+)\n", out)
    return tuple(map(int, m.groups())) if m else None


def crop(img, x0, y0, w, h):   # rows of (r, g, b) bytes; window_scale is 1 at QEMU's 800x600
    W, _, pix = img
    return [pix[((y0 + j) * W + x0) * 3:((y0 + j) * W + x0 + w) * 3] for j in range(h)]


def ink(rows, x0, y0, w, h):   # dark pixels in a box of the crop: only INK text (0x1C1C1E and its antialiasing) is this dark
    return sum(1 for j in range(y0, y0 + h) for i in range(x0, x0 + w) if max(rows[j][i * 3:i * 3 + 3]) < 0x60)


# notes.c's layout at a window under 600 wide: the notes column starts at 20 + 100 + 16, is 170 wide, and its first row's
# text sits at COL_Y + 46 - 4 = 82; the editor's first line at ED_TOP 28, 33 rows a line, from ED_MARGIN 56.
NOTES_COL, FIRST_ROW, EDIT_LINE0, EDIT_LINE1 = (136, 170), (74, 22), (28, 33), (61, 33)
AUTOTEXT = b"autosaved with no clock"


def autosave_case():
    """Section 4. A fresh card, notesa (types, waits, red dot), the card read from the host, then notesr to find the text again."""
    card2 = os.path.join(TMP, "card2.img")
    build_card(card2, False)
    print("arm64 notes: autosave with no network clock, notesa-kernel8.img on a clean card")
    b = Boot(os.path.join(arch, "notesa-kernel8.img"), card2)
    try:
        done = b.wait_for("notestest: done", 90)
        out = b.uart()
    finally:
        b.close()
    check("the line is typed, left alone for four seconds, and the red dot is clicked", done and in_order(out, ["notes: new=N0000001.TXT", "notestest: waiting\n", "notestest: red dot\n", "notestest: done"]), out[-900:])
    check("Notes saved the %d bytes by itself, exactly once, before the red dot" % len(AUTOTEXT),
          done and out.count("notes: saved=") == 1 and in_order(out, ["notes: saved=%d\n" % len(AUTOTEXT), "notestest: red dot\n"]), out[-900:])
    check("the red dot ended the program the kill way: 'closed by its red dot', no 'notes: closed' (Esc's exit)",
          "M3c app notes closed by its red dot" in out and "notes: closed" not in out, out[-600:])
    for bad in ("M3 EL0 fault", "KERNEL CRASH", "sd FAIL"):
        check("the autosave boot never printed %r once Notes started" % bad, bad not in out.partition("M3c app notes starts")[2], out[-400:])
    def mt2(*a): return subprocess.run([a[0], "-i", f"{card2}@@{PART * 512}", *a[1:]], capture_output=True, env=MT)
    got = mt2("mtype", "::JT/NOTES/NOTES/N0000001.TXT").stdout
    check("JT/NOTES/NOTES/N0000001.TXT on the card holds exactly the typed line", got == AUTOTEXT, repr(got))
    bad = []
    clean = fat_pass(card2, "after the autosave", bad) is not None
    check("the FAT32 pass finds the card clean after the red-dot kill", clean and not bad, bad)
    print("arm64 notes: autosave, notesr-kernel8.img on the same card")
    b = Boot(os.path.join(arch, "notesr-kernel8.img"), card2)
    try:
        up = b.wait_for("notestest: notes is up", 60)
        done = b.wait_for("notestest: done", 60)
        editor = b.shot() if done else None
        out2 = b.uart()
        wr = window(out2)
    finally:
        b.close()
    check("after the reboot Notes lists the autosaved note and Enter opens it", "notes: folders=01 notes=01\n" in out2 and "notes: edit=N0000001.TXT\n" in out2, out2[-600:])
    if wr and editor:
        cw, chh, cx, cy = wr
        l0 = ink(crop(editor, cx, cy, cw, chh), 56, EDIT_LINE0[0], cw - 112, EDIT_LINE0[1])
        check("on screen, the reopened note shows its text: ink on the first line", l0 > 40, "ink %d" % l0)
    else:
        check("the reopened note's window was captured", False, out2[-400:])


MT = dict(os.environ, MTOOLS_SKIP_CHECK="1")
card = os.path.join(TMP, "card.img")
if "--mutate" in sys.argv:   # the old timing back: JT_SYS_TIME is 0 with no clock, so 0 - 0 >= 2 never fires and the line is never saved
    notes_c = os.path.join(root, "user/notes.c")
    orig = open(notes_c).read()
    good = "static unsigned now_ticks(void) { struct jt_tasks t; jt_tasks(&t, -1); return t.ticks; }"
    if good not in orig: print("FAIL: mutation target not found in user/notes.c"); sys.exit(2)
    open(notes_c, "w").write(orig.replace(good, "static unsigned now_ticks(void) { return (unsigned)jt_time(0) * 100; }"))
    subprocess.run(["make", "-C", arch, "clean"], capture_output=True)
    n0 = len(fails)
    try:
        r = subprocess.run(["make", "-C", arch, "notesa-kernel8.img", "notesr-kernel8.img"], capture_output=True, text=True, timeout=900)
        if r.returncode: print("FAIL: the mutant does not build:\n" + (r.stdout + r.stderr)[-1500:]); sys.exit(2)
        autosave_case()
    finally:
        open(notes_c, "w").write(orig)
        subprocess.run(["make", "-C", arch, "clean"], capture_output=True)
    if len(fails) > n0: print("PASS (mutation caught): with the JT_SYS_TIME timing the autosave case fails, %d checks" % (len(fails) - n0)); sys.exit(0)
    print("FAIL: the old JT_SYS_TIME timing still passes the autosave case"); sys.exit(1)
build_card(card, False)
subprocess.run(["make", "-C", arch, "clean"], capture_output=True)
try:
    r = subprocess.run(["make", "-C", arch, "notestest"], capture_output=True, text=True, timeout=900)
    if r.returncode: print("FAIL: arch/arm64 `make notestest` does not build:\n" + (r.stdout + r.stderr)[-1500:]); sys.exit(1)
    print("arm64 notes: boot 1, notesw-kernel8.img on a clean card")
    b = Boot(os.path.join(arch, "notesw-kernel8.img"), card)
    try:
        up = b.wait_for("notestest: notes is up", 60)
        empty = b.shot() if up else None
        done = b.wait_for("notestest: done", 60)
        saved = b.shot() if done else None
        out1 = b.uart()
    finally:
        b.close()
    check("the dock's fifth tile opens Notes from the keyboard (Right five times, Enter) and the kernel starts user/notes.c at EL0 in its own window",
          up and out1.count("key 106 down\n") == 5 and in_order(out1, ["usb key 0x28\n", "M3c load notes: ", "M3c app notes starts\n", "M3c window notes 560x"]), out1[-800:])
    check("Notes finds JT/NOTES with its default folder and no notes yet", "notes: ring-3 window\nnotes: folders=01 notes=00\n" in out1, out1[-600:])
    wb = window(out1)
    want = ["notestest: notes is up", "notes: new=N0000001.TXT", "notes: edit=N0000001.TXT", "M3c event key 259 to notes", "notes: saved=17", "notestest: done"]
    at = 0
    for w in want:
        at = out1.find(w + "\n", at)
        if at < 0: break
    check("n makes N0000001.TXT, the line is typed, Esc saves its 17 bytes through open, write and close", at >= 0 and done, out1[-900:])
    for bad in ("M3 EL0 fault", "KERNEL CRASH", "sd FAIL", "M3c app notes exit"):   # after Notes starts: the M3a self-test faults on purpose before
        check("boot 1 never printed %r once Notes started" % bad, bad not in out1.partition("M3c app notes starts")[2], out1[-400:])
    if wb and empty and saved:
        cw, chh, cx, cy = wb
        e, s = crop(empty, cx, cy, cw, chh), crop(saved, cx, cy, cw, chh)
        check("on screen, the notes column was empty before and lists the note by its line after Esc",
              ink(e, NOTES_COL[0], FIRST_ROW[0], NOTES_COL[1], FIRST_ROW[1]) == 0 and ink(s, NOTES_COL[0], FIRST_ROW[0], NOTES_COL[1], FIRST_ROW[1]) > 40,
              "ink before %d, after %d" % (ink(e, NOTES_COL[0], FIRST_ROW[0], NOTES_COL[1], FIRST_ROW[1]), ink(s, NOTES_COL[0], FIRST_ROW[0], NOTES_COL[1], FIRST_ROW[1])))

    print("arm64 notes: the card from the host")
    def mt(*a): return subprocess.run([a[0], "-i", f"{card}@@{PART * 512}", *a[1:]], capture_output=True, env=MT)
    note = mt("mtype", "::JT/NOTES/NOTES/N0000001.TXT").stdout
    listing = sorted(mt("mdir", "-/", "-b", "::JT").stdout.decode(errors="replace").split())
    check("JT/NOTES/NOTES/N0000001.TXT holds exactly the typed line", note == LINE, repr(note))
    check("JT/ holds only NOTES/, NOTES/NOTES/ and the note", listing == ["::/JT/NOTES/", "::/JT/NOTES/NOTES/", "::/JT/NOTES/NOTES/N0000001.TXT"], listing)
    bad = []
    clean = fat_pass(card, "after Notes saved", bad) is not None
    said = fsck(card, "after Notes saved", bad, TMP)
    check("the FAT32 pass finds the card clean (%s)" % said, clean and not bad, bad)

    print("arm64 notes: boot 2, notesr-kernel8.img on the same card")
    b = Boot(os.path.join(arch, "notesr-kernel8.img"), card)
    try:
        up = b.wait_for("notestest: notes is up", 60)
        listed = b.shot() if up else None
        done = b.wait_for("notestest: done", 60)
        editor = b.shot() if done else None
        out2 = b.uart()
    finally:
        b.close()
    check("after the reboot Notes lists the note", "notes: folders=01 notes=01\n" in out2, out2[-600:])
    check("Enter opens it in the editor", re.search(r"notestest: notes is up\n(.*\n)*M3c event key 258 to notes\n(.*\n)*notes: edit=N0000001.TXT\n", out2) and done, out2[-600:])
    for bad in ("M3 EL0 fault", "KERNEL CRASH", "sd FAIL", "M3c app notes exit"):
        check("boot 2 never printed %r once Notes started" % bad, bad not in out2.partition("M3c app notes starts")[2], out2[-400:])
    wr = window(out2)
    if wb and wr and saved and listed and editor:
        cw, chh, cx, cy = wr
        check("the window is where boot 1 had it", wr == wb, "%r %r" % (wb, wr))
        check("on screen, the list after the reboot is pixel for pixel the list boot 1 drew after saving",
              crop(listed, cx, cy, cw, chh) == crop(saved, *wb[2:], cw, chh))
        ed = crop(editor, cx, cy, cw, chh)
        l0, l1 = ink(ed, 56, EDIT_LINE0[0], cw - 112, EDIT_LINE0[1]), ink(ed, 56, EDIT_LINE1[0], cw - 112, EDIT_LINE1[1])
        check("on screen, the editor shows the line read back off the card: ink on the first line, none on the second", l0 > 40 and l1 == 0, "first %d, second %d" % (l0, l1))
    autosave_case()
finally:
    subprocess.run(["make", "-C", arch, "clean"], capture_output=True)

if fails:
    print("FAIL: %d of the Notes checks failed" % len(fails)); sys.exit(1)
print("PASS: on QEMU's raspi4b the dock opens the unchanged user/notes.c at EL0, a typed note is saved through the ARM file calls to "
      "JT/NOTES/NOTES/N0000001.TXT on the FAT32 card (mtools reads the line, the card is clean), and after a reboot Notes lists it and opens it; with no network clock a typed line saves itself on the uptime ticks and survives a red-dot close")
