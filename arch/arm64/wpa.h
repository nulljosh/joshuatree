/* WPA2-PSK, the host side. The Pi's Wi-Fi firmware has no supplicant of its own (sup_wpa answers -23, unsupported, on
   the real board), so we do the 4-way handshake: SHA-1, HMAC-SHA1, the 802.11 PRF that turns the PMK and both nonces
   into the pairwise keys, and AES key unwrap (RFC 3394) for the group key. The PMK itself (PBKDF2 over the passphrase,
   4096 rounds) is worked out at build time by wifi_cfg.sh, so the passphrase never reaches the kernel. Plain C, no
   state, no allocation; tools/checks/wpa-test.c checks it against the RFC test vectors. */
#ifndef WPA_H
#define WPA_H

static inline unsigned wpa_rol(unsigned v, int n) { return v << n | v >> (32 - n); }

typedef struct { unsigned h[5]; unsigned long long n; unsigned char b[64]; unsigned bl; } wpa_sha1;
static void wpa_sha1_block(wpa_sha1 *c, const unsigned char *p) {
    unsigned w[80], a = c->h[0], b = c->h[1], d = c->h[3], e = c->h[4], cc = c->h[2];
    for (int i = 0; i < 16; i++) w[i] = (unsigned)p[4 * i] << 24 | p[4 * i + 1] << 16 | p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 80; i++) w[i] = wpa_rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    for (int i = 0; i < 80; i++) {
        unsigned f, k;
        if (i < 20) { f = (b & cc) | (~b & d); k = 0x5A827999; }
        else if (i < 40) { f = b ^ cc ^ d; k = 0x6ED9EBA1; }
        else if (i < 60) { f = (b & cc) | (b & d) | (cc & d); k = 0x8F1BBCDC; }
        else { f = b ^ cc ^ d; k = 0xCA62C1D6; }
        unsigned t = wpa_rol(a, 5) + f + e + k + w[i];
        e = d; d = cc; cc = wpa_rol(b, 30); b = a; a = t;
    }
    c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d; c->h[4] += e;
}
static void wpa_sha1_init(wpa_sha1 *c) {
    c->h[0] = 0x67452301; c->h[1] = 0xEFCDAB89; c->h[2] = 0x98BADCFE; c->h[3] = 0x10325476; c->h[4] = 0xC3D2E1F0; c->n = 0; c->bl = 0;
}
static void wpa_sha1_add(wpa_sha1 *c, const unsigned char *p, unsigned len) {
    for (unsigned i = 0; i < len; i++) { c->b[c->bl++] = p[i]; c->n += 8; if (c->bl == 64) { wpa_sha1_block(c, c->b); c->bl = 0; } }
}
static void wpa_sha1_end(wpa_sha1 *c, unsigned char out[20]) {
    unsigned long long n = c->n; unsigned char z = 0x80;
    wpa_sha1_add(c, &z, 1); z = 0;
    while (c->bl != 56) wpa_sha1_add(c, &z, 1);
    for (int i = 7; i >= 0; i--) { unsigned char v = (unsigned char)(n >> (8 * i)); wpa_sha1_add(c, &v, 1); }
    for (int i = 0; i < 20; i++) out[i] = (unsigned char)(c->h[i / 4] >> (24 - 8 * (i % 4)));
}

/* HMAC-SHA1 over two pieces (a, then b), so the PRF needs no scratch buffer. Keys here are 16 or 32 bytes. */
static void wpa_hmac(const unsigned char *key, unsigned klen, const unsigned char *a, unsigned alen,
                     const unsigned char *b, unsigned blen, unsigned char out[20]) {
    unsigned char k[64] = {0}, pad[64], inner[20];
    for (unsigned i = 0; i < klen && i < 64; i++) k[i] = key[i];
    wpa_sha1 c;
    for (int i = 0; i < 64; i++) pad[i] = k[i] ^ 0x36;
    wpa_sha1_init(&c); wpa_sha1_add(&c, pad, 64); wpa_sha1_add(&c, a, alen); if (b) wpa_sha1_add(&c, b, blen); wpa_sha1_end(&c, inner);
    for (int i = 0; i < 64; i++) pad[i] = k[i] ^ 0x5c;
    wpa_sha1_init(&c); wpa_sha1_add(&c, pad, 64); wpa_sha1_add(&c, inner, 20); wpa_sha1_end(&c, out);
}

/* PTK = PRF-384(PMK, "Pairwise key expansion", min(AA,SPA) | max(AA,SPA) | min(ANonce,SNonce) | max(ANonce,SNonce)).
   KCK is bytes 0-15, KEK 16-31, TK 32-47. */
static int wpa_lt(const unsigned char *x, const unsigned char *y, unsigned n) {
    for (unsigned i = 0; i < n; i++) if (x[i] != y[i]) return x[i] < y[i];
    return 0;
}
static void wpa_ptk(const unsigned char pmk[32], const unsigned char aa[6], const unsigned char spa[6],
                    const unsigned char an[32], const unsigned char sn[32], unsigned char ptk[48]) {
    static const char label[] = "Pairwise key expansion";
    unsigned char d[sizeof label + 76 + 1], out[20];
    unsigned n = 0;
    for (unsigned i = 0; i < sizeof label; i++) d[n++] = (unsigned char)label[i];   /* the label and its 0 byte */
    const unsigned char *lo = wpa_lt(aa, spa, 6) ? aa : spa, *hi = lo == aa ? spa : aa;
    for (int i = 0; i < 6; i++) d[n++] = lo[i];
    for (int i = 0; i < 6; i++) d[n++] = hi[i];
    lo = wpa_lt(an, sn, 32) ? an : sn; hi = lo == an ? sn : an;
    for (int i = 0; i < 32; i++) d[n++] = lo[i];
    for (int i = 0; i < 32; i++) d[n++] = hi[i];
    for (unsigned char ctr = 0; ctr < 3; ctr++) {
        d[n] = ctr;
        wpa_hmac(pmk, 32, d, n + 1, 0, 0, out);
        for (int i = 0; i < 20 && ctr * 20 + i < 48; i++) ptk[ctr * 20 + i] = out[i];
    }
}

