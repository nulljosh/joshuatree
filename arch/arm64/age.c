/* 13.26.0: age file encryption on the Pi (age-encryption.org/v1), the format the `age` tool reads and writes, so a file
   locked here opens on a Mac with `age -d` and the other way round. One X25519 key per card, JT/AGEKEY.TXT, kept like
   the account files (accounts_protected: no cat, no write, never an input or output here).
     age keygen                 a new key, written as age-keygen writes one; the public key (age1...) is printed
     age pub                    the public key again
     age enc IN OUT [age1...]   IN locked to a recipient (your own key when none is given) into OUT, all in JT/
     age dec IN OUT             a file locked to your key, opened
   The format: a random 16-byte file key; per recipient an ephemeral X25519 share and the file key wrapped with
   ChaCha20-Poly1305 under HKDF-SHA256(shared secret, share || recipient, "age-encryption.org/v1/X25519"); a header
   MAC (HMAC-SHA256 under HKDF(file key, "header")); then a 16-byte nonce and the payload in 64 KiB ChaCha20-Poly1305
   chunks under HKDF(file key, nonce, "payload"), each nonce an 11-byte counter and a last-chunk flag. Keys are Bech32.
   Every primitive is the vendored BearSSL. BearSSL's X25519 takes the scalar big-endian, so ours is reversed first.
   The secret key and file key are wiped after each command. tools/checks/arm64-age-check.py runs it against `age`. */
#include <stddef.h>
#include "bearssl.h"
#include "rng.h"

extern void (*cmd_out)(const char *s); void cmd_dec(unsigned v);   /* cmd_out is a pointer: the Terminal, or the UART while a sheet runs */
void *kmalloc(unsigned int n); unsigned long heap_mark(void); void heap_release(unsigned long m);
int sd_ready(void); int sd_enter_jt(int make); int sd_write(const char *name, const void *data, unsigned len);
int fat_read_file(const char *name, void *buf, unsigned bufsize); void fat_cwd_set(unsigned cluster);
int accounts_protected(const char *name); void accounts_wipe(void *p, unsigned n);

#define AGE_KEYFILE "AGEKEY.TXT"
#define AGE_MAX (4u << 20)                 /* the largest file it locks or opens: 4 MiB */
#define CHUNK 65536u

static unsigned slen(const char *s) { unsigned n = 0; while (s[n]) n++; return n; }
static int same(const void *a, const void *b, unsigned n) {   /* constant time */
    const unsigned char *x = a, *y = b; unsigned char d = 0; while (n--) d |= *x++ ^ *y++; return d == 0;
}

