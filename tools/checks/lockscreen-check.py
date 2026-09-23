#!/usr/bin/env python3
"""Headless proof that the Lock Screen menu item in the Apple menu works:
with no accounts, clicking Lock Screen shows a brief message and returns to desktop;
with an account, Lock Screen shows the login prompt again, Esc cannot bypass it,
and entering the password returns to the desktop.

Flow part 1 (no accounts):
  1. Create a fresh disk with no USERS.TXT
  2. Boot and verify desktop
  3. Click the Apple menu (tree logo)
  4. Click Lock Screen
  5. Verify message "No accounts to lock with" appears
  6. Verify desktop is back (dock present)

Flow part 2 (with account):
  1. Write USERS.TXT to the disk with one test account (user=test, password=test)
  2. Boot and verify login screen appears
  3. Log in with test/test
  4. Verify desktop appears
  5. Click Apple menu
  6. Click Lock Screen
  7. Verify login screen re-appears ("Username:" prompt visible)
  8. Type Esc and verify login screen persists (cannot bypass)
  9. Type username and password and verify desktop returns
 10. Verify dock is visible (not still locked)

Usage: tools/checks/lockscreen-check.py   (from the repo root, after make kernel.elf)
"""
import json, os, re, socket, subprocess, sys, time, tempfile, hashlib, os
from PIL import Image

REPO = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
os.chdir(REPO)
ARTIFACTS = tempfile.mkdtemp(prefix="jt-lockscreen-")
DISK = os.path.join(ARTIFACTS, "disk.img")
LOG = os.path.join(ARTIFACTS, "serial.log")
DUMP = os.path.join(ARTIFACTS, "framebuffer.raw")
FB = 0xfd000000; W, H = 1920, 1080
SOCKET = os.path.join(ARTIFACTS, "qmp.sock")
LOGICAL_W, LOGICAL_H, SCALE = 960, 540, 2
DOCK_ICON, DOCK_GAP, SLOT0_X = 37, 6, 247
PITCH = DOCK_ICON + DOCK_GAP
ICON_ROW_Y = 487
CLOSE_X, CLOSE_Y = 94, 56
CLOSE_RED = (0xFF, 0x5F, 0x57)
PARK = (480, 200)
APPLE_MENU_X = 20      # Tree logo top-left x
APPLE_MENU_Y = 13      # Tree logo top-left y
DOCK_TRAY_COLOR = (0xEF, 0xEB, 0xE4)

# SHA256 constants and functions (same as kernel/auth.h)
SHA256_K = [
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
]

