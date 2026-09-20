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
   line per user: "username:saltHex:hashHex\n". Never plaintext, and
   never logged: the password itself is never passed to klog/serial_puts
   anywhere in this file, only fixed strings like "auth: login ok" or
   "auth: login rejected" that name the *event*, not the *secret*. */

/* ---------------------------------------------------------------------
   SHA-256, implemented from scratch (FIPS 180-4), freestanding: no
   libc beyond memcpy/memset above, no stdint (this target's `unsigned
   int` is already a real 32-bit word, `unsigned char` a real byte,
   i386 -target confirmed via CFLAGS). Standard reference constants,
   verified against the FIPS 180-4 published test vectors in
   tools/checks/auth-check.sh (empty string and "abc"), not just
   assumed correct because the algorithm text was copied right. */

typedef struct {
    unsigned int h[8];
    unsigned char buf[64];
    unsigned int buflen;
    unsigned long long total_len; /* clang's freestanding i386 target still gives us a real 64-bit long long */
} sha256_ctx;

static const unsigned int SHA256_K[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};

static unsigned int sha256_rotr(unsigned int x, unsigned int n) {
    return (x >> n) | (x << (32 - n));
}

static void sha256_init(sha256_ctx *c) {
    c->h[0]=0x6a09e667; c->h[1]=0xbb67ae85; c->h[2]=0x3c6ef372; c->h[3]=0xa54ff53a;
    c->h[4]=0x510e527f; c->h[5]=0x9b05688c; c->h[6]=0x1f83d9ab; c->h[7]=0x5be0cd19;
    c->buflen = 0;
    c->total_len = 0;
}

