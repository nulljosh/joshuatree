#!/usr/bin/env python3
"""Auto lock on the Pi (13.24.0: screen_idle.h decides, login_ui.h draws the card and checks the password).

QEMU raspi4b, a card whose JT/USERS.TXT holds ann and bob, and the LOCK_SELFTEST build's made-up keys (QEMU's raspi4b has
no keyboard). Each step prints a `lockself:` line; a screendump follows it.
  1. ann is signed in (straight from the login screen: the form needs the RNG200, which QEMU's raspi4b lacks; the form is
     arm64-login-check.py's): the screendump is her desktop. Every password after that is checked for real.
  2. The saver starts and the screen locks (`screen: saver on`, `screen: locked`): the screendump is dark, not the desktop.
  3. A key wakes it to the lock card: a cream card on her wallpaper, not the desktop. The key itself is not typed: it is
     the h of hunter22, so "unter22" after it must be wrong. A square drawn into fb while locked is not on screen.
  4. bob's right password is wrong here, a wrong one is wrong, and a mouse move changes nothing: still the card.
  5. Her password unlocks: her desktop exactly as it was, plus the square drawn while it was locked.
  No typed password ever reaches the UART. The state machine and its broken copies are in arm64-screen-idle-check.py.
Skips (exit 0) without clang, ld.lld, mtools, or a qemu-system-aarch64 with the raspi4b model.
"""
import hashlib, json, os, re, shutil, socket, subprocess, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from scratch import scratch_dir
from sdcard import PART, build_card

root = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
arch = os.path.join(root, "arch", "arm64")
if not all(shutil.which(t) for t in ("clang", "ld.lld", "qemu-system-aarch64", "mmd", "mcopy")):
    print("SKIP: clang, ld.lld, qemu-system-aarch64 or mtools not installed"); sys.exit(0)
if "raspi4b" not in subprocess.run(["qemu-system-aarch64", "-machine", "help"], capture_output=True, text=True).stdout:
    print("SKIP: QEMU here has no raspi4b model (needs QEMU 9 or newer)"); sys.exit(0)
b = subprocess.run(["make", "-s", "-C", arch, "lock-kernel8.img"], capture_output=True, text=True, timeout=900)
if b.returncode: print("FAIL: make lock-kernel8.img\n" + (b.stdout + b.stderr)[-1500:]); sys.exit(1)

tmp = scratch_dir("arm64-lock")
def record(name, pw, salt): return "%s:p2:100000:%s:%s\n" % (name, salt.hex(), hashlib.pbkdf2_hmac("sha256", pw, salt, 100000, 32).hex())
card = os.path.join(tmp, "lock.img"); build_card(card, False)
MT = dict(os.environ, MTOOLS_SKIP_CHECK="1")
def mt(*a): return subprocess.run([a[0], "-i", f"{card}@@{PART * 512}", *a[1:]], capture_output=True, env=MT)
users = os.path.join(tmp, "users")
open(users, "w").write(record("ann", b"hunter22", bytes(range(32, 48))) + record("bob", b"bobpass99", bytes(range(48, 64))))
if mt("mmd", "::JT").returncode or mt("mcopy", users, "::JT/USERS.TXT").returncode: print("FAIL: mtools could not write JT/USERS.TXT"); sys.exit(1)

log, sock = os.path.join(tmp, "uart"), os.path.join(tmp, "qmp")
for p in (log, sock):
    if os.path.exists(p): os.remove(p)
