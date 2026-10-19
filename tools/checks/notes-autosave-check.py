#!/usr/bin/env python3
"""Notes saves itself: a dirty note lands on disk about two seconds after the last key,
so closing the window with the red dot (which kills the program, Esc never runs) keeps it.

Why it exists. The ring-3 Notes (user/notes.c) used to save only on Esc or Ctrl+S. The red
dot is task_kill on the program (kernel/kernel.c, the press_window arm), so edit_close never
ran and a dirty edit was lost on close; a crash mid-edit lost it too. The kernel has no
close hook for ring-3 programs, so the timer is the whole fix.

Proves, against the serial `notes:` markers and a FAT image read back with mtools (never the
guest's own idea of what it saved):

  1. No save per keystroke: a line typed with 0.2 s between keys produces next to no
     `notes: saved=` lines while the keys are still coming (a save per key would be 17), and
     none in the first 0.95 s after the last one. The RTC counts whole seconds and the app
     waits for two of them to tick over, so a save can never land sooner than one real
     second after a key.
  2. The save does land by itself, with the right length, within five seconds of the last
     key, and the editor repaints its title (the dirty star clears) when it does.
  3. The red dot closes the window the kill way: the reaper line is on serial, the
     program never printed `notes: closed` (that is Esc's exit), and no second save ran.
  4. The text is on the disk image, byte for byte, in the note file.
  5. Reopening Notes from the dock and opening the note puts the text back on screen.

  6. Mutation: a private copy of the tree with the timer turned off (AUTOSAVE_SECS made huge, so a dirty
     note never saves itself) is built and this same check is run against it. It must FAIL on the missing
     autosave. A check that also passes with autosave off proves nothing. Your tree is never touched.

Usage: python3 tools/checks/notes-autosave-check.py   (repo root, after make kernel.elf)
       python3 tools/checks/notes-autosave-check.py --no-mutant   (skip the mutation, used by the mutation run itself)
"""
import os, re, shutil, subprocess, sys, tempfile, time
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from jtvm import VM, make_disk, disk_files, SCALE

TEXT = 'saved by the timer'
CLOSE_X, CLOSE_Y = 94, 56            # the dock-opened window's red dot, logical px (appclose-check.py)
CLOSE_RED = (0xFF, 0x5F, 0x57)
DOCK_NOTES_X, DOCK_ROW_Y = 247 + 4 * 43 + 18, 487   # dock slot 4 (appclose-check.py: SLOT0_X, pitch 43, icon row)
VIEW_X, VIEW_Y, TEXT_X, TEXT_Y = 78, 72, 56, 34     # the window's viewport, and where line one's glyphs start
work = Path(tempfile.mkdtemp(prefix='jt-notesautosave-'))
disk = work / 'disk.img'
make_disk(disk)
fails = []


def fail(msg):
    fails.append(msg)
    print('FAIL: ' + msg)


def pixel(vm, x, y):
    return vm.frame().getpixel((x * SCALE + 1, y * SCALE + 1))[:3]


def red_dot(vm):
    p = pixel(vm, CLOSE_X, CLOSE_Y)
    return max(abs(p[i] - CLOSE_RED[i]) for i in range(3)) <= 12


def text_ink(vm):
    """Neutral dark pixels where line one's glyphs sit: the page's ink, not the reddish caret."""
    img = vm.frame().convert('RGB')
    x0, y0 = (VIEW_X + TEXT_X - 4) * SCALE, (VIEW_Y + TEXT_Y - 4) * SCALE
    n = 0
    for y in range(y0, y0 + 26 * SCALE):
        for x in range(x0, x0 + 190 * SCALE):
            r, g, b = img.getpixel((x, y))
            if r < 0x80 and abs(r - g) < 10 and abs(g - b) < 10:
                n += 1
    return n


def saved_lengths(vm):
    return [int(n) for n in re.findall(r'notes: saved=(\d+)', vm.serial())]


