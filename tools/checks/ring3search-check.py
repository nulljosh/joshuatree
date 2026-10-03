#!/usr/bin/env python3
"""Search runs as a real ring-3 process, the sixteenth app out of the kernel
(roadmap 2.0, 1.9.13), on top of the one new call, SYS_READDIR (388).

Two headless boots, both with `open=sear`, which launches Search from the
dock path the moment the desktop is up. Search is user/search.c, a flat
binary loaded off the VFS by exec_user and run at CPL 3 through the
table-driven launcher (kernel/ring3app.c, RING3_APPS).

Boot 1, no disk (ramfs with the seeded README.TXT and NOTES.TXT):
  1. off the serial log: the program opened a window of the dock viewport's
     size (804x345), and its three startup probes came back refused the way
     the contract says: a kernel pointer is -EFAULT (14), a 79-byte path with
     no NUL inside the 64-byte cap is -EINVAL (22), a directory that is not
     there is -ENOENT (2). Then "search: listed N dirs 0" with N >= 2;
  2. the first two result rows carry ink and row 0 is highlighted;
  3. types "notes" through the real keyboard: "search: matches 1", the
     "searchcontent" marker grows once per keystroke, row 0 still has ink
     and row 1 is back to the page colour;
  4. Enter: "search: open NOTES.TXT", "search: file bytes N" with N > 0 and
     "search: head Joshua Tree", the seeded file's real first line, and the
     content view has ink where the text and the footer go;
  5. Esc goes back to the list (one more "searchcontent"), Esc again closes
     it: exit 0 and the window released;
  6. reopens Search from the Apps folder grid and presses backquote: the
     program page-faults on purpose and the kernel reaps it, window torn
     down, desktop alive;
  7. closes the folder: the dock is on screen and Mail opens from it.

Boot 2, a fresh FAT16 image with DOCS/HELLO.TXT inside it:
  8. "search: listed N dirs 1"; types "docs", Enter: "search: dir DOCS",
     "search: cwd DOCS" and a listing of 1 with no dirs, so the walk into
     the folder went through SYS_READDIR's relative path;
  9. types "hello", Enter: "search: open DOCS/HELLO.TXT" and the file's real
     bytes ("search: head hello from docs"), so SYS_OPEN walked the same
     relative path;
 10. Esc, Esc closes it; a second Search from the Apps folder lists the
     root again ("dirs 1"): the kernel's own cwd never moved.

Every wait has a deadline. Discriminating: drop the paging_user_range_ok
call in sys_readdir and step 1 fails (the probe reads 0, not 14); let
path_enter skip its walk back and step 10 fails; make refilter a no-op and
step 3 fails; return before drawing in show_file and step 4 fails.

Usage: tools/checks/ring3search-check.py   (from the repo root, after make kernel.elf)
"""
import json, os, socket, subprocess, sys, tempfile, time
from PIL import Image
from freeport import free_port

LOG = "/tmp/jt-ring3search-serial.log"
DUMP = "/tmp/jt-ring3search.raw"
DISK = "/tmp/jt-ring3search-fat.img"
FB = 0xfd000000; W, H = 1920, 1080
LOGICAL_W, LOGICAL_H, SCALE = 960, 540, 2
DOCK_ICON, DOCK_GAP, SLOT0_X = 37, 6, 247
PITCH = DOCK_ICON + DOCK_GAP
ICON_ROW_Y = 487
CLOSE_X, CLOSE_Y = 94, 56
CLOSE_RED = (0xFF, 0x5F, 0x57)
VIEW_X, VIEW_Y = 78, 72   # gui_launch_from_dock: viewport at (x+8, y+32) for x=70, y=40
PARK = (480, 200)
ROW_Y, ROW_H = 78, 22     # user/search.c: first row's text y, row pitch
PROBE_X = 600             # right of any filename: only the highlight or the page colour
SEL_COLOR, BG_COLOR = (0xED, 0xE6, 0xDC), (0xFA, 0xF8, 0xF6)
HELLO_TEXT = "hello from docs\nsecond line\n"

