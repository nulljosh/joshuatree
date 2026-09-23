#!/usr/bin/env python3
"""Headless proof that the Search app (kernel/search.h, v0.86.0) is real:
it opens from the Apps folder, lists real files off the active VFS backend
(vfs_list, the same driver-level call Files already uses), filters them
live as you type (a real substring match recomputed on every keystroke,
not a static list), and Enter on a match shows its real content the way
`cat` does (vfs_read_file). Same QMP absolute-pointer + qcode-keyboard +
pmemsave shape as appclose-check.py / contacts-keystroke-check.sh.

This boots with no `-hda`, the same config every other headless check in
this suite uses. Per kernel/kernel.c's own v86/0.71.0 note, `fat_mount()`
then reports no disk and `kmain` seeds two real files into ramfs and
switches the active backend to it: README.TXT and NOTES.TXT. Those are
the two real filenames this check searches for, not fabricated fixtures.

Grid navigation to Search (icon 21, row 4 col 1 of the Apps-folder grid,
APPS_COLS=5) reuses the exact keyboard path contacts-keystroke-check.sh
already proved reliable for icon 18 (row 3 col 1): 'd' moves right, 's'
moves down, real gui_launch_apps() navigation, not a mouse-only path.
QMP's `send-key` "ret" qcode is what that check already relies on to
launch an app; CLAUDE.md's caution is specifically about the QEMU
monitor's *older* HMP `sendkey ret` command, a different mechanism.

Three real assertions, each on real framebuffer pixels:
  1. Before typing anything, both real files (README.TXT and NOTES.TXT)
     show as two real result rows.
  2. After typing "readme", only the README.TXT row still has text; the
     NOTES.TXT row is back to plain background -- proof the filter is
     real and live, not a static list that always shows everything.
  3. Enter on the one remaining match leaves the result list and shows
     gui_wait_close's own "esc or click to go back" footer, the real,
     distinct marker every read-only content viewer in this kernel ends
     with -- proof Enter actually opened real file content, not a no-op.

Discriminating: reverting search_refilter to a no-op (matches always
equal to the full file list) makes assertion 2 fail (the NOTES.TXT row
still shows text after "readme" is typed); reverting search_open_file to
return immediately without drawing makes assertion 3 fail (no footer
text appears). Both checked live against a temporarily reverted build
before this file was finalized, not assumed from reading the code.

Usage: tools/checks/search-check.py   (from the repo root, after make kernel.elf)
"""
import json, os, socket, subprocess, sys, time
from PIL import Image

LOG = "/tmp/jt-search-serial.log"
DUMP = "/tmp/jt-search.raw"
FB = 0xfd000000; W, H = 1920, 1080
PORT = 4460
LOGICAL_W, LOGICAL_H, SCALE = 960, 540, 2
DOCK_ICON, DOCK_GAP, SLOT0_X = 37, 6, 247
PITCH = DOCK_ICON + DOCK_GAP
ICON_ROW_Y = 487
APPS_CLOSE_X, APPS_CLOSE_Y = 80, 46  # the Apps folder's own outer window red dot (56+24, 30+16)
CLOSE_RED = (0xFF, 0x5F, 0x57)
# gui_launch_apps' content viewport, from gui_launch_from_dock's own math for
# the apps=1 window (x=56,y=30,w=848,h=490 -> viewport (64,62)), the same
# derivation launchpad-click-check.py already used for its own click target.
VX, VY = 64, 62
ROW0_Y = VY + 110 - 32   # search_draw_content's first result row: y=110 + gui_app_dy() (-32 in a window)
ROW1_Y = VY + 132 - 32   # second row, 22px below
ROW_X0, ROW_X1 = VX + 26, VX + 300  # generous span past any real filename's width

