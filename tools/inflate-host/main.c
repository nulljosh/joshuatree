/* Native host harness for the kernel's own inflate (drivers/inflate.c), run by
   tools/checks/user-compress-check.py. The check generates a main.c that
   includes every drivers/user_<app>.h and fills in APPS[]; this file is the
   rest, so the code under test is the very inflate_checked() the kernel runs
   in ring3app_unpack(). Usage: harness <dir-with-user-bins>
   Per app it prints "ok <name>" when the embedded bytes inflate to exactly
   user/<name>.bin with the matching Adler-32, and then proves the checker can
   say no: a single flipped byte (at the start, middle and end), a truncated
   stream and a wrong sum must each be REJECTED, printing "rejected <name> ...". */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "inflate.h"

struct app { const char *name; const unsigned char *packed; unsigned clen, len, sum; };
extern const struct app APPS[];
extern const unsigned NAPPS;

static unsigned char *slurp(const char *path, unsigned *n) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char *b = malloc(sz ? (size_t)sz : 1);
    if (fread(b, 1, (size_t)sz, f) != (size_t)sz) { fclose(f); free(b); return 0; }
    fclose(f); *n = (unsigned)sz; return b;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: harness <bin-dir>\n"); return 2; }
    int bad = 0;
    for (unsigned i = 0; i < NAPPS; i++) {
        const struct app *a = &APPS[i];
        char path[512]; snprintf(path, sizeof path, "%s/%s.bin", argv[1], a->name);
        unsigned want_n = 0; unsigned char *want = slurp(path, &want_n);
        if (!want || want_n != a->len) { printf("FAIL %s: header LEN %u but %s is %u\n", a->name, a->len, path, want ? want_n : 0); bad++; continue; }
        unsigned char *out = malloc(a->len + 1), *cp = malloc(a->clen + 1);
        int r = inflate_checked(a->packed, a->clen, out, a->len, a->sum);
        if (r != 0 || memcmp(out, want, a->len) != 0) { printf("FAIL %s: round trip r=%d\n", a->name, r); bad++; goto next; }
        printf("ok %s %u -> %u\n", a->name, a->clen, a->len);
        /* the discriminating half: every kind of damage must be refused */
        {
            unsigned pos[3] = { 0, a->clen / 2, a->clen - 1 };
            for (int k = 0; k < 3; k++) {
                memcpy(cp, a->packed, a->clen); cp[pos[k]] ^= 0x01;
                memset(out, 0, a->len);
                int rr = inflate_checked(cp, a->clen, out, a->len, a->sum);
                if (rr == 0) { printf("FAIL %s: flipped byte %u was ACCEPTED\n", a->name, pos[k]); bad++; }
                else printf("rejected %s flipped-byte@%u (%d)\n", a->name, pos[k], rr);
            }
            if (inflate_checked(a->packed, a->clen - 1, out, a->len, a->sum) == 0) { printf("FAIL %s: truncated stream ACCEPTED\n", a->name); bad++; }
            else printf("rejected %s truncated\n", a->name);
            if (inflate_checked(a->packed, a->clen, out, a->len, a->sum ^ 1u) == 0) { printf("FAIL %s: wrong sum ACCEPTED\n", a->name); bad++; }
            else printf("rejected %s wrong-sum\n", a->name);
            if (a->len > 1 && inflate_checked(a->packed, a->clen, out, a->len - 1, a->sum) == 0) { printf("FAIL %s: short buffer ACCEPTED\n", a->name); bad++; }
            else printf("rejected %s short-buffer\n", a->name);
        }
    next:
        free(want); free(out); free(cp);
    }
    printf("%u apps, %d failures\n", NAPPS, bad);
    return bad ? 1 : 0;
}
