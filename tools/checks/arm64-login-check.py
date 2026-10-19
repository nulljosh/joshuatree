#!/usr/bin/env python3
"""ARM64 login screen (arch/arm64/login.c, login_ui.h, the `logout` command; 4.9.0, slice 2 of 6.0).

  1. Host: the real login.c under address and undefined-behaviour sanitizers with a stubbed accounts_check and wait. The form
     starts on the name; capitals fold and bad characters are dropped; Enter in the name moves to the password and an empty
     name stays; a wrong password waits exactly 1000 ms once, clears the password, keeps the name, shows the message until the
     next key and signs nobody in; the right one sets current_user and leaves no password or name in the buffers; length caps
     hold; a password is never passed on once checked (the stub sees it exactly once).
  2. QEMU virt, headless, a check card whose JT/USERS.TXT holds ann (a record Python's hashlib made): the boot stops at the
     login screen (a screendump shows a card on bare wallpaper, no menu bar; `login: desktop up` is absent); a wrong password
     prints `login: wrong password`, takes at least a second and leaves the screen up; the right one signs in and the
     screendump has the menu bar back with the name in the menu bar; the Terminal's `logout` brings the screen back and a
     right password gets the desktop again; no typed password and no key line ever reached the UART before the sign-in.
  3. A card with no accounts boots with no login screen and `logout` says there is nobody to sign out.
  4. Mutation: login.c made to accept any password must FAIL part 2's wrong-password step (it is restored afterwards).
Skips (exit 0) when the tools are missing.
"""
import hashlib, json, os, shutil, socket, subprocess, sys, tempfile, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from scratch import scratch_dir
from sdcard import PART, build_card

root = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
arch = os.path.join(root, "arch", "arm64")
if not all(shutil.which(t) for t in ("clang", "ld.lld", "qemu-system-aarch64", "mmd", "mcopy")):
    print("SKIP: clang, ld.lld, qemu-system-aarch64 or mtools not installed"); sys.exit(0)
fails = []


def check(name, ok, detail=""):
    print(("  ok: " if ok else "  FAIL: ") + name + ("" if ok or not detail else " -- " + str(detail)[-400:]))
    if not ok: fails.append(name)


