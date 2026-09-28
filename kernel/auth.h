/* v0.77 (Sep 2026): real user accounts. docs/THREAT-MODEL.md was written
   first and gates everything here: this is a desktop lock for whoever is
   physically at the keyboard, backed by a real salted, iterated SHA-256
   hash instead of a fake one, not a claim of disk encryption or
   ring-0/ring-3 isolation (neither exists in this kernel yet).

   Header-only with static functions/state, the exact idiom reminders.h,
   contacts.h, calendar.h etc. already use: no separate .c, no libc
   beyond lib/libc.h (memcpy/memset/memcmp/strlen/strcmp), included from
   kernel.c after gui_prompt.h since login uses the shared prompt chrome.

   Storage: accounts persist in USERS.TXT through the same
   vfs_replace_file/vfs_read_file pair every other app-level file
   (REMINDERS.TXT, CONTACTS.TXT, SETTINGS.TXT, ...) already uses. One
   line per user, "username:p2:iterations:saltHex:hashHex\n" (PBKDF2,
   1.7.9+) or the untagged legacy "username:saltHex:hashHex\n" that a
   successful login upgrades in place. Never plaintext, and
   never logged: the password itself is never passed to klog/serial_puts
   anywhere in this file, only fixed strings like "auth: login ok" or
   "auth: login rejected" that name the *event*, not the *secret*. */

/* ---------------------------------------------------------------------
   SHA-256 is BearSSL's (third_party/bearssl, the same sha2small.c the
   entropy DRBG links). 1.7.9 retired the from-scratch FIPS 180-4 copy
   that used to live here; sha256_digest stays as a thin wrapper because
   the legacy record scheme below still needs a plain digest to verify
   accounts written before PBKDF2, and tools/auth-host still pins it to
   the FIPS vectors. */
#include "bearssl_hash.h"
#include "auth_kdf.h"

static void sha256_digest(const void *data, unsigned int len, unsigned char out[32]) {
    br_sha256_context c;
    br_sha256_init(&c);
    br_sha256_update(&c, data, len);
    br_sha256_out(&c, out);
}

/* ---------------------------------------------------------------------
   Password hashing. Two schemes live side by side:

   PBKDF2 (every record written since 1.7.9): PBKDF2-HMAC-SHA256 per
   RFC 8018, BearSSL's HMAC underneath (kernel/auth_kdf.c), a 16-byte
   salt from the entropy pool, 32-byte output, and the iteration count
   stored in the record itself so it can be raised later without a
   format change. AUTH_PBKDF2_ITERS is what new records get. Each
   iteration is two SHA-256 compressions (inner and outer HMAC), so
   100,000 iterations costs about what the old 200,000-round chain did:
   measured in QEMU by tools/checks/auth-flow-check.py, which prints the
   wall-clock login time and asserts it stays under a second.

   Legacy (records with no scheme tag, written before 1.7.9):
   hash = sha256^N(sha256(salt || password)) with N = 200,000, a homemade
   construction. Kept verify-only: a successful login on a legacy record
   rehashes the password as PBKDF2 and rewrites the record, so no
   existing account is ever lost or locked out, and the old scheme
   disappears from disk one login at a time. */
#define AUTH_LEGACY_ROUNDS 200000
#define AUTH_PBKDF2_ITERS 100000
#define AUTH_PBKDF2_ITERS_MIN 1000
#define AUTH_PBKDF2_ITERS_MAX 10000000
#define AUTH_SALT_LEN 16
#define AUTH_HASH_LEN 32
#define AUTH_USERNAME_MAX 24
#define AUTH_PASSWORD_MAX 64

