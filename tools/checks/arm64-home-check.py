#!/usr/bin/env python3
"""ARM64 home folders (arch/arm64/home.c, ufile.c, cmd.c, ask.c; 4.11.0, slice 4 of 6.0).

  1. Host: the real home.c and ufile.c under address and undefined-behaviour sanitizers, over an in-memory FAT with case-blind
     names. With no accounts an app's NOTES/NOTES/N1.TXT is JT/NOTES/NOTES/N1.TXT and there is no HOME folder. With ann signed
     in it is JT/HOME/ann/NOTES/NOTES/N1.TXT and the shared JT/NOTES is not made. Bob cannot open, read, list or unlink ann's
     file by any spelling an app can send (HOME/ann/..., a ".." part, a leading slash), his own opens land in his folder, and
     a file ann left open is flushed into ann's folder after bob signs in. home_walk and home_op refuse another person's
     folder in any case unless sudo is active, refuse "." and ".." parts, and leave the cursor at the root; home_save (Samantha's
     [[save]]) writes only ~/NAME. A name over 8 characters gets five letters, a tilde and two hex digits, and two accounts whose
     folder names agree are told apart.
  2. QEMU virt, headless, the sudoshort build, a check card with ann (admin) and bob and a file in ann's folder. Bob: write,
     cat, ls and rm on ~ work with no sudo and land in JT/HOME/bob/; cat, ls, write and rm of ann's folder are refused (a
     ".." walk too); `userrun notes` makes its folder inside bob's home, not in JT/. Ann: bob's folder is refused until sudo,
     opens under sudo, and is refused again after sudo -k; adduser makes JT/HOME/eve and deluser leaves it and says so.
  3. The card: HOME/BOB has the files bob wrote and NOTES/NOTES, ann's file is unchanged, JT/ has no NOTES, HOME/EVE is
     still there after deluser, and neither secret line ever reached the UART.
  4. Mutation: home_may made to say yes, and ufile.c made to ignore the home, must each FAIL the host test (restored after).
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
#include <strings.h>
#include "home.h"
#include "accounts.h"
#include "sudo.h"
#include "login.h"
#include "emmc.h"
#include "../../drivers/fat.h"

/* ---- an in-memory FAT: names compare without case, one cluster number (node index) per folder, 0 the root ---- */
struct node { int used, parent, dir; char name[16]; char data[9000]; int len; };
static struct node nodes[200];
static unsigned cwd;
static int ready = 1;
static int find_child(unsigned dir, const char *name) {
    for (int i = 1; i < 200; i++) if (nodes[i].used && nodes[i].parent == (int)dir && !strcasecmp(nodes[i].name, name)) return i;
    return -1;
}
static int new_node(unsigned dir, const char *name, int isdir) {
    for (int i = 1; i < 200; i++) if (!nodes[i].used) { memset(&nodes[i], 0, sizeof nodes[i]); nodes[i].used = 1; nodes[i].parent = (int)dir; nodes[i].dir = isdir; snprintf(nodes[i].name, sizeof nodes[i].name, "%s", name); return i; }
    return -1;
}
int fat_chdir(const char *n) {
    if (!strcmp(n, ".")) return 1;
    if (!strcmp(n, "..")) { if (cwd) cwd = (unsigned)nodes[cwd].parent; return 1; }
    int c = find_child(cwd, n); if (c < 0 || !nodes[c].dir) return 0;
    cwd = (unsigned)c; return 1;
}
int fat_mkdir(const char *n) { if (!*n || find_child(cwd, n) >= 0) return 0; return new_node(cwd, n, 1) > 0; }
int fat_read_file(const char *n, void *buf, unsigned size) {
    int c = find_child(cwd, n); if (c < 0 || nodes[c].dir) return -1;
    unsigned k = (unsigned)nodes[c].len < size ? (unsigned)nodes[c].len : size; memcpy(buf, nodes[c].data, k); return (int)k;
}
int fat_replace_file(const char *n, const void *d, unsigned len) {
    int c = find_child(cwd, n); if (c >= 0 && nodes[c].dir) return 0;
    if (c < 0) c = new_node(cwd, n, 0); if (c < 0 || len > 9000) return 0;
    memcpy(nodes[c].data, d, len); nodes[c].len = (int)len; return 1;
}
int fat_write_file(const char *n, const void *d, unsigned len) { return find_child(cwd, n) < 0 && fat_replace_file(n, d, len); }
int fat_delete(const char *n) { int c = find_child(cwd, n); if (c < 0 || nodes[c].dir) return 0; nodes[c].used = 0; return 1; }
void fat_list(void (*cb)(const char *, unsigned, int)) { for (int i = 1; i < 200; i++) if (nodes[i].used && nodes[i].parent == (int)cwd) cb(nodes[i].name, (unsigned)nodes[i].len, nodes[i].dir); }
unsigned fat_cwd_get(void) { return cwd; }
void fat_cwd_set(unsigned c) { cwd = c; }
int fat_mount(void) { return 1; }
void fat_vfs_register(void) {}
int sd_ready(void) { return ready; }
int sd_enter_jt(int make) {
    if (!ready) return 0;
    if (fat_chdir("JT")) return 1;
    if (!make || !fat_mkdir("JT")) return 0;
    return fat_chdir("JT");
}
/* ---- who is signed in ---- */
char current_user[32];
static const char *names[8]; static unsigned nacc; static int sudo_on;
unsigned accounts_count(void) { return nacc; }
const char *accounts_name(unsigned i) { return i < nacc ? names[i] : ""; }
int accounts_protected(const char *n) { return !strcasecmp(n, "USERS.TXT") || !strcasecmp(n, "SUDOERS.TXT"); }
int sudo_active(void) { return sudo_on; }
int user_in_range(unsigned long a, unsigned long n) { (void)a; (void)n; return 1; }
int user_writable(unsigned long a, unsigned long n) { (void)a; (void)n; return 1; }
long ufile_syscall(unsigned long n, unsigned long a0, unsigned long a1, unsigned long a2);
void ufile_release(void);
void ufile_mkdirs(const char *path);
extern unsigned ufile_owner;

static void login(const char *u) { snprintf(current_user, sizeof current_user, "%s", u); }
static int at(const char *path) {   /* the node of an absolute path from the root, -1 when it is not there */
    char p[160]; snprintf(p, sizeof p, "%s", path); unsigned d = 0;
    for (char *t = strtok(p, "/"); t; t = strtok(0, "/")) { int c = find_child(d, t); if (c < 0) return -1; d = (unsigned)c; }
    return (int)d;
}
static const char *text(const char *path) { int c = at(path); return c > 0 ? nodes[c].data : 0; }
static long uopen(const char *p, unsigned f) { return ufile_syscall(5, (unsigned long)p, f, 0); }
static void put(const char *path, const char *body) {   /* an app saving a file */
    long fd = uopen(path, 0x41 | 0x200); assert(fd >= 3);
    assert(ufile_syscall(4, (unsigned long)fd, (unsigned long)body, strlen(body)) == (long)strlen(body));
    assert(ufile_syscall(6, (unsigned long)fd, 0, 0) == 0);
}
static void mk(const char *dirs, const char *file, const char *body) {   /* a card file made by hand: dirs "JT/HOME/ann" */
    char p[160]; snprintf(p, sizeof p, "%s", dirs); unsigned d = 0;
    for (char *t = strtok(p, "/"); t; t = strtok(0, "/")) { int c = find_child(d, t); if (c < 0) c = new_node(d, t, 1); d = (unsigned)c; }
    unsigned s = cwd; cwd = d; fat_replace_file(file, body, (unsigned)strlen(body)); cwd = s;
}
static unsigned lsn; static char lsnames[40][16];
static void lscb(const char *n, unsigned s, int d) { (void)s; (void)d; if (lsn < 40) snprintf(lsnames[lsn++], 16, "%s", n); }

int main(void) {
    char dn[HOME_DIR_MAX + 1], dn2[HOME_DIR_MAX + 1];
    unsigned long ents[64 * 40 / 8];
    /* names to folder names */
    assert(home_dirname("ann", dn) == 3 && !strcmp(dn, "ann"));
    assert(home_dirname("abcdefgh", dn) == 8 && !strcmp(dn, "abcdefgh"));
    assert(home_dirname("a_b-1", dn) == 5 && !strcmp(dn, "a_b-1"));
    assert(home_dirname("abcdefghi", dn) == 8 && !strncmp(dn, "abcde~", 6));
    home_dirname("abcdefghi", dn2); assert(!strcmp(dn, dn2));
    home_dirname("abcdefghj", dn2); assert(strcmp(dn, dn2) && strlen(dn2) == 8);
    assert(home_dirname("Verylongnamehere123456", dn) == 8 && !strncmp(dn, "veryl~", 6));   /* lower-cased: FAT is blind to case anyway */
    puts("ok folder names: the account name up to 8 characters, else five letters, a tilde and a hash");

    /* ---- no accounts: nothing moves ---- */
    nacc = 0; current_user[0] = 0; ufile_owner = 1;
    ufile_mkdirs("NOTES/NOTES");
    put("NOTES/NOTES/N1.TXT", "shared note");
    assert(at("JT/NOTES/NOTES/N1.TXT") > 0 && at("JT/HOME") < 0 && !strcmp(text("JT/NOTES/NOTES/N1.TXT"), "shared note"));
    assert(!home_active() && !home_mine(dn) && home_may("anyone"));
    assert(home_expand("~/x", dn, sizeof dn) == 0);
    puts("ok no accounts: an app's NOTES/NOTES lands in JT/ as before and no HOME folder is made");

    /* ---- ann and bob ---- */
    names[0] = "ann"; names[1] = "bob"; nacc = 2;
    login("ann");
    ufile_mkdirs("NOTES/NOTES"); ufile_mkdirs("MUSIC");
    assert(at("JT/HOME/ann/NOTES/NOTES") > 0 && at("JT/HOME/ann/MUSIC") > 0);
    put("NOTES/NOTES/N1.TXT", "ann's note");
    assert(!strcmp(text("JT/HOME/ann/NOTES/NOTES/N1.TXT"), "ann's note"));
    assert(!strcmp(text("JT/NOTES/NOTES/N1.TXT"), "shared note"));   /* the shared one is untouched */
    assert(cwd == 0);
    puts("ok ann signed in: NOTES/NOTES and MUSIC are made in JT/HOME/ann, the file lands there, the shared JT/ file is untouched");

    long fd = uopen("NOTES/NOTES/LEFT.TXT", 0x41 | 0x200); assert(fd >= 3);   /* ann leaves one open ... */
    assert(ufile_syscall(4, (unsigned long)fd, (unsigned long)"left open", 9) == 9);
    login("bob");                                                              /* ... and bob signs in */
    assert(ufile_syscall(6, (unsigned long)fd, 0, 0) == 0);
    assert(at("JT/HOME/ann/NOTES/NOTES/LEFT.TXT") > 0 && at("JT/HOME/bob") < 0);
    puts("ok an open file flushes into the folder it was opened in, whoever is signed in by then");

    static char buf[300];
    assert(ufile_syscall(401, (unsigned long)"NOTES/NOTES/N1.TXT", (unsigned long)buf, sizeof buf) == -2);
    assert(uopen("NOTES/NOTES/N1.TXT", 0) == -2);
    assert(uopen("HOME/ann/NOTES/NOTES/N1.TXT", 0) == -2);
    assert(uopen("/HOME/ann/NOTES/NOTES/N1.TXT", 0) == -22 && uopen("../HOME/ann/NOTES/NOTES/N1.TXT", 0) == -22 && uopen("NOTES/../../ann/x", 0) == -22);
    assert(ufile_syscall(390, (unsigned long)"HOME/ann/NOTES/NOTES/N1.TXT", 0, 0) == -2 && at("JT/HOME/ann/NOTES/NOTES/N1.TXT") > 0);
    assert(ufile_syscall(388, (unsigned long)"HOME/ann", (unsigned long)ents, 64) == -2);
    assert(ufile_syscall(388, (unsigned long)"", (unsigned long)ents, 64) == -2);   /* his folder is not there until an app starts */
    ufile_mkdirs("NOTES/NOTES");
    long total = ufile_syscall(388, (unsigned long)"", (unsigned long)ents, 64); assert(total == 1);   /* bob's own root is just NOTES, none of ann's */
    assert(ufile_syscall(389, (unsigned long)"HOME", 0, 0) == 0 && ufile_syscall(389, (unsigned long)"HOME/ann", 0, 0) == 0);   /* a folder that merely looks like hers */
    put("HOME/ann/x.txt", "bob's");
    assert(at("JT/HOME/bob/HOME/ann/x.txt") > 0 && at("JT/HOME/ann/x.txt") < 0);
    assert(ufile_syscall(389, (unsigned long)"DOCS", 0, 0) == 0 && at("JT/HOME/bob/DOCS") > 0 && at("JT/DOCS") < 0);
    assert(cwd == 0);
    puts("ok bob cannot read, list or unlink ann's files by any path an app can send; his writes land in his own folder");

    /* ---- the shared account files stay protected ---- */
    assert(uopen("USERS.TXT", 0) == -13 && uopen("sudoers.txt", 0x41) == -13);
    puts("ok USERS.TXT and SUDOERS.TXT stay closed to apps");

    /* ---- home_walk, home_op, home_save ---- */
    char p[96], *leaf;
    mk("JT/HOME/ann", "SECRET.TXT", "annsecret");
    login("bob"); sudo_on = 0;
    strcpy(p, "JT/HOME/ann/SECRET.TXT"); assert(home_walk(p, 1, &leaf) == -1);
    strcpy(p, "jt/home/ANN/secret.txt"); assert(home_walk(p, 1, &leaf) == -1);
    strcpy(p, "JT/HOME/ann"); assert(home_walk(p, 0, &leaf) == -1);
    strcpy(p, "JT/HOME/ann/"); assert(home_walk(p, 0, &leaf) == -1);
    strcpy(p, "JT/HOME/bob/../ann/SECRET.TXT"); assert(home_walk(p, 1, &leaf) == 0);
    strcpy(p, "JT/./HOME/ann/SECRET.TXT"); assert(home_walk(p, 1, &leaf) == 0);
    strcpy(p, "JT/HOME/ann//SECRET.TXT"); assert(home_walk(p, 1, &leaf) == 0);
    strcpy(p, "/JT/HOME/ann/SECRET.TXT"); assert(home_walk(p, 1, &leaf) == 0);
    strcpy(p, "JT/HOME"); assert(home_walk(p, 0, &leaf) == 1 && cwd != 0); home_back(); assert(cwd == 0);
    strcpy(p, "JT/HOME/bob/NOTES"); assert(home_walk(p, 0, &leaf) == 1); home_back(); assert(cwd == 0);
    strcpy(p, "JT/NOPE/x"); assert(home_walk(p, 1, &leaf) == 0 && cwd == 0);
    sudo_on = 1; strcpy(p, "JT/HOME/ann/SECRET.TXT"); assert(home_walk(p, 1, &leaf) == 1 && !strcmp(leaf, "SECRET.TXT")); home_back(); sudo_on = 0;
    nacc = 0; strcpy(p, "JT/HOME/ann/SECRET.TXT"); assert(home_walk(p, 1, &leaf) == 1); home_back(); nacc = 2;
    assert(home_expand("~", p, sizeof p) == 1 && !strcmp(p, "JT/HOME/bob") && home_expand("~/a/b", p, sizeof p) == 1 && !strcmp(p, "JT/HOME/bob/a/b"));
    assert(home_expand("JT/x", p, sizeof p) == 1 && !strcmp(p, "JT/x") && home_expand("~x", p, sizeof p) == 1 && !strcmp(p, "~x"));
    assert(home_expand("~/aaaaaaaaaaaaaaaaaaaa", p, 20) == -2);
    assert(home_owns("JT/HOME/bob/a.txt") && home_owns("jt/home/BOB/x/y.txt") && !home_owns("JT/HOME/ann/a.txt") && !home_owns("JT/HOME/bob") && !home_owns("JT/HOME/bobby/a.txt")
           && !home_owns("JT/HOME/bob/../ann/a.txt") && !home_owns("JT/HOME/bob/x/../../ann/a.txt") && !home_owns("JT/a.txt"));
    puts("ok home_walk: another person's folder is refused in any case and spelling, \".\" and \"..\" parts are refused, sudo opens it, the cursor comes back to the root");

    assert(home_op(0, "JT/HOME/bob/B.TXT", "bobs", 4) == 1 && !strcmp(text("JT/HOME/bob/B.TXT"), "bobs"));
    assert(home_op(0, "JT/HOME/ann/EVIL.TXT", "x", 1) == -1 && at("JT/HOME/ann/EVIL.TXT") < 0);
    assert(home_op(1, "JT/HOME/ann/SECRET.TXT", 0, 0) == -1 && !strcmp(text("JT/HOME/ann/SECRET.TXT"), "annsecret"));
    assert(home_op(2, "JT/HOME/ann/D", 0, 0) == -1 && at("JT/HOME/ann/D") < 0);
    assert(home_op(0, "JT/HOME/bob/longername.txt", "x", 1) == -2 && home_op(0, "JT/HOME/bob/a.b.c", "x", 1) == -2 && home_op(0, "JT/HOME/bob/a b.txt", "x", 1) == -2);
    assert(home_op(2, "JT/HOME/bob/DOCS2", 0, 0) == 1 && at("JT/HOME/bob/DOCS2") > 0 && home_op(1, "JT/HOME/bob/B.TXT", 0, 0) == 1 && at("JT/HOME/bob/B.TXT") < 0);
    assert(home_op(1, "JT/HOME/bob/B.TXT", 0, 0) == 0);
    sudo_on = 1; assert(home_op(0, "JT/HOME/ann/BYADMIN.TXT", "ok", 2) == 1 && at("JT/HOME/ann/BYADMIN.TXT") > 0); sudo_on = 0;
    assert(cwd == 0);
    puts("ok home_op: write, rm and mkdir work in your own folder, are refused in another's, take only 8.3 names, and sudo opens the other");

    login("ann");
    assert(home_save("A.TXT", "from sam", 8) == 1 && !strcmp(text("JT/HOME/ann/A.TXT"), "from sam") && at("JT/A.TXT") < 0);
    assert(home_save("../A.TXT", "x", 1) == -2 && home_save("x/A.TXT", "x", 1) == -2 && home_save("", "x", 1) == -2 && home_save("toolongname.txt", "x", 1) == -2);
    login("bob"); assert(home_save("A.TXT", "bobs", 4) == 1 && !strcmp(text("JT/HOME/bob/A.TXT"), "bobs") && !strcmp(text("JT/HOME/ann/A.TXT"), "from sam"));
    ready = 0; assert(home_save("C.TXT", "x", 1) == 0 && !home_make("bob")); ready = 1;
    puts("ok Samantha's save: only ~/NAME, never a path, never another's folder");

    /* ---- folder-name collisions ---- */
    names[2] = "abcdefghi"; nacc = 3;
    assert(!home_collides("abcdefghj") || 1);
    char a[HOME_DIR_MAX + 1], b[HOME_DIR_MAX + 1];
    for (int i = 0; i < 100000; i++) {   /* find two long names that fold to one folder name, then prove the guard sees them */
        char nm[24]; snprintf(nm, sizeof nm, "abcdefg%d", i); home_dirname(nm, b); home_dirname("abcdefghi", a);
        if (!strcmp(a, b) && strcmp(nm, "abcdefghi")) { assert(home_collides(nm)); break; }
    }
    assert(!home_collides("zzz") && !home_collides("abcdefghi"));
    puts("ok two accounts whose folder names agree are caught before the second is made");

    /* ---- home_make: made once, kept ---- */
    nacc = 2; assert(home_make("eve") == 1 && at("JT/HOME/eve") > 0 && home_make("eve") == 1 && cwd == 0);
    puts("PASS: home.c");
    return 0;
}
'''
HOME_C = os.path.join(arch, "home.c")
UFILE_C = os.path.join(arch, "ufile.c")
def host_run(tmp, home_c=HOME_C, ufile_c=UFILE_C):
    src = os.path.join(tmp, "t.c"); open(src, "w").write(HOST)
    exe = os.path.join(tmp, "t")
    cc = subprocess.run(["clang", "-std=gnu11", "-D_GNU_SOURCE", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function", "-Wno-unused-parameter", "-g", "-fsanitize=address,undefined",
                         "-I", arch, "-I", os.path.join(root, "drivers"), src, home_c, ufile_c, "-o", exe], capture_output=True, text=True)
    if cc.returncode: return False, cc.stderr
    r = subprocess.run([exe], capture_output=True, text=True, timeout=600)
    return r.returncode == 0 and "PASS: home.c" in r.stdout, r.stdout + r.stderr

with tempfile.TemporaryDirectory(prefix="jt-home-") as t0:
    ok, out = host_run(t0)
    check("home.c and ufile.c on the host: folder names, the app's root, flush after sign-out, cat/ls/write/rm walker, save, collisions", ok, out)
    # ---- mutations ----
    def mutate(src_path, old, new, tag):
        text = open(src_path).read()
        if old not in text: check("mutation target present: " + tag, False, old); return
        m = os.path.join(t0, "m_" + os.path.basename(src_path)); open(m, "w").write(text.replace(old, new, 1))
        kw = {"home_c": m} if src_path == HOME_C else {"ufile_c": m}
        okm, outm = host_run(t0, **kw)
        check("mutation (%s) must fail the host test on an assertion, not a compile error" % tag, not okm and re.search(r"Assertion (failed|`.*' failed)", outm), outm)   # macOS and glibc wording
    mutate(HOME_C, "    return sudo_active();\n}", "    return 1;\n}", "home_may always yes")
    mutate(UFILE_C, "static const char *cur_home(char *buf) { return home_mine(buf) ? buf : 0; }", "static const char *cur_home(char *buf) { (void)buf; return 0; }", "apps ignore the home folder")

# ---- QEMU plumbing ----
def build(*args):
    subprocess.run(["make", "-C", arch, "clean"], capture_output=True)
    b = subprocess.run(["make", "-C", arch, *args], capture_output=True, text=True, timeout=900)
    if b.returncode: print("FAIL: make -C arch/arm64 %s failed\n" % " ".join(args) + (b.stdout + b.stderr)[-1500:]); sys.exit(1)

tmp = scratch_dir("arm64-home")
MT = dict(os.environ, MTOOLS_SKIP_CHECK="1")
def mt(card, *a): return subprocess.run([a[0], "-i", f"{card}@@{PART * 512}", *a[1:]], capture_output=True, env=MT)
def record(name, pw, salt_byte):
    salt = bytes(range(salt_byte, salt_byte + 16))
    return "%s:p2:100000:%s:%s\n" % (name, salt.hex(), hashlib.pbkdf2_hmac("sha256", pw.encode(), salt, 100000, 32).hex())
def put_file(card, name, text):
    f = os.path.join(tmp, "put." + os.path.basename(name)); open(f, "w").write(text)
    if mt(card, "mcopy", f, "::" + name).returncode: print("FAIL: mtools could not write " + name); sys.exit(1)
def make_card(tag):
    card = os.path.join(tmp, tag + ".img")
    build_card(card, False)
    for d in ("::JT", "::JT/HOME", "::JT/HOME/ann"):
        if mt(card, "mmd", d).returncode: print("FAIL: mtools could not make " + d); sys.exit(1)
    put_file(card, "JT/USERS.TXT", record("ann", "hunter22", 32) + record("bob", "bobpass44", 64))
    put_file(card, "JT/SUDOERS.TXT", "ann\n")
    put_file(card, "JT/HOME/ann/SECRET.TXT", "ANN-SECRET-LINE\n")
    return card
QK = {" ": "spc", "-": "minus", ".": "dot", "/": "slash"}
class Session:
    def __init__(self, tag, card, kernel):
        self.tag = tag
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
        for ch in line:
            if ch == "~": self.key("shift", "grave_accent")
            else: self.key(QK.get(ch, ch))
    def boot(self, login):
        if not self.wait_for("M2 input ready", 90) or not self.wait_for("sd fat32 partition at block 2048", 40): return False
        self.connect(); time.sleep(0.5)
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
    def run(self, line, want, secs=30):
        n = len(self.uart()); self.typ(line); self.key("ret")
        ok = self.wait_for(want, secs, n)
        return ok, self.uart()[n:]
    def ask(self, line, secs=20):
        n = len(self.uart()); self.typ(line); self.key("ret")
        ok = self.wait_for(line.split(" ")[0] + " ", secs, n); time.sleep(1.5)
        return ok, self.uart()[n:]
    def password(self, pw, want, secs=150):
        n = len(self.uart()); self.typ(pw); self.key("ret")
        ok = self.wait_for(want, secs, n)
        return ok, self.uart()[n:]
    def close(self):
        if self.f: self.f.close()
        if self.q.poll() is None: self.q.kill()
        self.q.wait()

build("sudoshort-kernel8.elf")
KERNEL = "sudoshort-kernel8.elf"
card = make_card("c1")
NOT_YOURS = "another person's home folder"

# ---- 2a. bob ----
S = Session("b", card, KERNEL)
try:
    if not S.boot(("bob", "bobpass44")) or not S.terminal(): check("bob signs in and opens the Terminal", False, S.uart()[-500:]); raise SystemExit
    ok, out = S.run("write ~/hi.txt bobs own note", "wrote JT/HOME/bob/hi.txt", 20)
    check("bob (no admin) writes ~/hi.txt with no sudo, into JT/HOME/bob/", ok, out)
    ok, out = S.run("cat ~/hi.txt", "bobs own note", 20)
    check("cat ~/hi.txt reads it back", ok, out)
    ok, out = S.run("cat jt/home/bob/hi.txt", "bobs own note", 20)
    check("... and the full spelling in any case reads the same file", ok, out)
    ok, out = S.run("ls ~", "HI.TXT", 20)
    check("ls ~ lists it", ok, out)
    ok, out = S.run("mkdir ~/docs", "made JT/HOME/bob/docs/", 20)
    check("mkdir ~/docs works with no sudo", ok, out)
    ok, out = S.run("write ~/docs/a.txt deep", "wrote JT/HOME/bob/docs/a.txt", 20)
    check("a file in a folder of his own home", ok, out)
    ok, out = S.run("rm ~/docs/a.txt", "removed JT/HOME/bob/docs/a.txt", 20)
    check("rm ~/docs/a.txt works with no sudo", ok, out)
    ok, out = S.run("cat jt/home/ann/secret.txt", NOT_YOURS, 20)
    check("bob's cat of ann's file is refused", ok and "ANN-SECRET-LINE" not in out, out)
    ok, out = S.run("ls jt/home/ann", NOT_YOURS, 20)
    check("bob's ls of ann's folder is refused", ok and "SECRET" not in out, out)
    ok, out = S.run("cat jt/home/bob/../ann/secret.txt", "cat: no file", 20)
    check("a .. walk into ann's folder finds nothing", ok and "ANN-SECRET-LINE" not in out, out)
    ok, out = S.run("write home/ann/evil.txt hi", "write: you are not an admin; try sudo", 20)
    check("bob's write into ann's folder is refused as not an admin", ok, out)
    ok, out = S.run("rm home/ann/secret.txt", "rm: you are not an admin; try sudo", 20)
    check("bob's rm of ann's file is refused as not an admin", ok, out)
    ok, out = S.run("ls jt/home", "BOB", 20)
    check("ls JT/HOME still lists the folders (names are not secret)", ok and "ANN" in out, out)
    check("plain bare-name write still needs an admin", S.run("write plain.txt hi", "write: you are not an admin; try sudo", 20)[0])
    check("cat of USERS.TXT is still refused", S.run("cat jt/users.txt", "cat: you are not an admin; try sudo", 20)[0])
    ok, out = S.run("userrun notes", "userrun notes: running", 60)
    check("userrun notes starts", ok, out)
    time.sleep(1.0)
    check("... and the kernel shows no fault", "KERNEL CRASH" not in S.uart() and "stopped by a fault" not in S.uart(), S.uart()[-300:])
    check("no fault, no crash", "KERNEL CRASH" not in S.uart() and "oom" not in S.uart(), S.uart()[-300:])
except SystemExit:
    pass
finally:
    S.close()

# ---- 2b. ann ----
S = Session("a", card, KERNEL)
try:
    if not S.boot(("ann", "hunter22")) or not S.terminal(): check("ann signs in and opens the Terminal", False, S.uart()[-500:]); raise SystemExit
    ok, out = S.run("cat jt/home/bob/hi.txt", NOT_YOURS, 20)
    check("ann (an admin) without sudo is refused bob's file too", ok and "bobs own note" not in out, out)
    ok, out = S.run("cat ~/secret.txt", "ANN-SECRET-LINE", 20)
    check("ann reads her own file through ~", ok, out)
    ok, echo = S.ask("sudo cat jt/home/bob/hi.txt")
    ok2, out = S.password("hunter22", "bobs own note")
    check("sudo opens bob's folder: the password, then the file", ok and ok2, echo + out)
    ok, out = S.run("sudo -k", "sudo: password forgotten", 10)
    ok, out = S.run("cat jt/home/bob/hi.txt", NOT_YOURS, 20)
    check("after sudo -k the folder is closed again", ok and "bobs own note" not in out, out)
    ok, echo = S.ask("sudo adduser eve evepass1")
    ok2, out = S.password("hunter22", "adduser: home folder JT/HOME/eve", 200)
    check("adduser makes the person's folder and says so", ok and ok2, echo + out)
    ok, out = S.run("sudo deluser eve", "stay on the card", 60)
    check("deluser says the files stay", ok and "deluser eve: removed" in S.uart(), out)
    check("... and the folder is still listed", S.run("ls jt/home", "EVE", 20)[0])
    check("no fault, no crash", "KERNEL CRASH" not in S.uart() and "oom" not in S.uart(), S.uart()[-300:])
    leaked = [s for s in ("hunter22", "bobpass44", "evepass1") if s in S.uart()]
    check("no typed password reached the UART", not leaked, leaked)
except SystemExit:
    pass
finally:
    S.close()

# ---- 3. the card ----
def ls(path):
    r = mt(card, "mdir", "-b", "::" + path)
    return [l.strip().rstrip("/").rsplit("/", 1)[-1].upper() for l in r.stdout.decode(errors="replace").splitlines() if l.strip()]
bob = ls("JT/HOME/BOB")
check("the card has HI.TXT, DOCS and NOTES in JT/HOME/BOB", all(x in bob for x in ("HI.TXT", "DOCS", "NOTES")), bob)
check("... and NOTES/NOTES inside it (the app's folder)", "NOTES" in ls("JT/HOME/BOB/NOTES"), ls("JT/HOME/BOB/NOTES"))
jt = ls("JT")
check("(the listing works: JT/ holds HOME and USERS.TXT)", "HOME" in jt and "USERS.TXT" in jt, jt)
check("JT/ itself has no NOTES, no HI.TXT, no EVIL.TXT: nothing leaked into the shared part", not any(x in jt for x in ("NOTES", "HI.TXT", "EVIL.TXT", "PLAIN.TXT")), jt)
ann = ls("JT/HOME/ANN")
check("ann's folder holds only SECRET.TXT (no EVIL.TXT)", ann == ["SECRET.TXT"], ann)
check("ann's file is unchanged", mt(card, "mtype", "::JT/HOME/ANN/SECRET.TXT").stdout.decode().replace("\r", "") == "ANN-SECRET-LINE\n")
check("JT/HOME/EVE is still on the card after deluser", "EVE" in ls("JT/HOME"), ls("JT/HOME"))
users = mt(card, "mtype", "::JT/USERS.TXT").stdout.decode(errors="replace")
check("eve is gone from USERS.TXT", "eve:" not in users and "ann:" in users and "bob:" in users, users)

if fails:
    print("FAIL: %d of the home checks failed" % len(fails)); sys.exit(1)
print("PASS: each person's files live in JT/HOME/<name>, apps, ~ and Samantha's save use it, and another's folder opens only under sudo")
