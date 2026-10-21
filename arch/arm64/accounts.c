/* User accounts on the Pi, the part that is not drawing. JT/USERS.TXT holds one line a user, the same record the i386
   kernel's kernel/auth.h writes: `name:p2:100000:saltHex:hashHex`. The hash is PBKDF2-HMAC-SHA256 (kernel/auth_kdf.c over
   BearSSL), a 16 byte salt from tls_entropy, 32 bytes out, compared in constant time. The password is never stored, never
   printed and never sent to the UART; every buffer that held it is wiped through a volatile pointer.
   The file is untrusted: a line that is short, long, has bad hex, a bad name or a count outside 1000 to 10000000 is skipped
   whole, never half believed. Lines without the p2 tag (the old i386 scheme) are skipped too. A change is written to the
   card first and kept in memory only if the write worked; with no card the add is refused, not quietly held in RAM.
   tools/checks/arm64-accounts-check.py runs this file on the host under sanitizers, then drives the Terminal in QEMU. */
#include "accounts.h"
#include "emmc.h"
#include "rng.h"
#include "../../drivers/fat.h"
#include "../../kernel/auth_kdf.h"

#define ACC_FILE "USERS.TXT"
#define ACC_ITERS 100000u
#define ACC_ITERS_MIN 1000u
#define ACC_ITERS_MAX 10000000u
#define ACC_SALT 16
#define ACC_HASH 32
/* name + ":p2:" + 8 digit count + ":" + 32 + ":" + 64 + "\n" */
#define ACC_LINE (ACC_NAME_MAX + 4 + 8 + 1 + ACC_SALT * 2 + 1 + ACC_HASH * 2 + 1)
#define ACC_FILE_MAX (ACC_USERS_MAX * ACC_LINE + 8)

struct acc { char name[ACC_NAME_MAX + 1]; unsigned iters; unsigned char salt[ACC_SALT], hash[ACC_HASH]; };
static struct acc users[ACC_USERS_MAX];
static unsigned nusers;

void accounts_wipe(void *p, unsigned n) { volatile unsigned char *v = p; while (n--) *v++ = 0; }
static unsigned slen(const char *s) { unsigned n = 0; while (s[n]) n++; return n; }
static int same(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }
static int ct_eq(const unsigned char *a, const unsigned char *b, unsigned n) {   /* always walks all n bytes */
    unsigned char d = 0;
    for (unsigned i = 0; i < n; i++) d |= (unsigned char)(a[i] ^ b[i]);
    return d == 0;
}
int accounts_name_ok(const char *s) {
    unsigned n = slen(s);
    if (!n || n > ACC_NAME_MAX) return 0;
    for (unsigned i = 0; i < n; i++) {
        char c = s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) return 0;
    }
    return 1;
}
int accounts_pass_ok(const char *s) { unsigned n = slen(s); return n >= ACC_PASS_MIN && n <= ACC_PASS_MAX; }
static int find(const char *name) { for (unsigned i = 0; i < nusers; i++) if (same(users[i].name, name)) return (int)i; return -1; }
static void derive(const struct acc *u, const char *pass, unsigned char out[ACC_HASH]) {
    auth_pbkdf2_sha256(pass, slen(pass), u->salt, ACC_SALT, u->iters, out, ACC_HASH);
}

