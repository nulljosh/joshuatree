#!/usr/bin/env python3
"""Notes typing, typography, pointer controls, persistence, against the ring-3 Notes.

Notes is user/notes.c now: a compositor window drawing antialiased libjt text,
with a folder/notes browser and a word-wrapping editor. Nothing about it is in
kernel memory any more, so every claim here is read from what a person could
see or keep: the app's own serial markers, the framebuffer, and the bytes
that land on the FAT image (read back with mtools, never from the guest).

  1. typing: n makes a note, Shift, punctuation, Enter, Backspace, Left, Delete,
     Home, End and Caps Lock build one exact string; Ctrl+S saves it and the
     note file on disk holds exactly those bytes.
  2. typography: the page is the cream sheet, the ink is antialiased (real
     intermediate coverage, not a 1-bit face), and the 2 px caret is drawn.
  3. persistence: a fresh boot on the same disk reopens the note with its text
     (the saved byte count proves what was read back), appends, saves again.
  4. scrolling: 24 blank lines push the caret past the window; it stays on
     screen because the view follows it.
  5. pointer controls: a click on a row in the notes list selects it, and
     Enter opens exactly that note.
  6. no disk: on a ramfs boot a save is read back by a second edit session.

Usage: python3 tools/checks/editor_qa.py   (repo root, after make kernel.elf)
"""
import sys, tempfile, time
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from jtvm import VM, make_disk, disk_files, SCALE

VIEW_X, VIEW_Y, VIEW_W, VIEW_H = 78, 72, 804, 345   # gui_launch_from_dock: viewport at (x+8, y+32)
PAGE = (0xFA, 0xF8, 0xF6)
CARET = (0x85, 0x14, 0x4B)
ROW0_Y = 40 + 24 + 22 - 4                            # user/notes.c click(): first list row
fails = []


def fail(msg):
    fails.append(msg)
    print('FAIL: ' + msg)


def near(p, c, tol=10):
    return max(abs(p[i] - c[i]) for i in range(3)) <= tol


def view(frame):
    return frame.crop((VIEW_X * SCALE, VIEW_Y * SCALE, (VIEW_X + VIEW_W) * SCALE, (VIEW_Y + VIEW_H) * SCALE))


def stats(img):
    """(ink, antialiased, caret) pixel counts in a viewport crop."""
    ink = aa = caret = 0
    for r, g, b in img.get_flattened_data() if hasattr(img, 'get_flattened_data') else img.getdata():
        if near((r, g, b), CARET, 6):
            caret += 1
        elif max(r, g, b) < 0x50:
            ink += 1
        elif min(r, g, b) < 0xE0 and not near((r, g, b), PAGE, 10):
            aa += 1
    return ink, aa, caret


def saved_bytes(vm, n=1):
    """The Nth 'notes: saved=N' value the app has logged so far."""
    vals = [int(l.split('saved=')[1].split()[0]) for l in vm.serial().splitlines() if 'notes: saved=' in l]
    return vals[n - 1] if len(vals) >= n else None


def note_on_disk(disk, name):
    for path, data in disk_files(disk).items():
        if path.split('/')[-1] == name:
            return data
    return None


work = Path(tempfile.mkdtemp(prefix='jt-editor-qa-'))
disk = work / 'disk.img'
make_disk(disk)

first = 'Hello, Joshua Tree!\nReally Beautiful type!\nQA'

# ---- boot 1: typing, typography, save ----
vm = VM(disk, 'open=notes', 'notes: folders=', work=work)
try:
    page = vm.frame().getpixel(((VIEW_X + 400) * SCALE, (VIEW_Y + 300) * SCALE))
    if not near(page, PAGE):
        fail(f'the Notes page is not the cream sheet {PAGE} (got {page})')
    time.sleep(1.5)   # a key sent the instant the window is up can land before the app polls
    vm.key('n')
    if not (vm.wait('notes: new=N0000001.TXT', 8) and vm.wait('notes: edit=N0000001.TXT', 8)):
        fail('n did not make a new note and open it in the editor')
    time.sleep(1.0)
    vm.type('Hello, Joshua Tree!\nBeautiful type.\n')
    vm.key('backspace'); vm.key('left'); vm.key('delete'); vm.type('!')
    vm.key('home'); vm.type('Really ')
    vm.key('end'); vm.type('\n')
    vm.key('caps_lock'); vm.type('qa'); vm.key('caps_lock')
    shot = view(vm.frame())
    ink, aa, caret = stats(shot)
    print(f'typed page: ink {ink}, antialiased {aa}, caret {caret}')
    if ink < 300:
        fail(f'the typed text is not on the page (only {ink} ink pixels)')
    if aa < 150:
        fail(f'the text is not antialiased: only {aa} intermediate-coverage pixels (a 1-bit face has none)')
    if caret < 20:
        fail(f'the 2 px caret ({CARET}) is not drawn ({caret} pixels)')
    vm.key('ctrl-s')
    if not vm.wait('notes: saved=', 8):
        fail('Ctrl+S did not save')
    elif saved_bytes(vm) != len(first):
        fail(f'saved {saved_bytes(vm)} bytes, expected {len(first)} for {first!r}: a keystroke was lost or mangled')
    vm.key('esc'); vm.key('esc')
    vm.wait('notes: closed', 5)
    time.sleep(1.0)
