"""Headless sharpness check for Notes' runtime-TTF text at both ends of the
size range: 12pt (the smallest EDITOR_PT_SIZES entry) and 200pt (the
largest, the owner's "scale nicely to 200pt" ask). Types "Ag" at each size,
pmemsaves the physical framebuffer, and proves the rendering is real
antialiased TrueType, not a scaled-up bitmap:
  - real antialiasing: at least a few glyph pixels land at intermediate
    coverage (20..80% ink), the same signal textsharp-check.py uses to
    catch a binary/jagged renderer.
  - no 2x2 duplicated blocks: a naive nearest-neighbour bitmap upscale (the
    bug this guards against) makes every 2x2 pixel block four copies of
    the same value; a real per-pixel rasterization at the target size does
    not. Sampled across the glyph's bounding box.

Usage: python3 tools/checks/notessharp-check.py   (repo root, after make kernel.elf)
"""
import json, re, shutil, socket, subprocess, sys, tempfile, time
from pathlib import Path
from PIL import Image

# editor_qa.py runs its whole check suite at import time (script-style), so
# this doesn't import it -- it re-implements the small slice of its Machine
# class needed here, against the same disk-less boot and dock-click-to-
# open-Notes path.

ROOT = Path(__file__).resolve().parent.parent.parent
ARTIFACTS = Path(tempfile.mkdtemp(prefix='jt-notessharp-'))
symbols = {}
nm = shutil.which('nm') or 'nm'
for line in subprocess.check_output([nm, str(ROOT / 'kernel.elf')], text=True).splitlines():
    fields = line.split()
    if len(fields) == 3:
        symbols[fields[2]] = int(fields[0], 16) - 0xC0000000


