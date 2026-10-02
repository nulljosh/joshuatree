#!/usr/bin/env python3
"""Headless proof of the system-wide clipboard (SYS_CLIPBOARD, 399).

Ring-3 Notes and Terminal call jt_clip_set on Ctrl+C/X and jt_clip_get on
Ctrl+V, so the one kernel buffer is what crosses from one app (one task) to
the other. Ctrl+C copies the current line (Notes) or the input line
(Terminal), Ctrl+X cuts it, Ctrl+V pastes at the cursor within the field's
own room, never past it.

One QEMU boot, headless, through tools/checks/jtvm.py: `open=notes` opens
Notes the moment the desktop is up (no dock coordinates for it), and the
Terminal is opened by its dock tile in the same boot, because the clipboard
lives in the kernel and dies with the boot. The `cliptrace` flag makes the
kernel add an FNV-1a hash to its markers, so every proof is a discriminating
serial line (CLIPCOPY:<len>:<hash> / CLIPPASTE:<len>:<hash> / CLIPTRUNC, never
the text):

  1. Notes: type a line, Ctrl+C, Ctrl+V pastes it back.
  2. Notes: type a line, Ctrl+X, then Terminal's Ctrl+V gets that same line
     (the buffer crosses apps) and Enter runs it.
  3. Notes: a line longer than Terminal's input room, Ctrl+C; Terminal's
     Ctrl+V gets exactly LINE_MAX-1 bytes and CLIPTRUNC follows.

Usage: python3 tools/checks/clipboard-check.py   (repo root, after make kernel.elf)
"""
import sys, time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from jtvm import VM

TERM_X, DOCK_Y = 523, 487   # Terminal's dock tile (slot 6: x0 247 + 6 * 43 + 18)
TERM_ROOM = 95              # user/terminal.c LINE_MAX 95: the input holds 95 chars


def h(t):  # matches clip_log: "<len>:<fnv1a32 hex>"
    v = 2166136261
    for c in t.encode():
        v = ((v ^ c) * 16777619) & 0xFFFFFFFF
    return f'{len(t)}:{v:08x}'


fails = []
vm = VM(None, 'cliptrace open=notes', 'notes: ring-3 window')
try:
    time.sleep(1.5)
    vm.key('n')
    if not vm.wait('notes: edit=', 10):
        fails.append('Notes: n did not open a note in the editor')
    time.sleep(1.0)

    # 1: copy then paste inside Notes
    vm.type('clip-roundtrip', gap=0.1)
    vm.key('ctrl-c')
    if not vm.wait('CLIPCOPY:' + h('clip-roundtrip'), 20):
        fails.append('Notes Ctrl+C: no CLIPCOPY marker for the typed line')
    vm.key('ctrl-v')
    if not vm.wait('CLIPPASTE:' + h('clip-roundtrip'), 20):
        fails.append('Notes Ctrl+V: no CLIPPASTE marker with the copied line')
    else:
        print('Notes    : typed a line, Ctrl+C copied it, Ctrl+V pasted it back (serial-verified)')

    # 2: cut in Notes, paste into Terminal
    vm.key('ret')
    vm.type('echo cross-app-clip', gap=0.1)
    vm.key('ctrl-x')
    if not vm.wait('CLIPCOPY:' + h('echo cross-app-clip'), 20):
        fails.append('Notes Ctrl+X: no CLIPCOPY marker for the cut line')
    vm.move(TERM_X, DOCK_Y)
    time.sleep(0.4)
    vm.click()
    if not vm.wait('terminal: ring-3 window', 15):
        fails.append('Terminal: its dock tile did not open the window')
    time.sleep(1.2)
    vm.key('ctrl-v')
    if not vm.wait('CLIPPASTE:' + h('echo cross-app-clip'), 20):
        fails.append('Terminal Ctrl+V: no CLIPPASTE marker with the line cut in Notes')
    else:
        print('Terminal : cut a line in Notes, pasted it into Terminal\'s input line (serial-verified)')
        vm.key('ret')

    # 3: an over-long clipboard truncates at the Terminal's own room.
    # Notes is behind the Terminal now; its dock tile (slot 4) brings it forward.
    long_text = 'x' * (TERM_ROOM + 21)
    vm.move(247 + 4 * 43 + 18, DOCK_Y)
    time.sleep(0.4)
    vm.click()
    time.sleep(1.2)
    vm.key('ret')
    vm.type(long_text, gap=0.05)
    vm.key('ctrl-c')
    if not vm.wait('CLIPCOPY:' + h(long_text), 60):
        fails.append('Notes Ctrl+C: no CLIPCOPY marker for the long line')
    vm.move(TERM_X, DOCK_Y)
    time.sleep(0.4)
    vm.click()
    time.sleep(1.2)
    vm.key('ctrl-v')
    want = 'CLIPPASTE:' + h('x' * TERM_ROOM)
    if not vm.wait(want, 20):
        fails.append(f'Terminal Ctrl+V: no clean {TERM_ROOM}-byte truncated paste ({want})')
    else:
        time.sleep(0.4)
        log = vm.serial()
        tail = log.split(want, 1)[1][:40]
        if 'CLIPTRUNC' not in tail:
            fails.append('Terminal Ctrl+V: CLIPTRUNC missing right after the clipped paste')
        elif 'CLIPPASTE:' + h(long_text) in log:
            fails.append('Terminal Ctrl+V: the whole long line was pasted, nothing was truncated')
        else:
            print(f'Terminal : pasted a {len(long_text)}-byte clipboard, truncated cleanly to {TERM_ROOM} bytes (serial-verified)')
finally:
    tail = vm.serial()[-500:]
    vm.quit()

if fails:
    print('FAIL:')
    for m in fails:
        print('  - ' + m)
    print('serial tail: ' + repr(tail))
    sys.exit(1)
print('PASS: clipboard copy/cut/paste round-trips real text within Notes, across Notes -> Terminal, and truncates a too-long paste cleanly at Terminal\'s own input limit')
