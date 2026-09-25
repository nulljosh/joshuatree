#!/usr/bin/env python3
"""Headless check for the Notes editor's glyph blit resolution.

Owner-photographed bug: the Notes body text looked "pixely" -- every glyph
edge stepped in visible 2x2 blocks. Root cause: editor_draw_glyph
(kernel/editor.h) blended each coverage sample and wrote it through
window_pixel in LOGICAL coordinates; window_pixel's own scaled path
(drivers/window.c) then writes that SAME colour to every one of the
scale x scale physical pixels a logical pixel covers -- a nearest-neighbour
blow-up. The GUI's own text (gui_aa_char) already draws antialiased
straight at physical resolution and shows no such blocking; this check
proves Notes now matches it.

What this measures, on a real headless frame: boots kernel.elf with
-display none (the same QMP/serial harness editor_qa.py uses), opens Notes
from the dock, types a real line, pmemsaves the physical framebuffer, and
scans the typed text row for pixels whose right AND lower neighbour are
exactly equal to them -- the hallmark of nearest-neighbour 2x2 block
duplication (a properly interpolated blit essentially never repeats an
exact value into its own neighbours along a glyph edge). The old
(nearest-neighbour) blit scores a high fraction; the fixed, physical-
resolution/interpolated blit scores low.

Usage: python3 tools/checks/notessharp-check.py   (repo root, after make kernel.elf)
"""
import json, re, socket, subprocess, sys, tempfile, time
from pathlib import Path
from PIL import Image

ROOT = Path(__file__).resolve().parent.parent.parent
ARTIFACTS = Path(tempfile.mkdtemp(prefix='jt-notessharp-'))
BLOCK_MAX = 0.35  # fraction of glyph ink pixels that are exact 2x2-duplicated blocks


class Machine:
    def __init__(self):
        self.socket_path = ARTIFACTS / 'qmp.sock'
        arguments = ['qemu-system-i386', '-kernel', str(ROOT / 'kernel.elf'), '-display', 'none', '-vga', 'std',
                     '-qmp', f'unix:{self.socket_path},server=on,wait=off']
        self.process = subprocess.Popen(arguments, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        self.connection = socket.socket(socket.AF_UNIX)
        for _ in range(100):
            try:
                self.connection.connect(str(self.socket_path))
                break
            except (FileNotFoundError, ConnectionRefusedError):
                time.sleep(.05)
        self.connection.settimeout(10)
        self.stream = self.connection.makefile('rwb', buffering=0)
        self.stream.readline()
        self.command('qmp_capabilities')
        for _ in range(100):
            if 'b007c0de' in self.monitor('xp /1xw 0x9000'):
                break
            time.sleep(.1)
        else:
            self.close()
            raise SystemExit('FAIL: boot marker not reached')
        time.sleep(2)

    def command(self, name, arguments=None):
        self.stream.write((json.dumps({'execute': name, 'arguments': arguments or {}}) + '\n').encode())
        while True:
            result = json.loads(self.stream.readline())
            if 'error' in result:
                raise RuntimeError(result)
            if 'return' in result:
                return result['return']

    def monitor(self, command):
        return self.command('human-monitor-command', {'command-line': command})

    def key(self, key):
        self.monitor(f'sendkey {key} 30')
        time.sleep(.065)

    def move(self, x, y):
        self.command('input-send-event', {'events': [
            {'type': 'abs', 'data': {'axis': 'x', 'value': x * 32768 // 960}},
            {'type': 'abs', 'data': {'axis': 'y', 'value': y * 32768 // 540}}]})
        time.sleep(.2)

    def click(self):
        for down in (True, False):
            self.command('input-send-event', {'events': [{'type': 'btn', 'data': {'down': down, 'button': 'left'}}]})
            time.sleep(.15)

    def type(self, text):
        punctuation = {' ': 'spc', '\n': 'ret'}
        for character in text:
            before = self.integer('editor_length')
            self.key(punctuation.get(character, character))
            for _ in range(50):
                if self.integer('editor_length') != before:
                    break
                time.sleep(0.05)

    def memory(self, symbol, count):
        result = self.monitor(f'xp /{count}xb 0x{symbols[symbol]:x}')
        return bytes(int(value, 16) for line in result.splitlines() if ':' in line
                     for value in re.findall(r'0x([0-9a-f]{2})\b', line.split(':', 1)[1]))

    def integer(self, symbol):
        return int.from_bytes(self.memory(symbol, 4), 'little')

    def screenshot(self):
        raw = ARTIFACTS / 'framebuffer.raw'
        self.command('pmemsave', {'val': 0xfd000000, 'size': 1920 * 1080 * 4, 'filename': str(raw)})
        return Image.frombytes('RGB', (1920, 1080), raw.read_bytes(), 'raw', 'BGRX').convert('L')

    def open_notes(self):
        self.move(458, 487)
        self.click()
        for _ in range(50):
            if self.integer('editor_loaded') and self.integer('gui_app_windowed'):
                time.sleep(.5)
                return
            time.sleep(.1)
        raise SystemExit('FAIL: Notes dock click did not launch editor')

    def close(self):
        try:
            self.command('quit')
        except (OSError, ValueError):
            pass
        finally:
            try:
                self.process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait()
            self.stream.close()
            self.connection.close()


import shutil
nm = shutil.which('nm') or 'nm'
symbols = {}
for line in subprocess.check_output([nm, str(ROOT / 'kernel.elf')], text=True).splitlines():
    fields = line.split()
    if len(fields) == 3:
        symbols[fields[2]] = int(fields[0], 16) - 0xC0000000

machine = Machine()
try:
    machine.open_notes()
    # Move the caret to the end of the seeded doc and type a fresh line, so
    # the check exercises the live-typed glyph path Joshua actually
    # photographed, not just the pre-seeded body text.
    machine.key('end')
    machine.type('\nsharpnotes')
    time.sleep(.3)
    img = machine.screenshot()
finally:
    machine.close()

# app_view_x/y put the windowed Notes' text area a known offset from the
# dock-launched window's own origin; editor_qa.py's own toolbar() helper
# establishes the same viewport. The typed line lands on the second text
# row (EDITOR_TEXT_TOP + one line_height), a wide box well clear of the
# caret and any selection chrome.
BOX = (280, 470, 900, 520)
crop = img.crop(BOX)
w, h = crop.size
px = list(crop.getdata())
bg = max(set(px), key=px.count)
fg = min(px)
if bg - fg < 60:
    print(f'FAIL: no typed text found in {BOX} (surface {bg}, darkest {fg})')
    sys.exit(1)

glyph_px = 0
blocked = 0
for y in range(h - 1):
    for x in range(w - 1):
        v = px[y * w + x]
        if abs(bg - v) < 20:
            continue
        glyph_px += 1
        right = px[y * w + x + 1]
        down = px[(y + 1) * w + x]
        diag = px[(y + 1) * w + x + 1]
        if v == right and v == down and v == diag:
            blocked += 1

if glyph_px == 0:
    print(f'FAIL: no ink pixels found in {BOX}')
    sys.exit(1)

frac = blocked / glyph_px
print(f'typed line: {glyph_px} glyph px, {frac:.3f} in duplicated 2x2 blocks (need < {BLOCK_MAX})')
if frac >= BLOCK_MAX:
    print(f'FAIL: Notes glyph edges are nearest-neighbour blocked (pixely) -- {frac:.1%} of ink pixels are duplicated 2x2 blocks')
    sys.exit(1)
print('PASS: Notes glyphs are blitted at physical resolution, no 2x2 block duplication')
