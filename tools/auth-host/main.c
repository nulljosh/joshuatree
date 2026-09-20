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

    /* --- account creation, wrong password rejected, right accepted --- */
    fake_vfs_len = -1; /* fresh "disk" */
    auth_users_loaded = 0; auth_user_count = 0; auth_logged_in = 0;

    check("auth_create_user succeeds for a fresh username/password",
          auth_create_user("joshua", "correcthorsebatterystaple"));
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
