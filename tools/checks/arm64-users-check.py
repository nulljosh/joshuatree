#!/usr/bin/env python3
"""ARM64 Settings, Users pane (arch/arm64/users_ui.h, cmd.c users_add / users_delete / users_passwd; 4.14.0).

Settings has a sixth section, Users, that manages accounts with the code the Terminal's adduser, deluser, passwd, makeadmin and
unadmin run, and asks the signed-in admin for a password through sudo_check, so the same remembered five minutes apply.
QEMU virt, headless, ramfb, a virtio keyboard and tablet and the check card (sdcard.py) holding ann (admin) and bob (normal),
their hashes made here with hashlib. Everything is driven with keys and clicks; the UART lines `settings users ...` say what
happened (names only, never a password).

  1. Admin (ann): the sixth sidebar tile, the list (accounts, You and Admin marks, Add user), a click on Add user opens the
     form; a second password that differs is refused and the fields clear; the same password twice asks ann's password
     (the auth view); a wrong one is refused and nothing is added; the right one adds cy. The password fields are stars: the
     same row painted with two different passwords of one length is the same picture. makeadmin on bob (the password is
     remembered: no second prompt), a reset of bob's password by an admin (no old one), a delete with a confirm step that
     cancels once and then removes cy.
  2. Persistence: the card holds ann and bob and not cy, SUDOERS.TXT lists ann and bob, bob's hash is PBKDF2 of the reset
     password; a reboot lets bob sign in with it.
  3. A normal user (bob, a second card): the list shows, ann's page, Add user and every admin-only row are refused, and he
     changes his own password, which needs the old one (a wrong old one is refused, the right one works).
  4. No password reached the UART: no typed secret in the log, no `key N` line while a password view was up, and the key codes
     of the whole log do not spell any password.
  5. Mutation: the pane made to skip sudo_check must FAIL the wrong-password step in QEMU (restored afterwards).
Set JT_USERS_SHOT to a .png path to keep the list's screen. Skips (exit 0) when the tools are missing.
"""
import hashlib, json, os, re, shutil, socket, struct, subprocess, sys, time, zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from scratch import scratch_dir
from sdcard import PART, build_card

root = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
arch = os.path.join(root, "arch", "arm64")
if not all(shutil.which(t) for t in ("clang", "ld.lld", "qemu-system-aarch64", "mmd", "mcopy", "mtype")):
    print("SKIP: clang, ld.lld, qemu-system-aarch64 or mtools not installed"); sys.exit(0)
fails = []


def check(name, ok, detail=""):
    print(("  ok: " if ok else "  FAIL: ") + name + ("" if ok or not detail else " -- " + str(detail)[-500:]), flush=True)
    if not ok: fails.append(name)


def build(*args):
    subprocess.run(["make", "-C", arch, "clean"], capture_output=True)
    b = subprocess.run(["make", "-C", arch, *args], capture_output=True, text=True, timeout=900)
    if b.returncode: print("FAIL: make -C arch/arm64 %s failed\n" % " ".join(args) + (b.stdout + b.stderr)[-1500:]); sys.exit(1)


tmp = scratch_dir("arm64-users")
MT = dict(os.environ, MTOOLS_SKIP_CHECK="1")
def mt(card, *a): return subprocess.run([a[0], "-i", f"{card}@@{PART * 512}", *a[1:]], capture_output=True, env=MT)
def record(name, pw, salt_byte):
    salt = bytes(range(salt_byte, salt_byte + 16))
    return "%s:p2:100000:%s:%s\n" % (name, salt.hex(), hashlib.pbkdf2_hmac("sha256", pw.encode(), salt, 100000, 32).hex())
def make_card(tag, users, sudoers):
    card = os.path.join(tmp, tag + ".img")
    build_card(card, False)
    if mt(card, "mmd", "::JT").returncode: print("FAIL: mtools could not make JT/"); sys.exit(1)
    for name, text in (("USERS.TXT", users), ("SUDOERS.TXT", sudoers)):
        f = os.path.join(tmp, tag + "." + name); open(f, "w").write(text)
        if mt(card, "mcopy", f, "::JT/" + name).returncode: print("FAIL: mtools could not write " + name); sys.exit(1)
    return card