/* ---- Bech32 (BIP 173), lower case out, either case in ---- */
static const char B32[] = "qpzry9x8gf2tvdw0s3jn54khce6mua7l";
static unsigned polymod(const unsigned char *v, unsigned n) {
    static const unsigned G[5] = { 0x3b6a57b2, 0x26508e6d, 0x1ea119fa, 0x3d4233dd, 0x2a1462b3 };
    unsigned c = 1;
    for (unsigned i = 0; i < n; i++) {
        unsigned b = c >> 25; c = (c & 0x1ffffff) << 5 ^ v[i];
        for (int k = 0; k < 5; k++) if (b >> k & 1) c ^= G[k];
    }
    return c;
}
static unsigned hrp_expand(const char *hrp, unsigned char *v) {
    unsigned h = slen(hrp), n = 0;
    for (unsigned i = 0; i < h; i++) v[n++] = (unsigned char)(hrp[i] >> 5);
    v[n++] = 0;
    for (unsigned i = 0; i < h; i++) v[n++] = (unsigned char)(hrp[i] & 31);
    return n;
}
static void bech32_enc(const char *hrp, const unsigned char key[32], char *out, int upper) {   /* out: hrp + "1" + 52 + 6 */
    unsigned char d[52], v[128]; unsigned nd = 0, acc = 0, bits = 0;
    for (unsigned i = 0; i < 32; i++) { acc = acc << 8 | key[i]; bits += 8; while (bits >= 5) { bits -= 5; d[nd++] = (unsigned char)(acc >> bits & 31); } }
    if (bits) d[nd++] = (unsigned char)(acc << (5 - bits) & 31);
    unsigned n = hrp_expand(hrp, v);
    for (unsigned i = 0; i < nd; i++) v[n++] = d[i];
    for (int i = 0; i < 6; i++) v[n++] = 0;
    unsigned pm = polymod(v, n) ^ 1, o = 0;
    for (const char *h = hrp; *h; h++) out[o++] = *h;
    out[o++] = '1';
    for (unsigned i = 0; i < nd; i++) out[o++] = B32[d[i]];
    for (int i = 0; i < 6; i++) out[o++] = B32[pm >> 5 * (5 - i) & 31];
    out[o] = 0;
    if (upper) for (unsigned i = 0; i < o; i++) if (out[i] >= 'a' && out[i] <= 'z') out[i] = (char)(out[i] - 32);
}
static int bech32_dec(const char *hrp, const char *s, unsigned len, unsigned char key[32]) {   /* 1 when s is hrp's 32 bytes */
    unsigned h = slen(hrp), lo = 0, up = 0;
    if (len != h + 1 + 52 + 6) return 0;
    char t[128];
    for (unsigned i = 0; i < len; i++) {
        char c = s[i];
        if (c >= 'a' && c <= 'z') lo = 1; else if (c >= 'A' && c <= 'Z') { up = 1; c = (char)(c + 32); }
        t[i] = c;
    }
    if (lo && up) return 0;
    for (unsigned i = 0; i < h; i++) if (t[i] != hrp[i]) return 0;
    if (t[h] != '1') return 0;
    unsigned char v[128], d[58]; unsigned n = hrp_expand(hrp, v);
    for (unsigned i = 0; i < 58; i++) {
        unsigned k = 0; while (k < 32 && B32[k] != t[h + 1 + i]) k++;
        if (k == 32) return 0;
        d[i] = (unsigned char)k; v[n++] = (unsigned char)k;
    }
    if (polymod(v, n) != 1) return 0;
    unsigned acc = 0, bits = 0, o = 0;
    for (unsigned i = 0; i < 52; i++) { acc = acc << 5 | d[i]; bits += 5; if (bits >= 8) { bits -= 8; key[o++] = (unsigned char)(acc >> bits); } }
    return o == 32 && !(acc & ((1u << bits) - 1));
}

/* ---- base64, standard alphabet, no padding (age's header encoding) ---- */
static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static unsigned b64_enc(const unsigned char *in, unsigned n, char *out) {
    unsigned o = 0, i = 0;
    for (; i + 3 <= n; i += 3) {
        unsigned v = (unsigned)in[i] << 16 | in[i + 1] << 8 | in[i + 2];
        for (int k = 3; k >= 0; k--) out[o++] = B64[v >> 6 * k & 63];
    }
    if (n - i == 1) { unsigned v = (unsigned)in[i] << 16; out[o++] = B64[v >> 18 & 63]; out[o++] = B64[v >> 12 & 63]; }
    if (n - i == 2) { unsigned v = (unsigned)in[i] << 16 | in[i + 1] << 8; out[o++] = B64[v >> 18 & 63]; out[o++] = B64[v >> 12 & 63]; out[o++] = B64[v >> 6 & 63]; }
    return o;
}
static int b64_dec(const char *s, unsigned n, unsigned char *out, unsigned cap) {   /* bytes, or -1; canonical only */
    unsigned acc = 0, bits = 0, o = 0;
    if (n % 4 == 1) return -1;
    for (unsigned i = 0; i < n; i++) {
        unsigned k = 0; while (k < 64 && B64[k] != s[i]) k++;
        if (k == 64) return -1;
        acc = acc << 6 | k; bits += 6;
        if (bits >= 8) { bits -= 8; if (o == cap) return -1; out[o++] = (unsigned char)(acc >> bits); }
    }
    if (acc & ((1u << bits) - 1)) return -1;   /* stray low bits: not the one encoding */
    return (int)o;
}