static void auth_hash_legacy(const unsigned char salt[AUTH_SALT_LEN], const char *password,
                             unsigned char out[AUTH_HASH_LEN]) {
    unsigned int pwlen = strlen(password);
    if (pwlen > AUTH_PASSWORD_MAX) pwlen = AUTH_PASSWORD_MAX; /* bounds even though the input already came in through a fixed-size buffer */
    unsigned char msg[AUTH_SALT_LEN + AUTH_PASSWORD_MAX];
    memcpy(msg, salt, AUTH_SALT_LEN);
    memcpy(msg + AUTH_SALT_LEN, password, pwlen);
    sha256_digest(msg, AUTH_SALT_LEN + pwlen, out);
    /* msg held a plaintext copy of the password. Wipe through a volatile
       pointer: a plain memset of a dying stack buffer is a dead store the
       compiler is allowed to delete, and at -O2 it did. */
    { volatile unsigned char *vp = msg; for (unsigned int i = 0; i < sizeof(msg); i++) vp[i] = 0; }
    for (int i = 1; i < AUTH_LEGACY_ROUNDS; i++) {
        unsigned char next[AUTH_HASH_LEN];
        sha256_digest(out, AUTH_HASH_LEN, next);
        memcpy(out, next, AUTH_HASH_LEN);
    }
}

/* iters == 0 selects the legacy scheme; anything else is a PBKDF2
   iteration count. */
static void auth_hash_password(const unsigned char salt[AUTH_SALT_LEN], unsigned int iters,
                               const char *password, unsigned char out[AUTH_HASH_LEN]) {
    if (iters == 0) { auth_hash_legacy(salt, password, out); return; }
    unsigned int pwlen = strlen(password);
    if (pwlen > AUTH_PASSWORD_MAX) pwlen = AUTH_PASSWORD_MAX;
    auth_pbkdf2_sha256(password, pwlen, salt, AUTH_SALT_LEN, iters, out, AUTH_HASH_LEN);
}

/* Salts come from kernel/entropy.c: an HMAC_DRBG (SHA-256, vendored
   BearSSL) seeded from RDRAND when present, RDTSC jitter, and interrupt
   timing. 1.7.10 replaced the tick-seeded LCG that lived here; it made
   salts guessable from boot timing. Stored accounts keep their old salts
   next to their hashes, so nothing already on disk changes. */
static void auth_gen_salt(unsigned char salt[AUTH_SALT_LEN]) {
    entropy_bytes(salt, AUTH_SALT_LEN);
}

static const char AUTH_HEX[] = "0123456789abcdef";
static void auth_to_hex(const unsigned char *bytes, unsigned int len, char *out /* len*2+1 */) {
    for (unsigned int i = 0; i < len; i++) {
        out[i*2]   = AUTH_HEX[(bytes[i] >> 4) & 0xf];
        out[i*2+1] = AUTH_HEX[bytes[i] & 0xf];
    }
    out[len*2] = 0;
}

/* Returns 1 and fills *out on a valid hex nibble, 0 otherwise -- every
   USERS.TXT parse below treats the file as untrusted (it's plain, user-
   writable disk content) and checks this rather than assuming well-formed
   hex. */
static int auth_hex_nibble(char c, unsigned char *out) {
    if (c >= '0' && c <= '9') { *out = (unsigned char)(c - '0'); return 1; }
    if (c >= 'a' && c <= 'f') { *out = (unsigned char)(c - 'a' + 10); return 1; }
    if (c >= 'A' && c <= 'F') { *out = (unsigned char)(c - 'A' + 10); return 1; }
    return 0;
}

/* Parses exactly len*2 hex chars starting at str into len raw bytes.
   Returns 1 on success, 0 if any char isn't valid hex (caller must not
   trust *out on failure). */
static int auth_from_hex(const char *str, unsigned int len, unsigned char *out) {
    for (unsigned int i = 0; i < len; i++) {
        unsigned char hi, lo;
        if (!auth_hex_nibble(str[i*2], &hi)) return 0;
        if (!auth_hex_nibble(str[i*2+1], &lo)) return 0;
        out[i] = (unsigned char)((hi << 4) | lo);
    }
    return 1;
}

/* Constant-time compare: always walks the full length and accumulates
   an OR of differences rather than returning early on the first
   mismatch, so a timing side channel can't be used to guess the hash
   one byte at a time. The one place in this file that's genuinely worth
   being paranoid about (see docs/THREAT-MODEL.md's "not lazy about
   this one part" framing). */
static int auth_const_time_eq(const unsigned char *a, const unsigned char *b, unsigned int len) {
    unsigned char diff = 0;
    for (unsigned int i = 0; i < len; i++) diff |= (unsigned char)(a[i] ^ b[i]);
    return diff == 0;
}

