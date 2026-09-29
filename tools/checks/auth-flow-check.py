#!/usr/bin/env python3
"""End-to-end proof of the whole account story on a fresh disk image, the
1.0 gate Joshua asked for directly: "it needs account creation and login,
super stable and secure." kernel/auth.h and tools/auth-host/ already prove
the crypto and USERS.TXT parsing logic in isolation (tools/checks/
auth-check.sh, a host-compiled unit suite); this proves the real, whole
flow through the actual GUI, on the actual boot path, the way a person at
the keyboard experiences it -- Settings navigation, real QMP keystrokes,
a real disk image, three real reboots.

Boot/QMP/keyboard/pmemsave plumbing copied from editor_qa.py's Machine
class (same shape: QMP monitor xp/sendkey/input-send-event, poll a real
kernel symbol instead of a bare sleep, reboot the same disk image to
prove persistence). Settings navigation (Apple-menu logo click then the
Settings item) copied from walldemo-regression-check.py, which already
proved those exact two click coordinates open Settings. tools/mkdisk.sh
builds the real FAT disk image, same as every other disk-backed check.

The flow, each step polled on a real kernel symbol or window_present_count
(never a bare sleep):
  0. (1.7.9) A pre-PBKDF2 USERS.TXT written straight into the image with
     mtools: the untagged chained-SHA-256 record. It still logs in, that
     login rewrites it as a tagged PBKDF2 record (checked through the FAT
     directory, not a raw grep), a reboot logs in again on the new record
     with the login time printed and bounded, wrong passwords rejected
     both times.
  1. Fresh image, first boot, no USERS.TXT: no gate, desktop reachable
     (kernel/auth.h's own documented design: nothing configured, nothing
     to protect).
  2. Settings -> Add user "joshua" / "hidden valley 1". auth_user_count
     flips to 1 in kernel memory; USERS.TXT on the raw disk image (read
     back directly, not trusted from RAM) holds exactly one well-formed
     "joshua:p2:<iters>:<salt>:<hash>" line and never the plaintext
     password.
  3. Reboot the same image. The dock is inert until login succeeds (no
     gui_draw_boot_screen() call has happened yet -- auth_gate() blocks
     first). Esc on either field does not bypass the gate. A wrong
     password is rejected with a real, measured delay (bracketed by two
     window_present_count changes, not a host-side timer). An empty
     password is rejected. The real password logs in and the desktop
     becomes interactive.
  4. Settings -> Account (change password) to "new pass 2".
  5. Reboot again: the old password is rejected, the new one is accepted.
     The raw disk image never contains either plaintext password at any
     point, and the salt/hash actually rotated (not left stale).

Real bug found and fixed by writing this check: kernel/auth.h's login-
rejection message and kernel.c's "Password changed."/"Account created."
status lines were drawn into the back buffer but never presented before
falling into sleep_ticks() -- drivers/window.c's own header comment
documents window_present() as belonging "at a real frame boundary...
about to wait for input", and none of these three call sites had one. The
next loop iteration's window_clear() erased each message before it was
ever flipped to the visible framebuffer, so a real person typing a wrong
password (or renaming/changing their account) saw the screen pause and
silently reset with no feedback at all. Fixed by adding the missing
window_present() call at all three sites; verified by this check, which
depends on that exact frame boundary to bracket the rejection delay via
window_present_count.
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
from freeport import free_port

ROOT = Path(__file__).resolve().parent.parent.parent
WORKDIR = Path(tempfile.mkdtemp(prefix='jt-auth-flow-', dir='/tmp'))
DISK = WORKDIR / 'disk.img'
QMP_PORT = free_port()
LOGICAL_W, LOGICAL_H = 960, 540

USERNAME = 'joshua'
PASSWORD1 = 'hidden valley 1'
PASSWORD2 = 'new pass 2'

# Apple-menu logo, then the Settings item in its dropdown -- the exact two
# click coordinates tools/checks/walldemo-regression-check.py already
# proved open Settings for real.
LOGO_X, LOGO_Y = 16, 13
SETTINGS_MENU_X, SETTINGS_MENU_Y = 94, 111
ROW_ACCOUNT = 5
ROW_ADDUSER = 6
# 1.8's redesign (kernel/settings_ui.h) replaced the old flat 8-row list
# with a left sidebar of sections (General/Assistant/Account) and a right
# detail pane showing only the current section's rows -- SETTINGS_ROWS_Y
# no longer exists and a bare click at the old absolute row y no longer
# lands anywhere (it's now inside the sidebar's own x range, and row 6
# isn't even drawn until the Account section is selected). Mirrors
# kernel/settings_ui.h's own SETTINGS_SECTION_ROWS / SETTINGS_SIDEBAR_* /
# SETTINGS_DETAIL_* constants so a click_settings_row() call reaches a row
# the same way a person clicking the real UI would: pick the section in
# the sidebar first, then the row inside it.
SETTINGS_SECTIONS = [
    [0, 1, 2, 7],  # General: Wind, Dock size, Wallpaper, Location
    [3, 4],        # Assistant: LLM model, LLM host:port
    [5, 6],        # Account: Account (change password), Add user
]
SETTINGS_SIDEBAR_X = 90
SETTINGS_SIDEBAR_Y0 = 60
SETTINGS_SIDEBAR_ROW_H = 34
SETTINGS_DETAIL_X = 300
SETTINGS_DETAIL_Y0 = 92
SETTINGS_ROW_H = 36


def settings_section_for_row(row):
    for section, rows in enumerate(SETTINGS_SECTIONS):
        if row in rows:
            return section, rows.index(row)
    raise ValueError(f'row {row} is not in any settings section')
# The Notes dock icon, the exact coordinates editor_qa.py's open_notes()
# already proved land on it.
DOCK_NOTES_X, DOCK_NOTES_Y = 458, 487

nm = shutil.which('nm') or 'nm'
symbols = {}
for line in subprocess.check_output([nm, str(ROOT / 'kernel.elf')], text=True).splitlines():
    fields = line.split()
    if len(fields) == 3:
        symbols[fields[2]] = int(fields[0], 16) - 0xC0000000


class Machine:
    def __init__(self, disk):
        arguments = ['qemu-system-i386', '-kernel', str(ROOT / 'kernel.elf'), '-display', 'none', '-vga', 'std',
                     '-drive', f'file={disk},format=raw,if=ide',
                     '-qmp', f'tcp:127.0.0.1:{QMP_PORT},server,nowait']
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
        # Three sequential Machine()s all bind the SAME static QMP_PORT
        # (the task calls for a fixed port), each right after the previous
        # one quit. In practice the very next QMP command after a fresh
        # connect can still see the socket drop out from under it (macOS
        # releasing the previous process' bound port is not instant) --
        # this retries the connect+handshake, not a bare sleep, so it
        # recovers the moment the new QEMU's listener is really live.
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

    def wait_present_change(self, baseline, timeout=3):
        # window_present_count only increments at a real frame boundary
        # (drivers/window.c: "a no-op when nothing was drawn since the
        # last call") -- polling it, rather than sleeping a guessed
        # duration, is how this brackets the real in-kernel rejection
        # delay precisely.
        deadline = time.time() + timeout
        while time.time() < deadline:
            v = self.integer('window_present_count')
            if v != baseline:
                return v
            time.sleep(0.02)
        raise AssertionError(f'window_present_count never advanced past {baseline}')

    def assert_no_int_change(self, symbol, expected, window):
        deadline = time.time() + window
        while time.time() < deadline:
            v = self.integer(symbol)
            assert v == expected, (symbol, 'changed unexpectedly to', v, 'expected it to stay', expected)
            time.sleep(0.05)

    def screenshot(self, name):
        raw = WORKDIR / 'framebuffer.raw'
        self.command('pmemsave', {'val': 0xfd000000, 'size': 1920 * 1080 * 4, 'filename': str(raw)})
        frame = Image.frombytes('RGB', (1920, 1080), raw.read_bytes(), 'raw', 'BGRX')
        frame.save(WORKDIR / (name + '.png'))
        return frame

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


def open_settings(machine):
    machine.click_at(LOGO_X, LOGO_Y)
    machine.click_at(SETTINGS_MENU_X, SETTINGS_MENU_Y)
    time.sleep(0.5)


def click_settings_row(machine, row):
    # Click the row's own section in the sidebar first -- exactly what a
    # person at the keyboard does, and required now: the detail pane only
    # draws (and only hit-tests) the current section's rows, so a row in a
    # not-yet-selected section can't be clicked directly.
    section, pos = settings_section_for_row(row)
    machine.click_at(SETTINGS_SIDEBAR_X, SETTINGS_SIDEBAR_Y0 + section * SETTINGS_SIDEBAR_ROW_H + 9)
    machine.click_at(SETTINGS_DETAIL_X, SETTINGS_DETAIL_Y0 + pos * SETTINGS_ROW_H + 5)


def check(label, condition):
    status = 'PASS' if condition else 'FAIL'
    print(f'  {status}: {label}')
    if not condition:
        raise AssertionError(label)


def legacy_hash(salt, password):
    # The pre-1.7.9 scheme kernel/auth.h still verifies (never writes):
    # sha256(salt || password), then 199,999 more rounds of sha256 over
    # the previous digest. Computed here on the host so the fixture is an
    # honest old-format record, not one the new kernel wrote for itself.
    import hashlib
    h = hashlib.sha256(salt + password.encode()).digest()
    for _ in range(200000 - 1):
        h = hashlib.sha256(h).digest()
    return h


def users_txt(disk):
    # The real file as the FAT directory sees it (mtools), not a raw grep
    # that also sees freed clusters.
    return subprocess.run(['mtype', '-i', str(disk), '::USERS.TXT'], capture_output=True, text=True).stdout


def timed_login(machine, password, label, limit=1.5):
    machine.type(USERNAME)
    machine.key('ret')
    time.sleep(0.3)
    machine.type(password)
    t0 = time.time()
    machine.key('ret')
    machine.wait_int('auth_logged_in', lambda v: v == 1, f'{label}: password did not log in', timeout=8)
    elapsed = time.time() - t0
    check(f'{label}: logged in, {elapsed:.2f}s from Enter to auth_logged_in (limit {limit}s)', elapsed <= limit)
    return elapsed


print('=== Phase 0: pre-1.7.9 USERS.TXT fixture -> login upgrades it to PBKDF2 in place ===')
subprocess.run(['bash', str(ROOT / 'tools' / 'mkdisk.sh'), str(DISK)], check=True, cwd=ROOT)
legacy_salt = bytes(range(0x30, 0x40))
legacy_line = f'{USERNAME}:{legacy_salt.hex()}:{legacy_hash(legacy_salt, PASSWORD1).hex()}\n'
fixture = WORKDIR / 'USERS.TXT'
fixture.write_text(legacy_line)
subprocess.run(['mcopy', '-i', str(DISK), str(fixture), '::USERS.TXT'], check=True)
check('legacy fixture: USERS.TXT on disk is the untagged 3-field record', users_txt(DISK) == legacy_line)

m0 = Machine(DISK)
try:
    check('legacy boot: the old record still loads (auth_user_count == 1)', m0.integer('auth_user_count') == 1)
    check('legacy boot: gate engaged (auth_logged_in == 0)', m0.integer('auth_logged_in') == 0)
    m0.type(USERNAME)
    m0.key('ret')
    time.sleep(0.3)
    m0.type('notrealpassword')
    m0.key('ret')
    time.sleep(1.5)
    check('legacy record: wrong password rejected', m0.integer('auth_logged_in') == 0)
    check('legacy record: a failed login does not rewrite it', m0.integer('auth_upgraded_count') == 0)
    timed_login(m0, PASSWORD1, 'legacy record, right password', limit=3.0)
    check('legacy record: upgraded on that login (auth_upgraded_count == 1)', m0.integer('auth_upgraded_count') == 1)
    time.sleep(1.0)
finally:
    m0.close()

after = users_txt(DISK)
upgraded = re.findall(rf'^{USERNAME}:p2:(\d+):([0-9a-f]{{32}}):([0-9a-f]{{64}})$', after, re.M)
check('USERS.TXT after the login: exactly one tagged PBKDF2 "joshua" line', len(upgraded) == 1 and after.count('\n') == 1)
check('USERS.TXT after the login: iteration count stored in the record (100000)', upgraded and upgraded[0][0] == '100000')
check('USERS.TXT after the login: salt rotated, hash changed', upgraded and upgraded[0][1] != legacy_salt.hex() and upgraded[0][2] != legacy_line.split(':')[2].strip())
check('USERS.TXT after the login: plaintext never on disk', PASSWORD1.encode() not in DISK.read_bytes())

m0b = Machine(DISK)
try:
    check('reboot on the upgraded record: account persisted', m0b.integer('auth_user_count') == 1)
    m0b.type(USERNAME)
    m0b.key('ret')
    time.sleep(0.3)
    m0b.type('notrealpassword')
    m0b.key('ret')
    time.sleep(1.5)
    check('upgraded record: wrong password rejected', m0b.integer('auth_logged_in') == 0)
    pbkdf2_login_s = timed_login(m0b, PASSWORD1, 'upgraded PBKDF2 record, right password')
    check('upgraded record: nothing left to upgrade (auth_upgraded_count stays 0)', m0b.integer('auth_upgraded_count') == 0)
finally:
    m0b.close()
print(f'  PBKDF2 login time in QEMU: {pbkdf2_login_s:.2f}s')

print('=== Phase 1: fresh image, no account -> no gate, desktop reachable ===')
subprocess.run(['bash', str(ROOT / 'tools' / 'mkdisk.sh'), str(DISK)], check=True, cwd=ROOT)

m1 = Machine(DISK)
try:
    check('fresh boot: auth_user_count is 0 (no USERS.TXT yet)', m1.integer('auth_user_count') == 0)
    check('fresh boot: auth_logged_in is 0 (nothing to log into)', m1.integer('auth_logged_in') == 0)
    m1.screenshot('01-fresh-desktop')
    m1.click_at(DOCK_NOTES_X, DOCK_NOTES_Y)
    # v2.0 (notes/folders): the dock click opens straight into a note (a
    # fresh disk gets a new one). editor_loaded is shrunk to one byte by
    # the compiler and sits right before editor_buffer, so mask the low
    # byte when reading it as an int.
    m1.wait_int('editor_loaded', lambda v: (v & 0xff) == 1, 'Notes did not open on a gate-free fresh image', timeout=5)
    check('fresh boot: desktop is live with no accounts configured (today\'s documented behaviour)', True)
    m1.key('esc')
    time.sleep(0.2)
    m1.key('esc')
    time.sleep(0.3)

    print('=== Phase 2: Settings -> Add user "joshua" / "hidden valley 1" ===')
    open_settings(m1)
    click_settings_row(m1, ROW_ADDUSER)
    time.sleep(0.4)
    m1.type(USERNAME)
    m1.key('ret')
    time.sleep(0.3)
    m1.type(PASSWORD1)
    m1.key('ret')
    # auth_create_user() flips auth_user_count *before* returning (it still
    # has AUTH_HASH_ROUNDS of hashing and a disk write left to do), and the
    # caller only sets auth_logged_in once auth_create_user() has fully
    # returned -- so polling auth_user_count alone can observe a real,
    # narrow window where the account exists but the session hasn't been
    # marked logged-in yet. Wait on auth_logged_in, the last write in that
    # causal chain, which also guarantees auth_user_count already settled.
    m1.wait_int('auth_logged_in', lambda v: v == 1, 'Account was not created / session not adopted', timeout=8)
    check('account created: auth_user_count == 1', m1.integer('auth_user_count') == 1)
    check('account created: auth_logged_in == 1 (first account auto-adopted as the session)', m1.integer('auth_logged_in') == 1)
    check('account created: auth_current_user == "joshua"', m1.string('auth_current_user', 25) == USERNAME)
    time.sleep(0.8)  # let the (now-fixed) "Account created." frame present and its in-kernel delay finish before tearing the process down
finally:
    m1.close()

raw = DISK.read_bytes()
matches = re.findall(rb'joshua:p2:\d+:([0-9a-f]{32}):([0-9a-f]{64})', raw)
check('USERS.TXT on the raw disk image: exactly one well-formed "joshua" line', len(matches) == 1)
original_hash = matches[0][1] if matches else None
check('USERS.TXT on the raw disk image: plaintext "hidden valley 1" never appears anywhere', b'hidden valley 1' not in raw)

print('=== Phase 3: reboot -> real login gate, wrong/empty rejected, esc never bypasses, right password admits ===')
m2 = Machine(DISK)
try:
    check('reboot: account persisted (auth_user_count == 1)', m2.integer('auth_user_count') == 1)
    check('reboot: not logged in yet', m2.integer('auth_logged_in') == 0)
    m2.screenshot('02-login-screen')

    # The dock cannot be interactive at all yet: gui_draw_boot_screen() is
    # called strictly after auth_gate() returns (kernel.c's gui_run), so
    # there is no desktop app router running behind the prompt to leak a
    # previous session or answer a click.
    m2.click_at(DOCK_NOTES_X, DOCK_NOTES_Y)
    m2.assert_no_int_change('editor_loaded', 0, window=1.0)
    check('login gate: the dock is inert behind the login prompt (Notes never opens)', True)

    # Esc on the username field must not bypass the gate.
    m2.key('esc')
    time.sleep(0.5)
    check('esc on username field: still not logged in', m2.integer('auth_logged_in') == 0)
    m2.click_at(DOCK_NOTES_X, DOCK_NOTES_Y)
    m2.assert_no_int_change('editor_loaded', 0, window=0.6)
    check('esc on username field: does not drop to the desktop', True)

    # Esc on the password field must not bypass the gate either.
    m2.type(USERNAME)
    m2.key('ret')
    time.sleep(0.3)
    m2.key('esc')
    time.sleep(0.5)
    check('esc on password field: still not logged in', m2.integer('auth_logged_in') == 0)
    m2.click_at(DOCK_NOTES_X, DOCK_NOTES_Y)
    m2.assert_no_int_change('editor_loaded', 0, window=0.6)
    check('esc on password field: does not drop to the desktop', True)

    # Wrong password: rejected, and the real in-kernel delay is bracketed
    # by two window_present_count changes (the now-fixed rejection-message
    # frame, then the next frame once the fixed 1-second throttle in
    # auth_login_screen releases the loop), not a host-side guess.
    m2.type(USERNAME)
    m2.key('ret')
    time.sleep(0.3)
    m2.type('notrealpassword')
    # Capture the baseline only AFTER typing: every keystroke's own dot-echo
    # redraw also bumps window_present_count, so sampling it before typing
    # would bracket the last keystroke's redraw instead of the rejection
    # message. Enter itself presents nothing on its own (get_key_or_click
    # returns the moment it sees the key, before its next-iteration present
    # fallback), so the very next change after this baseline is really the
    # rejection message.
    pre = m2.integer('window_present_count')
    m2.key('ret')
    c_shown = m2.wait_present_change(pre)
    t_shown = time.time()
    m2.wait_present_change(c_shown)
    t_next = time.time()
    delay = t_next - t_shown
    check('wrong password: rejected (auth_logged_in stays 0)', m2.integer('auth_logged_in') == 0)
    check(f'wrong password: a real delay before the prompt returns was observed ({delay:.2f}s)', 0.3 <= delay <= 2.0)

    # Empty password: rejected too, no blank-field special case.
    m2.type(USERNAME)
    m2.key('ret')
    time.sleep(0.3)
    m2.key('ret')  # submit an empty password
    time.sleep(0.8)
    check('empty password: rejected (auth_logged_in stays 0)', m2.integer('auth_logged_in') == 0)

    # The real password logs in.
    m2.type(USERNAME)
    m2.key('ret')
    time.sleep(0.3)
    m2.type(PASSWORD1)
    m2.key('ret')
    m2.wait_int('auth_logged_in', lambda v: v == 1, 'Correct password did not log in', timeout=5)
    check('correct password: logged in (auth_logged_in == 1)', m2.integer('auth_logged_in') == 1)
    check('correct password: auth_current_user == "joshua"', m2.string('auth_current_user', 25) == USERNAME)

    m2.click_at(DOCK_NOTES_X, DOCK_NOTES_Y)
    m2.wait_int('editor_loaded', lambda v: (v & 0xff) == 1, 'Desktop did not become interactive after a real login', timeout=5)
    check('post-login: the desktop is interactive (Notes opens)', True)
    m2.key('esc')
    time.sleep(0.2)
    m2.key('esc')
    time.sleep(0.3)
    m2.screenshot('03-post-login-desktop')

    print('=== Phase 4: Settings -> Account, change password to "new pass 2" ===')
    open_settings(m2)
    click_settings_row(m2, ROW_ACCOUNT)
    time.sleep(0.4)
    m2.type(PASSWORD1)
    m2.key('ret')
    time.sleep(0.3)
    m2.type(PASSWORD2)
    m2.key('ret')
    time.sleep(0.3)
    m2.type(PASSWORD2)
    m2.key('ret')
    # Wait for the evidence itself, not a guessed duration: the rotated
    # line has to be hashed (a deliberately slow iterated SHA-256) and
    # written through FAT before the image is read back, and a fixed 1 s
    # was enough on a laptop but not on a loaded CI runner, where this
    # phase failed twice in a row with the write still in flight. The
    # raw image is readable while QEMU runs (cache=writeback lands guest
    # writes in the host page cache), so poll it for a new "joshua" line.
    deadline = time.time() + 20
    while time.time() < deadline:
        found = re.findall(rb'joshua:p2:\d+:([0-9a-f]{32}):([0-9a-f]{64})', DISK.read_bytes())
        if any(h != original_hash for _, h in found):
            break
        time.sleep(0.2)
    time.sleep(1.0)  # in-kernel status message + its delay, before tearing the process down
finally:
    m2.close()

raw = DISK.read_bytes()
matches = re.findall(rb'joshua:p2:\d+:([0-9a-f]{32}):([0-9a-f]{64})', raw)
# Unlike the very first write (Phase 2, where a single "joshua" line is the
# only one that can possibly exist), a raw grep here can legitimately also
# still see the OLD hash: replacing a file on this FAT filesystem unlinks
# its old cluster from the directory entry but does not zero the bytes
# still sitting in what is now free space, ordinary filesystem behaviour
# (docs/THREAT-MODEL.md already disclaims any disk-level secure-delete
# guarantee) and not a plaintext leak. What matters is that the real new
# hash actually made it to disk and differs from the original.
new_hashes = {h for _, h in matches if h != original_hash}
check('password change: a new "joshua" line with a rotated salt/hash was written to disk', len(new_hashes) >= 1)
check('USERS.TXT after password change: plaintext "hidden valley 1" never appears anywhere', b'hidden valley 1' not in raw)
check('USERS.TXT after password change: plaintext "new pass 2" never appears anywhere', b'new pass 2' not in raw)

print('=== Phase 5: reboot again -> old password rejected, new password accepted ===')
m3 = Machine(DISK)
try:
    check('second reboot: account still persisted', m3.integer('auth_user_count') == 1)

    m3.type(USERNAME)
    m3.key('ret')
    time.sleep(0.3)
    m3.type(PASSWORD1)  # the OLD password
    m3.key('ret')
    time.sleep(0.8)
    check('old password rejected after the change', m3.integer('auth_logged_in') == 0)

    m3.type(USERNAME)
    m3.key('ret')
    time.sleep(0.3)
    m3.type(PASSWORD2)  # the NEW password
    m3.key('ret')
    m3.wait_int('auth_logged_in', lambda v: v == 1, 'New password did not log in', timeout=5)
    check('new password accepted', m3.integer('auth_logged_in') == 1)
    check('new password login: auth_current_user == "joshua"', m3.string('auth_current_user', 25) == USERNAME)
finally:
    m3.close()

print()
print('PASS: fresh boot has no gate; Settings creates a real account persisted to USERS.TXT with no plaintext; '
      'reboot enforces a real login screen (esc on either field and an empty password never bypass it, a wrong '
      'password is rejected with a real measured delay); Settings rotates the password; a second reboot rejects '
      'the old password and accepts the new one, still with no plaintext ever on disk')
print(f'Artifacts: {WORKDIR}')