/* ---- the primitives ---- */
static void x25519(unsigned char out[32], const unsigned char scalar[32], const unsigned char point[32]) {
    unsigned char k[32];
    for (int i = 0; i < 32; i++) { k[i] = scalar[31 - i]; out[i] = point[i]; }   /* BearSSL wants the scalar big-endian */
    br_ec_c25519_m31.mul(out, 32, k, 32, BR_EC_curve25519);
    accounts_wipe(k, sizeof k);
}
static const unsigned char BASE[32] = { 9 };
static void hkdf(const void *salt, unsigned sn, const void *ikm, unsigned in, const char *info, unsigned char *out, unsigned on) {
    br_hkdf_context hc;
    br_hkdf_init(&hc, &br_sha256_vtable, sn ? salt : BR_HKDF_NO_SALT, sn);
    br_hkdf_inject(&hc, ikm, in);
    br_hkdf_flip(&hc);
    br_hkdf_produce(&hc, info, slen(info), out, on);
    accounts_wipe(&hc, sizeof hc);
}
static void seal(const unsigned char key[32], const unsigned char nonce[12], unsigned char *data, unsigned n, unsigned char tag[16]) {
    br_poly1305_ctmul_run(key, nonce, data, n, 0, 0, tag, br_chacha20_ct_run, 1);
}
static int open_(const unsigned char key[32], const unsigned char nonce[12], unsigned char *data, unsigned n, const unsigned char tag[16]) {
    unsigned char t[16];
    br_poly1305_ctmul_run(key, nonce, data, n, 0, 0, t, br_chacha20_ct_run, 0);
    return same(t, tag, 16);
}
static void header_mac(const unsigned char fk[16], const unsigned char *hdr, unsigned n, unsigned char mac[32]) {
    unsigned char k[32]; br_hmac_key_context kc; br_hmac_context hc;
    hkdf(0, 0, fk, 16, "header", k, 32);
    br_hmac_key_init(&kc, &br_sha256_vtable, k, 32);
    br_hmac_init(&hc, &kc, 0);
    br_hmac_update(&hc, hdr, n);
    br_hmac_out(&hc, mac);
    accounts_wipe(k, sizeof k); accounts_wipe(&kc, sizeof kc); accounts_wipe(&hc, sizeof hc);
}
/* The payload, in place: n bytes of plaintext at p become n + 16 per chunk at out (out may not overlap p). */
static unsigned payload_seal(const unsigned char fk[16], const unsigned char nonce16[16], const unsigned char *p, unsigned n, unsigned char *out) {
    unsigned char k[32], iv[12] = { 0 }; unsigned o = 0, i = 0, ctr = 0;
    hkdf(nonce16, 16, fk, 16, "payload", k, 32);
    do {
        unsigned m = n - i > CHUNK ? CHUNK : n - i;
        iv[7] = (unsigned char)(ctr >> 24); iv[8] = (unsigned char)(ctr >> 16); iv[9] = (unsigned char)(ctr >> 8); iv[10] = (unsigned char)ctr;
        iv[11] = i + m == n;
        for (unsigned j = 0; j < m; j++) out[o + j] = p[i + j];
        seal(k, iv, out + o, m, out + o + m);
        o += m + 16; i += m; ctr++;
    } while (i < n);
    accounts_wipe(k, sizeof k);
    return o;
}
static int payload_open(const unsigned char fk[16], const unsigned char *c, unsigned n, unsigned char *out) {   /* bytes, or -1 */
    if (n < 16 + 16) return -1;
    unsigned char k[32], iv[12] = { 0 }; unsigned o = 0, i = 16, ctr = 0;
    hkdf(c, 16, fk, 16, "payload", k, 32);
    while (i < n) {
        unsigned left = n - i, last = left <= CHUNK + 16, m = (last ? left : CHUNK + 16) - 16;
        if (left < 16 || (last && m == 0 && ctr)) { o = (unsigned)-1; break; }   /* an empty last chunk only for an empty file */
        iv[7] = (unsigned char)(ctr >> 24); iv[8] = (unsigned char)(ctr >> 16); iv[9] = (unsigned char)(ctr >> 8); iv[10] = (unsigned char)ctr;
        iv[11] = (unsigned char)last;
        for (unsigned j = 0; j < m; j++) out[o + j] = c[i + j];
        if (!open_(k, iv, out + o, m, c + i + m)) { o = (unsigned)-1; break; }
        o += m; i += m + 16; ctr++;
    }
    accounts_wipe(k, sizeof k);
    return (int)o;
}