/* ---------------------------------------------------------------------
   USERS.TXT: one line per user. Two record shapes are accepted:
     "username:p2:iterations:saltHex:hashHex\n"   PBKDF2-HMAC-SHA256 (1.7.9+)
     "username:saltHex:hashHex\n"                 legacy chained SHA-256
   The scheme tag is the second field; a record without one is legacy
   and carries iters == 0 in the table. Saving always writes the tagged
   shape for PBKDF2 records and the untagged shape for legacy ones still
   awaiting their first login, so a save never silently changes a
   record's scheme. Loaded into a small fixed-size table, same shape
   reminders_text[]/reminders_done[] already use. Every field is
   bounds-checked against both its max length and the delimiters actually
   being present -- this file is untrusted input (plain text on a disk
   anyone with the image can edit), not a trusted internal format. A
   malformed line is skipped, never trusted partially and never overruns
   a buffer. */
#define AUTH_USERS_MAX 8
#define AUTH_USERS_FILE "USERS.TXT"
#define AUTH_SCHEME_TAG "p2"

typedef struct {
    char username[AUTH_USERNAME_MAX + 1];
    unsigned int iters;                  /* 0 = legacy scheme, else PBKDF2 iteration count */
    unsigned char salt[AUTH_SALT_LEN];
    unsigned char hash[AUTH_HASH_LEN];
} auth_user_t;

static auth_user_t auth_users[AUTH_USERS_MAX];
static int auth_user_count = 0;
static int auth_users_loaded = 0;
static int auth_logged_in = 0;               /* 1 once a real login/first-run has succeeded this boot */
static char auth_current_user[AUTH_USERNAME_MAX + 1] = "";
static int auth_upgraded_count = 0;          /* legacy records rewritten as PBKDF2 this boot; auth-flow-check.py reads it */

/* Parses a bounded decimal field into *out. Returns 1 iff every char is
   a digit and the value lands inside [AUTH_PBKDF2_ITERS_MIN, _MAX]. */
static int auth_parse_iters(const char *str, unsigned int len, unsigned int *out) {
    if (len == 0 || len > 8) return 0;
    unsigned int v = 0;
    for (unsigned int i = 0; i < len; i++) {
        if (str[i] < '0' || str[i] > '9') return 0;
        v = v * 10 + (unsigned int)(str[i] - '0');
    }
    if (v < AUTH_PBKDF2_ITERS_MIN || v > AUTH_PBKDF2_ITERS_MAX) return 0;
    *out = v;
    return 1;
}

static void auth_users_load(void) {
    if (auth_users_loaded) return;
    auth_users_loaded = 1;
    static char buf[2048];
    int n = vfs_read_file(AUTH_USERS_FILE, buf, sizeof(buf) - 1);
    if (n < 0) n = 0;
    buf[n] = 0;
    auth_user_count = 0;
    int i = 0;
    while (i < n && auth_user_count < AUTH_USERS_MAX) {
        int start = i;
        while (i < n && buf[i] != '\n') i++;
        int line_end = i;
        if (i < n) i++; /* skip newline */
        if (line_end == start) continue; /* blank line */

        /* Split on ':' within this line only, at most 5 fields. */
        int f[6]; int nf = 0;
        f[nf++] = start;
        for (int j = start; j < line_end && nf < 6; j++) if (buf[j] == ':') f[nf++] = j + 1;
        if (nf != 3 && nf != 5) continue; /* malformed: field count matches neither shape */
        f[nf] = line_end + 1;
        #define FLEN(k) ((unsigned int)(f[(k) + 1] - f[k] - 1))

        unsigned int ulen = FLEN(0);
        if (ulen == 0 || ulen > AUTH_USERNAME_MAX) continue;            /* bounds check: username field */
        unsigned int iters = 0;
        int si = 1;
        if (nf == 5) {
            if (FLEN(1) != 2 || buf[f[1]] != 'p' || buf[f[1] + 1] != '2') continue; /* unknown scheme tag */
            if (!auth_parse_iters(buf + f[2], FLEN(2), &iters)) continue;
            si = 3;
        }
        if (FLEN(si) != AUTH_SALT_LEN * 2) continue;                    /* bounds check: salt must be exactly 32 hex chars */
        if (FLEN(si + 1) != AUTH_HASH_LEN * 2) continue;                /* bounds check: hash must be exactly 64 hex chars */

        auth_user_t *u = &auth_users[auth_user_count];
        memcpy(u->username, buf + start, ulen);
        u->username[ulen] = 0;
        u->iters = iters;
        if (!auth_from_hex(buf + f[si], AUTH_SALT_LEN, u->salt)) continue;
        if (!auth_from_hex(buf + f[si + 1], AUTH_HASH_LEN, u->hash)) continue;
        #undef FLEN
        auth_user_count++;
    }
}

