#!/usr/bin/env python3
"""Books, the ring-3 plain-text reader (user/books.c), on a real FAT image.

Proves, read back with mtools (never the guest's own view of the disk):

  1. The library lists the .TXT files in BOOKS/, and a book over 3 MB is refused
     with a message (serial: books: too big BIG.TXT) and stays closed.
  2. Right turns the page twice (serial: books: page 2, books: page 3), and every
     page turn saves the page number to BOOKS/TEST.POS as one decimal number.
  3. Esc goes back to the library, Esc closes the app (books: closed).
  4. A fresh boot on the same disk opens TEST.TXT on the saved page
     (serial: books: open TEST.TXT page 3), not on page 1.

Usage: python3 tools/checks/books-check.py   (repo root, after make kernel.elf)
"""
import subprocess, sys, tempfile, time
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from jtvm import VM, make_disk, disk_files

work = Path(tempfile.mkdtemp(prefix='jt-books-'))
disk = work / 'disk.img'
make_disk(disk)
fails = []


def fail(msg):
    fails.append(msg)
    print('FAIL: ' + msg)


# A 40 KB book of numbered paragraphs: many pages at any window size.
text = ''.join('Paragraph %04d. The quick brown fox runs over the lazy dog while the wind keeps on blowing.\n\n' % i
               for i in range(1, 500))
(work / 'test.txt').write_text(text)
(work / 'big.txt').write_text('abcdefghi jklmnop ' * (3300000 // 18))   # over the 3 MB line
subprocess.run(['mmd', '-i', str(disk), '::BOOKS'], check=True)
subprocess.run(['mcopy', '-i', str(disk), str(work / 'test.txt'), '::BOOKS/TEST.TXT'], check=True)
subprocess.run(['mcopy', '-i', str(disk), str(work / 'big.txt'), '::BOOKS/BIG.TXT'], check=True)

# ---- boot 1: refuse the big book, turn two pages, save, close ----
vm = VM(disk, 'open=books', 'books: books=2', work=work)
try:
    time.sleep(1.0)
    vm.key('down')
    vm.key('ret')
    if not vm.wait('books: too big BIG.TXT', 8):
        fail('a book over 3 MB was not refused with "books: too big BIG.TXT"')
    vm.key('up')
    vm.key('ret')
    if not vm.wait('books: open TEST.TXT page 1', 8):
        fail('Enter on TEST.TXT did not open it on page 1')
    time.sleep(0.5)
    vm.key('right')
    if not vm.wait('books: page 2', 8):
        fail('Right did not turn to page 2')
    vm.key('right')
    if not vm.wait('books: page 3', 8):
        fail('Right did not turn to page 3')
    time.sleep(0.5)
    vm.key('esc')
    if not vm.wait('books: back', 5):
        fail('Esc did not go back to the library')
    vm.key('esc')
    if not vm.wait('books: closed', 5):
        fail('Esc in the library did not close Books')
finally:
    vm.quit()
pos = disk_files(disk).get('BOOKS/TEST.POS')
if pos != b'3':
    fail(f'BOOKS/TEST.POS on disk is {pos!r}, expected b"3" (the page on screen when Books closed)')
else:
    print('PASS: a too-big book is refused, two page turns landed, and TEST.POS holds page 3')

# ---- boot 2: the same disk, the saved page comes back ----
vm = VM(disk, 'open=books', 'books: books=2', work=work)
try:
    time.sleep(1.0)
    vm.key('ret')
    if not vm.wait('books: open TEST.TXT page 3', 8):
        fail('after a reboot TEST.TXT did not reopen on the saved page 3')
    else:
        print('PASS: after a reboot TEST.TXT reopens on page 3, the saved place')
    time.sleep(0.5)
finally:
    vm.quit()

if fails:
    print(f'\n{len(fails)} FAILURE(S)')
    sys.exit(1)
print('PASS: Books: library, big-book refusal, page turns, saved place and reopen all hold')