/* ---- the key file ---- */
static unsigned char sk[32];               /* the card's secret key while a command runs, wiped after */
static int load_key(void) {                /* 1 with sk set, 0 when there is none, -1 when it is not a key */
    char t[512]; int got = -1;
    if (sd_ready() && sd_enter_jt(0)) { got = fat_read_file(AGE_KEYFILE, t, sizeof t - 1); fat_cwd_set(0); }
    if (got < 0) return 0;
    int ok = -1;
    for (int i = 0; i < got; ) {
        int e = i; while (e < got && t[e] != '\n' && t[e] != '\r') e++;
        if (e > i && t[i] != '#' && bech32_dec("age-secret-key-", t + i, (unsigned)(e - i), sk)) { ok = 1; break; }
        i = e + 1;
    }
    accounts_wipe(t, sizeof t);
    return ok;
}
static void public_of(char out[64]) { unsigned char pk[32]; x25519(pk, sk, BASE); bech32_enc("age", pk, out, 0); }
static void done(void) { accounts_wipe(sk, sizeof sk); }

static void keygen(void) {
    int have = load_key();
    if (have) { done(); cmd_out(have > 0 ? "age: this card already has a key, age pub shows it\n" : "age: JT/AGEKEY.TXT is there but holds no key, remove it first\n"); return; }
    if (!sd_ready()) { cmd_out("age: no SD card\n"); return; }
    if (!tls_entropy(sk, 32)) { cmd_out("age: no randomness, no key made\n"); return; }
    char pub[64], sec[80], file[200]; unsigned o = 0;
    public_of(pub); bech32_enc("age-secret-key-", sk, sec, 1);
    const char *a = "# created: by Joshua Tree\n# public key: ";
    for (unsigned i = 0; a[i]; i++) file[o++] = a[i];
    for (unsigned i = 0; pub[i]; i++) file[o++] = pub[i];
    file[o++] = '\n';
    for (unsigned i = 0; sec[i]; i++) file[o++] = sec[i];
    file[o++] = '\n';
    int ok = sd_write(AGE_KEYFILE, file, o);
    accounts_wipe(sec, sizeof sec); accounts_wipe(file, sizeof file); done();
    if (ok <= 0) { cmd_out("age: JT/AGEKEY.TXT could not be written\n"); return; }
    cmd_out("age: a new key in JT/AGEKEY.TXT; your public key is "); cmd_out(pub); cmd_out("\n");
}

static int read_jt(const char *name, unsigned char *buf, unsigned cap) {   /* bytes, -1 no file, -2 too big */
    int got = -1;
    if (sd_ready() && sd_enter_jt(0)) { got = fat_read_file(name, buf, cap + 1); fat_cwd_set(0); }
    if (got > (int)cap) return -2;
    return got;
}
static int name_ok(const char *verb, const char *name) {
    if (!name[0]) { cmd_out("age: use age "); cmd_out(verb); cmd_out(verb[0] == 'e' ? " IN OUT [age1...]\n" : " IN OUT\n"); return 0; }
    if (accounts_protected(name)) { cmd_out("age: "); cmd_out(name); cmd_out(" is a key or account file, age leaves it alone\n"); return 0; }
    return 1;
}

