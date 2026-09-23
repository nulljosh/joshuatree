#!/usr/bin/env python3
"""Headless proof that the Lock Screen menu item in the Apple menu works.

Boot/QMP/keyboard plumbing (the Machine class, open_settings and
click_settings_row) below is copied from tools/checks/auth-flow-check.py,
which already proved this exact plumbing end to end on this same kernel:
the human-monitor "sendkey" mechanism for real keystrokes, the boot-marker
poll in Machine.__init__, and the two click coordinates that open Settings
and its "Add user" row. auth-flow-check.py isn't import-safe (it runs its
whole check at module scope with no __main__ guard, so importing it would
just run it), so the functions are copied here rather than imported, with
a comment at each one saying so.

This file's own earlier version drove the keyboard through the raw QMP
"send-key" event instead, and its login typing never worked; a previous
worker reported PASS without mtools even installed to run the script at
all. Reusing auth-flow-check.py's proven mechanism, instead of the broken
one this file had, is the actual fix -- not a new mechanism. It also lets
Part 2 create its test account the real way, through Settings, instead of
hand-rolling SHA256 and writing USERS.TXT with mtools directly.

Part 1 (no accounts): fresh disk, no USERS.TXT. Clicking Lock Screen in
the Apple menu shows a brief message and leaves the desktop intact --
gui_lock_screen()'s no-account branch in kernel/kernel.c.

Part 2 (with an account, the real lock/unlock story):
  1. Same fresh disk, first boot, no account yet (no gate).
  2. Settings -> Add user, the same account-creation flow
     auth-flow-check.py's Phase 2 already proved (Settings row 6, type
     username/enter/password/enter, wait on auth_logged_in).
  3. Reboot the same image -> a real login gate this time (USERS.TXT now
     has one account). Log in for real, the same way auth-flow-check.py's
     Phase 3 does.
  4. Apple menu -> Lock Screen (index 4 of GUI_MENU_LABELS in
     kernel/kernel.c; the click coordinates below are checked against
     GUI_MENUBAR_H/GUI_MENU_PAD_V/GUI_MENU_ROW_H). auth_logged_in flips
     back to 0, serial gets "auth: locked", the login screen is really
     back up (same dock-inert detection auth-flow-check.py's Phase 3
     uses: click where the Notes dock icon is and prove editor_loaded
     never moves), and the dock is visually gone too (the same
     framebuffer pixel sample this file's Part 1 already used).
  5. Esc does not bypass it.
  6. A wrong password does not bypass it either.
  7. The real password brings the desktop back: dock visible again, and
     the same editor_loaded signal auth-flow-check.py's own post-login
     check uses (clicking the Notes dock icon really opens Notes this
     time) proves it is genuinely interactive, not just painted.

Usage: tools/checks/lockscreen-check.py   (from the repo root, after make kernel.elf)
"""
import json
import re
import shutil
import socket
import subprocess
import tempfile
import time
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parent.parent.parent
WORKDIR = Path(tempfile.mkdtemp(prefix='jt-lockscreen-', dir='/tmp'))
DISK = WORKDIR / 'disk.img'
LOG = WORKDIR / 'serial.log'
QMP_PORT = 4661
LOGICAL_W, LOGICAL_H = 960, 540

# Framebuffer pixel sampling: same pmemsave technique and the same tray
# pixel/color this file's previous Part 1 already used successfully.
FB, FB_W, FB_H, SCALE = 0xfd000000, 1920, 1080, 2
DOCK_TRAY_COLOR = (0xEF, 0xEB, 0xE4)
DOCK_SAMPLE_X, DOCK_SAMPLE_Y = 480, 511  # a tray pixel, present whenever the dock is drawn at all

USERNAME = 'locktest'
PASSWORD = 'pass1234'
WRONG_PASSWORD = 'wrongpass9'