# ---- boot 1: type, wait for the timer, close with the red dot ----
vm = VM(disk, 'open=notes', 'notes: folders=', work=work)
try:
    time.sleep(1.5)
    n0 = len(fails)
    before = 0
    vm.key('n')
    if not vm.wait('notes: edit=N0000001.TXT', 8):
        fail('n did not open a new note in the editor')
    time.sleep(1.0)
    chrome0 = vm.count('editorchrome')
    vm.type(TEXT[:-1])
    # A save per key would be 17 here. A little slack: a stalled driver on a loaded runner can let one
    # timer save land between two keys, and that must not read as a failure of the timing proof below.
    before = len(saved_lengths(vm))
    if before > len(TEXT) // 4:
        fail(f'{before} saves landed while the keys were still coming: Notes must not save per keystroke')
    t_last = time.time()
    vm.key(TEXT[-1], gap=0)
    first = None
    while time.time() - t_last < 6:
        if len(saved_lengths(vm)) > before:
            first = time.time() - t_last
            break
        time.sleep(0.05)
    if first is None:
        fail('no `notes: saved=` within six seconds of the last key: the dirty note never saved itself')
    else:
        if first < 0.95:
            fail(f'the save landed {first:.2f}s after the last key, sooner than one real second: it is saving on keystrokes')
        time.sleep(0.5)
        lens = saved_lengths(vm)
        if lens[-1] != len(TEXT) or len(lens) != before + 1:
            fail(f'saved lengths {lens}, expected one new save of {len(TEXT)} bytes after the last key')
        if vm.count('editorchrome') <= chrome0 + 1:
            fail('the title did not repaint after the save, so the dirty star would stay on')
        if len(fails) == n0:
            print(f'PASS: no save per keystroke, none in the first {first:.2f}s after the last key; one {len(TEXT)}-byte save landed by itself')
    time.sleep(max(0.0, 3.0 - (time.time() - t_last)))   # about three seconds since the last key, then the red dot
    n0 = len(fails)
    if not red_dot(vm):
        fail('the window\'s red dot is not on screen before the close click')
    vm.move(CLOSE_X, CLOSE_Y)
    vm.click()
    reaped = vm.wait('window torn down', 8)
    if not reaped:
        fail('the red dot did not close the Notes window (no reaper line on serial)')
    if vm.count('notes: closed'):
        fail('Notes printed `notes: closed`: that is the Esc exit, the red dot must be the kill path')
    time.sleep(1.0)
    if len(saved_lengths(vm)) != before + 1:
        fail(f'{len(saved_lengths(vm))} saves in all, the close itself must not add one')
    if len(fails) == n0:
        print('PASS: the red dot killed the program (reaper line, no `notes: closed`, no extra save)')

    # ---- reopen from the dock, open the note, find the text on screen ----
    n0 = len(fails)
    vm.move(DOCK_NOTES_X, DOCK_ROW_Y)
    time.sleep(0.3)
    vm.click()
    if not vm.wait('notes: folders=01 notes=01', 10, 1) or vm.count('notes: ring-3 window') < 2:
        fail('reopening Notes from the dock did not list the one saved note')
    else:
        time.sleep(1.0)
        vm.key('ret')
        if not vm.wait('notes: edit=N0000001.TXT', 8, 2):
            fail('Enter on the saved note did not open it in the editor')
        time.sleep(1.0)
        ink = text_ink(vm)
        if ink < 60:
            fail(f'the reopened note shows {ink} ink pixels on line one, the text is not there')
        elif len(fails) == n0:
            print(f'PASS: reopened Notes and the note shows its text ({ink} ink pixels on line one)')
finally:
    vm.quit()

# ---- the disk image, read back by the host ----
files = disk_files(disk)
mine = [d for p, d in files.items() if p.split('/')[-1] == 'N0000001.TXT']
if mine != [TEXT.encode()]:
    fail(f'the note on disk is {mine}, expected {TEXT.encode()!r}: the autosave never reached the image')
else:
    print('PASS: the note file on the image holds the text byte for byte after a red-dot close')

# ---- mutation: autosave off must fail this very check ----
if '--no-mutant' not in sys.argv and not fails:
    root = Path(__file__).resolve().parent.parent.parent
    copy = work / 'mutant-tree'
    subprocess.run(['rsync', '-a', '--exclude', '.git', '--exclude', 'landing', '--exclude', 'docs', f'{root}/', f'{copy}/'], check=True)
    (copy / 'landing' / 'v86').mkdir(parents=True, exist_ok=True)   # the Makefile copies kernel.elf there
    src = copy / 'user' / 'notes.c'
    text = src.read_text()
    old = '#define AUTOSAVE_SECS 2 '
    if old not in text:
        fail('mutation target `#define AUTOSAVE_SECS 2` is gone from user/notes.c, update the mutation')
    else:
        src.write_text(text.replace(old, '#define AUTOSAVE_SECS 100000 ', 1))
        built = subprocess.run(['make', '-C', str(copy), 'kernel.elf', '-j4'], capture_output=True, text=True)
        if built.returncode != 0:
            fail('the mutant kernel did not build:\n' + built.stderr[-600:])
        else:
            run = subprocess.run([sys.executable, str(Path(__file__).resolve()), '--no-mutant'], capture_output=True, text=True,
                                 env=dict(os.environ, JT_KERNEL=str(copy / 'kernel.elf'), TMPDIR=str(work)))
            if run.returncode == 0:
                fail('MUTATION SURVIVED: with autosave off this check still passed, so it proves nothing')
            elif 'no `notes: saved=` within six seconds' not in run.stdout:
                fail('the mutant failed, but not on the missing autosave:\n' + run.stdout[-800:])
            else:
                print('PASS: mutation (autosave off) is caught: no save within six seconds of the last key')
    shutil.rmtree(copy, ignore_errors=True)

if fails:
    print(f'\n{len(fails)} FAILURE(S)')
    sys.exit(1)
print('PASS: Notes autosave: no save per keystroke, one by itself after the last key, survives a red-dot close')