q = subprocess.Popen(["qemu-system-aarch64", "-machine", "raspi4b", "-display", "none", "-serial", "null", "-serial", "file:" + log,
                      "-qmp", "unix:%s,server,nowait" % sock,
                      "-drive", f"if=none,id=card,file={card},format=raw", "-device", "sd-card,drive=card,bus=/generic-sdhci/sd-bus",
                      "-kernel", os.path.join(arch, "lock-kernel8.img")], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
def uart(): return open(log, errors="replace").read() if os.path.exists(log) else ""
def wait_for(text, secs):
    end = time.monotonic() + secs
    while time.monotonic() < end:
        if text in uart(): return True
        if q.poll() is not None: break
        time.sleep(0.1)
    print("FAIL: no `%s` in time\n%s" % (text, uart()[-800:])); q.kill(); sys.exit(1)
f = None
def shot(tag):
    path = os.path.join(tmp, tag + ".ppm")
    f.write(json.dumps({"execute": "screendump", "arguments": {"filename": path}}) + "\n"); f.flush()
    while "return" not in (r := json.loads(f.readline())): assert "error" not in r, r
    d = open(path, "rb").read(); m = re.match(rb"P6\s+(\d+)\s+(\d+)\s+\d+\s", d)
    return int(m.group(1)), int(m.group(2)), d[m.end():]
def px(s, x, y): w, _, d = s; i = (y * w + x) * 3; return tuple(d[i:i + 3])
def near(c, want, tol=10): return all(abs(a - b) <= tol for a, b in zip(c, want))
def differ(a, b, skip=None):   # the share of pixels that differ, outside the skip box (x0, y0, x1, y1)
    w, h, da = a; db = b[2]; n = diff = 0
    for y in range(0, h, 4):
        for x in range(0, w, 4):
            if skip and skip[0] <= x < skip[2] and skip[1] <= y < skip[3]: continue
            i = (y * w + x) * 3; n += 1; diff += da[i:i + 3] != db[i:i + 3]
    return diff / n
fails = []
def check(name, ok, detail=""):
    print(("PASS: " if ok else "FAIL: ") + name + ("" if ok else "  " + detail), flush=True)
    if not ok: fails.append(name)
try:
    wait_for("login: screen up", 90)
    for _ in range(100):
        try: s = socket.socket(socket.AF_UNIX); s.connect(sock); break
        except OSError: time.sleep(0.1)
    f = s.makefile("rw"); f.readline(); f.write('{"execute": "qmp_capabilities"}\n'); f.flush(); f.readline()
    wait_for("lockself: desktop", 120); A = shot("desktop"); W, H = A[0], A[1]
    check("ann is signed in", "login: signed in ann" in uart())
    wait_for("lockself: saver", 30); S = shot("saver")
    check("the saver starts and the screen locks", "screen: saver on" in uart() and "screen: locked" in uart(), uart()[-400:])
    check("... and the saver hides the desktop", differ(A, S) > 0.3)
    wait_for("lockself: card", 30); C = shot("card")
    check("a key wakes it to the lock card, not the desktop", differ(A, C) > 0.3 and near(px(C, W // 2, H // 2 - 20), (0xfa, 0xf8, 0xf4)),
          "differ %.2f, centre %s" % (differ(A, C), px(C, W // 2, H // 2 - 20)))
    check("... and the square drawn into fb while locked is not on screen", not near(px(C, 32, H - 32), (255, 0, 255), 30))
    wait_for("lockself: unter", 60)
    check("the key that woke it was not typed: \"unter22\" is wrong", "screen: unlocked" not in uart() and uart().count("lock: wrong password") == 1, uart()[-400:])
    wait_for("lockself: bob", 60)
    check("bob's right password does not open ann's desktop", "screen: unlocked" not in uart() and uart().count("lock: wrong password") == 2)
    wait_for("lockself: wrong", 60); C2 = shot("wrong")
    check("a wrong password and a mouse move leave the card up", "screen: unlocked" not in uart() and uart().count("lock: wrong password") == 3
          and differ(C, C2) < 0.02, "differ %.3f" % differ(C, C2))
    wait_for("lockself: hers", 60); D = shot("unlocked")
    check("her password unlocks", "screen: unlocked" in uart())
    check("... to her desktop exactly as it was", differ(A, D, (0, H - 64, 64, H)) < 0.005, "differ %.4f" % differ(A, D, (0, H - 64, 64, H)))
    check("... with what was drawn while locked", near(px(D, 32, H - 32), (255, 0, 255), 30), str(px(D, 32, H - 32)))
    check("no typed password ever reached the UART", not any(p in uart() for p in ("hunter22", "unter22", "bobpass99", "wrongpass1")))
    check("the login form was never used for the lock", "login: wrong password" not in uart())
finally:
    if f: f.close()
    q.terminate()
    try: q.wait(timeout=5)
    except subprocess.TimeoutExpired: q.kill(); q.wait()
if fails: print("FAIL: %d lock check(s) failed" % len(fails)); sys.exit(1)
print("PASS: auto lock on raspi4b: the saver locks, a key wakes the card and is not typed, only her password opens it, and her desktop comes back exactly, with what was drawn meanwhile")