static void sha256_process_block(sha256_ctx *c, const unsigned char *p) {
    unsigned int w[64];
    for (int i = 0; i < 16; i++) {
        w[i] = ((unsigned int)p[i*4] << 24) | ((unsigned int)p[i*4+1] << 16) |
               ((unsigned int)p[i*4+2] << 8) | (unsigned int)p[i*4+3];
    }
    for (int i = 16; i < 64; i++) {
        unsigned int s0 = sha256_rotr(w[i-15], 7) ^ sha256_rotr(w[i-15], 18) ^ (w[i-15] >> 3);
        unsigned int s1 = sha256_rotr(w[i-2], 17) ^ sha256_rotr(w[i-2], 19) ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    unsigned int a=c->h[0],b=c->h[1],cc=c->h[2],d=c->h[3],e=c->h[4],f=c->h[5],g=c->h[6],hh=c->h[7];
    for (int i = 0; i < 64; i++) {
        unsigned int S1 = sha256_rotr(e,6) ^ sha256_rotr(e,11) ^ sha256_rotr(e,25);
        unsigned int ch = (e & f) ^ ((~e) & g);
        unsigned int t1 = hh + S1 + ch + SHA256_K[i] + w[i];
        unsigned int S0 = sha256_rotr(a,2) ^ sha256_rotr(a,13) ^ sha256_rotr(a,22);
        unsigned int maj = (a & b) ^ (a & cc) ^ (b & cc);
        unsigned int t2 = S0 + maj;
        hh=g; g=f; f=e; e=d+t1; d=cc; cc=b; b=a; a=t1+t2;
    }
    c->h[0]+=a; c->h[1]+=b; c->h[2]+=cc; c->h[3]+=d;
    c->h[4]+=e; c->h[5]+=f; c->h[6]+=g; c->h[7]+=hh;
}

static void sha256_update(sha256_ctx *c, const void *data, unsigned int len) {
    const unsigned char *p = (const unsigned char *)data;
    c->total_len += len;
    while (len > 0) {
        unsigned int take = 64 - c->buflen;
        if (take > len) take = len;
        memcpy(c->buf + c->buflen, p, take);
        c->buflen += take;
        p += take;
        len -= take;
        if (c->buflen == 64) {
            sha256_process_block(c, c->buf);
            c->buflen = 0;
        }
    }
}

/* out must be 32 bytes. */
static void sha256_final(sha256_ctx *c, unsigned char *out) {
    unsigned long long bitlen = c->total_len * 8ull;
    unsigned char pad = 0x80;
    sha256_update(c, &pad, 1);
    /* sha256_update above just mutated buflen via its own internal calls;
       pad to 56 bytes mod 64 directly rather than re-entering update's
       total_len accounting (that field is done being meaningful now). */
    unsigned char zero = 0;
    while (c->buflen != 56) {
        if (c->buflen == 0) { /* just rolled over a block: nothing else to do, loop condition re-checks */ }
        c->buf[c->buflen++] = zero;
        if (c->buflen == 64) { sha256_process_block(c, c->buf); c->buflen = 0; }
    }
    for (int i = 7; i >= 0; i--) c->buf[c->buflen++] = (unsigned char)((bitlen >> (i * 8)) & 0xff);
    sha256_process_block(c, c->buf);
    for (int i = 0; i < 8; i++) {
        out[i*4]   = (unsigned char)((c->h[i] >> 24) & 0xff);
        out[i*4+1] = (unsigned char)((c->h[i] >> 16) & 0xff);
        out[i*4+2] = (unsigned char)((c->h[i] >> 8) & 0xff);
        out[i*4+3] = (unsigned char)(c->h[i] & 0xff);
    }
}

static void sha256_digest(const void *data, unsigned int len, unsigned char out[32]) {
    sha256_ctx c;
    sha256_init(&c);
    sha256_update(&c, data, len);
    sha256_final(&c, out);
}

/* ---------------------------------------------------------------------
   Salted, iterated hash. hash = sha256^N(sha256(salt || password)),
   i.e. one initial digest over salt||password, then N-1 more rounds of
   sha256(previous digest), matching the direct spec
   ("hash = iterate(sha256(salt || password), N)").

   N (AUTH_HASH_ROUNDS below) was picked by real measurement, not a
   guess, and the measurement itself is worth being honest about: the
   first attempt used this kernel's own ticks() (PIT-driven, house
   pattern) diffed around a boot-time direct-call block (same trick
   check.sh/v54/v56/v59 use, temporary in kmain, removed before commit),
   but this machine had other sessions' QEMU instances running
   concurrently while this was measured, and the in-guest tick counter
   swung by a full order of magnitude between otherwise-identical runs
   under that host contention (e.g. 400,000 total rounds read as both
   66 ticks and 1300+ ticks across two runs) -- not trustworthy on its
   own here. Re-measured the reliable way instead: real host wall-clock
   (python3 time.time()) from QEMU launch to the AUTHTIME serial line
   appearing, with a baseline run at AUTH_HASH_ROUNDS=1 (0 extra rounds,
   pure boot overhead: measured 0.133s) subtracted from a loaded run, so
   the result isolates hash cost from boot cost rather than mixing them.
   200,000 rounds measured at 0.463s total - 0.133s baseline = ~0.33s of
   real hashing, on a machine that was itself under load from unrelated
   concurrent QEMU sessions (i.e. this is closer to a worst-case reading
   than a best case). That's the number this ships with: a third of a
   second is a real, meaningful cost for an offline attacker (200,000x
   slower than a single unsalted SHA-256 call) while staying well clear
   of feeling like a hang during first-run account creation or login,
   even measured under contention; a dedicated host or real hardware
   should only be faster. Named and tunable here, not buried: raise it
   if a future faster/dedicated host makes 0.33s feel too cheap, lower
   it if a slower target makes it feel like a stall. */
#define AUTH_HASH_ROUNDS 200000
#define AUTH_SALT_LEN 16
#define AUTH_HASH_LEN 32
#define AUTH_USERNAME_MAX 24
#define AUTH_PASSWORD_MAX 64

static void auth_hash_password(const unsigned char salt[AUTH_SALT_LEN], const char *password,
                                unsigned char out[AUTH_HASH_LEN]) {
    unsigned int pwlen = strlen(password);
    if (pwlen > AUTH_PASSWORD_MAX) pwlen = AUTH_PASSWORD_MAX; /* bounds even though the input already came in through a fixed-size buffer */
    unsigned char msg[AUTH_SALT_LEN + AUTH_PASSWORD_MAX];
    memcpy(msg, salt, AUTH_SALT_LEN);
    memcpy(msg + AUTH_SALT_LEN, password, pwlen);
    sha256_digest(msg, AUTH_SALT_LEN + pwlen, out);
    for (int i = 1; i < AUTH_HASH_ROUNDS; i++) {
        unsigned char next[AUTH_HASH_LEN];
        sha256_digest(out, AUTH_HASH_LEN, next);
        memcpy(out, next, AUTH_HASH_LEN);
    }
}

/* Not a cryptographically strong RNG (this kernel has none; the honest
   gap is logged in docs/THREAT-MODEL.md -- nothing here claims otherwise).
   Same tiny LCG house pattern weather_rand/keyrate_rand already use,
   seeded from the real PIT tick count so at least two salts generated in
   the same boot at different moments differ. Good enough for its actual
   job: making two identical passwords hash differently and defeating a
   precomputed rainbow table, not resisting a targeted attacker who can
   also influence or observe boot timing. */
static unsigned int auth_rng_state = 0;
static unsigned int auth_rand(void) {
    if (auth_rng_state == 0) auth_rng_state = ticks() ? ticks() : 1;
    auth_rng_state = auth_rng_state * 1103515245u + 12345u;
    return (auth_rng_state >> 16) & 0x7fff;
}

static void auth_gen_salt(unsigned char salt[AUTH_SALT_LEN]) {
    for (int i = 0; i < AUTH_SALT_LEN; i++) salt[i] = (unsigned char)(auth_rand() & 0xff);
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
   USERS.TXT: one line per user, "username:saltHex:hashHex\n". Loaded
   into a small fixed-size table, same shape reminders_text[]/
   reminders_done[] already use. Every field is bounds-checked against
   both its max length and the delimiters actually being present --
   this file is untrusted input (plain text on a disk anyone with the
   image can edit), not a trusted internal format. A malformed line is
   skipped, never trusted partially and never overruns a buffer. */
#define AUTH_USERS_MAX 8
#define AUTH_USERS_FILE "USERS.TXT"

typedef struct {
    char username[AUTH_USERNAME_MAX + 1];
    unsigned char salt[AUTH_SALT_LEN];
    unsigned char hash[AUTH_HASH_LEN];
} auth_user_t;

static auth_user_t auth_users[AUTH_USERS_MAX];
static int auth_user_count = 0;
static int auth_users_loaded = 0;
static int auth_logged_in = 0;               /* 1 once a real login/first-run has succeeded this boot */
static char auth_current_user[AUTH_USERNAME_MAX + 1] = "";

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

        /* Find the two ':' delimiters within this line only. */
        int c1 = -1, c2 = -1;
        for (int j = start; j < line_end; j++) {
            if (buf[j] == ':') { if (c1 < 0) c1 = j; else { c2 = j; break; } }
        }
        if (c1 < 0 || c2 < 0) continue; /* malformed: not enough fields, skip */

        unsigned int ulen = (unsigned int)(c1 - start);
        unsigned int saltlen = (unsigned int)(c2 - c1 - 1);
        unsigned int hashlen = (unsigned int)(line_end - c2 - 1);
        if (ulen == 0 || ulen > AUTH_USERNAME_MAX) continue;            /* bounds check: username field */
        if (saltlen != AUTH_SALT_LEN * 2) continue;                     /* bounds check: salt must be exactly 32 hex chars */
        if (hashlen != AUTH_HASH_LEN * 2) continue;                     /* bounds check: hash must be exactly 64 hex chars */

        auth_user_t *u = &auth_users[auth_user_count];
        memcpy(u->username, buf + start, ulen);
        u->username[ulen] = 0;
        if (!auth_from_hex(buf + c1 + 1, AUTH_SALT_LEN, u->salt)) continue;
        if (!auth_from_hex(buf + c2 + 1, AUTH_HASH_LEN, u->hash)) continue;
        auth_user_count++;
    }
}