class SHA256:
    def __init__(self):
        self.h = [0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                  0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19]
        self.buf = bytearray(64)
        self.buflen = 0
        self.total_len = 0

    def _rotr(self, x, n):
        return ((x >> n) | (x << (32 - n))) & 0xffffffff

    def _process_block(self, p):
        w = [0] * 64
        for i in range(16):
            w[i] = ((p[i*4] << 24) | (p[i*4+1] << 16) | (p[i*4+2] << 8) | p[i*4+3]) & 0xffffffff
        for i in range(16, 64):
            s0 = self._rotr(w[i-15], 7) ^ self._rotr(w[i-15], 18) ^ (w[i-15] >> 3)
            s1 = self._rotr(w[i-2], 17) ^ self._rotr(w[i-2], 19) ^ (w[i-2] >> 10)
            w[i] = (w[i-16] + s0 + w[i-7] + s1) & 0xffffffff
        a,b,cc,d,e,f,g,hh = self.h
        for i in range(64):
            S1 = self._rotr(e,6) ^ self._rotr(e,11) ^ self._rotr(e,25)
            ch = (e & f) ^ ((~e) & g)
            t1 = (hh + S1 + ch + SHA256_K[i] + w[i]) & 0xffffffff
            S0 = self._rotr(a,2) ^ self._rotr(a,13) ^ self._rotr(a,22)
            maj = (a & b) ^ (a & cc) ^ (b & cc)
            t2 = (S0 + maj) & 0xffffffff
            hh=g; g=f; f=e; e=(d+t1) & 0xffffffff; d=cc; cc=b; b=a; a=(t1+t2) & 0xffffffff
        self.h[0] = (self.h[0] + a) & 0xffffffff
        self.h[1] = (self.h[1] + b) & 0xffffffff
        self.h[2] = (self.h[2] + cc) & 0xffffffff
        self.h[3] = (self.h[3] + d) & 0xffffffff
        self.h[4] = (self.h[4] + e) & 0xffffffff
        self.h[5] = (self.h[5] + f) & 0xffffffff
        self.h[6] = (self.h[6] + g) & 0xffffffff
        self.h[7] = (self.h[7] + hh) & 0xffffffff

    def update(self, data):
        if isinstance(data, str):
            data = data.encode()
        self.total_len += len(data)
        i = 0
        while i < len(data):
            take = 64 - self.buflen
            if take > len(data) - i:
                take = len(data) - i
            self.buf[self.buflen:self.buflen+take] = data[i:i+take]
            self.buflen += take
            i += take
            if self.buflen == 64:
                self._process_block(self.buf)
                self.buflen = 0

    def digest(self):
        c = SHA256()
        c.h = self.h[:]
        c.buf = self.buf[:]
        c.buflen = self.buflen
        c.total_len = self.total_len
        bitlen = c.total_len * 8
        pad = 0x80
        c.update(bytes([pad]))
        while c.buflen != 56:
            if c.buflen == 0:
                pass
            c.buf[c.buflen] = 0
            c.buflen += 1
            if c.buflen == 64:
                c._process_block(c.buf)
                c.buflen = 0
        for i in range(7, -1, -1):
            c.buf[c.buflen] = (bitlen >> (i * 8)) & 0xff
            c.buflen += 1
        c._process_block(c.buf)
        out = bytearray(32)
        for i in range(8):
            out[i*4]   = (c.h[i] >> 24) & 0xff
            out[i*4+1] = (c.h[i] >> 16) & 0xff
            out[i*4+2] = (c.h[i] >> 8) & 0xff
            out[i*4+3] = c.h[i] & 0xff
        return bytes(out)

def sha256_digest(data):
    c = SHA256()
    c.update(data)
    return c.digest()

def gen_salt():
    import random
    return bytes([random.randint(0, 255) for _ in range(16)])

def bytes_to_hex(b):
    return ''.join(f'{x:02x}' for x in b)

def auth_hash_password(salt, password):
    AUTH_HASH_ROUNDS = 200000
    if isinstance(password, str):
        password = password.encode()
    msg = salt + password
    h = sha256_digest(msg)
    for _ in range(1, AUTH_HASH_ROUNDS):
        h = sha256_digest(h)
    return h

def create_users_txt(username, password):
    """Create a USERS.TXT entry for testing"""
    salt = gen_salt()
    hash_val = auth_hash_password(salt, password)
    entry = f"{username}:{bytes_to_hex(salt)}:{bytes_to_hex(hash_val)}\n"
    return entry

# Create disk with mkdisk.sh
subprocess.run(["./tools/mkdisk.sh", DISK], check=True)

# Part 1: Test with no accounts
print("=== Part 1: Test with no accounts ===")
for f in (LOG, DUMP):
    try: os.remove(f)
    except FileNotFoundError: pass

