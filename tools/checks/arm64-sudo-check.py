#!/usr/bin/env python3
"""ARM64 admins, sudo and passwd (arch/arm64/sudo.c, cmd.c, ask.c; 4.10.0, slice 3 of 6.0).

  1. Host: the real sudo.c, accounts.c and kernel/auth_kdf.c under address and undefined-behaviour sanitizers with a stubbed
     card and clock. The first account is the only admin; a SUDOERS.TXT with junk, repeats, CRLF and names that are no account
     keeps only the real ones; a card with accounts and no SUDOERS.TXT makes the first account the admin; makeadmin and
     unadmin write the card first, refuse a repeat, a stranger and the last admin; sudo_check waits LOGIN_DELAY_MS once on a
     wrong password and not on a right one, says "not an admin" after a right password, remembers a success for exactly the
     time limit for that one name and forgets it on sudo -k, a sign-out or losing the admin; a deleted name does not come back
     as an admin; passwd (accounts_passwd) changes the hash with a fresh salt and keeps the old one when the card refuses; an
     unknown name takes as long as a wrong password; USERS.TXT and SUDOERS.TXT are protected file names.
  2. QEMU virt, headless, the sudoshort build (sudo remembered for 12 s), a check card with ann (admin) and bob (normal), made
     with Python's hashlib. Bob: adduser, write, cat of USERS.TXT, makeadmin and deluser of someone else are refused with
     "not an admin; try sudo"; `sudo` asks for his password, then says he is not an admin; passwd refuses a wrong old
     password, a short one and a mismatch, Esc cancels, and a change takes: his old password stops working at the login
     screen and the new one signs in. Ann: a plain adduser says it needs sudo; sudo with a wrong password is refused
     ("wrong password", nothing runs); the right one adds eve; the password is remembered (the next sudo runs at once) and
     forgotten after 12 s and after `sudo -k`; the last admin cannot be removed; makeadmin bob, `sudo passwd bob` (a reset),
     `sudo deluser eve` and unadmin ann work; USERS.TXT and SUDOERS.TXT cannot be written by hand even under sudo; Esc
     cancels a sudo prompt. A third boot: bob signs in with the reset password and, now the only admin, sudo works. An empty
     card: sudo just runs the command, and the first adduser makes an admin who is signed in. No typed password, and no
     star count, ever reached the UART.
  3. The card: SUDOERS.TXT is `bob`, USERS.TXT holds ann and bob, bob's hash is PBKDF2 of the reset password with the stored
     salt, no plaintext anywhere, and the files the refused commands would have written are not there.
  4. Quiet boot (the Pi's): with an account, the login card is up before the desktop is first drawn.
  5. Mutation: sudo_check made to skip the password check must FAIL the host test and the wrong-password step in QEMU
     (restored afterwards).
Skips (exit 0) when the tools are missing.
"""
import hashlib, json, os, re, shutil, socket, subprocess, sys, tempfile, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from scratch import scratch_dir
from sdcard import PART, build_card

root = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
arch = os.path.join(root, "arch", "arm64")
if not all(shutil.which(t) for t in ("clang", "ld.lld", "qemu-system-aarch64", "mmd", "mcopy", "mtype", "mdir")):
    print("SKIP: clang, ld.lld, qemu-system-aarch64 or mtools not installed"); sys.exit(0)
fails = []


def check(name, ok, detail=""):
    print(("  ok: " if ok else "  FAIL: ") + name + ("" if ok or not detail else " -- " + str(detail)[-500:]))
    if not ok: fails.append(name)