/* AES-128 decryption, enough for key unwrap. The S-box is built at first use from its definition (the inverse in
   GF(2^8), then the affine map), not typed in as a table. */
static unsigned char wpa_sb[256], wpa_isb[256], wpa_sb_ready;
static unsigned char wpa_xt(unsigned char x) { return (unsigned char)(x << 1 ^ (x & 0x80 ? 0x1b : 0)); }
static unsigned char wpa_mul(unsigned char a, unsigned char b) {
    unsigned char r = 0; while (b) { if (b & 1) r ^= a; a = wpa_xt(a); b >>= 1; } return r;
}
static void wpa_sbox(void) {
    if (wpa_sb_ready) return;
    for (int x = 0; x < 256; x++) {
        unsigned char inv = 0;
        if (x) for (int y = 1; y < 256; y++) if (wpa_mul((unsigned char)x, (unsigned char)y) == 1) { inv = (unsigned char)y; break; }
        unsigned char s = inv, r = inv;
        for (int i = 0; i < 4; i++) { r = (unsigned char)(r << 1 | r >> 7); s ^= r; }
        s ^= 0x63; wpa_sb[x] = s; wpa_isb[s] = (unsigned char)x;
    }
    wpa_sb_ready = 1;
}
static void wpa_aes_keys(const unsigned char key[16], unsigned char rk[176]) {
    wpa_sbox();
    for (int i = 0; i < 16; i++) rk[i] = key[i];
    unsigned char rc = 1;
    for (int i = 16; i < 176; i += 4) {
        unsigned char t[4] = {rk[i - 4], rk[i - 3], rk[i - 2], rk[i - 1]};
        if (i % 16 == 0) {
            unsigned char u = t[0]; t[0] = wpa_sb[t[1]] ^ rc; t[1] = wpa_sb[t[2]]; t[2] = wpa_sb[t[3]]; t[3] = wpa_sb[u];
            rc = wpa_xt(rc);
        }
        for (int j = 0; j < 4; j++) rk[i + j] = rk[i - 16 + j] ^ t[j];
    }
}
static void wpa_aes_dec(const unsigned char rk[176], unsigned char s[16]) {
    for (int i = 0; i < 16; i++) s[i] ^= rk[160 + i];
    for (int r = 9; r >= 0; r--) {
        unsigned char t[16];
        for (int c = 0; c < 4; c++) for (int w = 0; w < 4; w++) t[4 * ((c + w) % 4) + w] = s[4 * c + w];   /* inverse shift rows */
        for (int i = 0; i < 16; i++) s[i] = wpa_isb[t[i]] ^ rk[16 * r + i];
        if (r) for (int c = 0; c < 4; c++) {   /* inverse mix columns */
            unsigned char *m = s + 4 * c, a0 = m[0], a1 = m[1], a2 = m[2], a3 = m[3];
            m[0] = wpa_mul(a0, 14) ^ wpa_mul(a1, 11) ^ wpa_mul(a2, 13) ^ wpa_mul(a3, 9);
            m[1] = wpa_mul(a0, 9) ^ wpa_mul(a1, 14) ^ wpa_mul(a2, 11) ^ wpa_mul(a3, 13);
            m[2] = wpa_mul(a0, 13) ^ wpa_mul(a1, 9) ^ wpa_mul(a2, 14) ^ wpa_mul(a3, 11);
            m[3] = wpa_mul(a0, 11) ^ wpa_mul(a1, 13) ^ wpa_mul(a2, 9) ^ wpa_mul(a3, 14);
        }
    }
}
/* RFC 3394 unwrap: in is n+1 64-bit blocks, out gets n. Returns 1 when the check value is A6A6A6A6A6A6A6A6. */
static int wpa_unwrap(const unsigned char kek[16], const unsigned char *in, unsigned len, unsigned char *out) {
    if (len < 24 || len % 8 || len > 8 + 256) return 0;
    unsigned n = len / 8 - 1;
    unsigned char rk[176], a[8], b[16];
    wpa_aes_keys(kek, rk);
    for (int i = 0; i < 8; i++) a[i] = in[i];
    for (unsigned i = 0; i < 8 * n; i++) out[i] = in[8 + i];
    for (int j = 5; j >= 0; j--) for (unsigned i = n; i >= 1; i--) {
        unsigned long long t = (unsigned long long)n * (unsigned)j + i;
        for (int k = 0; k < 8; k++) b[k] = a[k] ^ (unsigned char)(t >> (56 - 8 * k));
        for (int k = 0; k < 8; k++) b[8 + k] = out[8 * (i - 1) + k];
        wpa_aes_dec(rk, b);
        for (int k = 0; k < 8; k++) { a[k] = b[k]; out[8 * (i - 1) + k] = b[8 + k]; }
    }
    for (int i = 0; i < 8; i++) if (a[i] != 0xA6) return 0;
    return 1;
}
#endif