static void auth_users_save(void) {
    char buf[AUTH_USERS_MAX * (AUTH_USERNAME_MAX + 1 + 3 + 9 + AUTH_SALT_LEN*2 + 1 + AUTH_HASH_LEN*2 + 2)];
    int n = 0;
    for (int i = 0; i < auth_user_count; i++) {
        auth_user_t *u = &auth_users[i];
        const char *s = u->username;
        while (*s) buf[n++] = *s++;
        buf[n++] = ':';
        if (u->iters) {
            buf[n++] = 'p'; buf[n++] = '2'; buf[n++] = ':';
            char dec[11]; int d = 0; unsigned int v = u->iters;
            do { dec[d++] = (char)('0' + v % 10); v /= 10; } while (v);
            while (d) buf[n++] = dec[--d];
            buf[n++] = ':';
        }
        char hex[AUTH_SALT_LEN*2 + 1];
        auth_to_hex(u->salt, AUTH_SALT_LEN, hex);
        for (int j = 0; j < AUTH_SALT_LEN*2; j++) buf[n++] = hex[j];
        buf[n++] = ':';
        char hex2[AUTH_HASH_LEN*2 + 1];
        auth_to_hex(u->hash, AUTH_HASH_LEN, hex2);
        for (int j = 0; j < AUTH_HASH_LEN*2; j++) buf[n++] = hex2[j];
        buf[n++] = '\n';
    }
    vfs_replace_file(AUTH_USERS_FILE, buf, (unsigned int)n);
}

static int auth_find_user(const char *username) {
    for (int i = 0; i < auth_user_count; i++) if (!strcmp(auth_users[i].username, username)) return i;
    return -1;
}

/* Returns 1 on success (account created), 0 if the table is full, the
   username is empty/too long, the password is empty, or the username is
   already taken. Never overwrites an existing account through this
   path -- that's what auth_change_password is for. */
static int auth_create_user(const char *username, const char *password) {
    unsigned int ulen = strlen(username);
    if (ulen == 0 || ulen > AUTH_USERNAME_MAX) return 0;
    if (strlen(password) == 0) return 0;
    if (auth_user_count >= AUTH_USERS_MAX) return 0;
    auth_users_load();
    if (auth_find_user(username) >= 0) return 0;

    auth_user_t *u = &auth_users[auth_user_count];
    memcpy(u->username, username, ulen);
    u->username[ulen] = 0;
    auth_gen_salt(u->salt);
    u->iters = AUTH_PBKDF2_ITERS;
    auth_hash_password(u->salt, u->iters, password, u->hash);
    auth_user_count++;
    auth_users_save();
    return 1;
}

/* Verifies a login. Returns 1 iff both the username exists and the
   password's re-derived hash (under the record's own scheme) constant-
   time-matches the stored one. A legacy record that verifies is rehashed
   as PBKDF2 with a fresh salt and written back right here, while the
   plaintext is still in hand: the only moment an upgrade is possible. */
static int auth_verify(const char *username, const char *password) {
    auth_users_load();
    int idx = auth_find_user(username);
    if (idx < 0) return 0;
    auth_user_t *u = &auth_users[idx];
    unsigned char candidate[AUTH_HASH_LEN];
    auth_hash_password(u->salt, u->iters, password, candidate);
    if (!auth_const_time_eq(candidate, u->hash, AUTH_HASH_LEN)) return 0;
    if (u->iters == 0) {
        auth_gen_salt(u->salt);
        u->iters = AUTH_PBKDF2_ITERS;
        auth_hash_password(u->salt, u->iters, password, u->hash);
        auth_users_save();
        auth_upgraded_count++;
        klog("auth: legacy record upgraded to pbkdf2");
    }
    return 1;
}

