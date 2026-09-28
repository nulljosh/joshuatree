/* Native host harness for kernel/auth.h, run by tools/checks/auth-check.sh.
   Same relationship tools/png-host/main.c has to drivers/png.c: this
   compiles the real kernel/auth.h header natively (fast iteration, no
   QEMU boot), while the in-kernel path (auth_gate wired into gui_run,
   exercised by every real boot) is what actually ships.

   auth.h calls a handful of kernel-only functions (VFS, GUI draw calls,
   ticks()) that only the login/first-run screens use; this harness never
   calls those screens, but still needs real declarations in scope for
   auth.h's function bodies to compile. VFS gets a real, tiny in-memory
   implementation below (so USERS.TXT persistence is actually exercised
   end to end, not just declared away); the GUI draw calls are declared
   but never invoked by anything this harness calls, so they're linked
   only if referenced -- they aren't. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "libc.h" /* the kernel's own memcpy/memset/memcmp/strlen/strcmp subset, shimmed to real string.h on host */

/* ---- minimal stand-ins for the kernel functions auth.h references ---- */
static unsigned int fake_ticks = 1;
unsigned int ticks(void) { return fake_ticks++; }
/* kernel/entropy.c stand-in: the host harness tests hashing and parsing,
   not the DRBG, so a counter is enough to keep salts distinct. */
static void entropy_bytes(void *buf, unsigned int n) { unsigned char *b = buf; for (unsigned int i = 0; i < n; i++) b[i] = (unsigned char)(fake_ticks++ * 97u + i); }

#define FAKE_VFS_MAX 4096
static char fake_vfs_buf[FAKE_VFS_MAX];
static int fake_vfs_len = -1; /* -1: no file written yet */

int vfs_read_file(const char *name, void *buf, unsigned int bufsize) {
    (void)name;
    if (fake_vfs_len < 0) return -1;
    unsigned int n = (unsigned int)fake_vfs_len;
    if (n > bufsize) n = bufsize;
    memcpy(buf, fake_vfs_buf, n);
    return (int)n;
}
int vfs_replace_file(const char *name, const void *data, unsigned int len) {
    (void)name;
    if (len > FAKE_VFS_MAX) return 0;
    memcpy(fake_vfs_buf, data, len);
    fake_vfs_len = (int)len;
    return 1;
}

/* Declared so auth.h's (unused-by-this-harness) GUI functions compile;
   never actually called from main() below, so never actually linked in
   a way that requires real behavior. */
void window_clear(unsigned int color) { (void)color; }
void window_rect(int x, int y, int w, int h, unsigned int color) { (void)x; (void)y; (void)w; (void)h; (void)color; }
/* v0.77.x auth-flow fix: the login-rejection screen now calls this (see
   auth.h) so its message actually reaches the visible framebuffer before
   sleep_ticks -- a real in-kernel bug tools/checks/auth-flow-check.py
   found, drivers/window.c's own header comment already required this
   call at exactly that kind of frame boundary. Declared here so this
   host harness keeps compiling. */
void window_present(void) {}
unsigned int window_width(void) { return 960; }
unsigned int window_height(void) { return 540; }
void font_draw_string(const char *s, int x, int y, unsigned int fg, int bg) { (void)s; (void)x; (void)y; (void)fg; (void)bg; }
void mouse_click_edge_sync(void) {}
void sleep_ticks(unsigned int n) { (void)n; }
void klog(const char *msg) { (void)msg; }
static void gui_draw_app_titlebar(const char *title) { (void)title; }
static int get_key_or_click(void) { return 0; }
#define KEY_UP    256
#define KEY_DOWN  257
#define KEY_ENTER 258
#define KEY_ESC   259
#define KEY_CLICK 260
#define GUI_BG 0x00FAF8F6

#include "auth.h"

static int fails = 0;
static void check(const char *name, int ok) {
    printf("%s: %s\n", name, ok ? "ok" : "FAIL");
    if (!ok) fails++;
}