os.chdir(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
fails = []


def run_boot(label, extra_args, body):
    for f in (LOG, DUMP):
        try: os.remove(f)
        except FileNotFoundError: pass
    port = free_port()
    q = subprocess.Popen(["qemu-system-i386", "-kernel", "kernel.elf", "-append", "open=sear",
                          "-display", "none", "-vga", "std", "-name", f"jt-ring3search-{label}",
                          "-qmp", f"tcp:127.0.0.1:{port},server,nowait", "-serial", "file:" + LOG] + extra_args,
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    cmd = None
    try:
        s = None
        for _ in range(50):
            time.sleep(0.2)
            try: s = socket.create_connection(("127.0.0.1", port)); break
            except OSError: pass
        if s is None: raise SystemExit("FAIL: QEMU's QMP socket never came up")
        f = s.makefile("rw")
        def cmd(o):
            f.write(json.dumps(o) + "\n"); f.flush()
            while True:
                r = json.loads(f.readline())
                if "return" in r or "error" in r: return r
        f.readline()
        cmd({"execute": "qmp_capabilities"})
        body(cmd)
    finally:
        try: cmd({"execute": "quit"})
        except (ConnectionResetError, BrokenPipeError, OSError, ValueError, TypeError): pass
        q.terminate()
        try: q.wait(5)
        except subprocess.TimeoutExpired: q.kill()


def serial():
    try: return open(LOG, errors="replace").read()
    except OSError: return ""
def wait_serial(needle, secs, count=1):
    for _ in range(int(secs * 10)):
        if serial().count(needle) >= count: return True
        time.sleep(0.1)
    return False
def serial_int(prefix):
    """The number after the last `prefix` line, or None."""
    val = None
    for line in serial().splitlines():
        at = line.find(prefix)  # the kernel echoes ring-3 writes as "syscall: write(1) from ring 3: <line>"
        if at >= 0:
            try: val = int(line[at + len(prefix):].strip().split()[0])
            except (ValueError, IndexError): pass
    return val


class Drive:
    def __init__(self, cmd):
        self.cmd = cmd
    def keys(self, *qcodes):
        r = self.cmd({"execute": "send-key", "arguments": {"keys": [{"type": "qcode", "data": k} for k in qcodes]}})
        if "error" in r: raise SystemExit(f"FAIL: QMP rejected send-key {qcodes}: {r['error']}")
        time.sleep(0.35)  # faster than this drops scancodes on a loaded host
    def typed(self, word):
        n0 = serial().count("searchcontent\n")
        for i, ch in enumerate(word):
            self.keys({".": "dot"}.get(ch, ch))
            wait_serial("searchcontent\n", 5, n0 + i + 1)
    def move(self, x, y):
        self.cmd({"execute": "input-send-event", "arguments": {"events": [
            {"type": "abs", "data": {"axis": "x", "value": int(x * 32768 / LOGICAL_W)}},
            {"type": "abs", "data": {"axis": "y", "value": int(y * 32768 / LOGICAL_H)}}]}})
    def click(self):
        self.cmd({"execute": "input-send-event", "arguments": {"events": [{"type": "btn", "data": {"down": True, "button": "left"}}]}})
        time.sleep(0.1)
        self.cmd({"execute": "input-send-event", "arguments": {"events": [{"type": "btn", "data": {"down": False, "button": "left"}}]}})
    def frame(self):
        self.cmd({"execute": "pmemsave", "arguments": {"val": FB, "size": W * H * 4, "filename": DUMP}})
        return Image.frombytes("RGBA", (W, H), open(DUMP, "rb").read(), "raw", "BGRA").convert("RGB")
    def pixel(self, x, y, img=None):
        return (img or self.frame()).getpixel((x * SCALE + 1, y * SCALE + 1))
    def row_pixel(self, i, img=None):
        return self.pixel(VIEW_X + PROBE_X, VIEW_Y + ROW_Y - 4 + i * ROW_H + 8, img)
    def ink(self, x, y, w, h, img=None):
        img = img or self.frame()
        x0, y0 = (VIEW_X + x) * SCALE, (VIEW_Y + y) * SCALE
        crop = img.crop((x0, y0, x0 + w * SCALE, y0 + h * SCALE)).tobytes()
        # anything clearly darker than the page (lum ~247) and the highlight (~225): INK, GREEN and the HINT grey all count
        return sum(1 for k in range(0, len(crop), 3) if (crop[k] * 299 + crop[k + 1] * 587 + crop[k + 2] * 114) // 1000 < 180)
    def row_ink(self, i, img=None):
        return self.ink(28, ROW_Y + i * ROW_H, 120, 16, img)
    def open_from_folder(self):
        n = serial().count("appsfullrepaint")
        self.move(SLOT0_X + DOCK_ICON // 2, ICON_ROW_Y); time.sleep(0.3); self.click()
        if not wait_serial("appsfullrepaint", 10, n + 1):
            fails.append("the Apps folder did not open from the dock"); return False
        time.sleep(0.6)
        for c in ("d", "d", "d", "d", "s", "s", "s"): self.keys(c)  # icon 19: row 3, col 4 (APPS_COLS = 5)
        self.keys("ret")
        return True


def near(p, c, tol=12): return max(abs(p[i] - c[i]) for i in range(3)) <= tol


def expect_launch(tag):
    if not wait_serial("ring3app: launching SEARCH.BIN at ring 3", 40):
        fails.append(f"{tag}: Search was never launched as a ring-3 program (open=sear flag or ring3app.c broken)")
    if not wait_serial("syscall: window opened for ring-3 task", 10):
        fails.append(f"{tag}: SYS_WINDOW_OPEN never succeeded from ring 3")
    if not wait_serial("search: ring-3 window 804x345", 10):
        fails.append(f"{tag}: the program did not report the app viewport's size (expected 804x345) through write()")
    if not wait_serial("search: listed ", 10):
        fails.append(f"{tag}: no \"search: listed\" line: SYS_READDIR never answered")
    if not wait_serial("searchcontent\n", 10):
        fails.append(f"{tag}: no 'searchcontent' marker: the first draw never ran")


def close_clean(d, tag):
    exits = serial().count("SEARCH.BIN exited 0")
    d.keys("esc")
    if not wait_serial("search: closed", 5):
        fails.append(f"{tag}: Esc did not reach the program (no closed line)")
    if not wait_serial("SEARCH.BIN exited 0", 5, exits + 1):
        fails.append(f"{tag}: Search did not exit 0 on Esc")
    if not wait_serial("syscall: window released, task gone", 5, exits + 1):
        fails.append(f"{tag}: Search did not release its window on Esc")


def ramfs_boot(cmd):
    d = Drive(cmd)
    # 1. up, probed, listed
    expect_launch("ramfs")
    for name, want, what in (("efault", 14, "a kernel pointer as the output array"),
                             ("einval", 22, "a 79-byte path with no NUL inside the cap"),
                             ("enoent", 2, "a directory that does not exist")):
        got = serial_int(f"search: probe {name} ")
        print(f"SYS_READDIR with {what}: -{got}")
        if got != want:
            fails.append(f"SYS_READDIR did not refuse {what} with -{want} (got {got})")
    listed = serial_int("search: listed ")
    print(f"ramfs listing: {listed} entries")
    if listed is None or listed < 2:
        fails.append(f"expected README.TXT and NOTES.TXT in the ramfs listing, got {listed}")
    if "dirs 1" not in serial():  # ramfs_seed_demo_docs() seeds one real folder for the tour's Burrow scene
        fails.append("ramfs has exactly the one seeded demo folder but the listing did not report it")
    time.sleep(0.5)

    # 2. two rows of ink, row 0 highlighted
    img = d.frame()
    r0, r1 = d.row_pixel(0, img), d.row_pixel(1, img)
    ink0, ink1 = d.row_ink(0, img), d.row_ink(1, img)
    print(f"row 0 {r0} ink {ink0}, row 1 {r1} ink {ink1}")
    if not near(r0, SEL_COLOR): fails.append(f"row 0 did not draw the selected colour (got {r0})")
    if not near(r1, BG_COLOR): fails.append(f"row 1 is not the page colour (got {r1})")
    if ink0 < 20 or ink1 < 20: fails.append(f"the first two files are not both on screen (ink {ink0} / {ink1})")

    # 3. live filter
    m0 = serial().count("searchcontent\n")
    d.typed("notes")
    if not wait_serial("searchcontent\n", 5, m0 + 5):
        fails.append("typing five letters did not redraw the content once per keystroke")
    if not wait_serial("search: matches 1\n", 5):
        fails.append('typing "notes" did not narrow the list to one match ("search: matches 1")')
    time.sleep(0.4)
    img = d.frame()
    ink0, ink1 = d.row_ink(0, img), d.row_ink(1, img)
    print(f"after 'notes': row 0 ink {ink0}, row 1 ink {ink1}")
    if ink0 < 20: fails.append("NOTES.TXT should still match 'notes' but row 0 went blank")
    if ink1 >= 6: fails.append("row 1 still shows text after 'notes': the filter is not live")

    # 4. Enter shows the real bytes
    d.keys("ret")
    if not wait_serial("search: open NOTES.TXT", 5):
        fails.append('Enter did not open NOTES.TXT ("search: open NOTES.TXT")')
    if not wait_serial("searchfile\n", 5):
        fails.append("the file view never drew (no searchfile marker)")
    nbytes = serial_int("search: file bytes ")
    print(f"NOTES.TXT bytes read through open/read: {nbytes}")
    if not nbytes: fails.append(f"the file view read no bytes ({nbytes})")
    if "search: head Joshua Tree" not in serial():
        fails.append("the file's first line is not the seeded NOTES.TXT text (\"Joshua Tree\")")
    time.sleep(0.4)
    img = d.frame()
    body, footer = d.ink(20, 44, 400, 60, img), d.ink(20, 345 - 30, 200, 16, img)
    print(f"file view ink: body {body}, footer {footer}")
    if body < 40: fails.append(f"the file view shows no text (ink {body})")
    if footer < 20: fails.append(f"the file view has no 'esc or click to go back' footer (ink {footer})")

    # 5. back to the list, then close
    m1 = serial().count("searchcontent\n")
    d.keys("esc")
    if not wait_serial("searchcontent\n", 5, m1 + 1):
        fails.append("Esc in the file view did not go back to the list")
    close_clean(d, "ramfs")
    d.move(*PARK); time.sleep(0.5)
    for _ in range(100):
        if not near(d.pixel(CLOSE_X, CLOSE_Y), CLOSE_RED): break
        time.sleep(0.1)
    if near(d.pixel(CLOSE_X, CLOSE_Y), CLOSE_RED):
        fails.append("an app window is still open after Esc")

    # 6. crash on purpose from the Apps folder
    launches = serial().count("ring3app: launching SEARCH.BIN")
    if d.open_from_folder():
        if not wait_serial("ring3app: launching SEARCH.BIN", 10, launches + 1):
            fails.append("Search did not launch from the Apps folder grid (icon 19)")
        wait_serial("searchcontent\n", 10, serial().count("searchcontent\n") + 1)
        time.sleep(0.3)
        d.keys("grave_accent")
        if not wait_serial("search: crashing on purpose", 5):
            fails.append("backquote did not reach the program")
        if not wait_serial("SEARCH.BIN crashed (page-fault), window torn down, desktop alive", 10):
            fails.append("the deliberate null write was not reaped as a ring-3 page fault with the desktop alive")
        n = serial().count("appsfullrepaint")
        wait_serial("appsfullrepaint", 10, n)  # the folder repaints under the torn-down window
        time.sleep(0.6)

    # 7. the desktop answers
    d.keys("esc"); time.sleep(0.8)
    if "exception: ring-0" in serial() or "panic in" in serial() or "ring3app: BUG" in serial():
        fails.append("the kernel faulted or ring3app logged a BUG line")
    d.move(*PARK); time.sleep(0.5)
    dock = d.pixel(480, 511)
    if dock != (0xEF, 0xEB, 0xE4):
        fails.append(f"desktop dock not on screen after the close (got {dock})")
    d.move(SLOT0_X + 2 * PITCH + DOCK_ICON // 2, ICON_ROW_Y); time.sleep(0.3); d.click()
    opened = False
    for _ in range(40):
        time.sleep(0.1)
        if near(d.pixel(CLOSE_X, CLOSE_Y), CLOSE_RED): opened = True; break
    print(f"Mail opens from the dock after the crash: {'yes' if opened else 'NO'}")
    if not opened: fails.append("Mail did not open from a dock click after Search crashed: desktop not responsive")
    d.keys("esc"); time.sleep(0.5)


def fat_boot(cmd):
    d = Drive(cmd)
    # 8. the root has DOCS/, Enter walks into it
    expect_launch("fat")
    if not wait_serial("fat_mount: FAT16", 10) and "fat_mount: FAT16" not in serial():
        fails.append("fat: the FAT16 image did not mount")
    if not wait_serial("dirs 1", 5):
        fails.append(f"fat: the root listing did not report one directory (DOCS): {serial_int('search: listed ')} entries")
    time.sleep(0.4)
    d.typed("docs")
    if not wait_serial("search: matches 1\n", 5):
        fails.append('fat: typing "docs" did not narrow the list to DOCS')
    d.keys("ret")
    if not wait_serial("search: dir DOCS", 5):
        fails.append("fat: Enter on DOCS/ did not try to step into it")
    if not wait_serial("search: cwd DOCS", 5):
        fails.append("fat: SYS_READDIR refused the relative path DOCS (the app kept the root)")
    if not wait_serial("search: listed 1 dirs 0", 5):
        fails.append("fat: inside DOCS the listing should be HELLO.TXT alone")

    # 9. the file inside opens through a relative path
    d.typed("hello")
    if not wait_serial("search: matches 1\n", 5, 2):
        fails.append('fat: typing "hello" did not narrow the list inside DOCS')
    d.keys("ret")
    if not wait_serial("search: open DOCS/HELLO.TXT", 5):
        fails.append("fat: Enter did not open DOCS/HELLO.TXT by its relative path")
    if not wait_serial("search: head hello from docs", 5):
        fails.append("fat: the bytes shown are not HELLO.TXT's real content (SYS_OPEN did not walk the path)")
    nbytes = serial_int("search: file bytes ")
    print(f"DOCS/HELLO.TXT bytes read: {nbytes} (want {len(HELLO_TEXT)})")
    if nbytes != len(HELLO_TEXT):
        fails.append(f"fat: DOCS/HELLO.TXT read {nbytes} bytes, the image holds {len(HELLO_TEXT)}")

    # 10. close; a fresh Search still lists the root: the kernel cwd never moved
    m1 = serial().count("searchcontent\n")
    d.keys("esc")
    wait_serial("searchcontent\n", 5, m1 + 1)
    close_clean(d, "fat")
    d.move(*PARK); time.sleep(0.5)
    for _ in range(100):
        if not near(d.pixel(CLOSE_X, CLOSE_Y), CLOSE_RED): break
        time.sleep(0.1)
    launches = serial().count("ring3app: launching SEARCH.BIN")
    if d.open_from_folder():
        if not wait_serial("ring3app: launching SEARCH.BIN", 10, launches + 1):
            fails.append("fat: Search did not relaunch from the Apps folder")
        if not wait_serial("dirs 1", 10, 2):
            fails.append("fat: the second Search did not list DOCS/ at the root: the kernel's cwd was left inside DOCS")
        listed = serial_int("search: listed ")
        print(f"second Search at the root: {listed} entries")
        time.sleep(0.3)
        d.keys("esc"); time.sleep(0.8)
    d.keys("esc"); time.sleep(0.5)
    if "syscall: BUG" in serial() or "exception: ring-0" in serial() or "panic in" in serial():
        fails.append("fat: the kernel logged a BUG or faulted")


def make_disk():
    subprocess.run(["bash", "tools/mkdisk.sh", DISK], check=True, stdout=subprocess.DEVNULL)
    with tempfile.NamedTemporaryFile("w", suffix=".txt", delete=False) as t:
        t.write(HELLO_TEXT); hello = t.name
    try:
        subprocess.run(["mmd", "-i", DISK, "::DOCS"], check=True)
        subprocess.run(["mcopy", "-i", DISK, hello, "::DOCS/HELLO.TXT"], check=True)
    finally:
        os.remove(hello)


run_boot("ramfs", [], ramfs_boot)
try:
    make_disk()
except (subprocess.CalledProcessError, FileNotFoundError) as e:
    fails.append(f"could not build the FAT16 image with DOCS/HELLO.TXT (mkfs.vfat, mmd, mcopy): {e}")
else:
    run_boot("fat", ["-drive", f"file={DISK},format=raw,if=ide,index=0"], fat_boot)

if fails:
    print("FAIL:")
    for x in fails: print("  - " + x)
    print("--- serial tail ---")
    print(serial()[-1500:])
    sys.exit(1)
print("PASS: Search ran at ring 3 with its own window, SYS_READDIR refused a kernel pointer, an over-long path and a missing directory, the list filtered live, a file showed its real bytes, DOCS/ opened by relative path with the kernel's cwd untouched, Esc closed it, a crash was reaped, and the desktop stayed alive")