/* Changes the password for an existing user, after verifying the old
   one. Returns 1 on success, 0 if the user doesn't exist, old_password
   is wrong, or new_password is empty. Rotates to a fresh salt (never
   reuses the old one, so two consecutive passwords for the same account
   never happen to share a salt). */
static int auth_change_password(const char *username, const char *old_password, const char *new_password) {
    if (!auth_verify(username, old_password)) return 0;
    if (strlen(new_password) == 0) return 0;
    int idx = auth_find_user(username);
    if (idx < 0) return 0;
    auth_user_t *u = &auth_users[idx];
    auth_gen_salt(u->salt);
    u->iters = AUTH_PBKDF2_ITERS;
    auth_hash_password(u->salt, u->iters, new_password, u->hash);
    auth_users_save();
    return 1;
}

/* ---------------------------------------------------------------------
   Login / first-run GUI screens. Reuses gui_prompt.h's shared chrome
   (window_clear/gui_draw_app_titlebar/font_draw_string/window_rect) and
   the established Mojave palette (0x001C1C1E text, 0x0075726E labels,
   0x00807468 hint text, 0x00EDE6DC highlight, 0x00FFFFFF input boxes --
   the exact constants gui_prompt.h/kernel.c's other prompt loops already
   use), not new chrome. Password entry echoes a fixed-width dot per
   typed character (never the character itself), the standard "hide the
   secret, show its length" convention, and the password buffer itself
   is a local stack array cleared before returning so a stale copy
   doesn't linger in memory longer than it has to (belt-and-suspenders
   given the honest "any ring-0 code can read memory anyway" limit
   docs/THREAT-MODEL.md already names). */

/* One text-entry field, either plain (username) or masked (password).
   Returns 1 on enter, 0 on esc. Chrome (titlebar + static label above
   the box) is drawn by the caller once; this only redraws the box each
   keystroke, the same chrome/content split gui_prompt.h's own header
   comment documents and requires. */
static int auth_field_input(char *out, int max, int y, int masked) {
    unsigned int n = 0;
    out[0] = 0;
    mouse_click_edge_sync();
    for (;;) {
        window_rect(20, y, (int)window_width() - 40, 20, 0x00FFFFFF);
        if (masked) {
            char dots[AUTH_PASSWORD_MAX + 1];
            unsigned int dn = n; if (dn > AUTH_PASSWORD_MAX) dn = AUTH_PASSWORD_MAX;
            for (unsigned int i = 0; i < dn; i++) dots[i] = '*';
            dots[dn] = 0;
            font_draw_string(dots, 24, y + 2, 0x001C1C1E, -1);
        } else {
            out[n] = 0;
            font_draw_string(out, 24, y + 2, 0x001C1C1E, -1);
        }
        int k = get_key_or_click();
        if (k == KEY_ESC) return 0;
        if (k == KEY_ENTER) break;
        if (k == '\b') { if (n > 0) n--; }
        else if ((int)n < max - 1 && k >= 32 && k < 127) out[n++] = (char)k;
    }
    out[n] = 0;
    return 1;
}

/* Real login: wrong credentials refuse and re-prompt. No lockout table
   (there's exactly one local user in front of the keyboard in this threat
   model, and a fake-looking account-lockout screen would be exactly the
   overclaiming docs/THREAT-MODEL.md warns against), but a fixed 1-second
   delay (100 PIT ticks at 100/sec, see kernel/irq.h) after every rejected
   attempt below -- security pass: a real throttle against an automated
   guesser is worth having even in a single-user threat model, and unlike
   a lockout it can't be used to lock the real owner out. esc on the
   username field is the one way out of the loop, matching every other
   esc-closes-and-does-nothing-destructive contract this GUI already
   keeps, and just re-shows the same screen since there's no desktop to
   fall back to yet. */