finally:
    vm.quit()
data = note_on_disk(disk, 'N0000001.TXT')
if data != first.encode():
    fail(f'the note on disk is {data!r}, expected {first!r}')
else:
    print('PASS: typing, editing keys and Caps Lock built the exact text; it is on the disk byte for byte')

# ---- boot 2: persistence, scrolling, pointer ----
second = first + '\nSaved twice.'
vm = VM(disk, 'open=notes', 'notes: folders=', work=work)
try:
    if not vm.wait('notes: folders=01 notes=01', 5):
        fail('a fresh boot did not list the saved note')
    time.sleep(1.5)
    vm.key('ret')
    if not vm.wait('notes: edit=N0000001.TXT', 8):
        fail('Enter on the note did not open it')
    time.sleep(1.0)
    vm.type('\nSaved twice.')
    vm.key('ctrl-s')
    vm.wait('notes: saved=', 8)
    if saved_bytes(vm) != len(second):
        fail(f'after reopening and appending, saved {saved_bytes(vm)} bytes, expected {len(second)}: the old text did not come back')
    else:
        print('PASS: a fresh boot reopened the note with its text and a second save kept both')
    for _ in range(24):
        vm.key('ret', 0.12)
    vm.type('Still visible.')
    ink, aa, caret = stats(view(vm.frame()))
    if caret < 20:
        fail('after 24 blank lines the caret left the window: the view did not scroll to follow it')
    else:
        print('PASS: 24 blank lines later the caret is still on screen (the editor scrolled)')
    vm.key('esc')
    # a second note, so the list has two rows and the newest is selected
    vm.key('n')
    vm.wait('notes: edit=N0000002.TXT', 8)
    time.sleep(1.0)
    vm.type('other')
    vm.key('esc')
    vm.wait('notes: saved=', 8, 3)
    n_before = vm.count('notes: edit=N0000001.TXT')
    vm.move(VIEW_X + 200, VIEW_Y + ROW0_Y + 8)
    vm.click()
    vm.key('ret')
    if not vm.wait('notes: edit=N0000001.TXT', 8, n_before + 1):
        fail('clicking the first row and pressing Enter did not open the first note: the pointer did not select it')
    else:
        print('PASS: a click on a notes-list row selects it and Enter opens exactly that note')
    vm.key('esc'); vm.key('esc')
    time.sleep(1.0)
finally:
    vm.quit()

# ---- boot 3: no disk, ramfs keeps a save across edit sessions ----
vm = VM(None, 'open=notes', 'notes: folders=', work=work)
try:
    time.sleep(1.5)
    vm.key('ret')
    if not vm.wait('notes: edit=', 8):
        fail('ramfs boot: Enter did not open the seeded note')
    vm.type('No disk. Keep this text!')
    vm.key('ctrl-s')
    vm.wait('notes: saved=', 8)
    a = saved_bytes(vm)
    vm.key('esc')
    vm.key('ret')
    vm.wait('notes: edit=', 8, 2)
    vm.type('.')
    vm.key('ctrl-s')
    vm.wait('notes: saved=', 8, 2)
    b = saved_bytes(vm, 2)
    if a is None or b != a + 1:
        fail(f'ramfs: the second session saved {b} bytes, expected {a} + 1: the first save was not read back')
    else:
        print('PASS: with no disk the ramfs file keeps the save and the next session reads it back')
finally:
    vm.quit()

if fails:
    print(f'\n{len(fails)} FAILURE(S)')
    sys.exit(1)
print('PASS: Notes typing, antialiased type, caret, scrolling, pointer selection, save and reboot, ramfs reopen')
print(f'Artifacts: {work}')
