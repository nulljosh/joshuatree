#!/usr/bin/env python3
"""Apps read their whole data file, not just the first 255 bytes (2.6.32).

SYS_READ moves at most 255 bytes a call, so one jt_read into a big buffer
silently dropped the rest of the file. Stocks had it (fixed in 2.6.31). Mail
and Weather had it too. This boots headless with a FAT16 image holding a file
well past 255 bytes for each app and asserts the app saw the text past byte 255:

  Mail     MAIL.TXT: a 400 byte first message, then two more. The app's own
           marker "mail: n=3" only shows if the parse got past byte 255.
  Notes    NOTES.TXT (migrated to a note): 700 bytes. Ctrl+A in the editor
           selects all and the app prints edsel=0,700, the length it loaded.
           Notes already loops, so this one passes before and after the fix;
           it keeps the loop honest.
  Weather  WEATHER.TXT with a long padding line before the readings: the app
           prints wxshow=33 only if it read the temp line past byte 255.

Weather note: the kernel rewrites WEATHER.TXT on its first fetch. If that wins
the race against the app's first read, the step is skipped and says so.

Discriminating: put mail.c back to one jt_read and the Mail step fails; put
weather.c back and the Weather step fails.

Usage: tools/checks/read-long-files-check.py   (from the repo root, after make kernel.elf)
"""
import subprocess, sys, tempfile
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from jtvm import VM, make_disk

work = Path(tempfile.mkdtemp(prefix='jt-readlong-'))
fails = []


def fail(msg):
    fails.append(msg)
    print('FAIL: ' + msg)


def disk_with(name, data):
    disk = work / (name.replace('.', '_') + '.img')
    make_disk(disk)
    src = work / ('src-' + name)
    src.write_bytes(data)
    subprocess.run(['mcopy', '-i', str(disk), str(src), '::' + name], check=True)
    return disk


def boot(disk, flag, ready, steps, extra=()):
    vm = VM(disk, flag, ready, extra=extra, work=work)
    try:
        steps(vm)
        if 'exception: ring-0' in vm.serial() or 'panic in' in vm.serial():
            fail(flag + ': the kernel faulted')
    finally:
        vm.quit()


# ---- Mail: three messages, the first one longer than a whole read ----
mail = ('r|Ann|First|' + 'long body ' * 40 + '\n'
        'r|Bob|Second|short\n'
        'u|Cat|Third|also short\n')
assert len(mail.split('\n')[0]) > 400
disk = disk_with('MAIL.TXT', mail.encode())
def mail_steps(vm):
    if not vm.wait('mail: n=3', 15):
        got = [l for l in vm.serial().splitlines() if 'mail: n=' in l]
        fail(f'Mail lost the messages past byte 255 of MAIL.TXT (wanted mail: n=3, saw {got})')
boot(disk, 'open=mail', 'mail: n=', mail_steps)

# ---- Notes: a 700 byte note, select all, the editor knows its length ----
note = ''.join(f'line {i:02d} of a long note\n' for i in range(32))[:700]
assert len(note) == 700
disk = disk_with('NOTES.TXT', note.encode())
def notes_steps(vm):
    vm.wait('notes: folders=01 notes=01', 8)
    vm.key('ret')
    vm.wait('notes: edit=', 8)
    vm.key('ctrl-a')
    if not vm.wait('edsel=0,700', 8):
        got = [l for l in vm.serial().splitlines() if 'edsel=' in l]
        fail(f'Notes did not load all 700 bytes of the note (saw {got})')
boot(disk, 'open=notes', 'notes: folders=', notes_steps)

# ---- Weather: the readings sit after a long line, past byte 255 ----
wx = ('state ok\nerr \ncity Langley\nword Clear\n'
      + 'pad ' + 'x' * 300 + '\n'
      + 'have 1\ntemp 33\ncode 0\nextra 0\nfeels 30\nhum 40\nwind 5\n'
      + 'd 1 0 30 20\nd 2 0 31 21\nd 3 0 32 22\nd 4 0 33 23\nd 5 0 34 24\n')
disk = disk_with('WEATHER.TXT', wx.encode())
def wx_steps(vm):
    vm.wait('wxshow=', 15)
    s = vm.serial()
    if 'wxshow=33 ' not in s:
        got = [l for l in s.splitlines() if 'wxshow=' in l or 'wxstate=' in l]
        fail(f'Weather did not show the temp that sits past byte 255 of WEATHER.TXT (saw {got})')
boot(disk, 'open=weat', 'wxwin=', wx_steps, extra=('-nic', 'none'))

if fails:
    sys.exit(1)
print('PASS: Mail, Notes and Weather each read past byte 255 of their data file')
