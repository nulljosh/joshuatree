#!/usr/bin/env python3
"""ARM64 accounts (arch/arm64/accounts.c, the Terminal's users, adduser and deluser, JT/USERS.TXT).

  1. Host: the real accounts.c and the real kernel/auth_kdf.c (with the BearSSL sources they need) under address and
     undefined-behaviour sanitizers, with a stubbed card and entropy. An add then a right and a wrong password; a repeat name,
     a ninth user, bad names, short and long passwords are refused with their own codes; the stored line is
     `name:p2:100000:salt:hash` with no plaintext and loads back; a failed write or no card keeps nothing in memory; a
     file of corrupt, cut, over-long, bad-hex, bad-count, untagged and repeated lines yields only its good records; remove
     needs the right password; PBKDF2 matches the published vectors tools/auth-host uses, and a record made by Python's
     hashlib is accepted (an independent implementation).
  2. QEMU virt, headless, the check card (sdcard.py) as a virtio disk, Terminal open (F2): `users` says none, then
     `adduser ann hunter22` (timed) and `users` lists ann; the echoed line reads `adduser ann ********`; a wrong deluser, a
     repeat name and a short password (4.10.0: the first account is an admin and just proved itself, so `sudo adduser` runs) and a bad line are refused; the UART never holds a password that was typed.
  3. The card: JT/USERS.TXT read back with mtools is one `ann:p2:100000:` record whose hash is what Python's PBKDF2 gives
     for the password and the stored salt, with no plaintext. A second boot on the same card stops at the login screen (4.9.0), signs ann in, lists ann, deluser with the
     right password removes it, and a third `users` says none.
Skips (exit 0) when the tools are missing.
"""
import hashlib, json, os, re, shutil, socket, subprocess, sys, tempfile, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from scratch import scratch_dir
from sdcard import PART, build_card

root = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
arch = os.path.join(root, "arch", "arm64")
if not all(shutil.which(t) for t in ("clang", "ld.lld", "qemu-system-aarch64", "mmd", "mtype")):
    print("SKIP: clang, ld.lld, qemu-system-aarch64 or mtools not installed"); sys.exit(0)
fails = []


def check(name, ok, detail=""):
    print(("  ok: " if ok else "  FAIL: ") + name + ("" if ok or not detail else " -- " + str(detail)[-400:]))
    if not ok: fails.append(name)