def card_file(card, name): return mt(card, "mtype", "::JT/" + name).stdout.decode(errors="replace")

W, H = 800, 600
WX, WY = (W - 500) // 2, 100
CX, CY = WX + 8 + 150 + 16, WY + 74            # the card's top left, as settings_ui.h draws it
ROW = 30
def row_xy(r): return CX + 150, CY + r * ROW + 15
SIDE_USERS = (WX + 60, WY + 40 + 5 * 30 + 6)
QK = {" ": "spc", "-": "minus"}
EVDEV = {**{c: "1234567890"[c - 2] for c in range(2, 12)}, 12: "-", 57: " ",
         **{16 + i: ch for i, ch in enumerate("qwertyuiop")}, **{30 + i: ch for i, ch in enumerate("asdfghjkl")}, **{44 + i: ch for i, ch in enumerate("zxcvbnm")}}
def keystream(uart): return "".join(EVDEV.get(int(m), "?") for m in re.findall(r"^key (\d+) down$", uart, re.M))
SECRETS = ["hunter22", "pass1111", "pass1112", "nopenope", "newpass1", "wrongold1", "newbob111", "bobpass44", "starsaaa1", "starszzz9"]
SILENT = ("settings users view add", "settings users view passwd", "settings users view auth")
OUT_OF_SECRET = ("settings users view list", "settings users view detail", "settings users view confirm", "settings users view sidebar")


