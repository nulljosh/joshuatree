import json
from pathlib import Path
import re
import shutil
import socket
import struct
import subprocess
import tempfile
import time
from PIL import Image

ROOT = Path(__file__).resolve().parent.parent.parent
ARTIFACTS = Path(tempfile.mkdtemp(prefix='jt-editor-qa-'))
symbols = {}
# v0.76.9: plain `nm` (binutils), not `llvm-nm`. The macOS-homebrew
# fallback here was never actually exercised on Linux CI (this script isn't
# wired into check.yml yet), but walldefault-check.sh hit the identical
# trap for real once it was: `llvm-nm` comes from the separate `llvm` apt
# package, which check.yml's own install line never pulls in, only
# clang/lld. `nm` ships with `binutils`, already present on every Ubuntu
# image and every macOS Xcode CLT install, no extra fallback path needed.
nm = shutil.which('nm') or 'nm'
for line in subprocess.check_output([nm, str(ROOT / 'kernel.elf')], text=True).splitlines():
    fields = line.split()
    if len(fields) == 3:
        symbols[fields[2]] = int(fields[0], 16) - 0xC0000000


def make_disk(path):
    disk = bytearray(16 * 1024 * 1024)
    disk[:3] = b'\xeb\x3c\x90'
    disk[3:11] = b'JT QA   '
    struct.pack_into('<HBHBHHBHHHII', disk, 11, 512, 2, 1, 2, 512, 32768, 248, 64, 32, 64, 0, 0)
    disk[510:512] = b'\x55\xaa'
    for sector in (1, 65):
        disk[sector * 512:sector * 512 + 4] = b'\xf8\xff\xff\xff'
    path.write_bytes(disk)