# Apple-menu logo, then the Settings item in its dropdown, and the
# Settings rows -- the exact coordinates tools/checks/auth-flow-check.py
# already proved open Settings and its "Add user" row for real.
LOGO_X, LOGO_Y = 16, 13
SETTINGS_MENU_X, SETTINGS_MENU_Y = 94, 111
SETTINGS_ROWS_Y = [84, 116, 148, 180, 212, 252, 284]
ROW_ADDUSER = 6
# The Notes dock icon, the exact coordinates editor_qa.py's open_notes() /
# auth-flow-check.py already proved land on it.
DOCK_NOTES_X, DOCK_NOTES_Y = 458, 487

# Lock Screen's own row in the Apple menu. kernel/kernel.c's
# GUI_MENU_LABELS is {"About Joshua Tree", "Files", "Notes", "Settings",
# "Lock Screen", "-", "Restart", "Shut Down"} -- index 4. gui_menu_hit_test
# there computes each row's top as GUI_MENUBAR_H + GUI_MENU_PAD_V +
# i * GUI_MENU_ROW_H; with the real constants (26, 8, 22) item 4's row
# spans y in [122, 144). (110, 133) sits inside that band.
GUI_MENUBAR_H, GUI_MENU_PAD_V, GUI_MENU_ROW_H = 26, 8, 22
LOCK_SCREEN_ITEM = 4
LOCK_SCREEN_X = 110
LOCK_SCREEN_Y = GUI_MENUBAR_H + GUI_MENU_PAD_V + LOCK_SCREEN_ITEM * GUI_MENU_ROW_H + GUI_MENU_ROW_H // 2

nm = shutil.which('nm') or 'nm'
symbols = {}
for line in subprocess.check_output([nm, str(ROOT / 'kernel.elf')], text=True).splitlines():
    fields = line.split()
    if len(fields) == 3:
        symbols[fields[2]] = int(fields[0], 16) - 0xC0000000


