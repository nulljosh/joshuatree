/* Fuzzes arch/arm64/appsig.c on the host under ASan and UBSan, with the vendored BearSSL, run by tools/checks/appsig-fuzz-check.sh.
   NAME.SIG is a file anyone can put on the card. The check script makes a throwaway key (built in as APP_KEY_H), an image and
   its signature by tools/signapp.sh; argv: the image, the signature, the name it was signed as. The good pair must pass. Every
   cut of the signature, fixed-seed mutations of it, lengths past APPSIG_MAX, one flipped image byte and another name must
   not. Seconds, no QEMU. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "appsig.h"

static long g_cases;
static unsigned char *slurp(const char *p, unsigned long *n) {
    FILE *f = fopen(p, "rb"); if (!f) { perror(p); exit(1); }
    fseek(f, 0, SEEK_END); *n = (unsigned long)ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char *b = malloc(*n ? *n : 1); if (fread(b, 1, *n, f) != *n) exit(1); fclose(f);
    return b;
}
static int one(const char *name, const unsigned char *img, unsigned long len, const unsigned char *sig, unsigned sl) {
    g_cases++;
    unsigned char *s = malloc(sl ? sl : 1); memcpy(s, sig, sl);   /* exactly its size, so a read past it is an ASan error */
    int r = appsig_ok(name, (unsigned)strlen(name), img, len, s, sl);
    free(s);
    return r;
}
static unsigned rnd_state = 777;
static unsigned rnd(void) { rnd_state = rnd_state * 1103515245u + 12345u; return rnd_state >> 8; }
int main(int argc, char **argv) {
    if (argc != 4) { puts("usage: fuzz_appsig IMAGE SIG NAME"); return 1; }
    unsigned long il, sl; unsigned char *img = slurp(argv[1], &il), *sig = slurp(argv[2], &sl);
    const char *name = argv[3];
    if (!one(name, img, il, sig, (unsigned)sl)) { puts("FAIL: the good signature did not verify"); return 1; }
    for (unsigned k = 0; k < sl; k++) if (one(name, img, il, sig, k)) { printf("FAIL: the signature cut to %u bytes verified\n", k); return 1; }
    unsigned char m[APPSIG_MAX + 16];
    for (int t = 0; t < 3000; t++) {
        unsigned len = (unsigned)sl; memcpy(m, sig, sl);
        int edits = 1 + (int)(rnd() % 3);
        for (int e = 0; e < edits; e++) {
            unsigned p = rnd() % len, op = rnd() % 4;
            if (op == 0) m[p] ^= (unsigned char)(1 + rnd() % 255);
            else if (op == 1) m[p] = (unsigned char)(rnd() % 2 ? 0x80 | (rnd() & 0x7f) : 0);
            else if (op == 2 && len + 1 < sizeof m) { memmove(m + p + 1, m + p, len - p); m[p] = (unsigned char)rnd(); len++; }
            else if (len > 2) { memmove(m + p, m + p + 1, len - p - 1); len--; }
        }
        if (len == sl && !memcmp(m, sig, sl)) continue;
        if (one(name, img, il, m, len)) { puts("FAIL: a changed signature verified"); return 1; }
    }
    unsigned char big[APPSIG_MAX + 9]; memset(big, 0x30, sizeof big); memcpy(big, sig, sl);
    if (one(name, img, il, big, sizeof big)) { puts("FAIL: a signature over APPSIG_MAX was taken"); return 1; }
    img[il / 2] ^= 1;
    if (one(name, img, il, sig, (unsigned)sl)) { puts("FAIL: one flipped image byte still verified"); return 1; }
    img[il / 2] ^= 1;
    if (one("other", img, il, sig, (unsigned)sl)) { puts("FAIL: the signature verified for another name"); return 1; }
    free(img); free(sig);   /* LeakSanitizer runs on Linux (CI), not on a Mac */
    fprintf(stderr, "appsig-fuzz: %ld cases\n", g_cases);
    puts("APPSIG FUZZ: PASS");
    return 0;
}
