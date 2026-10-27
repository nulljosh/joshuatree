/* The keychain (6.21.0): what an account keeps secret across restarts (today the mail sign-in), sealed on the card in
   its own home folder as KEYCHAIN.BIN with a key only its login password makes. At sign-in, login.c hands the typed
   password to keychain_unlock before wiping it; that stretches it (PBKDF2-HMAC-SHA256, 100,000 rounds, the file's own
   16 byte salt) into an AES-256-GCM key held in memory while that account stays signed in. Another sign-in, or nobody,
   wipes the key. The file is "JTK1", the salt, a fresh 12 byte nonce on every write, the 16 byte tag, then the sealed
   lines "item\tvalue"; one byte changed anywhere fails the tag and nothing in it is believed. passwd re-seals it under
   the new password; a password reset by an admin leaves it unreadable, as a Mac's does. With no accounts there is no
   home and no keychain: secrets stay in memory, as before. tools/checks/arm64-keychain-check.py signs in, restarts,
   reads the card and tampers with it. */
#include <stddef.h>
#include "bearssl.h"
#include "rng.h"
#include "home.h"
#include "../../kernel/auth_kdf.h"

extern char current_user[];   /* login.c */
void accounts_wipe(void *p, unsigned n);
int sd_ready(void); int sd_enter_jt(int make);
int fat_read_file(const char *name, void *buf, unsigned bufsize); void fat_cwd_set(unsigned int cluster);
void uart_puts(const char *s);

#define KC_FILE "KEYCHAIN.BIN"
#define KC_ITERS 100000u
#define KC_HEAD (4 + 16 + 12 + 16)   /* magic, salt, nonce, tag */
#define KC_MAX 8192                  /* the sealed lines (6.26.0: 8 KiB, room for a cookie jar) */
static unsigned char kc_key[32], kc_salt[16];
static char kc_owner[40];
static int kc_ready;
static unsigned char kc_file[KC_HEAD + KC_MAX];
static char kc_text[KC_MAX + 1];

static unsigned slen(const char *s) { unsigned n = 0; while (s[n]) n++; return n; }
static int same(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }
static void forget(void) { accounts_wipe(kc_key, sizeof kc_key); kc_ready = 0; kc_owner[0] = 0; }
static int mine(void) { if (kc_ready && !same(kc_owner, current_user)) forget(); return kc_ready; }

static int read_file(void) {   /* KEYCHAIN.BIN from the signed-in person's home into kc_file: its length, or <= 0 */
    char dir[HOME_DIR_MAX + 1];
    if (!sd_ready() || !sd_enter_jt(0)) return -1;
    if (!home_active() || !home_mine(dir) || !home_in(dir, 0)) { fat_cwd_set(0); return -1; }
    int got = fat_read_file(KC_FILE, kc_file, sizeof kc_file);
    fat_cwd_set(0);
    return got;
}
static void gcm(const unsigned char *key, const unsigned char *nonce, const unsigned char *aad, unsigned char *data, unsigned n, int encrypt, unsigned char tag[16], int *ok) {
    br_aes_ct_ctr_keys aes; br_gcm_context g;
    br_aes_ct_ctr_init(&aes, key, 32);
    br_gcm_init(&g, &aes.vtable, br_ghash_ctmul32);
    br_gcm_reset(&g, nonce, 12);
    br_gcm_aad_inject(&g, aad, 20);   /* the magic and the salt are bound to the sealed text */
    br_gcm_flip(&g);
    br_gcm_run(&g, encrypt, data, n);
    if (encrypt) br_gcm_get_tag(&g, tag); else *ok = br_gcm_check_tag(&g, tag);
    accounts_wipe(&aes, sizeof aes); accounts_wipe(&g, sizeof g);
}
/* The sealed lines into kc_text: their length; 0 when there is no file yet; -1 when it is there but does not open. */
static int open_text(void) {
    int got = read_file();
    if (got <= 0) return 0;
    if ((unsigned)got < KC_HEAD || got - KC_HEAD > KC_MAX || kc_file[0] != 'J' || kc_file[1] != 'T' || kc_file[2] != 'K' || kc_file[3] != '1') return -1;
    unsigned n = (unsigned)got - KC_HEAD; int ok = 0;
    for (unsigned i = 0; i < n; i++) kc_text[i] = (char)kc_file[KC_HEAD + i];
    gcm(kc_key, kc_file + 20, kc_file, (unsigned char *)kc_text, n, 0, kc_file + 32, &ok);
    if (!ok) { accounts_wipe(kc_text, sizeof kc_text); return -1; }
    kc_text[n] = 0;
    return (int)n;
}
static int seal(unsigned n) {   /* kc_text[0..n) sealed with a fresh nonce and written: 1 when it is on the card */
    unsigned char *f = kc_file;
    f[0] = 'J'; f[1] = 'T'; f[2] = 'K'; f[3] = '1';
    for (unsigned i = 0; i < 16; i++) f[4 + i] = kc_salt[i];
    if (!tls_entropy(f + 20, 12)) return 0;
    for (unsigned i = 0; i < n; i++) f[KC_HEAD + i] = (unsigned char)kc_text[i];
    gcm(kc_key, f + 20, f, f + KC_HEAD, n, 1, f + 32, 0);
    accounts_wipe(kc_text, sizeof kc_text);
    return home_save(KC_FILE, f, KC_HEAD + n) == 1;
}
static void derive(const char *pass) { auth_pbkdf2_sha256(pass, slen(pass), kc_salt, 16, KC_ITERS, kc_key, 32); }

