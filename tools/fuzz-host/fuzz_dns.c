/* Fuzzes drivers/dns.h's dns_answer on the host under ASan and UBSan, run by tools/checks/dns-fuzz-check.sh. Every DNS
   reply both kernels believe (any host on the network can send one, and its 16-bit id is guessable) goes through it.
   A real reply (a CNAME, then an A record, names compressed) is truncated at every length and mutated from a fixed seed,
   in a buffer of exactly its size, plus hand-made ones: a huge answer count, a huge rdlength, a name that never ends.
   It must also still find the address in the good reply. Seconds, no QEMU. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dns.h"

static long g_cases;
static int one(const unsigned char *in, unsigned n, unsigned *ip) {
    g_cases++;
    unsigned char *b = malloc(n ? n : 1); memcpy(b, in, n);
    int r = dns_answer(b, n, 0x1234, ip);
    free(b);
    return r;
}
static unsigned rnd_state = 31337;
static unsigned rnd(void) { rnd_state = rnd_state * 1103515245u + 12345u; return rnd_state >> 8; }
int main(void) {
    static const unsigned char good[] = {
        0x12, 0x34, 0x81, 0x80, 0, 1, 0, 2, 0, 0, 0, 0,
        7, 'e', 'x', 'a', 'm', 'p', 'l', 'e', 3, 'c', 'o', 'm', 0, 0, 1, 0, 1,
        0xc0, 12, 0, 5, 0, 1, 0, 0, 0, 60, 0, 6, 3, 'w', 'w', 'w', 0xc0, 12,   /* CNAME www.example.com */
        0xc0, 41, 0, 1, 0, 1, 0, 0, 0, 60, 0, 4, 93, 184, 216, 34 };            /* A 93.184.216.34 */
    unsigned ip = 0, n = sizeof good;
    if (one(good, n, &ip) != 1 || ip != (93u << 24 | 184u << 16 | 216u << 8 | 34u)) { printf("FAIL: the good reply did not give 93.184.216.34 (%x)\n", ip); return 1; }
    unsigned char other[sizeof good]; memcpy(other, good, n); other[1] = 0x35;
    if (one(other, n, &ip) != -1) { puts("FAIL: a reply to another id was believed"); return 1; }
    for (unsigned k = 0; k < n; k++) if (one(good, k, &ip) == 1 && k < n) { if (ip != (93u << 24 | 184u << 16 | 216u << 8 | 34u)) { puts("FAIL: a cut reply gave another address"); return 1; } }
    unsigned char m[sizeof good + 64];
    for (int t = 0; t < 20000; t++) {
        unsigned len = n; memcpy(m, good, n);
        int edits = 1 + (int)(rnd() % 5);
        for (int e = 0; e < edits; e++) {
            unsigned p = rnd() % len, op = rnd() % 4;
            if (op == 0) m[p] = (unsigned char)rnd();
            else if (op == 1) m[p] = (unsigned char)(rnd() % 2 ? 0xff : 0xc0 | (rnd() & 0x3f));
            else if (op == 2 && len + 1 < sizeof m) { memmove(m + p + 1, m + p, len - p); m[p] = (unsigned char)rnd(); len++; }
            else if (len > 13) { memmove(m + p, m + p + 1, len - p - 1); len--; }
        }
        one(m, len, &ip);
    }
    { unsigned char x[12 + 5] = { 0x12, 0x34, 0x81, 0x80, 0, 1, 0xff, 0xff, 0, 0, 0, 0, 0, 0, 1, 0, 1 }; one(x, sizeof x, &ip); }   /* 65535 answers, no room */
    { unsigned char x[12 + 5 + 13] = { 0x12, 0x34, 0x81, 0x80, 0, 1, 0, 1, 0, 0, 0, 0, 0, 0, 1, 0, 1, 0, 0, 1, 0, 1, 0, 0, 0, 0, 0xff, 0xff, 1, 2 }; one(x, sizeof x, &ip); }   /* rdlength 65535 */
    { unsigned char x[12 + 40]; memset(x, 63, sizeof x); x[0] = 0x12; x[1] = 0x34; x[6] = 0; x[7] = 1; one(x, sizeof x, &ip); }   /* a name that never ends */
    fprintf(stderr, "dns-fuzz: %ld cases\n", g_cases);
    puts("DNS FUZZ: PASS");
    return 0;
}
