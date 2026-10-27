/* Fuzzes arch/arm64/wifi_proto.h's frame readers on the host under ASan and UBSan, run by tools/checks/wifi-fuzz-check.sh.
   escan_walk reads scan results, and what a nearby access point puts in its beacon (its name, its length fields) is
   anyone's to choose; sdpcm_parse and bcdc_reply read every frame the radio chip hands up. Each sample is truncated at
   every length and mutated a few thousand times from a fixed seed, in a buffer of exactly its size. Seconds, no QEMU. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "wifi_proto.h"

static unsigned g_sink;
static void cb(int rssi, unsigned chan, const char *ssid, unsigned slen) { for (unsigned i = 0; i < slen; i++) g_sink += (unsigned char)ssid[i]; g_sink += (unsigned)rssi + chan; }
static long g_cases;
static void one(const unsigned char *in, unsigned n) {
    g_cases++;
    unsigned char *b = malloc(n ? n : 1); memcpy(b, in, n);
    escan_walk(b, n, cb);
    unsigned off, len, o2; if (sdpcm_parse(b, n, &off, &len) >= 0 && off + len <= n) bcdc_reply(b + off, len, 9, &o2);
    bcdc_reply(b, n, 9, &o2);
    free(b);
}
static unsigned rnd_state = 99;
static unsigned rnd(void) { rnd_state = rnd_state * 1103515245u + 12345u; return rnd_state >> 8; }
static void fuzz(const char *name, const unsigned char *s, unsigned n) {
    long before = g_cases;
    for (unsigned k = 0; k <= n; k++) one(s, k);
    unsigned char *m = malloc(n + 64);
    for (int t = 0; t < 5000; t++) {
        unsigned len = n; memcpy(m, s, n);
        int edits = 1 + (int)(rnd() % 6);
        for (int e = 0; e < edits; e++) {
            unsigned p = len ? rnd() % len : 0, op = rnd() % 4;
            if (op == 0 && len) m[p] = (unsigned char)rnd();
            else if (op == 1 && len) m[p] = (unsigned char)(rnd() % 3 == 0 ? 0xff : rnd() % 3 == 0 ? 0 : rnd() % 100);
            else if (op == 2 && len + 1 < n + 64) { memmove(m + p + 1, m + p, len - p); m[p] = (unsigned char)rnd(); len++; }
            else if (len) { memmove(m + p, m + p + 1, len - p - 1); len--; }
        }
        one(m, len);
    }
    free(m);
    fprintf(stderr, "wifi-fuzz: %s, %ld cases\n", name, g_cases - before);
}
int main(void) {
    unsigned char e[12 + 2 * 128] = {0}; wr16(e + 10, 2);   /* two BSS entries, as wifi-host-check.sh builds them */
    for (int i = 0; i < 2; i++) { unsigned char *b = e + 12 + i * 128; wr32(b, 128); b[18] = 4; memcpy(b + 19, i ? "Shaw" : "Cafe", 4); wr16(b + 72, i ? 6 : 36); wr16(b + 78, (unsigned short)(i ? -51 : -70)); }
    fuzz("escan", e, sizeof e);
    for (unsigned bl = 0; bl < 128; bl++) {   /* one entry of every short length, ending the buffer: an access point can send it */
        unsigned char *x = calloc(1, 16 + bl); wr16(x + 10, 1); wr32(x + 12, bl >= 4 ? bl : 40);
        one(x, 12 + bl); free(x);
    }
    { unsigned char x[12 + 80] = {0}; wr16(x + 10, 3); wr32(x + 12, 0xffffffffu); one(x, sizeof x); }   /* a length that wraps */
    unsigned char f[64]; unsigned char p[4] = {1, 2, 3, 4};
    unsigned k = sdpcm_pack(f, 7, SDPCM_CONTROL, BCDC_GET_VAR, 9, 0, p, 4);
    fuzz("sdpcm", f, k);
    puts("WIFI FUZZ: PASS");
    return 0;
}