/* login.c, with the password that just signed name in (it wipes its own copy after). */
void keychain_unlock(const char *name, const char *pass) {
    forget();
    if (!home_active()) return;
    unsigned i = 0; for (; name[i] && i < sizeof kc_owner - 1; i++) kc_owner[i] = name[i];
    kc_owner[i] = 0;
    int got = read_file();
    if (got >= KC_HEAD && kc_file[0] == 'J' && kc_file[1] == 'T' && kc_file[2] == 'K' && kc_file[3] == '1') for (unsigned k = 0; k < 16; k++) kc_salt[k] = kc_file[4 + k];
    else if (!tls_entropy(kc_salt, 16)) { kc_owner[0] = 0; return; }
    derive(pass);
    kc_ready = 1;
}
/* cmd.c's passwd, once the signed-in person's password changed: the keychain re-sealed under the new one. */
void keychain_rekey(const char *name, const char *pass) {
    if (!mine() || !same(kc_owner, name)) return;
    int n = open_text();
    if (!tls_entropy(kc_salt, 16)) { forget(); return; }
    derive(pass);
    if (n > 0 && !seal((unsigned)n)) uart_puts("keychain: could not re-seal KEYCHAIN.BIN\n");
}
/* item's value into out: 1; 0 when there is none (or no keychain); -1 when the file does not open (changed, or sealed
   under a password that was reset). */
int keychain_get(const char *item, char *out, unsigned max) {
    out[0] = 0;
    if (!mine()) return 0;
    int n = open_text();
    if (n <= 0) return n;
    unsigned k = slen(item), r = 0;
    for (unsigned i = 0; i < (unsigned)n;) {
        unsigned e = i; while (e < (unsigned)n && kc_text[e] != '\n') e++;
        if (e - i > k && kc_text[i + k] == '\t') {
            unsigned m = 0; while (m < k && kc_text[i + m] == item[m]) m++;
            if (m == k) { unsigned v = i + k + 1, o = 0; while (v < e && o + 1 < max) out[o++] = kc_text[v++]; out[o] = 0; r = 1; break; }
        }
        i = e + 1;
    }
    accounts_wipe(kc_text, sizeof kc_text);
    return (int)r;
}
/* item set to value (0 removes it): 1 when the keychain on the card says so; 0 with no keychain; -1 when it would not
   open or would not write. */
int keychain_set(const char *item, const char *value) {
    if (!mine()) return 0;
    int n = open_text();
    if (n < 0) return -1;   /* never overwrite a keychain that does not open: it may be someone's, sealed before a reset */
    unsigned k = slen(item), w = 0;
    for (unsigned i = 0; i < (unsigned)n;) {   /* every other item stays */
        unsigned e = i; while (e < (unsigned)n && kc_text[e] != '\n') e++;
        unsigned m = 0; while (m < k && i + m < e && kc_text[i + m] == item[m]) m++;
        int drop = m == k && i + k < e && kc_text[i + k] == '\t';
        if (!drop) { for (unsigned c = i; c < e; c++) kc_text[w++] = kc_text[c]; kc_text[w++] = '\n'; }
        i = e + 1;
    }
    if (value) {
        if (w + k + 2 + slen(value) > KC_MAX) { accounts_wipe(kc_text, sizeof kc_text); return -1; }
        for (unsigned c = 0; c < k; c++) kc_text[w++] = item[c];
        kc_text[w++] = '\t';
        for (const char *p = value; *p; p++) kc_text[w++] = *p == '\n' ? ' ' : *p;
        kc_text[w++] = '\n';
    }
    return seal(w) ? 1 : -1;
}