static int hexv(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }
static int unhex(const char *s, unsigned bytes, unsigned char *out) {
    for (unsigned i = 0; i < bytes; i++) {
        int hi = hexv(s[2 * i]), lo = hexv(s[2 * i + 1]);
        if (hi < 0 || lo < 0) return 0;
        out[i] = (unsigned char)(hi << 4 | lo);
    }
    return 1;
}
/* One line, t[0..n) with no newline. Fills *u and returns 1 only when every field is whole and in range. */
static int parse_line(const char *t, unsigned n, struct acc *u) {
    unsigned f[5], nf = 0;
    f[nf++] = 0;
    for (unsigned i = 0; i < n; i++) if (t[i] == ':') { if (nf == 5) return 0; f[nf++] = i + 1; }
    if (nf != 5) return 0;                       /* the legacy untagged shape has 3 fields and is skipped */
    unsigned nl = f[1] - 1;
    if (!nl || nl > ACC_NAME_MAX) return 0;
    for (unsigned i = 0; i < nl; i++) {
        char c = t[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) return 0;
    }
    if (f[2] - f[1] - 1 != 2 || t[f[1]] != 'p' || t[f[1] + 1] != '2') return 0;
    unsigned dl = f[3] - f[2] - 1;
    if (!dl || dl > 8) return 0;
    unsigned long it = 0;
    for (unsigned i = 0; i < dl; i++) { char c = t[f[2] + i]; if (c < '0' || c > '9') return 0; it = it * 10 + (unsigned)(c - '0'); }
    if (it < ACC_ITERS_MIN || it > ACC_ITERS_MAX) return 0;
    if (f[4] - f[3] - 1 != ACC_SALT * 2 || n - f[4] != ACC_HASH * 2) return 0;
    struct acc tmp;
    if (!unhex(t + f[3], ACC_SALT, tmp.salt) || !unhex(t + f[4], ACC_HASH, tmp.hash)) return 0;
    for (unsigned i = 0; i < nl; i++) u->name[i] = t[i];
    u->name[nl] = 0;
    u->iters = (unsigned)it;
    for (unsigned i = 0; i < ACC_SALT; i++) u->salt[i] = tmp.salt[i];
    for (unsigned i = 0; i < ACC_HASH; i++) u->hash[i] = tmp.hash[i];
    return 1;
}
/* The whole file's text into the table: up to eight good lines, a repeated name keeps the first. Anything else is skipped. */
static void parse_text(const char *t, unsigned n) {
    nusers = 0;
    for (unsigned i = 0; i < n && nusers < ACC_USERS_MAX;) {
        unsigned s = i;
        while (i < n && t[i] != '\n') i++;
        unsigned e = i; if (i < n) i++;
        if (e > s && t[e - 1] == '\r') e--;
        struct acc u;
        if (e > s && parse_line(t + s, e - s, &u) && find(u.name) < 0) users[nusers++] = u;
    }
}
static unsigned put(char *o, unsigned k, const char *s) { while (*s) o[k++] = *s++; return k; }
/* The records of u[0..n) into out (ACC_FILE_MAX bytes); an empty list is a lone newline so the file always has a byte. */
static unsigned format(const struct acc *u, unsigned n, char *out) {
    static const char hx[] = "0123456789abcdef";
    unsigned k = 0;
    for (unsigned r = 0; r < n; r++) {
        k = put(out, k, u[r].name); k = put(out, k, ":p2:");
        char d[11]; unsigned dn = 0, v = u[r].iters;
        do { d[dn++] = (char)('0' + v % 10); v /= 10; } while (v);
        while (dn) out[k++] = d[--dn];
        out[k++] = ':';
        for (unsigned i = 0; i < ACC_SALT; i++) { out[k++] = hx[u[r].salt[i] >> 4]; out[k++] = hx[u[r].salt[i] & 15]; }
        out[k++] = ':';
        for (unsigned i = 0; i < ACC_HASH; i++) { out[k++] = hx[u[r].hash[i] >> 4]; out[k++] = hx[u[r].hash[i] & 15]; }
        out[k++] = '\n';
    }
    if (!k) out[k++] = '\n';
    return k;
}
static int store(const struct acc *u, unsigned n) {   /* 0 ok, else the error code */
    static char buf[ACC_FILE_MAX];
    if (!sd_ready()) return ACC_E_NOCARD;
    unsigned len = format(u, n, buf);
    return sd_write(ACC_FILE, buf, len) == 1 ? ACC_OK : ACC_E_WRITE;
}

void accounts_load(void) {
    static char buf[4096];           /* room for a hand-edited file with junk lines; at most eight good ones are kept */
    nusers = 0;
    if (!sd_ready() || !sd_enter_jt(0)) return;
    int got = fat_read_file(ACC_FILE, buf, sizeof buf);
    fat_chdir("..");
    if (got > 0) parse_text(buf, (unsigned)got);
}
unsigned accounts_count(void) { return nusers; }
const char *accounts_name(unsigned i) { return i < nusers ? users[i].name : ""; }