class Machine:
    def __init__(self):
        self.socket_path = ARTIFACTS / 'qmp.sock'
        self.socket_path.unlink(missing_ok=True)
        arguments = ['qemu-system-i386', '-kernel', str(ROOT / 'kernel.elf'), '-display', 'none', '-vga', 'std',
                     '-qmp', f'unix:{self.socket_path},server=on,wait=off']
        self.process = subprocess.Popen(arguments, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        self.stream = None
        sock = socket.socket(socket.AF_UNIX)
        for attempt in range(100):
            try:
                sock.connect(str(self.socket_path))
                break
            except (FileNotFoundError, ConnectionRefusedError):
                time.sleep(.05)
        self.stream = sock.makefile('rwb')
        self.stream.readline()
        self.command('qmp_capabilities')
        time.sleep(3.0)  # let the kernel finish boot and register its input devices before the first move()

    def command(self, name, arguments=None):
        self.stream.write((json.dumps({'execute': name, 'arguments': arguments or {}}) + '\n').encode())
        self.stream.flush()
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

    def move(self, target_x, target_y):
        self.command('input-send-event', {'events': [
            {'type': 'abs', 'data': {'axis': 'x', 'value': target_x * 32768 // 960}},
            {'type': 'abs', 'data': {'axis': 'y', 'value': target_y * 32768 // 540}}]})
        time.sleep(.2)

    def click(self):
        for down in (True, False):
            self.command('input-send-event', {'events': [{'type': 'btn', 'data': {'down': down, 'button': 'left'}}]})
            time.sleep(.15)

    def type(self, text):
        punctuation = {' ': 'spc', '\n': 'ret'}
        for character in text:
            before = self.integer('editor_length')
            self.key(punctuation.get(character, 'shift-' + character.lower() if character.isupper() else character))
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
        for attempt in range(50):
            if self.integer('editor_loaded') and self.integer('gui_app_windowed'):
                time.sleep(.5)
                return
            time.sleep(.1)
        raise AssertionError('Notes dock click did not launch editor')

    def close(self):
        try:
            self.command('quit')
        except (OSError, ValueError):
            pass
        try:
            self.process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.process.kill()


def set_size(machine, index):
    """editor_size defaults to 1; F2 advances it by one, mod EDITOR_N_SIZES
    (11), same as the kernel's own key handler. Presses one at a time and
    confirms each landed before the next, self-correcting for any missed
    or double-counted key event instead of trusting a fixed press count."""
    for _ in range(20):
        current = machine.integer('editor_size')
        if current == index:
            return
        machine.key('f2')
        time.sleep(.1)
    raise AssertionError(('editor_size never reached', machine.integer('editor_size'), index))


def assert_sharp(crop, label):
    """The actual sharpness measurement, factored out so other checks (the
    Terminal's mono grid, termsharp-check.py) can reuse it against their own
    QEMU/pmemsave crop instead of re-deriving the antialiasing/duplicated-
    block logic. crop is a PIL 'L' (grayscale) image of just the glyph
    region. Returns nothing; raises AssertionError on failure, same as the
    inline version this replaced."""
    w, h = crop.size
    px = list(crop.getdata())
    bg = max(set(px), key=px.count)
    # Notes is dark ink on a light page (fg = darkest pixel); the Terminal
    # is light text on a dark surface (fg = brightest pixel). Pick
    # whichever extreme sits farther from the background mode so the same
    # measurement works for either polarity.
    fg = min(px) if (bg - min(px)) >= (max(px) - bg) else max(px)
    span = abs(bg - fg)
    assert span >= 60, f'{label}: no text found (surface {bg}, extreme {fg})'
    levels = [abs(bg - p) / span for p in px if abs(bg - p) / span > 0.08]
    mid = sum(0.2 < a < 0.8 for a in levels)
    assert mid >= 8, f'{label}: no intermediate coverage values, edges are not antialiased ({mid} mid-tone px)'
    # No 2x2 duplicated blocks, checked on the EDGE pixels specifically:
    # a solid glyph interior legitimately has same-value neighbours
    # under any renderer, real or upscaled, so that's not a useful
    # signal. A nearest-neighbour bitmap upscale (the bug this guards
    # against) instead stretches its edge pixels too, so an edge block
    # -- one containing an intermediate-coverage pixel -- comes out as
    # four identical values far more often than real per-pixel
    # rasterization, which recomputes coverage at every physical pixel
    # independently.
    edge_xy = {(i % w, i // w) for i, p in enumerate(px) if 0.08 < abs(bg - p) / span < 0.92}
    assert edge_xy, f'{label}: no antialiased edge pixels to check for duplicated blocks'
    uniform, total = 0, 0
    seen = set()
    for (ex, ey) in edge_xy:
        bx, by = ex - ex % 2, ey - ey % 2
        if (bx, by) in seen or bx + 1 >= w or by + 1 >= h:
            continue
        seen.add((bx, by))
        block = [crop.getpixel((bx + dx, by + dy)) for dy in (0, 1) for dx in (0, 1)]
        total += 1
        if len(set(block)) == 1:
            uniform += 1
    duplicated_ratio = uniform / total if total else 0
    assert duplicated_ratio < 0.5, (
        f'{label}: {duplicated_ratio:.0%} of edge-pixel 2x2 blocks are duplicated pixels '
        f'(looks like a nearest-neighbour bitmap upscale, not real rasterization)')
    print(f'PASS: {label}: {mid} mid-tone px, {duplicated_ratio:.0%} duplicated 2x2 blocks (< 50%)')


def check_size(index, label):
    machine = Machine()
    try:
        machine.open_notes()
        set_size(machine, index)
        machine.move(480, 300)
        machine.click()
        machine.type('Ag')
        for _ in range(50):
            if machine.integer('editor_length') == 2:
                break
            time.sleep(0.1)
        time.sleep(.4)
        img = machine.screenshot()
        # The typed text starts at the text area's left/top; a box comfortably
        # larger than any of the 11 point sizes' "Ag" at physical resolution
        # (scale 2) covers it without also picking up chrome.
        origin_x, origin_y = machine.integer('app_view_x'), machine.integer('app_view_y')
        box = ((origin_x + 40) * 2, (origin_y + 60) * 2, (origin_x + 40) * 2 + 620, (origin_y + 60) * 2 + 460)
        crop = img.crop(box)
        assert_sharp(crop, label)
    finally:
        machine.close()


if __name__ == '__main__':
    # Guarded so termsharp-check.py can `from notessharp_check import
    # assert_sharp` (see tools/checks/ci_import.py) without this module's
    # own Notes/QEMU run firing a second time.
    check_size(0, '12pt "Ag"')
    check_size(10, '200pt "Ag"')
    print('PASS: Notes renders real antialiased TrueType at both 12pt and 200pt, no duplicated-block upscaling')