# ---- 1. the host test of login.c ----
HOST = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "accounts.h"
#include "login.h"
static int checks, waits; static unsigned last_wait; static char seen[80];
int accounts_check(const char *n, const char *p) { checks++; strcpy(seen, p); return !strcmp(n, "ann") && !strcmp(p, "hunter22") ? ACC_OK : ACC_E_WRONG; }
void accounts_wipe(void *p, unsigned n) { volatile unsigned char *v = p; while (n--) *v++ = 0; }
void login_wait_ms(unsigned ms) { waits++; last_wait = ms; }
static void typ(const char *s) { for (; *s; s++) login_key(*s); }
int main(void) {
    login_reset(); assert(login_field() == 0 && !login_name()[0] && !login_pass_len() && !current_user[0]);
    assert(login_key('\n') == LOGIN_NONE && login_field() == 0);                   /* an empty name stays on the name */
    assert(login_key('\b') == LOGIN_NONE);
    typ("AnN"); assert(!strcmp(login_name(), "ann"));                              /* capitals fold */
    assert(login_key(' ') == LOGIN_NONE && login_key('.') == LOGIN_NONE && login_key(':') == LOGIN_NONE && !strcmp(login_name(), "ann"));
    assert(login_key('\x01') == LOGIN_NONE && login_key(200) == LOGIN_NONE);
    assert(login_key('_') == LOGIN_REDRAW && login_key('-') == LOGIN_REDRAW && login_key('\b') == LOGIN_REDRAW && login_key('\b') == LOGIN_REDRAW && !strcmp(login_name(), "ann"));
    puts("ok name field");
    assert(login_key('\n') == LOGIN_REDRAW && login_field() == 1);
    assert(login_key('\t') == LOGIN_REDRAW && login_field() == 0 && login_key('\t') == LOGIN_REDRAW && login_field() == 1);
    typ("wrongpass1"); assert(login_pass_len() == 10 && !strcmp(login_name(), "ann"));
    assert(checks == 0);
    assert(login_key('\n') == LOGIN_REDRAW);                                       /* wrong */
    assert(checks == 1 && waits == 1 && last_wait == 1000 && !strcmp(seen, "wrongpass1"));
    assert(login_message() == 1 && login_pass_len() == 0 && !current_user[0] && login_field() == 1 && !strcmp(login_name(), "ann"));
    assert(login_key('x') == LOGIN_REDRAW && login_message() == 0);                /* the next key takes the message down */
    assert(login_key('\n') == LOGIN_REDRAW && checks == 2 && waits == 2 && login_message() == 1);   /* "x" is wrong too */
    assert(login_key('\n') == LOGIN_REDRAW && login_message() == 0 && checks == 2);   /* an empty password is not even checked */
    puts("ok wrong password: one wait, cleared, name kept, message");
    typ("hunter2"); assert(login_key('2') == LOGIN_REDRAW);
    assert(login_key('\n') == LOGIN_DONE && checks == 3 && waits == 2 && !strcmp(current_user, "ann"));
    assert(login_pass_len() == 0 && !login_name()[0] && login_field() == 0 && login_message() == 0);
    puts("ok right password signs in and clears the form");
    login_reset(); assert(!current_user[0]);
    typ("abcdefghijklmnopqrstuvwxyz"); assert(strlen(login_name()) == 24);         /* the name caps at 24 */
    login_key('\n'); for (int i = 0; i < 80; i++) login_key('p'); assert(login_pass_len() == 64);   /* the password at 64 */
    login_key('\n'); assert(login_pass_len() == 0 && waits == 3);
    puts("PASS: login.c");
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix="jt-login-") as tmp:
    src = os.path.join(tmp, "t.c"); open(src, "w").write(HOST)
    exe = os.path.join(tmp, "t")
    def host_run(label):
        cc = subprocess.run(["clang", "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-g", "-fsanitize=address,undefined", "-I", arch, src,
                             os.path.join(arch, "login.c"), "-o", exe], capture_output=True, text=True)
        if cc.returncode: return False, cc.stderr
        r = subprocess.run([exe], capture_output=True, text=True, timeout=60)
        return r.returncode == 0 and "PASS: login.c" in r.stdout, r.stdout + r.stderr
    ok, out = host_run("real")
    check("login.c on the host: form, folding, wrong password (one 1000 ms wait, cleared), right password, caps", ok, out)

# ---- 2. the screen in QEMU ----
def build():
    subprocess.run(["make", "-C", arch, "clean"], capture_output=True)
    b = subprocess.run(["make", "-C", arch, "kernel8.elf"], capture_output=True, text=True, timeout=900)
    if b.returncode: print("FAIL: make -C arch/arm64 kernel8.elf failed\n" + (b.stdout + b.stderr)[-1500:]); sys.exit(1)

build()
tmp = scratch_dir("arm64-login")
MT = dict(os.environ, MTOOLS_SKIP_CHECK="1")
SECRETS = ["hunter22", "wrongpass1"]
salt = bytes(range(32, 48))
RECORD = "ann:p2:100000:%s:%s\n" % (salt.hex(), hashlib.pbkdf2_hmac("sha256", b"hunter22", salt, 100000, 32).hex())


def make_card(tag, users):
    card = os.path.join(tmp, tag + ".img")
    build_card(card, False)
    def mt(*a): return subprocess.run([a[0], "-i", f"{card}@@{PART * 512}", *a[1:]], capture_output=True, env=MT)
    if mt("mmd", "::JT").returncode: print("FAIL: mtools could not make JT/ on the check card"); sys.exit(1)
    if users:
        f = os.path.join(tmp, tag + ".users"); open(f, "w").write(users)
        if mt("mcopy", f, "::JT/USERS.TXT").returncode: print("FAIL: mtools could not write USERS.TXT"); sys.exit(1)
    return card