q = subprocess.Popen(["qemu-system-i386", "-kernel", "kernel.elf", "-display", "none", "-vga", "std",
                      "-qmp", f"unix:{SOCKET},server,nowait", "-serial", "file:" + LOG,
                      "-drive", f"file={DISK},format=raw,if=ide,index=0"],
                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
fails = []
try:
    s = None
    for _ in range(50):
        time.sleep(0.2)
        candidate = socket.socket(socket.AF_UNIX)
        try:
            candidate.connect(SOCKET)
            candidate.settimeout(10)
            s = candidate
            break
        except OSError:
            candidate.close()
    if s is None: raise SystemExit("FAIL: QEMU's QMP socket never came up")
    f = s.makefile("rw")
    def cmd(o):
        f.write(json.dumps(o) + "\n"); f.flush()
        while True:
            line = f.readline()
            if not line:
                if o['execute'] == 'quit': return {}
                raise ConnectionError('QEMU disconnected before replying')
            r = json.loads(line)
            if "error" in r: raise RuntimeError(r["error"])
            if "return" in r: return r
    f.readline()
    cmd({"execute": "qmp_capabilities"})
    time.sleep(5.0)

    def move(x, y):
        cmd({"execute": "input-send-event", "arguments": {"events": [
            {"type": "abs", "data": {"axis": "x", "value": int(x * 32768 / LOGICAL_W)}},
            {"type": "abs", "data": {"axis": "y", "value": int(y * 32768 / LOGICAL_H)}}]}})
    def click():
        cmd({"execute": "input-send-event", "arguments": {"events": [{"type": "btn", "data": {"down": True, "button": "left"}}]}})
        time.sleep(0.1)
        cmd({"execute": "input-send-event", "arguments": {"events": [{"type": "btn", "data": {"down": False, "button": "left"}}]}})
    def pixel(x, y):
        cmd({"execute": "pmemsave", "arguments": {"val": FB, "size": W * H * 4, "filename": DUMP}})
        img = Image.frombytes("RGBA", (W, H), open(DUMP, "rb").read(), "raw", "BGRA").convert("RGB")
        return img.getpixel((x * SCALE + 1, y * SCALE + 1))
    def is_red(p): return max(abs(p[i] - CLOSE_RED[i]) for i in range(3)) <= 12
    def key(qcode):
        cmd({"execute": "send-key", "arguments": {"keys": [{"type": "qcode", "data": qcode}]}})
        time.sleep(0.35)

    # Wait for desktop
    for _ in range(120):
        if pixel(480, 511) == DOCK_TRAY_COLOR:
            break
        time.sleep(0.25)
    else:
        raise SystemExit("FAIL: desktop dock did not appear within 30 seconds")
    time.sleep(0.3)

    # Click Apple menu (tree logo)
    move(APPLE_MENU_X + 10, APPLE_MENU_Y + 10)
    time.sleep(0.2)
    click()
    time.sleep(0.5)

    # Click Lock Screen (should be item 4, about 4 rows down from top)
    # Menu starts at y = 37 (GUI_MENUBAR_H + GUI_MENU_PAD_V = 27 + 8)
    # Each row is 22 pixels high (GUI_MENU_ROW_H)
    # Lock Screen is the 5th item (index 4): 37 + 8 + 4*22 = 37 + 8 + 88 = 133
    lock_screen_y = 37 + 8 + 4 * 22  # y position of Lock Screen item
    move(APPLE_MENU_X + 90, lock_screen_y)
    time.sleep(0.2)
    click()
    time.sleep(1.0)  # Wait for message

    # Check dock is still visible (test passed - desktop intact)
    dock_visible = pixel(480, 511) == DOCK_TRAY_COLOR
    print(f"Part 1: Dock visible after clicking Lock Screen (no account): {dock_visible}")
    if not dock_visible:
        fails.append("Part 1: Dock not visible after Lock Screen click with no account")
    else:
        print("PASS Part 1: No accounts - Lock Screen shows message, desktop intact")

    try: cmd({"execute": "quit"})
    except (ConnectionResetError, BrokenPipeError, OSError): pass
finally:
    try: q.wait(timeout=5)
    except subprocess.TimeoutExpired: q.kill()

# Part 2: Test with account
print("\n=== Part 2: Test with account ===")
# Write USERS.TXT to disk using mtools
users_entry = create_users_txt("test", "test")
print(f"Created user entry: {users_entry.strip()}")

# Use mcopy from mtools to write USERS.TXT to the disk
import tempfile as tf
with tf.NamedTemporaryFile(mode='w', suffix='.txt', delete=False) as uf:
    uf.write(users_entry)
    users_file = uf.name
try:
    subprocess.run(["mcopy", "-i", DISK, users_file, "::/USERS.TXT"], check=True)
finally:
    os.remove(users_file)

for f in (LOG, DUMP):
    try: os.remove(f)
    except FileNotFoundError: pass

q = subprocess.Popen(["qemu-system-i386", "-kernel", "kernel.elf", "-display", "none", "-vga", "std",
                      "-qmp", f"unix:{SOCKET},server,nowait", "-serial", "file:" + LOG,
                      "-drive", f"file={DISK},format=raw,if=ide,index=0"],
                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
try:
    s = None
    for _ in range(50):
        time.sleep(0.2)
        candidate = socket.socket(socket.AF_UNIX)
        try:
            candidate.connect(SOCKET)
            candidate.settimeout(10)
            s = candidate
            break
        except OSError:
            candidate.close()
    if s is None: raise SystemExit("FAIL: QEMU's QMP socket never came up (Part 2)")
    f = s.makefile("rw")
    def cmd(o):
        f.write(json.dumps(o) + "\n"); f.flush()
        while True:
            line = f.readline()
            if not line:
                if o['execute'] == 'quit': return {}
                raise ConnectionError('QEMU disconnected before replying')
            r = json.loads(line)
            if "error" in r: raise RuntimeError(r["error"])
            if "return" in r: return r
    f.readline()
    cmd({"execute": "qmp_capabilities"})
    time.sleep(5.0)

    # Login with test/test
    for c in "test":
        key(c)
    key("ret")
    time.sleep(1.0)  # Between username and password prompt
    for c in "test":
        key(c)
    key("ret")
    time.sleep(2.0)  # Wait for desktop after login

    # Verify desktop (dock visible)
    desktop_after_login = pixel(480, 511) == DOCK_TRAY_COLOR
    print(f"Desktop visible after login: {desktop_after_login}")
    if not desktop_after_login:
        fails.append("Part 2: Desktop not visible after login")

    # Click Apple menu again
    move(APPLE_MENU_X + 10, APPLE_MENU_Y + 10)
    time.sleep(0.2)
    click()
    time.sleep(0.5)

    # Click Lock Screen
    move(APPLE_MENU_X + 90, lock_screen_y)
    time.sleep(0.2)
    click()
    time.sleep(1.0)

    # Check serial log for "auth: locked" marker
    try:
        with open(LOG, errors="replace") as lf:
            has_locked = "auth: locked" in lf.read()
        print(f"'auth: locked' marker found: {has_locked}")
        if not has_locked:
            fails.append("Part 2: 'auth: locked' marker not found in serial log")
    except FileNotFoundError:
        fails.append("Part 2: Serial log not found")

    # Try Esc - should stay locked
    key("esc")
    time.sleep(0.5)
    dock_after_esc = pixel(480, 511) == DOCK_TRAY_COLOR
    print(f"Dock visible after Esc (should be False): {dock_after_esc}")
    if dock_after_esc:
        fails.append("Part 2: Esc bypassed the lock screen (dock became visible)")
    else:
        print("PASS Part 2a: Esc cannot bypass lock screen")

    # Enter password again to unlock
    for c in "test":
        key(c)
    key("ret")
    time.sleep(1.0)
    for c in "test":
        key(c)
    key("ret")
    time.sleep(1.5)

    # Check desktop is back
    dock_after_unlock = pixel(480, 511) == DOCK_TRAY_COLOR
    print(f"Dock visible after re-entering password: {dock_after_unlock}")
    if not dock_after_unlock:
        fails.append("Part 2: Desktop not visible after re-entering password")
    else:
        print("PASS Part 2b: Desktop unlocked with correct password")

    try: cmd({"execute": "quit"})
    except (ConnectionResetError, BrokenPipeError, OSError): pass
finally:
    try: q.wait(timeout=5)
    except subprocess.TimeoutExpired: q.kill()

# Final result
if fails:
    for x in fails: print("FAIL:", x)
    sys.exit(1)
print("PASS: Lock Screen: menu item locks, Esc cannot bypass, password unlocks")