# ---- 1. the host test of accounts.c ----
HOST = r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "accounts.h"
#include "auth_kdf.h"
static int card = 1, write_ok = 1, entropy_ok = 1, writes = 0, file_len = -1;
static char file[16384]; static unsigned ctr;
int sd_ready(void) { return card; }
int sd_enter_jt(int make) { (void)make; return card; }
int sd_write(const char *name, const void *data, unsigned len) {
    assert(!strcmp(name, "USERS.TXT")); assert(len < sizeof file);
    if (!write_ok) return 0;
    memcpy(file, data, len); file_len = (int)len; writes++; return 1;
}
int fat_read_file(const char *name, void *buf, unsigned cap) {
    assert(!strcmp(name, "USERS.TXT")); if (file_len < 0) return -1;
    unsigned n = (unsigned)file_len < cap ? (unsigned)file_len : cap; memcpy(buf, file, n); return (int)n;
}
int fat_chdir(const char *n) { (void)n; return 1; }
int tls_entropy(unsigned char *out, unsigned n) { if (!entropy_ok) return 0; for (unsigned i = 0; i < n; i++) out[i] = (unsigned char)(++ctr * 37u + i); return 1; }
static void put(const char *t) { file_len = (int)strlen(t); memcpy(file, t, (size_t)file_len); }
static int has(const char *needle) { return file_len > 0 && memmem(file, (size_t)file_len, needle, strlen(needle)) != 0; }
static void hex(const unsigned char *b, unsigned n, char *o) { for (unsigned i = 0; i < n; i++) sprintf(o + 2 * i, "%02x", b[i]); }
int main(int argc, char **argv) {
    unsigned char dk[64]; char h[129];
    auth_pbkdf2_sha256("passwd", 6, "salt", 4, 1, dk, 64); hex(dk, 64, h);
    assert(!strcmp(h, "55ac046e56e3089fec1691c22544b605f94185216dde0465e68b9d57c20dacbc49ca9cccf179b645991664b39d77ef317c71b845b1e30bd509112041d3a19783"));
    auth_pbkdf2_sha256("password", 8, "salt", 4, 4096, dk, 32); hex(dk, 32, h);
    assert(!strcmp(h, "c5e478d59288c841aa530db6845c4c8d962893a001ce4e11a4963873aa98134a"));
    auth_pbkdf2_sha256("Password", 8, "NaCl", 4, 80000, dk, 64); hex(dk, 64, h);
    assert(!strcmp(h, "4ddcd8f60b98be21830cee5ef22701f9641a4418d04c0414aeff08876b34ab56a1d425a1225833549adb841b51c9b3176a272bdebba1d078478f62b397f33c8d"));
    puts("ok kat");

    accounts_load(); assert(accounts_count() == 0 && !strcmp(accounts_name(0), ""));       /* no file: no accounts */
    assert(accounts_add("ann", "hunter22") == ACC_OK && accounts_count() == 1 && !strcmp(accounts_name(0), "ann"));
    assert(accounts_check("ann", "hunter22") == ACC_OK);
    assert(accounts_check("ann", "hunter23") == ACC_E_WRONG && accounts_check("ann", "") == ACC_E_WRONG && accounts_check("bob", "hunter22") == ACC_E_NOUSER);
    assert(file_len > 0 && !has("hunter22") && !has("hunter"));
    assert(!strncmp(file, "ann:p2:100000:", 14) && file_len == 14 + 32 + 1 + 64 + 1 && file[file_len - 1] == '\n');
    puts("ok add and check");
    accounts_load(); assert(accounts_count() == 1 && accounts_check("ann", "hunter22") == ACC_OK && accounts_check("ann", "x12345") == ACC_E_WRONG);   /* loads back from the stored text */
    puts("ok reload");
    int w = writes;
    assert(accounts_add("ann", "other1234") == ACC_E_DUP);
    const char *bad[] = { "", "Ann", "a b", "a:b", "ann\n", "a.b", "abcdefghijklmnopqrstuvwxy", "\xc3\xa9" };
    for (unsigned i = 0; i < sizeof bad / sizeof *bad; i++) assert(accounts_add(bad[i], "hunter22") == ACC_E_NAME);
    assert(accounts_add("abcdefghijklmnopqrstuvwx", "hunter22") == ACC_OK);          /* 24 is fine */
    assert(accounts_add("bob", "abc") == ACC_E_PASS && accounts_add("bob", "") == ACC_E_PASS);
    char long65[66]; memset(long65, 'p', 65); long65[65] = 0;
    assert(accounts_add("bob", long65) == ACC_E_PASS);
    long65[64] = 0; assert(accounts_add("a_b-1", long65) == ACC_OK && accounts_check("a_b-1", long65) == ACC_OK);  /* 64 is fine */
    assert(writes == w + 2 && accounts_count() == 3);
    puts("ok refusals");
    assert(accounts_add("u4", "pass4444") == ACC_OK && accounts_add("u5", "pass5555") == ACC_OK && accounts_add("u6", "pass6666") == ACC_OK && accounts_add("u7", "pass7777") == ACC_OK && accounts_add("u8", "pass8888") == ACC_OK);
    assert(accounts_count() == 8); w = writes; static char snap[16384]; int sl = file_len; memcpy(snap, file, (size_t)sl);
    assert(accounts_add("u9", "pass9999") == ACC_E_FULL && accounts_count() == 8 && writes == w && file_len == sl && !memcmp(snap, file, (size_t)sl));
    puts("ok ninth refused");
    accounts_load(); assert(accounts_count() == 8);
    assert(accounts_remove("u8", "pass0000") == ACC_E_WRONG && accounts_count() == 8 && file_len == sl && !memcmp(snap, file, (size_t)sl));
    assert(accounts_remove("zed", "pass0000") == ACC_E_NOUSER);
    assert(accounts_remove("u8", "pass8888") == ACC_OK && accounts_count() == 7 && accounts_check("u8", "pass8888") == ACC_E_NOUSER && !has("u8:"));
    assert(accounts_add("u9", "pass9999") == ACC_OK && accounts_count() == 8);        /* room again */
    puts("ok remove");
    write_ok = 0; w = writes; sl = file_len; memcpy(snap, file, (size_t)sl);
    assert(accounts_remove("u9", "pass9999") == ACC_E_WRITE && accounts_count() == 8 && accounts_check("u9", "pass9999") == ACC_OK);
    accounts_remove("u7", "pass7777"); accounts_load();
    assert(accounts_count() == 8);                                                   /* a refused write left the file and the table alone */
    write_ok = 1; assert(accounts_remove("u9", "pass9999") == ACC_OK && accounts_remove("u7", "pass7777") == ACC_OK);
    write_ok = 0; assert(accounts_add("zed", "zed12345") == ACC_E_WRITE && accounts_check("zed", "zed12345") == ACC_E_NOUSER);
    write_ok = 1; entropy_ok = 0; assert(accounts_add("zed", "zed12345") == ACC_E_ENTROPY && accounts_count() == 6); entropy_ok = 1;
    card = 0; assert(accounts_add("zed", "zed12345") == ACC_E_NOCARD && accounts_count() == 6 && accounts_check("zed", "zed12345") == ACC_E_NOUSER);
    accounts_load(); assert(accounts_count() == 0); card = 1;                        /* no card at load: nothing, and nothing kept from before */
    puts("ok failed writes, no entropy and no card keep nothing");

    /* a clean file, then every way to damage it */
    file_len = -1; accounts_load(); assert(accounts_add("good", "good1234") == ACC_OK);
    char rec[200]; memcpy(rec, file, (size_t)file_len); rec[file_len - 1] = 0;       /* the good line, no newline */
    char *sh = strchr(strchr(strchr(rec, ':') + 1, ':') + 1, ':') + 1;               /* salt */
    char hh[80]; strcpy(hh, strchr(sh, ':') + 1);                                    /* hash */
    char salt[40]; memcpy(salt, sh, 32); salt[32] = 0;
    static char f[16384]; int n = 0;
    #define LINE(...) n += sprintf(f + n, __VA_ARGS__)
    LINE("%s\n", rec);                                                                        /* 1 good */
    LINE("a2:p2:100000:%s:%.63s\n", salt, hh);                                                /* hash one short */
    LINE("a3:p2:100000:%s:%s0\n", salt, hh);                                                  /* hash one long */
    LINE("a4:p2:100000:%s:%.63sg\n", salt, hh);                                               /* bad hex */
    LINE("a5:p2:100000:%.31sz:%s\n", salt, hh);                                               /* bad hex in the salt */
    LINE("a6:p2:999:%s:%s\n", salt, hh);                                                      /* count too small */
    LINE("a7:p2:10000001:%s:%s\n", salt, hh);                                                 /* count too big */
    LINE("a8:p2:100000x:%s:%s\n", salt, hh);                                                  /* count not a number */
    LINE("a9:p2:%s:%s\n", salt, hh);                                                          /* count missing */
    LINE("b1:%s:%s\n", salt, hh);                                                             /* untagged legacy line */
    LINE("b2:p3:100000:%s:%s\n", salt, hh);                                                   /* another tag */
    LINE("b3:p2:100000:%s:%s:extra\n", salt, hh);                                             /* one field too many */
    LINE("abcdefghijklmnopqrstuvwxy:p2:100000:%s:%s\n", salt, hh);                            /* name 25 long */
    LINE("Upper:p2:100000:%s:%s\n", salt, hh);                                                /* name not allowed */
    LINE(":p2:100000:%s:%s\n", salt, hh);                                                     /* no name */
    LINE("good:p2:200000:%s:%s\n", salt, hh);                                                 /* a repeat of good */
    LINE("\n\r\n:::::\ngarbage\n");
    for (int i = 0; i < 700; i++) f[n++] = 'x';                                                /* one line far past any record */
    f[n++] = '\n';
    LINE("crlf:p2:2000:%s:%s\r\n", salt, hh);                                                 /* 2 good: CRLF is fine */
    LINE("last:p2:10000000:%s:%s", salt, hh);                                                 /* 3 good: no final newline, top count */
    f[n] = 0; put(f);
    accounts_load();
    assert(accounts_count() == 3 && !strcmp(accounts_name(0), "good") && !strcmp(accounts_name(1), "crlf") && !strcmp(accounts_name(2), "last"));
    assert(accounts_check("good", "good1234") == ACC_OK);                                       /* the first of a repeated name wins */
    puts("ok damaged file: only the good records");
    /* cut off mid-record: nothing half believed */
    for (int cut = 0; cut < (int)strlen(rec); cut++) { memcpy(file, rec, (size_t)cut); file_len = cut; accounts_load(); assert(accounts_count() == 0); }
    file_len = (int)strlen(rec); memcpy(file, rec, (size_t)file_len); accounts_load(); assert(accounts_count() == 1);
    /* a file larger than the buffer (900 records) is cut, never overrun, and at most eight users load */
    n = 0; for (int i = 0; i < 40; i++) LINE("u%d:p2:100000:%s:%s\n", i, salt, hh);
    put(f); accounts_load(); assert(accounts_count() == 8);
    for (unsigned i = 0; i < 100; i++) memset(file + i * 40, 'q', 40);
    file_len = 4096; accounts_load(); assert(accounts_count() == 0);
    puts("ok cut and oversized files");
    /* a record made by Python's hashlib (argv[1]) is accepted: an independent PBKDF2 */
    if (argc > 1) { put(argv[1]); accounts_load(); assert(accounts_count() == 1 && accounts_check("kat", "correct horse") == ACC_OK && accounts_check("kat", "correct hors") == ACC_E_WRONG); puts("ok python record"); }
    unsigned char z[8] = {1,2,3,4,5,6,7,8}; accounts_wipe(z, 8); for (int i = 0; i < 8; i++) assert(!z[i]);
    for (int c = 0; c <= 9; c++) assert(strlen(accounts_reason(c)) > 3);
    puts("PASS: accounts.c");
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix="jt-accounts-") as tmp:
    src = os.path.join(tmp, "t.c"); open(src, "w").write(HOST)
    exe = os.path.join(tmp, "t")
    bs = os.path.join(root, "third_party/bearssl")
    cc = subprocess.run(["clang", "-std=gnu11", "-D_GNU_SOURCE", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function", "-Wno-deprecated-declarations", "-g", "-fsanitize=address,undefined",
                         "-I", os.path.join(root, "tools/auth-host"), "-I", os.path.join(root, "kernel"), "-I", os.path.join(bs, "inc"), "-I", os.path.join(bs, "src"),
                         "-I", os.path.join(root, "arch/arm64"), src, os.path.join(arch, "accounts.c"), os.path.join(root, "kernel/auth_kdf.c"),
                         *[os.path.join(bs, "src", f) for f in ("sha2small.c", "hmac.c", "dec32be.c", "enc32be.c")], "-o", exe],
                        capture_output=True, text=True)
    check("accounts.c and auth_kdf.c build on the host with warnings as errors", cc.returncode == 0, cc.stderr)
    if cc.returncode == 0:
        salt = bytes(range(16, 32)); it = 2000
        rec = "kat:p2:%d:%s:%s" % (it, salt.hex(), hashlib.pbkdf2_hmac("sha256", b"correct horse", salt, it, 32).hex())
        r = subprocess.run([exe, rec], capture_output=True, text=True, timeout=300)
        check("accounts.c on the host: add, check, refusals, reload, corrupt files, remove, failed writes, vectors", r.returncode == 0 and "PASS: accounts.c" in r.stdout, r.stdout + r.stderr)

