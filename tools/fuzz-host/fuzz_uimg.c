/* Fuzzes arch/arm64/uimg.h's uimg_ok on the host under ASan and UBSan, run by tools/checks/uimg-fuzz-check.sh. Since 6.51.0 an
   app can come off the card (JT/APPS/NAME.APP), so every byte of an image is untrusted. A good image (one code page, a data
   word, two relocations, the trailer) is cut at every length and mutated from a fixed seed, in a buffer of exactly its
   size; every image uimg_ok passes then has its relocations applied the way main.c's user_load does, into that buffer, so
   a check that lets a write out of the image is an ASan error. It must still pass the good image. Seconds, no QEMU. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "uimg.h"

#define ZERO 8
static long g_cases, g_passed;
static void put32(unsigned char *p, unsigned v) { for (int i = 0; i < 4; i++) p[i] = (unsigned char)(v >> 8 * i); }
static void put64(unsigned char *p, unsigned long v) { put32(p, (unsigned)v); put32(p + 4, (unsigned)(v >> 32)); }
static int one(const unsigned char *in, unsigned long n) {
    g_cases++;
    char *b = malloc(n ? n : 1); memcpy(b, in, n);
    unsigned ce, rs, re;
    int ok = uimg_ok(b, n, 0x40000, ZERO, &ce, &rs, &re);
    if (ok) {   /* user_load's apply: base + off gets base + addend */
        g_passed++;
        for (unsigned long r = rs; r < re; r += 24) { unsigned long off = uimg_rd64(b + r); memcpy(b + off, &(unsigned long){ 0x10000 + uimg_rd64(b + r + 16) }, 8); }
    }
    free(b);
    return ok;
}
static unsigned rnd_state = 4242;
static unsigned rnd(void) { rnd_state = rnd_state * 1103515245u + 12345u; return rnd_state >> 8; }
int main(void) {
    enum { N = 4096 + 16 + 48 + 16 };   /* code page, two data words, two relocations, the trailer */
    unsigned char good[N] = { 0 };
    put64(good + 4112, 4096 + 8); put64(good + 4120, 1027); put64(good + 4128, 4104);   /* the first data word points at the second */
    put64(good + 4136, 4096); put64(good + 4144, 1027); put64(good + 4152, 0);           /* the second at the code */
    put32(good + N - 16, UIMG_MAGIC); put32(good + N - 12, 4096); put32(good + N - 8, 4112); put32(good + N - 4, 4160);
    if (!one(good, N)) { puts("FAIL: the good image was refused"); return 1; }
    for (unsigned long k = 0; k < N; k++) one(good, k);
    unsigned char m[N + 64];
    for (int t = 0; t < 40000; t++) {
        unsigned long len = N; memcpy(m, good, N);
        int edits = 1 + (int)(rnd() % 4);
        for (int e = 0; e < edits; e++) {
            unsigned long p = 4096 + rnd() % (len - 4096), op = rnd() % 5;   /* the code page is never read */
            if (op == 0) m[p] = (unsigned char)rnd();
            else if (op == 1) m[p] = (unsigned char)(rnd() % 2 ? 0xff : 0xf0 | (rnd() & 15));
            else if (op == 2) put32(m + (p & ~3UL), rnd() % 2 ? rnd() : (unsigned)len - (rnd() % 64));
            else if (op == 3 && len + 4 < sizeof m) { memmove(m + p + 4, m + p, len - p); len += 4; }
            else if (len > 4096 + 32) { memmove(m + p, m + p + 4, len - p - 4 > len ? 0 : len - p - 4); len -= 4; }
        }
        one(m, len);
    }
    unsigned char x[N]; memcpy(x, good, N);
    put64(x + 4112, 0xFFFFFFF0); if (one(x, N)) { puts("FAIL: a relocation 4 GB out was passed"); return 1; }
    memcpy(x, good, N); put64(x + 4112, N - 20); if (one(x, N)) { puts("FAIL: a relocation over the trailer was passed"); return 1; }
    memcpy(x, good, N); put32(x + N - 4, 0xFFFFFFF8); if (one(x, N)) { puts("FAIL: a relocation list past the end was passed"); return 1; }
    memcpy(x, good, N); put64(x + 4112, 4136); if (one(x, N)) { puts("FAIL: a relocation that rewrites the next one was passed"); return 1; }
    fprintf(stderr, "uimg-fuzz: %ld cases, %ld passed\n", g_cases, g_passed);
    puts("UIMG FUZZ: PASS");
    return 0;
}
