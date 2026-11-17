/* 13.19.0, the card's keys: what a dev card needs to reach its Mac, kept on the card instead of inside the kernel, so a
   signed release image (which carries no secret) can replace the kernel with `update install` and still find the relay.
   tools/gen/card-keys.py writes them when tools/flash-pi.sh flashes the card:
     JT/RELAYKEY.TXT  the relay's bearer token, 16 to 63 printable characters on one line
     JT/RELAYTA.BIN   the relay CA as one BearSSL trust anchor:
                        "JTTA1", type (1 RSA, 2 EC), DN length (2 bytes, big-endian), DN,
                        RSA: n length (2), n, e length (2), e;  EC: curve (1), point length (2), point
     JT/WIFI.TXT      ssid= and pmk= lines in hex (wifi.c's net_load already reads it)
   The two parsers below are pure: no card, no BearSSL types, so tools/checks/arm64-cardkeys-check.py runs them on the
   host under sanitizers. Both read network-shaped input (a card anyone could write) and check every length. */
#ifndef CARDKEYS_H
#define CARDKEYS_H

/* The token in b (a whole RELAYKEY.TXT), trailing spaces and line ends dropped, copied to out (64 bytes) with a NUL.
   Its length, or 0 when it is not 16 to 63 printable characters with nothing after it. */
static inline unsigned cardkey_token(const char *b, unsigned n, char *out) {
    while (n && (b[n - 1] == '\n' || b[n - 1] == '\r' || b[n - 1] == ' ' || b[n - 1] == '\t')) n--;
    if (n < 16 || n > 63) return 0;
    for (unsigned i = 0; i < n; i++) if (b[i] <= ' ' || b[i] > '~') return 0;
    for (unsigned i = 0; i < n; i++) out[i] = b[i];
    out[n] = 0;
    return n;
}

struct cardta { unsigned type, curve; unsigned dn_at, dn_len, a_at, a_len, b_at, b_len; };   /* a: n or the point; b: e */
static inline unsigned cardta_be16(const unsigned char *p) { return (unsigned)p[0] << 8 | p[1]; }
/* 1 with t filled (offsets into b), 0 when b is not a whole, well-formed RELAYTA.BIN. */
static inline int cardta_parse(const unsigned char *b, unsigned n, struct cardta *t) {
    unsigned i = 6;
    if (n < 6 || b[0] != 'J' || b[1] != 'T' || b[2] != 'T' || b[3] != 'A' || b[4] != '1') return 0;
    t->type = b[5]; t->curve = 0; t->b_at = t->b_len = 0;
    if (t->type != 1 && t->type != 2) return 0;
    if (i + 2 > n) return 0;
    t->dn_len = cardta_be16(b + i); t->dn_at = i + 2; i = t->dn_at + t->dn_len;
    if (!t->dn_len || i > n) return 0;
    if (t->type == 2) {
        if (i + 3 > n) return 0;
        t->curve = b[i]; t->a_len = cardta_be16(b + i + 1); t->a_at = i + 3; i = t->a_at + t->a_len;
        if (t->curve < 23 || t->curve > 25 || !t->a_len || i != n) return 0;   /* secp256r1, secp384r1, secp521r1 */
        return 1;
    }
    if (i + 2 > n) return 0;
    t->a_len = cardta_be16(b + i); t->a_at = i + 2; i = t->a_at + t->a_len;
    if (t->a_len < 64 || i + 2 > n) return 0;   /* an RSA modulus of 512 bits at the least */
    t->b_len = cardta_be16(b + i); t->b_at = i + 2; i = t->b_at + t->b_len;
    if (!t->b_len || t->b_len > 8 || i != n) return 0;
    return 1;
}
#endif