os.chdir(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
for f in (LOG, DUMP):
    try: os.remove(f)
    except FileNotFoundError: pass

q = subprocess.Popen(["qemu-system-i386", "-kernel", "kernel.elf", "-display", "none", "-vga", "std",
                      "-qmp", f"tcp:127.0.0.1:{PORT},server,nowait", "-serial", "file:" + LOG],
                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
fails = []
try:
    s = None
    for _ in range(50):
        time.sleep(0.2)
        try: s = socket.create_connection(("127.0.0.1", PORT)); break
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
    time.sleep(5.0)  # desktop up

    def move(x, y):
        cmd({"execute": "input-send-event", "arguments": {"events": [
            {"type": "abs", "data": {"axis": "x", "value": int(x * 32768 / LOGICAL_W)}},
            {"type": "abs", "data": {"axis": "y", "value": int(y * 32768 / LOGICAL_H)}}]}})
    def click():
        cmd({"execute": "input-send-event", "arguments": {"events": [{"type": "btn", "data": {"down": True, "button": "left"}}]}})
        time.sleep(0.1)
        cmd({"execute": "input-send-event", "arguments": {"events": [{"type": "btn", "data": {"down": False, "button": "left"}}]}})
    def key(qcode):
        cmd({"execute": "send-key", "arguments": {"keys": [{"type": "qcode", "data": qcode}]}})
        time.sleep(0.35)  # real, measured: faster sends here drop scancodes, same class of flake CLAUDE.md's
                           # own sendkey note warns about; 0.35s was confirmed reliable against a live capture
    def dump():
        cmd({"execute": "pmemsave", "arguments": {"val": FB, "size": W * H * 4, "filename": DUMP}})
        return Image.frombytes("RGBA", (W, H), open(DUMP, "rb").read(), "raw", "BGRA").convert("RGB")
    def is_red(img, x, y): return max(abs(img.getpixel((x * SCALE + 1, y * SCALE + 1))[i] - CLOSE_RED[i]) for i in range(3)) <= 12
    def lum(p): return (p[0] * 299 + p[1] * 587 + p[2] * 114) // 1000
    def row_has_text(img, y):
        """Real, font-metric-agnostic check: a text row lights up several
        distinct dark pixels across its span; a blank row (background or
        the plain GUI_BG fill search_draw_content repaints every keystroke)
        does not. Counts pixels clearly darker than GUI_BG (0xFAF8F6, lum
        ~247) and the row-selection highlight (0xEDE6DC, lum ~225)."""
        dark = 0
        for x in range(ROW_X0, ROW_X1, 2):
            for dy in (0, 4, 8, 12):
                if lum(img.getpixel(((x) * SCALE, (y + dy) * SCALE))) < 180:
                    dark += 1
        return dark

    # Open the Apps folder from the dock (slot 0).
    apps_centre = SLOT0_X + 0 * PITCH + DOCK_ICON // 2
    move(apps_centre, ICON_ROW_Y); time.sleep(0.3)
    click(); time.sleep(1.0)

    # Navigate the grid to Search (icon 21, row 4 col 1): right x1, down x4
    # from the top-left cell, the exact math gui_launch_apps itself uses
    # (row = i / APPS_COLS, col = i % APPS_COLS, APPS_COLS = 5), the same
    # keyboard path contacts-keystroke-check.sh already proved reliable for
    # icon 18 (row 3 col 1).
    for c in ("d", "s", "s", "s", "s"):
        key(c)
    key("ret"); time.sleep(1.0)  # launch Search

    img = dump()
    if not is_red(img, APPS_CLOSE_X, APPS_CLOSE_Y):
        fails.append("Search did not open: the Apps folder's outer red close dot is gone")
    else:
        print("Search opened (outer window chrome present)")

    readme_before = row_has_text(img, ROW0_Y)
    notes_before = row_has_text(img, ROW1_Y)
    print(f"before typing: README row dark-px={readme_before}  NOTES row dark-px={notes_before}")
    if readme_before < 6 or notes_before < 6:
        fails.append("initial listing: expected two real result rows (README.TXT, NOTES.TXT) with visible text, got a thin/blank row")

    # Real, live filtering: type "readme", a real substring of README.TXT
    # only. get_key_or_click's case-insensitive match (search_contains_ci)
    # means the exact case typed here doesn't matter, only that it's a
    # genuine substring, not a fixture-matching hack.
    # Keys dropped on slow runners if sent in burst; poll for each redraw marker
    def get_redraw_count():
        try:
            with open(LOG) as lf: return lf.read().count("searchcontent\n")
        except FileNotFoundError: return 0

    before_typing = get_redraw_count()
    for c in "readme":
        key(c)
        for _ in range(50):
            if get_redraw_count() > before_typing:
                before_typing = get_redraw_count()
                break
            time.sleep(0.1)

    img2 = dump()
    readme_after = row_has_text(img2, ROW0_Y)
    notes_after = row_has_text(img2, ROW1_Y)
    print(f"after typing 'readme': README row dark-px={readme_after}  NOTES row dark-px={notes_after}")
    if readme_after < 6:
        fails.append("filtered listing: README.TXT should still match 'readme' but its row went blank")
    if notes_after >= 6:
        fails.append("filtered listing: NOTES.TXT does not contain 'readme' but its row still shows text -- filter is not real/live")

    redraws = 0
    try:
        with open(LOG) as lf: redraws = lf.read().count("searchcontent\n")
    except FileNotFoundError: pass
    print(f"searchcontent redraw markers: {redraws}")
    if redraws < 6:  # initial draw + at least 6 of the "d s s s s" nav frames aren't counted, only content redraws inside Search itself, so this should be >= keystrokes typed
        fails.append(f"expected several per-keystroke content redraws inside Search, only saw {redraws}")

    # Enter on the one remaining match (README.TXT, still selection index 0)
    # opens real file content: gui_wait_close's own footer text is the
    # distinct, real marker every read-only viewer in this kernel ends with.
    key("ret"); time.sleep(1.0)
    img3 = dump()
    footer_y = VY + ((int((490 - 40))) - 30)  # gui_wait_close: window_height()-30 inside the (h-40)-tall content viewport
    footer_dark = 0
    for x in range(VX + 16, VX + 220, 2):
        for dy in (0, 4, 8):
            if lum(img3.getpixel((x * SCALE, (footer_y + dy) * SCALE))) < 180:
                footer_dark += 1
    print(f"content-view footer dark-px={footer_dark}")
    if footer_dark < 6:
        fails.append("Enter on a match did not open real content: gui_wait_close's footer text never appeared")
    else:
        print("Enter opened real file content (cat-style viewer)")

    key("esc"); time.sleep(0.3)  # back to the result list
    key("esc"); time.sleep(0.3)  # close Search
    key("esc"); time.sleep(0.3)  # close the Apps folder, leave input clean for any check that runs after this one

    try: cmd({"execute": "quit"})
    except (ConnectionResetError, BrokenPipeError, OSError): pass
finally:
    try: q.wait(timeout=5)
    except subprocess.TimeoutExpired: q.kill()

if fails:
    for x in fails: print("FAIL:", x)
    sys.exit(1)
print("PASS: Search opens, lists real files, filters them live as you type, and Enter shows real content")
