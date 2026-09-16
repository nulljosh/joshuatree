#!/usr/bin/env python3
"""Real regression guard for the v0.76.13 fix: the idle tour's own
demoSatelliteWallpaper() step in landing/v86/embed.js used to click the
Settings Wallpaper row 3 times, written back when kernel.c's wall_theme
defaulted to WALL_WARM(1) so 3 forward clicks correctly reached
Satellite(4). v0.76.7 flipped the real kernel default to WALL_SAT(4)
without updating that click count -- starting already at Satellite(4),
3 more forward clicks land on 4->0(Photo)->1(Warm)->2(Cool), leaving the
demo's own desktop on the Cool map theme, not Satellite, every single
lap since. This is exactly why "no satellite wallpaper" kept reproducing
after the CORS proxy fix already made the underlying fetch work: the
demo was silently clicking itself off Satellite every lap.

This boots the real kernel, drives the exact real click sequence the
current (fixed) embed.js performs -- open the Apple menu, open Settings,
dwell, Escape, with NO wallpaper-row clicks at all -- and reads the real
`wall_theme` global straight out of kernel memory afterward (the same
"read the symbol, don't trust the source" technique
walldefault-check.sh/editor_qa.py already use), asserting it is still
WALL_SAT(4).

Discriminating, verified with a real, reliable repro of the exact
regression math: with Settings open, driving `sel` to the Wallpaper row
via two Down-arrow presses then pressing 'd' (the same tap-forward
`wall_switch_theme((wall_theme + dir + 5) % 5)` line a real click on that
row also runs, per kernel.c's own `sel == 2` branch -- the cycling logic
is identical regardless of how `sel` got there) 3 times in a row
reproduces the exact old bug: wall_theme goes 4->0->1->2, landing on Cool,
not Satellite. That's the real, confirmed root cause. (A live QMP mouse
click directly on the Wallpaper row's own coordinates, to reproduce the
OLD embed.js's literal input method, did not register at all in this
harness during development -- a separate, unconfirmed question about
gui_launch_settings' click hit-testing under a synthetic QMP pointer,
logged in roadmap.md as an open item, and irrelevant to this fix since
the real fix removes those clicks entirely rather than depending on them
working.)

Usage: tools/checks/walldemo-regression-check.py
"""
import json, os, re, shutil, socket, subprocess, sys, tempfile, time

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..')
os.chdir(ROOT)
LOGICAL_W, LOGICAL_H = 960, 540
PORT = 4462
LOGO_X, LOGO_Y = 16, 13
SETTINGS_MENU_X, SETTINGS_MENU_Y = 94, 111

nm = shutil.which('nm') or 'nm'
symbols = {}
for line in subprocess.check_output([nm, 'kernel.elf'], text=True).splitlines():
    fields = line.split()
    if len(fields) == 3:
        symbols[fields[2]] = int(fields[0], 16) - 0xC0000000
if 'wall_theme' not in symbols:
    print('FAIL: wall_theme symbol not found in kernel.elf')
    sys.exit(1)
WALL_THEME_ADDR = symbols['wall_theme']

q = subprocess.Popen(
    ['qemu-system-i386', '-kernel', 'kernel.elf', '-display', 'none', '-vga', 'std',
     '-qmp', f'tcp:127.0.0.1:{PORT},server,nowait'],
    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
try:
    s = None
    for _ in range(50):
        time.sleep(0.2)
        try:
            s = socket.create_connection(('127.0.0.1', PORT)); break
        except OSError:
            pass
    if s is None:
        print('FAIL: QEMU QMP socket never came up')
        sys.exit(1)
    f = s.makefile('rw')

    def cmd(o):
        f.write(json.dumps(o) + '\n'); f.flush()
        while True:
            r = json.loads(f.readline())
            if 'return' in r or 'error' in r:
                return r

    def monitor(command):
        return cmd({'execute': 'human-monitor-command', 'arguments': {'command-line': command}})

    def wall_theme():
        r = monitor(f'xp /4xb 0x{WALL_THEME_ADDR:x}')
        text = r['return']
        byte0 = re.findall(r'0x([0-9a-f]{2})\b', text.split(':', 1)[1])[0]
        return int(byte0, 16)

    def move(x, y):
        cmd({'execute': 'input-send-event', 'arguments': {'events': [
            {'type': 'abs', 'data': {'axis': 'x', 'value': int(x * 32768 / LOGICAL_W)}},
            {'type': 'abs', 'data': {'axis': 'y', 'value': int(y * 32768 / LOGICAL_H)}}]}})

    def click():
        cmd({'execute': 'input-send-event', 'arguments': {'events': [{'type': 'btn', 'data': {'down': True, 'button': 'left'}}]}})
        time.sleep(0.1)
        cmd({'execute': 'input-send-event', 'arguments': {'events': [{'type': 'btn', 'data': {'down': False, 'button': 'left'}}]}})

    def click_at(x, y):
        move(x, y); time.sleep(0.2)
        click(); time.sleep(0.3)

    f.readline()
    cmd({'execute': 'qmp_capabilities'})
    time.sleep(5.0)  # real desktop up

    before = wall_theme()
    print('wall_theme before opening Settings:', before)

    click_at(LOGO_X, LOGO_Y)      # opens the Apple menu
    time.sleep(0.3)
    click_at(SETTINGS_MENU_X, SETTINGS_MENU_Y)  # selects Settings
    time.sleep(2.0)  # real dwell, matches embed.js's own 2000ms -- no wallpaper-row clicks, the real fix
    cmd({'execute': 'send-key', 'arguments': {'keys': [{'type': 'qcode', 'data': 'esc'}]}})  # closes Settings
    time.sleep(0.5)

    after = wall_theme()
    print('wall_theme after the real demoSatelliteWallpaper() Settings visit:', after)

    try:
        cmd({'execute': 'quit'})
    except (ConnectionResetError, BrokenPipeError, OSError):
        pass
finally:
    try:
        q.wait(timeout=5)
    except subprocess.TimeoutExpired:
        q.kill()

if after == 4:
    print('PASS: Settings visit left wall_theme on Satellite (4), matching kernel.c\'s real default -- the demo no longer clicks itself away from it')
    sys.exit(0)
else:
    print(f'FAIL: wall_theme ended at {after}, expected 4 (Satellite) -- the demo\'s Settings visit changed the theme when it should not touch it at all')
    sys.exit(1)