static void encrypt(const char *in, const char *out, const char *to, unsigned tn) {
    unsigned char rk[32];
    if (tn) { if (!bech32_dec("age", to, tn, rk)) { cmd_out("age: that is not an age1... public key\n"); return; } }
    else {
        int k = load_key();
        if (k <= 0) { cmd_out(k ? "age: JT/AGEKEY.TXT holds no key\n" : "age: no key yet, age keygen makes one\n"); return; }
        x25519(rk, sk, BASE); done();
    }
    unsigned long mark = heap_mark();
    unsigned char *p = kmalloc(AGE_MAX + 1), *c = kmalloc(AGE_MAX + AGE_MAX / CHUNK * 16 + 512);
    if (!p || !c) { heap_release(mark); cmd_out("age: not enough memory\n"); return; }
    int n = read_jt(in, p, AGE_MAX);
    if (n < 0) { heap_release(mark); cmd_out(n == -2 ? "age: that file is over 4 MiB\n" : "age: no file JT/"); if (n != -2) { cmd_out(in); cmd_out("\n"); } return; }
    unsigned char fk[16], e[32], share[32], shared[32], salt[64], wk[32], body[32], nonce[16], mac[32], zero12[12] = { 0 };
    if (!tls_entropy(fk, 16) || !tls_entropy(e, 32) || !tls_entropy(nonce, 16)) { heap_release(mark); cmd_out("age: no randomness\n"); return; }
    x25519(share, e, BASE); x25519(shared, e, rk);
    for (int i = 0; i < 32; i++) { salt[i] = share[i]; salt[32 + i] = rk[i]; }
    hkdf(salt, 64, shared, 32, "age-encryption.org/v1/X25519", wk, 32);
    for (int i = 0; i < 16; i++) body[i] = fk[i];
    seal(wk, zero12, body, 16, body + 16);
    unsigned o = 0;
    const char *h1 = "age-encryption.org/v1\n-> X25519 ";
    for (unsigned i = 0; h1[i]; i++) c[o++] = (unsigned char)h1[i];
    o += b64_enc(share, 32, (char *)c + o); c[o++] = '\n';
    o += b64_enc(body, 32, (char *)c + o); c[o++] = '\n';
    c[o++] = '-'; c[o++] = '-'; c[o++] = '-';
    header_mac(fk, c, o, mac);
    c[o++] = ' '; o += b64_enc(mac, 32, (char *)c + o); c[o++] = '\n';
    for (int i = 0; i < 16; i++) c[o++] = nonce[i];
    o += payload_seal(fk, nonce, p, (unsigned)n, c + o);
    accounts_wipe(fk, 16); accounts_wipe(e, 32); accounts_wipe(shared, 32); accounts_wipe(wk, 32); accounts_wipe(p, (unsigned)n);
    int ok = sd_write(out, c, o);
    heap_release(mark);
    if (ok <= 0) { cmd_out("age: JT/"); cmd_out(out); cmd_out(" could not be written\n"); return; }
    cmd_out("age: locked JT/"); cmd_out(in); cmd_out(" into JT/"); cmd_out(out); cmd_out(", "); cmd_dec(o); cmd_out(" bytes\n");
}

static int line(const unsigned char *c, unsigned n, unsigned *i, const unsigned char **s) {   /* the next header line, or -1 */
    unsigned b = *i; while (*i < n && c[*i] != '\n') (*i)++;
    if (*i >= n) return -1;
    *s = c + b; return (int)((*i)++ - b);
}
static int starts(const unsigned char *s, int n, const char *w) { int k = 0; for (; w[k]; k++) if (k >= n || s[k] != (unsigned char)w[k]) return 0; return 1; }