class Machine:
    def __init__(self, disk=None):
        self.socket_path = ARTIFACTS / 'qmp.sock'
        self.socket_path.unlink(missing_ok=True)
        arguments = ['qemu-system-i386', '-kernel', str(ROOT / 'kernel.elf'), '-display', 'none', '-vga', 'std',
                     '-qmp', f'unix:{self.socket_path},server=on,wait=off']
        if disk:
            arguments += ['-drive', f'file={disk},format=raw,if=ide']
        self.process = subprocess.Popen(arguments, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        self.connection = socket.socket(socket.AF_UNIX)
        for attempt in range(100):
            try:
                self.connection.connect(str(self.socket_path))
                break
            except (FileNotFoundError, ConnectionRefusedError):
                time.sleep(.05)
        self.connection.settimeout(10)
        self.stream = self.connection.makefile('rwb', buffering=0)
        self.stream.readline()
        self.command('qmp_capabilities')
        for attempt in range(100):
            if 'b007c0de' in self.monitor('xp /1xw 0x9000'):
                break
            time.sleep(.1)
        else:
            self.close()
            raise AssertionError('Boot marker not reached')
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

    def move(self, target_x, target_y):
        self.command('input-send-event', {'events': [
            {'type': 'abs', 'data': {'axis': 'x', 'value': target_x * 32768 // 960}},
            {'type': 'abs', 'data': {'axis': 'y', 'value': target_y * 32768 // 540}}]})
        time.sleep(.2)

    def toolbar(self, local_x):
        self.move(self.integer('app_view_x') + local_x, self.integer('app_view_y') + 56)
        self.click()
        time.sleep(.3)

    def click(self):
        for down in (True, False):
            self.command('input-send-event', {'events': [{'type': 'btn', 'data': {'down': down, 'button': 'left'}}]})
            time.sleep(.15)

    def type(self, text):
        punctuation = {' ': 'spc', '\n': 'ret', '\t': 'tab', '.': 'dot', ',': 'comma', '!': 'shift-1', '?': 'shift-slash'}
        for character in text:
            self.key(punctuation.get(character, 'shift-' + character.lower() if character.isupper() else character))

    def memory(self, symbol, count):
        result = self.monitor(f'xp /{count}xb 0x{symbols[symbol]:x}')
        return bytes(int(value, 16) for line in result.splitlines() if ':' in line
                     for value in re.findall(r'0x([0-9a-f]{2})\b', line.split(':', 1)[1]))

    def integer(self, symbol):
        return int.from_bytes(self.memory(symbol, 4), 'little')

    def wait_int(self, symbol, predicate, desc=''):
        # v0.76.32: the same buffer-settle race expect() already fixed
        # (v0.76.26/27) also hits every raw `assert machine.integer(...)`
        # in this file -- 3 separate CI runs failed on 3 different
        # assertions here, each passing locally every time. One shared
        # poll instead of patching each of the 8 call sites separately.
        value = None
        for _ in range(20):
            value = self.integer(symbol)
            if predicate(value):
                return value
            time.sleep(0.1)
        assert predicate(value), (desc or symbol, value)
        return value

    def expect(self, expected):
        # v0.76.27: the buffer read can race the keyboard event that
        # produced `expected` on a slower/CI runner -- proven real by two
        # separate failures on two separate assertions in this same file
        # (a missing trailing '\n', then later a dropped space), both
        # passing locally every time. Same "settle time varies by
        # environment" shape appclose-check.py's own v0.76.18 flake fix
        # already established. Poll for the buffer to match before
        # asserting, instead of patching each of this file's 9 call
        # sites individually -- fix it once, where every caller routes
        # through.
        target_len = len(expected.encode())
        actual = None
        for attempt in range(20):
            length = self.integer('editor_length')
            if length == target_len:
                actual = self.memory('editor_buffer', length).decode()
                if actual == expected:
                    return
            time.sleep(0.1)
        if actual is None:
            length = self.integer('editor_length')
            actual = self.memory('editor_buffer', length).decode()
        assert actual == expected, (actual, expected)

    def saved(self):
        for attempt in range(100):
            if self.integer('editor_dirty') == 0:
                return
            time.sleep(.1)
        self.screenshot('save-timeout')
        raise AssertionError('Save did not complete')

    def screenshot(self, name):
        raw = ARTIFACTS / 'framebuffer.raw'
        self.command('pmemsave', {'val': 0xfd000000, 'size': 1920 * 1080 * 4, 'filename': str(raw)})
        frame = Image.frombytes('RGB', (1920, 1080), raw.read_bytes(), 'raw', 'BGRX')
        frame.save(ARTIFACTS / (name + '.png'))
        return frame

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
        finally:
            try:
                self.process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait()
            self.stream.close()
            self.connection.close()


disk = ARTIFACTS / 'disk.img'
make_disk(disk)
machine = Machine(disk)
try:
    machine.screenshot('boot-desktop')
    machine.open_notes()
    machine.screenshot('dock-open')
    machine.wait_int('editor_loaded', lambda v: v == 1, 'Notes dock click did not launch editor')
    machine.toolbar(112)
    machine.wait_int('editor_family', lambda v: v == 1, 'One font click must advance exactly one family')
    machine.key('f1')
    machine.key('f1')
    machine.toolbar(312)
    machine.wait_int('editor_size', lambda v: v == 2, 'Size toolbar click failed')
    for repeat in range(3):
        machine.key('f2')
    machine.toolbar(542)
    machine.wait_int('editor_weight', lambda v: v == 1, 'Weight toolbar click failed')
    machine.key('f3')
    machine.move(480, 300)
    machine.click()
    machine.type('Hello, Joshua Tree!\nBeautiful type.\n')
    expected = 'Hello, Joshua Tree!\nBeautiful type.\n'
    machine.expect(expected)
    machine.key('backspace')
    machine.key('left')
    machine.key('delete')
    machine.type('!')
    expected = 'Hello, Joshua Tree!\nBeautiful type!'
    machine.expect(expected)
    machine.key('home')
    machine.type('Really ')
    expected = 'Hello, Joshua Tree!\nReally Beautiful type!'
    machine.expect(expected)
    machine.key('ctrl-end')
    machine.type('\n\t')
    machine.key('caps_lock')
    machine.type('qa')
    machine.key('caps_lock')
    expected += '\n\tQA'
    machine.expect(expected)
    rendered_styles = set()
    for family in range(3):
        for size in range(4):
            for weight in range(2):
                machine.wait_int('editor_family', lambda v, family=family: v == family)
                machine.wait_int('editor_size', lambda v, size=size: v == (size + 1) % 4)
                machine.wait_int('editor_weight', lambda v, weight=weight: v == weight)
                frame = machine.screenshot(f'type-{family}-{size}-{weight}')
                origin_x, origin_y = machine.integer('app_view_x'), machine.integer('app_view_y')
                crop = frame.crop(((origin_x + 40) * 2, (origin_y + 92) * 2,
                                   (origin_x + 740) * 2, (origin_y + 210) * 2))
                rendered_styles.add(crop.tobytes())
                machine.key('f3')
            machine.key('f2')
        machine.key('f1')
    assert len(rendered_styles) == 24, 'Typography controls did not produce 24 distinct text renderings'
    machine.expect(expected)
    machine.key('ctrl-s')
    machine.saved()
    machine.key('esc')
finally:
    machine.close()

machine = Machine(disk)
try:
    machine.open_notes()
    machine.expect(expected)
    machine.key('ctrl-end')
    machine.type('\nSaved twice.')
    expected += '\nSaved twice.'
    machine.key('ctrl-s')
    machine.saved()
    machine.screenshot('saved-note')
finally:
    machine.close()

machine = Machine(disk)
try:
    machine.open_notes()
    machine.expect(expected)
    machine.key('ctrl-end')
    machine.type('\n' * 24 + 'Still visible.')
    machine.wait_int('editor_scroll', lambda v: v > 0)
    machine.screenshot('scrolled-note')
    machine.key('ctrl-home')
    machine.wait_int('editor_scroll', lambda v: v == 0)
finally:
    machine.close()

machine = Machine()
try:
    machine.open_notes()
    initial = machine.memory('editor_buffer', machine.integer('editor_length')).decode()
    machine.type('No disk. Keep this text!')
    machine.key('ctrl-s')
    machine.saved()
    machine.expect(initial + 'No disk. Keep this text!')
    machine.screenshot('ramfs-save')
    machine.key('esc')
    machine.open_notes()
    machine.expect(initial + 'No disk. Keep this text!')
finally:
    machine.close()

print('PASS: boot, dock and toolbar clicks, typing, Shift, Caps Lock, Enter, Tab, insertion, deletion, 24 distinct typography renderings, scrolling, save/reboot/overwrite, ramfs reopen')
print(f'Artifacts: {ARTIFACTS}')