class Session:
    def __init__(self, tag, card, kernel="kernel8.elf"):
        self.tag = tag; self.shots = 0
        self.log, sock = os.path.join(tmp, tag + ".uart"), os.path.join(tmp, tag + ".qmp")
        self.q = subprocess.Popen(["qemu-system-aarch64", "-machine", "virt", "-cpu", "cortex-a72", "-m", "256", "-global", "virtio-mmio.force-legacy=false",
                                   "-nic", "none", "-device", "ramfb", "-device", "virtio-keyboard-device", "-device", "virtio-tablet-device",
                                   "-drive", "if=none,id=card,file=%s,format=raw" % card, "-device", "virtio-blk-device,drive=card",
                                   "-display", "none", "-serial", "file:" + self.log, "-qmp", "unix:%s,server,nowait" % sock,
                                   "-kernel", os.path.join(arch, kernel)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.sock, self.f = sock, None
    def uart(self): return open(self.log, errors="replace").read() if os.path.exists(self.log) else ""
    def wait_for(self, text, secs=10.0, after=0):
        end = time.time() + secs
        while time.time() < end:
            if text in self.uart()[after:]: return True
            time.sleep(0.05)
        return False
    def wait_any(self, texts, secs, after=0):
        end = time.time() + secs
        while time.time() < end:
            u = self.uart()[after:]
            for t in texts:
                if t in u: return t
            time.sleep(0.05)
        return None
    def connect(self):
        s_ = socket.socket(socket.AF_UNIX); s_.settimeout(120)
        for _ in range(100):
            try: s_.connect(self.sock); break
            except OSError: time.sleep(0.1)
        self.f = s_.makefile("rw"); self.f.readline(); self.cmd("qmp_capabilities")
    def cmd(self, c, **a):
        self.f.write(json.dumps({"execute": c, "arguments": a}) + "\n"); self.f.flush()
        while True:
            r = json.loads(self.f.readline())
            if "return" in r or "error" in r: return r
    def key(self, *qcodes): self.cmd("send-key", keys=[{"type": "qcode", "data": k} for k in qcodes]); time.sleep(0.15)
    def typ(self, text):
        for ch in text: self.key(QK.get(ch, ch))
    def step(self, qcode, want, secs=10):   # a key, then the UART line it must cause
        n = len(self.uart()); self.key(qcode); return self.wait_for(want, secs, n)
    def line(self, text, want, secs=10):    # type text, then Return, then the UART line it must cause
        self.typ(text); return self.step("ret", want, secs)
    def click(self, x, y, want, secs=10):
        n = len(self.uart())
        ax, ay = int((x + 0.5) * 32768 / W), int((y + 0.5) * 32768 / H)
        self.cmd("input-send-event", events=[{"type": "abs", "data": {"axis": "x", "value": ax}}, {"type": "abs", "data": {"axis": "y", "value": ay}}])
        time.sleep(0.2)
        self.cmd("input-send-event", events=[{"type": "btn", "data": {"down": True, "button": "left"}}])
        self.cmd("input-send-event", events=[{"type": "btn", "data": {"down": False, "button": "left"}}])
        return self.wait_for(want, secs, n)
    def shot(self):
        path = os.path.join(tmp, "%s%d.ppm" % (self.tag, self.shots)); self.shots += 1
        self.cmd("screendump", filename=path)
        for _ in range(60):
            if os.path.exists(path) and os.path.getsize(path) >= W * H * 3: break
            time.sleep(0.1)
        parts = open(path, "rb").read().split(b"\n", 3)
        return (int(parts[1].split()[0]), parts[3])
    def boot(self, login):
        if not self.wait_for("M2 input ready", 90) or not self.wait_for("sd fat32 partition at block 2048", 40): return False
        self.connect(); time.sleep(0.5)
        if not self.wait_for("login: screen up", 40): return False
        n = len(self.uart())
        self.typ(login[0]); self.key("ret"); self.typ(login[1]); self.key("ret")
        if not self.wait_for("login: desktop up", 150, n): return False
        time.sleep(0.5)
        return True
    def users_pane(self):   # Spotlight, Settings, down to Users, into the pane
        self.key("f1"); self.typ("sett")
        if not self.step("ret", "settings open\n", 10): return False
        for i, name in enumerate(["Display", "Sound", "Dock", "Date and Time", "Users"], 1):
            if not self.step("down", "settings section %d %s\n" % (i, name)): return False
        time.sleep(0.4)
        return True
    def close(self):
        if self.f: self.f.close()
        if self.q.poll() is None: self.q.kill()
        self.q.wait()


def px(img, x, y): i = (y * img[0] + x) * 3; return tuple(img[1][i:i + 3])
def box(img, x, y, w, h): return [px(img, i, j) for j in range(y, y + h) for i in range(x, x + w)]
def png(img, path):
    rows = b"".join(b"\0" + img[1][y * img[0] * 3:(y + 1) * img[0] * 3] for y in range(len(img[1]) // (img[0] * 3)))
    def chunk(t, d): c = struct.pack(">I", len(d)) + t + d; return c + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    open(path, "wb").write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", img[0], len(img[1]) // (img[0] * 3), 8, 2, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(rows, 6)) + chunk(b"IEND", b""))


def leaks(uart):
    """Every way a password could reach the UART: its text, its key codes, or any key line inside a password view."""
    bad = [s for s in SECRETS if s in uart]
    bad += ["keys:" + s for s in SECRETS if s in keystream(uart)]
    inside = False
    for ln in uart.split("\n"):
        if ln.startswith(SILENT): inside = True
        elif ln.startswith(OUT_OF_SECRET) or ln.startswith("settings closed"): inside = False
        elif inside and ln.startswith("key ") and int(ln.split()[1]) < 256: bad.append("key line inside a password view: " + ln)   # 272 is the mouse button
    return bad


ANN, BOB = ("ann", "hunter22"), ("bob", "bobpass44")
USERS = record("ann", ANN[1], 32) + record("bob", BOB[1], 64)

build("kernel8.elf")
logs = []

# ---- 1. the admin ----
cardA = make_card("a", USERS, "ann\n")
S = Session("a", cardA)
try:
    if not S.boot(ANN) or not S.users_pane(): check("ann signs in and Settings opens at Users", False, S.uart()[-700:]); raise SystemExit
    check("Settings has six sidebar sections and Down from Date and Time reaches Users", "settings section 5 Users\n" in S.uart())
    a = S.shot()
    check("Users' sidebar tile is terracotta with a glyph", sum(1 for p in box(a, WX + 20, WY + 40 + 5 * 30, 18, 18) if p == (0xb5, 0x50, 0x2c)) > 100 and sum(1 for p in box(a, WX + 20, WY + 40 + 5 * 30, 18, 18) if sum(abs(p[k] - (0xfa, 0xf8, 0xf4)[k]) for k in range(3)) < 40) > 5)
    check("Return enters the pane", S.step("ret", "settings users view list\n"))
    time.sleep(0.4)
    lst = S.shot()
    keep = os.environ.get("JT_USERS_SHOT")
    if keep:
        os.makedirs(os.path.dirname(keep), exist_ok=True); png(lst, keep); print("  (screenshot kept at %s)" % keep)
    check("the card is white with three rows (ann, bob, Add user) and a hairline between them", px(lst, CX + 5, CY + 5) == (255, 255, 255) and px(lst, CX + 100, CY + ROW) == (0xe8, 0xe2, 0xd8) and px(lst, CX + 100, CY + 2 * ROW) == (0xe8, 0xe2, 0xd8) and px(lst, CX + 5, CY + 3 * ROW + 2) != (255, 255, 255))
    check("ann's row says You, admin and bob's says nothing (grey text at the right edge of row 0, none on row 1)",
          sum(1 for p in box(lst, CX + 220, CY + 5, 90, 20) if max(p) < 0xB0) > 15 and sum(1 for p in box(lst, CX + 220, CY + ROW + 5, 90, 20) if max(p) < 0xB0) == 0)
    # the mouse: a click on Add user opens the form
    check("a click on Add user opens the form", S.click(*row_xy(2), "settings users view add\n"))
    time.sleep(0.3)
    # name, then a mismatch
    S.typ("cy")
    S.key("ret")
    S.typ("pass1111"); S.key("ret")
    n = len(S.uart())
    S.typ("pass1112"); S.key("ret")
    check("a second password that differs is refused", S.wait_for("settings users refused mismatch\n", 10, n))
    time.sleep(0.4)
    check("... and nothing was added (still two accounts on the card)", card_file(cardA, "USERS.TXT").count("\n") == 2, card_file(cardA, "USERS.TXT")[-200:])
    # stars: the same row, two different passwords of one length, the same picture
    S.typ("starsaaa1"); time.sleep(0.4); s1 = S.shot()
    for _ in range(9): S.key("backspace")
    S.typ("starszzz9"); time.sleep(0.4); s2 = S.shot()
    r1, r2 = box(s1, CX + 100, CY + ROW + 3, 200, 24), box(s2, CX + 100, CY + ROW + 3, 200, 24)
    for _ in range(9): S.key("backspace")
    time.sleep(0.4); s0 = S.shot(); r0 = box(s0, CX + 100, CY + ROW + 3, 200, 24)
    check("a typed password is stars: nine letters and nine other letters paint the same pixels, and an empty field paints different ones", r1 == r2 and r1 != r0 and sum(1 for p in r1 if max(p) < 0x80) > 20)
    # the right pair; no password is remembered yet, so the admin's is asked
    S.typ("pass1111"); S.key("ret")
    n = len(S.uart())
    S.typ("pass1111"); S.key("ret")
    check("a matching pair asks the admin's password (the auth view)", S.wait_for("settings users view auth\n", 10, n))
    n = len(S.uart())
    S.typ("nopenope"); S.key("ret")
    check("a wrong admin password is refused", S.wait_for("settings users refused wrong-password\n", 60, n))
    check("... and cy is not on the card", "cy:" not in card_file(cardA, "USERS.TXT"))
    n = len(S.uart())
    S.typ(ANN[1]); S.key("ret")
    ok = S.wait_for("settings users added cy\n", 200, n)
    check("ann's own password adds cy", ok, S.uart()[-500:])
    check("... and the card now has ann, bob and cy", card_file(cardA, "USERS.TXT").count("\n") == 3 and card_file(cardA, "USERS.TXT").count("cy:p2:") == 1)
    # makeadmin: sudo's remembered window means no second prompt
    n = len(S.uart())
    S.key("up")
    check("Return on bob opens his page", S.step("ret", "settings users view detail bob\n"))
    n = len(S.uart())
    S.key("down")
    ok = S.step("ret", "settings users admin on bob\n", 20)
    check("the Admin switch makes bob an admin with no second password (sudo's five minutes)", ok and "settings users view auth" not in S.uart()[n:], S.uart()[n:][-300:])
    check("... SUDOERS.TXT on the card lists ann and bob", card_file(cardA, "SUDOERS.TXT") == "ann\nbob\n", card_file(cardA, "SUDOERS.TXT"))
    # reset bob's password as an admin: no old password
    S.key("up")
    check("Change password opens the form", S.step("ret", "settings users view passwd bob\n"))
    S.typ("newpass1"); S.key("ret")
    S.typ("newpass1")
    check("an admin changes bob's password with no old one", S.step("ret", "settings users passwd bob\n", 60))
    # delete cy: a confirm, cancelled once
    S.key("down")
    check("Return on cy opens her page", S.step("ret", "settings users view detail cy\n"))
    S.key("down"); S.key("down")
    check("Remove user opens a confirm", S.step("ret", "settings users view confirm cy\n"))
    check("the confirm starts on Cancel: Return goes back and cy stays", S.step("ret", "settings users view detail cy\n") and "cy:p2:" in card_file(cardA, "USERS.TXT"))
    S.key("ret")
    S.key("up")
    check("Remove cy deletes the account", S.step("ret", "settings users deleted cy\n", 60))
    check("... USERS.TXT has ann and bob, not cy", "cy:" not in card_file(cardA, "USERS.TXT") and card_file(cardA, "USERS.TXT").count("\n") == 2)
    # Esc steps back out: list, then the sidebar, then closes
    check("Esc leaves the list for the sidebar", S.step("esc", "settings users view sidebar\n"))
    check("Esc closes Settings", S.step("esc", "settings closed\n"))
    check("no fault, no crash", "KERNEL CRASH" not in S.uart() and "oom" not in S.uart(), S.uart()[-300:])
    logs.append(S.uart())
except SystemExit:
    pass
finally:
    S.close()

# ---- 2. persistence ----
users_txt = card_file(cardA, "USERS.TXT")
bob_line = [l for l in users_txt.split("\n") if l.startswith("bob:")]
ok = False
if bob_line:
    f = bob_line[0].split(":")
    ok = hashlib.pbkdf2_hmac("sha256", b"newpass1", bytes.fromhex(f[3]), int(f[2]), 32).hex() == f[4]
check("the card holds bob's new password hash (PBKDF2 of the reset password with his stored salt) and no plain text", ok and "newpass1" not in users_txt)
S = Session("a2", cardA)
try:
    if S.boot(("bob", "newpass1")) and S.users_pane():
        n = len(S.uart())
        check("after a reboot bob signs in with the changed password and, now an admin, sees Users", S.step("ret", "settings users view list\n"))
        S.key("esc"); S.key("esc")
        logs.append(S.uart())
    else:
        check("after a reboot bob signs in with the changed password", False, S.uart()[-500:])
finally:
    S.close()

# ---- 3. a normal user ----
cardB = make_card("b", USERS, "ann\n")
S = Session("b", cardB)
try:
    if not S.boot(BOB): check("bob signs in", False, S.uart()[-500:]); raise SystemExit
    S.key("f1"); S.typ("sett")
    S.step("ret", "settings open\n")
    check("a click on the Users tile opens the section", S.click(*SIDE_USERS, "settings section 5 Users\n"))
    check("a normal user is refused another account's page (a click on ann's row)", S.click(*row_xy(0), "settings users refused not-admin\n"))
    n = len(S.uart())
    check("Add user is refused to a normal user", S.click(*row_xy(2), "settings users refused not-admin\n") and "settings users view add" not in S.uart()[n:])
    check("bob's own row opens his page", S.click(*row_xy(1), "settings users view detail bob\n"))
    time.sleep(0.5); two = S.shot()
    check("it has only Change password and Back (two rows, no Admin switch or Remove: white at row 1, not at row 2)", px(two, CX + 30, CY + ROW + 4) == (255, 255, 255) and px(two, CX + 30, CY + 2 * ROW + 4) != (255, 255, 255))
    check("Change password opens the form", S.click(*row_xy(0), "settings users view passwd bob\n"))
    S.typ("wrongold1"); S.key("ret"); S.typ("newbob111"); S.key("ret")
    n = len(S.uart())
    S.typ("newbob111"); S.key("ret")
    check("his own password needs the old one: a wrong one is refused", S.wait_for("settings users refused wrong-password\n", 60, n))
    n = len(S.uart())
    S.typ(BOB[1]); S.key("ret"); S.key("ret"); S.key("ret")
    check("the right old password changes it, and he never needed an admin", S.wait_for("settings users passwd bob\n", 120, n) and "view auth" not in S.uart()[n:])
    time.sleep(0.5)
    t = card_file(cardB, "USERS.TXT")
    bl = [l for l in t.split("\n") if l.startswith("bob:")][0].split(":")
    al = [l for l in t.split("\n") if l.startswith("ann:")][0]
    check("... on the card too", hashlib.pbkdf2_hmac("sha256", b"newbob111", bytes.fromhex(bl[3]), int(bl[2]), 32).hex() == bl[4] and al + "\n" in USERS)
    check("... and SUDOERS.TXT still lists only ann", card_file(cardB, "SUDOERS.TXT") == "ann\n")
    logs.append(S.uart())
    check("no fault, no crash", "KERNEL CRASH" not in S.uart(), S.uart()[-300:])
except SystemExit:
    pass
finally:
    S.close()

# ---- 4. no password on the UART ----
for i, u in enumerate(logs):
    bad = leaks(u)
    check("no password text, key code or key line reached the UART (log %d of %d, %d lines)" % (i + 1, len(logs), u.count("\n")), not bad, bad)

# ---- 5. mutation: the pane skips sudo_check (JT_USERS_NOMUT=1 skips it while developing) ----
if os.environ.get("JT_USERS_NOMUT"):
    print("(mutation skipped by JT_USERS_NOMUT)"); sys.exit(1 if fails else 0)
UI = os.path.join(arch, "users_ui.h")
LINE = "int r = sudo_check(current_user, us_auth);"
orig = open(UI).read()
assert LINE in orig
try:
    open(UI, "w").write(orig.replace(LINE, "int r = SUDO_OK; (void)sudo_check;"))
    build("kernel8.elf")
    cardM = make_card("m", USERS, "ann\n")
    M = Session("m", cardM)
    try:
        if not M.boot(ANN) or not M.users_pane() or not M.step("ret", "settings users view list\n"):
            check("mutation: the mutant boots and opens the pane", False, M.uart()[-400:])
        else:
            M.click(*row_xy(2), "settings users view add\n")
            M.typ("zed"); M.key("ret"); M.typ("pass1111"); M.key("ret"); M.typ("pass1111"); M.key("ret")
            M.wait_for("settings users view auth\n", 10)
            n = len(M.uart())
            M.typ("nopenope"); M.key("ret")
            added = M.wait_for("settings users added zed\n", 60, n)
            check("mutation: QEMU FAILS the wrong-password step when the pane skips sudo_check (a wrong admin password added an account)", added)
    finally:
        M.close()
finally:
    open(UI, "w").write(orig)
    build("kernel8.elf")
    if open(UI).read() != orig: print("FAIL: users_ui.h was not restored"); sys.exit(1)

if fails:
    print("FAIL: %d of the Users pane checks failed" % len(fails)); sys.exit(1)
print("PASS: Settings' Users pane lists accounts, adds one (a mismatch is refused, a wrong admin password is refused, the right one works), makes an admin, resets a password, removes with a confirm, keeps all of it across a reboot, refuses a normal user every admin action, lets him change his own password with the old one, never shows or logs a password, and a pane that skips the admin check is caught")