int main(void) {
    /* --- SHA-256 against the real FIPS 180-4 published test vectors --- */
    unsigned char digest[32];
    char hex[65];

    sha256_digest("", 0, digest);
    auth_to_hex(digest, 32, hex);
    check("sha256(\"\") matches FIPS 180-4 empty-string vector",
          !strcmp(hex, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));

    sha256_digest("abc", 3, digest);
    auth_to_hex(digest, 32, hex);
    check("sha256(\"abc\") matches FIPS 180-4 vector",
          !strcmp(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));

    /* Also the one from FIPS 180-4's second published vector, a longer
       multi-block message, so the padding/block-boundary path (not just
       the single-short-block path "abc" exercises) is proven too. */
    const char *long_msg = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    sha256_digest(long_msg, strlen(long_msg), digest);
    auth_to_hex(digest, 32, hex);
    check("sha256(56-byte multi-block message) matches FIPS 180-4 vector",
          !strcmp(hex, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));

    /* --- PBKDF2-HMAC-SHA256 (kernel/auth_kdf.c) against published vectors:
       RFC 7914 section 11's two SHA-256 PBKDF2 vectors, then the RFC 6070
       set recomputed for SHA-256 (the values every mainstream library
       ships as its own test set). Covers c=1, c=2, c=4096, c=80000, a
       two-block dkLen=64 output and a 40-byte partial second block. --- */
    struct { const char *pw, *salt; unsigned int c, dklen; const char *hex; } pv[] = {
        {"passwd", "salt", 1, 64, "55ac046e56e3089fec1691c22544b605f94185216dde0465e68b9d57c20dacbc49ca9cccf179b645991664b39d77ef317c71b845b1e30bd509112041d3a19783"},
        {"Password", "NaCl", 80000, 64, "4ddcd8f60b98be21830cee5ef22701f9641a4418d04c0414aeff08876b34ab56a1d425a1225833549adb841b51c9b3176a272bdebba1d078478f62b397f33c8d"},
        {"password", "salt", 1, 32, "120fb6cffcf8b32c43e7225256c4f837a86548c92ccc35480805987cb70be17b"},
        {"password", "salt", 2, 32, "ae4d0c95af6b46d32d0adff928f06dd02a303f8ef3c251dfd6e2d85a95474c43"},
        {"password", "salt", 4096, 32, "c5e478d59288c841aa530db6845c4c8d962893a001ce4e11a4963873aa98134a"},
        {"passwordPASSWORDpassword", "saltSALTsaltSALTsaltSALTsaltSALTsalt", 4096, 40, "348c89dbcbd32b2f32d814b8116e84cf2b17347ebc1800181c4e2a1fb8dd53e1c635518c7dac47e9"},
    };
    for (unsigned int i = 0; i < sizeof(pv)/sizeof(pv[0]); i++) {
        unsigned char dk[64]; char dkhex[129]; char name[96];
        auth_pbkdf2_sha256(pv[i].pw, (unsigned int)strlen(pv[i].pw), pv[i].salt, (unsigned int)strlen(pv[i].salt), pv[i].c, dk, pv[i].dklen);
        auth_to_hex(dk, pv[i].dklen, dkhex);
        snprintf(name, sizeof name, "pbkdf2-sha256(\"%s\", \"%s\", c=%u, dkLen=%u) matches published vector", pv[i].pw, pv[i].salt, pv[i].c, pv[i].dklen);
        check(name, !strcmp(dkhex, pv[i].hex));
    }

    /* --- legacy record migration: a pre-1.7.9 USERS.TXT line (no scheme
       tag, chained SHA-256) must still log in, and that login must rewrite
       it as a tagged PBKDF2 record that then verifies on a fresh load. --- */
    {
        unsigned char lsalt[AUTH_SALT_LEN], lhash[AUTH_HASH_LEN];
        for (int i = 0; i < AUTH_SALT_LEN; i++) lsalt[i] = (unsigned char)(0xA0 + i);
        auth_hash_legacy(lsalt, "old style pw", lhash);
        char shex[AUTH_SALT_LEN*2+1], hhex[AUTH_HASH_LEN*2+1], line[256];
        auth_to_hex(lsalt, AUTH_SALT_LEN, shex); auth_to_hex(lhash, AUTH_HASH_LEN, hhex);
        snprintf(line, sizeof line, "legacy:%s:%s\n", shex, hhex);
        fake_vfs_len = (int)strlen(line); memcpy(fake_vfs_buf, line, (size_t)fake_vfs_len);
        auth_users_loaded = 0; auth_user_count = 0; auth_upgraded_count = 0;
        auth_users_load();
        check("legacy (untagged) USERS.TXT record loads with iters == 0", auth_user_count == 1 && auth_users[0].iters == 0);
        check("legacy record: wrong password rejected", !auth_verify("legacy", "old style pX"));
        check("legacy record: not upgraded by a failed login", auth_upgraded_count == 0 && strstr(fake_vfs_buf, ":p2:") == NULL);
        check("legacy record: right password accepted", auth_verify("legacy", "old style pw"));
        check("legacy record: upgraded on that login (counter, tagged line on disk, old line gone)",
              auth_upgraded_count == 1 && strncmp(fake_vfs_buf, "legacy:p2:100000:", 17) == 0 && strstr(fake_vfs_buf, hhex) == NULL);
        auth_users_loaded = 0; auth_user_count = 0;
        check("upgraded record reloads from disk as PBKDF2 and still accepts the password",
              auth_verify("legacy", "old style pw") && auth_users[0].iters == AUTH_PBKDF2_ITERS);
        check("upgraded record still rejects a wrong password", !auth_verify("legacy", "old style pw "));
        check("upgrade happens exactly once", auth_upgraded_count == 1);
        const char *badtag = "x:p3:100000:00112233445566778899aabbccddeeff00:"
                             "aabbccddeeff00112233445566778899aabbccddeeff00112233445566778899\n"
                             "y:p2:12:00112233445566778899aabbccddeeff00:"
                             "aabbccddeeff00112233445566778899aabbccddeeff00112233445566778899\n";
        fake_vfs_len = (int)strlen(badtag); memcpy(fake_vfs_buf, badtag, (size_t)fake_vfs_len);
        auth_users_loaded = 0; auth_user_count = 0; auth_users_load();
        check("unknown scheme tag and out-of-range iteration count are both skipped", auth_user_count == 0);
    }

    /* --- account creation, wrong password rejected, right accepted --- */
    fake_vfs_len = -1; /* fresh "disk" */
    auth_users_loaded = 0; auth_user_count = 0; auth_logged_in = 0;

    check("auth_create_user succeeds for a fresh username/password",
          auth_create_user("joshua", "correcthorsebatterystaple"));
    check("new records are written tagged as PBKDF2 with the iteration count",
          strncmp(fake_vfs_buf, "joshua:p2:100000:", 17) == 0);
    check("auth_create_user refuses a duplicate username",
          !auth_create_user("joshua", "somethingelse"));
    check("auth_verify accepts the right password",
          auth_verify("joshua", "correcthorsebatterystaple"));
    check("auth_verify rejects a wrong password",
          !auth_verify("joshua", "wrongpassword"));
    check("auth_verify rejects a password that's a prefix of the real one",
          !auth_verify("joshua", "correcthorsebatterystapl"));
    check("auth_verify rejects an unknown username",
          !auth_verify("nobody", "correcthorsebatterystaple"));

    /* Reload straight from the fake USERS.TXT written above, proving the
       stored salt+hash actually round-trips through the real hex parse
       (auth_from_hex) and not just the in-memory table that just created
       it. */
    auth_users_loaded = 0; auth_user_count = 0;
    check("account persists across a fresh USERS.TXT load (right password)",
          auth_verify("joshua", "correcthorsebatterystaple"));
    check("account persists across a fresh USERS.TXT load (wrong password still rejected)",
          !auth_verify("joshua", "nope"));

    /* --- change password --- */
    check("auth_change_password refuses the wrong current password",
          !auth_change_password("joshua", "wrongcurrent", "newpassword"));
    check("auth_change_password succeeds with the right current password",
          auth_change_password("joshua", "correcthorsebatterystaple", "newpassword"));
    check("old password rejected after a real change",
          !auth_verify("joshua", "correcthorsebatterystaple"));
    check("new password accepted after a real change",
          auth_verify("joshua", "newpassword"));

    /* --- USERS.TXT bounds/parse hardening: hand-corrupt the "disk" and
       confirm a malformed line is skipped, not trusted or overrun --- */
    const char *bad = "onlyonefield\nname:nothexnothexnothexnothexnothexnothexnoth:aa\n"
                       "ok:0011223344556677889900112233445566778899001122334455667788:"
                       "aabbccddeeff00112233445566778899aabbccddeeff00112233445566778899\n";
    fake_vfs_len = (int)strlen(bad);
    memcpy(fake_vfs_buf, bad, (size_t)fake_vfs_len);
    auth_users_loaded = 0; auth_user_count = 0;
    auth_users_load();
    check("malformed USERS.TXT lines are skipped, not crashed on or trusted",
          auth_user_count == 0); /* every one of the 3 lines above is deliberately malformed (missing field, bad hex, hash field one char short) */

    /* --- opt-in gate: no accounts = no prompt, straight to desktop;
       one account = the gate really does engage --- */
    fake_vfs_len = -1; /* fresh "disk", no USERS.TXT at all */
    auth_users_loaded = 0; auth_user_count = 0; auth_logged_in = 0;
    check("auth_gate_would_prompt is false with no USERS.TXT (unconfigured system)",
          !auth_gate_would_prompt());

    auth_users_loaded = 0; auth_user_count = 0; auth_logged_in = 0;
    check("auth_create_user succeeds against an empty/no USERS.TXT",
          auth_create_user("first", "somepassword"));
    auth_users_loaded = 0; /* force a real reload from the "disk" auth_create_user just wrote, not the in-memory table */
    check("auth_gate_would_prompt is true once a real account exists",
          auth_gate_would_prompt());

    /* --- constant-time compare, real behavioral check not just a name --- */
    unsigned char a[4] = {1,2,3,4}, b[4] = {1,2,3,4}, c[4] = {1,2,3,5};
    check("auth_const_time_eq: identical buffers compare equal",
          auth_const_time_eq(a, b, 4));
    check("auth_const_time_eq: a single differing byte is still caught",
          !auth_const_time_eq(a, c, 4));

    if (fails) {
        printf("\n%d check(s) FAILED\n", fails);
        return 1;
    }
    printf("\nall auth checks passed\n");
    return 0;
}