# ---------------------------------------------------------------------------
# Copied from tools/checks/auth-flow-check.py's own Machine class (same QMP
# monitor sendkey/input-send-event plumbing, same boot-marker poll). The
# only change is an optional serial_log path, needed here to see the
# "auth: locked" line gui_lock_screen() prints to serial -- auth-flow-check
# never needed serial output, only in-memory kernel symbols.
class Machine:
    def __init__(self, disk, serial_log=None):
        arguments = ['qemu-system-i386', '-kernel', str(ROOT / 'kernel.elf'), '-display', 'none', '-vga', 'std',
                     '-drive', f'file={disk},format=raw,if=ide',
                     '-qmp', f'tcp:127.0.0.1:{QMP_PORT},server,nowait']
        if serial_log is not None:
            arguments += ['-serial', f'file:{serial_log}']
        self.process = subprocess.Popen(arguments, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        self._connect_qmp()
        for attempt in range(100):
            if 'b007c0de' in self.monitor('xp /1xw 0x9000'):
                break
            time.sleep(.1)
        else:
            self.close()
            raise AssertionError('Boot marker not reached')
        time.sleep(2)

    def _connect_qmp(self):
        connection = None
        for attempt in range(300):
            try:
                connection = socket.create_connection(('127.0.0.1', QMP_PORT), timeout=1)
                connection.settimeout(10)
                stream = connection.makefile('rwb', buffering=0)
                greeting = stream.readline()
                if not greeting:
                    raise ConnectionError('empty QMP greeting')
                self.connection, self.stream = connection, stream
                self.command('qmp_capabilities')
                return
            except (OSError, ConnectionError, ValueError):
                if connection is not None:
                    try:
                        connection.close()
                    except OSError:
                        pass
                time.sleep(.1)
        self.process.kill()
        raise AssertionError('QEMU QMP socket never came up')

    def command(self, name, arguments=None):
        payload = (json.dumps({'execute': name, 'arguments': arguments or {}}) + '\n').encode()
        for attempt in (1, 2, 3):
            try:
                self.stream.write(payload)
                while True:
                    line = self.stream.readline()
                    if not line:
                        raise ConnectionError('QMP connection dropped')
                    result = json.loads(line)
                    if 'error' in result:
                        raise RuntimeError(result)
                    if 'return' in result:
                        return result['return']
            except (ConnectionError, OSError, json.JSONDecodeError):
                if attempt == 3:
                    raise
                self._connect_qmp()

    def monitor(self, command):
        return self.command('human-monitor-command', {'command-line': command})

    def key(self, key):
        self.monitor(f'sendkey {key} 30')
        time.sleep(.08)

    def type(self, text):
        punctuation = {' ': 'spc', '\n': 'ret', '\t': 'tab', '.': 'dot', ',': 'comma'}
        for character in text:
            self.key(punctuation.get(character, 'shift-' + character.lower() if character.isupper() else character))

    def move(self, target_x, target_y):
        self.command('input-send-event', {'events': [
            {'type': 'abs', 'data': {'axis': 'x', 'value': target_x * 32768 // LOGICAL_W}},
            {'type': 'abs', 'data': {'axis': 'y', 'value': target_y * 32768 // LOGICAL_H}}]})
        time.sleep(.2)

    def click(self):
        for down in (True, False):
            self.command('input-send-event', {'events': [{'type': 'btn', 'data': {'down': down, 'button': 'left'}}]})
            time.sleep(.15)

    def click_at(self, x, y):
        self.move(x, y)
        self.click()
        time.sleep(.3)

    def memory(self, symbol, count):
        result = self.monitor(f'xp /{count}xb 0x{symbols[symbol]:x}')
        return bytes(int(value, 16) for line in result.splitlines() if ':' in line
                     for value in re.findall(r'0x([0-9a-f]{2})\b', line.split(':', 1)[1]))

    def integer(self, symbol):
        return int.from_bytes(self.memory(symbol, 4), 'little')

    def string(self, symbol, maxlen):
        raw = self.memory(symbol, maxlen)
        return raw.split(b'\x00', 1)[0].decode('latin1')

    def wait_int(self, symbol, predicate, desc='', timeout=6):
        value = None
        deadline = time.time() + timeout
        while time.time() < deadline:
            value = self.integer(symbol)
            if predicate(value):
                return value
            time.sleep(0.05)
        value = self.integer(symbol)
        assert predicate(value), (desc or symbol, value)
        return value

    def assert_no_int_change(self, symbol, expected, window):
        deadline = time.time() + window
        while time.time() < deadline:
            v = self.integer(symbol)
            assert v == expected, (symbol, 'changed unexpectedly to', v, 'expected it to stay', expected)
            time.sleep(0.05)

    def screenshot(self, name):
        raw = WORKDIR / 'framebuffer.raw'
        self.command('pmemsave', {'val': FB, 'size': FB_W * FB_H * 4, 'filename': str(raw)})
        frame = Image.frombytes('RGB', (FB_W, FB_H), raw.read_bytes(), 'raw', 'BGRX')
        frame.save(WORKDIR / (name + '.png'))
        return frame

    # Not part of auth-flow-check.py's Machine (it only ever needed the
    # functional editor_loaded signal below, never a visual dock check) --
    # added here for the "dock is NOT visible" / "dock visible" checks this
    # file's own previous version already used successfully in Part 1.
    def pixel(self, x, y):
        raw = WORKDIR / 'pixel.raw'
        self.command('pmemsave', {'val': FB, 'size': FB_W * FB_H * 4, 'filename': str(raw)})
        img = Image.frombytes('RGBA', (FB_W, FB_H), raw.read_bytes(), 'raw', 'BGRA').convert('RGB')
        return img.getpixel((x * SCALE + 1, y * SCALE + 1))

    def dock_visible(self):
        return self.pixel(DOCK_SAMPLE_X, DOCK_SAMPLE_Y) == DOCK_TRAY_COLOR

    def wait_dock(self, expected, timeout=6):
        # gui_lock_screen()'s no-account branch alone holds the message for
        # ~1s (sleep_ticks(100)) and then gui_draw_boot_screen() holds its
        # own splash for ~1s more before the real desktop repaint ever
        # happens -- a fixed host-side sleep guessed short here, so this
        # polls the real framebuffer instead of guessing a duration. It
        # also debounces: a single matching sample can land mid-repaint
        # (pmemsave racing the guest's own draw, or one still-animating
        # splash frame that happens to match), so it requires three
        # consecutive matching samples, 0.1s apart, before it trusts it.
        # Returns whether it settled on `expected`, for the caller's own
        # check() -- never re-sampled again after this settles, so a lone
        # unlucky read right at the assertion can't undo a real result.
        deadline = time.time() + timeout
        streak = 0
        visible = self.dock_visible()
        while time.time() < deadline:
            visible = self.dock_visible()
            streak = streak + 1 if visible == expected else 0
            if streak >= 3:
                return True
            time.sleep(0.1)
        return visible == expected

    def close(self):
        try:
            self.command('quit')
        except (OSError, ValueError, RuntimeError, ConnectionError, AssertionError, json.JSONDecodeError):
            pass
        finally:
            try:
                self.process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait()
            try:
                self.stream.close()
            except Exception:
                pass
            try:
                self.connection.close()
            except Exception:
                pass


# Copied from tools/checks/auth-flow-check.py.
def open_settings(machine):
    machine.click_at(LOGO_X, LOGO_Y)
    machine.click_at(SETTINGS_MENU_X, SETTINGS_MENU_Y)
    time.sleep(0.5)


# Copied from tools/checks/auth-flow-check.py.
def click_settings_row(machine, row):
    machine.click_at(200, SETTINGS_ROWS_Y[row] + 5)


def click_lock_screen(machine):
    machine.click_at(LOGO_X, LOGO_Y)
    time.sleep(0.3)
    machine.click_at(LOCK_SCREEN_X, LOCK_SCREEN_Y)
    time.sleep(0.3)


def login(machine, username, password):
    machine.type(username)
    machine.key('ret')
    time.sleep(0.3)
    machine.type(password)
    machine.key('ret')


# Copied from tools/checks/auth-flow-check.py.
def check(label, condition):
    status = 'PASS' if condition else 'FAIL'
    print(f'  {status}: {label}')
    if not condition:
        raise AssertionError(label)


print('=== Part 1: no accounts -- Lock Screen shows a message and leaves the desktop intact ===')
subprocess.run(['bash', str(ROOT / 'tools' / 'mkdisk.sh'), str(DISK)], check=True, cwd=ROOT)

m1 = Machine(DISK)
try:
    check('fresh boot: auth_user_count is 0 (no USERS.TXT yet)', m1.integer('auth_user_count') == 0)
    check('fresh boot: dock is visible (no gate to block it)', m1.dock_visible())

    click_lock_screen(m1)
    time.sleep(0.4)  # let the "No accounts to lock with" message frame present before it's gone
    m1.screenshot('00-no-account-message')
    dock_back = m1.wait_dock(True, timeout=6)
    check('no accounts: still not logged in (nothing to log into)', m1.integer('auth_logged_in') == 0)
    check('no accounts: dock is back (Lock Screen left the desktop intact)', dock_back)
finally:
    m1.close()

print()
print('=== Part 2: with an account -- lock really locks, Esc and a wrong password cannot bypass it, the right one unlocks ===')
print('--- create the account through Settings, the flow auth-flow-check.py Phase 2 already proved ---')
m2 = Machine(DISK)
try:
    open_settings(m2)
    click_settings_row(m2, ROW_ADDUSER)
    time.sleep(0.4)
    m2.type(USERNAME)
    m2.key('ret')
    time.sleep(0.3)
    m2.type(PASSWORD)
    m2.key('ret')
    m2.wait_int('auth_logged_in', lambda v: v == 1, 'Account was not created / session not adopted', timeout=8)
    check('account created: auth_user_count == 1', m2.integer('auth_user_count') == 1)
    check(f'account created: auth_current_user == "{USERNAME}"', m2.string('auth_current_user', 25) == USERNAME)
    time.sleep(0.8)  # let the "Account created." frame present before tearing the process down
finally:
    m2.close()

print('--- reboot the same image: a real login gate this time; log in for real ---')
try:
    LOG.unlink()
except FileNotFoundError:
    pass
m3 = Machine(DISK, serial_log=LOG)
try:
    check('reboot: account persisted (auth_user_count == 1)', m3.integer('auth_user_count') == 1)
    check('reboot: not logged in yet (real gate this time)', m3.integer('auth_logged_in') == 0)
    check('reboot: dock is not visible behind the login prompt', not m3.wait_dock(True, timeout=2))

    login(m3, USERNAME, PASSWORD)
    m3.wait_int('auth_logged_in', lambda v: v == 1, 'Correct password did not log in', timeout=5)
    check('login: logged in (auth_logged_in == 1)', m3.integer('auth_logged_in') == 1)
    # A successful login still runs gui_draw_boot_screen()'s ~1s splash
    # before the real desktop repaint happens, so poll (debounced against a
    # torn pmemsave read or a still-animating splash frame) rather than
    # guess a duration or trust a single sample.
    check('login: dock is visible again', m3.wait_dock(True, timeout=6))
    m3.screenshot('01-post-login-desktop')

    print('--- Apple menu -> Lock Screen: really locks ---')
    click_lock_screen(m3)
    dock_gone = m3.wait_dock(False, timeout=6)
    with open(LOG, errors='replace') as lf:
        locked_logged = 'auth: locked' in lf.read()
    check("locking: serial log gained 'auth: locked'", locked_logged)
    check('locking: not logged in any more (auth_logged_in == 0)', m3.integer('auth_logged_in') == 0)
    # Same detection auth-flow-check.py's Phase 3 uses for "the login screen
    # is really up, not just painted": editor_loaded is still 0 (Notes was
    # never opened in this boot yet), so a click on the Notes dock icon
    # that leaves it at 0 proves the gate, not gui_run's normal dock hit
    # test, is the one eating the click.
    m3.click_at(DOCK_NOTES_X, DOCK_NOTES_Y)
    m3.assert_no_int_change('editor_loaded', 0, window=1.0)
    check('locking: the login screen is really back (dock inert, same detection auth-flow-check.py uses)', True)
    check('locking: dock is NOT visible', dock_gone)
    m3.screenshot('02-locked')

    print('--- Esc does not bypass the lock ---')
    m3.key('esc')
    time.sleep(0.5)
    check('esc: still locked (auth_logged_in stays 0)', m3.integer('auth_logged_in') == 0)
    check('esc: still the login screen (dock still not visible)', not m3.wait_dock(True, timeout=2))

    print('--- a wrong password does not bypass the lock ---')
    login(m3, USERNAME, WRONG_PASSWORD)
    time.sleep(1.0)  # the fixed 1s rejection throttle in auth_login_screen
    check('wrong password: still locked (auth_logged_in stays 0)', m3.integer('auth_logged_in') == 0)
    check('wrong password: still the login screen (dock still not visible)', not m3.wait_dock(True, timeout=2))

    print('--- the real password unlocks it ---')
    login(m3, USERNAME, PASSWORD)
    m3.wait_int('auth_logged_in', lambda v: v == 1, 'Correct password did not unlock', timeout=5)
    check('unlock: logged in again (auth_logged_in == 1)', m3.integer('auth_logged_in') == 1)
    check('unlock: dock is visible again', m3.wait_dock(True, timeout=6))
    # Same signal auth-flow-check.py's own post-login check uses: a real
    # click on the Notes dock icon really opens Notes (editor_loaded flips
    # 0 -> 1 for the first and only time in this boot), not just a pixel
    # that happens to look right.
    m3.click_at(DOCK_NOTES_X, DOCK_NOTES_Y)
    m3.wait_int('editor_loaded', lambda v: v == 1, 'Desktop did not become interactive after unlocking', timeout=5)
    check('unlock: the desktop is interactive again (Notes opens, same signal auth-flow-check.py uses)', True)
    m3.key('esc')
    time.sleep(0.3)
    m3.screenshot('03-unlocked')
finally:
    m3.close()

print()
print('PASS: Lock Screen: with no accounts it shows a message and leaves the desktop intact; with an account it '
      'really locks (auth: locked on serial, login screen back up, dock gone), Esc and a wrong password cannot '
      'bypass it, and the real password brings the desktop back interactive')
print(f'Artifacts: {WORKDIR}')
