"""Shared headless QEMU driver for the ring-3 era text checks (Notes, Terminal).

One VM per boot: a QMP socket on a free port, the serial log in a file, and
the handful of helpers every check needs (keys, chords, pointer, framebuffer,
serial waits). The kernel's own `open=notes` / `open=term` boot flags open the
app the moment the desktop is up, so a check never guesses dock coordinates.
Ring-3 apps speak through write(1), which the kernel logs behind
"syscall: write(1) from ring 3: ", so every wait is a substring match on the
app's own marker.

Not a check itself; ci-suite.sh never runs it.
"""
import json, socket, subprocess, tempfile, time
from pathlib import Path
from PIL import Image
from freeport import free_port

ROOT = Path(__file__).resolve().parent.parent.parent
FB, FBW, FBH = 0xfd000000, 1920, 1080
LOGICAL_W, LOGICAL_H, SCALE = 960, 540, 2
PUNCT = {' ': 'spc', '\n': 'ret', '.': 'dot', ',': 'comma', '!': 'shift-1', '?': 'shift-slash', '-': 'minus',
         '/': 'slash', ';': 'semicolon', "'": 'apostrophe', ':': 'shift-semicolon', '=': 'equal'}


# The ring-3 Terminal (user/terminal.c) opened by `open=term`: its viewport is the
# dock-launch one (logical 78,72, 805 x 345), the prompt line sits 52 px above the
# bottom edge and its typed text starts after "~> " at local x 189 (the 2.11.0 tab rail takes the first 149), so the first
# typed glyph's cell is at logical (267, 365). Cells are 8 logical px wide (the
# libjt mono advance), 16 tall; the page colour is 0x1A1512.
TERM_INPUT_X0, TERM_INPUT_Y0 = 267, 365
TERM_CELL_L, TERM_BG = 8, (0x1A, 0x15, 0x12)


def term_cell_has_ink(img, cell_index):
    """True if any non-page pixel sits inside this input cell's own physical box."""
    x0 = (TERM_INPUT_X0 + cell_index * TERM_CELL_L) * SCALE
    y0 = TERM_INPUT_Y0 * SCALE
    for y in range(y0, y0 + 16 * SCALE):
        for x in range(x0, x0 + TERM_CELL_L * SCALE):
            p = img.getpixel((x, y))
            if max(abs(p[i] - TERM_BG[i]) for i in range(3)) > 10:
                return True
    return False


def term_type_line(vm, line):
    """Types line into the Terminal's prompt like a person watching the screen:
    each key waits for its own cell to ink before the next goes out (a fixed
    sleep dropped keys on a slow runner). Returns the last frame."""
    img = vm.frame()
    for i, ch in enumerate(line):
        vm.key('shift-' + ch.lower() if ch.isupper() else {'|': 'shift-backslash'}.get(ch, ch), gap=0.05)
        for _ in range(60):
            time.sleep(0.1)
            img = vm.frame()
            if term_cell_has_ink(img, i):
                break
    time.sleep(0.2)
    return vm.frame()


class VM:
    def __init__(self, disk=None, append='', ready=None, extra=(), work=None, boot_wait=60):
        self.work = Path(work or tempfile.mkdtemp(prefix='jt-vm-'))
        self.log = self.work / ('serial-%d.log' % time.time_ns())
        self.port = free_port()
        args = ['qemu-system-i386', '-kernel', str(ROOT / 'kernel.elf'), '-display', 'none', '-vga', 'std',
                '-qmp', f'tcp:127.0.0.1:{self.port},server,nowait', '-serial', 'file:' + str(self.log)]
        if disk:
            args += ['-drive', f'file={disk},format=raw,if=ide']
        if append:
            args += ['-append', append]
        args += list(extra)
        self.proc = subprocess.Popen(args, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        sock = None
        for _ in range(100):
            time.sleep(0.2)
            try:
                sock = socket.create_connection(('127.0.0.1', self.port))
                break
            except OSError:
                pass
        if sock is None:
            self.quit()
            raise AssertionError('QEMU QMP socket never came up')
        self.f = sock.makefile('rw')
        self.f.readline()
        self.cmd({'execute': 'qmp_capabilities'})
        if ready and not self.wait(ready, boot_wait):
            tail = self.serial()[-600:]
            self.quit()
            raise AssertionError(f'never saw {ready!r} on serial; tail: {tail!r}')
        time.sleep(0.8)

    def cmd(self, obj):
        self.f.write(json.dumps(obj) + '\n')
        self.f.flush()
        while True:
            r = json.loads(self.f.readline())
            if 'return' in r or 'error' in r:
                return r

    def serial(self):
        try:
            return self.log.read_text(errors='replace')
        except OSError:
            return ''

    def count(self, needle):
        return self.serial().count(needle)

    def wait(self, needle, secs, count=1):
        for _ in range(int(secs * 10)):
            if self.serial().count(needle) >= count:
                return True
            time.sleep(0.1)
        return False

    def key(self, qcode, gap=0.2):
        if '-' in qcode:   # a chord (shift-a, ctrl-s): the human monitor's sendkey takes combos
            self.cmd({'execute': 'human-monitor-command', 'arguments': {'command-line': f'sendkey {qcode} 30'}})
        else:
            self.cmd({'execute': 'send-key', 'arguments': {'keys': [{'type': 'qcode', 'data': qcode}]}})
        time.sleep(gap)

    def type(self, text, gap=0.2):
        for c in text:
            self.key(PUNCT.get(c, 'shift-' + c.lower() if c.isupper() else c), gap)

    def move(self, x, y):
        self.cmd({'execute': 'input-send-event', 'arguments': {'events': [
            {'type': 'abs', 'data': {'axis': 'x', 'value': int(x * 32768 / LOGICAL_W)}},
            {'type': 'abs', 'data': {'axis': 'y', 'value': int(y * 32768 / LOGICAL_H)}}]}})
        time.sleep(0.2)

    def click(self):
        for down in (True, False):
            self.cmd({'execute': 'input-send-event', 'arguments': {'events': [{'type': 'btn', 'data': {'down': down, 'button': 'left'}}]}})
            time.sleep(0.12)
        time.sleep(0.2)

    def frame(self):
        raw = self.work / 'fb.raw'
        self.cmd({'execute': 'pmemsave', 'arguments': {'val': FB, 'size': FBW * FBH * 4, 'filename': str(raw)}})
        return Image.frombytes('RGB', (FBW, FBH), raw.read_bytes(), 'raw', 'BGRX')

    def quit(self):
        try:
            self.cmd({'execute': 'quit'})
        except Exception:
            pass
        try:
            self.proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            self.proc.wait()


def make_disk(path):
    """A fresh FAT16 image through the repo's own tools/mkdisk.sh."""
    subprocess.run(['bash', str(ROOT / 'tools' / 'mkdisk.sh'), str(path)], check=True, stdout=subprocess.DEVNULL)


def disk_files(disk):
    """Every file on the image as {path: bytes}, read with mtools, never from the guest's own view."""
    out = {}
    listing = subprocess.run(['mdir', '-/', '-b', '-i', str(disk), '::'], capture_output=True, text=True).stdout
    for line in listing.splitlines():
        p = line.strip()
        if not p.startswith('::'):
            continue
        r = subprocess.run(['mtype', '-i', str(disk), p], capture_output=True)
        if r.returncode == 0:
            out[p[2:].lstrip('/')] = r.stdout
    return out