static void auth_users_save(void) {
    char buf[AUTH_USERS_MAX * (AUTH_USERNAME_MAX + 1 + AUTH_SALT_LEN*2 + 1 + AUTH_HASH_LEN*2 + 2)];
    int n = 0;
    for (int i = 0; i < auth_user_count; i++) {
        auth_user_t *u = &auth_users[i];
        const char *s = u->username;
        while (*s) buf[n++] = *s++;
        buf[n++] = ':';
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
    auth_hash_password(u->salt, password, u->hash);
    auth_user_count++;
    auth_users_save();
    return 1;
}

/* Verifies a login. Returns 1 iff both the username exists and the
   password's re-derived hash constant-time-matches the stored one. */
static int auth_verify(const char *username, const char *password) {
    auth_users_load();
    int idx = auth_find_user(username);
    if (idx < 0) return 0;
    unsigned char candidate[AUTH_HASH_LEN];
    auth_hash_password(auth_users[idx].salt, password, candidate);
    return auth_const_time_eq(candidate, auth_users[idx].hash, AUTH_HASH_LEN);
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
    auth_hash_password(u->salt, new_password, u->hash);
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

/* First-run: no accounts exist yet. Walks username -> password ->
   confirm, creates the account, loops back to the same prompt on any
   mismatch/cancel rather than falling through to a broken login. */
static void auth_first_run_screen(void) {
    char username[AUTH_USERNAME_MAX + 1];
    char password[AUTH_PASSWORD_MAX + 1];
    char confirm[AUTH_PASSWORD_MAX + 1];
    for (;;) {
        window_clear(GUI_BG);
        gui_draw_app_titlebar("Welcome to Joshua Tree");
        font_draw_string("No account yet. Create one to use this desktop.", 20, 52, 0x0075726E, -1);
        font_draw_string("Username:", 20, 76, 0x0075726E, -1);
        if (!auth_field_input(username, AUTH_USERNAME_MAX + 1, 100, 0)) continue;
        if (username[0] == 0) continue;

        window_clear(GUI_BG);
        gui_draw_app_titlebar("Welcome to Joshua Tree");
        font_draw_string("Choose a password:", 20, 52, 0x0075726E, -1);
        if (!auth_field_input(password, AUTH_PASSWORD_MAX + 1, 76, 1)) continue;
        if (password[0] == 0) continue;

        window_clear(GUI_BG);
        gui_draw_app_titlebar("Welcome to Joshua Tree");
        font_draw_string("Confirm password:", 20, 52, 0x0075726E, -1);
        if (!auth_field_input(confirm, AUTH_PASSWORD_MAX + 1, 76, 1)) continue;

        if (!strcmp(password, confirm) && auth_create_user(username, password)) {
            unsigned int p = 0; while (username[p] && p < AUTH_USERNAME_MAX) { auth_current_user[p] = username[p]; p++; } auth_current_user[p] = 0;
            auth_logged_in = 1;
            memset(password, 0, sizeof(password));
            memset(confirm, 0, sizeof(confirm));
            return;
        }
        memset(password, 0, sizeof(password));
        memset(confirm, 0, sizeof(confirm));
        window_clear(GUI_BG);
        gui_draw_app_titlebar("Welcome to Joshua Tree");
        font_draw_string("Passwords didn't match or that name is taken. Try again.", 20, 52, 0x00A33B3B, -1);
        sleep_ticks(60);
    }
}

/* Real login: wrong credentials refuse and re-prompt (no lockout/rate
   limiting -- there's exactly one local user in front of the keyboard in
   this threat model, and adding a fake-looking lockout would be exactly
   the overclaiming docs/THREAT-MODEL.md warns against). esc on the
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
        sleep_ticks(60);
    }
}

/* Called once, right after the GUI window opens and before the desktop
   ever paints, from gui_run(). Idempotent within a boot via
   auth_logged_in so re-entering gui_run() (the shell's "gui" command,
   after esc'ing back out) doesn't re-prompt every time -- the same
   "lock the desktop once per session, not once per window" behavior a
   real OS login has, matching this kernel's existing per-boot (not
   per-window) state for things like settings_load. */
static void auth_gate(void) {
    if (auth_logged_in) return;
    auth_users_load();
    if (auth_user_count == 0) auth_first_run_screen();
    else auth_login_screen();
}