static void decrypt(const char *in, const char *out) {
    int k = load_key();
    if (k <= 0) { cmd_out(k ? "age: JT/AGEKEY.TXT holds no key\n" : "age: no key yet, age keygen makes one\n"); return; }
    unsigned long mark = heap_mark();
    unsigned char *c = kmalloc(AGE_MAX + AGE_MAX / CHUNK * 16 + 4096), *p = kmalloc(AGE_MAX + 1);
    if (!c || !p) { heap_release(mark); done(); cmd_out("age: not enough memory\n"); return; }
    int n = read_jt(in, c, AGE_MAX + AGE_MAX / CHUNK * 16 + 4095);
    if (n < 0) { heap_release(mark); done(); cmd_out(n == -2 ? "age: that file is too big\n" : "age: no file JT/"); if (n != -2) { cmd_out(in); cmd_out("\n"); } return; }
    unsigned char pk[32], fk[16], found = 0; x25519(pk, sk, BASE);
    unsigned i = 0; const unsigned char *s; int len = line(c, (unsigned)n, &i, &s);
    const char *why = 0;
    if (len != 21 || !starts(s, len, "age-encryption.org/v1")) why = "that is not an age file";
    while (!why) {
        unsigned at = i;
        len = line(c, (unsigned)n, &i, &s);
        if (len < 0) { why = "the header ends early"; break; }
        if (starts(s, len, "---")) {
            unsigned char mac[32], want[32];
            if (len != 47 || s[3] != ' ' || b64_dec((const char *)s + 4, 43, want, 32) != 32) { why = "the header MAC is malformed"; break; }
            if (!found) { why = "it is not locked to this card's key"; break; }
            header_mac(fk, c, at + 3, mac);
            if (!same(mac, want, 32)) { why = "the header was changed"; break; }
            int m = payload_open(fk, c + i, (unsigned)n - i, p);
            if (m < 0) { why = "the contents were changed or cut short"; break; }
            int ok = sd_write(out, p, (unsigned)m);
            accounts_wipe(p, (unsigned)m);
            if (ok <= 0) { why = "the output could not be written"; break; }
            cmd_out("age: opened JT/"); cmd_out(in); cmd_out(" into JT/"); cmd_out(out); cmd_out(", "); cmd_dec((unsigned)m); cmd_out(" bytes\n");
            break;
        }
        if (!starts(s, len, "-> ")) { why = "the header is malformed"; break; }
        const unsigned char *a = s; int an = len;               /* the stanza's arguments, then its body lines */
        unsigned char body[48]; int bn = 0;
        for (;;) {
            int bl = line(c, (unsigned)n, &i, &s);
            if (bl < 0 || bl > 64) { why = "a stanza is malformed"; break; }
            unsigned char tmp[48]; int got = b64_dec((const char *)s, (unsigned)bl, tmp, sizeof tmp);
            if (got < 0) { why = "a stanza is malformed"; break; }
            for (int j = 0; j < got; j++) { if (bn < (int)sizeof body) body[bn] = tmp[j]; bn++; }
            if (bl < 64) break;
        }
        if (why) break;
        if (found || an != 3 + 7 + 43 || !starts(a + 3, an - 3, "X25519 ") || bn != 32) continue;
        unsigned char share[32], shared[32], salt[64], wk[32], zero12[12] = { 0 };
        if (b64_dec((const char *)a + 10, 43, share, 32) != 32) continue;
        x25519(shared, sk, share);
        for (int j = 0; j < 32; j++) { salt[j] = share[j]; salt[32 + j] = pk[j]; }
        hkdf(salt, 64, shared, 32, "age-encryption.org/v1/X25519", wk, 32);
        if (open_(wk, zero12, body, 16, body + 16)) { for (int j = 0; j < 16; j++) fk[j] = body[j]; found = 1; }
        accounts_wipe(shared, 32); accounts_wipe(wk, 32); accounts_wipe(body, sizeof body);
    }
    accounts_wipe(fk, sizeof fk); done();
    heap_release(mark);
    if (why) { cmd_out("age: not opened, "); cmd_out(why); cmd_out("\n"); }
}

static int eq(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }
static unsigned word(const char *q, unsigned n, unsigned *i, char *out, unsigned cap) {
    while (*i < n && q[*i] == ' ') (*i)++;
    unsigned k = 0, b = *i;
    while (*i < n && q[*i] != ' ') { if (k + 1 < cap) out[k++] = q[*i]; (*i)++; }
    out[k] = 0;
    return *i - b;
}
void age_command(const char *q, unsigned n) {   /* the whole line, starting with "age" */
    char verb[8], a[64], b[64], r[80]; unsigned i = 3;
    word(q, n, &i, verb, sizeof verb);
    if (!verb[0]) { cmd_out("age keygen, age pub, age enc IN OUT [age1...], age dec IN OUT (files in JT/)\n"); return; }
    if (!sd_ready()) { cmd_out("age: no SD card\n"); return; }
    if (eq(verb, "keygen")) { keygen(); return; }
    if (eq(verb, "pub")) {
        int k = load_key();
        if (k <= 0) { cmd_out(k ? "age: JT/AGEKEY.TXT holds no key\n" : "age: no key yet, age keygen makes one\n"); return; }
        char pub[64]; public_of(pub); done();
        cmd_out("age: your public key is "); cmd_out(pub); cmd_out("\n");
        return;
    }
    int enc = eq(verb, "enc"), dec = eq(verb, "dec");
    if (!enc && !dec) { cmd_out("age: age keygen, age pub, age enc IN OUT [age1...], age dec IN OUT\n"); return; }
    word(q, n, &i, a, sizeof a); word(q, n, &i, b, sizeof b);
    unsigned rn = word(q, n, &i, r, sizeof r);
    if (!name_ok(verb, a) || !name_ok(verb, b[0] ? b : "")) return;
    if (dec && rn) { cmd_out("age: use age dec IN OUT\n"); return; }
    if (enc) encrypt(a, b, r, rn > sizeof r - 1 ? sizeof r : rn); else decrypt(a, b);
}