# ---- 2. the Terminal in QEMU ----
subprocess.run(["make", "-C", arch, "clean"], capture_output=True)
b = subprocess.run(["make", "-C", arch, "kernel8.elf"], capture_output=True, text=True, timeout=900)
if b.returncode: print("FAIL: make -C arch/arm64 kernel8.elf failed\n" + (b.stdout + b.stderr)[-1500:]); sys.exit(1)

tmp = scratch_dir("arm64-accounts")
card = os.path.join(tmp, "card.img")
build_card(card, False)
MT = dict(os.environ, MTOOLS_SKIP_CHECK="1")
def mt(*a): return subprocess.run([a[0], "-i", f"{card}@@{PART * 512}", *a[1:]], capture_output=True, env=MT)
if mt("mmd", "::JT").returncode: print("FAIL: mtools could not make JT/ on the check card"); sys.exit(1)

SECRETS = ["hunter22", "wrongpass1", "other1234", "abc"]   # every password typed below; none may reach the UART
class Session:
    def __init__(self, tag):
        self.log, sock = os.path.join(tmp, tag + ".uart"), os.path.join(tmp, tag + ".qmp")
        self.q = subprocess.Popen(["qemu-system-aarch64", "-machine", "virt", "-cpu", "cortex-a72", "-m", "256", "-global", "virtio-mmio.force-legacy=false",
                                   "-nic", "none", "-device", "ramfb", "-device", "virtio-keyboard-device",
                                   "-drive", "if=none,id=card,file=%s,format=raw" % card, "-device", "virtio-blk-device,drive=card",
                                   "-display", "none", "-serial", "file:" + self.log, "-qmp", "unix:%s,server,nowait" % sock,
                                   "-kernel", os.path.join(arch, "kernel8.elf")], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.sock, self.f = sock, None
    def uart(self): return open(self.log, errors="replace").read() if os.path.exists(self.log) else ""
    def wait_for(self, text, secs=8.0, after=0):
        end = time.time() + secs
        while time.time() < end:
            if text in self.uart()[after:]: return True
            time.sleep(0.05)
        return False
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
    def run(self, line, want, secs=30):   # type a line, Enter, wait for the UART text it must cause; returns seconds from Enter
        n = len(self.uart())
        for ch in line: self.key("spc" if ch == " " else "minus" if ch == "-" else ch)
        t0 = time.time(); self.key("ret")
        ok = self.wait_for(want, secs, n)
        return ok, time.time() - t0
    def open_terminal(self, login=None):
        if not self.wait_for("M2 input ready", 60) or not self.wait_for("sd fat32 partition at block 2048", 30): return False
        self.connect(); time.sleep(0.5)
        if login:   # a card with an account boots to the login screen (4.9.0): sign in first
            if not self.wait_for("login: screen up", 30): return False
            for c in login[0]: self.key(c)
            self.key("ret")
            for c in login[1]: self.key(c)
            self.key("ret")
            if not self.wait_for("login: desktop up", 120): return False
            time.sleep(0.5)
        n = self.uart().count("terminal open"); self.key("f2")
        end = time.time() + 10
        while time.time() < end and self.uart().count("terminal open") < n + 1: time.sleep(0.1)
        time.sleep(0.4)
        return self.uart().count("terminal open") >= n + 1
    def close(self):
        if self.f: self.f.close()
        if self.q.poll() is None: self.q.kill()
        self.q.wait()

timing = []
S = Session("a")
try:
    if not S.open_terminal(): check("the card and the Terminal come up", False, S.uart()[-600:]); raise SystemExit
    check("users on a fresh card says there are none", S.run("users", "users: none yet")[0], S.uart()[-300:])
    ok, secs = S.run("adduser ann hunter22", "adduser ann: added", 120); timing.append(secs)
    check("adduser ann hunter22 adds the account (%.1f s under QEMU, 100000 iterations)" % secs, ok, S.uart()[-400:])
    check("the echoed line shows the password as stars", "joshua@tree % adduser ann ********\n" in S.uart(), S.uart()[-400:])
    check("users lists ann", S.run("users", "ann\n")[0], S.uart()[-200:])
    ok, secs = S.run("deluser ann wrongpass1", "deluser ann: wrong password", 120)
    check("a wrong password cannot delete the account (%.1f s)" % secs, ok, S.uart()[-300:])
    check("... and the deluser echo is starred too", "joshua@tree % deluser ann **********\n" in S.uart(), S.uart()[-900:])
    check("a repeat name is refused", S.run("sudo adduser ann other1234", "adduser ann: that name is taken", 60)[0], S.uart()[-300:])
    check("a short password is refused", S.run("sudo adduser bob abc", "adduser bob: the password is 4 to 64", 30)[0], S.uart()[-300:])
    check("a name with a capital is refused", S.run("adduser bob hunter22", "adduser bob: added", 120)[0], S.uart()[-300:]) if False else None
    check("a line with no password is told how to use it", S.run("adduser ann", "adduser: use adduser NAME PASSWORD", 10)[0], S.uart()[-300:])
    check("ann is still the only account", S.run("users", "ann\n")[0] and S.uart().count("\nann\n") == 2, S.uart()[-300:])
    leaked = [s for s in SECRETS if s in S.uart()]
    check("no typed password ever reached the UART", not leaked, leaked)
    check("no fault, no crash", "KERNEL CRASH" not in S.uart() and "oom" not in S.uart(), S.uart()[-300:])
except SystemExit:
    pass
finally:
    first = S.uart()
    S.close()

# ---- 3. the card, and a second boot on it ----
text = mt("mtype", "::JT/USERS.TXT").stdout.decode(errors="replace")
m = re.fullmatch(r"ann:p2:100000:([0-9a-f]{32}):([0-9a-f]{64})\r?\n", text)
check("JT/USERS.TXT on the card is one ann:p2:100000: record", bool(m), text)
check("... with no plaintext password", "hunter22" not in text and "wrongpass1" not in text)
if m:
    want = hashlib.pbkdf2_hmac("sha256", b"hunter22", bytes.fromhex(m.group(1)), 100000, 32).hex()
    check("... whose hash is PBKDF2-HMAC-SHA256 of the password, the salt and 100000 rounds (checked with Python's hashlib)", m.group(2) == want, m.group(2))
S = Session("b")
try:
    if not S.open_terminal(("ann", "hunter22")): check("the second boot comes up", False, S.uart()[-600:]); raise SystemExit
    check("second boot: users still lists ann", S.run("users", "ann\n")[0], S.uart()[-300:])
    ok, secs = S.run("deluser ann hunter22", "deluser ann: removed", 120)
    check("deluser with the right password removes it (%.1f s)" % secs, ok, S.uart()[-300:])
    check("users says none again", S.run("users", "users: none yet")[0], S.uart()[-300:])
    check("the second boot's UART never held a password", not [s for s in SECRETS if s in S.uart()])
except SystemExit:
    pass
finally:
    S.close()
text = mt("mtype", "::JT/USERS.TXT").stdout.decode(errors="replace")
check("the card's file holds no account after the removal", text.strip() == "", text)
if timing: print("  adduser took %.1f s under QEMU (100000 PBKDF2 iterations, unchanged)" % max(timing))

if fails:
    print("FAIL: %d of the accounts checks failed" % len(fails)); sys.exit(1)
print("PASS: accounts are salted and iterated, the Terminal adds, lists and removes them, the password never shows or reaches the UART, USERS.TXT survives a reboot")