static void auth_login_screen(void) {
    char username[AUTH_USERNAME_MAX + 1];
    char password[AUTH_PASSWORD_MAX + 1];
    for (;;) {
        window_clear(GUI_BG);
        gui_draw_app_titlebar("Joshua Tree");
        font_draw_string("Username:", 20, 52, 0x0075726E, -1);
        if (!auth_field_input(username, AUTH_USERNAME_MAX + 1, 76, 0)) { sleep_ticks(30); continue; }

        window_clear(GUI_BG);
        gui_draw_app_titlebar("Joshua Tree");
        font_draw_string("Password:", 20, 52, 0x0075726E, -1);
        if (!auth_field_input(password, AUTH_PASSWORD_MAX + 1, 76, 1)) continue;

        if (auth_verify(username, password)) {
            unsigned int p = 0; while (username[p] && p < AUTH_USERNAME_MAX) { auth_current_user[p] = username[p]; p++; } auth_current_user[p] = 0;
            auth_logged_in = 1;
            memset(password, 0, sizeof(password));
            klog("auth: login ok"); /* names the event, never the credential */
            return;
        }
        memset(password, 0, sizeof(password));
        klog("auth: login rejected");
        window_clear(GUI_BG);
        gui_draw_app_titlebar("Joshua Tree");
        font_draw_string("Wrong username or password. Try again.", 20, 52, 0x00A33B3B, -1);
        /* Real bug, found by tools/checks/auth-flow-check.py: drawing lands
           in a back buffer (drivers/window.c) and only window_present()
           ever copies it to the visible framebuffer, "at a real frame
           boundary... about to wait for input" per that file's own header
           comment. This branch had no such boundary of its own -- it fell
           straight into sleep_ticks(), and the outer loop's next
           window_clear() erased the error text before it was ever
           presented, so a real person typing a wrong password saw the
           screen pause and silently reset with no message at all. */
        window_present();
        sleep_ticks(100); /* fixed 1-second throttle per failed attempt, see comment above auth_login_screen */
    }
}

/* Called once, right after the GUI window opens and before the desktop
   ever paints, from gui_run(). Idempotent within a boot via
   auth_logged_in so re-entering gui_run() (the shell's "gui" command,
   after esc'ing back out) doesn't re-prompt every time -- the same
   "lock the desktop once per session, not once per window" behavior a
   real OS login has, matching this kernel's existing per-boot (not
   per-window) state for things like settings_load.

   Opt-in gate, direct design call: the login screen only engages when
   USERS.TXT actually has at least one account in it. An unconfigured
   system (no USERS.TXT, or an empty one) boots straight to the desktop,
   no first-run account-creation screen blocking the gate -- creating
   the first account lives in Settings ("Add user", already built)
   instead. Two real reasons, not just a preference: (1) the public
   landing demo runs this exact kernel inside v86 in a browser, where a
   visitor can't be handed a password prompt and there's no one present
   to create an account against -- a wall there kills the demo outright;
   (2) it means every existing headless GUI check script (the ~40 under
   tools/checks/ that drive "gui" straight through to desktop
   interaction, none of which know this feature exists) keeps working
   unmodified on a fresh image with no USERS.TXT, since the gate is a
   pure no-op in that case. It's also the honest reading of
   docs/THREAT-MODEL.md's own scope: with no accounts configured there
   is nothing to protect, so there's nothing for the gate to do. */
/* Pure, GUI-free: whether auth_gate would actually show the blocking
   login screen (at least one account exists) or no-op straight to the
   desktop (unconfigured system, nothing to protect). Extracted into its
   own function so the gating *rule* is unit-testable without invoking
   the real interactive GUI loop, the same "extract the decision so it's
   testable without a mouse/keyboard/boot" shape settings_row_at's own
   extraction already established in kernel.c for exactly this reason. */
static int auth_gate_would_prompt(void) {
    auth_users_load();
    return auth_user_count > 0;
}

static void auth_gate(void) {
    if (auth_logged_in) return;
    if (!auth_gate_would_prompt()) return; /* unconfigured system: no accounts, no gate, straight to desktop */
    auth_login_screen();
}