class Session:
    def __init__(self, tag, card):
        self.tag = tag
        self.log, sock = os.path.join(tmp, tag + ".uart"), os.path.join(tmp, tag + ".qmp")
        self.q = subprocess.Popen(["qemu-system-aarch64", "-machine", "virt", "-cpu", "cortex-a72", "-m", "256", "-global", "virtio-mmio.force-legacy=false",
                                   "-nic", "none", "-device", "ramfb", "-device", "virtio-keyboard-device",
                                   "-drive", "if=none,id=card,file=%s,format=raw" % card, "-device", "virtio-blk-device,drive=card",
                                   "-display", "none", "-serial", "file:" + self.log, "-qmp", "unix:%s,server,nowait" % sock,
                                   "-kernel", os.path.join(arch, "kernel8.elf")], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.sock, self.f, self.shots = sock, None, 0
    def uart(self): return open(self.log, errors="replace").read() if os.path.exists(self.log) else ""
    def wait_for(self, text, secs=8.0, after=0):
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
        s_ = socket.socket(socket.AF_UNIX); s_.settimeout(60)
        for _ in range(100):
            try: s_.connect(self.sock); break
            except OSError: time.sleep(0.1)
        self.f = s_.makefile("rw"); self.f.readline(); self.cmd("qmp_capabilities")
    def cmd(self, c, **a):
        self.f.write(json.dumps({"execute": c, "arguments": a}) + "\n"); self.f.flush()
        while True:
            r = json.loads(self.f.readline())
            if "return" in r or "error" in r: return r
    def key(self, *qcodes): self.cmd("send-key", keys=[{"type": "qcode", "data": k} for k in qcodes]); time.sleep(0.12)
    def typ(self, line):
        for ch in line: self.key("spc" if ch == " " else "minus" if ch == "-" else ch)
    def shot(self):
        path = os.path.join(tmp, "%s%d.ppm" % (self.tag, self.shots)); self.shots += 1
        self.cmd("screendump", filename=path)
        for _ in range(60):
            if os.path.exists(path) and os.path.getsize(path) > 100: time.sleep(0.2); break
            time.sleep(0.1)
        d = open(path, "rb").read(); parts = d.split(b"\n", 3)
        w, h = (int(v) for v in parts[1].split())
        return w, h, parts[3]
    @staticmethod
    def px(shot, x, y):
        w, h, d = shot; i = (y * w + x) * 3; return tuple(d[i:i + 3])
    def close(self):
        if self.f: self.f.close()
        if self.q.poll() is None: self.q.kill()
        self.q.wait()


WALL = (0x6E, 0x6E, 0x6E)
def desktop_up(sh):   # the menu bar is on screen: some pixel of the top rows is not the bare wallpaper (the login screen is wallpaper and one card)
    w, h, _ = sh; return any(Session.px(sh, x, y) != WALL for y in range(0, 20, 3) for x in range(0, w, 5))
def accent_in_bar(sh):
    w, h, d = sh; n = 0
    for y in range(0, 24):
        for x in range(0, w // 2):
            r, g, b = d[(y * w + x) * 3:(y * w + x) * 3 + 3]
            if abs(r - 0xB5) < 40 and abs(g - 0x50) < 40 and abs(b - 0x2C) < 40: n += 1
    return n


def boot_to_login(S):
    if not S.wait_for("M2 input ready", 90) or not S.wait_for("sd fat32 partition at block 2048", 40): return False
    S.connect(); return S.wait_for("login: screen up", 40)


def wrong_phase(S):
    """The screen is up. Returns (stayed_on_login, seconds, shot)."""
    sh = S.shot(); on_login = not desktop_up(sh) and "login: desktop up" not in S.uart()
    n = len(S.uart())
    S.typ("ann"); S.key("ret"); S.typ("wrongpass1"); t0 = time.time(); S.key("ret")
    got = S.wait_any(["login: wrong password", "login: signed in"], 150, n)
    secs = time.time() - t0
    return on_login, got, secs, S.shot()


S = Session("a", make_card("a", RECORD))
try:
    if not boot_to_login(S): check("the card with an account boots to the login screen", False, S.uart()[-600:]); raise SystemExit
    on_login, got, secs, sh2 = wrong_phase(S)
    check("boot with an account stops at the login screen (the screendump has no menu bar, no `login: desktop up`)", on_login, S.uart()[-300:])
    check("a wrong password prints `login: wrong password`, takes a second or more, and signs nobody in", got == "login: wrong password" and secs >= 1.0 and "login: signed in" not in S.uart(), (got, secs))
    check("... and the login screen is still up afterwards", not desktop_up(sh2) and "login: desktop up" not in S.uart())
    n = len(S.uart())
    S.typ("hunter22"); S.key("ret")
    ok = S.wait_for("login: signed in ann", 150, n) and S.wait_for("login: desktop up", 30, n)
    check("the right password signs ann in and the desktop comes up", ok, S.uart()[-300:])
    pre = S.uart().split("login: signed in ann")[0]
    check("no typed password and no key line reached the UART before the sign-in", not [s for s in SECRETS if s in S.uart()] and "key " not in pre.split("login: screen up")[-1], pre[-300:])
    time.sleep(0.5); sh3 = S.shot()
    check("the screendump is the desktop again (the menu bar is back)", desktop_up(sh3))
    na = accent_in_bar(sh3)
    check("the signed-in name is in the menu bar in terracotta (%d accent pixels)" % na, na > 10)
    # the Terminal's logout
    n = S.uart().count("terminal open"); S.key("f2"); end = time.time() + 10
    while time.time() < end and S.uart().count("terminal open") < n + 1: time.sleep(0.1)
    time.sleep(0.4); S.typ("logout"); m = len(S.uart()); S.key("ret")
    check("`logout` in the Terminal says it is signing out and the login screen comes back", S.wait_for("logout: signing out", 20, m) and S.wait_for("login: screen up", 20, m), S.uart()[-300:])
    time.sleep(0.5); sh4 = S.shot()
    check("... the screendump is the login screen, not the desktop", not desktop_up(sh4))
    check("... and the menu bar name is gone from the screen", accent_in_bar(sh4) < na // 2 + 1)
    m = len(S.uart()); S.typ("ann"); S.key("ret"); S.typ("hunter22"); S.key("ret")
    check("signing in again works", S.wait_for("login: desktop up", 150, m), S.uart()[-300:])
    check("no typed password reached the UART in the whole session", not [s for s in SECRETS if s in S.uart()])
    check("no fault, no crash", "KERNEL CRASH" not in S.uart() and "oom" not in S.uart(), S.uart()[-300:])
except SystemExit:
    pass
finally:
    S.close()

# ---- 3. no accounts: no login ----
S = Session("b", make_card("b", ""))
try:
    if not S.wait_for("M2 input ready", 90) or not S.wait_for("sd fat32 partition at block 2048", 40): check("the account-free card boots", False, S.uart()[-400:]); raise SystemExit
    S.connect(); time.sleep(3)
    check("with no accounts there is no login screen", "login: screen up" not in S.uart())
    sh = S.shot(); check("... the screendump is the desktop", desktop_up(sh))
    n = S.uart().count("terminal open"); S.key("f2"); end = time.time() + 10
    while time.time() < end and S.uart().count("terminal open") < n + 1: time.sleep(0.1)
    time.sleep(0.4); S.typ("logout"); m = len(S.uart()); S.key("ret")
    check("`logout` with no accounts says nobody is signed in and shows no login screen", S.wait_for("logout: no accounts", 20, m) and "login: screen up" not in S.uart(), S.uart()[-300:])
except SystemExit:
    pass
finally:
    S.close()

# ---- 4. mutation: any password accepted ----
LOGIN_C = os.path.join(arch, "login.c")
orig = open(LOGIN_C).read()
needle = "int r = accounts_check(name, pass);"
assert needle in orig
try:
    open(LOGIN_C, "w").write(orig.replace(needle, "int r = ACC_OK; (void)accounts_check;"))
    ok, _ = host_run("mutant")
    check("mutation: the host test fails when login.c accepts any password", not ok)
    build()
    M = Session("m", make_card("m", RECORD))
    try:
        if not boot_to_login(M): check("mutation: the mutant kernel boots to the login screen", False, M.uart()[-400:])
        else:
            on_login, got, secs, sh = wrong_phase(M)
            check("mutation: QEMU FAILS the wrong-password step when any password is accepted (signed in with `wrongpass1`)", got == "login: signed in", (got, secs))
    finally:
        M.close()
finally:
    open(LOGIN_C, "w").write(orig)
    build()
    if open(LOGIN_C).read() != orig: print("FAIL: login.c was not restored"); sys.exit(1)

if fails:
    print("FAIL: %d of the login checks failed" % len(fails)); sys.exit(1)
print("PASS: with an account the Pi boots to a login screen, a wrong password costs a second and shows a plain line, the right one brings the desktop and the name, logout returns to it, no password reaches the UART, and a login that accepts any password is caught")
