#!/usr/bin/env python3
"""Notes folders and multiple notes, against the ring-3 Notes (user/notes.c).

Proves, on a real FAT image read back with mtools (never the guest's own idea
of what it saved):

  1. Migration: a NOTES.TXT already on the disk (the old always-open single
     note) becomes the first note in the default folder with its content
     intact, edited in place, and NOTES.TXT itself is gone from the root.
  2. A new note ('n' in the browser) lands in the current folder, next to the
     migrated one, as a real file.
  3. Both survive a reboot: a fresh boot on the same image lists both.
  4. Delete asks first: 'd' alone only asks and Esc cancels (the file is
     still on disk); 'd' then 'd' really removes it, and only it.

Usage: python3 tools/checks/notesfolders-check.py   (repo root, after make kernel.elf)
"""
import subprocess, sys, tempfile, time
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from jtvm import VM, make_disk, disk_files

work = Path(tempfile.mkdtemp(prefix='jt-notesfolders-'))
disk = work / 'disk.img'
make_disk(disk)
LEGACY = 'Migrated note\n\nThis one predates folders.\n'
APPEND = '\nplus edited'
SECOND = 'second note content'
(work / 'legacy.txt').write_text(LEGACY)
subprocess.run(['mcopy', '-i', str(disk), str(work / 'legacy.txt'), '::NOTES.TXT'], check=True)
fails = []


def fail(msg):
    fails.append(msg)
    print('FAIL: ' + msg)


def notes_on_disk():
    """{(folder, file): bytes} for every N*.TXT, plus whether NOTES.TXT is still at the root."""
    files = disk_files(disk)
    found = {tuple(p.split('/')[-2:]) if '/' in p else ('', p): d for p, d in files.items()
             if p.split('/')[-1].startswith('N') and p.endswith('.TXT') and p.split('/')[-1] != 'NOTES.TXT'}
    return found, 'NOTES.TXT' in files


# ---- boot 1: migration, then a new note in the same folder ----
vm = VM(disk, 'open=notes', 'notes: folders=', work=work)
try:
    time.sleep(1.5)
    if not vm.wait('notes: folders=01 notes=01', 5):
        fail('the legacy NOTES.TXT was not migrated into the default folder as its one note')
    vm.key('ret')
    vm.wait('notes: edit=N0000001.TXT', 8)
    time.sleep(1.0)
    vm.type(APPEND)
    vm.key('ctrl-s')
    vm.wait('notes: saved=', 8)
    vm.key('esc')
    time.sleep(0.5)
    vm.key('n')
    if not vm.wait('notes: new=N0000002.TXT', 8):
        fail('n did not make N0000002.TXT in the open folder')
    time.sleep(1.0)
    vm.type(SECOND)
    vm.key('ctrl-s')
    vm.wait('notes: saved=', 8, 2)
    vm.key('esc'); vm.key('esc')
    time.sleep(1.0)
finally:
    vm.quit()
found, legacy_left = notes_on_disk()
first = [d for (folder, f), d in found.items() if f == 'N0000001.TXT']
second = [d for (folder, f), d in found.items() if f == 'N0000002.TXT']
folders = {folder for (folder, f) in found}
if legacy_left:
    fail('NOTES.TXT is still at the disk root after migration, it should have moved into the notes folder')
if first != [(LEGACY + APPEND).encode()]:
    fail(f'migrated note on disk is {first}, expected the legacy text plus the edit, byte for byte')
if second != [SECOND.encode()]:
    fail(f'new note on disk is {second}, expected {SECOND!r}: it did not land as a real file')
if len(folders) != 1:
    fail(f'the two notes are in different folders {folders}: a new note must land in the current folder')
if not fails:
    print('PASS: migration kept the legacy note intact, and a new note lands in the same folder as a real file')

# ---- boot 2: reboot persistence, then delete asks first ----
vm = VM(disk, 'open=notes', 'notes: folders=', work=work)
try:
    time.sleep(1.5)
    if not vm.wait('notes: folders=01 notes=02', 5):
        fail('a fresh boot did not list both notes')
    else:
        print('PASS: both notes survive a reboot, same folder')
    vm.key('d')
    if not vm.wait('notes: delete asks', 5):
        fail('d deleted straight away: delete must ask first')
    vm.key('esc')
    if not vm.wait('notes: delete cancelled', 5):
        fail('Esc at the delete prompt did not cancel it')
    time.sleep(0.5)
    if vm.count('notes: deleted=') != 0:
        fail('Esc at the delete prompt still deleted the note')
    vm.key('d'); vm.wait('notes: delete asks', 5, 2)
    vm.key('d')
    if not vm.wait('notes: deleted=', 5):
        fail('a second d did not delete the note')
    time.sleep(1.0)
finally:
    vm.quit()
found, _ = notes_on_disk()
names = sorted(f for (folder, f) in found)
if len(names) != 1:
    fail(f'after one confirmed delete the folder holds {names}, expected exactly one note left')
else:
    print('PASS: d asks first, Esc cancels it, d then d removes exactly one note')

if fails:
    print(f'\n{len(fails)} FAILURE(S)')
    sys.exit(1)
print('PASS: Notes folders: migration intact, new notes land in the current folder, survive reboot, delete asks first')