# ---- 1. the host test ----
HOST = r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "accounts.h"
#include "login.h"
#include "sudo.h"
#include "auth_kdf.h"
static int card = 1, write_ok = 1, waits = 0; static unsigned last_wait, ctr;
static char users_f[8192], sudo_f[4096]; static int users_len = -1, sudo_len = -1;
static unsigned long now; static unsigned ttl = 300000;
char current_user[ACC_NAME_MAX + 1];
int sd_ready(void) { return card; }
int sd_enter_jt(int make) { (void)make; return card; }
int sd_write(const char *name, const void *data, unsigned len) {
    if (!write_ok) return 0;
    if (!strcmp(name, "USERS.TXT")) { memcpy(users_f, data, len); users_len = (int)len; return 1; }
    if (!strcmp(name, "SUDOERS.TXT")) { memcpy(sudo_f, data, len); sudo_len = (int)len; return 1; }
    assert(!"a file the account code must not write"); return 0;
}
int fat_read_file(const char *name, void *buf, unsigned cap) {
    char *f = !strcmp(name, "USERS.TXT") ? users_f : !strcmp(name, "SUDOERS.TXT") ? sudo_f : 0; assert(f);
    int len = f == users_f ? users_len : sudo_len; if (len < 0) return -1;
    unsigned n = (unsigned)len < cap ? (unsigned)len : cap; memcpy(buf, f, n); return (int)n;
}
int fat_chdir(const char *n) { (void)n; return 1; }
int tls_entropy(unsigned char *out, unsigned n) { for (unsigned i = 0; i < n; i++) out[i] = (unsigned char)(++ctr * 37u + i); return 1; }
void login_wait_ms(unsigned ms) { waits++; last_wait = ms; }
unsigned long sudo_now_ms(void) { return now; }
unsigned sudo_ttl_ms(void) { return ttl; }
static double secs(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (double)t.tv_sec + (double)t.tv_nsec / 1e9; }
static void sfile(const char *t) { sudo_len = (int)strlen(t); memcpy(sudo_f, t, (size_t)sudo_len); }
static int sudo_is(const char *t) { return sudo_len == (int)strlen(t) && !memcmp(sudo_f, t, (size_t)sudo_len); }
static void reload(void) { accounts_load(); sudo_load(); }
int main(void) {
    reload(); assert(!accounts_count() && !sudo_admin_count() && sudo_may_write() && !sudo_is_admin("") && !sudo_is_admin("ann"));
    assert(!sudo_active());
    assert(accounts_add("ann", "hunter22") == ACC_OK && sudo_first("ann") == SUDO_OK && sudo_is("ann\n") && sudo_is_admin("ann"));
    assert(accounts_add("bob", "bobpass44") == ACC_OK && !sudo_is_admin("bob") && sudo_admin_count() == 1 && sudo_is("ann\n"));
    reload(); assert(sudo_is_admin("ann") && !sudo_is_admin("bob") && sudo_admin_count() == 1);
    puts("ok first account is the admin, and it reloads");

    /* who may write settings */
    current_user[0] = 0; assert(!sudo_may_write());
    strcpy(current_user, "bob"); assert(!sudo_may_write());
    strcpy(current_user, "ann"); assert(sudo_may_write());

    /* sudo_check */
    strcpy(current_user, "ann");
    assert(sudo_check("ann", "nope1234") == ACC_E_WRONG && waits == 1 && last_wait == 1000 && !sudo_active());
    assert(sudo_check("nobody", "hunter22") == ACC_E_NOUSER && waits == 2 && !sudo_active());
    assert(sudo_check("bob", "bobpass44") == SUDO_E_NOTADMIN && waits == 2);       /* a right password, no wait, but no admin */
    strcpy(current_user, "bob"); assert(!sudo_active());                              /* bob never got it */
    strcpy(current_user, "ann"); assert(!sudo_active());
    now = 1000; assert(sudo_check("ann", "hunter22") == SUDO_OK && waits == 2 && sudo_active());
    now = 1000 + ttl - 1; assert(sudo_active());                                      /* just inside the time */
    now = 1000 + ttl; assert(!sudo_active());                                         /* exactly the limit: gone */
    now = 5000; assert(sudo_check("ann", "hunter22") == SUDO_OK && sudo_active());
    sudo_drop(); assert(!sudo_active());                                              /* sudo -k */
    assert(sudo_check("ann", "hunter22") == SUDO_OK && sudo_active());
    strcpy(current_user, "bob"); assert(!sudo_active());                              /* another user does not inherit it */
    strcpy(current_user, "ann"); assert(!sudo_active());                              /* and it was dropped for good by the check above */
    ttl = 12000; now = 0; assert(sudo_check("ann", "hunter22") == SUDO_OK); now = 11999; assert(sudo_active()); now = 12000; assert(!sudo_active()); ttl = 300000;
    puts("ok sudo_check: one wait on a wrong password, not-admin after a right one, five minutes, sudo -k");

    /* makeadmin, unadmin */
    assert(sudo_make("zed") == ACC_E_NOUSER && sudo_make("ann") == SUDO_E_ALREADY && sudo_unmake("ann") == SUDO_E_LAST && sudo_unmake("bob") == SUDO_E_NOTLISTED && sudo_unmake("zed") == ACC_E_NOUSER);
    write_ok = 0; assert(sudo_make("bob") == ACC_E_WRITE && !sudo_is_admin("bob") && sudo_is("ann\n")); write_ok = 1;   /* a refused write keeps nothing */
    card = 0; assert(sudo_make("bob") == ACC_E_NOCARD && !sudo_is_admin("bob")); card = 1;
    assert(sudo_make("bob") == SUDO_OK && sudo_is_admin("bob") && sudo_admin_count() == 2 && sudo_is("ann\nbob\n"));
    reload(); assert(sudo_is_admin("ann") && sudo_is_admin("bob"));
    now = 0; assert(sudo_check("ann", "hunter22") == SUDO_OK && sudo_active());
    write_ok = 0; assert(sudo_unmake("ann") == ACC_E_WRITE && sudo_is_admin("ann") && sudo_active()); write_ok = 1;
    assert(sudo_unmake("ann") == SUDO_OK && !sudo_is_admin("ann") && sudo_is("bob\n") && !sudo_active());   /* losing the admin drops the memory */
    assert(sudo_unmake("bob") == SUDO_E_LAST);
    assert(sudo_check("ann", "hunter22") == SUDO_E_NOTADMIN && !sudo_active());
    reload(); assert(!sudo_is_admin("ann") && sudo_is_admin("bob") && sudo_admin_count() == 1);
    puts("ok makeadmin and unadmin");

    /* a deleted name must not come back an admin */
    assert(sudo_make("ann") == SUDO_OK);
    assert(accounts_delete("ann") == ACC_OK); sudo_scrub("ann"); assert(!sudo_is_admin("ann") && sudo_is("bob\n"));
    assert(accounts_add("ann", "newpass99") == ACC_OK); sudo_scrub("ann"); assert(!sudo_is_admin("ann") && sudo_admin_count() == 1);
    reload(); assert(!sudo_is_admin("ann"));
    /* even when the stale line was left on the card: the name is an account again after the re-add, so scrub on add is what stops it */
    sfile("bob\nann\n"); reload(); assert(sudo_is_admin("ann")); sudo_scrub("ann"); assert(!sudo_is_admin("ann") && sudo_is("bob\n"));
    puts("ok a removed name does not return as an admin");

    /* the file is untrusted */
    sfile("ann\r\nghost\nBob\nbob\n\n:::\nabcdefghijklmnopqrstuvwxyz1\nann\nbob"); reload();
    assert(sudo_admin_count() == 2 && sudo_is_admin("ann") && sudo_is_admin("bob") && !sudo_is_admin("ghost") && !sudo_is_admin("Bob"));
    sfile("ghost\nnobody\n"); reload(); assert(sudo_admin_count() == 1 && sudo_is_admin(accounts_name(0)));          /* none real: the first account */
    sudo_len = -1; reload(); assert(sudo_admin_count() == 1 && sudo_is_admin(accounts_name(0)) && sudo_len == -1);   /* no file: the first account, in memory only */
    { char big[2000]; memset(big, 'a', sizeof big); big[1999] = 0; sfile(big); reload(); assert(sudo_admin_count() == 1); }
    card = 0; reload(); assert(accounts_count() == 0 && sudo_admin_count() == 0); card = 1; reload();
    puts("ok damaged SUDOERS.TXT");

    /* passwd */
    reload(); assert(accounts_check("bob", "bobpass44") == ACC_OK);
    char before[8192]; memcpy(before, users_f, (size_t)users_len); int bl = users_len;
    assert(accounts_passwd("bob", "bobnew555") == ACC_OK && accounts_check("bob", "bobnew555") == ACC_OK && accounts_check("bob", "bobpass44") == ACC_E_WRONG);
    assert(users_len == bl && memcmp(before, users_f, (size_t)bl) && !memmem(users_f, (size_t)users_len, "bobnew555", 9));
    reload(); assert(accounts_check("bob", "bobnew555") == ACC_OK);
    memcpy(before, users_f, (size_t)users_len);
    assert(accounts_passwd("bob", "abc") == ACC_E_PASS && accounts_passwd("zed", "abcd1234") == ACC_E_NOUSER);
    write_ok = 0; assert(accounts_passwd("bob", "other1234") == ACC_E_WRITE && accounts_check("bob", "bobnew555") == ACC_OK && accounts_check("bob", "other1234") == ACC_E_WRONG); write_ok = 1;
    card = 0; assert(accounts_passwd("bob", "other1234") == ACC_E_NOCARD); card = 1;
    assert(!memcmp(before, users_f, (size_t)users_len));
    puts("ok passwd: a fresh salt and hash, short and unknown refused, a refused write keeps the old password");

    /* timing: an unknown name costs what a wrong password costs */
    double w = 1e9, u = 1e9;
    for (int i = 0; i < 3; i++) {
        double t = secs(); accounts_check("bob", "wrongwrong"); double a = secs() - t; if (a < w) w = a;
        t = secs(); accounts_check("nobody-here", "wrongwrong"); double b = secs() - t; if (b < u) u = b;
    }
    printf("wrong %.4f s, unknown %.4f s\n", w, u);
    assert(u > 0.6 * w && u < 1.7 * w);
    puts("ok unknown name and wrong password take the same time");

    assert(accounts_protected("USERS.TXT") && accounts_protected("users.txt") && accounts_protected("SuDoErS.TxT") && accounts_protected("sudoers.txt"));
    assert(!accounts_protected("USERS.TX") && !accounts_protected("USERS.TXTX") && !accounts_protected("NOTE.TXT") && !accounts_protected("") && !accounts_protected("SETTINGS.TXT"));
    assert(strlen(sudo_reason(SUDO_E_LAST)) > 5 && strlen(sudo_reason(SUDO_E_NOTADMIN)) > 5 && strlen(sudo_reason(ACC_E_NOUSER)) > 3);
    puts("PASS: sudo.c");
    return 0;
}
'''
SUDO_C = os.path.join(arch, "sudo.c")
CHECK_LINE = "int r = accounts_check(user, pass);"
def host_run(tmp):
    src = os.path.join(tmp, "t.c"); open(src, "w").write(HOST)
    exe = os.path.join(tmp, "t")
    bs = os.path.join(root, "third_party/bearssl")
    cc = subprocess.run(["clang", "-std=gnu11", "-D_GNU_SOURCE", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function", "-Wno-deprecated-declarations", "-g", "-fsanitize=address,undefined",
                         "-I", os.path.join(root, "tools/auth-host"), "-I", os.path.join(root, "kernel"), "-I", os.path.join(bs, "inc"), "-I", os.path.join(bs, "src"),
                         "-I", arch, src, SUDO_C, os.path.join(arch, "accounts.c"), os.path.join(root, "kernel/auth_kdf.c"),
                         *[os.path.join(bs, "src", f) for f in ("sha2small.c", "hmac.c", "dec32be.c", "enc32be.c")], "-o", exe],
                        capture_output=True, text=True)
    if cc.returncode: return False, cc.stderr
    r = subprocess.run([exe], capture_output=True, text=True, timeout=600)
    return r.returncode == 0 and "PASS: sudo.c" in r.stdout, r.stdout + r.stderr

with tempfile.TemporaryDirectory(prefix="jt-sudo-") as t0:
    ok, out = host_run(t0)
    check("sudo.c, accounts.c on the host: first admin, sudo_check, five minutes, makeadmin, unadmin, damaged files, passwd, timing", ok, out)

# ---- QEMU plumbing ----
def build(*args):
    subprocess.run(["make", "-C", arch, "clean"], capture_output=True)
    b = subprocess.run(["make", "-C", arch, *args], capture_output=True, text=True, timeout=900)
    if b.returncode: print("FAIL: make -C arch/arm64 %s failed\n" % " ".join(args) + (b.stdout + b.stderr)[-1500:]); sys.exit(1)

tmp = scratch_dir("arm64-sudo")
MT = dict(os.environ, MTOOLS_SKIP_CHECK="1")
def mt(card, *a): return subprocess.run([a[0], "-i", f"{card}@@{PART * 512}", *a[1:]], capture_output=True, env=MT)
def record(name, pw, salt_byte):
    salt = bytes(range(salt_byte, salt_byte + 16))
    return "%s:p2:100000:%s:%s\n" % (name, salt.hex(), hashlib.pbkdf2_hmac("sha256", pw.encode(), salt, 100000, 32).hex())
def make_card(tag, users="", sudoers=None):
    card = os.path.join(tmp, tag + ".img")
    build_card(card, False)
    if mt(card, "mmd", "::JT").returncode: print("FAIL: mtools could not make JT/"); sys.exit(1)
    for name, text in (("USERS.TXT", users), ("SUDOERS.TXT", sudoers)):
        if text:
            f = os.path.join(tmp, tag + "." + name); open(f, "w").write(text)
            if mt(card, "mcopy", f, "::JT/" + name).returncode: print("FAIL: mtools could not write " + name); sys.exit(1)
    return card
QK = {" ": "spc", "-": "minus", ".": "dot", "/": "slash"}
EVDEV = {**{c: "1234567890"[c - 2] for c in range(2, 12)}, 12: "-", 52: ".", 53: "/", 57: " ",
         **{16 + i: ch for i, ch in enumerate("qwertyuiop")}, **{30 + i: ch for i, ch in enumerate("asdfghjkl")}, **{44 + i: ch for i, ch in enumerate("zxcvbnm")}}
def keystream(uart):   # what the key lines on the UART spell: every "key N down" turned back into its character
    return "".join(EVDEV.get(int(m), "?") for m in re.findall(r"^key (\d+) down$", uart, re.M))

class Session:
    def __init__(self, tag, card, kernel="kernel8.elf"):
        self.tag = tag; self.shots = 0
        self.log, sock = os.path.join(tmp, tag + ".uart"), os.path.join(tmp, tag + ".qmp")
        self.q = subprocess.Popen(["qemu-system-aarch64", "-machine", "virt", "-cpu", "cortex-a72", "-m", "256", "-global", "virtio-mmio.force-legacy=false",
                                   "-nic", "none", "-device", "ramfb", "-device", "virtio-keyboard-device",
                                   "-drive", "if=none,id=card,file=%s,format=raw" % card, "-device", "virtio-blk-device,drive=card",
                                   "-display", "none", "-serial", "file:" + self.log, "-qmp", "unix:%s,server,nowait" % sock,
                                   "-kernel", os.path.join(arch, kernel)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.sock, self.f = sock, None
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
    def key(self, *qcodes): self.cmd("send-key", keys=[{"type": "qcode", "data": k} for k in qcodes]); time.sleep(0.1)
    def typ(self, line):
        for ch in line: self.key(QK.get(ch, ch))
    def boot(self, login=None):
        if not self.wait_for("M2 input ready", 90) or not self.wait_for("sd fat32 partition at block 2048", 40): return False
        self.connect(); time.sleep(0.5)
        if login:
            if not self.wait_for("login: screen up", 40): return False
            n = len(self.uart())
            self.typ(login[0]); self.key("ret"); self.typ(login[1]); self.key("ret")
            if not self.wait_for("login: desktop up", 150, n): return False
            time.sleep(0.5)
        return True
    def terminal(self):
        n = self.uart().count("terminal open"); self.key("f2"); end = time.time() + 10
        while time.time() < end and self.uart().count("terminal open") < n + 1: time.sleep(0.1)
        time.sleep(0.4)
        return self.uart().count("terminal open") >= n + 1
    def run(self, line, want, secs=30):   # a command; returns (seen, seconds from Enter, the UART text it caused)
        n = len(self.uart()); self.typ(line); t0 = time.time(); self.key("ret")
        ok = self.wait_for(want, secs, n)
        return ok, time.time() - t0, self.uart()[n:]
    def ask(self, line, secs=20):   # a command that must stop at a password prompt: its echo is on the UART and nothing else is
        n = len(self.uart()); self.typ(line); self.key("ret")
        ok = self.wait_for(line.split(" ")[0] + " ", secs, n)   # the echo of the line
        time.sleep(1.5)
        return ok, self.uart()[n:]
    def password(self, pw, want, secs=120):   # answer the prompt; returns (seen, seconds, text)
        n = len(self.uart()); self.typ(pw); t0 = time.time(); self.key("ret")
        ok = self.wait_for(want, secs, n)
        return ok, time.time() - t0, self.uart()[n:]
    def shot(self):
        path = os.path.join(tmp, "%s%d.ppm" % (self.tag, self.shots)); self.shots += 1
        self.cmd("screendump", filename=path)
        for _ in range(60):
            if os.path.exists(path) and os.path.getsize(path) > 100: time.sleep(0.2); break
            time.sleep(0.1)
        d = open(path, "rb").read(); parts = d.split(b"\n", 3)
        w, h = (int(v) for v in parts[1].split())
        return w, h, parts[3]
    def close(self):
        if self.f: self.f.close()
        if self.q.poll() is None: self.q.kill()
        self.q.wait()

def well(sh, x0=170, x1=630, y0=140, y1=400):   # the Terminal's text well in a screendump (800x600, scale 1): the rows where output would show
    w, h, d = sh
    return b"".join(d[(y * w + x0) * 3:(y * w + x1) * 3] for y in range(y0, y1))
def key_leaks(S): return [x for x in SECRETS if x in keystream(S.uart())]
SECRETS = ["hunter22", "bobpass44", "bobnew555", "bobnew556", "wrongold1", "nope1234", "evepass1", "bobreset7", "wrongpw99", "zedpass99"]
def sudo_k(S):
    return S.run("sudo -k", "sudo: password forgotten", 10)[0]
def with_sudo(S, line, pw, want, secs=150):   # forget, ask, answer: one full password check
    sudo_k(S)
    ok, echo = S.ask("sudo " + line)
    if not ok: return False, 0, echo
    return S.password(pw, want, secs)

build("sudoshort-kernel8.elf")
KERNEL = "sudoshort-kernel8.elf"
ANN = ("ann", "hunter22"); BOB = ("bob", "bobpass44")
USERS = record("ann", "hunter22", 32) + record("bob", "bobpass44", 64)

# ---- 2a. an empty card ----
card0 = make_card("c0")
S = Session("z", card0, KERNEL)
try:
    if not S.boot() or not S.terminal(): check("the empty card boots to the Terminal", False, S.uart()[-500:]); raise SystemExit
    ok, _, out = S.run("sudo write open.txt hello", "wrote JT/open.txt", 20)
    check("with no accounts sudo just runs the command (no password, no prompt)", ok, out)
    check("... and plain `write` works as before", S.run("write plain.txt hello", "wrote JT/plain.txt", 20)[0])
    check("passwd with no accounts says so", S.run("passwd", "passwd: no accounts yet", 10)[0])
    check("admins says there are none", S.run("admins", "admins: none", 10)[0])
    ok, secs, out = S.run("adduser ann hunter22", "ann is the first account, so an admin, and signed in", 150)
    check("the first adduser makes an admin who is signed in (%.1f s)" % secs, ok, out)
    check("admins lists ann", S.run("admins", "\nann\n", 10)[0])
    check("ann, signed in by making the account, can sudo straight away (no password, it was just set)", S.run("sudo write first.txt hi", "wrote JT/first.txt", 20)[0])
    check("no fault, no crash", "KERNEL CRASH" not in S.uart() and "oom" not in S.uart(), S.uart()[-300:])
    check("no password reached the UART, as text or as key codes", not [s for s in SECRETS if s in S.uart()] and not key_leaks(S), key_leaks(S))
except SystemExit:
    pass
finally:
    S.close()
text = mt(card0, "mtype", "::JT/SUDOERS.TXT").stdout.decode(errors="replace")
check("the empty card's SUDOERS.TXT is `ann` after the first account", text.replace("\r", "") == "ann\n", text)

# ---- 2b. session one: bob ----
card = make_card("c1", USERS, "ann\n")
S = Session("b", card, KERNEL)
try:
    if not S.boot(BOB) or not S.terminal(): check("bob signs in and opens the Terminal", False, S.uart()[-500:]); raise SystemExit
    time.sleep(0.6); fresh = well(S.shot())
    for line, want in (("adduser eve evepass1", "adduser: you are not an admin; try sudo"), ("write note.txt hi", "write: you are not an admin; try sudo"),
                       ("mkdir logs", "mkdir: you are not an admin; try sudo"), ("rm note.txt", "rm: you are not an admin; try sudo"),
                       ("cat jt/users.txt", "cat: you are not an admin; try sudo"), ("makeadmin bob", "makeadmin: you are not an admin; try sudo"),
                       ("unadmin ann", "unadmin: you are not an admin; try sudo"), ("deluser ann hunter22", "deluser: you are not an admin; try sudo"),
                       ("passwd ann", "passwd: you are not an admin; try sudo")):
        ok, _, out = S.run(line, want, 20)
        check("bob's `%s` is refused: %s" % (line.split(" ")[0], want.split(": ", 1)[1]), ok, out)
    check("bob can still read the plain files (ls)", S.run("ls jt", "USERS.TXT", 10)[0])
    ok, echo = S.ask("sudo adduser eve evepass1")
    check("bob's `sudo adduser eve ...` stops at a prompt and the echo stars the password", ok and "sudo adduser eve ********\n" in echo, echo)
    ok, secs, out = S.password("bobpass44", "sudo: bob is not an admin")
    check("bob's right password, then `sudo: bob is not an admin` (%.1f s)" % secs, ok and "[sudo] password for bob: \n" in out and "adduser eve: added" not in out, out)
    check("... and nothing was added", S.run("users", "bob\n", 10)[0] and "eve" not in S.uart().split("sudo: bob is not an admin")[-1])
    ok, echo = S.ask("sudo write note.txt hi")
    ok2, secs, out = S.password("wrongpw99", "sudo: wrong password")
    check("bob's wrong sudo password is `wrong password` (%.1f s) and nothing ran" % secs, ok and ok2 and "wrote JT" not in out, out)
    # passwd
    ok, _, _ = S.run("passwd", "bob@tree % passwd\n", 10); time.sleep(1.0)
    check("a signed-in prompt shows the name: bob@tree % (not joshua@tree %) in the echoed line", "bob@tree % passwd\n" in S.uart() and "joshua@tree % passwd" not in S.uart(), S.uart()[-300:])
    ok2, secs, out = S.password("wrongold1", "passwd: wrong password")
    check("passwd: a wrong current password is refused (%.1f s)" % secs, ok and ok2 and "Current password: \n" in out, out)
    S.run("passwd", "bob@tree % passwd\n", 10); time.sleep(1.0)
    check("passwd: the right current password goes on to ask for a new one", S.password("bobpass44", "Current password: \n", 120)[0])
    time.sleep(1.0); ok, _, out = S.password("abc", "the password is 4 to 64 characters")
    check("passwd: a 3 character new password is refused", ok and "nothing changed" in out, out)
    S.run("passwd", "bob@tree % passwd\n", 10); time.sleep(1.0); S.password("bobpass44", "Current password: \n", 120); time.sleep(1.0)
    ok, _, out = S.password("bobnew555", "New password: \n", 20); time.sleep(1.0)
    ok2, _, out2 = S.password("bobnew556", "the two passwords differ")
    check("passwd: a retype that does not match changes nothing", ok and ok2 and "Retype new password: \n" in out2 and "nothing changed" in out2, out + out2)
    S.run("passwd", "bob@tree % passwd\n", 10); time.sleep(1.0)
    n = len(S.uart()); S.typ("bobpa"); S.key("esc"); time.sleep(0.5)
    check("Esc at the password prompt cancels it", S.wait_for("cancelled", 5, n), S.uart()[n:])
    ok, _, out = S.run("users", "bob\n", 10)
    check("... and the next line is a command again, not a password", ok and "zsh: command not found" not in out, out)
    S.run("passwd", "bob@tree % passwd\n", 10); time.sleep(1.0); S.password("bobpass44", "Current password: \n", 120); time.sleep(1.0)
    S.password("bobnew555", "New password: \n", 20); time.sleep(1.0)
    ok, secs, out = S.password("bobnew555", "passwd bob: password changed", 150)
    check("passwd: old right, new twice, `passwd bob: password changed` (%.1f s)" % secs, ok, out)
    # the login screen: the old password no longer works
    used = well(S.shot())
    check("(before the logout the Terminal's well is full of bob's session, so the clean-page test below can fail)", used != fresh)
    m = len(S.uart()); S.typ("logout"); S.key("ret")
    check("logout brings the login screen back", S.wait_for("login: screen up", 20, m), S.uart()[m:])
    m = len(S.uart()); S.typ("bob"); S.key("ret"); S.typ("bobpass44"); S.key("ret")
    check("passwd: the OLD password is now refused at the login screen", S.wait_for("login: wrong password", 150, m) and "login: signed in" not in S.uart()[m:], S.uart()[m:])
    m = len(S.uart()); S.typ("bobnew555"); S.key("ret")
    check("passwd: the NEW password signs in", S.wait_for("login: desktop up", 150, m), S.uart()[m:])
    S.terminal(); time.sleep(0.6)
    check("logout cleared the Terminal's scrollback: after signing in again the well is the same clean page as at first (screendump compare)", well(S.shot()) == fresh)
    check("no fault, no crash", "KERNEL CRASH" not in S.uart() and "oom" not in S.uart(), S.uart()[-300:])
    leaked = [s for s in SECRETS if s in S.uart()]
    check("bob's session: no typed password reached the UART, as text or as key codes", not leaked and not key_leaks(S), (leaked, key_leaks(S)))
    check("... and no star run longer than the command's own (the prompt echoes are the label alone)", not re.search(r"\*{10,}", S.uart()), re.findall(r".*\*{10,}.*", S.uart()))
except SystemExit:
    pass
finally:
    S.close()

# ---- 2c. session two: ann ----
S = Session("a", card, KERNEL)
try:
    if not S.boot(ANN) or not S.terminal(): check("ann signs in and opens the Terminal", False, S.uart()[-500:]); raise SystemExit
    ok, _, out = S.run("adduser eve evepass1", "adduser: needs sudo, try sudo adduser", 20)
    check("ann (an admin) typing a plain `adduser` is told it needs sudo", ok, out)
    check("ann's plain `write` needs sudo too", S.run("write note.txt hi", "write: needs sudo, try sudo write", 20)[0])
    ok, echo = S.ask("sudo adduser eve evepass1")
    ok2, secs, out = S.password("nope1234", "sudo: wrong password")
    check("ann's wrong password is refused with `sudo: wrong password` (%.1f s, over the 1 s wait)" % secs, ok and ok2 and secs >= 1.0 and "adduser eve: added" not in out, out)
    check("... and eve was not added", S.run("users", "ann\n", 10)[0] and "\neve\n" not in S.uart().split("sudo: wrong password")[-1])
    ok, echo = S.ask("sudo adduser eve evepass1")
    ok2, secs, out = S.password("hunter22", "adduser eve: added", 200)
    check("ann's right password: `adduser eve: added` (%.1f s)" % secs, ok and ok2, out)
    check("... eve is listed", S.run("users", "eve\n", 10)[0])
    # memory
    ok, secs, out = with_sudo(S, "write a.txt one", "hunter22", "wrote JT/a.txt")
    check("sudo write after the password runs (%.1f s)" % secs, ok, out)
    t_arm = time.time()
    ok, secs, out = S.run("sudo write b.txt two", "wrote JT/b.txt", 8)
    check("the password is remembered: the next sudo runs at once, no prompt (%.1f s)" % secs, ok and "[sudo] password" not in out, out)
    wait = 14 - (time.time() - t_arm)
    if wait > 0: time.sleep(wait)
    ok, echo = S.ask("sudo write c.txt three")
    check("after the time limit (12 s in this build) sudo asks again", ok and "wrote JT/c.txt" not in echo, echo)
    ok, secs, out = S.password("hunter22", "wrote JT/c.txt")
    check("... and runs after the password (%.1f s)" % secs, ok, out)
    check("sudo -k forgets it", sudo_k(S))
    ok, echo = S.ask("sudo write d.txt four")
    check("after sudo -k the next sudo asks again", ok and "wrote JT/d.txt" not in echo, echo)
    ok, secs, out = S.password("wrongpw99", "sudo: wrong password")
    check("... and a wrong password runs nothing (d.txt is not written)", ok and "wrote JT/d.txt" not in out, out)
    # protected files, even under sudo
    ok, secs, out = with_sudo(S, "write users.txt hacked", "hunter22", "is kept by adduser, passwd and makeadmin")
    check("USERS.TXT cannot be written by hand, even under sudo", ok, out)
    check("... nor SUDOERS.TXT", S.run("sudo write sudoers.txt bob", "write: sudoers.txt is kept by", 20)[0])
    check("... nor removed", S.run("sudo rm users.txt", "rm: users.txt is kept by", 20)[0])
    # Esc at the sudo prompt
    sudo_k(S); ok, echo = S.ask("sudo write e.txt five")
    n = len(S.uart()); S.typ("hun"); S.key("esc")
    check("Esc at the sudo prompt cancels it", ok and S.wait_for("cancelled", 5, n), S.uart()[n:])
    ok, _, out = S.run("users", "ann\n", 10)
    check("... and the next line is a command", ok and "zsh: command not found" not in out)
    # a stray extra space must not un-star the password
    sudo_k(S); ok, echo = S.ask("sudo  adduser zed zedpass99")
    check("`sudo  adduser zed ...` with two spaces: the echo still stars the password", ok and "sudo  adduser zed *********\n" in echo, echo)
    ok, secs, out = S.password("hunter22", "adduser zed: added", 200)
    check("... and it runs (the extra space is ignored)", ok, out)
    ok, secs, out = S.run("sudo   deluser zed", "deluser zed: removed", 200)
    check("`sudo   deluser zed` (three spaces) removes it again", ok, out)
    # admins
    ok, secs, out = with_sudo(S, "unadmin ann", "hunter22", "unadmin ann: that is the last admin")
    check("the last admin cannot be removed", ok, out)
    ok, secs, out = with_sudo(S, "makeadmin bob", "hunter22", "makeadmin bob: now an admin")
    check("sudo makeadmin bob", ok, out)
    check("admins now lists ann and bob", S.run("admins", "\nbob\n", 10)[0] and "\nann\n" in S.uart().split("makeadmin bob: now an admin")[-1])
    check("makeadmin of a stranger is refused", S.run("sudo makeadmin zed", "makeadmin zed: no such account", 20)[0])
    check("makeadmin of an admin is told so", S.run("sudo makeadmin bob", "makeadmin bob: already an admin", 20)[0])
    ok, secs, out = with_sudo(S, "passwd bob", "hunter22", "[sudo] password for ann: \n")
    time.sleep(1.0)
    ok2, _, out2 = S.password("bobreset7", "New password: \n", 20); time.sleep(1.0)
    ok3, secs, out3 = S.password("bobreset7", "passwd bob: password changed", 150)
    check("`sudo passwd bob` asks for a new password and a retype, with no old one, and resets bob's (%.1f s)" % secs, ok and ok2 and ok3 and "Current password" not in out + out2 + out3 and "Retype new password: \n" in out3, out + out2 + out3)
    ok, secs, out = with_sudo(S, "deluser eve", "hunter22", "deluser eve: removed", 200)
    check("`sudo deluser eve` removes the account with no password of eve's (%.1f s)" % secs, ok, out)
    check("... eve is gone", S.run("users", "bob\n", 10)[0] and "\neve\n" not in S.uart().split("deluser eve: removed")[-1])
    ok, secs, out = with_sudo(S, "unadmin ann", "hunter22", "unadmin ann: no longer an admin")
    check("with bob an admin, `sudo unadmin ann` works", ok, out)
    ok, secs, out = with_sudo(S, "write f.txt six", "hunter22", "sudo: ann is not an admin")
    check("ann is no admin any more: a right password then `sudo: ann is not an admin`", ok and "wrote JT/f.txt" not in out, out)
    check("... and a plain write tells her she is not an admin", S.run("write g.txt seven", "write: you are not an admin; try sudo", 20)[0])
    check("no fault, no crash", "KERNEL CRASH" not in S.uart() and "oom" not in S.uart(), S.uart()[-300:])
    leaked = [s for s in SECRETS if s in S.uart()]
    check("ann's session: no typed password reached the UART, as text or as key codes", not leaked and not key_leaks(S), (leaked, key_leaks(S)))
    check("... and no long star run", not re.search(r"\*{10,}", S.uart()), re.findall(r".*\*{10,}.*", S.uart()))
except SystemExit:
    pass
finally:
    S.close()

# ---- 2d. session three: bob, the only admin, with the reset password ----
S = Session("c", card, KERNEL)
try:
    if not S.boot(("bob", "bobreset7")) or not S.terminal(): check("bob signs in with the reset password after a reboot", False, S.uart()[-500:]); raise SystemExit
    check("third boot: admins lists bob and not ann", S.run("admins", "bob\n", 10)[0] and "\nann\n" not in S.uart().split("terminal open")[-1])
    ok, secs, out = with_sudo(S, "write h.txt eight", "bobreset7", "wrote JT/h.txt")
    check("bob, the admin that survived the reboot, can sudo (%.1f s)" % secs, ok, out)
    leaked = [s for s in SECRETS if s in S.uart()]
    check("no typed password reached the UART in the third boot, as text or as key codes", not leaked and not key_leaks(S), (leaked, key_leaks(S)))
except SystemExit:
    pass
finally:
    S.close()

# ---- 3. the card ----
sudoers = mt(card, "mtype", "::JT/SUDOERS.TXT").stdout.decode(errors="replace").replace("\r", "")
check("JT/SUDOERS.TXT is `bob` after the session", sudoers == "bob\n", sudoers)
users = mt(card, "mtype", "::JT/USERS.TXT").stdout.decode(errors="replace").replace("\r", "")
lines = [l for l in users.split("\n") if l]
check("JT/USERS.TXT holds ann and bob only", sorted(l.split(":")[0] for l in lines) == ["ann", "bob"], users)
bob = [l for l in lines if l.startswith("bob:")]
m = re.fullmatch(r"bob:p2:100000:([0-9a-f]{32}):([0-9a-f]{64})", bob[0]) if bob else None
check("... bob's record is a PBKDF2 record", bool(m), bob)
if m:
    check("... whose hash is PBKDF2 of the RESET password with its own salt (Python's hashlib)", m.group(2) == hashlib.pbkdf2_hmac("sha256", b"bobreset7", bytes.fromhex(m.group(1)), 100000, 32).hex())
    check("... and the salt is not the one he started with (a fresh salt per change)", m.group(1) != bytes(range(64, 80)).hex())
check("... ann's record is unchanged", record("ann", "hunter22", 32) in users)
check("... no plaintext password on the card", not [s for s in SECRETS if s in users + sudoers])
names = mdir = mt(card, "mdir", "-b", "::JT").stdout.decode(errors="replace").lower()
for f in ("a.txt", "b.txt", "c.txt", "h.txt"):
    check("JT/%s from a sudo write is on the card" % f.upper(), "/" + f in names or "::" + f in names or f in names, names)
for f in ("note.txt", "d.txt", "e.txt", "f.txt", "g.txt", "logs"):
    check("JT/%s, from a refused or cancelled command, is not there" % f.upper(), f not in names, names)

# ---- 4. the Pi's quiet boot: the login card first ----
build("MAINFLAGS=-DQUIET_BOOT -DWIFI_ICON_TEST=0")
cardq = make_card("cq", record("ann", "hunter22", 32), "ann\n")
S = Session("q", cardq)
try:
    if not S.wait_for("M2 input ready", 90) or not S.wait_for("login: screen up", 90): check("the quiet boot reaches the login card", False, S.uart()[-500:]); raise SystemExit
    S.connect(); time.sleep(1.0)
    up = S.uart()
    check("quiet boot: the desktop is NOT drawn before the login card (`M1c fb ok` comes after the sign-in)", "M1c fb ok" not in up and "login: desktop up" not in up, up[-400:])
    sh = S.shot(); w, h, d = sh
    bare = all(d[(y * w + x) * 3:(y * w + x) * 3 + 3] == bytes((0x6E, 0x6E, 0x6E)) for y in range(0, 20, 3) for x in range(0, w, 7))
    cx, cy = (w - 300) // 2, (h - 244) // 2 - 8
    cream = d[((cy + 120) * w + cx + 8) * 3:((cy + 120) * w + cx + 8) * 3 + 3]
    check("... and the screen shows the cream login card (%s at its edge) on the bare wallpaper, no menu bar" % cream.hex(), bare and cream == bytes((0xFA, 0xF8, 0xF4)))
    n = len(S.uart()); S.typ("ann"); S.key("ret"); S.typ("hunter22"); S.key("ret")
    ok = S.wait_for("login: desktop up", 150, n)
    after = S.uart()[n:]
    check("... after the sign-in the desktop is drawn for the first time (`M1c fb ok`), with the menu bar", ok and "M1c fb ok" in after, after[-300:])
    time.sleep(0.5); sh = S.shot(); w, h, d = sh
    check("... the screendump has the menu bar", any(d[(y * w + x) * 3:(y * w + x) * 3 + 3] != bytes((0x6E, 0x6E, 0x6E)) for y in range(0, 20, 3) for x in range(0, w, 7)))
except SystemExit:
    pass
finally:
    S.close()

# ---- 5. mutation: sudo skips the password check ----
orig = open(SUDO_C).read()
assert CHECK_LINE in orig
try:
    open(SUDO_C, "w").write(orig.replace(CHECK_LINE, "int r = ACC_OK; (void)accounts_check;"))
    with tempfile.TemporaryDirectory(prefix="jt-sudo-mut-") as t1:
        ok, out = host_run(t1)
    check("mutation: the host test fails when sudo_check skips the password", not ok)
    build("sudoshort-kernel8.elf")
    cardm = make_card("cm", USERS, "ann\n")
    M = Session("m", cardm, KERNEL)
    try:
        if not M.boot(ANN) or not M.terminal(): check("mutation: the mutant boots and ann signs in", False, M.uart()[-400:])
        else:
            M.ask("sudo write mut.txt x")
            ok, secs, out = M.password("wrongpw99", "wrote JT/mut.txt", 60)
            check("mutation: QEMU FAILS the wrong-password step when sudo skips the check (a wrong password ran the command)", ok, out)
    finally:
        M.close()
finally:
    open(SUDO_C, "w").write(orig)
    build("kernel8.elf")
    if open(SUDO_C).read() != orig: print("FAIL: sudo.c was not restored"); sys.exit(1)

if fails:
    print("FAIL: %d of the sudo checks failed" % len(fails)); sys.exit(1)
print("PASS: the first account is an admin, a normal user is refused every privileged command, sudo asks for the password (starred, never on the UART), remembers it for the time limit and forgets it on sudo -k, makeadmin and unadmin keep one admin, passwd changes a password and the old one stops working, and a sudo that skips the check is caught")