int accounts_add(const char *name, const char *pass) {
    if (!accounts_name_ok(name)) return ACC_E_NAME;
    if (!accounts_pass_ok(pass)) return ACC_E_PASS;
    if (find(name) >= 0) return ACC_E_DUP;
    if (nusers >= ACC_USERS_MAX) return ACC_E_FULL;
    if (!sd_ready()) return ACC_E_NOCARD;
    struct acc u;
    for (unsigned i = 0; i <= ACC_NAME_MAX; i++) u.name[i] = 0;
    for (unsigned i = 0; name[i]; i++) u.name[i] = name[i];
    u.iters = ACC_ITERS;
    if (!tls_entropy(u.salt, ACC_SALT)) return ACC_E_ENTROPY;
    derive(&u, pass, u.hash);
    struct acc all[ACC_USERS_MAX];
    for (unsigned i = 0; i < nusers; i++) all[i] = users[i];
    all[nusers] = u;
    int r = store(all, nusers + 1);
    if (r) return r;
    users[nusers++] = u;
    return ACC_OK;
}
int accounts_check(const char *name, const char *pass) {
    int i = find(name);
    if (i < 0) {   /* the same work as a real check, so an unknown name takes as long as a wrong password and the time tells nothing */
        static const struct acc dummy = { "", ACC_ITERS, { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 }, { 0 } };
        unsigned char h[ACC_HASH];
        derive(&dummy, pass, h);
        accounts_wipe(h, sizeof h);
        return ACC_E_NOUSER;
    }
    unsigned char h[ACC_HASH];
    derive(&users[i], pass, h);
    int ok = ct_eq(h, users[i].hash, ACC_HASH);
    accounts_wipe(h, sizeof h);
    return ok ? ACC_OK : ACC_E_WRONG;
}
int accounts_has(const char *name) { return find(name) >= 0; }
int accounts_protected(const char *name) {   /* the two account files, whatever the case: a file command must not touch them */
    static const char *const files[] = { "USERS.TXT", "SUDOERS.TXT" };
    for (unsigned f = 0; f < 2; f++) {
        const char *a = name, *b = files[f];
        while (*a && *b && (*a >= 'a' && *a <= 'z' ? *a - 32 : *a) == *b) { a++; b++; }
        if (!*a && !*b) return 1;
    }
    return 0;
}
int accounts_passwd(const char *name, const char *pass) {   /* a new password: fresh salt, written to the card first */
    int at = find(name);
    if (at < 0) return ACC_E_NOUSER;
    if (!accounts_pass_ok(pass)) return ACC_E_PASS;
    if (!sd_ready()) return ACC_E_NOCARD;
    struct acc u = users[at];
    u.iters = ACC_ITERS;
    if (!tls_entropy(u.salt, ACC_SALT)) return ACC_E_ENTROPY;
    derive(&u, pass, u.hash);
    struct acc all[ACC_USERS_MAX];
    for (unsigned i = 0; i < nusers; i++) all[i] = users[i];
    all[at] = u;
    int r = store(all, nusers);
    if (r) return r;
    users[at] = u;
    return ACC_OK;
}
int accounts_delete(const char *name) {   /* an admin's removal: no password of the account's own needed */
    int at = find(name);
    if (at < 0) return ACC_E_NOUSER;
    struct acc rest[ACC_USERS_MAX];
    unsigned m = 0;
    for (unsigned i = 0; i < nusers; i++) if ((int)i != at) rest[m++] = users[i];
    int r = store(rest, m);
    if (r) return r;
    for (unsigned i = 0; i < m; i++) users[i] = rest[i];
    nusers = m;
    return ACC_OK;
}
int accounts_remove(const char *name, const char *pass) {
    int r = accounts_check(name, pass);
    return r ? r : accounts_delete(name);
}
const char *accounts_reason(int code) {
    switch (code) {
    case ACC_E_NAME: return "the name is 1 to 24 characters of a to z, 0 to 9, _ and -";
    case ACC_E_PASS: return "the password is 4 to 64 characters";
    case ACC_E_DUP: return "that name is taken";
    case ACC_E_FULL: return "eight accounts is the most";
    case ACC_E_NOCARD: return "no SD card, so nothing could be kept";
    case ACC_E_WRITE: return "the card would not take the write, nothing changed";
    case ACC_E_NOUSER: return "no such account";
    case ACC_E_WRONG: return "wrong password";
    case ACC_E_ENTROPY: return "no random salt could be made";
    }
    return "done";
}
